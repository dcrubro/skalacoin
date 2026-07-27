#ifndef CHAIN_H
#define CHAIN_H

#include <block/block.h>
#include <dynarr.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <uint256.h>
#include <storage/block_table.h>
#include <balance_sheet.h>

typedef struct {
    DynArr* blocks;
    size_t size;
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

#endif
