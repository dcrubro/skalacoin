#ifndef CHAIN_H
#define CHAIN_H

#include <block/block.h>
#include <dynarr.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <uint256.h>
#include <storage/block_table.h>
#include <balance_sheet.h>

// One entry of the memoised DAG size recurrence, one per epoch. See Chain_DagParamsForHeight.
typedef struct {
    uint64_t sizeBytes;  // DAG size used by every block whose height falls in this epoch
    bool downQualified;  // this epoch's own votes met the down supermajority
} dag_epoch_state_t;

// Tagged so block.h can forward-declare it: PoW validity depends on the chain (it needs the epoch
// seed), but chain.h includes block.h, so the tag is what breaks the cycle.
typedef struct blockchain {
    DynArr* blocks;
    size_t size;

    /**
     * Memoised DAG size recurrence: a pure cache of a function of the block headers, extended
     * lazily and dropped whenever anything at or below the tip changes (every epoch's size depends
     * on the votes of every epoch before it). It lives on the chain rather than in a global because
     * a second, header-only blockchain_t is built to re-verify historical PoW, and the two must not
     * share a cache.
     *
     * `dagEpochsComputed` counts valid `sizeBytes` entries. `downQualified` is only filled in for
     * an epoch once the *following* entry has been computed, so it is valid on
     * [0, dagEpochsComputed - 1).
     *
     * Guarded by `dagCacheLock`, which is always taken AFTER `chainLock` and is never held across a
     * call back into chain.c.
    **/
    dag_epoch_state_t* dagEpochs;
    size_t dagEpochsComputed;
    size_t dagEpochsCapacity;
    pthread_mutex_t dagCacheLock;
} blockchain_t;

blockchain_t* Chain_Create();
void Chain_Destroy(blockchain_t* chain);
bool Chain_AddBlock(blockchain_t* chain, block_t* block);
block_t* Chain_GetBlock(blockchain_t* chain, size_t index);
size_t Chain_Size(blockchain_t* chain);
bool Chain_IsValid(blockchain_t* chain);
void Chain_Wipe(blockchain_t* chain);

// Roll back the chain to `height` (exclusive): after this call, Chain_Size(chain) == height
// Returns true on success.
bool Chain_RollbackToHeight(blockchain_t* chain, size_t height);

/**
 * Atomically replace the blocks at [forkHeight, tip] with `newBlocks` (ascending, `count` of them).
 *
 * The swap happens only if the candidate branch is properly linked, has strictly more cumulative
 * work, and has served its Horizen delayed-submission penalty. `observedAtTipHeight` is the local
 * tip height at which the branch was FIRST seen and must not be recomputed as the chain grows --
 * see the comment in the implementation. The initial-block-download exemption is decided inside,
 * from local state only, so no caller can switch the penalty off.
 *
 * `bypassPenalty` skips the delay check ONLY. It exists for an explicit operator action (`sync
 * force`) on a node whose chain is known to be the wrong one -- the penalty is served by local
 * chain growth, so a node that is neither mining nor stale enough to count as catching up cannot
 * clear it on its own. It must never be reachable from anything a peer says; work comparison,
 * linkage and atomicity are still enforced, so this cannot adopt a branch that is not heavier.
 *
 * On any failure the original chain, balance sheet, supply and reward are restored and false is
 * returned. The caller keeps ownership of `newBlocks` in every case: the chain applies copies.
**/
bool Chain_ReplaceBranch(blockchain_t* chain,
                         size_t forkHeight,
                         block_t** newBlocks,
                         size_t count,
                         uint64_t observedAtTipHeight,
                         bool bypassPenalty);

// True when this node is catching up rather than following the tip (empty chain, or a median
// block time far in the past). Used to exempt initial sync from the reorg penalty.
bool Chain_IsInitialBlockDownload(blockchain_t* chain);

// Penalty in blocks of local chain growth before a branch forking `reorgDepth` blocks back may be
// adopted. Thin wrapper over FetchScheduler_ComputeReorgPenaltyBlocks, for callers that only
// want to report it.
uint64_t Chain_ReorgPenaltyForDepth(uint64_t reorgDepth);

// Recompute `currentSupply` and `currentReward` from the in-memory chain blocks.
// Returns true on success and updates runtime state globals.
bool Chain_RecomputeRuntimeState(blockchain_t* chain);

// Retrieve a deep copy of the block at `index`. Caller must free with `Block_Destroy`.
bool Chain_GetBlockCopy(blockchain_t* chain, size_t index, block_t** outCopy);

// I/O
bool Chain_SaveToFile(blockchain_t* chain, const char* dirpath, uint256_t currentSupply, uint64_t currentReward);
bool Chain_LoadFromFile(blockchain_t* chain, const char* dirpath, uint256_t* outCurrentSupply, uint32_t* outDifficultyTarget, uint64_t* outCurrentReward, uint8_t* outLastSavedHash, bool loadTransactions);
bool Chain_LoadBlockFromFile(const char* dirpath, uint64_t blockNumber, bool loadTransactions, block_t** outBlock, size_t* outTxCount);

// Difficulty
// Retarget for the block at `height`, measured over the window [height - INTERVAL, height - 1].
// `chain` must hold blocks 0..height-1. Takes no locks; safe to call while holding `chainLock`.
uint32_t Chain_ComputeTargetAtHeight(blockchain_t* chain, uint64_t height, uint32_t currentTarget);

// The consensus-required difficultyTarget for the block at `height`, derived from the chain alone.
// Takes no locks; safe to call while holding `chainLock`.
uint32_t Chain_GetTargetForHeight(blockchain_t* chain, uint64_t height);

// Refresh runtime state derived from the chain tip (difficulty target, epoch DAG).
// Call after any change to the tip. Must NOT be called while holding `chainLock`.
void Chain_OnTipAdvanced(blockchain_t* chain);

// DAG
/**
 * The Autolykos2 DAG size and epoch seed that the block at `blockHeight` must be hashed against.
 *
 * This is the single source of truth for both, so the mining path and the verification path cannot
 * drift apart. Size follows the default-grow recurrence gated by the miner votes in
 * `header.reserved[0]` (see the DAG band in constants.h); the seed is epoch-aligned -- epoch 0 uses
 * the genesis seed, epoch k uses the hash of the last block of epoch k-1 -- so it is constant for
 * the whole epoch rather than changing every block.
 *
 * Requires the chain to hold every block below the start of `blockHeight`'s epoch, which is always
 * true when validating or mining a block at that height. Returns false if it cannot produce both
 * values; callers MUST treat that as an invalid proof rather than falling back to a default.
 *
 * Takes `chainLock` for reading internally. Must NOT be called while holding it.
**/
bool Chain_DagParamsForHeight(blockchain_t* chain, uint64_t blockHeight,
                              size_t* outDagBytes, uint8_t outSeed[32]);

// Work
// Expected number of hashes to satisfy `difficultyTargetBits`, i.e. 2^256 / (target + 1).
bool Chain_ComputeBlockWork(uint32_t difficultyTargetBits, uint256_t* outWork);

// Summed work of the chain's blocks over the half-open range [from, to).
// Takes no locks; safe to call while holding `chainLock`.
bool Chain_ComputeWorkRange(blockchain_t* chain, size_t from, size_t to, uint256_t* outWork);

// Summed work of a candidate branch that is not (yet) part of the chain.
bool Chain_ComputeBranchWork(block_t** blocks, size_t count, uint256_t* outWork);

#endif
