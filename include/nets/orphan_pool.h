#ifndef ORPHAN_POOL_H
#define ORPHAN_POOL_H

#include <stdint.h>
#include <stdbool.h>
#include <block/block.h>
#include <block/chain.h>

// Initialize/destroy the global orphan pool
void OrphanPool_Init(void);
void OrphanPool_Destroy(void);

// Insert an orphan block into the pool. Ownership of `block` is transferred to the pool.
// `height` is the block number from the header. `observedAtTipHeight` is the local chain tip
// height at the moment the block arrived; it is stamped once and drives the Horizen reorg
// penalty, so it must never be re-derived from a later tip.
// Duplicates (same block hash) are rejected and the block is destroyed.
void OrphanPool_Insert(block_t* block, uint64_t height, uint64_t observedAtTipHeight);

// Attempt to attach any orphans whose parents now exist in `chain`, and to adopt a competing
// branch when one is heavier and has served its reorg penalty.
// Returns the number of blocks successfully attached.
size_t OrphanPool_AttemptAttach(blockchain_t* chain);

/**
 * As OrphanPool_AttemptAttach, but skips the reorg delay penalty when `bypassPenalty` is set.
 *
 * Reserved for an explicit operator action (`sync force`). The penalty is served by local chain
 * growth, so a node that is neither mining nor stale enough to count as catching up can never
 * clear it by itself; this is the manual way out for an operator who knows their branch is the
 * wrong one. Work comparison and linkage still apply, so it cannot adopt a lighter branch, and
 * nothing a peer sends can reach it.
**/
size_t OrphanPool_AttemptAttachForced(blockchain_t* chain, bool bypassPenalty);

// True if a block with this hash is already pooled.
bool OrphanPool_Contains(const uint8_t blockHash[32]);

// Number of pooled orphans (diagnostics).
size_t OrphanPool_Size(void);

#endif
