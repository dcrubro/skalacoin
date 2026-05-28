#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <crypto/crypto.h>
#include <uint256.h>
#include <time.h>

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

static inline uint64_t get_current_time_ms(void) {
    struct timespec spec;
    if (clock_gettime(CLOCK_REALTIME, &spec) == -1) {
        return 0; // Handle error
    }
    // Convert seconds to milliseconds and add nanoseconds converted to milliseconds
    return (spec.tv_sec * 1000) + (spec.tv_nsec / 1000000);
}

static inline void sleep_for_microseconds(uint64_t microseconds) {
    struct timespec req;
    req.tv_sec = (time_t)(microseconds / 1000000ULL);
    req.tv_nsec = (long)((microseconds % 1000000ULL) * 1000ULL);
    while (nanosleep(&req, &req) == -1) {
        continue;
    }
}

static inline void sleep_for_milliseconds(uint64_t milliseconds) {
    sleep_for_microseconds(milliseconds * 1000ULL);
}

static inline void AddressToHexString(const uint8_t address[32], char out[65]) {
    if (!address || !out) {
        return;
    }
    to_hex(address, out);
}

static inline void AddressFromCompressedPubkey(const uint8_t compressedPubkey[33], uint8_t outAddress[32]) {
    if (!compressedPubkey || !outAddress) {
        return;
    }

    SHA256(compressedPubkey, 33, outAddress);
}

static inline void PrintHexBytes(const uint8_t* bytes, size_t length) {
    if (!bytes) {
        return;
    }

    for (size_t i = 0; i < length; ++i) {
        printf("%02x", bytes[i]);
    }
}

static inline int CompareHashToTarget(const uint8_t hash[32], const uint8_t target[32]) {
    if (!hash || !target) {
        return 1;
    }

    for (size_t i = 0; i < 32; ++i) {
        if (hash[i] < target[i]) {
            return -1;
        }
        if (hash[i] > target[i]) {
            return 1;
        }
    }

    return 0;
}

static inline bool DecodeCompactTarget(uint32_t nBits, uint8_t target[32]) {
    if (!target) {
        return false;
    }

    memset(target, 0, 32);

    uint32_t exponent = nBits >> 24;
    uint32_t mantissa = nBits & 0x007fffff;
    bool negative = (nBits & 0x00800000) != 0;

    if (negative || mantissa == 0) {
        return false;
    }

    if (exponent <= 3) {
        mantissa >>= 8 * (3 - exponent);
        target[29] = (uint8_t)((mantissa >> 16) & 0xffu);
        target[30] = (uint8_t)((mantissa >> 8) & 0xffu);
        target[31] = (uint8_t)(mantissa & 0xffu);
    } else {
        uint32_t byteIndex = exponent - 3;
        if (byteIndex > 29) {
            return false;
        }

        target[32 - byteIndex - 3] = (uint8_t)((mantissa >> 16) & 0xffu);
        target[32 - byteIndex - 2] = (uint8_t)((mantissa >> 8) & 0xffu);
        target[32 - byteIndex - 1] = (uint8_t)(mantissa & 0xffu);
    }

    return true;
}

static inline bool GenerateTestMinerIdentity(uint8_t privateKey[32], uint8_t compressedPubkey[33], uint8_t address[32]) {
    if (!privateKey || !compressedPubkey || !address) {
        return false;
    }

    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    if (!ctx) {
        return false;
    }

    uint8_t seed[64];
    secp256k1_pubkey pubkey;

    for (uint64_t counter = 0; counter < 1024; ++counter) {
        const char* base = "skalacoin-test-miner-key";
        size_t baseLen = strlen(base);
        memcpy(seed, base, baseLen);
        memcpy(seed + baseLen, &counter, sizeof(counter));
        SHA256(seed, baseLen + sizeof(counter), privateKey);

        if (!secp256k1_ec_seckey_verify(ctx, privateKey)) {
            continue;
        }

        if (!secp256k1_ec_pubkey_create(ctx, &pubkey, privateKey)) {
            continue;
        }

        size_t pubLen = 33;
        if (!secp256k1_ec_pubkey_serialize(ctx, compressedPubkey, &pubLen, &pubkey, SECP256K1_EC_COMPRESSED) || pubLen != 33) {
            continue;
        }

        AddressFromCompressedPubkey(compressedPubkey, address);
        secp256k1_context_destroy(ctx);
        return true;
    }

    secp256k1_context_destroy(ctx);
    return false;
}

static inline bool GenerateRandomTestAddress(uint8_t outAddress[32], uint8_t outPrivateKey[32], uint8_t outCompressedPubkey[33]) {
    if (!outAddress) {
        return false;
    }

    uint8_t privateKey[32];
    uint8_t compressedPubkey[33];
    secp256k1_pubkey pubkey;

    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    if (!ctx) {
        return false;
    }

    for (size_t attempt = 0; attempt < 4096; ++attempt) {
        for (size_t i = 0; i < sizeof(privateKey); ++i) {
            privateKey[i] = (uint8_t)(rand() & 0xFF);
        }

        if (!secp256k1_ec_seckey_verify(ctx, privateKey)) {
            continue;
        }

        if (!secp256k1_ec_pubkey_create(ctx, &pubkey, privateKey)) {
            continue;
        }

        size_t pubLen = sizeof(compressedPubkey);
        if (!secp256k1_ec_pubkey_serialize(ctx, compressedPubkey, &pubLen, &pubkey, SECP256K1_EC_COMPRESSED) || pubLen != 33) {
            continue;
        }

        AddressFromCompressedPubkey(compressedPubkey, outAddress);
        if (outPrivateKey) {
            memcpy(outPrivateKey, privateKey, 32);
        }
        if (outCompressedPubkey) {
            memcpy(outCompressedPubkey, compressedPubkey, 33);
        }
        secp256k1_context_destroy(ctx);
        return true;
    }

    secp256k1_context_destroy(ctx);

    return false;
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
