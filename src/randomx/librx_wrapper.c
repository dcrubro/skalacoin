#include <randomx/librx_wrapper.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static randomx_cache* rxCache = NULL;
static randomx_dataset* rxDataset = NULL;
static randomx_vm* rxVm = NULL;

bool RandomX_Init(const char* key) {
    if (!key || rxCache || rxVm) {
        return false;
    }

    const randomx_flags baseFlags = randomx_get_flags() | RANDOMX_FLAG_JIT;
    randomx_flags vmFlags = baseFlags | RANDOMX_FLAG_FULL_MEM;

    rxCache = randomx_alloc_cache(baseFlags);
    if (!rxCache) {
        return false;
    }

    randomx_init_cache(rxCache, key, strlen(key));

    // Prefer full-memory mode. If dataset allocation fails, fall back to light mode.
    rxDataset = randomx_alloc_dataset(vmFlags);
    if (rxDataset) {
        const unsigned long datasetItems = randomx_dataset_item_count();
        randomx_init_dataset(rxDataset, rxCache, 0, datasetItems);
        rxVm = randomx_create_vm(vmFlags, NULL, rxDataset);
        if (rxVm) {
            printf("RandomX initialized in full-memory mode\n");
            return true;
        }

        randomx_release_dataset(rxDataset);
        rxDataset = NULL;
    }

    vmFlags = baseFlags;
    rxVm = randomx_create_vm(vmFlags, rxCache, NULL);
    if (!rxVm) {
        randomx_release_cache(rxCache);
        rxCache = NULL;
        return false;
    }

    printf("RandomX initialized in light mode\n");
    return true;
}

void RandomX_Destroy() {
    if (rxVm) {
        randomx_destroy_vm(rxVm);
        rxVm = NULL;
    }
    if (rxDataset) {
        randomx_release_dataset(rxDataset);
        rxDataset = NULL;
    }
    if (rxCache) {
        randomx_release_cache(rxCache);
        rxCache = NULL;
    }
}

void RandomX_CalculateHash(const uint8_t* input, size_t inputLen, uint8_t* output) {
    if (!rxVm || !input || !output) {
        return;
    }
    randomx_calculate_hash(rxVm, input, inputLen, output);
}
