#include <block/chain.h>
#include <block/transaction.h>
#include <openssl/sha.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <randomx/librx_wrapper.h>
#include <signal.h>

#include <constants.h>

#ifndef CHAIN_DATA_DIR
#define CHAIN_DATA_DIR "chain_data"
#endif

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

int main(int argc, char* argv[]) {
    signal(SIGINT, handle_sigint);

    const char* chainDataDir = CHAIN_DATA_DIR;
    const uint64_t blocksToMine = 10;
    const double targetSeconds = TARGET_BLOCK_TIME;

    uint256_t currentSupply = uint256_from_u64(0);

    // Init RandomX
    if (!RandomX_Init("minicoin", false)) { // TODO: Use a key that is not hardcoded; E.g. hash of the last block, every thousand blocks, difficulty recalibration, etc.
        fprintf(stderr, "failed to initialize RandomX\n");
        return 1;
    }

    blockchain_t* chain = Chain_Create();
    if (!chain) {
        fprintf(stderr, "failed to create chain\n");
        return 1;
    }

    if (!Chain_LoadFromFile(chain, chainDataDir, &currentSupply)) {
        printf("No existing chain loaded from %s\n", chainDataDir);
    }

    if (Chain_Size(chain) > 0) {
        if (Chain_IsValid(chain)) {
            printf("Loaded chain with %zu blocks from disk\n", Chain_Size(chain));
        } else {
            fprintf(stderr, "loaded chain is invalid, scrapping, resyncing.\n"); // TODO: Actually implement resyncing from peers instead of just scrapping the chain
            const size_t badSize = Chain_Size(chain);

            // Delete files (wipe dir)
            for (size_t i = 0; i < badSize; i++) {
                char filePath[256];
                snprintf(filePath, sizeof(filePath), "%s/block_%zu.dat", chainDataDir, i);
                remove(filePath);
            }

            char metaPath[256];
            snprintf(metaPath, sizeof(metaPath), "%s/chain.meta", chainDataDir);
            remove(metaPath);

            Chain_Wipe(chain);
        }
    }

    // Get flag from argv "-mine" to mine blocks
    if (argc > 1 && strcmp(argv[1], "-mine") == 0) {
        printf("Mining %llu blocks with target time %.0fs...\n", (unsigned long long)blocksToMine, targetSeconds);

        const double hps = MeasureRandomXHashrate();
        const double expectedHashes = (hps > 0.0) ? (hps * targetSeconds) : 65536.0;
        const uint32_t calibratedBits = CompactTargetForExpectedHashes(expectedHashes);
        //const uint32_t calibratedBits = 0xffffffff; // Absurdly low diff for testing

        printf("RandomX benchmark: %.2f H/s, target %.0fs, nBits=0x%08x, diff=%.2f\n",
            hps,
            targetSeconds,
            calibratedBits,
            (double)(1.f / calibratedBits) * 4294967296.0); // Basic representation as a big number for now

        uint8_t minerAddress[32];
        SHA256((const unsigned char*)"minicoin-miner-1", strlen("minicoin-miner-1"), minerAddress);

        for (uint64_t mined = 0; mined < blocksToMine; ++mined) {
            block_t* block = Block_Create();
            if (!block) {
                fprintf(stderr, "failed to create block\n");
                Chain_Destroy(chain);
                RandomX_Destroy();
                return 1;
            }

            block->header.version = 1;
            block->header.blockNumber = (uint64_t)Chain_Size(chain);
            if (Chain_Size(chain) > 0) {
                block_t* lastBlock = Chain_GetBlock(chain, Chain_Size(chain) - 1);
                if (lastBlock) {
                    Block_CalculateHash(lastBlock, block->header.prevHash);
                } else {
                    memset(block->header.prevHash, 0, sizeof(block->header.prevHash));
                }
            } else {
                memset(block->header.prevHash, 0, sizeof(block->header.prevHash));
            }
            block->header.timestamp = (uint64_t)time(NULL);
            block->header.difficultyTarget = calibratedBits;
            block->header.nonce = 0;

            signed_transaction_t coinbaseTx;
            memset(&coinbaseTx, 0, sizeof(coinbaseTx));
            coinbaseTx.transaction.version = 1;
            coinbaseTx.transaction.amount = CalculateBlockReward(currentSupply, block->header.blockNumber);
            coinbaseTx.transaction.fee = 0;
            memcpy(coinbaseTx.transaction.recipientAddress, minerAddress, sizeof(minerAddress));
            memset(coinbaseTx.transaction.compressedPublicKey, 0, sizeof(coinbaseTx.transaction.compressedPublicKey));
            memset(coinbaseTx.transaction.senderAddress, 0xFF, sizeof(coinbaseTx.transaction.senderAddress));
            Block_AddTransaction(block, &coinbaseTx);

            uint8_t merkleRoot[32];
            Block_CalculateMerkleRoot(block, merkleRoot);
            memcpy(block->header.merkleRoot, merkleRoot, sizeof(block->header.merkleRoot));

            if (!MineBlock(block)) {
                fprintf(stderr, "failed to mine block within nonce range\n");
                Block_Destroy(block);
                Chain_Destroy(chain);
                RandomX_Destroy();
                return 1;
            }

            if (!Chain_AddBlock(chain, block)) {
                fprintf(stderr, "failed to append block to chain\n");
                Block_Destroy(block);
                Chain_Destroy(chain);
                RandomX_Destroy();
                return 1;
            }

            (void)uint256_add_u64(&currentSupply, coinbaseTx.transaction.amount);

            uint8_t blockHash[32];
            Block_CalculateHash(block, blockHash);
            printf("Mined block %llu/%llu (height=%llu) nonce=%llu reward=%llu supply=%llu merkle=%02x%02x%02x%02x... hash=%02x%02x%02x%02x...\n",
                (unsigned long long)(mined + 1),
                (unsigned long long)blocksToMine,
                (unsigned long long)block->header.blockNumber,
                (unsigned long long)block->header.nonce,
                (unsigned long long)coinbaseTx.transaction.amount,
                (unsigned long long)currentSupply.limbs[0],
                block->header.merkleRoot[0], block->header.merkleRoot[1], block->header.merkleRoot[2], block->header.merkleRoot[3],
                blockHash[0], blockHash[1], blockHash[2], blockHash[3]);

            free(block); // chain stores blocks by value; transactions are owned by chain copy

            // Save chain after each mined block
            Chain_SaveToFile(chain, chainDataDir, currentSupply);
        }

        if (!Chain_SaveToFile(chain, chainDataDir, currentSupply)) {
            fprintf(stderr, "failed to save chain to %s\n", chainDataDir);
        } else {
            printf("Saved chain with %zu blocks to %s (supply=%llu)\n",
                Chain_Size(chain),
                chainDataDir,
                (unsigned long long)currentSupply.limbs[0]);
        }
    } else {
        printf("Current chain has %zu blocks, total supply %llu\n", Chain_Size(chain), (unsigned long long)currentSupply.limbs[0]);
    }

    // Print chain
    for (size_t i = 0; i < Chain_Size(chain); i++) {
        block_t* blk = Chain_GetBlock(chain, i);
        if (blk) {
            Block_Print(blk);
        }
    }

    Chain_Destroy(chain);
    RandomX_Destroy();
    return 0;
}
