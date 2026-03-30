#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <stdint.h>
#include <uint256.h>
#include <stdbool.h>

#define DECIMALS 1000000000000ULL
#define DIFFICULTY_ADJUSTMENT_INTERVAL 960 // Every 960 blocks (roughly every 24 hours with a 90 second block time)
                                           // Max adjustment per is x2. So if blocks are coming in too fast, the difficulty will at most double every 24 hours, and vice versa if they're coming in too slow.
#define TARGET_BLOCK_TIME 90 // Target block time in seconds
#define INITIAL_DIFFICULTY 0x1f0c1422 // Default compact target used by Autolykos2 PoW (This is ridiculously low)
//#define INITIAL_DIFFICULTY 0x1d1b7c51 // This takes 90s on my machine with a single thread, good for testing

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
static const uint64_t TAIL_EMISSION = DECIMALS; // Emission floor is 1.0 coins per block
// No max supply. Instead of halving, it'll follow a more gradual, Monero-like emission curve.

static uint256_t currentSupply = {{0, 0, 0, 0}}; // Global variable to track total supply; updated with each block mined

static inline uint64_t CalculateBlockReward(uint256_t currentSupply, uint64_t height) {
    // Inclusive of block 0
    (void)height;

    if (currentSupply.limbs[1] > 0 || 
        currentSupply.limbs[2] > 0 || 
        currentSupply.limbs[3] > 0 || 
        currentSupply.limbs[0] >= M_CAP) {
        return TAIL_EMISSION;
    }

    uint64_t supply_64 = currentSupply.limbs[0];
    
    // Formula: ((M - Supply) >> 20) * 181 / 256
    // Use 128-bit intermediate to avoid overflow while preserving integer math.
    __uint128_t rewardWide = (((__uint128_t)(M_CAP - supply_64) >> 20) * 181u) >> 8;
    uint64_t reward = (rewardWide > UINT64_MAX) ? UINT64_MAX : (uint64_t)rewardWide;
    // At a block time of ~90s and a floor of 1.0 coins, this will make a curve of ~8.5 years

    // Check if the calculated reward has fallen below the floor
    if (reward < TAIL_EMISSION) {
        return TAIL_EMISSION;
    }

    return reward;
}

#endif
