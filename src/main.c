#include <block/chain.h>
#include <block/transaction.h>
#include <openssl/sha.h>
#include <secp256k1.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <randomx/librx_wrapper.h>
#include <signal.h>

void handle_sigint(int sig) {
    printf("Caught signal %d, exiting...\n", sig);
    RandomX_Destroy();
    exit(0);
}

static double MonotonicSeconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ((double)ts.tv_nsec / 1000000000.0);
}

static double MeasureRandomXHashrate(void) {
    uint8_t input[80] = {0};
    uint8_t outHash[32];
    uint64_t counter = 0;

    const double start = MonotonicSeconds();
    double now = start;
    do {
        memcpy(input, &counter, sizeof(counter));
        RandomX_CalculateHash(input, sizeof(input), outHash);
        counter++;
        now = MonotonicSeconds();
    } while ((now - start) < 0.75); // short local benchmark window

    const double elapsed = now - start;
    if (elapsed <= 0.0 || counter == 0) {
        return 0.0;
    }

    return (double)counter / elapsed;
}

static uint32_t CompactTargetForExpectedHashes(double expectedHashes) {
    if (expectedHashes < 1.0) {
        expectedHashes = 1.0;
    }

    // For exponent 0x1f: target = mantissa * 2^(8*(0x1f-3)) = mantissa * 2^224
    // So expected hashes ~= 2^256 / target = 2^32 / mantissa.
    double mantissaF = 4294967296.0 / expectedHashes;
    if (mantissaF < 1.0) {
        mantissaF = 1.0;
    }
    if (mantissaF > 8388607.0) {
        mantissaF = 8388607.0; // 0x007fffff
    }

    const uint32_t mantissa = (uint32_t)mantissaF;
    return (0x1fU << 24) | (mantissa & 0x007fffffU);
}

static bool GenerateKeypair(
    const secp256k1_context* ctx,
    uint8_t outPrivateKey[32],
    uint8_t outCompressedPublicKey[33]
) {
    if (!ctx || !outPrivateKey || !outCompressedPublicKey) {
        return false;
    }

    secp256k1_pubkey pubkey;
    for (size_t i = 0; i < 1024; ++i) {
        arc4random_buf(outPrivateKey, 32);
        if (!secp256k1_ec_seckey_verify(ctx, outPrivateKey)) {
            continue;
        }

        if (!secp256k1_ec_pubkey_create(ctx, &pubkey, outPrivateKey)) {
            continue;
        }

        size_t serializedLen = 33;
        if (!secp256k1_ec_pubkey_serialize(
            ctx,
            outCompressedPublicKey,
            &serializedLen,
            &pubkey,
            SECP256K1_EC_COMPRESSED
        )) {
            continue;
        }

        return serializedLen == 33;
    }

    return false;
}

static bool MineBlock(block_t* block) {
    if (!block) {
        return false;
    }

    for (uint64_t nonce = 0;; ++nonce) {
        block->header.nonce = nonce;
        if (Block_HasValidProofOfWork(block)) {
            return true;
        }

        if (nonce == UINT64_MAX) {
            return false;
        }
    }
}

int main(void) {
    signal(SIGINT, handle_sigint);

    // Init RandomX
    if (!RandomX_Init("minicoin")) { // TODO: Use a key that is not hardcoded; E.g. hash of the last block, every thousand blocks, etc.
        fprintf(stderr, "failed to initialize RandomX\n");
        return 1;
    }

    blockchain_t* chain = Chain_Create();
    if (!chain) {
        fprintf(stderr, "failed to create chain\n");
        return 1;
    }

    secp256k1_context* secpCtx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
    if (!secpCtx) {
        fprintf(stderr, "failed to create secp256k1 context\n");
        Chain_Destroy(chain);
        return 1;
    }

    uint8_t senderPrivateKey[32];
    uint8_t receiverPrivateKey[32];
    uint8_t senderCompressedPublicKey[33];
    uint8_t receiverCompressedPublicKey[33];

    if (!GenerateKeypair(secpCtx, senderPrivateKey, senderCompressedPublicKey) ||
        !GenerateKeypair(secpCtx, receiverPrivateKey, receiverCompressedPublicKey)) {
        fprintf(stderr, "failed to generate keypairs\n");
        secp256k1_context_destroy(secpCtx);
        Chain_Destroy(chain);
        return 1;
    }

    signed_transaction_t tx;
    memset(&tx, 0, sizeof(tx));
    tx.transaction.version = 1;
    tx.transaction.amount = 100;
    tx.transaction.fee = 1;

    SHA256(senderCompressedPublicKey, 33, tx.transaction.senderAddress);
    SHA256(receiverCompressedPublicKey, 33, tx.transaction.recipientAddress);
    memcpy(tx.transaction.compressedPublicKey, senderCompressedPublicKey, 33);

    Transaction_Sign(&tx, senderPrivateKey);
    if (!Transaction_Verify(&tx)) {
        fprintf(stderr, "signed transaction did not verify\n");
        secp256k1_context_destroy(secpCtx);
        Chain_Destroy(chain);
        RandomX_Destroy();
        return 1;
    }

    block_t* block = Block_Create();
    if (!block) {
        fprintf(stderr, "failed to create block\n");
        secp256k1_context_destroy(secpCtx);
        Chain_Destroy(chain);
        RandomX_Destroy();
        return 1;
    }

    block->header.version = 1;
    block->header.blockNumber = (uint32_t)Chain_Size(chain);
    memset(block->header.prevHash, 0, sizeof(block->header.prevHash));
    memset(block->header.merkleRoot, 0, sizeof(block->header.merkleRoot));
    block->header.timestamp = (uint64_t)time(NULL);

        const double hps = MeasureRandomXHashrate();
        const double targetSeconds = 60.0;
        const double expectedHashes = (hps > 0.0) ? (hps * targetSeconds) : 65536.0;
        block->header.difficultyTarget = CompactTargetForExpectedHashes(expectedHashes);
    block->header.nonce = 0;

        printf("RandomX benchmark: %.2f H/s, target %.0fs, nBits=0x%08x\n",
            hps,
            targetSeconds,
            block->header.difficultyTarget);

    Block_AddTransaction(block, &tx);
    printf("Added transaction to block: sender %02x... -> recipient %02x..., amount %lu, fee %lu\n",
           tx.transaction.senderAddress[0], tx.transaction.senderAddress[31],
           tx.transaction.recipientAddress[0], tx.transaction.recipientAddress[31],
           tx.transaction.amount, tx.transaction.fee);

    if (!MineBlock(block)) {
        fprintf(stderr, "failed to mine block within nonce range\n");
        Block_Destroy(block);
        secp256k1_context_destroy(secpCtx);
        Chain_Destroy(chain);
        RandomX_Destroy();
        return 1;
    }

    if (!Chain_AddBlock(chain, block)) {
        fprintf(stderr, "failed to append block to chain\n");
        Block_Destroy(block);
        secp256k1_context_destroy(secpCtx);
        Chain_Destroy(chain);
        RandomX_Destroy();
        return 1;
    }

        printf("Mined block %u with nonce %llu and chain size %zu\n",
           block->header.blockNumber,
            (unsigned long long)block->header.nonce,
           Chain_Size(chain));

    printf("Block hash (SHA256): ");
    uint8_t blockHash[32];
    Block_CalculateHash(block, blockHash);
    for (size_t i = 0; i < 32; ++i) {
        printf("%02x", blockHash[i]);
    }
    printf("\nBlock hash (RandomX): ");
    uint8_t randomXHash[32];
    Block_CalculateRandomXHash(block, randomXHash);
    for (size_t i = 0; i < 32; ++i) {
        printf("%02x", randomXHash[i]);
    }
    printf("\n");

    // Chain currently stores a copy of block_t that references the same tx array pointer,
    // so we do not destroy `block` here to avoid invalidating chain data.
    secp256k1_context_destroy(secpCtx);

    Chain_Destroy(chain);
    return 0;
}
