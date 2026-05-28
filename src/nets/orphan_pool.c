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

static size_t OrphanPool_TryAdoptBranch(blockchain_t* chain, uint64_t forkHeight) {
    if (!g_orphans || !chain) return 0;

    DynArr* seq = DYNARR_CREATE(block_t*, 8);
    if (!seq) return 0;

    size_t cursor = forkHeight;
    while (1) {
        bool found = false;
        size_t count = DynArr_size(g_orphans);
        for (size_t i = 0; i < count; ++i) {
            orphan_entry_t* entry = (orphan_entry_t*)DynArr_at(g_orphans, i);
            if (!entry || !entry->block) continue;
            if (entry->height == cursor) {
                (void)DynArr_push_back(seq, &entry->block);
                found = true;
                break;
            }
        }
        if (!found) break;
        cursor++;
    }

    size_t seqCount = DynArr_size(seq);
    if (seqCount == 0) {
        DynArr_destroy(seq);
        return 0;
    }

    size_t currentTipHeight = Chain_Size(chain) == 0 ? 0 : Chain_Size(chain) - 1;
    size_t seqTopHeight = forkHeight + seqCount - 1;
    if (seqTopHeight <= currentTipHeight) {
        DynArr_destroy(seq);
        return 0;
    }

    size_t rollbackHeight = (forkHeight == 0) ? 0 : (forkHeight - 1);
    if (!Chain_RollbackToHeight(chain, rollbackHeight)) {
        DynArr_destroy(seq);
        return 0;
    }

    size_t attached = 0;
    for (size_t i = 0; i < seqCount; ++i) {
        block_t* bptr = *(block_t**)DynArr_at(seq, i);
        if (!bptr || !Chain_AddBlock(chain, bptr)) {
            break;
        }

        size_t count = DynArr_size(g_orphans);
        for (size_t j = 0; j < count; ++j) {
            orphan_entry_t* entry = (orphan_entry_t*)DynArr_at(g_orphans, j);
            if (entry && entry->block == bptr) {
                DynArr_remove(g_orphans, j);
                break;
            }
        }

        attached++;
    }

    DynArr_destroy(seq);
    return attached;
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
                if (e->height < Chain_Size(chain)) {
                    block_t* local = NULL;
                    if (Chain_GetBlockCopy(chain, (size_t)e->height, &local) && local) {
                        uint8_t localHash[32];
                        uint8_t orphanHash[32];
                        Block_CalculateHash(local, localHash);
                        Block_CalculateHash(e->block, orphanHash);
                        Block_Destroy(local);

                        if (memcmp(localHash, orphanHash, 32) != 0) {
                            size_t adopted = OrphanPool_TryAdoptBranch(chain, e->height);
                            if (adopted > 0) {
                                attached += adopted;
                                madeProgress = true;
                                n = DynArr_size(g_orphans);
                                i = (size_t)-1;
                                break;
                            }
                        }
                    } else if (local) {
                        Block_Destroy(local);
                    }
                }

                // Verify that the parent's hash matches the orphan's prevHash before attaching.
                bool parentMatches = false;
                if (e->height == 0) {
                    parentMatches = (Chain_Size(chain) == 0);
                } else {
                    block_t* parent = NULL;
                    if (Chain_GetBlockCopy(chain, (size_t)parentIndex, &parent) && parent) {
                        uint8_t parentHash[32];
                        Block_CalculateHash(parent, parentHash);
                        parentMatches = (memcmp(parentHash, e->block->header.prevHash, 32) == 0);
                        Block_Destroy(parent);
                    } else {
                        parentMatches = false;
                    }
                }

                if (!parentMatches) {
                    // Parent exists but does not match this orphan's prevHash.
                    size_t adopted = OrphanPool_TryAdoptBranch(chain, e->height);
                    if (adopted > 0) {
                        attached += adopted;
                        madeProgress = true;
                        n = DynArr_size(g_orphans);
                        i = (size_t)-1;
                        break;
                    }

                    continue;
                }

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
