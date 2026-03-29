#ifndef CRYPTO_H
#define CRYPTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <openssl/sha.h>
#include <secp256k1.h>

bool Crypto_VerifySignature(const uint8_t* data, size_t len, const uint8_t* signature, const uint8_t* publicKey);
void Crypto_SignData(const uint8_t* data, size_t len, const uint8_t* privateKey, uint8_t* outSignature);

#endif
