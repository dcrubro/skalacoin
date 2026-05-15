#include <block/block.h>
#include <autolykos2/autolykos2.h>
#include <utils.h>
#include <stdlib.h>

static Autolykos2Context* g_autolykos2Ctx = NULL;

static Autolykos2Context* GetAutolykos2Ctx(void) {
    if (!g_autolykos2Ctx) {
        g_autolykos2Ctx = Autolykos2_Create();
        if (!g_autolykos2Ctx) {
            fprintf(stderr, "Failed to create Autolykos2 context\n");
            exit(1);
        }
        Autolykos2_DagAllocate(g_autolykos2Ctx, DAG_BASE_SIZE);
    }
    return g_autolykos2Ctx;
}

void Block_ShutdownPowContext(void) {
    if (g_autolykos2Ctx) {
        Autolykos2_Destroy(g_autolykos2Ctx);
        g_autolykos2Ctx = NULL;
    }
}

bool Block_RebuildAutolykos2Dag(size_t dagBytes, const uint8_t seed32[32]) {
    if (!seed32 || dagBytes == 0) {
        return false;
    }

    Autolykos2Context* ctx = GetAutolykos2Ctx();
    if (!ctx) {
        return false;
    }

    Autolykos2_DagClear(ctx);
    if (!Autolykos2_DagAllocate(ctx, dagBytes)) {
        return false;
    }

    return Autolykos2_DagGenerate(ctx, seed32);
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

void Block_CalculateAutolykos2Hash(const block_t* block, uint8_t* outHash) {
    if (!block || !outHash) {
        return;
    }

    // PoW hash is computed from the block header, while canonical block hash remains SHA256.
    Autolykos2Context* ctx = GetAutolykos2Ctx();
    if (!ctx) {
        memset(outHash, 0, 32);
        return;
    }

    if (!Autolykos2_Hash(
        ctx,
        (const uint8_t*)&block->header,
        sizeof(block_header_t),
        block->header.nonce,
        (uint32_t)block->header.blockNumber,
        outHash
    )) {
        memset(outHash, 0, 32);
    }
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

bool Block_HasValidProofOfWork(const block_t* block) {
    if (!block) {
        return false;
    }

    uint8_t target[32];
    if (!DecodeCompactTarget(block->header.difficultyTarget, target)) {
        return false;
    }

    uint8_t hash[32];
    Block_CalculateAutolykos2Hash(block, hash);

    return Uint256_CompareBE(hash, target) <= 0;
}

bool Block_AllTransactionsValid(const block_t* block) {
    if (!block || !block->transactions) {
        return false;
    }

    bool hasCoinbase = false;

    for (size_t i = 0; i < DynArr_size(block->transactions); i++) {
        signed_transaction_t* tx = (signed_transaction_t*)DynArr_at(block->transactions, i);
        if (tx && Address_IsCoinbase(tx->transaction.senderAddress)) {
            if (hasCoinbase) {
                return false; // More than one coinbase transaction
            }

            hasCoinbase = true;
            continue; // Coinbase transactions are valid since the miner has the right to create coins. Only rule is one per block.
        }
        
        if (!Transaction_Verify(tx)) {
            return false;
        }
    }

    return true && hasCoinbase && DynArr_size(block->transactions) > 0; // Every block must have at least one transaction (the coinbase)
}

bool Block_IsFullyValid(const block_t* block) {
    bool merkleValid = false;
    uint8_t calculatedMerkleRoot[32];
    if (block && block->transactions) {
        Block_CalculateMerkleRoot(block, calculatedMerkleRoot);
        merkleValid = (memcmp(calculatedMerkleRoot, block->header.merkleRoot, 32) == 0);
    }

    return Block_HasValidProofOfWork(block) && Block_AllTransactionsValid(block) && DynArr_size(block->transactions) > 0 && merkleValid;
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
