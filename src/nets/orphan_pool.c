#include <nets/orphan_pool.h>
#include <dynarr.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    block_t* block;
    uint64_t height;
} orphan_entry_t;

static DynArr* g_orphans = NULL;

void OrphanPool_Init(void) {
    if (g_orphans) return;
    g_orphans = DYNARR_CREATE(orphan_entry_t, 16);
}

void OrphanPool_Destroy(void) {
    if (!g_orphans) return;
    size_t n = DynArr_size(g_orphans);
    for (size_t i = 0; i < n; ++i) {
        orphan_entry_t* e = (orphan_entry_t*)DynArr_at(g_orphans, i);
        if (e && e->block) {
            Block_Destroy(e->block);
        }
    }
    DynArr_destroy(g_orphans);
    g_orphans = NULL;
}

void OrphanPool_Insert(block_t* block, uint64_t height) {
    if (!block) return;
    if (!g_orphans) OrphanPool_Init();
    orphan_entry_t e;
    e.block = block;
    e.height = height;
    (void)DynArr_push_back(g_orphans, &e);
}

size_t OrphanPool_AttemptAttach(blockchain_t* chain) {
    if (!g_orphans || !chain) return 0;
    size_t attached = 0;
    bool madeProgress = true;

    // Attempt repeatedly while progress is made (to handle chained orphans)
    while (madeProgress) {
        madeProgress = false;
        size_t n = DynArr_size(g_orphans);
        for (size_t i = 0; i < n; ++i) {
            orphan_entry_t* e = (orphan_entry_t*)DynArr_at(g_orphans, i);
            if (!e || !e->block) continue;

            uint64_t parentIndex = (e->height == 0) ? (uint64_t)-1 : (e->height - 1);
            bool parentExists = false;
            if (e->height == 0) {
                // genesis-style block: parent is zero-hash; accept if chain empty
                parentExists = (Chain_Size(chain) == 0);
            } else if (parentIndex < Chain_Size(chain)) {
                block_t* parent = NULL;
                if (Chain_GetBlockCopy(chain, (size_t)parentIndex, &parent) && parent) {
                    parentExists = true;
                    Block_Destroy(parent);
                } else {
                    parentExists = false;
                }
            }

            if (parentExists) {
                // Try to add to chain
                if (Chain_AddBlock(chain, e->block)) {
                    attached++;
                    madeProgress = true;
                    // remove this entry
                    DynArr_remove(g_orphans, i);
                    // adjust indices
                    n = DynArr_size(g_orphans);
                    i = (size_t)-1; // reset outer loop
                    break;
                } else {
                    // Chain_AddBlock rejected it (maybe invalid). Drop it.
                    Block_Destroy(e->block);
                    DynArr_remove(g_orphans, i);
                    n = DynArr_size(g_orphans);
                    i = (size_t)-1;
                    madeProgress = true;
                    break;
                }
            }
        }
    }

    return attached;
}
