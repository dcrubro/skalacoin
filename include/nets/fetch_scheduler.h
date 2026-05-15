#ifndef FETCH_SCHEDULER_H
#define FETCH_SCHEDULER_H

#include <stdint.h>

// Compute penalty in blocks for a delayed/heavy reorg reported by a peer.
// Returns the number of penalty blocks to subtract from the peer's advertised work.
uint64_t FetchScheduler_ComputeReorgPenaltyBlocks(uint64_t delayBlocks);

#endif
