#include <block/block.h>
#include <block/chain.h>
#include <autolykos2/autolykos2.h>
#include <utils.h>
#include <stdlib.h>
#include <pthread.h>

/**
 * The process-global mining DAG.
 *
 * Guarded by `g_powCtxLock` because generation frees and reallocates the buffer that hashing reads
 * from: without the lock, an epoch rollover would pull the DAG out from under a miner mid-hash.
 * Only the miner ever builds or reads this -- validation goes through the light path -- so the lock
 * is essentially uncontended, and a node that does not mine never allocates a DAG at all.
**/
static Autolykos2Context* g_autolykos2Ctx = NULL;
static pthread_mutex_t g_powCtxLock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_dagEpoch = 0;
// The seed the current DAG was generated from. Matching on epoch index and size is NOT enough: a
// reorg replaces the block an epoch's seed is derived from while leaving the epoch index and size
// unchanged, so a stale DAG would still look current and silently hash against the wrong lanes.
static uint8_t g_dagSeed[32];
static bool g_dagReady = false;

// Caller must hold `g_powCtxLock`.
static Autolykos2Context* GetAutolykos2CtxLocked(void) {
    if (!g_autolykos2Ctx) {
        g_autolykos2Ctx = Autolykos2_Create();
        if (!g_autolykos2Ctx) {
            fprintf(stderr, "Failed to create Autolykos2 context\n");
            exit(1);
        }
        // Deliberately no DagAllocate here. Allocating without generating leaves dag.len == 0, so
        // every heavy hash fails -- which used to be indistinguishable from a valid proof, because
        // the failure path handed back a zeroed hash that compares below every target.
    }
    return g_autolykos2Ctx;
}

void Block_ShutdownPowContext(void) {
    pthread_mutex_lock(&g_powCtxLock);
    if (g_autolykos2Ctx) {
        Autolykos2_Destroy(g_autolykos2Ctx);
        g_autolykos2Ctx = NULL;
    }
    g_dagReady = false;
    pthread_mutex_unlock(&g_powCtxLock);
}

bool Block_EnsureAutolykos2Dag(uint64_t epochIndex, size_t dagBytes, const uint8_t seed32[32]) {
    if (!seed32 || dagBytes < 32u || (dagBytes % 32u) != 0u) {
        return false;
    }

    pthread_mutex_lock(&g_powCtxLock);

    // Already built from exactly this seed at this size: generation is seconds of work, never redo
    // it. The seed has to be part of the test -- see g_dagSeed.
    if (g_dagReady && g_autolykos2Ctx && g_dagEpoch == epochIndex &&
        Autolykos2_DagSize(g_autolykos2Ctx) == dagBytes &&
        memcmp(g_dagSeed, seed32, 32) == 0) {
        pthread_mutex_unlock(&g_powCtxLock);
        return true;
    }

    Autolykos2Context* ctx = GetAutolykos2CtxLocked();
    g_dagReady = false; // the buffer is about to be invalid; no heavy hash may run against it

    // Generation is one Blake2b per 64 bytes, single-threaded, so a multi-GiB DAG is tens of
    // seconds. Say so rather than leaving the miner looking hung.
    printf("Generating the epoch %llu mining DAG (%zu MiB), this takes a moment...\n",
        (unsigned long long)epochIndex, dagBytes >> 20);
    fflush(stdout);

    Autolykos2_DagClear(ctx);
    const bool ok = Autolykos2_DagAllocate(ctx, dagBytes) && Autolykos2_DagGenerate(ctx, seed32);
    if (ok) {
        g_dagEpoch = epochIndex;
        memcpy(g_dagSeed, seed32, 32);
        g_dagReady = true;
    }

    pthread_mutex_unlock(&g_powCtxLock);
    return ok;
}

bool Block_PowHashHeavy(const block_t* block, uint64_t epochIndex, size_t dagBytes,
                        const uint8_t seed32[32], uint8_t outHash[32]) {
    if (!block || !seed32 || !outHash) {
        return false;
    }

    pthread_mutex_lock(&g_powCtxLock);
    // Verifying the SEED here, not just the epoch and size, is what makes this impossible to
    // misuse. A reorg changes the block an epoch's seed is derived from while the epoch index and
    // size stay put, so an epoch+size check alone happily accepts a DAG built from the pre-reorg
    // seed and returns a hash for the wrong lanes -- which shows up as a valid block failing PoW
    // while a branch is being applied. A mismatch yields false and the caller derives the lanes
    // from the seed instead.
    const bool usable = g_dagReady && g_autolykos2Ctx && g_dagEpoch == epochIndex &&
                        Autolykos2_DagSize(g_autolykos2Ctx) == dagBytes &&
                        memcmp(g_dagSeed, seed32, 32) == 0;
    const bool ok = usable &&
                    Autolykos2_Hash(
                        g_autolykos2Ctx,
                        (const uint8_t*)&block->header,
                        sizeof(block_header_t),
                        block->header.nonce,
                        block->header.blockNumber, // full 64-bit width; the light path takes uint64
                        outHash);
    pthread_mutex_unlock(&g_powCtxLock);
    return ok;
}

bool Block_PowHashLight(const block_t* block, size_t dagBytes, const uint8_t seed32[32], uint8_t outHash[32]) {
    if (!block || !seed32 || !outHash) {
        return false;
    }

    return Autolykos2_LightHashAtHeight(
        seed32,
        (const uint8_t*)&block->header,
        sizeof(block_header_t),
        block->header.nonce,
        block->header.blockNumber,
        dagBytes,
        outHash);
}

block_t* Block_Create() {
    block_t* block = (block_t*)malloc(sizeof(block_t));
    if (!block) {
        return NULL;
    }
    memset(&block->header, 0, sizeof(block_header_t));
    block->transactions = DYNARR_CREATE(signed_transaction_t, 1);
    if (!block->transactions) {
        free(block);
        return NULL;
    }
    
    // Zero out padding
    memset(block->header.reserved, 0, sizeof(block->header.reserved));

    return block;
}

void Block_CalculateHash(const block_t* block, uint8_t* outHash) {
    if (!block || !outHash) {
        return;
    }

    // Canonical block hash commits to header fields, including merkleRoot.
    SHA256((const unsigned char*)&block->header, sizeof(block_header_t), outHash);
    SHA256(outHash, 32, outHash); // Double-Hash
}

void Block_CalculateMerkleRoot(const block_t* block, uint8_t* outHash) {
    if (!block || !block->transactions || !outHash) {
        return;
    }

    const size_t txCount = DynArr_size(block->transactions);
    if (txCount == 0) {
        memset(outHash, 0, 32);
        return;
    }
    if (txCount == 1) {
        signed_transaction_t* tx = (signed_transaction_t*)DynArr_at(block->transactions, 0);
        Transaction_CalculateHash(tx, outHash);
        return;
    }

    uint8_t* current = (uint8_t*)malloc(txCount * 32u);
    uint8_t* next = (uint8_t*)malloc(txCount * 32u);
    if (!current || !next) {
        free(current);
        free(next);
        memset(outHash, 0, 32);
        return;
    }

    for (size_t i = 0; i < txCount; ++i) {
        signed_transaction_t* tx = (signed_transaction_t*)DynArr_at(block->transactions, i);
        if (!tx) {
            free(current);
            free(next);
            memset(outHash, 0, 32);
            return;
        }
        Transaction_CalculateHash(tx, current + (i * 32u));
    }

    size_t levelCount = txCount;
    while (levelCount > 1) {
        size_t nextCount = 0;
        for (size_t i = 0; i < levelCount; i += 2) {
            const uint8_t* left = current + (i * 32u);
            const uint8_t* right = (i + 1 < levelCount) ? current + ((i + 1) * 32u) : left;

            uint8_t dataInBuffer[64];
            memcpy(dataInBuffer, left, 32);
            memcpy(dataInBuffer + 32, right, 32);

            SHA256((const unsigned char*)dataInBuffer, 64, next + (nextCount * 32u));
            SHA256(next + (nextCount * 32u), 32, next + (nextCount * 32u));
            ++nextCount;
        }

        uint8_t* swap = current;
        current = next;
        next = swap;
        levelCount = nextCount;
    }

    memcpy(outHash, current, 32);
    free(current);
    free(next);
}

void Block_AddTransaction(block_t* block, signed_transaction_t* tx) {
    if (!block || !tx || !block->transactions) {
        return;
    }

    DynArr_push_back(block->transactions, tx);
}

void Block_RemoveTransaction(block_t* block, uint8_t* txHash) {
    if (!block || !txHash || !block->transactions) {
        return;
    }

    for (size_t i = 0; i < DynArr_size(block->transactions); i++) {
        signed_transaction_t* currentTx = (signed_transaction_t*)DynArr_at(block->transactions, i);
        uint8_t currentTxHash[32];
        Transaction_CalculateHash(currentTx, currentTxHash);
        if (memcmp(currentTxHash, txHash, 32) == 0) {
            DynArr_remove(block->transactions, i);
            return;
        }
    }
}

static int Uint256_CompareBE(const uint8_t a[32], const uint8_t b[32]) {
    for (int i = 0; i < 32; ++i) {
        if (a[i] < b[i]) return -1;
        if (a[i] > b[i]) return 1;
    }
    return 0;
}

bool Block_HasValidProofOfWorkWithParams(const block_t* block, uint64_t epochIndex,
                                         size_t dagBytes, const uint8_t seed32[32]) {
    if (!block) {
        return false;
    }

    uint8_t target[32];
    if (!DecodeCompactTarget(block->header.difficultyTarget, target)) {
        return false;
    }

    // Prefer the prebuilt DAG when it is provably the one for this block's epoch and size -- the
    // miner keeps it warm, and reading a lane beats recomputing it -- otherwise derive the lanes
    // from the epoch seed. The two produce identical hashes, so which one runs is invisible to
    // consensus; only speed differs.
    uint8_t hash[32];
    if (!Block_PowHashHeavy(block, epochIndex, dagBytes, seed32, hash) &&
        !Block_PowHashLight(block, dagBytes, seed32, hash)) {
        // Fail CLOSED. This used to hand back a zeroed hash on any failure and compare that to the
        // target -- and zero is below every target, so a DAG that was missing, mis-sized or failed
        // to build made the PoW check pass for every block instead of rejecting them.
        return false;
    }

    return Uint256_CompareBE(hash, target) <= 0;
}

bool Block_HasValidProofOfWork(const block_t* block, blockchain_t* chain) {
    if (!block || !chain) {
        return false;
    }

    size_t dagBytes = 0;
    uint8_t seed[32];
    if (!Chain_DagParamsForHeight(chain, block->header.blockNumber, &dagBytes, seed)) {
        return false;
    }

    const uint64_t epochIndex = block->header.blockNumber / (uint64_t)EPOCH_LENGTH;
    return Block_HasValidProofOfWorkWithParams(block, epochIndex, dagBytes, seed);
}

bool Block_HasValidVote(const block_t* block) {
    if (!block) {
        return false;
    }

    // Unrecognised vote values and non-zero spare bytes are rejected rather than ignored, so the
    // header has no bits whose meaning is undefined and nothing to grind for extra nonce space.
    return block->header.reserved[0] <= (uint8_t)DAG_VOTE_MAX &&
           block->header.reserved[1] == 0u &&
           block->header.reserved[2] == 0u;
}

bool Block_AllTransactionsValid(const block_t* block) {
    if (!block || !block->transactions) {
        return false;
    }

    bool hasCoinbase = false;

    for (size_t i = 0; i < DynArr_size(block->transactions); i++) {
        signed_transaction_t* tx = (signed_transaction_t*)DynArr_at(block->transactions, i);
        if (!Transaction_Verify(tx)) {
            return false;
        }

        if (tx && Address_IsCoinbase(tx->transaction.senderAddress)) {
            if (hasCoinbase) {
                return false;
            }

            hasCoinbase = true;
        }
    }

    return true && hasCoinbase && DynArr_size(block->transactions) > 0; // Every block must have at least one transaction (the coinbase)
}

bool Block_ValidateCoinbaseAndFees(const block_t* block, uint64_t expectedCoinbaseAmount, uint64_t* outTotalFees) {
    if (!block || !block->transactions) {
        return false;
    }

    bool hasCoinbase = false;
    uint64_t totalFees = 0;
    uint8_t zeroAddress[32] = {0};

    for (size_t i = 0; i < DynArr_size(block->transactions); ++i) {
        signed_transaction_t* tx = (signed_transaction_t*)DynArr_at(block->transactions, i);
        if (!tx) {
            return false;
        }

        if (Address_IsCoinbase(tx->transaction.senderAddress)) {
            if (hasCoinbase) {
                return false;
            }

            hasCoinbase = true;

            if (!Transaction_Verify(tx)) {
                return false;
            }

            if (tx->transaction.fee != 0 || tx->transaction.amount2 != 0) {
                return false;
            }

            if (tx->transaction.amount1 != expectedCoinbaseAmount) {
                return false;
            }

            if (Address_IsCoinbase(tx->transaction.recipientAddress1)) {
                return false;
            }

            if (memcmp(tx->transaction.recipientAddress2, zeroAddress, sizeof(zeroAddress)) != 0) {
                return false;
            }

            continue;
        }

        if (!Transaction_Verify(tx)) {
            return false;
        }

        if (UINT64_MAX - totalFees < tx->transaction.fee) {
            return false;
        }
        totalFees += tx->transaction.fee;
    }

    if (!hasCoinbase) {
        return false;
    }

    if (outTotalFees) {
        *outTotalFees = totalFees;
    }

    return true;
}

bool Block_HasValidStructure(const block_t* block) {
    if (!block || !block->transactions) {
        return false;
    }

    uint8_t calculatedMerkleRoot[32];
    Block_CalculateMerkleRoot(block, calculatedMerkleRoot);
    if (memcmp(calculatedMerkleRoot, block->header.merkleRoot, 32) != 0) {
        return false;
    }

    return Block_HasValidVote(block) &&
           Block_AllTransactionsValid(block) &&
           DynArr_size(block->transactions) > 0;
}

bool Block_IsFullyValid(const block_t* block, blockchain_t* chain) {
    return Block_HasValidStructure(block) && Block_HasValidProofOfWork(block, chain);
}

void Block_Destroy(block_t* block) {
    if (!block) return;
    DynArr_destroy(block->transactions);
    free(block);
}

void Block_Print(const block_t* block) {
    if (!block) return;

    printf("Block #%llu\n", (unsigned long long)block->header.blockNumber);
    printf("Timestamp: %llu\n", (unsigned long long)block->header.timestamp);
    printf("Nonce: %llu\n", (unsigned long long)block->header.nonce);
    printf("Difficulty Target: 0x%08x\n", block->header.difficultyTarget);
    printf("Version: %u\n", block->header.version);
    printf("Previous Hash: ");
    for (size_t i = 0; i < 32; i++) {
        printf("%02x", block->header.prevHash[i]);
    }
    printf("\n");
    printf("Merkle Root: ");
    for (size_t i = 0; i < 32; i++) {
        printf("%02x", block->header.merkleRoot[i]);
    }
    printf("\n");
    if (block->transactions) {
        printf("Transactions (%zu):\n", DynArr_size(block->transactions));
        for (size_t i = 0; i < DynArr_size(block->transactions); i++) {
            signed_transaction_t* tx = (signed_transaction_t*)DynArr_at(block->transactions, i);
            if (tx) {
                printf("  Tx #%zu: 1: %llu -> %02x%02x...%02x%02x, fee %llu\n           2: %llu -> %02x%02x...%02x%02x, fee %llu\n", 
                    i,
                    (unsigned long long)tx->transaction.amount1,
                    tx->transaction.recipientAddress1[0], tx->transaction.recipientAddress1[1], tx->transaction.recipientAddress1[30], tx->transaction.recipientAddress1[31],
                    (unsigned long long)tx->transaction.fee,
                    (unsigned long long)tx->transaction.amount2,
                    tx->transaction.recipientAddress2[0], tx->transaction.recipientAddress2[1], tx->transaction.recipientAddress2[30], tx->transaction.recipientAddress2[31],
                    (unsigned long long)tx->transaction.fee);
            }
        }
    } else {
        printf("No transactions (or none loaded)\n");
    }
}

void Block_ShortPrint(const block_t* block) {
    if (!block) return;

    printf("Block #%llu: Timestamp %llu, Nonce %llu, DiffTarget 0x%08x, Version %u, PrevHash %02x%02x...%02x%02x, MerkleRoot %02x%02x...%02x%02x, TxCount %zu\n",
        (unsigned long long)block->header.blockNumber,
        (unsigned long long)block->header.timestamp,
        (unsigned long long)block->header.nonce,
        block->header.difficultyTarget,
        block->header.version,
        block->header.prevHash[0], block->header.prevHash[1], block->header.prevHash[30], block->header.prevHash[31],
        block->header.merkleRoot[0], block->header.merkleRoot[1], block->header.merkleRoot[30], block->header.merkleRoot[31],
        block->transactions ? DynArr_size(block->transactions) : 0);
}

block_t* Block_Copy(const block_t* src) {
    if (!src) return NULL;
    block_t* dst = (block_t*)malloc(sizeof(block_t));
    if (!dst) return NULL;
    dst->header = src->header;
    if (src->transactions) {
        size_t txCount = DynArr_size(src->transactions);
        dst->transactions = DYNARR_CREATE(signed_transaction_t, txCount == 0 ? 1 : txCount);
        if (!dst->transactions) {
            free(dst);
            return NULL;
        }
        for (size_t i = 0; i < txCount; ++i) {
            signed_transaction_t* tx = (signed_transaction_t*)DynArr_at(src->transactions, i);
            if (!tx) {
                DynArr_destroy(dst->transactions);
                free(dst);
                return NULL;
            }
            if (!DynArr_push_back(dst->transactions, tx)) {
                DynArr_destroy(dst->transactions);
                free(dst);
                return NULL;
            }
        }
    } else {
        dst->transactions = NULL;
    }
    return dst;
}
