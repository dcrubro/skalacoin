#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <stdint.h>
#include <uint256.h>
#include <stdbool.h>

#define DECIMALS 1000000000000ULL
#define EMISSION_SPEED_FACTOR 20
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
    
    // Formula: (M - Supply) >> 2^k - lifted from Monero's codebase (thanks guys!)
    uint64_t reward = (M_CAP - supply_64) >> EMISSION_SPEED_FACTOR;

    // Check if the calculated reward has fallen below the floor
    if (reward < TAIL_EMISSION) {
        return TAIL_EMISSION;
    }

    return reward;
}

#endif
