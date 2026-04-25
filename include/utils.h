#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <uint256.h>

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

static inline bool ParseHexAddress32(const char* in, uint8_t outAddress[32]) {
    if (!in || !outAddress) {
        return false;
    }

    const char* p = in;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }

    if (strlen(p) != 64) {
        return false;
    }

    for (size_t i = 0; i < 32; ++i) {
        char hi = p[i * 2];
        char lo = p[i * 2 + 1];
        int hiVal = (hi >= '0' && hi <= '9') ? (hi - '0') :
                    (hi >= 'a' && hi <= 'f') ? (10 + hi - 'a') :
                    (hi >= 'A' && hi <= 'F') ? (10 + hi - 'A') : -1;
        int loVal = (lo >= '0' && lo <= '9') ? (lo - '0') :
                    (lo >= 'a' && lo <= 'f') ? (10 + lo - 'a') :
                    (lo >= 'A' && lo <= 'F') ? (10 + lo - 'A') : -1;

        if (hiVal < 0 || loVal < 0) {
            return false;
        }

        outAddress[i] = (uint8_t)((hiVal << 4) | loVal);
    }

    return true;
}

static inline bool IsValidIPv4(const char* ip) {
    if (!ip || *ip == '\0') {
        return false;
    }

    int octetCount = 0;
    const char* p = ip;

    while (*p != '\0') {
        if (octetCount >= 4) {
            return false;
        }

        if (*p < '0' || *p > '9') {
            return false;
        }

        unsigned int value = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            value = (value * 10u) + (unsigned int)(*p - '0');
            if (value > 255u) {
                return false;
            }
            ++digits;
            if (digits > 3) {
                return false;
            }
            ++p;
        }

        if (digits == 0) {
            return false;
        }

        ++octetCount;
        if (octetCount < 4) {
            if (*p != '.') {
                return false;
            }
            ++p;
            if (*p == '\0') {
                return false;
            }
        }
    }

    return octetCount == 4;
}

static inline void Uint256ToDecimal(const uint256_t* value, char* out, size_t outSize) {
    if (!value || !out || outSize == 0) {
        return;
    }

    uint64_t tmp[4] = {
        value->limbs[0],
        value->limbs[1],
        value->limbs[2],
        value->limbs[3]
    };

    if (tmp[0] == 0 && tmp[1] == 0 && tmp[2] == 0 && tmp[3] == 0) {
        if (outSize >= 2) {
            out[0] = '0';
            out[1] = '\0';
        } else {
            out[0] = '\0';
        }
        return;
    }

    char digits[80];
    size_t digitCount = 0;

    while (tmp[0] != 0 || tmp[1] != 0 || tmp[2] != 0 || tmp[3] != 0) {
        uint64_t remainder = 0;
        for (int i = 3; i >= 0; --i) {
            __uint128_t cur = ((__uint128_t)remainder << 64) | tmp[i];
            tmp[i] = (uint64_t)(cur / 10u);
            remainder = (uint64_t)(cur % 10u);
        }

        if (digitCount < sizeof(digits) - 1) {
            digits[digitCount++] = (char)('0' + remainder);
        } else {
            break;
        }
    }

    size_t writeLen = (digitCount < (outSize - 1)) ? digitCount : (outSize - 1);
    for (size_t i = 0; i < writeLen; ++i) {
        out[i] = digits[digitCount - 1 - i];
    }
    out[writeLen] = '\0';
}

#endif
