#include <block/chain.h>
#include <constants.h>
#include <runtime_state.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <pthread.h>

uint64_t currentBlockHeight = 0;

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

static bool BuildSpendAmount(const signed_transaction_t* tx, uint256_t* outSpend) {
    if (!tx || !outSpend) {
        return false;
    }

    *outSpend = uint256_from_u64(0);
    if (uint256_add_u64(outSpend, tx->transaction.amount1)) {
        return false;
    }
    if (uint256_add_u64(outSpend, tx->transaction.amount2)) {
        return false;
    }
    if (uint256_add_u64(outSpend, tx->transaction.fee)) {
        return false;
    }

    return true;
}

static bool CreditAddress(const uint8_t address[32], uint64_t amount) {
    if (!address || amount == 0) {
        return true;
    }

    balance_sheet_entry_t entry;
    if (BalanceSheet_Lookup((uint8_t*)address, &entry)) {
        if (uint256_add_u64(&entry.balance, amount)) {
            return false;
        }
    } else {
        memset(&entry, 0, sizeof(entry));
        memcpy(entry.address, address, 32);
        entry.balance = uint256_from_u64(amount);
    }

    return BalanceSheet_Insert(entry) >= 0;
}

static bool DebitAddress(const uint8_t address[32], const uint256_t* amount) {
    if (!address || !amount) {
        return false;
    }

    balance_sheet_entry_t entry;
    if (!BalanceSheet_Lookup((uint8_t*)address, &entry)) {
        return false;
    }

    if (uint256_cmp(&entry.balance, amount) < 0) {
        return false;
    }

    if (!uint256_subtract(&entry.balance, amount)) {
        return false;
    }

    return BalanceSheet_Insert(entry) >= 0;
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
    bool ok = true;

    if (!chain || !block || !chain->blocks) {
        return false;
    }

    if (!block->transactions) {
        return false;
    }

    // Acquire global write locks to protect chain and balance sheet mutations.
    pthread_rwlock_wrlock(&chainLock);
    pthread_mutex_lock(&balanceSheetLock);

    do {
        // First pass: ensure all non-coinbase senders can cover the full spend
        // (amount1 + amount2 + fee) before mutating the chain or balance sheet.
        size_t txCount = DynArr_size(block->transactions);
        for (size_t i = 0; i < txCount; ++i) {
            signed_transaction_t* tx = (signed_transaction_t*)DynArr_at(block->transactions, i);
            if (!tx) {
                ok = false; break;
            }

            if (Address_IsCoinbase(tx->transaction.senderAddress)) {
                continue;
            }

            uint256_t spend;
            if (!BuildSpendAmount(tx, &spend)) { ok = false; break; }

            balance_sheet_entry_t senderEntry;
            if (!BalanceSheet_Lookup(tx->transaction.senderAddress, &senderEntry)) {
                fprintf(stderr, "Error: Sender address not found in balance sheet during block addition. Bailing!\n");
                ok = false; break;
            }

            if (uint256_cmp(&senderEntry.balance, &spend) < 0) {
                fprintf(stderr, "Error: Sender balance insufficient for block transaction. Bailing!\n");
                ok = false; break;
            }
        }
        if (!ok) break;

        // Push the block only after validation succeeds.
        block_t* blk = (block_t*)DynArr_push_back(chain->blocks, block);
        if (!blk) { ok = false; break; }
        chain->size++;
        currentBlockHeight = (uint64_t)(chain->size - 1);

        // Second pass: apply the ledger changes.
        if (blk->transactions) {
            txCount = DynArr_size(blk->transactions);
            for (size_t i = 0; i < txCount; ++i) {
                signed_transaction_t* tx = (signed_transaction_t*)DynArr_at(blk->transactions, i);
                if (!tx) {
                    continue;
                }

                if (!Address_IsCoinbase(tx->transaction.senderAddress)) {
                    uint256_t spend;
                    if (!BuildSpendAmount(tx, &spend) || !DebitAddress(tx->transaction.senderAddress, &spend)) {
                        fprintf(stderr, "Error: Failed to debit sender balance during block addition. Bailing!\n");
                        ok = false; break;
                    }
                }

                    if (!CreditAddress(tx->transaction.recipientAddress1, tx->transaction.amount1)) {
                    fprintf(stderr, "Error: Failed to credit recipient1 balance during block addition. Bailing!\n");
                    ok = false; break;
                }

                if (tx->transaction.amount2 > 0) {
                    uint8_t zeroAddress[32] = {0};
                    if (memcmp(tx->transaction.recipientAddress2, zeroAddress, 32) == 0) {
                        fprintf(stderr, "Error: amount2 is non-zero but recipient2 is empty during block addition. Bailing!\n");
                        ok = false; break;
                    }

                    if (!CreditAddress(tx->transaction.recipientAddress2, tx->transaction.amount2)) {
                        fprintf(stderr, "Error: Failed to credit recipient2 balance during block addition. Bailing!\n");
                        ok = false; break;
                    }
                }
            }
        }
        // ok remains true if no failures
    } while (0);

    // Release locks
    pthread_mutex_unlock(&balanceSheetLock);
    pthread_rwlock_unlock(&chainLock);

    return ok;
}

block_t* Chain_GetBlock(blockchain_t* chain, size_t index) {
    if (!chain) return NULL;
    block_t* blk = NULL;
    pthread_rwlock_rdlock(&chainLock);
    blk = (block_t*)DynArr_at(chain->blocks, index);
    pthread_rwlock_unlock(&chainLock);
    return blk;
}

bool Chain_GetBlockCopy(blockchain_t* chain, size_t index, block_t** outCopy) {
    if (!chain || !outCopy) return false;
    *outCopy = NULL;
    pthread_rwlock_rdlock(&chainLock);
    block_t* src = (block_t*)DynArr_at(chain->blocks, index);
    if (!src) {
        pthread_rwlock_unlock(&chainLock);
        return false;
    }
    block_t* copy = Block_Copy(src);
    pthread_rwlock_unlock(&chainLock);
    if (!copy) return false;
    *outCopy = copy;
    return true;
}

size_t Chain_Size(blockchain_t* chain) {
    if (!chain) return 0;
    size_t sz = 0;
    pthread_rwlock_rdlock(&chainLock);
    sz = DynArr_size(chain->blocks);
    pthread_rwlock_unlock(&chainLock);
    return sz;
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
        // with valid PoW, so that we can assume all blocks in the chain are valid in that regard.
        // During the initial sync, we can verify the PoW, the validity of each transaction + coinbase, etc.
    }
    
    // Genesis needs special handling because the prevHash is always invalid (no previous block)
    block_t* genesis = (block_t*)DynArr_at(chain->blocks, 0);
    if (!genesis || genesis->header.blockNumber != 0) { return false; }

    return true;
}

bool Chain_RollbackToHeight(blockchain_t* chain, size_t height) {
    if (!chain || !chain->blocks) return false;

    pthread_rwlock_wrlock(&chainLock);
    pthread_mutex_lock(&balanceSheetLock);

    size_t cur = DynArr_size(chain->blocks);
    if (height >= cur) {
        pthread_mutex_unlock(&balanceSheetLock);
        pthread_rwlock_unlock(&chainLock);
        return true; // nothing to do
    }

    // Remove blocks above height
    for (size_t i = cur; i > height; --i) {
        size_t idx = i - 1;
        block_t* blk = (block_t*)DynArr_at(chain->blocks, idx);
        if (blk && blk->transactions) {
            DynArr_destroy(blk->transactions);
            blk->transactions = NULL;
        }
        DynArr_remove(chain->blocks, idx);
    }

    chain->size = DynArr_size(chain->blocks);
    currentBlockHeight = chain->size ? (uint64_t)(chain->size - 1) : 0ULL;

    // Rebuild balance sheet from scratch up to current chain size
    BalanceSheet_Destroy();
    BalanceSheet_Init();

    for (size_t i = 0; i < chain->size; ++i) {
        block_t* blk = (block_t*)DynArr_at(chain->blocks, i);
        block_t* toProcess = blk;
        bool loaded = false;

        if (!blk || !blk->transactions) {
            // Try to load from disk
            block_t* loadedBlk = NULL;
            size_t txCount = 0;
            if (!Chain_LoadBlockFromFile(chainDataDir, (uint64_t)i, true, &loadedBlk, &txCount)) {
                // Can't rebuild without transactions
                pthread_mutex_unlock(&balanceSheetLock);
                pthread_rwlock_unlock(&chainLock);
                return false;
            }
            toProcess = loadedBlk;
            loaded = true;
        }

        // Apply transactions
        if (toProcess && toProcess->transactions) {
            size_t txCount = DynArr_size(toProcess->transactions);
            for (size_t ti = 0; ti < txCount; ++ti) {
                signed_transaction_t* tx = (signed_transaction_t*)DynArr_at(toProcess->transactions, ti);
                if (!tx) continue;

                // Coinbase credit
                if (Address_IsCoinbase(tx->transaction.senderAddress)) {
                    balance_sheet_entry_t entry;
                    if (!BalanceSheet_Lookup(tx->transaction.recipientAddress1, &entry)) {
                        memset(&entry, 0, sizeof(entry));
                        memcpy(entry.address, tx->transaction.recipientAddress1, 32);
                        entry.balance = uint256_from_u64(tx->transaction.amount1);
                    } else {
                        (void)uint256_add_u64(&entry.balance, tx->transaction.amount1);
                    }
                    (void)BalanceSheet_Insert(entry);
                    continue;
                }

                // Non-coinbase: debit sender
                uint256_t spend = uint256_from_u64(0);
                (void)uint256_add_u64(&spend, tx->transaction.amount1);
                if (tx->transaction.amount2 > 0) (void)uint256_add_u64(&spend, tx->transaction.amount2);
                if (tx->transaction.fee > 0) (void)uint256_add_u64(&spend, tx->transaction.fee);

                balance_sheet_entry_t senderEntry;
                if (!BalanceSheet_Lookup(tx->transaction.senderAddress, &senderEntry)) {
                    // Missing sender; create zero and then subtract (will underflow if invalid)
                    memset(&senderEntry, 0, sizeof(senderEntry));
                    memcpy(senderEntry.address, tx->transaction.senderAddress, 32);
                    senderEntry.balance = uint256_from_u64(0);
                }
                (void)uint256_subtract(&senderEntry.balance, &spend);
                (void)BalanceSheet_Insert(senderEntry);

                // Credit recipient1
                balance_sheet_entry_t rec1;
                if (!BalanceSheet_Lookup(tx->transaction.recipientAddress1, &rec1)) {
                    memset(&rec1, 0, sizeof(rec1));
                    memcpy(rec1.address, tx->transaction.recipientAddress1, 32);
                    rec1.balance = uint256_from_u64(tx->transaction.amount1);
                } else {
                    (void)uint256_add_u64(&rec1.balance, tx->transaction.amount1);
                }
                (void)BalanceSheet_Insert(rec1);

                // Credit recipient2 if any
                if (tx->transaction.amount2 > 0) {
                    balance_sheet_entry_t rec2;
                    if (!BalanceSheet_Lookup(tx->transaction.recipientAddress2, &rec2)) {
                        memset(&rec2, 0, sizeof(rec2));
                        memcpy(rec2.address, tx->transaction.recipientAddress2, 32);
                        rec2.balance = uint256_from_u64(tx->transaction.amount2);
                    } else {
                        (void)uint256_add_u64(&rec2.balance, tx->transaction.amount2);
                    }
                    (void)BalanceSheet_Insert(rec2);
                }
            }
        }

        if (loaded && toProcess) {
            if (toProcess->transactions) DynArr_destroy(toProcess->transactions);
            free(toProcess);
        }
    }

    pthread_mutex_unlock(&balanceSheetLock);
    pthread_rwlock_unlock(&chainLock);

    return true;
}

void Chain_Wipe(blockchain_t* chain) {
    Chain_ClearBlocks(chain);
    currentBlockHeight = 0;
}

bool Chain_SaveToFile(blockchain_t* chain, const char* dirpath, uint256_t currentSupply, uint64_t currentReward) {
    // To avoid stalling the chain from peers, write after every block addition (THAT IS VERIFIED)
    // TODO: Check fwrite() and fread() calls if they actually didn't error

    if (!chain || !chain->blocks || !EnsureDirectoryExists(dirpath)) {
        return false;
    }

    char metaPath[512];
    if (!BuildPath(metaPath, sizeof(metaPath), dirpath, "chain.meta")) {
        return false;
    }

    char chainPath[512];
    if (!BuildPath(chainPath, sizeof(chainPath), dirpath, "chain.data")) {
        return false;
    }
    
    char tablePath[512];
    if (!BuildPath(tablePath, sizeof(tablePath), dirpath, "chain.table")) {
        return false;
    }
    
    // Find metadata file (create if not exists) to get the saved chain size (+ other things)
    FILE* metaFile = fopen(metaPath, "rb+");
    FILE* chainFile = fopen(chainPath, "rb+");
    FILE* tableFile = fopen(tablePath, "rb+");
    if (!metaFile || !chainFile || !tableFile) {
        // Just overwrite everything
        metaFile = fopen(metaPath, "wb+");
        if (!metaFile) { return false; }

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

        chainFile = fopen(chainPath, "wb+");
        if (!chainFile) { return false; }

        tableFile = fopen(tablePath, "wb+");
        if (!tableFile) { return false; }

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
        fclose(chainFile);
        fclose(tableFile);
        return false;
    }

    // Filename format: dirpath/chain.data
    // File format: ([block_header][num_transactions][transactions...])[*length] - since block_header is fixed size, LoadFromFile will only read those by default

    fseek(chainFile, 0, SEEK_END); // Seek to the end of those files
    fseek(tableFile, 0, SEEK_END);
    long pos = ftell(chainFile);
    if (pos < 0) {
        fclose(metaFile);
        fclose(chainFile);
        fclose(tableFile);
        return false;
    }

    uint64_t byteCount = (uint64_t)pos; // Get the size

    // Save blocks that are not yet saved
    for (size_t i = savedSize; i < DynArr_size(chain->blocks); i++) {
        block_t* blk = (block_t*)DynArr_at(chain->blocks, i);
        if (!blk) {
            fclose(metaFile);
            fclose(chainFile);
            fclose(tableFile);
            return false;
        }

        uint64_t preIncrementByteSize = byteCount;

        // Construct file path
        // Write block header
        fwrite(&blk->header, sizeof(block_header_t), 1, chainFile);
        size_t txSize = DynArr_size(blk->transactions);
        fwrite(&txSize, sizeof(size_t), 1, chainFile); // Write number of transactions
        byteCount += sizeof(block_header_t) + sizeof(size_t);
        // Write transactions
        for (size_t j = 0; j < txSize; j++) {
            signed_transaction_t* tx = (signed_transaction_t*)DynArr_at(blk->transactions, j);
            if (fwrite(tx, sizeof(signed_transaction_t), 1, chainFile) != 1) {
                fclose(chainFile);
                fclose(metaFile);
                fclose(tableFile);
                return false;
            }

            byteCount += sizeof(signed_transaction_t);
        }

        // Create an entry in the block table
        block_table_entry_t entry;
        entry.blockNumber = i;
        entry.byteNumber = preIncrementByteSize;
        entry.blockSize = byteCount - preIncrementByteSize;
        fwrite(&entry, sizeof(block_table_entry_t), 1, tableFile);

        DynArr_destroy(blk->transactions);
        blk->transactions = NULL; // Clear transactions to save memory since they're now saved on disk
    }

    // Update metadata with new size and last block hash
    size_t newSize = DynArr_size(chain->blocks);
    fseek(metaFile, 0, SEEK_SET);
    fwrite(&newSize, sizeof(size_t), 1, metaFile);
    uint32_t difficultyTarget = INITIAL_DIFFICULTY;
    if (newSize > 0) {
        block_t* lastBlock = (block_t*)DynArr_at(chain->blocks, newSize - 1);
        uint8_t lastHash[32];
        Block_CalculateHash(lastBlock, lastHash);
        fwrite(lastHash, sizeof(uint8_t), 32, metaFile);
        difficultyTarget = lastBlock->header.difficultyTarget;
    } else {
        uint8_t zeroHash[32] = {0};
        fwrite(zeroHash, sizeof(uint8_t), 32, metaFile);
    }
    fwrite(&currentSupply, sizeof(uint256_t), 1, metaFile);
    fwrite(&difficultyTarget, sizeof(uint32_t), 1, metaFile);
    fwrite(&currentReward, sizeof(uint64_t), 1, metaFile);

    // Safety
    fflush(metaFile);
    fflush(chainFile);
    fflush(tableFile);

    // Close all pointers
    fclose(metaFile);
    fclose(chainFile);
    fclose(tableFile);

    return true;
}

bool Chain_LoadFromFile(blockchain_t* chain, const char* dirpath, uint256_t* outCurrentSupply, uint32_t* outDifficultyTarget, uint64_t* outCurrentReward, uint8_t* outLastSavedHash, bool loadTransactions) {
    if (!chain || !chain->blocks || !dirpath || !outCurrentSupply || !outLastSavedHash) {
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

    char chainPath[512];
    if (!BuildPath(chainPath, sizeof(chainPath), dirpath, "chain.data")) {
        return false;
    }
    
    char tablePath[512];
    if (!BuildPath(tablePath, sizeof(tablePath), dirpath, "chain.table")) {
        return false;
    }

    // Read metadata file to get saved chain size (+ other things)
    FILE* metaFile = fopen(metaPath, "rb+");
    FILE* chainFile = fopen(chainPath, "rb+");
    FILE* tableFile = fopen(tablePath, "rb+");
    if (!metaFile || !chainFile || !tableFile) {
        if (metaFile) fclose(metaFile);
        if (chainFile) fclose(chainFile);
        if (tableFile) fclose(tableFile);
        return false;
    }

    size_t savedSize = 0;
    if (fread(&savedSize, sizeof(size_t), 1, metaFile) != 1) {
        fclose(metaFile);
        fclose(chainFile);
        fclose(tableFile);
        return false;
    }
    if (fread(outLastSavedHash, sizeof(uint8_t), 32, metaFile) != 32) {
        fclose(metaFile);
        fclose(chainFile);
        fclose(tableFile);
        return false;
    }
    if (fread(outCurrentSupply, sizeof(uint256_t), 1, metaFile) != 1) {
        fclose(metaFile);
        fclose(chainFile);
        fclose(tableFile);
        return false;
    }
    if (fread(outDifficultyTarget, sizeof(uint32_t), 1, metaFile) != 1) {
        fclose(metaFile);
        fclose(chainFile);
        fclose(tableFile);
        return false;
    }
    if (fread(outCurrentReward, sizeof(uint64_t), 1, metaFile) != 1) {
        fclose(metaFile);
        fclose(chainFile);
        fclose(tableFile);
        return false;
    }
    fclose(metaFile);

    // TODO: Might add a flag to allow reading from a point onward, but just rewrite for now
    Chain_ClearBlocks(chain); // Clear current chain blocks and free owned transaction buffers before reload.

    // Load blocks
    for (size_t i = 0; i < savedSize; i++) {
        // Get the table entry
        //fseek(tableFile, sizeof(block_table_entry_t) * i, SEEK_SET); // I think that fread() should take care of this for us
        block_table_entry_t loc;
        if (fread(&loc, sizeof(block_table_entry_t), 1, tableFile) != 1) {
            fclose(chainFile);
            fclose(tableFile);
            return false;
        }

        if (loc.blockNumber != i) {
            fclose(chainFile);
            fclose(tableFile);
            return false; // Mismatch
        }

        // Seek to that position
        if (fseek(chainFile, loc.byteNumber, SEEK_SET) != 0) {
            fclose(chainFile);
            fclose(tableFile);
            return false;
        }

        block_t* blk = (block_t*)calloc(1, sizeof(block_t));
        if (!blk) {
            fclose(chainFile);
            fclose(tableFile);
            return false;
        }

        // Read block header and transactions
        if (fread(&blk->header, sizeof(block_header_t), 1, chainFile) != 1) {
            fclose(chainFile);
            fclose(tableFile);
            free(blk);
            return false;
        }

        size_t txSize = 0;
        if (fread(&txSize, sizeof(size_t), 1, chainFile) != 1) {
            fclose(chainFile);
            fclose(tableFile);
            free(blk);
            return false;
        }
        if (loadTransactions) {
            blk->transactions = DYNARR_CREATE(signed_transaction_t, txSize == 0 ? 1 : txSize);
            if (!blk->transactions) {
                fclose(chainFile);
                fclose(tableFile);
                free(blk);
                return false;
            }

            for (size_t j = 0; j < txSize; j++) {
                signed_transaction_t tx;
                if (fread(&tx, sizeof(signed_transaction_t), 1, chainFile) != 1) {
                    fclose(chainFile);
                    fclose(tableFile);
                    DynArr_destroy(blk->transactions);
                    free(blk);
                    return false;
                }

                if (!DynArr_push_back(blk->transactions, &tx)) {
                    fclose(chainFile);
                    fclose(tableFile);
                    DynArr_destroy(blk->transactions);
                    free(blk);
                    return false;
                }
            }
        } else {
            if (txSize > 0 && fseek(chainFile, (long)(txSize * sizeof(signed_transaction_t)), SEEK_CUR) != 0) {
                fclose(chainFile);
                fclose(tableFile);
                free(blk);
                return false;
            }
            blk->transactions = NULL;
        }

        // Loading from disk currently restores headers only. Do not run Chain_AddBlock,
        // because it enforces transaction presence and mutates balances.
        if (!DynArr_push_back(chain->blocks, blk)) {
            fclose(chainFile);
            fclose(tableFile);
            free(blk);
            return false;
        }
        chain->size++;

        // DynArr_push_back stores blocks by value, so the copied block now owns
        // blk->transactions. Free wrapper only.
        free(blk);
    }

    chain->size = savedSize;
    fclose(chainFile);
    fclose(tableFile);

    // After read, you SHOULD verify chain validity. We're not doing it here since returning false is a bit unclear if the read failed or if the chain is invalid.
    return true;
}

bool Chain_LoadBlockFromFile(const char* dirpath, uint64_t blockNumber, bool loadTransactions, block_t** outBlock, size_t* outTxCount) {
    if (!dirpath || !outBlock) {
        return false;
    }

    *outBlock = NULL;
    if (outTxCount) {
        *outTxCount = 0;
    }

    struct stat st;
    if (stat(dirpath, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return false;
    }

    char metaPath[512];
    if (!BuildPath(metaPath, sizeof(metaPath), dirpath, "chain.meta")) {
        return false;
    }

    char chainPath[512];
    if (!BuildPath(chainPath, sizeof(chainPath), dirpath, "chain.data")) {
        return false;
    }

    char tablePath[512];
    if (!BuildPath(tablePath, sizeof(tablePath), dirpath, "chain.table")) {
        return false;
    }

    FILE* metaFile = fopen(metaPath, "rb");
    FILE* chainFile = fopen(chainPath, "rb");
    FILE* tableFile = fopen(tablePath, "rb");
    if (!metaFile || !chainFile || !tableFile) {
        if (metaFile) fclose(metaFile);
        if (chainFile) fclose(chainFile);
        if (tableFile) fclose(tableFile);
        return false;
    }

    size_t savedSize = 0;
    if (fread(&savedSize, sizeof(size_t), 1, metaFile) != 1) {
        fclose(metaFile);
        fclose(chainFile);
        fclose(tableFile);
        return false;
    }

    fclose(metaFile);

    if (blockNumber >= (uint64_t)savedSize) {
        fclose(chainFile);
        fclose(tableFile);
        return false;
    }

    uint64_t tableOffset = blockNumber * (uint64_t)sizeof(block_table_entry_t);
    if (blockNumber != 0 && tableOffset / blockNumber != (uint64_t)sizeof(block_table_entry_t)) {
        fclose(chainFile);
        fclose(tableFile);
        return false;
    }
    if (tableOffset > (uint64_t)LONG_MAX || fseek(tableFile, (long)tableOffset, SEEK_SET) != 0) {
        fclose(chainFile);
        fclose(tableFile);
        return false;
    }

    block_table_entry_t loc;
    if (fread(&loc, sizeof(block_table_entry_t), 1, tableFile) != 1) {
        fclose(chainFile);
        fclose(tableFile);
        return false;
    }

    if (loc.blockNumber != blockNumber) {
        fclose(chainFile);
        fclose(tableFile);
        return false;
    }

    if (loc.byteNumber > (uint64_t)LONG_MAX || fseek(chainFile, (long)loc.byteNumber, SEEK_SET) != 0) {
        fclose(chainFile);
        fclose(tableFile);
        return false;
    }

    block_t* blk = (block_t*)calloc(1, sizeof(block_t));
    if (!blk) {
        fclose(chainFile);
        fclose(tableFile);
        return false;
    }

    if (fread(&blk->header, sizeof(block_header_t), 1, chainFile) != 1) {
        free(blk);
        fclose(chainFile);
        fclose(tableFile);
        return false;
    }

    size_t txSize = 0;
    if (fread(&txSize, sizeof(size_t), 1, chainFile) != 1) {
        free(blk);
        fclose(chainFile);
        fclose(tableFile);
        return false;
    }

    if (outTxCount) {
        *outTxCount = txSize;
    }

    if (loadTransactions) {
        blk->transactions = DYNARR_CREATE(signed_transaction_t, txSize == 0 ? 1 : txSize);
        if (!blk->transactions) {
            free(blk);
            fclose(chainFile);
            fclose(tableFile);
            return false;
        }

        for (size_t i = 0; i < txSize; ++i) {
            signed_transaction_t tx;
            if (fread(&tx, sizeof(signed_transaction_t), 1, chainFile) != 1) {
                DynArr_destroy(blk->transactions);
                free(blk);
                fclose(chainFile);
                fclose(tableFile);
                return false;
            }

            if (!DynArr_push_back(blk->transactions, &tx)) {
                DynArr_destroy(blk->transactions);
                free(blk);
                fclose(chainFile);
                fclose(tableFile);
                return false;
            }
        }
    } else {
        if (txSize > 0) {
            uint64_t skipBytes = (uint64_t)txSize * (uint64_t)sizeof(signed_transaction_t);
            if (txSize != 0 && skipBytes / txSize != (uint64_t)sizeof(signed_transaction_t)) {
                free(blk);
                fclose(chainFile);
                fclose(tableFile);
                return false;
            }
            if (skipBytes > (uint64_t)LONG_MAX) {
                free(blk);
                fclose(chainFile);
                fclose(tableFile);
                return false;
            }
            if (fseek(chainFile, (long)skipBytes, SEEK_CUR) != 0) {
                free(blk);
                fclose(chainFile);
                fclose(tableFile);
                return false;
            }
        }
        blk->transactions = NULL;
    }

    fclose(chainFile);
    fclose(tableFile);

    *outBlock = blk;
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
    // Block timestamps are stored in milliseconds, so the target window must be ms too.
    uint64_t actualTime = 0;
    if (lastBlock->header.timestamp > adjustmentBlock->header.timestamp) {
        actualTime = lastBlock->header.timestamp - adjustmentBlock->header.timestamp;
    }
    if (actualTime == 0) {
        return currentTarget; // Invalid/non-increasing time window; keep current target
    }

    const uint64_t targetTime = (uint64_t)TARGET_BLOCK_TIME * 1000ULL * (uint64_t)DIFFICULTY_ADJUSTMENT_INTERVAL;
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
