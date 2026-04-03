#include <block/block.h>
#include <autolykos2/autolykos2.h>
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

    // TODO: Make this not shit
    DynArr* hashes1 = DynArr_create(sizeof(uint8_t) * 32, 1);
    DynArr* hashes2 = DynArr_create(sizeof(uint8_t) * 32, 1);
    if (!hashes1 || !hashes2) {
        if (hashes1) DynArr_destroy(hashes1);
        if (hashes2) DynArr_destroy(hashes2);
        return;
    }

    // Handle the transactions
    for (size_t i = 0; i < txCount - 1; i++) {
        signed_transaction_t* tx = (signed_transaction_t*)DynArr_at(block->transactions, i);
        signed_transaction_t* txNext = (signed_transaction_t*)DynArr_at(block->transactions, i + 1);
        uint8_t buf1[32] = {0}; uint8_t buf2[32] = {0}; // Zeroed out

        // Unless if by some miracle the hash just so happens to be all zeros,
        // I think we can safely assume that a 1 : 2^256 chance will NEVER be hit
        Transaction_CalculateHash(tx, buf1);
        Transaction_CalculateHash(txNext, buf2);

        // Concat the two hashes
        uint8_t dataInBuffer[64] = {0};
        uint8_t* nextStart = dataInBuffer;
        nextStart += 32;
        memcpy(dataInBuffer, buf1, 32);
        if (txNext) { memcpy(nextStart, buf2, 32); }

        // Double hash that tx set
        uint8_t outHash[32];
        SHA256((const unsigned char*)dataInBuffer, 64, outHash);
        SHA256(outHash, 32, outHash);

        // Copy to the hashes dynarr
        DynArr_push_back(hashes1, outHash);
    }

    // Move to hashing the existing ones until only one remains
    do {
        for (size_t i = 0; i < DynArr_size(hashes1) - 1; i++) {
            uint8_t* hash1 = (uint8_t*)DynArr_at(hashes1, i); uint8_t* hash2 = (uint8_t*)DynArr_at(hashes1, i + 1);

            // Concat the two hashes
            uint8_t dataInBuffer[64] = {0};
            uint8_t* nextStart = dataInBuffer;
            nextStart += 32;
            memcpy(dataInBuffer, hash1, 32);
            memcpy(nextStart, hash2, 32);

            // Double hash that tx set
            uint8_t outHash[32];
            SHA256((const unsigned char*)dataInBuffer, 64, outHash);
            SHA256(outHash, 32, outHash);

            DynArr_push_back(hashes2, outHash);
        }

        DynArr_erase(hashes1);
        for (size_t i = 0; i < DynArr_size(hashes2); i++) {
            DynArr_push_back(hashes1, (uint8_t*)DynArr_at(hashes2, i));
        }
        DynArr_erase(hashes2);
    } while (DynArr_size(hashes1) > 1);

    // Final Merkle
    uint8_t* merkle = (uint8_t*)DynArr_at(hashes1, 0);
    if (merkle) {
        memcpy(outHash, merkle, 32);
    } else {
        memset(outHash, 0, 32);
    }

    DynArr_destroy(hashes1);
    DynArr_destroy(hashes2);
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
        if (memcmp(currentTx->signature.txHash, txHash, 32) == 0) {
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

static bool DecodeCompactTarget(uint32_t nBits, uint8_t target[32]) {
    memset(target, 0, 32);

    uint32_t exponent = nBits >> 24;
    uint32_t mantissa = nBits & 0x007fffff;  // ignore sign bit for now
    bool negative = (nBits & 0x00800000) != 0;

    if (negative || mantissa == 0) {
        return false;
    }

    // Compute: target = mantissa * 256^(exponent - 3)
    if (exponent <= 3) {
        mantissa >>= 8 * (3 - exponent);

        target[29] = (mantissa >> 16) & 0xff;
        target[30] = (mantissa >> 8) & 0xff;
        target[31] = mantissa & 0xff;
    } else {
        uint32_t byte_index = exponent - 3;  // number of zero-bytes appended on right

        if (byte_index > 29) {
            return false; // overflow 256 bits
        }

        target[32 - byte_index - 3] = (mantissa >> 16) & 0xff;
        target[32 - byte_index - 2] = (mantissa >> 8) & 0xff;
        target[32 - byte_index - 1] = mantissa & 0xff;
    }

    return true;
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
