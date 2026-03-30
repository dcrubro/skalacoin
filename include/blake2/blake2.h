#ifndef MINICOIN_BLAKE2_H
#define MINICOIN_BLAKE2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MINICOIN_BLAKE2B_OUTBYTES 64
#define MINICOIN_BLAKE2S_OUTBYTES 32

bool Blake2b_Hash(const uint8_t* input, size_t inputLen, uint8_t* out, size_t outLen);
bool Blake2s_Hash(const uint8_t* input, size_t inputLen, uint8_t* out, size_t outLen);

#endif
