#ifndef LIBRX_WRAPPER_H
#define LIBRX_WRAPPER_H

#include <stddef.h>
#include <stdint.h>
#include <randomx.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool RandomX_Init(const char* key);
void RandomX_Destroy();
void RandomX_CalculateHash(const uint8_t* input, size_t inputLen, uint8_t* output);

#ifdef __cplusplus
}
#endif

#endif