#include <block/chain.h>
#include <block/transaction.h>
#include <openssl/sha.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>

#include <constants.h>

#ifndef CHAIN_DATA_DIR
#define CHAIN_DATA_DIR "chain_data"
#endif

void handle_sigint(int sig) {
    printf("Caught signal %d, exiting...\n", sig);
    Block_ShutdownPowContext();
    exit(0);
}

uint32_t difficultyTarget = INITIAL_DIFFICULTY;

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
    const uint64_t blocksToMine = 1000;
    const double targetSeconds = TARGET_BLOCK_TIME;

    uint256_t currentSupply = uint256_from_u64(0);

    blockchain_t* chain = Chain_Create();
    if (!chain) {
        fprintf(stderr, "failed to create chain\n");
        return 1;
    }

    if (!Chain_LoadFromFile(chain, chainDataDir, &currentSupply, &difficultyTarget)) {
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

        uint8_t minerAddress[32];
        SHA256((const unsigned char*)"minicoin-miner-1", strlen("minicoin-miner-1"), minerAddress);

        for (uint64_t mined = 0; mined < blocksToMine; ++mined) {
            block_t* block = Block_Create();
            if (!block) {
                fprintf(stderr, "failed to create block\n");
                Chain_Destroy(chain);
                Block_ShutdownPowContext();
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
            block->header.difficultyTarget = difficultyTarget;
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
                Block_ShutdownPowContext();
                return 1;
            }

            if (!Chain_AddBlock(chain, block)) {
                fprintf(stderr, "failed to append block to chain\n");
                Block_Destroy(block);
                Chain_Destroy(chain);
                Block_ShutdownPowContext();
                return 1;
            }

            (void)uint256_add_u64(&currentSupply, coinbaseTx.transaction.amount);

            uint8_t canonicalHash[32];
            uint8_t powHash[32];
            Block_CalculateHash(block, canonicalHash);
            Block_CalculateAutolykos2Hash(block,     powHash);
            printf("Mined block %llu/%llu (height=%llu) nonce=%llu reward=%llu supply=%llu diff=%#x merkle=%02x%02x%02x%02x... pow=%02x%02x%02x%02x... canonical=%02x%02x%02x%02x...\n",
                (unsigned long long)(mined + 1),
                (unsigned long long)blocksToMine,
                (unsigned long long)block->header.blockNumber,
                (unsigned long long)block->header.nonce,
                (unsigned long long)coinbaseTx.transaction.amount,
                (unsigned long long)currentSupply.limbs[0],
                (unsigned int)block->header.difficultyTarget,
                block->header.merkleRoot[0], block->header.merkleRoot[1], block->header.merkleRoot[2], block->header.merkleRoot[3],
                powHash[0], powHash[1], powHash[2], powHash[3],
                canonicalHash[0], canonicalHash[1], canonicalHash[2], canonicalHash[3]);

            free(block); // chain stores blocks by value; transactions are owned by chain copy

            // Save chain after each mined block
            Chain_SaveToFile(chain, chainDataDir, currentSupply);

            if (Chain_Size(chain) % DIFFICULTY_ADJUSTMENT_INTERVAL == 0) {
                difficultyTarget = Chain_ComputeNextTarget(chain, difficultyTarget);
            }
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
    Block_ShutdownPowContext();
    return 0;
}
