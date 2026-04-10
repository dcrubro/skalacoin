#ifndef BLOCK_H
#define BLOCK_H

#include <stdint.h>
#include <openssl/sha.h>
#include <dynarr.h>
#include <block/transaction.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#pragma pack(push, 1) // Ensure no padding for consistent file storage
typedef struct {
    uint64_t blockNumber;
    uint64_t timestamp;
    uint64_t nonce;
    uint8_t prevHash[32];
    uint8_t merkleRoot[32];
    uint32_t difficultyTarget; // Encoding: [1 byte exponent][3 byte coefficient]; Target = coefficient * 256^(exponent-3)
    uint8_t version;
    uint8_t reserved[3];       // 3 bytes (Explicit padding for 8-byte alignment)
} block_header_t;
#pragma pack(pop)

typedef struct {
    block_header_t header;
    DynArr* transactions; // Array of signed_transaction_t, NOTE: Potentially move to a hashmap at some point for quick lookups.
} block_t;

block_t* Block_Create();
void Block_CalculateHash(const block_t* block, uint8_t* outHash);
void Block_CalculateMerkleRoot(const block_t* block, uint8_t* outHash);
void Block_CalculateAutolykos2Hash(const block_t* block, uint8_t* outHash);
bool Block_RebuildAutolykos2Dag(size_t dagBytes, const uint8_t seed32[32]);
void Block_AddTransaction(block_t* block, signed_transaction_t* tx);
void Block_RemoveTransaction(block_t* block, uint8_t* txHash);
bool Block_HasValidProofOfWork(const block_t* block);
bool Block_AllTransactionsValid(const block_t* block);
void Block_ShutdownPowContext(void);
void Block_Destroy(block_t* block);
void Block_Print(const block_t* block);

#endif
