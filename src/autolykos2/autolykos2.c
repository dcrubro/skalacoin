#include <autolykos2/autolykos2.h>

#include "../../include/blake2/blake2.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t* buf;
    size_t cap;
    size_t len;
} dag_buffer_t;

struct Autolykos2Context {
    dag_buffer_t dag;
    void* backend;
};

#ifdef MINICOIN_AUTOLYKOS2_REF_AVAILABLE
extern void* minicoin_autolykos2_ref_create(void);
extern void minicoin_autolykos2_ref_destroy(void* handle);
extern bool minicoin_autolykos2_ref_check_target(
    void* handle,
    const uint8_t message32[32],
    uint64_t nonce,
    uint32_t height,
    const uint8_t target32[32]
);
#endif

static bool Autolykos2_FallbackHash(
    const uint8_t seed32[32],
    const uint8_t* message,
    size_t messageLen,
    uint64_t nonce,
    uint64_t height,
    uint8_t outHash[32]
) {
    if (!seed32 || !outHash) {
        return false;
    }

    uint8_t nonceBytes[8];
    uint8_t heightBytes[8];
    memcpy(nonceBytes, &nonce, sizeof(nonceBytes));
    memcpy(heightBytes, &height, sizeof(heightBytes));

    const size_t totalLen = 32 + messageLen + sizeof(nonceBytes) + sizeof(heightBytes);
    uint8_t* material = (uint8_t*)malloc(totalLen == 0 ? 1 : totalLen);
    if (!material) {
        return false;
    }

    size_t off = 0;
    memcpy(material + off, seed32, 32);
    off += 32;
    if (messageLen > 0) {
        memcpy(material + off, message, messageLen);
        off += messageLen;
    }
    memcpy(material + off, nonceBytes, sizeof(nonceBytes));
    off += sizeof(nonceBytes);
    memcpy(material + off, heightBytes, sizeof(heightBytes));

    const bool ok = Blake2b_Hash(material, totalLen, outHash, 32);
    free(material);
    return ok;
}

static int Cmp256BE(const uint8_t a[32], const uint8_t b[32]) {
    for (size_t i = 0; i < 32; ++i) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return 1;
    }
    return 0;
}

static void WriteU64LE(uint64_t v, uint8_t out[8]) {
    out[0] = (uint8_t)(v & 0xffu);
    out[1] = (uint8_t)((v >> 8) & 0xffu);
    out[2] = (uint8_t)((v >> 16) & 0xffu);
    out[3] = (uint8_t)((v >> 24) & 0xffu);
    out[4] = (uint8_t)((v >> 32) & 0xffu);
    out[5] = (uint8_t)((v >> 40) & 0xffu);
    out[6] = (uint8_t)((v >> 48) & 0xffu);
    out[7] = (uint8_t)((v >> 56) & 0xffu);
}

static void WriteU32LE(uint32_t v, uint8_t out[4]) {
    out[0] = (uint8_t)(v & 0xffu);
    out[1] = (uint8_t)((v >> 8) & 0xffu);
    out[2] = (uint8_t)((v >> 16) & 0xffu);
    out[3] = (uint8_t)((v >> 24) & 0xffu);
}

static bool DeriveSeedFromMessage(const uint8_t* message, size_t messageLen, uint8_t outSeed[32]) {
    if (!message || !outSeed) {
        return false;
    }

    return Blake2b_Hash(message, messageLen, outSeed, 32);
}

static bool ComputeLaneIndex(
    const uint8_t seed32[32],
    uint64_t nonce,
    uint64_t height,
    uint32_t round,
    uint32_t laneCount,
    uint32_t* outLane
) {
    if (!seed32 || !outLane || laneCount == 0) {
        return false;
    }

    uint8_t input[32 + 8 + 8 + 4];
    uint8_t digest[32];
    uint8_t nonceBytes[8];
    uint8_t heightBytes[8];
    uint8_t roundBytes[4];

    WriteU64LE(nonce, nonceBytes);
    WriteU64LE(height, heightBytes);
    WriteU32LE(round, roundBytes);

    memcpy(input, seed32, 32);
    memcpy(input + 32, nonceBytes, sizeof(nonceBytes));
    memcpy(input + 40, heightBytes, sizeof(heightBytes));
    memcpy(input + 48, roundBytes, sizeof(roundBytes));

    if (!Blake2b_Hash(input, sizeof(input), digest, sizeof(digest))) {
        return false;
    }

    uint32_t raw =
        ((uint32_t)digest[0]) |
        ((uint32_t)digest[1] << 8) |
        ((uint32_t)digest[2] << 16) |
        ((uint32_t)digest[3] << 24);
    *outLane = raw % laneCount;
    return true;
}

static bool ReadDagLaneFromContext(const Autolykos2Context* ctx, uint32_t laneIndex, uint8_t outLane[32]) {
    if (!ctx || !ctx->dag.buf || !outLane) {
        return false;
    }

    const size_t offset = (size_t)laneIndex * 32u;
    if (ctx->dag.len < offset + 32u) {
        return false;
    }

    memcpy(outLane, ctx->dag.buf + offset, 32);
    return true;
}

static bool ReadDagLaneFromSeed(const uint8_t seed32[32], uint32_t laneIndex, uint8_t outLane[32]) {
    if (!seed32 || !outLane) {
        return false;
    }

    const uint64_t counter = (uint64_t)laneIndex / 2u;
    const bool upperHalf = (laneIndex & 1u) != 0u;
    uint8_t input[32 + 8];
    uint8_t digest[64];
    uint8_t counterBytes[8];

    WriteU64LE(counter, counterBytes);
    memcpy(input, seed32, 32);
    memcpy(input + 32, counterBytes, sizeof(counterBytes));

    if (!Blake2b_Hash(input, sizeof(input), digest, sizeof(digest))) {
        return false;
    }

    memcpy(outLane, digest + (upperHalf ? 32 : 0), 32);
    return true;
}

static bool Autolykos2_HashCore(
    const uint8_t seed32[32],
    const uint8_t* message,
    size_t messageLen,
    uint64_t nonce,
    uint64_t height,
    uint32_t laneCount,
    const Autolykos2Context* ctx,
    bool useContextDag,
    uint8_t outHash[32]
) {
    if (!seed32 || !message || !outHash || laneCount == 0) {
        return false;
    }

    uint8_t acc[32];
    memset(acc, 0, sizeof(acc));

    for (uint32_t round = 0; round < 32; ++round) {
        uint32_t laneIndex = 0;
        uint8_t lane[32];

        if (!ComputeLaneIndex(seed32, nonce, height, round, laneCount, &laneIndex)) {
            return false;
        }

        if (useContextDag) {
            if (!ReadDagLaneFromContext(ctx, laneIndex, lane)) {
                return false;
            }
        } else {
            if (!ReadDagLaneFromSeed(seed32, laneIndex, lane)) {
                return false;
            }
        }

        for (size_t i = 0; i < 32; ++i) {
            acc[i] ^= lane[i];
        }
    }

    uint8_t baseHash[32];
    uint8_t accHash[32];
    if (!Autolykos2_FallbackHash(seed32, message, messageLen, nonce, height, baseHash)) {
        return false;
    }
    if (!Blake2b_Hash(acc, sizeof(acc), accHash, sizeof(accHash))) {
        return false;
    }

    uint8_t finalInput[32 + 32 + 8 + 8];
    uint8_t nonceBytes[8];
    uint8_t heightBytes[8];
    WriteU64LE(nonce, nonceBytes);
    WriteU64LE(height, heightBytes);

    memcpy(finalInput, baseHash, 32);
    memcpy(finalInput + 32, accHash, 32);
    memcpy(finalInput + 64, nonceBytes, 8);
    memcpy(finalInput + 72, heightBytes, 8);
    return Blake2b_Hash(finalInput, sizeof(finalInput), outHash, 32);
}

Autolykos2Context* Autolykos2_Create(void) {
    Autolykos2Context* ctx = (Autolykos2Context*)calloc(1, sizeof(Autolykos2Context));
    if (!ctx) {
        return NULL;
    }

#ifdef MINICOIN_AUTOLYKOS2_REF_AVAILABLE
    ctx->backend = minicoin_autolykos2_ref_create();
#endif

    return ctx;
}

void Autolykos2_Destroy(Autolykos2Context* ctx) {
    if (!ctx) {
        return;
    }

#ifdef MINICOIN_AUTOLYKOS2_REF_AVAILABLE
    if (ctx->backend) {
        minicoin_autolykos2_ref_destroy(ctx->backend);
        ctx->backend = NULL;
    }
#endif

    free(ctx->dag.buf);
    free(ctx);
}

bool Autolykos2_DagAllocate(Autolykos2Context* ctx, size_t bytes) {
    if (!ctx) {
        return false;
    }

    uint8_t* newBuf = (uint8_t*)realloc(ctx->dag.buf, bytes == 0 ? 1 : bytes);
    if (!newBuf) {
        return false;
    }

    ctx->dag.buf = newBuf;
    ctx->dag.cap = bytes;
    if (ctx->dag.len > bytes) {
        ctx->dag.len = bytes;
    }
    if (bytes > 0) {
        memset(ctx->dag.buf, 0, bytes);
    }
    return true;
}

bool Autolykos2_DagAppend(Autolykos2Context* ctx, const uint8_t* data, size_t len) {
    if (!ctx || !data || len == 0) {
        return false;
    }

    if (ctx->dag.len + len > ctx->dag.cap) {
        return false;
    }

    memcpy(ctx->dag.buf + ctx->dag.len, data, len);
    ctx->dag.len += len;
    return true;
}

bool Autolykos2_DagGenerate(Autolykos2Context* ctx, const uint8_t seed32[32]) {
    if (!ctx || !seed32 || !ctx->dag.buf || ctx->dag.cap == 0) {
        return false;
    }

    uint8_t input[32 + 8];
    uint8_t digest[64];
    size_t offset = 0;
    uint64_t counter = 0;

    while (offset < ctx->dag.cap) {
        WriteU64LE(counter, input + 32);
        memcpy(input, seed32, 32);

        if (!Blake2b_Hash(input, sizeof(input), digest, sizeof(digest))) {
            return false;
        }

        size_t chunk = ctx->dag.cap - offset;
        if (chunk > sizeof(digest)) {
            chunk = sizeof(digest);
        }

        memcpy(ctx->dag.buf + offset, digest, chunk);
        offset += chunk;
        ++counter;
    }

    ctx->dag.len = ctx->dag.cap;
    return true;
}

void Autolykos2_DagClear(Autolykos2Context* ctx) {
    if (!ctx || !ctx->dag.buf) {
        return;
    }
    memset(ctx->dag.buf, 0, ctx->dag.cap);
    ctx->dag.len = 0;
}

size_t Autolykos2_DagSize(const Autolykos2Context* ctx) {
    return ctx ? ctx->dag.len : 0;
}

bool Autolykos2_Hash(
    Autolykos2Context* ctx,
    const uint8_t* message,
    size_t messageLen,
    uint64_t nonce,
    uint64_t height,
    uint8_t outHash[32]
) {
    if (!ctx || !message || !outHash) {
        return false;
    }

    if (!ctx->dag.buf || ctx->dag.len < 32 || (ctx->dag.len % 32) != 0) {
        return false;
    }

    uint8_t seed32[32];
    if (!DeriveSeedFromMessage(message, messageLen, seed32)) {
        return false;
    }

    const size_t laneCount64 = ctx->dag.len / 32u;
    if (laneCount64 == 0 || laneCount64 > UINT32_MAX) {
        return false;
    }

    return Autolykos2_HashCore(
        seed32,
        message,
        messageLen,
        nonce,
        height,
        (uint32_t)laneCount64,
        ctx,
        true,
        outHash
    );
}

bool Autolykos2_LightHash(const uint8_t* seed, blockchain_t* chain, uint64_t nonce, uint8_t* out) {
    if (!seed || !chain || !out) {
        return false;
    }

    const uint64_t height = (uint64_t)Chain_Size(chain);
    const size_t dagBytes = CalculateTargetDAGSize(chain);
    if (dagBytes < 32 || (dagBytes % 32) != 0) {
        return false;
    }

    const size_t laneCount64 = dagBytes / 32u;
    if (laneCount64 == 0 || laneCount64 > UINT32_MAX) {
        return false;
    }

    // Light path derives the needed DAG lanes from seed on-demand, no large DAG allocation required.
    return Autolykos2_HashCore(
        seed,
        seed,
        32,
        nonce,
        height,
        (uint32_t)laneCount64,
        NULL,
        false,
        out
    );
}

bool Autolykos2_CheckTarget(
    Autolykos2Context* ctx,
    const uint8_t message32[32],
    uint64_t nonce,
    uint64_t height,
    const uint8_t target32[32],
    uint8_t outHash[32]
) {
    if (!ctx || !message32 || !target32 || !outHash) {
        return false;
    }

#ifdef MINICOIN_AUTOLYKOS2_REF_AVAILABLE
    if (ctx->backend) {
        const bool ok = minicoin_autolykos2_ref_check_target(ctx->backend, message32, nonce, height, target32);
        if (Autolykos2_Hash(ctx, message32, 32, nonce, height, outHash)) {
            return ok;
        }
        return false;
    }
#endif

    if (!Autolykos2_Hash(ctx, message32, 32, nonce, height, outHash)) {
        return false;
    }
    return Cmp256BE(outHash, target32) <= 0;
}

bool Autolykos2_FindNonceSingleCore(
    Autolykos2Context* ctx,
    const uint8_t message32[32],
    uint64_t height,
    const uint8_t target32[32],
    uint64_t startNonce,
    uint64_t maxIterations,
    uint64_t* outNonce,
    uint8_t outHash[32]
) {
    if (!ctx || !message32 || !target32 || !outNonce || !outHash) {
        return false;
    }

    uint64_t nonce = startNonce;
    for (uint64_t i = 0; i < maxIterations; ++i, ++nonce) {
        if (Autolykos2_CheckTarget(ctx, message32, nonce, height, target32, outHash)) {
            *outNonce = nonce;
            return true;
        }
    }

    return false;
}
