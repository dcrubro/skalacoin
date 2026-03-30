#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <stdint.h>
#include <uint256.h>
#include <stdbool.h>

#define DECIMALS 1000000000000ULL
#define DIFFICULTY_ADJUSTMENT_INTERVAL 960 // Every 960 blocks (roughly every 24 hours with a 90 second block time)
                                           // Max adjustment per is x2. So if blocks are coming in too fast, the difficulty will at most double every 24 hours, and vice versa if they're coming in too slow.
#define RANDOMX_KEY_ROTATION_INTERVAL 6720 // 1 week at 90s block time
#define TARGET_BLOCK_TIME 90 // Target block time in seconds
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
