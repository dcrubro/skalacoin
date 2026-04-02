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

static inline bool uint256_subtract_u64(uint256_t* balance, uint64_t amount) {
    if (!balance) {
        return false;
    }

    if (balance->limbs[0] >= amount) {
        balance->limbs[0] -= amount;
        return false;
    }

    uint64_t borrow = amount - balance->limbs[0];
    balance->limbs[0] = UINT64_MAX - borrow + 1ULL;

    for (int i = 1; i < 4; ++i) {
        if (balance->limbs[i] > 0) {
            balance->limbs[i]--;
            return false;
        }

        balance->limbs[i] = UINT64_MAX;
    }

    return true; // underflow past 256 bits
}

static inline bool uint256_subtract(uint256_t* a, const uint256_t* b) {
    // Check if a < b to prevent underflow
    for (int i = 3; i >= 0; i--) {
        if (a->limbs[i] > b->limbs[i]) break;
        if (a->limbs[i] < b->limbs[i]) return false; // Underflow
    }

    uint64_t borrow = 0;
    for (int i = 0; i < 4; i++) {
        uint64_t old_a = a->limbs[i];
        a->limbs[i] -= b->limbs[i] + borrow;

        // Detect borrow: if we subtracted more than we had, or we were at zero and had a borrow
        if (borrow) {
            borrow = (a->limbs[i] >= old_a);
        } else {
            borrow = (a->limbs[i] > old_a);
        }
    }
    return true;
}

/**
 * Compares two uint256_t values in a greater-than manner.
 * Returns [-1, 0, 1] if a > b, a < b, or a == b respectively.
**/
static inline int uint256_cmp(const uint256_t* a, const uint256_t* b) {
    for (int i = 3; i >= 0; i--) {
        if (a->limbs[i] > b->limbs[i]) return 1;
        if (a->limbs[i] < b->limbs[i]) return -1;
    }
    return 0;
}

static inline void uint256_serialize(const uint256_t* value, char* out) {
    if (!value || !out) {
        return;
    }

    // Convert into string of decimal digits for easier readability; max 78 digits for 256 bits
    char digits[80];
    size_t digitCount = 0;
    uint256_t tmp = *value;
    while (tmp.limbs[0] != 0 || tmp.limbs[1] != 0 || tmp.limbs[2] != 0 || tmp.limbs[3] != 0) {
        uint64_t remainder = 0;
        for (int i = 3; i >= 0; --i) {
            __uint128_t cur = ((__uint128_t)remainder << 64) | tmp.limbs[i];
            tmp.limbs[i] = (uint64_t)(cur / 10u);
            remainder = (uint64_t)(cur % 10u);
        }

        if (digitCount < sizeof(digits) - 1) {
            digits[digitCount++] = (char)('0' + remainder);
        } else {
            break;
        }
    }
    digits[digitCount] = '\0';

    for (size_t i = 0; i < digitCount; ++i) {
        out[i] = digits[digitCount - 1 - i];
    }
    out[digitCount] = '\0';
}

#endif
