#include <blake2/blake2.h>

#include <openssl/evp.h>
#include <string.h>

static bool Blake2_HashInternal(
    const EVP_MD* md,
    size_t maxDigestLen,
    const uint8_t* input,
    size_t inputLen,
    uint8_t* out,
    size_t outLen
) {
    if (!md || !out || outLen == 0 || outLen > maxDigestLen) {
        return false;
    }

    uint8_t digest[EVP_MAX_MD_SIZE];
    unsigned int digestLen = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return false;
    }

    bool ok = EVP_DigestInit_ex(ctx, md, NULL) == 1;
    if (ok && inputLen > 0) {
        ok = EVP_DigestUpdate(ctx, input, inputLen) == 1;
    }
    if (ok) {
        ok = EVP_DigestFinal_ex(ctx, digest, &digestLen) == 1;
    }

    EVP_MD_CTX_free(ctx);

    if (!ok || digestLen < outLen) {
        return false;
    }

    memcpy(out, digest, outLen);
    return true;
}

bool Blake2b_Hash(const uint8_t* input, size_t inputLen, uint8_t* out, size_t outLen) {
    return Blake2_HashInternal(EVP_blake2b512(), SKALACOIN_BLAKE2B_OUTBYTES, input, inputLen, out, outLen);
}

bool Blake2s_Hash(const uint8_t* input, size_t inputLen, uint8_t* out, size_t outLen) {
    return Blake2_HashInternal(EVP_blake2s256(), SKALACOIN_BLAKE2S_OUTBYTES, input, inputLen, out, outLen);
}
