#ifndef UINT256_H
#define UINT256_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

typedef struct {
    uint64_t limbs[4]; // 4 * 64 = 256 bits
} uint256_t;

// Initialize a uint256 with a standard 64-bit value
static inline uint256_t uint256_from_u64(uint64_t val) {
    uint256_t res = {{val, 0, 0, 0}};
    return res;
}

/**
 * Adds a uint64_t (transaction amount) to a uint256_t (balance).
 * Returns true if an overflow occurred (total supply exceeded 256 bits).
**/
static inline bool uint256_add_u64(uint256_t* balance, uint64_t amount) {
    uint64_t old = balance->limbs[0];
    balance->limbs[0] += amount;

    // Check for carry: if the new value is less than the old, it wrapped around
    if (balance->limbs[0] < old) {
        for (int i = 1; i < 4; i++) {
            balance->limbs[i]++;
            // If the limb didn't wrap to 0, the carry is fully absorbed
            if (balance->limbs[i] != 0) return false;
        }
        return true; // Overflowed all 256 bits
    }
    return false;
}

/**
 * Adds two uint256_t values together.
 * Standard full addition logic.
**/
static inline bool uint256_add(uint256_t* a, const uint256_t* b) {
    uint64_t carry = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t old_a = a->limbs[i];
        a->limbs[i] += b->limbs[i] + carry;
        
        // Detect carry: current is less than what we added, or we were at max and had a carry
        if (carry) {
            carry = (a->limbs[i] <= old_a);
        } else {
            carry = (a->limbs[i] < old_a);
        }
    }
    return carry > 0;
}

#endif