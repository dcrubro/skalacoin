#ifndef BLOCK_H
#define BLOCK_H

#include <stdint.h>
#include <openssl/sha.h>
#include <dynarr.h>
#include <block/transaction.h>
#include <stdbool.h>
#include <string.h>
#include <randomx/librx_wrapper.h>

typedef struct {
    uint8_t version;
    uint32_t blockNumber;
    uint8_t prevHash[32];
    uint8_t merkleRoot[32];
    uint64_t timestamp;
    uint32_t difficultyTarget; // Encoding: [1 byte exponent][3 byte coefficient]; Target = coefficient * 256^(exponent-3)
    uint64_t nonce; // Higher nonce for RandomX
} block_header_t;

typedef struct {
    block_header_t header;
    DynArr* transactions; // Array of signed_transaction_t
} block_t;

block_t* Block_Create();
void Block_CalculateHash(const block_t* block, uint8_t* outHash);
void Block_CalculateRandomXHash(const block_t* block, uint8_t* outHash);
void Block_AddTransaction(block_t* block, signed_transaction_t* tx);
void Block_RemoveTransaction(block_t* block, uint8_t* txHash);
bool Block_HasValidProofOfWork(const block_t* block);
bool Block_AllTransactionsValid(const block_t* block);
void Block_Destroy(block_t* block);

#endif
