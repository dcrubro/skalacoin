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
#include <balance_sheet.h>

#include <constants.h>
#include <autolykos2/autolykos2.h>
#include <time.h>

#ifndef CHAIN_DATA_DIR
#define CHAIN_DATA_DIR "chain_data"
#endif

void handle_sigint(int sig) {
    printf("Caught signal %d, exiting...\n", sig);
    Block_ShutdownPowContext();
    BalanceSheet_Destroy();
    exit(0);
}

uint32_t difficultyTarget = INITIAL_DIFFICULTY;

// extern the currentReward from constants.h so we can update it as we mine blocks and save it to disk
extern uint64_t currentReward;

static void AddressFromCompressedPubkey(const uint8_t compressedPubkey[33], uint8_t outAddress[32]) {
    if (!compressedPubkey || !outAddress) {
        return;
    }

    SHA256(compressedPubkey, 33, outAddress);
}

static bool GenerateTestMinerIdentity(uint8_t privateKey[32], uint8_t compressedPubkey[33], uint8_t address[32]) {
    if (!privateKey || !compressedPubkey || !address) {
        return false;
    }

    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    if (!ctx) {
        return false;
    }

    uint8_t seed[64];
    secp256k1_pubkey pubkey;

    for (uint64_t counter = 0; counter < 1024; ++counter) {
        const char* base = "skalacoin-test-miner-key";
        size_t baseLen = strlen(base);
        memcpy(seed, base, baseLen);
        memcpy(seed + baseLen, &counter, sizeof(counter));
        SHA256(seed, baseLen + sizeof(counter), privateKey);

        if (!secp256k1_ec_seckey_verify(ctx, privateKey)) {
            continue;
        }

        if (!secp256k1_ec_pubkey_create(ctx, &pubkey, privateKey)) {
            continue;
        }

        size_t pubLen = 33;
        if (!secp256k1_ec_pubkey_serialize(ctx, compressedPubkey, &pubLen, &pubkey, SECP256K1_EC_COMPRESSED) || pubLen != 33) {
            continue;
        }

        AddressFromCompressedPubkey(compressedPubkey, address);
        secp256k1_context_destroy(ctx);
        return true;
    }

    secp256k1_context_destroy(ctx);
    return false;
}

static int testCounts = 0;
static void MakeTestRecipientAddress(uint8_t outAddress[32]) {
    if (!outAddress) {
        return;
    }

    const char* label = "skalacoin-test-recipient-address-";
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "%s%d", label, testCounts);
    SHA256((const unsigned char*)buffer, strlen(buffer), outAddress);
    testCounts++;
}

static void Uint256ToDecimal(const uint256_t* value, char* out, size_t outSize) {
    if (!value || !out || outSize == 0) {
        return;
    }

    uint64_t tmp[4] = {
        value->limbs[0],
        value->limbs[1],
        value->limbs[2],
        value->limbs[3]
    };

    if (tmp[0] == 0 && tmp[1] == 0 && tmp[2] == 0 && tmp[3] == 0) {
        if (outSize >= 2) {
            out[0] = '0';
            out[1] = '\0';
        } else {
            out[0] = '\0';
        }
        return;
    }

    char digits[80];
    size_t digitCount = 0;

    while (tmp[0] != 0 || tmp[1] != 0 || tmp[2] != 0 || tmp[3] != 0) {
        uint64_t remainder = 0;
        for (int i = 3; i >= 0; --i) {
            __uint128_t cur = ((__uint128_t)remainder << 64) | tmp[i];
            tmp[i] = (uint64_t)(cur / 10u);
            remainder = (uint64_t)(cur % 10u);
        }

        if (digitCount < sizeof(digits) - 1) {
            digits[digitCount++] = (char)('0' + remainder);
        } else {
            break;
        }
    }

    size_t writeLen = (digitCount < (outSize - 1)) ? digitCount : (outSize - 1);
    for (size_t i = 0; i < writeLen; ++i) {
        out[i] = digits[digitCount - 1 - i];
    }
    out[writeLen] = '\0';
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
    srand((unsigned int)time(NULL));

    BalanceSheet_Init();
    const char* chainDataDir = CHAIN_DATA_DIR;
    const uint64_t blocksToMine = 1000;
    const double targetSeconds = TARGET_BLOCK_TIME;

    uint256_t currentSupply = uint256_from_u64(0);

    blockchain_t* chain = Chain_Create();
    if (!chain) {
        fprintf(stderr, "failed to create chain\n");
        return 1;
    }

    uint8_t lastSavedHash[32];
    bool isFirstBlockOfLoadedChain = true;

    if (!Chain_LoadFromFile(chain, chainDataDir, &currentSupply, &difficultyTarget, &currentReward, lastSavedHash)) {
        printf("No existing chain loaded from %s\n", chainDataDir);
    }

    if (!BalanceSheet_LoadFromFile(chainDataDir)) {
        printf("Failed to load the balance sheet or none existing\n");
    }

    const uint64_t effectivePhase1Blocks =
        (PHASE1_TARGET_BLOCKS / EMISSION_ACCELERATION_FACTOR) > 0
            ? (PHASE1_TARGET_BLOCKS / EMISSION_ACCELERATION_FACTOR)
            : 1;

    // During phase 1, reward is deterministic from (supply,height), so always recompute.
    // This avoids using stale on-disk cached rewards (e.g. floor reward after genesis).
    if ((uint64_t)Chain_Size(chain) < effectivePhase1Blocks || currentReward == 0) {
        currentReward = CalculateBlockReward(currentSupply, chain);
    }

    {
        uint8_t dagSeed[32];
        GetNextDAGSeed(chain, dagSeed);
        (void)Block_RebuildAutolykos2Dag(CalculateTargetDAGSize(chain), dagSeed);
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
        uint8_t minerPrivateKey[32];
        uint8_t minerCompressedPubkey[33];
        if (!GenerateTestMinerIdentity(minerPrivateKey, minerCompressedPubkey, minerAddress)) {
            fprintf(stderr, "failed to generate test miner keypair\n");
            Chain_Destroy(chain);
            Block_ShutdownPowContext();
            BalanceSheet_Destroy();
            return 1;
        }

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
                if (!isFirstBlockOfLoadedChain) {
                    block_t* lastBlock = Chain_GetBlock(chain, Chain_Size(chain) - 1);
                    if (lastBlock) {
                        Block_CalculateHash(lastBlock, block->header.prevHash);
                    } else {
                        memset(block->header.prevHash, 0, sizeof(block->header.prevHash));
                    }
                } else {
                    memcpy(block->header.prevHash, lastSavedHash, sizeof(lastSavedHash));
                }
            } else {
                memset(block->header.prevHash, 0, sizeof(block->header.prevHash));
            }
            block->header.timestamp = (uint64_t)time(NULL);
            block->header.difficultyTarget = difficultyTarget;
            block->header.nonce = 0;

            signed_transaction_t coinbaseTx;
            Transaction_Init(&coinbaseTx);
            coinbaseTx.transaction.version = 1;
            coinbaseTx.transaction.amount1 = currentReward;
            coinbaseTx.transaction.fee = 0;
            memcpy(coinbaseTx.transaction.recipientAddress1, minerAddress, sizeof(minerAddress));
            coinbaseTx.transaction.recipientAddress2[0] = 0; // Mark recipient 2 as unused
            coinbaseTx.transaction.amount2 = 0;
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

            (void)uint256_add_u64(&currentSupply, coinbaseTx.transaction.amount1);
            char supplyStr[80];
            Uint256ToDecimal(&currentSupply, supplyStr, sizeof(supplyStr));

            uint8_t canonicalHash[32];
            uint8_t powHash[32];
            Block_CalculateHash(block, canonicalHash);
            Block_CalculateAutolykos2Hash(block, powHash);
            printf("Mined block %llu/%llu (height=%llu) nonce=%llu reward=%llu supply=%s diff=%#x merkle=%02x%02x%02x%02x... pow=%02x%02x%02x%02x... canonical=%02x%02x%02x%02x...\n",
                (unsigned long long)(mined + 1),
                (unsigned long long)blocksToMine,
                (unsigned long long)block->header.blockNumber,
                (unsigned long long)block->header.nonce,
                (unsigned long long)coinbaseTx.transaction.amount1,
                supplyStr,
                (unsigned int)block->header.difficultyTarget,
                block->header.merkleRoot[0], block->header.merkleRoot[1], block->header.merkleRoot[2], block->header.merkleRoot[3],
                powHash[0], powHash[1], powHash[2], powHash[3],
                canonicalHash[0], canonicalHash[1], canonicalHash[2], canonicalHash[3]);

            free(block); // chain stores blocks by value; transactions are owned by chain copy

            currentReward = CalculateBlockReward(currentSupply, chain); // Update the global currentReward for the next block

            // Save chain after each mined block; NOTE: In reality, blocks will appear every ~90s, so this won't be a realistic bottleneck on the mainnet
            // Persist the reward for the *next* block so restart behavior is correct.
            printf("Saving chain at height %zu...\n", Chain_Size(chain));
            Chain_SaveToFile(chain, chainDataDir, currentSupply, currentReward);
            
            if (Chain_Size(chain) % DIFFICULTY_ADJUSTMENT_INTERVAL == 0) {
                difficultyTarget = Chain_ComputeNextTarget(chain, difficultyTarget);
            }

            if (Chain_Size(chain) % EPOCH_LENGTH == 0 && Chain_Size(chain) > 0) {
                uint8_t dagSeed[32];
                GetNextDAGSeed(chain, dagSeed);
                (void)Block_RebuildAutolykos2Dag(CalculateTargetDAGSize(chain), dagSeed);
            }

            isFirstBlockOfLoadedChain = false;
        }

        // Post-loop test: spend some coins from the miner address to a different address.
        // This validates sender balance checks, transaction signing, merkle root generation,
        // and PoW mining for a non-coinbase transaction.
        signed_transaction_t spends[100];
        for (int i = 0; i < 100; i++) {
            int rng = rand() % 10; // Random amount between 0 and 9 (inclusive)
            const uint64_t spendAmount = rng * DECIMALS;
            uint8_t recipientAddress[32];
            MakeTestRecipientAddress(recipientAddress);

            signed_transaction_t spendTx;
            Transaction_Init(&spendTx);
            spendTx.transaction.version = 1;
            spendTx.transaction.fee = 0;
            spendTx.transaction.amount1 = spendAmount;
            spendTx.transaction.amount2 = 0;
            memcpy(spendTx.transaction.senderAddress, minerAddress, sizeof(minerAddress));
            memcpy(spendTx.transaction.recipientAddress1, recipientAddress, sizeof(recipientAddress));
            memset(spendTx.transaction.recipientAddress2, 0, sizeof(spendTx.transaction.recipientAddress2));
            memcpy(spendTx.transaction.compressedPublicKey, minerCompressedPubkey, sizeof(minerCompressedPubkey));

            Transaction_Sign(&spendTx, minerPrivateKey);
            spends[i] = spendTx;
        }

        block_t* spendBlock = Block_Create();
        if (!spendBlock) {
            fprintf(stderr, "failed to create test spend block\n");
            Chain_Destroy(chain);
            Block_ShutdownPowContext();
            BalanceSheet_Destroy();
            return 1;
        }

        spendBlock->header.version = 1;
        spendBlock->header.blockNumber = (uint64_t)Chain_Size(chain);
        if (Chain_Size(chain) > 0) {
            block_t* lastBlock = Chain_GetBlock(chain, Chain_Size(chain) - 1);
            if (lastBlock) {
                Block_CalculateHash(lastBlock, spendBlock->header.prevHash);
            } else {
                memset(spendBlock->header.prevHash, 0, sizeof(spendBlock->header.prevHash));
            }
        } else {
            memset(spendBlock->header.prevHash, 0, sizeof(spendBlock->header.prevHash));
        }
        spendBlock->header.timestamp = (uint64_t)time(NULL);
        spendBlock->header.difficultyTarget = difficultyTarget;
        spendBlock->header.nonce = 0;

        signed_transaction_t testCoinbaseTx;
        Transaction_Init(&testCoinbaseTx);
        memset(&testCoinbaseTx, 0, sizeof(testCoinbaseTx));
        testCoinbaseTx.transaction.version = 1;
        testCoinbaseTx.transaction.amount1 = currentReward;
        testCoinbaseTx.transaction.fee = 0;
        memcpy(testCoinbaseTx.transaction.recipientAddress1, minerAddress, sizeof(minerAddress));
        testCoinbaseTx.transaction.recipientAddress2[0] = 0;
        testCoinbaseTx.transaction.amount2 = 0;
        memset(testCoinbaseTx.transaction.compressedPublicKey, 0, sizeof(testCoinbaseTx.transaction.compressedPublicKey));
        memset(testCoinbaseTx.transaction.senderAddress, 0xFF, sizeof(testCoinbaseTx.transaction.senderAddress));

        Block_AddTransaction(spendBlock, &testCoinbaseTx);
        for (int i = 0; i < 100; i++) {
            Block_AddTransaction(spendBlock, &spends[i]);
        }

        uint8_t merkleRoot[32];
        Block_CalculateMerkleRoot(spendBlock, merkleRoot);
        memcpy(spendBlock->header.merkleRoot, merkleRoot, sizeof(spendBlock->header.merkleRoot));

        if (!MineBlock(spendBlock)) {
            fprintf(stderr, "failed to mine test spend block\n");
            Block_Destroy(spendBlock);
            Chain_Destroy(chain);
            Block_ShutdownPowContext();
            BalanceSheet_Destroy();
            return 1;
        }

        if (!Chain_AddBlock(chain, spendBlock)) {
            fprintf(stderr, "failed to append test spend block to chain\n");
            Block_Destroy(spendBlock);
            Chain_Destroy(chain);
            Block_ShutdownPowContext();
            BalanceSheet_Destroy();
            return 1;
        }

        (void)uint256_add_u64(&currentSupply, testCoinbaseTx.transaction.amount1);
        currentReward = CalculateBlockReward(currentSupply, chain);

        //printf("Mined test spend block (height=%llu) sending %llu base units to a new address\n",
        //    (unsigned long long)spendBlock->header.blockNumber,
        //    (unsigned long long)spendAmount);

        free(spendBlock);

        bool chainSaved = Chain_SaveToFile(chain, chainDataDir, currentSupply, currentReward);
        bool sheetSaved = BalanceSheet_SaveToFile(chainDataDir);
        if (!chainSaved || !sheetSaved) {
            if (!chainSaved) {
                fprintf(stderr, "failed to save chain to %s\n", chainDataDir);
            }
            if (!sheetSaved) {
                fprintf(stderr, "failed to save balance sheet to %s\n", chainDataDir);
            }
        } else {
            char supplyStr[80];
            Uint256ToDecimal(&currentSupply, supplyStr, sizeof(supplyStr));
            printf("Saved chain with %zu blocks to %s (supply=%s)\n",
                Chain_Size(chain),
                chainDataDir,
                supplyStr);
        }
    } else {
        char supplyStr[80];
        Uint256ToDecimal(&currentSupply, supplyStr, sizeof(supplyStr));
        printf("Current chain has %zu blocks, total supply %s\n", Chain_Size(chain), supplyStr);
    }

    // Print chain
    /*for (size_t i = 0; i < Chain_Size(chain); i++) {
        block_t* blk = Chain_GetBlock(chain, i);
        if (blk) {
            Block_Print(blk);
        }
    }*/

    BalanceSheet_Print();
    if (!BalanceSheet_SaveToFile(chainDataDir)) {
        fprintf(stderr, "failed to save balance sheet to %s\n", chainDataDir);
    }

    Chain_Destroy(chain);
    Block_ShutdownPowContext();
    return 0;
}
