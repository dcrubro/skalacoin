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

#define INFLATION_PERCENTAGE_PER_EPOCH 15 // 1.5%

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

static uint64_t currentReward = 0; // Global variable to track current block reward; updated with each block mined
static const uint64_t M_CAP = 18446744073709551615ULL; // Max uint64
static const uint64_t TAIL_EMISSION = DECIMALS; // Emission floor is 1.0 coins per block
// No max supply. Instead of halving, it'll follow a more gradual, Monero-like emission curve.

static uint256_t currentSupply = {{0, 0, 0, 0}}; // Global variable to track total supply; updated with each block mined

// Call every epoch
static inline uint64_t GetInflationRateReward(uint256_t currentSupply, blockchain_t* chain) {
    if (!chain || !chain->blocks) { return 0x00; } // Invalid
    size_t height = Chain_Size(chain);
    
    block_t* blk = (block_t*)Chain_GetBlock(chain, height - 1); // Last block
    if (!blk) { return 0x00; } // Invalid
    
    if (height % EPOCH_LENGTH == 0) {
        // Calculate the new block reward (using all integer math to avoid floating point issues)

        // 1. Multiply supply by 3
        uint256_t multiplied = currentSupply;
        uint256_t temp = currentSupply;
        uint256_add(&multiplied, &temp); // currentSupply * 2
        uint256_add(&multiplied, &temp); // currentSupply * 3

        // 2. Divide by 70,000,000 using scalar short division
        uint64_t divisor = 70000000ULL;
        uint256_t quotient = {{0, 0, 0, 0}};
        unsigned __int128 remainder = 0;

        // Work from the most significant limb to the least
        for (int i = 3; i >= 0; i--) {
            unsigned __int128 current = (remainder << 64) | multiplied.limbs[i];
            quotient.limbs[i] = (uint64_t)(current / divisor);
            remainder = current % divisor;
        }

        currentReward = quotient.limbs[0]; // Update the global reward variable with the new calculated reward for this epoch
        return quotient.limbs[0]; // Return the least significant limb as the reward (the rest should be 0 for reasonable supply levels)
    }

    return currentReward;
}

static inline uint64_t CalculateBlockReward(uint256_t currentSupply, blockchain_t* chain) {
    if (!chain || !chain->blocks) { return 0x00; } // Invalid

    uint64_t height = Chain_Size(chain);

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
        if (height < EPOCH_LENGTH * 10) { // Transitionary period to inflation of 1.5% per epoch
            return TAIL_EMISSION;
        } else {
            return GetInflationRateReward(currentSupply, chain); // After the transitionary period, switch to the inflation-based reward
        }
    }

    return reward;
}

#endif
