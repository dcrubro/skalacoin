#ifndef MINICOIN_AUTOLYKOS2_H
#define MINICOIN_AUTOLYKOS2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <constants.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Autolykos2Context Autolykos2Context;

Autolykos2Context* Autolykos2_Create(void);
void Autolykos2_Destroy(Autolykos2Context* ctx);

bool Autolykos2_DagAllocate(Autolykos2Context* ctx, size_t bytes);
bool Autolykos2_DagAppend(Autolykos2Context* ctx, const uint8_t* data, size_t len);
bool Autolykos2_DagGenerate(Autolykos2Context* ctx, const uint8_t seed32[32]);
void Autolykos2_DagClear(Autolykos2Context* ctx);
size_t Autolykos2_DagSize(const Autolykos2Context* ctx);

bool Autolykos2_Hash(
	Autolykos2Context* ctx,
	const uint8_t* message,
	size_t messageLen,
	uint64_t nonce,
	uint64_t height,
	uint8_t outHash[32]
);

bool Autolykos2_LightHash(const uint8_t* seed, blockchain_t* chain, uint64_t nonce, uint8_t* out);

bool Autolykos2_CheckTarget(
	Autolykos2Context* ctx,
	const uint8_t message32[32],
	uint64_t nonce,
	uint64_t height,
	const uint8_t target32[32],
	uint8_t outHash[32]
);

bool Autolykos2_FindNonceSingleCore(
	Autolykos2Context* ctx,
	const uint8_t message32[32],
	uint64_t height,
	const uint8_t target32[32],
	uint64_t startNonce,
	uint64_t maxIterations,
	uint64_t* outNonce,
	uint8_t outHash[32]
);

#ifdef __cplusplus
}
#endif

#endif
