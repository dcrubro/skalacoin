#include <crypto/crypto.h>
#include <string.h>

static bool crypto_hash_to_32(const uint8_t* data, size_t len, uint8_t out32[32]) {
    if (!data || !out32) {
        return false;
    }
    return SHA256(data, len, out32) != NULL;
}

bool Crypto_VerifySignature(const uint8_t* data, size_t len, const uint8_t* signature, const uint8_t* publicKey) {
    if (!data || !signature || !publicKey) {
        return false;
    }

    uint8_t digest[32];
    if (!crypto_hash_to_32(data, len, digest)) {
        return false;
    }

    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    if (!ctx) {
        return false;
    }

    secp256k1_pubkey pubkey;
    secp256k1_ecdsa_signature sig;

    const int pub_ok = secp256k1_ec_pubkey_parse(ctx, &pubkey, publicKey, 33);
    const int sig_ok = secp256k1_ecdsa_signature_parse_compact(ctx, &sig, signature);
    const int verified = (pub_ok && sig_ok) ? secp256k1_ecdsa_verify(ctx, &sig, digest, &pubkey) : 0;

    secp256k1_context_destroy(ctx);
    return verified == 1;
}

void Crypto_SignData(const uint8_t* data, size_t len, const uint8_t* privateKey, uint8_t* outSignature) {
    if (!data || !privateKey || !outSignature) {
        return;
    }

    uint8_t digest[32];
    if (!crypto_hash_to_32(data, len, digest)) {
        memset(outSignature, 0, 64);
        return;
    }

    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    if (!ctx) {
        memset(outSignature, 0, 64);
        return;
    }

    if (!secp256k1_ec_seckey_verify(ctx, privateKey)) {
        memset(outSignature, 0, 64);
        secp256k1_context_destroy(ctx);
        return;
    }

    secp256k1_ecdsa_signature sig;
    const int sign_ok = secp256k1_ecdsa_sign(
        ctx,
        &sig,
        digest,
        privateKey,
        secp256k1_nonce_function_default,
        NULL
    );
    if (!sign_ok) {
        memset(outSignature, 0, 64);
        secp256k1_context_destroy(ctx);
        return;
    }

    secp256k1_ecdsa_signature_serialize_compact(ctx, outSignature, &sig);
    secp256k1_context_destroy(ctx);
}

void to_hex(const uint8_t *in, char *out) {
    static const char hex[] = "0123456789abcdef";

    for (int i = 0; i < 32; i++) {
        out[i * 2]     = hex[in[i] >> 4];
        out[i * 2 + 1] = hex[in[i] & 0x0F];
    }
    out[64] = '\0';
}
