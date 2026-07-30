#include <nets/fetch_scheduler.h>
#include <constants.h>

// Integer-only on purpose. This penalty gates fork choice (see Chain_ReplaceBranch), so every node
// must compute the exact same number of blocks from the same reorg depth. The previous
// implementation used double/pow/ceil, which is not reproducible across platforms and compilers.
uint64_t FetchScheduler_ComputeReorgPenaltyBlocks(uint64_t delayBlocks) {
    if (delayBlocks <= REORG_PENALTY_GRACE_BLOCKS) {
        return 0ULL;
    }

    uint64_t depth = delayBlocks;
    if (depth > REORG_PENALTY_MAX_DEPTH) {
        depth = REORG_PENALTY_MAX_DEPTH;
    }

    // depth^EXPONENT, saturating rather than wrapping.
    uint64_t raised = 1ULL;
    for (uint32_t i = 0; i < REORG_PENALTY_EXPONENT; ++i) {
        if (depth != 0ULL && raised > UINT64_MAX / depth) {
            return UINT64_MAX;
        }
        raised *= depth;
    }

    // Scale by theta and by the block-time ratio, as one fraction so there is a single rounding
    // step: penalty = ceil(raised * FACTOR_NUM * REF_BLOCK_TIME / (FACTOR_DEN * TARGET_BLOCK_TIME))
    //
    // REF_BLOCK_TIME is the NUMERATOR and TARGET_BLOCK_TIME the DENOMINATOR, not the other way
    // round. The result is a count of BLOCKS, so the wall-clock protection it buys is
    // penalty(d) * TARGET_BLOCK_TIME ~= d^p * REF_BLOCK_TIME -- TARGET_BLOCK_TIME cancels, and the
    // protection is the same number of seconds whatever the block time is. Inverting these two
    // makes wall-clock protection scale as TARGET_BLOCK_TIME^2, so shortening the block time
    // silently weakens reorg protection. Do not "simplify" this back.
    const uint64_t numeratorScale = REORG_PENALTY_FACTOR_NUM * REORG_PENALTY_REF_BLOCK_TIME;
    const uint64_t denominator = REORG_PENALTY_FACTOR_DEN * (uint64_t)TARGET_BLOCK_TIME;
    if (denominator == 0ULL) {
        return 0ULL;
    }

    if (numeratorScale != 0ULL && raised > UINT64_MAX / numeratorScale) {
        return UINT64_MAX;
    }
    const uint64_t numerator = raised * numeratorScale;

    // Ceiling division without overflowing on the +denominator-1 term.
    uint64_t penalty = numerator / denominator;
    if (numerator % denominator != 0ULL) {
        penalty++;
    }

    return penalty;
}
