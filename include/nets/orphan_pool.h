#ifndef ORPHAN_POOL_H
#define ORPHAN_POOL_H

#include <stdint.h>
#include <block/block.h>
#include <block/chain.h>

// Initialize/destroy the global orphan pool
void OrphanPool_Init(void);
void OrphanPool_Destroy(void);

// Insert an orphan block into the pool. Ownership of `block` is transferred to the pool.
// `height` is the block number from the header.
void OrphanPool_Insert(block_t* block, uint64_t height);

// Attempt to attach any orphans whose parents now exist in `chain`.
// Returns the number of blocks successfully attached.
size_t OrphanPool_AttemptAttach(blockchain_t* chain);

#endif
