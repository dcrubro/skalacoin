#include <block/chain.h>
#include <constants.h>
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

static void Chain_ClearBlocks(blockchain_t* chain) {
    if (!chain || !chain->blocks) {
        return;
    }

    for (size_t i = 0; i < DynArr_size(chain->blocks); i++) {
        block_t* blk = (block_t*)DynArr_at(chain->blocks, i);
        if (blk && blk->transactions) {
            DynArr_destroy(blk->transactions);
            blk->transactions = NULL;
        }
    }

    DynArr_erase(chain->blocks);
    chain->size = 0;
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
            Chain_ClearBlocks(chain);
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
    Chain_ClearBlocks(chain);
}

bool Chain_SaveToFile(blockchain_t* chain, const char* dirpath, uint256_t currentSupply, uint64_t currentReward) {
    // To avoid stalling the chain from peers, write after every block addition (THAT IS VERIFIED)
    // TODO: Write to one "db" file instead of one file per block - filesystems (and rm *) don't like millions of files :(

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
        uint32_t initialTarget = INITIAL_DIFFICULTY;
        fwrite(&initialTarget, sizeof(uint32_t), 1, metaFile);
        uint64_t initialReward = 0;
        fwrite(&initialReward, sizeof(uint64_t), 1, metaFile);

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

        DynArr_destroy(blk->transactions);
        blk->transactions = NULL; // Clear transactions to save memory since they're now saved on disk
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
    uint32_t difficultyTarget = ((block_t*)DynArr_at(chain->blocks, newSize - 1))->header.difficultyTarget;
    fwrite(&difficultyTarget, sizeof(uint32_t), 1, metaFile);
    fwrite(&currentReward, sizeof(uint64_t), 1, metaFile);
    fclose(metaFile);

    return true;
}

bool Chain_LoadFromFile(blockchain_t* chain, const char* dirpath, uint256_t* outCurrentSupply, uint32_t* outDifficultyTarget, uint64_t* outCurrentReward) {
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
    fread(outDifficultyTarget, sizeof(uint32_t), 1, metaFile);
    fread(outCurrentReward, sizeof(uint64_t), 1, metaFile);
    fclose(metaFile);

    // TODO: Might add a flag to allow reading from a point onward, but just rewrite for now
    Chain_ClearBlocks(chain); // Clear current chain blocks and free owned transaction buffers before reload.

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

        /*for (size_t j = 0; j < txSize; j++) {
            signed_transaction_t tx;
            if (fread(&tx, sizeof(signed_transaction_t), 1, blockFile) != 1) {
                fclose(blockFile);
                Block_Destroy(blk);
                return false;
            }
            Block_AddTransaction(blk, &tx);
        }*/ // Transactions are not read, we use the merkle root for validity

        fclose(blockFile);
        Chain_AddBlock(chain, blk);

        // Chain_AddBlock stores blocks by value, so the copied block now owns
        // blk->transactions. Only free the temporary wrapper struct here.
        free(blk);
    }

    chain->size = savedSize;

    // After read, you SHOULD verify chain validity. We're not doing it here since returning false is a bit unclear if the read failed or if the chain is invalid.
    return true;
}

uint32_t Chain_ComputeNextTarget(blockchain_t* chain, uint32_t currentTarget) {
    if (!chain || !chain->blocks) {
        return 0x00; // Impossible difficulty, only valid hash is all zeros (practically impossible)
    }

    size_t chainSize = DynArr_size(chain->blocks);
    if (chainSize < DIFFICULTY_ADJUSTMENT_INTERVAL) {
        // Baby-chain, return initial difficulty
        return INITIAL_DIFFICULTY;
    }

    // Assuming block validation validates timestamps, we can assume they're valid and can just read them
    block_t* lastBlock = (block_t*)DynArr_at(chain->blocks, chainSize - 1);
    block_t* adjustmentBlock = (block_t*)DynArr_at(chain->blocks, chainSize - DIFFICULTY_ADJUSTMENT_INTERVAL);
    if (!lastBlock || !adjustmentBlock) {
        return 0x00; // Impossible difficulty, only valid hash is all zeros (practically impossible)
    }

    // Retarget uses whole-window span. Per-block average is implicit:
    // (actualTime / interval) / targetBlockTime == actualTime / targetTime.
    uint64_t actualTime = 0;
    if (lastBlock->header.timestamp > adjustmentBlock->header.timestamp) {
        actualTime = lastBlock->header.timestamp - adjustmentBlock->header.timestamp;
    }
    if (actualTime == 0) {
        return currentTarget; // Invalid/non-increasing time window; keep current target
    }

    const uint64_t targetTime = (uint64_t)TARGET_BLOCK_TIME * (uint64_t)DIFFICULTY_ADJUSTMENT_INTERVAL;
    double timeRatio = (double)actualTime / (double)targetTime;

    // Clamp per-epoch target movement: at most x2 easier or x2 harder. TODO: Check if the clamp should be more aggressive or looser
    if (timeRatio > 2.0) {
        timeRatio = 2.0;
    } else if (timeRatio < 0.5) {
        timeRatio = 0.5;
    }

    uint32_t exponent = currentTarget >> 24;
    uint32_t mantissa = currentTarget & 0x007fffff;
    if (mantissa == 0 || exponent == 0) {
        return INITIAL_DIFFICULTY;
    }

    double newMantissa = (double)mantissa * timeRatio;

    // Normalize to compact format range.
    while (newMantissa > 8388607.0) { // 0x007fffff
        newMantissa /= 256.0;
        exponent++;
    }
    while (newMantissa > 0.0 && newMantissa < 32768.0 && exponent > 3) { // Keep coefficient in normal range
        newMantissa *= 256.0;
        exponent--;
    }

    if (exponent > 32) {
        // Easiest representable target in our decoder range.
        return (32u << 24) | 0x007fffff;
    }
    if (exponent < 1) {
        exponent = 1;
    }

    uint32_t newCoeff = (uint32_t)newMantissa;
    if (newCoeff == 0) {
        newCoeff = 1;
    }
    if (newCoeff > 0x007fffff) {
        newCoeff = 0x007fffff;
    }

    return (exponent << 24) | (newCoeff & 0x007fffff);
}
