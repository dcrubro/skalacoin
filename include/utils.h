#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

typedef struct {
    uint8_t bytes[32];
} key32_t;

static inline uint32_t hash_key32(key32_t k) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < 32; i++) {
        hash ^= k.bytes[i];
        hash *= 16777619;
    }
    return hash;
}

static inline int eq_key32(key32_t a, key32_t b) {
    return memcmp(a.bytes, b.bytes, 32) == 0;
}

static inline void AddressToHexString(const uint8_t address[32], char out[65]) {
    if (!address || !out) {
        return;
    }
    to_hex(address, out);
}

#endif
