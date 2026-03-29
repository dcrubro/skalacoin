#include <block/chain.h>
#include <errno.h>
#include <sys/stat.h>

static bool EnsureDirectoryExists(const char* dirpath) {
    if (!dirpath || dirpath[0] == '\0') {
        return false;
    }

    struct stat st;
    if (stat(dirpath, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    if (mkdir(dirpath, 0755) == 0) {
        return true;
    }

    if (errno == EEXIST && stat(dirpath, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    return false;
}

static bool BuildPath(char* out, size_t outSize, const char* dirpath, const char* filename) {
    if (!out || outSize == 0 || !dirpath || !filename) {
        return false;
    }

    const int written = snprintf(out, outSize, "%s/%s", dirpath, filename);
    return written > 0 && (size_t)written < outSize;
}

blockchain_t* Chain_Create() {
    blockchain_t* ptr = (blockchain_t*)malloc(sizeof(blockchain_t));
    if (!ptr) {
        return NULL;
    }

    ptr->blocks = DYNARR_CREATE(block_t, 1);
    ptr->size = 0;

    return ptr;
}

void Chain_Destroy(blockchain_t* chain) {
    if (chain) {
        if (chain->blocks) {
            DynArr_destroy(chain->blocks);
        }
        free(chain);
    }
}

bool Chain_AddBlock(blockchain_t* chain, block_t* block) {
    if (chain && block && chain->blocks) {
        DynArr_push_back(chain->blocks, block);
        chain->size++;
        return true;
    }

    return false;
}

block_t* Chain_GetBlock(blockchain_t* chain, size_t index) {
    if (chain) {
        return DynArr_at(chain->blocks, index);
    }
    return NULL;
}

size_t Chain_Size(blockchain_t* chain) {
    if (chain) {
        return DynArr_size(chain->blocks);
    }
    return 0;
}

bool Chain_IsValid(blockchain_t* chain) {
    if (!chain || !chain->blocks) {
        return false;
    }

    const size_t chainSize = DynArr_size(chain->blocks);
    if (chainSize == 0) {
        return true;
    }

    for (size_t i = 1; i < chainSize; i++) {
        block_t* blk = (block_t*)DynArr_at(chain->blocks, i);
        block_t* prevBlk = (block_t*)DynArr_at(chain->blocks, i - 1);
        if (!blk || !prevBlk || blk->header.blockNumber != i) { return false; } // NULL blocks or blockNumber != order in chain

        // Verify prevHash is valid
        uint8_t prevHash[32];
        Block_CalculateHash(prevBlk, prevHash);

        if (memcmp(blk->header.prevHash, prevHash, 32) != 0) {
            return false;
        }

        // A potential issue is verifying PoW, since the chain read might only have header data without transactions.
        // A potnetial fix is verifying PoW as we go, when getting new blocks from peers, and only accepting blocks
        //with valid PoW, so that we can assume all blocks in the chain are valid in that regard.
    }
    
    // Genesis needs special handling because the prevHash is always invalid (no previous block)
    block_t* genesis = (block_t*)DynArr_at(chain->blocks, 0);
    if (!genesis || genesis->header.blockNumber != 0) { return false; }

    return true;
}

void Chain_Wipe(blockchain_t* chain) {
    if (chain && chain->blocks) {
        DynArr_erase(chain->blocks);
        chain->size = 0;
    }
}

bool Chain_SaveToFile(blockchain_t* chain, const char* dirpath, uint256_t currentSupply) {
    // To avoid stalling the chain from peers, write after every block addition (THAT IS VERIFIED)

    if (!chain || !chain->blocks || !EnsureDirectoryExists(dirpath)) {
        return false;
    }

    char metaPath[512];
    if (!BuildPath(metaPath, sizeof(metaPath), dirpath, "chain.meta")) {
        return false;
    }

    // Find metadata file (create if not exists) to get the saved chain size (+ other things)
    FILE* metaFile = fopen(metaPath, "rb+");
    if (!metaFile) {
        metaFile = fopen(metaPath, "wb+");
        if (!metaFile) {
            return false;
        }

        // Initialize metadata with size 0
        size_t initialSize = 0;
        fwrite(&initialSize, sizeof(size_t), 1, metaFile);
        // Write last block hash (32 bytes of zeros for now)
        uint8_t zeroHash[32] = {0};
        fwrite(zeroHash, sizeof(uint8_t), 32, metaFile);
        uint256_t zeroSupply = {0};
        fwrite(&zeroSupply, sizeof(uint256_t), 1, metaFile);

        // TODO: Potentially some other things here, we'll see
    }

    // Read
    size_t savedSize = 0;
    fread(&savedSize, sizeof(size_t), 1, metaFile);
    uint8_t lastSavedHash[32];
    fread(lastSavedHash, sizeof(uint8_t), 32, metaFile);

    // Assume chain saved is valid, and that the chain in memory is valid (as LoadFromFile will verify the saved one)
    if (savedSize > DynArr_size(chain->blocks)) {
        // Saved chain is longer than current chain, this should not happen if we are always saving the current chain, but just in case, fail to save to avoid overwriting a potentially valid longer chain with a shorter one.
        fclose(metaFile);
        return false;
    }

    // Filename formart: dirpath/block_{index}.dat
    // File format: [block_header][num_transactions][transactions...] - since block_header is fixed size, LoadFromFile will only read those by default

    // Save blocks that are not yet saved
    for (size_t i = savedSize; i < DynArr_size(chain->blocks); i++) {
        block_t* blk = (block_t*)DynArr_at(chain->blocks, i);
        if (!blk) {
            fclose(metaFile);
            return false;
        }

        // Construct file path
        char filePath[256];
        snprintf(filePath, sizeof(filePath), "%s/block_%zu.dat", dirpath, i);

        FILE* blockFile = fopen(filePath, "wb");
        if (!blockFile) {
            fclose(metaFile);
            return false;
        }

        // Write block header
        fwrite(&blk->header, sizeof(block_header_t), 1, blockFile);
        size_t txSize = DynArr_size(blk->transactions);
        fwrite(&txSize, sizeof(size_t), 1, blockFile); // Write number of transactions
        // Write transactions
        for (size_t j = 0; j < txSize; j++) {
            signed_transaction_t* tx = (signed_transaction_t*)DynArr_at(blk->transactions, j);
            if (fwrite(tx, sizeof(signed_transaction_t), 1, blockFile) != 1) {
                fclose(blockFile);
                fclose(metaFile);
                return false;
            }
        }

        fclose(blockFile);
    }

    // Update metadata with new size and last block hash
    size_t newSize = DynArr_size(chain->blocks);
    fseek(metaFile, 0, SEEK_SET);
    fwrite(&newSize, sizeof(size_t), 1, metaFile);
    if (newSize > 0) {
        block_t* lastBlock = (block_t*)DynArr_at(chain->blocks, newSize - 1);
        uint8_t lastHash[32];
        Block_CalculateHash(lastBlock, lastHash);
        fwrite(lastHash, sizeof(uint8_t), 32, metaFile);
    }
    fwrite(&currentSupply, sizeof(uint256_t), 1, metaFile);
    fclose(metaFile);

    return true;
}

bool Chain_LoadFromFile(blockchain_t* chain, const char* dirpath, uint256_t* outCurrentSupply) {
    if (!chain || !chain->blocks || !dirpath || !outCurrentSupply) {
        return false;
    }

    struct stat st;
    if (stat(dirpath, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return false;
    }

    char metaPath[512];
    if (!BuildPath(metaPath, sizeof(metaPath), dirpath, "chain.meta")) {
        return false;
    }

    // Read metadata file to get saved chain size (+ other things)
    FILE* metaFile = fopen(metaPath, "rb");
    if (!metaFile) {
        return false;
    }

    size_t savedSize = 0;
    fread(&savedSize, sizeof(size_t), 1, metaFile);
    uint8_t lastSavedHash[32];
    fread(lastSavedHash, sizeof(uint8_t), 32, metaFile);
    fread(outCurrentSupply, sizeof(uint256_t), 1, metaFile);
    fclose(metaFile);

    // TODO: Might add a flag to allow reading from a point onward, but just rewrite for now
    DynArr_erase(chain->blocks); // Clear current chain blocks, but keep allocated memory for efficiency, since we will likely be loading a similar number of blocks as currently in memory.

    // Load blocks
    for (size_t i = 0; i < savedSize; i++) {
        // Construct file path
        char filePath[256];
        snprintf(filePath, sizeof(filePath), "%s/block_%zu.dat", dirpath, i);

        block_t* blk = Block_Create();
        if (!blk) {
            return false;
        }

        FILE* blockFile = fopen(filePath, "rb");
        if (!blockFile) {
            Block_Destroy(blk);
            return false;
        }

        // Read block header and transactions
        if (fread(&blk->header, sizeof(block_header_t), 1, blockFile) != 1) {
            fclose(blockFile);
            Block_Destroy(blk);
            return false;
        }

        size_t txSize = 0;
        if (fread(&txSize, sizeof(size_t), 1, blockFile) != 1) {
            fclose(blockFile);
            Block_Destroy(blk);
            return false;
        }

        for (size_t j = 0; j < txSize; j++) {
            signed_transaction_t tx;
            if (fread(&tx, sizeof(signed_transaction_t), 1, blockFile) != 1) {
                fclose(blockFile);
                Block_Destroy(blk);
                return false;
            }
            Block_AddTransaction(blk, &tx);
        }
        fclose(blockFile);

        Chain_AddBlock(chain, blk);
    }

    chain->size = savedSize;

    // After read, you SHOULD verify chain validity. We're not doing it here since returning false is a bit unclear if the read failed or if the chain is invalid.
    return true;
}
