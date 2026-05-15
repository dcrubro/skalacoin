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
                        // Attempt to detect a longer alternate chain in the orphan pool starting at this height.
                        // Build a consecutive sequence of orphans from this height upward.
                        DynArr* seq = DYNARR_CREATE(block_t*, 8);
                        size_t h = e->height;
                        while (1) {
                            bool found = false;
                            size_t gn = DynArr_size(g_orphans);
                            for (size_t gi = 0; gi < gn; ++gi) {
                                orphan_entry_t* oe = (orphan_entry_t*)DynArr_at(g_orphans, gi);
                                if (!oe || !oe->block) continue;
                                if (oe->height == h) {
                                    (void)DynArr_push_back(seq, &oe->block);
                                    found = true;
                                    break;
                                }
                            }
                            if (!found) break;
                            h++;
                        }

                        size_t seqCount = DynArr_size(seq);
                        if (seqCount > 0) {
                            size_t seqTopHeight = e->height + seqCount - 1;
                            if (seqTopHeight >= Chain_Size(chain)) {
                                // Found a candidate longer branch. Perform rollback to fork height and attach sequence.
                                if (Chain_RollbackToHeight(chain, (size_t)e->height)) {
                                    // Attach in-order
                                    for (size_t si = 0; si < seqCount; ++si) {
                                        block_t* bptr = *(block_t**)DynArr_at(seq, si);
                                        if (!Chain_AddBlock(chain, bptr)) {
                                            // failed to add; stop attempting further
                                            break;
                                        }
                                        // Remove the attached orphan from pool but keep the block object to preserve transactions in-memory (consistent with existing behavior)
                                        // Find and remove corresponding orphan entry
                                        size_t gn2 = DynArr_size(g_orphans);
                                        for (size_t gi2 = 0; gi2 < gn2; ++gi2) {
                                            orphan_entry_t* oe2 = (orphan_entry_t*)DynArr_at(g_orphans, gi2);
                                            if (oe2 && oe2->block == bptr) {
                                                DynArr_remove(g_orphans, gi2);
                                                gn2 = DynArr_size(g_orphans);
                                                gi2 = (size_t)-1; // restart search if needed
                                            }
                                        }
                                    }
                                    attached += seqCount;
                                    madeProgress = true;
                                    DynArr_destroy(seq);
                                    // reset outer loop
                                    n = DynArr_size(g_orphans);
                                    i = (size_t)-1;
                                    break;
                                }
                            }
                        }
                        DynArr_destroy(seq);
                        // If we didn't perform a reorg/attach, skip for now.
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
