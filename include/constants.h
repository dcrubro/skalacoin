#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <stdint.h>
#include <uint256.h>
#include <stdbool.h>
#include <block/chain.h>
#include <block/block.h>

#define DECIMALS 1000000000000ULL
#define DIFFICULTY_ADJUSTMENT_INTERVAL 960 // Every 960 blocks (roughly every 24 hours with a 90 second block time)
                                           // Max adjustment per is x2. So if blocks are coming in too fast, the difficulty will at most double every 24 hours, and vice versa if they're coming in too slow.
#define TARGET_BLOCK_TIME 90 // Target block time in seconds
#define INITIAL_DIFFICULTY 0x1f0c1422 // Default compact target used by Autolykos2 PoW (This is ridiculously low)
//#define INITIAL_DIFFICULTY 0x1d1b7c51 // This takes 90s on my machine with a single thread, good for testing

// Reward schedule acceleration: 1 means normal-speed progression.
#define EMISSION_ACCELERATION_FACTOR 1ULL

// Phase-one target horizon: emit ~2^64-1 atomic units by this many blocks at x1.
#define PHASE1_TARGET_BLOCKS 3000000ULL

// Inflation is expressed in tenths of a percent to preserve integer math.
#define INFLATION_PERCENTAGE_PER_EPOCH_TENTHS 15ULL // 1.5%

// Monero-style main emission: reward = (MONEY_SUPPLY - generated) >> speed factor.
// Keep this at 20 to match the canonical curve shape against a 2^64 atomic supply cap.
#define MONERO_EMISSION_SPEED_FACTOR 20U

// Future Autolykos2 constants:
#define EPOCH_LENGTH 350000 // ~1 year at 90s
#define BASE_DAG_SIZE (2ULL << 30) // 2 GB
#define DAG_BASE_GROWTH (1ULL << 30) // 1 GB, calculated fully later
#define DAG_BASE_CAP (8ULL << 30) // 8 GB, adjusted per cycle based off DAG_BASE_GROWTH
// Swings - calculated as MIN(percentage, absolute GB) to prevent absurd swings from low hashrate or very large DAG growth
#define DAG_MAX_UP_SWING_PERCENTAGE 1.3 // 30%
#define DAG_MAX_DOWN_SWING_PERCENTAGE 0.85 // 15%
#define DAG_MAX_UP_SWING_GB (4ULL << 30) // 4 GB
#define DAG_MAX_DOWN_SWING_GB (1ULL << 30) // 1 GB
#define KICKOUT_TARGET_PERCENTAGE 75
#define KICKOUT_TARGET_BLOCK 30000 // 1 month at 90s block time

/**
 * Each epoch has 2 phases, connected logarithmically:
 * - Phase 1: Aggressive DAG growth (target is ~75% of the max cap) to kick out any ASICs, 30k blocks (roughly 1 month)
 * - Phase 2: Stable DAG growth (target is the max cap) to provide a stable environment for GPU miners, 320k blocks (roughly 11 months)
**/

static const uint64_t M_CAP = 18446744073709551615ULL; // Max uint64
static const uint64_t TAIL_EMISSION = 750000000000ULL; // 0.75 coins per block floor
static uint64_t currentReward = 750000000000ULL; // Epoch reward cache for phase 3
// No max supply. Instead of halving, it'll follow a more gradual, Monero-like emission curve.

static uint256_t currentSupply = {{0, 0, 0, 0}}; // Global variable to track total supply; updated with each block mined

// Phase 3: update once per effective epoch and keep a fixed per-block reward for that epoch.
static inline uint64_t GetInflationRateReward(uint256_t currentSupply, blockchain_t* chain) {
    if (!chain || !chain->blocks) { return 0x00; } // Invalid
    size_t height = Chain_Size(chain);
    const uint64_t effectiveEpochLength =
        (EPOCH_LENGTH / EMISSION_ACCELERATION_FACTOR) > 0
            ? (EPOCH_LENGTH / EMISSION_ACCELERATION_FACTOR)
            : 1;

    if (height == 0) {
        currentReward = TAIL_EMISSION;
        return currentReward;
    }
    
    if (height % effectiveEpochLength == 0) {
        // inflationPerBlock = currentSupply * 1.5% / effectiveEpochLength
        // = currentSupply * 15 / (1000 * effectiveEpochLength)
        uint256_t multiplied = uint256_from_u64(0);
        for (uint64_t i = 0; i < INFLATION_PERCENTAGE_PER_EPOCH_TENTHS; ++i) {
            uint256_add(&multiplied, &currentSupply);
        }

        uint64_t divisor = 1000ULL * effectiveEpochLength;
        uint256_t quotient = {{0, 0, 0, 0}};
        unsigned __int128 remainder = 0;

        // Work from the most significant limb to the least
        for (int i = 3; i >= 0; i--) {
            unsigned __int128 current = (remainder << 64) | multiplied.limbs[i];
            quotient.limbs[i] = (uint64_t)(current / divisor);
            remainder = current % divisor;
        }

        uint64_t inflationPerBlock = quotient.limbs[0];
        currentReward = (inflationPerBlock > TAIL_EMISSION) ? inflationPerBlock : TAIL_EMISSION;
        return currentReward;
    }

    return (currentReward > TAIL_EMISSION) ? currentReward : TAIL_EMISSION;
}

static inline uint64_t CalculateBlockReward(uint256_t currentSupply, blockchain_t* chain) {
    if (!chain || !chain->blocks) { return 0x00; } // Invalid

    const uint64_t effectivePhase1Blocks =
        (PHASE1_TARGET_BLOCKS / EMISSION_ACCELERATION_FACTOR) > 0
            ? (PHASE1_TARGET_BLOCKS / EMISSION_ACCELERATION_FACTOR)
            : 1;
    const uint64_t height = (uint64_t)Chain_Size(chain);

    // After the phase-one target horizon, only floor/inflation schedule applies.
    if (height >= effectivePhase1Blocks) {
        return GetInflationRateReward(currentSupply, chain);
    }

    if (currentSupply.limbs[1] > 0 || 
        currentSupply.limbs[2] > 0 || 
        currentSupply.limbs[3] > 0 || 
        currentSupply.limbs[0] >= M_CAP)
    {
        // Post-Monero phase with unlimited supply: floor/inflation schedule only.
        return GetInflationRateReward(currentSupply, chain);
    }

    const uint64_t generated = currentSupply.limbs[0];
    const uint64_t remaining = M_CAP - generated;

    // Monero-style base curve against ~2^64 atomic-unit terminal supply.
    uint64_t reward = remaining >> MONERO_EMISSION_SPEED_FACTOR;

    // Acceleration preserves curve shape while reaching the floor sooner in block-height terms.
    if (EMISSION_ACCELERATION_FACTOR > 1ULL && reward > 0ULL) {
        __uint128_t accelerated = (__uint128_t)reward * (__uint128_t)EMISSION_ACCELERATION_FACTOR;
        reward = (accelerated > (__uint128_t)remaining) ? remaining : (uint64_t)accelerated;
    }

    // Retarget phase one to finish by PHASE1_TARGET_BLOCKS (x1), while keeping
    // Monero-style behavior as the preferred curve when it is already sufficient.
    const uint64_t blocksLeft = effectivePhase1Blocks - height;
    const uint64_t minRewardToFinish = (remaining + blocksLeft - 1ULL) / blocksLeft; // ceil(remaining / blocksLeft)
    if (reward < minRewardToFinish) {
        reward = minRewardToFinish;
    }
    if (reward > remaining) {
        reward = remaining;
    }

    // Phase 1 until Monero reward goes below the floor.
    if (reward > TAIL_EMISSION) {
        return reward;
    }

    // Phase 2 + 3: floor and epoch inflation updates.
    return GetInflationRateReward(currentSupply, chain);
}

#endif
