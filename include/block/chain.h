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

// I/O
bool Chain_SaveToFile(blockchain_t* chain, const char* dirpath, uint256_t currentSupply, uint64_t currentReward);
bool Chain_LoadFromFile(blockchain_t* chain, const char* dirpath, uint256_t* outCurrentSupply, uint32_t* outDifficultyTarget, uint64_t* outCurrentReward);

// Difficulty
uint32_t Chain_ComputeNextTarget(blockchain_t* chain, uint32_t currentTarget);

#endif
