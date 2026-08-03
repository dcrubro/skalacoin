#include <block/chain.h>
#include <constants.h>
#include <runtime_state.h>
#include <txmempool.h>
#include <nets/fetch_scheduler.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <pthread.h>

uint64_t currentBlockHeight = 0;

// Defined near the DAG helpers at the bottom; needed early by Chain_AddBlockLocked, which verifies
// proof of work while already holding chainLock for writing.
static bool Chain_DagParamsForHeightLocked(blockchain_t* chain, uint64_t blockHeight,
                                           size_t* outDagBytes, uint8_t outSeed[32]);

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

/**
 * Debit a sender and advance its replay guard in one step.
 *
 * `txTimestamp` is the timestamp of the transaction doing the spending; it becomes the account's
 * `lastTxTimestamp`, which is what makes a byte-identical replay of that transaction invalid later.
 * Taking it here rather than updating the entry separately means the debit and the guard cannot
 * drift apart -- every path that spends from an account necessarily advances it.
**/
static bool DebitAddress(const uint8_t address[32], const uint256_t* amount, uint64_t txTimestamp) {
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

    if (txTimestamp > entry.lastTxTimestamp) {
        entry.lastTxTimestamp = txTimestamp;
    }

    return BalanceSheet_Insert(entry) >= 0;
}

/**
 * Borrow the transaction list of the block at `index`.
 *
 * In-memory blocks are compacted to headers only once they have been persisted (Chain_SaveToFile)
 * or loaded without transactions (Chain_LoadFromFile), so any full replay of the chain has to be
 * able to fall back to the on-disk copy. On success `*outLoadedFromDisk` tells the caller whether
 * the returned block is a temporary that must be released with Chain_ReturnBlockTransactions.
 * Takes no locks; callers are expected to already hold `chainLock`.
**/
static bool Chain_BorrowBlockTransactions(blockchain_t* chain, size_t index, block_t** outBlock, bool* outLoadedFromDisk) {
    if (!chain || !chain->blocks || !outBlock || !outLoadedFromDisk) {
        return false;
    }

    *outBlock = NULL;
    *outLoadedFromDisk = false;

    block_t* blk = (block_t*)DynArr_at(chain->blocks, index);
    if (blk && blk->transactions) {
        *outBlock = blk;
        return true;
    }

    block_t* loadedBlk = NULL;
    size_t txCount = 0;
    if (!Chain_LoadBlockFromFile(chainDataDir, (uint64_t)index, true, &loadedBlk, &txCount) || !loadedBlk) {
        return false;
    }

    *outBlock = loadedBlk;
    *outLoadedFromDisk = true;
    return true;
}

static void Chain_ReturnBlockTransactions(block_t* blk, bool loadedFromDisk) {
    if (!loadedFromDisk || !blk) {
        return;
    }

    if (blk->transactions) {
        DynArr_destroy(blk->transactions);
    }
    free(blk);
}

bool Chain_RecomputeRuntimeState(blockchain_t* chain) {
    if (!chain) {
        return false;
    }

    uint256_t rebuiltSupply = uint256_from_u64(0);
    for (size_t i = 0; i < chain->size; ++i) {
        block_t* blk = NULL;
        bool loadedFromDisk = false;
        if (!Chain_BorrowBlockTransactions(chain, i, &blk, &loadedFromDisk)) {
            return false;
        }

        for (size_t j = 0; j < DynArr_size(blk->transactions); ++j) {
            signed_transaction_t* tx = (signed_transaction_t*)DynArr_at(blk->transactions, j);
            if (!tx) {
                Chain_ReturnBlockTransactions(blk, loadedFromDisk);
                return false;
            }

            if (Address_IsCoinbase(tx->transaction.senderAddress)) {
                if (uint256_add_u64(&rebuiltSupply, tx->transaction.amount1)) {
                    Chain_ReturnBlockTransactions(blk, loadedFromDisk);
                    return false;
                }
            }
        }

        Chain_ReturnBlockTransactions(blk, loadedFromDisk);
    }

    currentSupply = rebuiltSupply;
    // *AtHeight: never take chainLock from here, callers may already hold it.
    currentReward = CalculateBlockRewardAtHeight(currentSupply, (uint64_t)chain->size);
    return true;
}

/**
 * Drop the memoised DAG size recurrence.
 *
 * Every epoch's size is folded from the votes of every epoch before it, so any change at or below
 * the tip invalidates the whole table. It is only a cache -- rebuilding it costs one pass over the
 * headers reading a single byte each -- so blowing it away wholesale is both correct and cheap,
 * and far safer than trying to work out which suffix a reorg actually disturbed.
 *
 * Takes `dagCacheLock`. Safe to call while holding `chainLock` (that is the required lock order);
 * must NOT be called while already holding `dagCacheLock`.
**/
static void Chain_InvalidateDagEpochs(blockchain_t* chain) {
    if (!chain) {
        return;
    }

    pthread_mutex_lock(&chain->dagCacheLock);
    chain->dagEpochsComputed = 0;
    pthread_mutex_unlock(&chain->dagCacheLock);
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
    Chain_InvalidateDagEpochs(chain);
}

blockchain_t* Chain_Create() {
    blockchain_t* ptr = (blockchain_t*)malloc(sizeof(blockchain_t));
    if (!ptr) {
        return NULL;
    }

    ptr->blocks = DYNARR_CREATE(block_t, 1);
    ptr->size = 0;

    ptr->dagEpochs = NULL;
    ptr->dagEpochsComputed = 0;
    ptr->dagEpochsCapacity = 0;
    if (pthread_mutex_init(&ptr->dagCacheLock, NULL) != 0) {
        DynArr_destroy(ptr->blocks);
        free(ptr);
        return NULL;
    }

    return ptr;
}

void Chain_Destroy(blockchain_t* chain) {
    if (chain) {
        if (chain->blocks) {
            Chain_ClearBlocks(chain);
            DynArr_destroy(chain->blocks);
        }
        free(chain->dagEpochs);
        pthread_mutex_destroy(&chain->dagCacheLock);
        free(chain);
    }
}

/**
 * Append `block` to the tip.
 *
 * Caller MUST already hold `chainLock` (write) and `balanceSheetLock`, and MUST call
 * Chain_OnTipAdvanced afterwards (outside the locks) if this returns true. Chain_AddBlock is the
 * locked wrapper for single appends; Chain_ReplaceBranch drives this directly so that a whole
 * branch swap happens under one lock acquisition.
**/
static bool Chain_AddBlockLocked(blockchain_t* chain, block_t* block) {
    bool ok = true;
    block_t* stored = NULL; // the chain's own copy; the caller's `block` loses its transactions below

    if (!chain || !block || !chain->blocks || !block->transactions) {
        return false;
    }

    // Ensure the incoming block's header.blockNumber matches the index it will be appended at.
    size_t expectedIndex = DynArr_size(chain->blocks);
    if (block->header.blockNumber != expectedIndex) {
        // Mismatched block number; reject to avoid duplicate indices or inconsistent headers.
        return false;
    }

    // Ensure the block actually builds on our tip. Without this, a rollback-then-reapply path can
    // splice blocks from two different forks into a chain that no longer links up.
    if (expectedIndex > 0) {
        block_t* parent = (block_t*)DynArr_at(chain->blocks, expectedIndex - 1);
        if (!parent) {
            return false;
        }

        uint8_t parentHash[32];
        Block_CalculateHash(parent, parentHash);
        if (memcmp(parentHash, block->header.prevHash, sizeof(parentHash)) != 0) {
            printf("Chain_AddBlock: validation failed: blockIndex=%zu prevHash does not match current tip\n",
                expectedIndex);
            return false;
        }
    }

    // Ensure the block was mined at the difficulty this chain requires at that height. Without this
    // a peer whose difficulty went stale (or a malicious one) can hand us a block mined at an
    // easier target, which Block_HasValidProofOfWork accepts because it checks the header's own value.
    /**
     * Proof of work is verified HERE, at the moment a block joins the chain, and not only on the
     * receive path.
     *
     * A block on a competing branch cannot always be PoW-checked when it arrives: its epoch seed is
     * the last block of the previous epoch ON ITS OWN BRANCH, which may still be sitting in the
     * orphan pool rather than in our chain. Those blocks are pooled with PoW deferred, so this is
     * the checkpoint that keeps the real invariant -- nothing enters the chain without verified
     * work. By the time a branch is applied the rollback has put its ancestors in place, so the
     * epoch parameters always resolve here.
    **/
    {
        size_t dagBytes = 0;
        uint8_t dagSeed[32];
        if (!Chain_DagParamsForHeightLocked(chain, block->header.blockNumber, &dagBytes, dagSeed)) {
            printf("Chain_AddBlock: validation failed: blockIndex=%zu cannot resolve epoch DAG parameters\n",
                expectedIndex);
            return false;
        }

        const uint64_t epochIndex = block->header.blockNumber / (uint64_t)EPOCH_LENGTH;
        if (!Block_HasValidProofOfWorkWithParams(block, epochIndex, dagBytes, dagSeed)) {
            printf("Chain_AddBlock: validation failed: blockIndex=%zu proof of work is invalid\n",
                expectedIndex);
            return false;
        }
    }

    uint32_t expectedTarget = Chain_GetTargetForHeight(chain, (uint64_t)expectedIndex);
    if (block->header.difficultyTarget != expectedTarget) {
        printf("Chain_AddBlock: validation failed: blockIndex=%zu expectedDifficulty=%#x observedDifficulty=%#x\n",
            expectedIndex,
            (unsigned int)expectedTarget,
            (unsigned int)block->header.difficultyTarget);
        return false;
    }

    do {
        size_t txCount = DynArr_size(block->transactions);
        signed_transaction_t* candidateTxs = (signed_transaction_t*)calloc(txCount, sizeof(signed_transaction_t));
        if (!candidateTxs) {
            ok = false;
            break;
        }

        size_t nonCoinbaseCount = 0;
        for (size_t i = 0; i < txCount; ++i) {
            signed_transaction_t* tx = (signed_transaction_t*)DynArr_at(block->transactions, i);
            if (!tx) {
                ok = false;
                break;
            }

            candidateTxs[i] = *tx;
            if (!Address_IsCoinbase(tx->transaction.senderAddress)) {
                ++nonCoinbaseCount;
            }
        }

        if (!ok) {
            free(candidateTxs);
            break;
        }

        signed_transaction_t* spendableTxs = NULL;
        size_t spendableCount = 0;
        uint64_t totalFees = 0;
        if (!BalanceSheet_SelectSpendableTransactions(candidateTxs, txCount, &spendableTxs, &spendableCount, &totalFees)) {
            free(candidateTxs);
            ok = false;
            break;
        }

        free(candidateTxs);

        if (spendableCount != nonCoinbaseCount) {
            free(spendableTxs);
            ok = false;
            break;
        }

        uint64_t expectedCoinbaseAmount = currentReward;
        if (UINT64_MAX - expectedCoinbaseAmount < totalFees) {
            free(spendableTxs);
            ok = false;
            break;
        }
        expectedCoinbaseAmount += totalFees;

            // Debug: log expected coinbase and fees to aid diagnosis when nodes disagree
            {
                uint64_t cbAmount = 0;
                if (block->transactions && DynArr_size(block->transactions) > 0) {
                    signed_transaction_t* firstTx = (signed_transaction_t*)DynArr_at(block->transactions, 0);
                    if (firstTx && Address_IsCoinbase(firstTx->transaction.senderAddress)) {
                        cbAmount = firstTx->transaction.amount1;
                    }
                }
                char supplyStr[80];
                Uint256ToDecimal(&currentSupply, supplyStr, sizeof(supplyStr));
                printf("Chain_AddBlock: blockIndex=%zu expectedCoinbase=%llu totalFees=%llu observedBlockCoinbase=%llu currentReward=%llu currentSupply=%s\n",
                    expectedIndex,
                    (unsigned long long)expectedCoinbaseAmount,
                    (unsigned long long)totalFees,
                    (unsigned long long)cbAmount,
                    (unsigned long long)currentReward,
                    supplyStr);
            }

        uint64_t observedFees = 0;
        if (!Block_ValidateCoinbaseAndFees(block, expectedCoinbaseAmount, &observedFees) || observedFees != totalFees) {
            // Log mismatch details for debugging
            printf("Chain_AddBlock: validation failed: expectedCoinbase=%llu totalFees=%llu observedFees=%llu\n",
                (unsigned long long)expectedCoinbaseAmount,
                (unsigned long long)totalFees,
                (unsigned long long)observedFees);
            free(spendableTxs);
            ok = false;
            break;
        }

        free(spendableTxs);

        /**
         * Replay guard: each sender's transaction timestamps must strictly increase.
         *
         * Without this, any transaction already in the chain could be rebroadcast and mined again,
         * debiting the sender a second time. UTXO chains get this for free because the inputs no
         * longer exist; an account model has nothing to stop it. Timestamps are unix milliseconds,
         * so two genuinely distinct transactions never collide, and an exact collision means a
         * byte-identical copy -- precisely what must be refused.
         *
         * Checked HERE, with the rest of block validation and before the push, rather than in the
         * ledger pass below: that pass runs after the block is already in the chain and can only
         * return false, leaving an invalid block behind.
         *
         * Placement in Chain_AddBlockLocked is what makes it apply everywhere -- mining, broadcast,
         * windowed sync, orphan attach and reorg apply all funnel through here, and it is the only
         * insertion point into the chain besides the header-only disk load. Do not add a second
         * copy on the receive path where the symptom happens to be visible.
        **/
        if (!Chain_BlockRespectsSenderOrdering(block)) {
            printf("Chain_AddBlock: validation failed: blockIndex=%zu contains a transaction that is "
                   "not newer than its sender's last -- replay or out-of-order\n", expectedIndex);
            ok = false;
            break;
        }

        // Push the block only after validation succeeds.
        block_t* blk = (block_t*)DynArr_push_back(chain->blocks, block);
        if (!blk) { ok = false; break; }
        stored = blk;
        chain->size++;
        currentBlockHeight = (uint64_t)(chain->size - 1);

        /**
         * The chain now owns the transaction array, so clear the CALLER's pointer to it.
         *
         * DynArr_push_back stores the struct by value, which leaves the chain's element and the
         * caller's block_t sharing one `transactions` pointer. Three separate places later free
         * that array through the chain's copy -- Chain_ClearBlocks, Chain_RollbackToHeightLocked
         * and Chain_SaveToFile -- and each of them NULLs only the chain's side, leaving the
         * caller holding a dangling pointer. Whether a caller must then use free() or
         * Block_Destroy() was a convention carried in comments at every call site plus a
         * consumed-count passed around; getting it wrong aborted in the allocator.
         *
         * Clearing it here makes the rule structural: free(wrapper) and Block_Destroy(wrapper)
         * are now equivalent and both safe, because DynArr_destroy(NULL) is a no-op.
         *
         * This runs right after the push and NOT at the end on success, deliberately. If the
         * ledger pass below fails we return false with the block still in the chain, so a caller
         * that destroys its wrapper on failure would otherwise free the chain's array.
        **/
        block->transactions = NULL;

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
                    if (!BuildSpendAmount(tx, &spend) ||
                        !DebitAddress(tx->transaction.senderAddress, &spend, tx->transaction.timestamp)) {
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

        // Remove mined non-coinbase transactions from the mempool so they are not re-mined or re-broadcast.
        if (blk->transactions) {
            for (size_t i = 0; i < DynArr_size(blk->transactions); ++i) {
                signed_transaction_t* tx = (signed_transaction_t*)DynArr_at(blk->transactions, i);
                if (!tx) continue;
                if (Address_IsCoinbase(tx->transaction.senderAddress)) continue;
                uint8_t txHash[32];
                Transaction_CalculateHash(tx, txHash);
                if (TxMempool_Remove(txHash)) {
                    // optional: log removal
                    // printf("TxMempool_Remove: removed tx from mempool: "); PrintHexBytes(txHash, 32); printf("\n");
                }
            }
        }

        // Advance supply and reward here rather than in each caller. Callers used to do this
        // themselves, which meant the orphan-attach and maintenance-thread paths never did it: the
        // next block's coinbase was then validated against a stale currentReward and rejected
        // forever. It also has to happen per block so that applying a whole branch works.
        if (ok) {
            (void)uint256_add_u64(&currentSupply, expectedCoinbaseAmount);
            // Must be the *AtHeight variant: we hold chainLock for writing here, and
            // CalculateBlockReward would take it for reading via Chain_Size. chainLock is not
            // recursive, so that self-deadlocks as soon as another thread queues for the write lock.
            currentReward = CalculateBlockRewardAtHeight(currentSupply, (uint64_t)chain->size);
        }
        // ok remains true if no failures
    } while (0);

    if (ok) {
        printf("Added new block to chain:\n");
        Block_ShortPrint(stored ? stored : block); // stored still has the transactions; `block` no longer does
    }

    return ok;
}

bool Chain_AddBlock(blockchain_t* chain, block_t* block) {
    if (!chain || !block || !chain->blocks || !block->transactions) {
        return false;
    }

    // Acquire global write locks to protect chain and balance sheet mutations.
    pthread_rwlock_wrlock(&chainLock);
    pthread_mutex_lock(&balanceSheetLock);

    bool ok = Chain_AddBlockLocked(chain, block);

    pthread_mutex_unlock(&balanceSheetLock);
    pthread_rwlock_unlock(&chainLock);

    if (ok) {
        // Every path that appends comes through here, so this is where difficulty/DAG catch up.
        Chain_OnTipAdvanced(chain);
    }

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

/**
 * Truncate the chain to `height` blocks and rebuild the balance sheet and supply from what remains.
 *
 * Caller MUST already hold `chainLock` (write) and `balanceSheetLock`, and MUST call
 * Chain_OnTipAdvanced afterwards (outside the locks). Chain_RollbackToHeight is the locked wrapper.
**/
static bool Chain_RollbackToHeightLocked(blockchain_t* chain, size_t height) {
    if (!chain || !chain->blocks) return false;

    size_t cur = DynArr_size(chain->blocks);
    if (height >= cur) {
        return true; // nothing to do
    }

    /**
     * Return the discarded blocks' transactions to the mempool before anything is freed.
     *
     * Without this a reorg silently destroys them: they were removed from the mempool when mined
     * (see Chain_AddBlockLocked) and nothing ever put them back, so a transaction that existed only
     * in an orphaned block was gone from both the chain and the pool and would never be mined
     * unless its sender happened to rebroadcast. An accidental orphan should cost a transaction one
     * block of delay, not its existence.
     *
     * Coinbases are deliberately NOT restored -- they are bound to a specific height and reward and
     * are invalid anywhere else.
     *
     * A transaction that also appears in the branch replacing this one needs no special handling:
     * Chain_ReplaceBranch rolls back and then applies under a single lock acquisition, and
     * Chain_AddBlockLocked removes every applied transaction from the pool again. So it is
     * re-inserted here and removed again moments later, with no window in which a miner could pick
     * up a copy of something already back in the chain. That ordering is what makes this safe in an
     * account model, where re-mining a transaction would debit the sender twice.
     *
     * Blocks are usually header-only by now (anything saved or loaded has transactions == NULL), so
     * this goes through the same borrow/disk-fallback pair the balance-sheet replay below uses.
    **/
    for (size_t i = cur; i > height; --i) {
        const size_t idx = i - 1;

        block_t* source = NULL;
        bool loadedFromDisk = false;
        if (!Chain_BorrowBlockTransactions(chain, idx, &source, &loadedFromDisk)) {
            // Nothing recoverable for this block; the rollback itself must still proceed.
            continue;
        }

        if (source && source->transactions) {
            const size_t txCount = DynArr_size(source->transactions);
            for (size_t t = 0; t < txCount; ++t) {
                signed_transaction_t* tx = (signed_transaction_t*)DynArr_at(source->transactions, t);
                if (!tx || Address_IsCoinbase(tx->transaction.senderAddress)) {
                    continue;
                }
                (void)TxMempool_Insert(*tx);
            }
        }

        Chain_ReturnBlockTransactions(source, loadedFromDisk);
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

    // Blocks below the old tip are gone, so the DAG recurrence folded from their votes is stale.
    Chain_InvalidateDagEpochs(chain);

    // Rebuild balance sheet from scratch up to current chain size
    BalanceSheet_Destroy();
    BalanceSheet_Init();

    // Supply is accumulated in this same pass. Doing it here rather than in a second
    // Chain_RecomputeRuntimeState pass is what lets rollback work at all: that function used to
    // bail on any header-only block, which is every block once the chain has been saved or loaded.
    uint256_t rebuiltSupply = uint256_from_u64(0);

    for (size_t i = 0; i < chain->size; ++i) {
        block_t* toProcess = NULL;
        bool loaded = false;

        if (!Chain_BorrowBlockTransactions(chain, i, &toProcess, &loaded)) {
            // Can't rebuild without transactions
            return false;
        }

        // Apply transactions
        if (toProcess && toProcess->transactions) {
            size_t txCount = DynArr_size(toProcess->transactions);
            for (size_t ti = 0; ti < txCount; ++ti) {
                signed_transaction_t* tx = (signed_transaction_t*)DynArr_at(toProcess->transactions, ti);
                if (!tx) continue;

                // Coinbase credit
                if (Address_IsCoinbase(tx->transaction.senderAddress)) {
                    (void)uint256_add_u64(&rebuiltSupply, tx->transaction.amount1);
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
                // Rebuild the replay guard in the same pass that rebuilds the balance. Because this
                // replay is what a rollback already does, the guard needs no invalidation logic of
                // its own -- after any reorg it reflects exactly the blocks that remain.
                if (tx->transaction.timestamp > senderEntry.lastTxTimestamp) {
                    senderEntry.lastTxTimestamp = tx->transaction.timestamp;
                }
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

        Chain_ReturnBlockTransactions(toProcess, loaded);
    }

    currentSupply = rebuiltSupply;
    // *AtHeight: chainLock is held for writing here (see Chain_RollbackToHeight).
    currentReward = CalculateBlockRewardAtHeight(currentSupply, (uint64_t)chain->size);

    return true;
}

bool Chain_RollbackToHeight(blockchain_t* chain, size_t height) {
    if (!chain || !chain->blocks) return false;

    pthread_rwlock_wrlock(&chainLock);
    pthread_mutex_lock(&balanceSheetLock);

    bool ok = Chain_RollbackToHeightLocked(chain, height);

    pthread_mutex_unlock(&balanceSheetLock);
    pthread_rwlock_unlock(&chainLock);

    // A reorg can move the tip back across an adjustment boundary, so the target must come down too.
    Chain_OnTipAdvanced(chain);

    return ok;
}

static int Chain_CompareTimestamps(const void* lhs, const void* rhs) {
    const uint64_t a = *(const uint64_t*)lhs;
    const uint64_t b = *(const uint64_t*)rhs;
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

/**
 * Median timestamp of the last MEDIAN_TIME_SPAN blocks. Caller must hold `chainLock`.
**/
static uint64_t Chain_MedianTimePastLocked(blockchain_t* chain) {
    if (!chain || !chain->blocks) {
        return 0ULL;
    }

    const size_t size = DynArr_size(chain->blocks);
    if (size == 0) {
        return 0ULL;
    }

    size_t span = size < MEDIAN_TIME_SPAN ? size : MEDIAN_TIME_SPAN;
    uint64_t samples[MEDIAN_TIME_SPAN];
    size_t taken = 0;
    for (size_t i = 0; i < span; ++i) {
        block_t* blk = (block_t*)DynArr_at(chain->blocks, size - 1 - i);
        if (!blk) {
            continue;
        }
        samples[taken++] = blk->header.timestamp;
    }

    if (taken == 0) {
        return 0ULL;
    }

    qsort(samples, taken, sizeof(uint64_t), Chain_CompareTimestamps);
    return samples[taken / 2];
}

bool Chain_IsInitialBlockDownload(blockchain_t* chain) {
    if (!chain || !chain->blocks) {
        return true;
    }

    if (DynArr_size(chain->blocks) == 0) {
        return true;
    }

    const uint64_t medianTime = Chain_MedianTimePastLocked(chain);
    const uint64_t now = get_current_time_ms();
    if (medianTime == 0ULL || now <= medianTime) {
        return false; // we are at (or ahead of) the current tip time
    }

    const uint64_t ageMs = now - medianTime;
    const uint64_t thresholdMs = IBD_TIP_AGE_BLOCKS * (uint64_t)TARGET_BLOCK_TIME * 1000ULL;
    return ageMs > thresholdMs;
}

/**
 * Verify that a candidate branch is internally linked and attaches to the block below `forkHeight`.
 * Caller must hold `chainLock`.
**/
static bool Chain_BranchIsLinkedLocked(blockchain_t* chain, size_t forkHeight, block_t** blocks, size_t count) {
    if (!chain || !chain->blocks || !blocks || count == 0 || forkHeight == 0) {
        return false;
    }

    block_t* parent = (block_t*)DynArr_at(chain->blocks, forkHeight - 1);
    if (!parent) {
        return false;
    }

    uint8_t expectedPrevHash[32];
    Block_CalculateHash(parent, expectedPrevHash);

    for (size_t i = 0; i < count; ++i) {
        if (!blocks[i] || !blocks[i]->transactions) {
            return false;
        }
        if (blocks[i]->header.blockNumber != (uint64_t)(forkHeight + i)) {
            return false;
        }
        if (memcmp(blocks[i]->header.prevHash, expectedPrevHash, sizeof(expectedPrevHash)) != 0) {
            return false;
        }
        Block_CalculateHash(blocks[i], expectedPrevHash);
    }

    return true;
}

/**
 * Free an array of blocks and the array itself.
 *
 * No consumed-count is needed: Chain_AddBlockLocked clears the caller's `transactions` pointer when
 * it takes ownership, so Block_Destroy is correct whether or not a given block reached the chain.
 * The count used to be threaded through here, and getting it wrong -- resetting it after a rollback
 * had already freed the aliased arrays -- is what aborted in the allocator.
**/
static void Chain_FreeBlockArray(block_t** blocks, size_t count) {
    if (!blocks) {
        return;
    }

    for (size_t i = 0; i < count; ++i) {
        if (blocks[i]) {
            Block_Destroy(blocks[i]);
        }
    }

    free(blocks);
}

bool Chain_BlockRespectsSenderOrdering(const block_t* block) {
    if (!block || !block->transactions) {
        return false;
    }

    const size_t txCount = DynArr_size(block->transactions);

    for (size_t i = 0; i < txCount; ++i) {
        const signed_transaction_t* tx = (const signed_transaction_t*)DynArr_at(block->transactions, i);
        if (!tx || Address_IsCoinbase(tx->transaction.senderAddress)) {
            continue; // coinbase is exempt -- see balance_sheet.h
        }

        // Baseline is THIS sender's own last included transaction.
        uint64_t lastSeen = 0;
        balance_sheet_entry_t senderEntry;
        if (BalanceSheet_Lookup((uint8_t*)tx->transaction.senderAddress, &senderEntry)) {
            lastSeen = senderEntry.lastTxTimestamp;
        }

        /**
         * Fold in earlier transactions from THE SAME SENDER in this same block, which the sheet
         * does not know about yet: the ledger applies a block in order, so they must be ordered.
         *
         * The sender comparison is load-bearing. Without it this would absorb every other sender's
         * timestamps and reject ordinary multi-sender blocks -- Alice's transaction would be judged
         * against Bob's. Senders are strictly independent here.
         *
         * Quadratic in the worst case (a whole block from one sender). Fine while blocks are small;
         * worth a per-sender map if they grow.
        **/
        for (size_t j = 0; j < i; ++j) {
            const signed_transaction_t* prev = (const signed_transaction_t*)DynArr_at(block->transactions, j);
            if (!prev || Address_IsCoinbase(prev->transaction.senderAddress)) {
                continue;
            }
            if (memcmp(prev->transaction.senderAddress, tx->transaction.senderAddress, 32) != 0) {
                continue; // different sender -- says nothing about this one's ordering
            }
            if (prev->transaction.timestamp > lastSeen) {
                lastSeen = prev->transaction.timestamp;
            }
        }

        if (tx->transaction.timestamp <= lastSeen) {
            return false;
        }
    }

    return true;
}

uint64_t Chain_ReorgPenaltyForDepth(uint64_t reorgDepth) {
    return FetchScheduler_ComputeReorgPenaltyBlocks(reorgDepth);
}

bool Chain_ReplaceBranch(blockchain_t* chain,
                         size_t forkHeight,
                         block_t** newBlocks,
                         size_t count,
                         uint64_t observedAtTipHeight,
                         bool bypassPenalty) {
    if (!chain || !chain->blocks || !newBlocks || count == 0 || forkHeight == 0) {
        return false;
    }

    pthread_rwlock_wrlock(&chainLock);
    pthread_mutex_lock(&balanceSheetLock);

    bool ok = false;
    block_t** snapshot = NULL;      // deep copies of the blocks we are replacing
    size_t snapshotCount = 0;
    block_t** candidate = NULL;     // deep copies of the branch we are applying
    size_t candidateApplied = 0;    // how far the apply got, for the log line below

    do {
        const size_t tipCount = DynArr_size(chain->blocks);
        if (forkHeight > tipCount) {
            printf("Chain_ReplaceBranch: fork point %zu is beyond our tip %zu; nothing to replace\n",
                forkHeight, tipCount);
            break;
        }

        if (!Chain_BranchIsLinkedLocked(chain, forkHeight, newBlocks, count)) {
            printf("Chain_ReplaceBranch: candidate branch at height %zu is not linked; refusing\n", forkHeight);
            break;
        }

        // Horizen-style delayed block submission penalty. A branch that forks `depth` blocks below
        // our tip is held until our own chain has advanced `penalty(depth)` blocks, so a rented-
        // hashrate attacker cannot win by dumping a privately mined branch in one go. The depth is
        // measured at FIRST OBSERVATION and never recomputed: if it were re-derived from the moving
        // tip, depth and elapsed would both grow by one per block while penalty(depth) grows
        // faster, and a penalized branch could never be adopted at all.
        // The initial-block-download exemption is decided here from local state only, never handed
        // in by a caller: a peer claiming a huge height must not be able to switch the penalty off.
        const bool inInitialBlockDownload = Chain_IsInitialBlockDownload(chain);
        const uint64_t tipHeight = tipCount > 0 ? (uint64_t)(tipCount - 1) : 0ULL;
        if (bypassPenalty && tipCount > forkHeight) {
            // Operator override, never reachable from anything a peer sends. Announced loudly
            // because it is the one place the delay protection is deliberately not applied.
            printf("Chain_ReplaceBranch: reorg penalty BYPASSED at height %zu by operator request\n",
                forkHeight);
        }
        if (!bypassPenalty && !inInitialBlockDownload && tipCount > forkHeight) {
            const uint64_t observedTip = observedAtTipHeight > tipHeight ? tipHeight : observedAtTipHeight;
            const uint64_t depth = observedTip >= (uint64_t)forkHeight
                ? (observedTip - (uint64_t)forkHeight + 1ULL)
                : 1ULL;
            const uint64_t penalty = FetchScheduler_ComputeReorgPenaltyBlocks(depth);

            /**
             * The delay can be served by EITHER side making progress.
             *
             * Measuring it against our own chain growth alone assumed we are an active participant
             * whose tip advances. A node that is behind is not: its tip is frozen precisely because
             * it is rejecting the branch, so `elapsed` stays 0 forever and it can never rejoin. That
             * is not a reorg contest at all -- being 500 blocks behind is being behind, and
             * refusing to adopt protects nothing while the node falls further back every hour.
             *
             * A branch that already extends `penalty` blocks past our tip has demonstrated exactly
             * what the delay asks for -- that much sustained public work -- and every one of those
             * blocks carries PoW we validated ourselves. Requiring us to independently produce the
             * same amount is demanding the same proof twice. Counting only VALIDATED blocks is what
             * keeps this safe: a peer's advertised height is not evidence and never reaches here.
             *
             * The rule degrades correctly in both directions. A mining node's tip advances, so an
             * attacker has to outpace it by `penalty` blocks. A node that is merely observing
             * follows the heaviest chain, which is what an observer should do.
            **/
            const uint64_t localGrowth = tipHeight >= observedTip ? (tipHeight - observedTip) : 0ULL;
            const uint64_t candidateTip = (uint64_t)forkHeight + (uint64_t)count - 1ULL;
            const uint64_t branchLead = candidateTip > tipHeight ? (candidateTip - tipHeight) : 0ULL;
            const uint64_t elapsed = localGrowth > branchLead ? localGrowth : branchLead;

            if (elapsed < penalty) {
                // The maintenance thread retries pooled branches once a second, and `elapsed` can
                // only change when our own tip moves -- so without this the same line is printed
                // every second for as long as the branch stays deferred, which on a node that is
                // not mining is forever. Report each distinct deferral once, and again whenever the
                // situation actually changes. Guarded by chainLock (held for writing here).
                static size_t lastDeferredFork = SIZE_MAX;
                static uint64_t lastDeferredTip = UINT64_MAX;
                static uint64_t lastDeferredPenalty = UINT64_MAX;
                static uint64_t lastDeferredElapsed = UINT64_MAX;
                if (forkHeight != lastDeferredFork || tipHeight != lastDeferredTip ||
                    penalty != lastDeferredPenalty || elapsed != lastDeferredElapsed) {
                    printf("Chain_ReplaceBranch: deferring reorg at height %zu: depth=%" PRIu64
                           " penalty=%" PRIu64 " elapsed=%" PRIu64
                           " (localGrowth=%" PRIu64 " branchLead=%" PRIu64 ")\n",
                        forkHeight, depth, penalty, elapsed, localGrowth, branchLead);
                    lastDeferredFork = forkHeight;
                    lastDeferredTip = tipHeight;
                    lastDeferredPenalty = penalty;
                    lastDeferredElapsed = elapsed;
                }
                break;
            }
        }

        // Most cumulative work wins, not most blocks. Difficulty varies, so a long low-difficulty
        // branch must not beat a short high-difficulty one. Strictly greater, so tied tips do not
        // cause the two nodes to keep swapping.
        uint256_t incumbentWork;
        uint256_t candidateWork;
        if (!Chain_ComputeWorkRange(chain, forkHeight, tipCount, &incumbentWork) ||
            !Chain_ComputeBranchWork(newBlocks, count, &candidateWork)) {
            printf("Chain_ReplaceBranch: could not compute work for the branch at height %zu\n", forkHeight);
            break;
        }
        if (uint256_cmp(&candidateWork, &incumbentWork) <= 0) {
            // Very often this just means the branch is still arriving -- a partial branch is
            // genuinely lighter than what it would replace. Report the block counts so that case
            // is distinguishable from a peer that really is on a weaker chain.
            printf("Chain_ReplaceBranch: candidate at height %zu is not heavier "
                   "(%zu candidate block(s) vs %zu incumbent); keeping our chain\n",
                forkHeight, count, tipCount - forkHeight);
            break;
        }

        // Snapshot what we are about to discard so a failed apply can be undone. The in-memory
        // blocks are freed by the rollback and the on-disk copy is overwritten by the next save,
        // so without this a partial apply would be unrecoverable.
        snapshotCount = tipCount - forkHeight;
        if (snapshotCount > 0) {
            snapshot = (block_t**)calloc(snapshotCount, sizeof(block_t*));
            if (!snapshot) {
                printf("Chain_ReplaceBranch: out of memory snapshotting %zu block(s); chain unchanged\n",
                    snapshotCount);
                break;
            }

            bool snapshotOk = true;
            for (size_t i = 0; i < snapshotCount; ++i) {
                block_t* src = NULL;
                bool loadedFromDisk = false;
                if (!Chain_BorrowBlockTransactions(chain, forkHeight + i, &src, &loadedFromDisk)) {
                    snapshotOk = false;
                    break;
                }
                snapshot[i] = Block_Copy(src);
                Chain_ReturnBlockTransactions(src, loadedFromDisk);
                if (!snapshot[i]) {
                    snapshotOk = false;
                    break;
                }
            }
            if (!snapshotOk) {
                // Refusing here is the point: without a complete snapshot a failed apply could not
                // be undone, so we would rather not start than risk a half-replaced chain.
                printf("Chain_ReplaceBranch: could not snapshot the blocks being replaced at height %zu; "
                       "refusing the reorg rather than risk an unrecoverable apply\n", forkHeight);
                break;
            }
        }

        // Apply copies so the caller's blocks are never aliased by the chain nor destroyed by a
        // rollback -- the caller keeps ownership of what it passed in, whatever happens here.
        candidate = (block_t**)calloc(count, sizeof(block_t*));
        if (!candidate) {
            printf("Chain_ReplaceBranch: out of memory copying %zu candidate block(s); chain unchanged\n", count);
            break;
        }
        bool copiedAll = true;
        for (size_t i = 0; i < count; ++i) {
            candidate[i] = Block_Copy(newBlocks[i]);
            if (!candidate[i]) {
                copiedAll = false;
                break;
            }
        }
        if (!copiedAll) {
            printf("Chain_ReplaceBranch: could not copy the candidate branch at height %zu; chain unchanged\n",
                forkHeight);
            break;
        }

        if (!Chain_RollbackToHeightLocked(chain, forkHeight)) {
            printf("Chain_ReplaceBranch: rollback to height %zu failed; chain unchanged\n", forkHeight);
            break;
        }

        bool applied = true;
        for (size_t i = 0; i < count; ++i) {
            if (!Chain_AddBlockLocked(chain, candidate[i])) {
                applied = false;
                break;
            }
            candidateApplied++;
        }

        if (applied) {
            ok = true;
            break;
        }

        // Undo: drop whatever of the candidate branch made it in and put the original blocks back.
        printf("Chain_ReplaceBranch: candidate branch failed to apply at height %zu; restoring previous chain\n",
            forkHeight + candidateApplied);

        if (!Chain_RollbackToHeightLocked(chain, forkHeight)) {
            fprintf(stderr, "Chain_ReplaceBranch: FAILED to roll back after a failed apply; chain is truncated to %zu\n",
                forkHeight);
            break;
        }

        for (size_t i = 0; i < snapshotCount; ++i) {
            if (!Chain_AddBlockLocked(chain, snapshot[i])) {
                fprintf(stderr, "Chain_ReplaceBranch: FAILED to restore original block %zu; chain is truncated to %zu\n",
                    forkHeight + i, forkHeight + i);
                break;
            }
        }
    } while (0);

    Chain_FreeBlockArray(snapshot, snapshotCount);
    Chain_FreeBlockArray(candidate, candidate ? count : 0);

    pthread_mutex_unlock(&balanceSheetLock);
    pthread_rwlock_unlock(&chainLock);

    Chain_OnTipAdvanced(chain);

    return ok;
}

void Chain_Wipe(blockchain_t* chain) {
    Chain_ClearBlocks(chain);
    currentBlockHeight = 0;
    difficultyTarget = INITIAL_DIFFICULTY;
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

    char metaTmpPath[512];
    char chainTmpPath[512];
    char tableTmpPath[512];
    if (!BuildPath(metaTmpPath, sizeof(metaTmpPath), dirpath, "chain.meta.tmp") ||
        !BuildPath(chainTmpPath, sizeof(chainTmpPath), dirpath, "chain.data.tmp") ||
        !BuildPath(tableTmpPath, sizeof(tableTmpPath), dirpath, "chain.table.tmp")) {
        return false;
    }

    pthread_rwlock_wrlock(&chainLock);

    FILE* metaFile = fopen(metaTmpPath, "wb+");
    FILE* chainFile = fopen(chainTmpPath, "wb+");
    FILE* tableFile = fopen(tableTmpPath, "wb+");
    if (!metaFile || !chainFile || !tableFile) {
        if (metaFile) fclose(metaFile);
        if (chainFile) fclose(chainFile);
        if (tableFile) fclose(tableFile);
        pthread_rwlock_unlock(&chainLock);
        remove(metaTmpPath);
        remove(chainTmpPath);
        remove(tableTmpPath);
        return false;
    }

    const size_t chainSize = DynArr_size(chain->blocks);
    uint64_t byteCount = 0;
    for (size_t i = 0; i < chainSize; ++i) {
        block_t* blk = (block_t*)DynArr_at(chain->blocks, i);
        if (!blk) {
            fclose(metaFile);
            fclose(chainFile);
            fclose(tableFile);
            pthread_rwlock_unlock(&chainLock);
            remove(metaTmpPath);
            remove(chainTmpPath);
            remove(tableTmpPath);
            return false;
        }

        block_t* diskCopy = blk;
        bool loadedTemp = false;
        if (!diskCopy->transactions) {
            if (!Chain_LoadBlockFromFile(dirpath, (uint64_t)i, true, &diskCopy, NULL) || !diskCopy || !diskCopy->transactions) {
                if (loadedTemp && diskCopy) {
                    Block_Destroy(diskCopy);
                }
                fclose(metaFile);
                fclose(chainFile);
                fclose(tableFile);
                pthread_rwlock_unlock(&chainLock);
                remove(metaTmpPath);
                remove(chainTmpPath);
                remove(tableTmpPath);
                return false;
            }
            loadedTemp = true;
        }

        const uint64_t blockStart = byteCount;
        if (fwrite(&diskCopy->header, sizeof(block_header_t), 1, chainFile) != 1) {
            if (loadedTemp) Block_Destroy(diskCopy);
            fclose(metaFile);
            fclose(chainFile);
            fclose(tableFile);
            pthread_rwlock_unlock(&chainLock);
            remove(metaTmpPath);
            remove(chainTmpPath);
            remove(tableTmpPath);
            return false;
        }

        const size_t txSize = DynArr_size(diskCopy->transactions);
        if (fwrite(&txSize, sizeof(size_t), 1, chainFile) != 1) {
            if (loadedTemp) Block_Destroy(diskCopy);
            fclose(metaFile);
            fclose(chainFile);
            fclose(tableFile);
            pthread_rwlock_unlock(&chainLock);
            remove(metaTmpPath);
            remove(chainTmpPath);
            remove(tableTmpPath);
            return false;
        }
        byteCount += sizeof(block_header_t) + sizeof(size_t);

        for (size_t j = 0; j < txSize; ++j) {
            signed_transaction_t* tx = (signed_transaction_t*)DynArr_at(diskCopy->transactions, j);
            if (!tx || fwrite(tx, sizeof(signed_transaction_t), 1, chainFile) != 1) {
                if (loadedTemp) Block_Destroy(diskCopy);
                fclose(metaFile);
                fclose(chainFile);
                fclose(tableFile);
                pthread_rwlock_unlock(&chainLock);
                remove(metaTmpPath);
                remove(chainTmpPath);
                remove(tableTmpPath);
                return false;
            }
            byteCount += sizeof(signed_transaction_t);
        }

        block_table_entry_t entry;
        entry.blockNumber = i;
        entry.byteNumber = blockStart;
        entry.blockSize = byteCount - blockStart;
        if (fwrite(&entry, sizeof(block_table_entry_t), 1, tableFile) != 1) {
            if (loadedTemp) Block_Destroy(diskCopy);
            fclose(metaFile);
            fclose(chainFile);
            fclose(tableFile);
            pthread_rwlock_unlock(&chainLock);
            remove(metaTmpPath);
            remove(chainTmpPath);
            remove(tableTmpPath);
            return false;
        }

        if (loadedTemp) {
            Block_Destroy(diskCopy);
        } else if (blk->transactions) {
            DynArr_destroy(blk->transactions);
            blk->transactions = NULL;
        }
    }

    size_t newSize = chainSize;
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

    fflush(metaFile);
    fflush(chainFile);
    fflush(tableFile);

    fclose(metaFile);
    fclose(chainFile);
    fclose(tableFile);

    if (rename(metaTmpPath, metaPath) != 0 || rename(chainTmpPath, chainPath) != 0 || rename(tableTmpPath, tablePath) != 0) {
        pthread_rwlock_unlock(&chainLock);
        remove(metaTmpPath);
        remove(chainTmpPath);
        remove(tableTmpPath);
        return false;
    }

    pthread_rwlock_unlock(&chainLock);
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

uint32_t Chain_ComputeTargetAtHeight(blockchain_t* chain, uint64_t height, uint32_t currentTarget) {
    if (!chain || !chain->blocks) {
        return 0x00; // Impossible difficulty, only valid hash is all zeros (practically impossible)
    }

    if (height < DIFFICULTY_ADJUSTMENT_INTERVAL) {
        // Baby-chain, return initial difficulty
        return INITIAL_DIFFICULTY;
    }

    if (height > (uint64_t)DynArr_size(chain->blocks)) {
        return 0x00; // Retarget window is not fully present in this chain
    }

    // Assuming block validation validates timestamps, we can assume they're valid and can just read them
    block_t* lastBlock = (block_t*)DynArr_at(chain->blocks, (size_t)(height - 1));
    block_t* adjustmentBlock = (block_t*)DynArr_at(chain->blocks, (size_t)(height - DIFFICULTY_ADJUSTMENT_INTERVAL));
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

    // The window holds DIFFICULTY_ADJUSTMENT_INTERVAL blocks, but the two timestamps subtracted
    // above are its FIRST and LAST, so what was measured is one fewer interval than there are
    // blocks. Bitcoin compares that span against the full interval count anyway and carries a
    // permanent ~1/(interval-1) fast bias for it; we compare against the span we actually measured,
    // so the steady state is TARGET_BLOCK_TIME rather than TARGET_BLOCK_TIME * n/(n-1).
    const uint64_t measuredIntervals = (uint64_t)DIFFICULTY_ADJUSTMENT_INTERVAL - 1ULL;
    const uint64_t targetTime = (uint64_t)TARGET_BLOCK_TIME * 1000ULL * measuredIntervals;

    // Clamp per-epoch target movement: at most x2 easier or x2 harder. Clamping the measured span
    // is equivalent to clamping the ratio, but stays in integers.
    // Everything below is deliberately integer-only: the retarget is consensus-critical, and any
    // floating-point rounding difference between nodes would make them disagree on the target.
    uint64_t clampedTime = actualTime;
    if (clampedTime > targetTime * 2ULL) {
        clampedTime = targetTime * 2ULL;
    } else if (clampedTime < targetTime / 2ULL) {
        clampedTime = targetTime / 2ULL;
    }

    uint32_t exponent = currentTarget >> 24;
    uint32_t mantissa = currentTarget & 0x007fffff;
    if (mantissa == 0 || exponent == 0) {
        return INITIAL_DIFFICULTY;
    }

    // newMantissa = mantissa * clampedTime / targetTime. The mantissa is at most 23 bits and
    // clampedTime at most 2 * targetTime (~30 bits at the configured block time), so the product
    // cannot overflow 64 bits.
    uint64_t newMantissa = ((uint64_t)mantissa * clampedTime) / targetTime;

    // Normalize to compact format range.
    while (newMantissa > 0x007fffffULL) {
        newMantissa /= 256ULL;
        exponent++;
    }
    while (newMantissa > 0ULL && newMantissa < 32768ULL && exponent > 3) { // Keep coefficient in normal range
        newMantissa *= 256ULL;
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

uint32_t Chain_GetTargetForHeight(blockchain_t* chain, uint64_t height) {
    // The target is a pure function of the chain: replay every adjustment boundary at or below
    // `height`, starting from the genesis difficulty. Never trust a cached or peer-supplied value.
    uint32_t target = INITIAL_DIFFICULTY;
    for (uint64_t h = DIFFICULTY_ADJUSTMENT_INTERVAL; h <= height; h += DIFFICULTY_ADJUSTMENT_INTERVAL) {
        target = Chain_ComputeTargetAtHeight(chain, h, target);
    }

    return target;
}

bool Chain_ComputeBlockWork(uint32_t difficultyTargetBits, uint256_t* outWork) {
    if (!outWork) {
        return false;
    }

    uint8_t targetBytes[32];
    if (!DecodeCompactTarget(difficultyTargetBits, targetBytes)) {
        return false;
    }

    const uint256_t target = uint256_from_be_bytes(targetBytes);

    // work = 2^256 / (target + 1). 2^256 is not representable, but since 2^256 >= target + 1 it
    // equals (~target / (target + 1)) + 1, which is: all integer, no approximation.
    uint256_t denominator = target;
    if (uint256_add_u64(&denominator, 1ULL) || uint256_is_zero(&denominator)) {
        return false; // target was the maximum representable value
    }

    uint256_t numerator = target;
    uint256_bitwise_not(&numerator);

    uint256_t work;
    if (!uint256_divide(&numerator, &denominator, &work)) {
        return false;
    }
    if (uint256_add_u64(&work, 1ULL)) {
        return false;
    }

    *outWork = work;
    return true;
}

bool Chain_ComputeWorkRange(blockchain_t* chain, size_t from, size_t to, uint256_t* outWork) {
    if (!chain || !chain->blocks || !outWork || from > to) {
        return false;
    }

    if (to > DynArr_size(chain->blocks)) {
        return false;
    }

    uint256_t total = uint256_from_u64(0);
    for (size_t i = from; i < to; ++i) {
        block_t* blk = (block_t*)DynArr_at(chain->blocks, i);
        if (!blk) {
            return false;
        }

        uint256_t work;
        if (!Chain_ComputeBlockWork(blk->header.difficultyTarget, &work)) {
            return false;
        }
        if (uint256_add(&total, &work)) {
            return false; // 256-bit overflow; not reachable for any real chain
        }
    }

    *outWork = total;
    return true;
}

bool Chain_ComputeBranchWork(block_t** blocks, size_t count, uint256_t* outWork) {
    if (!blocks || !outWork) {
        return false;
    }

    uint256_t total = uint256_from_u64(0);
    for (size_t i = 0; i < count; ++i) {
        if (!blocks[i]) {
            return false;
        }

        uint256_t work;
        if (!Chain_ComputeBlockWork(blocks[i]->header.difficultyTarget, &work)) {
            return false;
        }
        if (uint256_add(&total, &work)) {
            return false;
        }
    }

    *outWork = total;
    return true;
}

/**
 * Grow the memoised epoch table to hold at least `needed` entries.
 * Caller must hold `chain->dagCacheLock`.
**/
static bool Chain_ReserveDagEpochsLocked(blockchain_t* chain, size_t needed) {
    if (needed <= chain->dagEpochsCapacity) {
        return true;
    }

    size_t capacity = chain->dagEpochsCapacity ? chain->dagEpochsCapacity : 8u;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) {
            return false;
        }
        capacity *= 2u;
    }

    dag_epoch_state_t* grown =
        (dag_epoch_state_t*)realloc(chain->dagEpochs, capacity * sizeof(dag_epoch_state_t));
    if (!grown) {
        return false;
    }

    chain->dagEpochs = grown;
    chain->dagEpochsCapacity = capacity;
    return true;
}

/**
 * Fold the DAG size recurrence forward until entry `epochIndex` is valid.
 *
 * Entry k's size comes from the votes cast during epoch k-1, so extending to k requires blocks
 * [0, k * EPOCH_LENGTH) to all be present. Growth is the default: the size only stays put or falls
 * when miners actively say so, and there is no vote that makes it climb faster.
 *
 * Caller must hold `chainLock` (read) and `chain->dagCacheLock`.
**/
static bool Chain_ExtendDagEpochsLocked(blockchain_t* chain, size_t epochIndex) {
    if (!chain || !chain->blocks) {
        return false;
    }

    if (!Chain_ReserveDagEpochsLocked(chain, epochIndex + 1u)) {
        return false;
    }

    if (chain->dagEpochsComputed == 0) {
        chain->dagEpochs[0].sizeBytes = DAG_BASE_SIZE;
        chain->dagEpochs[0].downQualified = false;
        chain->dagEpochsComputed = 1;
    }

    const size_t chainSize = DynArr_size(chain->blocks);
    const size_t epochLength = (size_t)EPOCH_LENGTH;

    for (size_t k = chain->dagEpochsComputed; k <= epochIndex; ++k) {
        const size_t prev = k - 1u; // the epoch whose votes decide entry k
        const size_t from = prev * epochLength;
        const size_t to = from + epochLength; // exclusive

        if (to > chainSize) {
            // The epoch that would decide this entry has not been fully mined yet.
            return false;
        }

        uint64_t holdVotes = 0;
        uint64_t downVotes = 0;
        for (size_t i = from; i < to; ++i) {
            const block_t* blk = (const block_t*)DynArr_at(chain->blocks, i);
            if (!blk) {
                return false;
            }

            // Anything other than HOLD or DOWN counts as GROW. The accept path rejects values
            // above DAG_VOTE_MAX, so in practice the only other value that reaches here is GROW.
            const uint8_t vote = blk->header.reserved[0];
            if (vote == DAG_VOTE_HOLD) {
                holdVotes++;
            } else if (vote == DAG_VOTE_DOWN) {
                downVotes++;
            }
        }

        // Cross-multiplied rather than divided, so there is no rounding for nodes to disagree on.
        // The denominator is the constant epoch length, not the number of blocks looked at, so a
        // partial epoch can never be read as a stronger signal than it is.
        const uint64_t epochBlocks = (uint64_t)EPOCH_LENGTH;
        const bool brake = (holdVotes + downVotes) * DAG_BRAKE_DEN > epochBlocks * DAG_BRAKE_NUM;
        const bool downQualified = downVotes * DAG_DOWN_DEN > epochBlocks * DAG_DOWN_NUM;

        chain->dagEpochs[prev].downQualified = downQualified;
        const bool priorDownQualified = (prev >= 1u) ? chain->dagEpochs[prev - 1u].downQualified : false;

        const uint64_t prevSize = chain->dagEpochs[prev].sizeBytes;
        uint64_t next;

        // Order matters: a qualifying down vote also satisfies the brake condition (down > 7/8
        // implies hold+down > 1/2), so testing the brake first would make shrinking impossible.
        if (downQualified && priorDownQualified) {
            // Shrinking needs the supermajority sustained across two consecutive epochs. That gates
            // the *onset* only -- it is deliberately not reset afterwards, so miners who are
            // genuinely being squeezed keep getting relief every epoch rather than every other one.
            next = (prevSize > DAG_MIN_SIZE + DAG_EPOCH_STEP) ? (prevSize - DAG_EPOCH_STEP)
                                                              : (uint64_t)DAG_MIN_SIZE;
        } else if (brake) {
            next = prevSize;
        } else {
            next = (prevSize + DAG_EPOCH_STEP < DAG_MAX_SIZE) ? (prevSize + DAG_EPOCH_STEP)
                                                              : (uint64_t)DAG_MAX_SIZE;
        }

        chain->dagEpochs[k].sizeBytes = next;
        chain->dagEpochs[k].downQualified = false; // filled in when entry k+1 is folded
        chain->dagEpochsComputed = k + 1u;
    }

    return true;
}

/**
 * Epoch-aligned DAG seed for `blockHeight`: the genesis seed in epoch 0, otherwise the hash of the
 * last block of the previous epoch. Constant for a whole epoch, which is what lets the DAG be
 * generated once per epoch instead of once per block.
 *
 * Caller must hold `chainLock`.
**/
static bool Chain_EpochDagSeedForHeightLocked(blockchain_t* chain, uint64_t blockHeight, uint8_t outSeed[32]) {
    const uint64_t epochIndex = blockHeight / (uint64_t)EPOCH_LENGTH;
    if (epochIndex == 0) {
        memset(outSeed, DAG_GENESIS_SEED, 32);
        return true;
    }

    const uint64_t seedBlockNumber = (epochIndex * (uint64_t)EPOCH_LENGTH) - 1ULL;
    if (seedBlockNumber >= (uint64_t)DynArr_size(chain->blocks)) {
        return false;
    }

    const block_t* seedBlock = (const block_t*)DynArr_at(chain->blocks, (size_t)seedBlockNumber);
    if (!seedBlock) {
        return false;
    }

    Block_CalculateHash(seedBlock, outSeed);
    return true;
}

/**
 * As Chain_DagParamsForHeight, but assumes `chainLock` is already held (read OR write).
 *
 * Needed because Chain_AddBlockLocked verifies proof of work while holding the write lock, and
 * chainLock is not recursive -- taking it for reading there deadlocks the moment another thread
 * queues for the write lock.
**/
static bool Chain_DagParamsForHeightLocked(blockchain_t* chain, uint64_t blockHeight,
                                           size_t* outDagBytes, uint8_t outSeed[32]) {
    if (!chain || !chain->blocks || !outDagBytes || !outSeed) {
        return false;
    }

    const size_t epochIndex = (size_t)(blockHeight / (uint64_t)EPOCH_LENGTH);

    uint64_t bytes = 0;
    bool ok = false;

    // Lock order is chainLock -> dagCacheLock, everywhere. Nothing under dagCacheLock calls back
    // into chain.c, so this pair cannot deadlock.
    pthread_mutex_lock(&chain->dagCacheLock);
    if (Chain_ExtendDagEpochsLocked(chain, epochIndex)) {
        bytes = chain->dagEpochs[epochIndex].sizeBytes;
        ok = true;
    }
    pthread_mutex_unlock(&chain->dagCacheLock);

    if (ok) {
        ok = Chain_EpochDagSeedForHeightLocked(chain, blockHeight, outSeed);
    }

    if (!ok) {
        return false;
    }

    // Autolykos2 addresses the DAG in 32-byte lanes. A size that is zero or not a multiple of 32
    // would make hashing fail, and a proof that cannot be computed must never read as valid.
    if (bytes < 32ULL || (bytes % 32ULL) != 0ULL) {
        return false;
    }

    *outDagBytes = (size_t)bytes;
    return true;
}

bool Chain_DagParamsForHeight(blockchain_t* chain, uint64_t blockHeight,
                              size_t* outDagBytes, uint8_t outSeed[32]) {
    if (!chain || !chain->blocks || !outDagBytes || !outSeed) {
        return false;
    }

    pthread_rwlock_rdlock(&chainLock);
    const bool ok = Chain_DagParamsForHeightLocked(chain, blockHeight, outDagBytes, outSeed);
    pthread_rwlock_unlock(&chainLock);

    return ok;
}

void Chain_OnTipAdvanced(blockchain_t* chain) {
    if (!chain || !chain->blocks) {
        return;
    }

    size_t chainSize = Chain_Size(chain);

    // Refresh the cached target for the block that comes next, so every path that moves the tip
    // (mining, P2P accept, sync, orphan attach, reorg) stays on the same difficulty as its peers.
    difficultyTarget = Chain_GetTargetForHeight(chain, (uint64_t)chainSize);

    // The epoch DAG is deliberately NOT rebuilt here. It is a mining accelerator only -- validation
    // derives its lanes from the epoch seed, so a node that does not mine never allocates one --
    // and MineBlock builds it on demand for the height it is working on. Keeping generation off
    // this path matters twice over: it is seconds of work per epoch, and it used to run from
    // whichever thread happened to advance the tip, freeing the buffer under any miner mid-hash.
}
