#include <nets/orphan_pool.h>
#include <constants.h>
#include <dynarr.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    block_t* block;
    uint64_t height;
    uint64_t observedAtTipHeight; // local tip height when first seen; stamped once (reorg penalty)
    uint64_t sequence;            // insertion order, used to evict the oldest entry when full
    uint8_t hash[32];
} orphan_entry_t;

static DynArr* g_orphans = NULL;
static uint64_t g_nextSequence = 0;

// The pool is touched by the maintenance thread, by every per-peer TCP thread and by the REPL
// thread. It used to have no synchronisation at all, so a concurrent Insert could realloc the
// array out from under a scan that was holding a raw element pointer.
//
// Lock ordering: this mutex is never held while calling into chain.c (which takes chainLock).
// Candidate branches are collected under the lock, the lock is dropped, and only then is
// Chain_ReplaceBranch/Chain_AddBlock called.
static pthread_mutex_t g_orphanLock = PTHREAD_MUTEX_INITIALIZER;

static void OrphanPool_InitLocked(void) {
    if (!g_orphans) {
        g_orphans = DYNARR_CREATE(orphan_entry_t, 16);
    }
}

void OrphanPool_Init(void) {
    pthread_mutex_lock(&g_orphanLock);
    OrphanPool_InitLocked();
    pthread_mutex_unlock(&g_orphanLock);
}

void OrphanPool_Destroy(void) {
    pthread_mutex_lock(&g_orphanLock);
    if (g_orphans) {
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
    pthread_mutex_unlock(&g_orphanLock);
}

static ssize_t OrphanPool_FindByHashLocked(const uint8_t blockHash[32]) {
    if (!g_orphans || !blockHash) {
        return -1;
    }

    size_t n = DynArr_size(g_orphans);
    for (size_t i = 0; i < n; ++i) {
        orphan_entry_t* e = (orphan_entry_t*)DynArr_at(g_orphans, i);
        if (e && memcmp(e->hash, blockHash, 32) == 0) {
            return (ssize_t)i;
        }
    }

    return -1;
}

// Drop the entry with the lowest sequence number, so a flood of unusable orphans cannot grow
// without bound. Returns true if something was evicted.
static bool OrphanPool_EvictOldestLocked(void) {
    if (!g_orphans) {
        return false;
    }

    size_t n = DynArr_size(g_orphans);
    if (n == 0) {
        return false;
    }

    size_t oldestIndex = 0;
    uint64_t oldestSequence = UINT64_MAX;
    for (size_t i = 0; i < n; ++i) {
        orphan_entry_t* e = (orphan_entry_t*)DynArr_at(g_orphans, i);
        if (e && e->sequence < oldestSequence) {
            oldestSequence = e->sequence;
            oldestIndex = i;
        }
    }

    orphan_entry_t* victim = (orphan_entry_t*)DynArr_at(g_orphans, oldestIndex);
    if (victim && victim->block) {
        Block_Destroy(victim->block);
    }
    DynArr_remove(g_orphans, oldestIndex);
    return true;
}

void OrphanPool_Insert(block_t* block, uint64_t height, uint64_t observedAtTipHeight) {
    if (!block) {
        return;
    }

    uint8_t blockHash[32];
    Block_CalculateHash(block, blockHash);

    pthread_mutex_lock(&g_orphanLock);
    OrphanPool_InitLocked();
    if (!g_orphans) {
        pthread_mutex_unlock(&g_orphanLock);
        Block_Destroy(block);
        return;
    }

    // Reject duplicates. The same block reaches us from every peer that relays it, and without
    // this each copy became its own permanently-resident entry.
    if (OrphanPool_FindByHashLocked(blockHash) >= 0) {
        pthread_mutex_unlock(&g_orphanLock);
        Block_Destroy(block);
        return;
    }

    while (DynArr_size(g_orphans) >= MAX_ORPHAN_BLOCKS) {
        if (!OrphanPool_EvictOldestLocked()) {
            break;
        }
    }

    orphan_entry_t e;
    memset(&e, 0, sizeof(e));
    e.block = block;
    e.height = height;
    e.observedAtTipHeight = observedAtTipHeight;
    e.sequence = g_nextSequence++;
    memcpy(e.hash, blockHash, 32);

    if (!DynArr_push_back(g_orphans, &e)) {
        pthread_mutex_unlock(&g_orphanLock);
        Block_Destroy(block);
        return;
    }

    pthread_mutex_unlock(&g_orphanLock);
}

bool OrphanPool_Contains(const uint8_t blockHash[32]) {
    pthread_mutex_lock(&g_orphanLock);
    bool found = OrphanPool_FindByHashLocked(blockHash) >= 0;
    pthread_mutex_unlock(&g_orphanLock);
    return found;
}

size_t OrphanPool_Size(void) {
    pthread_mutex_lock(&g_orphanLock);
    size_t n = g_orphans ? DynArr_size(g_orphans) : 0;
    pthread_mutex_unlock(&g_orphanLock);
    return n;
}

// Remove the entry with this hash without freeing the block, and hand the block back. Used once a
// block has been given to the chain, which then owns its transaction array.
static block_t* OrphanPool_TakeByHashLocked(const uint8_t blockHash[32]) {
    ssize_t index = OrphanPool_FindByHashLocked(blockHash);
    if (index < 0) {
        return NULL;
    }

    orphan_entry_t* e = (orphan_entry_t*)DynArr_at(g_orphans, (size_t)index);
    block_t* blk = e ? e->block : NULL;
    DynArr_remove(g_orphans, (size_t)index);
    return blk;
}

static void OrphanPool_DropByHashLocked(const uint8_t blockHash[32]) {
    ssize_t index = OrphanPool_FindByHashLocked(blockHash);
    if (index < 0) {
        return;
    }

    orphan_entry_t* e = (orphan_entry_t*)DynArr_at(g_orphans, (size_t)index);
    if (e && e->block) {
        Block_Destroy(e->block);
    }
    DynArr_remove(g_orphans, (size_t)index);
}

/**
 * Copy out the orphan that extends `prevHash` at `height`, if any.
 * Returns false when there is no such orphan. Caller must hold the pool lock.
**/
static bool OrphanPool_FindChildLocked(uint64_t height,
                                       const uint8_t prevHash[32],
                                       block_t** outBlock,
                                       uint64_t* outObservedAtTipHeight,
                                       uint8_t outHash[32]) {
    if (!g_orphans) {
        return false;
    }

    size_t n = DynArr_size(g_orphans);
    for (size_t i = 0; i < n; ++i) {
        orphan_entry_t* e = (orphan_entry_t*)DynArr_at(g_orphans, i);
        if (!e || !e->block) {
            continue;
        }
        if (e->height != height) {
            continue;
        }
        if (memcmp(e->block->header.prevHash, prevHash, 32) != 0) {
            continue;
        }

        *outBlock = e->block;
        *outObservedAtTipHeight = e->observedAtTipHeight;
        memcpy(outHash, e->hash, 32);
        return true;
    }

    return false;
}

/**
 * Follow prevHash links from `forkHeight` to build the longest branch the pool can offer.
 *
 * The old implementation took the first orphan found at each successive height with no linkage
 * check at all, which could splice blocks from two different forks into one incoherent branch.
 * Caller must hold the pool lock. The returned array borrows the pooled block pointers; the pool
 * still owns them (Chain_ReplaceBranch applies copies).
**/
static size_t OrphanPool_CollectBranchLocked(uint64_t forkHeight,
                                             const uint8_t forkParentHash[32],
                                             block_t*** outBlocks,
                                             uint8_t** outHashes,
                                             uint64_t* outObservedAtTipHeight) {
    *outBlocks = NULL;
    *outHashes = NULL;
    *outObservedAtTipHeight = 0;

    DynArr* collected = DYNARR_CREATE(block_t*, 8);
    DynArr* hashes = DYNARR_CREATE(uint8_t, 8 * 32);
    if (!collected || !hashes) {
        if (collected) DynArr_destroy(collected);
        if (hashes) DynArr_destroy(hashes);
        return 0;
    }

    uint8_t expectedPrevHash[32];
    memcpy(expectedPrevHash, forkParentHash, 32);

    uint64_t earliestObserved = UINT64_MAX;
    uint64_t cursor = forkHeight;
    size_t count = 0;

    while (1) {
        block_t* child = NULL;
        uint64_t observed = 0;
        uint8_t childHash[32];
        if (!OrphanPool_FindChildLocked(cursor, expectedPrevHash, &child, &observed, childHash)) {
            break;
        }

        if (!DynArr_push_back(collected, &child)) {
            break;
        }
        for (size_t b = 0; b < 32; ++b) {
            if (!DynArr_push_back(hashes, &childHash[b])) {
                break;
            }
        }

        if (observed < earliestObserved) {
            earliestObserved = observed;
        }

        memcpy(expectedPrevHash, childHash, 32);
        cursor++;
        count++;
    }

    if (count == 0) {
        DynArr_destroy(collected);
        DynArr_destroy(hashes);
        return 0;
    }

    block_t** blocks = (block_t**)calloc(count, sizeof(block_t*));
    uint8_t* hashOut = (uint8_t*)calloc(count, 32);
    if (!blocks || !hashOut) {
        free(blocks);
        free(hashOut);
        DynArr_destroy(collected);
        DynArr_destroy(hashes);
        return 0;
    }

    for (size_t i = 0; i < count; ++i) {
        blocks[i] = *(block_t**)DynArr_at(collected, i);
        for (size_t b = 0; b < 32; ++b) {
            hashOut[i * 32 + b] = *(uint8_t*)DynArr_at(hashes, i * 32 + b);
        }
    }

    DynArr_destroy(collected);
    DynArr_destroy(hashes);

    *outBlocks = blocks;
    *outHashes = hashOut;
    *outObservedAtTipHeight = earliestObserved == UINT64_MAX ? 0ULL : earliestObserved;
    return count;
}

// Discard orphans that can no longer ever be applied: anything at or below the current tip whose
// hash does not match the block we actually have there. Without this the pool only ever grew, and
// permanently-invalid entries were retried on every 1 Hz maintenance tick.
static void OrphanPool_PruneStale(blockchain_t* chain) {
    if (!chain) {
        return;
    }

    const size_t chainSize = Chain_Size(chain);

    // Collect the hashes to drop first, so we never call into chain.c while holding the pool lock.
    DynArr* doomed = DYNARR_CREATE(uint8_t, 32);
    if (!doomed) {
        return;
    }

    pthread_mutex_lock(&g_orphanLock);
    size_t n = g_orphans ? DynArr_size(g_orphans) : 0;
    DynArr* candidates = DYNARR_CREATE(uint8_t, 32);
    DynArr* candidateHeights = DYNARR_CREATE(uint64_t, 8);
    if (candidates && candidateHeights) {
        for (size_t i = 0; i < n; ++i) {
            orphan_entry_t* e = (orphan_entry_t*)DynArr_at(g_orphans, i);
            if (!e || !e->block) {
                continue;
            }
            if (e->height >= (uint64_t)chainSize) {
                continue; // still ahead of us; may attach later
            }
            for (size_t b = 0; b < 32; ++b) {
                (void)DynArr_push_back(candidates, &e->hash[b]);
            }
            (void)DynArr_push_back(candidateHeights, &e->height);
        }
    }
    pthread_mutex_unlock(&g_orphanLock);

    size_t candidateCount = candidateHeights ? DynArr_size(candidateHeights) : 0;
    for (size_t i = 0; i < candidateCount; ++i) {
        uint64_t height = *(uint64_t*)DynArr_at(candidateHeights, i);
        uint8_t orphanHash[32];
        for (size_t b = 0; b < 32; ++b) {
            orphanHash[b] = *(uint8_t*)DynArr_at(candidates, i * 32 + b);
        }

        block_t* local = NULL;
        if (!Chain_GetBlockCopy(chain, (size_t)height, &local) || !local) {
            continue;
        }

        uint8_t localHash[32];
        Block_CalculateHash(local, localHash);
        Block_Destroy(local);

        // Same block we already have: pure duplicate, drop it. A different block at a height we
        // have already passed is kept, because it may yet be the base of a heavier branch.
        if (memcmp(localHash, orphanHash, 32) == 0) {
            for (size_t b = 0; b < 32; ++b) {
                (void)DynArr_push_back(doomed, &orphanHash[b]);
            }
        }
    }

    size_t doomedCount = DynArr_size(doomed) / 32;
    if (doomedCount > 0) {
        pthread_mutex_lock(&g_orphanLock);
        for (size_t i = 0; i < doomedCount; ++i) {
            uint8_t h[32];
            for (size_t b = 0; b < 32; ++b) {
                h[b] = *(uint8_t*)DynArr_at(doomed, i * 32 + b);
            }
            OrphanPool_DropByHashLocked(h);
        }
        pthread_mutex_unlock(&g_orphanLock);
    }

    if (candidates) DynArr_destroy(candidates);
    if (candidateHeights) DynArr_destroy(candidateHeights);
    DynArr_destroy(doomed);
}

/**
 * Try to extend the current tip directly with pooled orphans.
 * Returns the number of blocks attached.
**/
static size_t OrphanPool_ExtendTip(blockchain_t* chain) {
    size_t attached = 0;

    while (1) {
        const size_t chainSize = Chain_Size(chain);

        uint8_t tipHash[32];
        memset(tipHash, 0, sizeof(tipHash));
        if (chainSize > 0) {
            block_t* tip = NULL;
            if (!Chain_GetBlockCopy(chain, chainSize - 1, &tip) || !tip) {
                break;
            }
            Block_CalculateHash(tip, tipHash);
            Block_Destroy(tip);
        }

        // Take a copy of the candidate under the lock, then release it before touching the chain.
        pthread_mutex_lock(&g_orphanLock);
        block_t* pooled = NULL;
        uint64_t observed = 0;
        uint8_t candidateHash[32];
        bool found = OrphanPool_FindChildLocked((uint64_t)chainSize, tipHash, &pooled, &observed, candidateHash);
        block_t* candidate = found ? Block_Copy(pooled) : NULL;
        pthread_mutex_unlock(&g_orphanLock);

        if (!found) {
            break;
        }
        if (!candidate) {
            break;
        }

        if (!Chain_AddBlock(chain, candidate)) {
            // Permanent rejection for this block at this height (bad coinbase, wrong difficulty,
            // ...). Drop it rather than retrying it on every maintenance tick forever.
            Block_Destroy(candidate);
            pthread_mutex_lock(&g_orphanLock);
            OrphanPool_DropByHashLocked(candidateHash);
            pthread_mutex_unlock(&g_orphanLock);
            continue;
        }

        // The chain took over the copy's transaction array; free only our wrapper.
        free(candidate);

        pthread_mutex_lock(&g_orphanLock);
        block_t* taken = OrphanPool_TakeByHashLocked(candidateHash);
        pthread_mutex_unlock(&g_orphanLock);
        if (taken) {
            Block_Destroy(taken); // the pool's own copy is independent of the one we applied
        }

        attached++;
    }

    return attached;
}

/**
 * Look for a competing branch that forks below our tip and is worth adopting.
 * The work comparison, the reorg penalty and the atomicity all live in Chain_ReplaceBranch.
**/
static size_t OrphanPool_TryAdoptBranch(blockchain_t* chain) {
    const size_t chainSize = Chain_Size(chain);
    if (chainSize == 0) {
        return 0;
    }

    // Walk fork points from just below the tip downwards; the shallowest fork wins, which is also
    // the one with the smallest reorg penalty.
    for (size_t forkHeight = chainSize; forkHeight >= 1; --forkHeight) {
        block_t* parent = NULL;
        if (!Chain_GetBlockCopy(chain, forkHeight - 1, &parent) || !parent) {
            continue;
        }
        uint8_t parentHash[32];
        Block_CalculateHash(parent, parentHash);
        Block_Destroy(parent);

        pthread_mutex_lock(&g_orphanLock);
        block_t** branch = NULL;
        uint8_t* branchHashes = NULL;
        uint64_t observedAtTipHeight = 0;
        size_t branchCount = OrphanPool_CollectBranchLocked((uint64_t)forkHeight, parentHash,
                                                            &branch, &branchHashes, &observedAtTipHeight);
        // Copy the branch so the pool lock can be released before we call into the chain.
        block_t** branchCopies = NULL;
        if (branchCount > 0) {
            branchCopies = (block_t**)calloc(branchCount, sizeof(block_t*));
            if (branchCopies) {
                for (size_t i = 0; i < branchCount; ++i) {
                    branchCopies[i] = Block_Copy(branch[i]);
                }
            }
        }
        pthread_mutex_unlock(&g_orphanLock);

        free(branch);

        if (branchCount == 0 || !branchCopies) {
            free(branchHashes);
            if (branchCopies) {
                free(branchCopies);
            }
            if (forkHeight == 1) break;
            continue;
        }

        bool copiedAll = true;
        for (size_t i = 0; i < branchCount; ++i) {
            if (!branchCopies[i]) {
                copiedAll = false;
            }
        }

        bool adopted = false;
        if (copiedAll) {
            adopted = Chain_ReplaceBranch(chain, forkHeight, branchCopies, branchCount, observedAtTipHeight);
        }

        for (size_t i = 0; i < branchCount; ++i) {
            if (branchCopies[i]) {
                Block_Destroy(branchCopies[i]); // Chain_ReplaceBranch applied its own copies
            }
        }
        free(branchCopies);

        if (adopted) {
            printf("Adopted competing branch of %zu block(s) at fork height %zu\n", branchCount, forkHeight);
            pthread_mutex_lock(&g_orphanLock);
            for (size_t i = 0; i < branchCount; ++i) {
                OrphanPool_DropByHashLocked(&branchHashes[i * 32]);
            }
            pthread_mutex_unlock(&g_orphanLock);
            free(branchHashes);
            return branchCount;
        }

        free(branchHashes);

        if (forkHeight == 1) {
            break;
        }
    }

    return 0;
}

size_t OrphanPool_AttemptAttach(blockchain_t* chain) {
    if (!chain) {
        return 0;
    }

    pthread_mutex_lock(&g_orphanLock);
    bool empty = (g_orphans == NULL) || (DynArr_size(g_orphans) == 0);
    pthread_mutex_unlock(&g_orphanLock);
    if (empty) {
        return 0;
    }

    size_t attached = 0;

    // Extending the tip is always preferable to a reorg, so try that to exhaustion first, and only
    // then consider replacing part of our chain with a competing branch.
    while (1) {
        size_t extended = OrphanPool_ExtendTip(chain);
        attached += extended;

        size_t adopted = OrphanPool_TryAdoptBranch(chain);
        attached += adopted;

        if (extended == 0 && adopted == 0) {
            break;
        }
    }

    OrphanPool_PruneStale(chain);

    return attached;
}
