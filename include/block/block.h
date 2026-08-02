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
    // reserved[0] carries the miner's DAG-size vote (DAG_VOTE_* in constants.h); reserved[1..2] must
    // be zero. All three are inside the hashed header, so a vote is committed to by both the
    // canonical hash and the PoW hash and cannot be altered after the block is mined.
    uint8_t reserved[3];
} block_header_t;
#pragma pack(pop)

typedef struct {
    block_header_t header;
    DynArr* transactions; // Array of signed_transaction_t, NOTE: Potentially move to a hashmap at some point for quick lookups.
} block_t;

// PoW validity is chain-relative: it needs the epoch DAG size and seed. chain.h includes this
// header, so the tag declared there is forward-declared here to break the cycle.
typedef struct blockchain blockchain_t;

block_t* Block_Create();
void Block_CalculateHash(const block_t* block, uint8_t* outHash);
void Block_CalculateMerkleRoot(const block_t* block, uint8_t* outHash);
void Block_AddTransaction(block_t* block, signed_transaction_t* tx);
void Block_RemoveTransaction(block_t* block, uint8_t* txHash);

/**
 * Autolykos2 PoW hashing.
 *
 * The heavy variant reads its lanes from the process-global DAG and is a MINING accelerator only;
 * the light variant derives the same lanes from the epoch seed on demand. They are bit-for-bit
 * equivalent by construction -- Autolykos2_DagGenerate fills lane i with exactly what
 * ReadDagLaneFromSeed recomputes for lane i -- so a block mined through either verifies through
 * either. Validation always uses the light path: it needs no allocation, which is what keeps the
 * DAG a miner requirement rather than a full-node memory requirement, and it stays correct for
 * blocks from earlier epochs (the heavy path can only ever answer for whichever epoch the global
 * DAG was last built for).
**/
bool Block_EnsureAutolykos2Dag(uint64_t epochIndex, size_t dagBytes, const uint8_t seed32[32]);
// Fails rather than answering from a DAG built for a different epoch, size OR SEED, so it can
// never silently hash against the wrong lanes. The seed matters because a reorg changes it while
// leaving the epoch index and size unchanged.
bool Block_PowHashHeavy(const block_t* block, uint64_t epochIndex, size_t dagBytes,
                        const uint8_t seed32[32], uint8_t outHash[32]);
bool Block_PowHashLight(const block_t* block, size_t dagBytes, const uint8_t seed32[32], uint8_t outHash[32]);

// PoW check against explicitly supplied epoch parameters, for callers that resolve them once and
// then iterate (the miner). Returns false if the hash cannot be computed -- never treat an
// uncomputable proof as valid.
bool Block_HasValidProofOfWorkWithParams(const block_t* block, uint64_t epochIndex,
                                         size_t dagBytes, const uint8_t seed32[32]);

// PoW check that resolves the epoch parameters for the block's own height from `chain`.
bool Block_HasValidProofOfWork(const block_t* block, blockchain_t* chain);

// Header vote field is a recognised value and the unused reserved bytes are zero.
bool Block_HasValidVote(const block_t* block);

bool Block_AllTransactionsValid(const block_t* block);
bool Block_ValidateCoinbaseAndFees(const block_t* block, uint64_t expectedCoinbaseAmount, uint64_t* outTotalFees);

/**
 * Self-contained validity: merkle root, transactions, vote encoding, non-empty. Needs no chain, so
 * it is meaningful for ANY block, including one on a branch we do not have.
 *
 * This is what the receive path checks. Proof of work is deliberately NOT checked there, because
 * PoW is only meaningful relative to the branch a block belongs to: the epoch seed is the last
 * block of the previous epoch on ITS OWN branch. Validating a competing branch's block against our
 * epoch seed does not merely fail to resolve -- when the two chains diverge before the boundary it
 * resolves to the WRONG seed and rejects a perfectly valid block, which made any fork spanning an
 * epoch boundary impossible to assemble.
 *
 * Chain_AddBlock verifies proof of work at the moment a block joins the chain, where the branch
 * context is real. That, not the receive path, is what enforces the invariant.
**/
bool Block_HasValidStructure(const block_t* block);

// Full check including chain-relative PoW. Only meaningful for a block that extends `chain`.
bool Block_IsFullyValid(const block_t* block, blockchain_t* chain);
void Block_ShutdownPowContext(void);
void Block_Destroy(block_t* block);
void Block_Print(const block_t* block);
void Block_ShortPrint(const block_t* block);
// Deep-copy a block (allocates a new `block_t*`). Caller must call `Block_Destroy`.
block_t* Block_Copy(const block_t* src);

#endif
