#include <block/chain.h>

blockchain_t* Chain_Create() {
    blockchain_t* ptr = (blockchain_t*)malloc(sizeof(blockchain_t));
    if (!ptr) {
        return NULL;
    }

    ptr->blocks = DYNARR_CREATE(block_t, 1);
    ptr->size = 0;

    return ptr;
}

void Chain_Destroy(blockchain_t* chain) {
    if (chain) {
        if (chain->blocks) {
            DynArr_destroy(chain->blocks);
        }
        free(chain);
    }
}

bool Chain_AddBlock(blockchain_t* chain, block_t* block) {
    if (chain && block && chain->blocks) {
        DynArr_push_back(chain->blocks, block);
        return true;
    }

    return false;
}

block_t* Chain_GetBlock(blockchain_t* chain, size_t index) {
    if (chain) {
        return DynArr_at(chain->blocks, index);
    }
    return NULL;
}

size_t Chain_Size(blockchain_t* chain) {
    if (chain) {
        return DynArr_size(chain->blocks);
    }
    return 0;
}

bool Chain_IsValid(blockchain_t* chain) {
    if (!chain || !chain->blocks) {
        return false;
    }
    // Add validation logic here
    return true;
}
