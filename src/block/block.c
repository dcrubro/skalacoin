#include <block/block.h>
#include <stdlib.h>

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
    return block;
}

void Block_CalculateHash(const block_t* block, uint8_t* outHash) {
    if (!block || !outHash || !block->transactions || DynArr_size(block->transactions) <= 0) {
        return;
    }

    // Merkle root TODO
    
    // Flatten the block header and transactions into a single buffer for hashing (assume that txs are verified - usually on receive)
    uint8_t buffer[sizeof(block_header_t) + (DynArr_size(block->transactions) * DynArr_elemSize(block->transactions))];
    memcpy(buffer, &block->header, sizeof(block_header_t));
    for (size_t i = 0; i < DynArr_size(block->transactions); i++) {
        void* txPtr = (char*)DynArr_at(block->transactions, i);
        memcpy(buffer + sizeof(block_header_t) + (i * DynArr_elemSize(block->transactions)), txPtr, DynArr_elemSize(block->transactions));
    }

    SHA256((const unsigned char*)buffer, sizeof(buffer), outHash);
    SHA256(outHash, 32, outHash); // Double-Hash
}

void Block_CalculateRandomXHash(const block_t* block, uint8_t* outHash) {
    if (!block || !outHash || !block->transactions || DynArr_size(block->transactions) <= 0) {
        return;
    }

    // Merkle root TODO
    
    // Flatten the block header and transactions into a single buffer for hashing (assume that txs are verified - usually on receive)
    uint8_t buffer[sizeof(block_header_t) + (DynArr_size(block->transactions) * DynArr_elemSize(block->transactions))];
    memcpy(buffer, &block->header, sizeof(block_header_t));
    for (size_t i = 0; i < DynArr_size(block->transactions); i++) {
        void* txPtr = (char*)DynArr_at(block->transactions, i);
        memcpy(buffer + sizeof(block_header_t) + (i * DynArr_elemSize(block->transactions)), txPtr, DynArr_elemSize(block->transactions));
    }

    RandomX_CalculateHash(buffer, sizeof(buffer), outHash);
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
    Block_CalculateRandomXHash(block, hash);

    return Uint256_CompareBE(hash, target) <= 0;
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

        if (Address_IsCoinbase(tx->transaction.senderAddress)) {
            if (hasCoinbase) {
                return false; // More than one coinbase transaction
            }

            hasCoinbase = true;
        }
    }

    return true && hasCoinbase && DynArr_size(block->transactions) > 0; // Every block must have at least one transaction (the coinbase)
}

void Block_Destroy(block_t* block) {
    if (!block) return;
    DynArr_destroy(block->transactions);
    free(block);
}
