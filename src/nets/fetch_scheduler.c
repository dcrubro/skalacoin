#include <nets/fetch_scheduler.h>
#include <constants.h>
#include <math.h>

// Note: floating point is used intentionally here for readability and
// because the final penalty is rounded to whole blocks. This keeps the
// implementation straightforward while avoiding subtle integer overflow
// for large exponents. If desired, replace with fixed-point arithmetic.
uint64_t FetchScheduler_ComputeReorgPenaltyBlocks(uint64_t delayBlocks) {
    if (delayBlocks <= REORG_PENALTY_GRACE_BLOCKS) {
        return 0ULL;
    }

    double B = (double)delayBlocks;
    double factor = REORG_PENALTY_FACTOR;
    double exp = REORG_PENALTY_EXPONENT;
    double timeScale = ((double)TARGET_BLOCK_TIME) / REORG_PENALTY_REF_BLOCK_TIME;

    double raw = factor * pow(B, exp) * timeScale;
    if (raw < 0.0) raw = 0.0;

    uint64_t penalty = (uint64_t)ceil(raw);
    return penalty;
}
