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
    const Autolykos2Context* ctx,
    const uint8_t* message,
    size_t messageLen,
    uint64_t nonce,
    uint32_t height,
    uint8_t outHash[32]
) {
    uint8_t nonceBytes[8];
    uint8_t heightBytes[4];
    memcpy(nonceBytes, &nonce, sizeof(nonceBytes));
    memcpy(heightBytes, &height, sizeof(heightBytes));

    size_t dagChunkLen = 0;
    const uint8_t* dagChunk = NULL;
    if (ctx && ctx->dag.buf && ctx->dag.len > 0) {
        dagChunk = ctx->dag.buf;
        dagChunkLen = ctx->dag.len > 64 ? 64 : ctx->dag.len;
    }

    const size_t totalLen = messageLen + sizeof(nonceBytes) + sizeof(heightBytes) + dagChunkLen;
    uint8_t* material = (uint8_t*)malloc(totalLen == 0 ? 1 : totalLen);
    if (!material) {
        return false;
    }

    size_t off = 0;
    if (messageLen > 0) {
        memcpy(material + off, message, messageLen);
        off += messageLen;
    }
    memcpy(material + off, nonceBytes, sizeof(nonceBytes));
    off += sizeof(nonceBytes);
    memcpy(material + off, heightBytes, sizeof(heightBytes));
    off += sizeof(heightBytes);
    if (dagChunkLen > 0) {
        memcpy(material + off, dagChunk, dagChunkLen);
    }

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
    uint32_t height,
    uint8_t outHash[32]
) {
    if (!ctx || !message || !outHash) {
        return false;
    }

    return Autolykos2_FallbackHash(ctx, message, messageLen, nonce, height, outHash);
}

bool Autolykos2_CheckTarget(
    Autolykos2Context* ctx,
    const uint8_t message32[32],
    uint64_t nonce,
    uint32_t height,
    const uint8_t target32[32],
    uint8_t outHash[32]
) {
    if (!ctx || !message32 || !target32 || !outHash) {
        return false;
    }

#ifdef MINICOIN_AUTOLYKOS2_REF_AVAILABLE
    if (ctx->backend) {
        const bool ok = minicoin_autolykos2_ref_check_target(ctx->backend, message32, nonce, height, target32);
        if (Autolykos2_FallbackHash(ctx, message32, 32, nonce, height, outHash)) {
            return ok;
        }
        return false;
    }
#endif

    if (!Autolykos2_FallbackHash(ctx, message32, 32, nonce, height, outHash)) {
        return false;
    }
    return Cmp256BE(outHash, target32) <= 0;
}

bool Autolykos2_FindNonceSingleCore(
    Autolykos2Context* ctx,
    const uint8_t message32[32],
    uint32_t height,
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
