#include <block/chain.h>
#include <block/transaction.h>
#include <utils.h>

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

#include <nets/net_node.h>

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

static bool FlushChainAndSheet(blockchain_t* chain,
                               const char* chainDataDir,
                               uint256_t currentSupply,
                               uint64_t currentReward) {
    bool chainSaved = Chain_SaveToFile(chain, chainDataDir, currentSupply, currentReward);
    bool sheetSaved = BalanceSheet_SaveToFile(chainDataDir);

    if (!chainSaved) {
        fprintf(stderr, "failed to save chain to %s\n", chainDataDir);
    }
    if (!sheetSaved) {
        fprintf(stderr, "failed to save balance sheet to %s\n", chainDataDir);
    }

    return chainSaved && sheetSaved;
}

static block_t* BuildNextBlock(blockchain_t* chain, uint32_t difficultyTarget) {
    block_t* block = Block_Create();
    if (!block) {
        return NULL;
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
    block->header.timestamp = (uint64_t)get_current_time_ms();
    block->header.difficultyTarget = difficultyTarget;
    block->header.nonce = 0;

    return block;
}

static void AddCoinbaseTransaction(block_t* block, const uint8_t minerAddress[32], uint64_t reward) {
    signed_transaction_t coinbaseTx;
    Transaction_Init(&coinbaseTx);
    coinbaseTx.transaction.version = 1;
    coinbaseTx.transaction.amount1 = reward;
    coinbaseTx.transaction.fee = 0;
    memcpy(coinbaseTx.transaction.recipientAddress1, minerAddress, 32);
    memset(coinbaseTx.transaction.recipientAddress2, 0, sizeof(coinbaseTx.transaction.recipientAddress2));
    coinbaseTx.transaction.amount2 = 0;
    memset(coinbaseTx.transaction.compressedPublicKey, 0, sizeof(coinbaseTx.transaction.compressedPublicKey));
    memset(coinbaseTx.transaction.senderAddress, 0xFF, sizeof(coinbaseTx.transaction.senderAddress));
    Block_AddTransaction(block, &coinbaseTx);
}

static void PrintBlockDetail(const block_t* block, size_t txCount, const uint8_t canonicalHash[32], const uint8_t powHash[32]) {
    if (!block) {
        return;
    }

    printf("Block #%llu\n", (unsigned long long)block->header.blockNumber);
    printf("  Timestamp: %llu\n", (unsigned long long)block->header.timestamp);
    printf("  Nonce: %llu\n", (unsigned long long)block->header.nonce);
    printf("  Difficulty Target: 0x%08x\n", block->header.difficultyTarget);
    printf("  Version: %u\n", block->header.version);
    printf("  Reserved: %02x %02x %02x\n",
        block->header.reserved[0],
        block->header.reserved[1],
        block->header.reserved[2]);
    printf("  Previous Hash: ");
    PrintHexBytes(block->header.prevHash, sizeof(block->header.prevHash));
    printf("\n");
    printf("  Merkle Root: ");
    PrintHexBytes(block->header.merkleRoot, sizeof(block->header.merkleRoot));
    printf("\n");
    printf("  Transactions on disk: %zu\n", txCount);
    printf("  Canonical Hash: ");
    PrintHexBytes(canonicalHash, 32);
    printf("\n");
    printf("  PoW Hash: ");
    PrintHexBytes(powHash, 32);
    printf("\n");
}

static bool ComputeEpochSeedForHeightFromChain(const blockchain_t* chain, uint64_t blockHeight, uint8_t outSeed[32]) {
    if (!chain || !outSeed) {
        return false;
    }

    const uint64_t epochIndex = blockHeight / EPOCH_LENGTH;
    if (epochIndex == 0) {
        memset(outSeed, DAG_GENESIS_SEED, 32);
        return true;
    }

    const uint64_t seedBlockNumber = (epochIndex * EPOCH_LENGTH) - 1ULL;
    if (seedBlockNumber >= Chain_Size((blockchain_t*)chain)) {
        return false;
    }

    block_t* seedBlock = Chain_GetBlock((blockchain_t*)chain, (size_t)seedBlockNumber);
    if (!seedBlock) {
        return false;
    }

    Block_CalculateHash(seedBlock, outSeed);
    return true;
}

static bool ComputeEpochDagBytesForHeightFromChain(const blockchain_t* chain, uint64_t blockHeight, size_t* outDagBytes) {
    if (!chain || !outDagBytes) {
        return false;
    }

    if (blockHeight <= EPOCH_LENGTH) {
        *outDagBytes = DAG_BASE_SIZE;
        return true;
    }

    const uint64_t lastBlockNumber = blockHeight - 1ULL;
    const uint64_t epochStartBlockNumber = lastBlockNumber - EPOCH_LENGTH;
    if (lastBlockNumber >= Chain_Size((blockchain_t*)chain) || epochStartBlockNumber >= Chain_Size((blockchain_t*)chain)) {
        return false;
    }

    block_t* lastBlock = Chain_GetBlock((blockchain_t*)chain, (size_t)lastBlockNumber);
    block_t* epochStartBlock = Chain_GetBlock((blockchain_t*)chain, (size_t)epochStartBlockNumber);
    if (!lastBlock || !epochStartBlock) {
        return false;
    }

    int64_t difficultyDelta = (int64_t)epochStartBlock->header.difficultyTarget - (int64_t)lastBlock->header.difficultyTarget;
    int64_t growth = (int64_t)((int64_t)DAG_BASE_GROWTH * difficultyDelta);

    if (growth > 0) {
        int64_t maxUp = (int64_t)((DAG_BASE_SIZE * 15ULL) / 100ULL);
        if (growth > maxUp) {
            growth = maxUp;
        }
        if (growth > (int64_t)DAG_MAX_UP_SWING_GB) {
            growth = (int64_t)DAG_MAX_UP_SWING_GB;
        }
    } else {
        int64_t maxDown = (int64_t)((DAG_BASE_SIZE * 10ULL) / 100ULL);
        if (-growth > maxDown) {
            growth = -maxDown;
        }
        if (-growth > (int64_t)DAG_MAX_DOWN_SWING_GB) {
            growth = -(int64_t)DAG_MAX_DOWN_SWING_GB;
        }
    }

    const int64_t targetSize = (int64_t)DAG_BASE_SIZE + growth;
    if (targetSize <= 0) {
        return false;
    }

    *outDagBytes = (size_t)targetSize;
    return true;
}

static bool ComputeHistoricalAutolykosHashFromChain(const blockchain_t* chain, const block_t* block, uint64_t blockHeight, uint8_t outHash[32]) {
    if (!chain || !block || !outHash) {
        return false;
    }

    uint8_t seed[32];
    size_t dagBytes = 0;
    if (!ComputeEpochSeedForHeightFromChain(chain, blockHeight, seed)) {
        return false;
    }
    if (!ComputeEpochDagBytesForHeightFromChain(chain, blockHeight, &dagBytes)) {
        return false;
    }

    return Autolykos2_LightHashAtHeight(
        seed,
        (const uint8_t*)&block->header,
        sizeof(block_header_t),
        block->header.nonce,
        blockHeight,
        dagBytes,
        outHash
    );
}

static bool ComputeHistoricalAutolykosHashFromDisk(const char* chainDataDir, uint64_t blockHeight, const block_t* block, uint8_t outHash[32]) {
    if (!chainDataDir || !block || !outHash) {
        return false;
    }

    blockchain_t* headerChain = Chain_Create();
    if (!headerChain) {
        return false;
    }

    uint256_t supply = uint256_from_u64(0);
    uint32_t difficulty = INITIAL_DIFFICULTY;
    uint64_t reward = 0;
    uint8_t lastHash[32] = {0};
    bool loaded = Chain_LoadFromFile(headerChain, chainDataDir, &supply, &difficulty, &reward, lastHash, false);
    bool ok = loaded && ComputeHistoricalAutolykosHashFromChain(headerChain, block, blockHeight, outHash);

    Chain_Destroy(headerChain);
    return ok;
}

static bool MineAndAppendBlock(blockchain_t* chain,
                               block_t* block,
                               uint256_t* currentSupply,
                               uint64_t* currentReward,
                               uint32_t* difficultyTarget) {
    if (!chain || !block || !currentSupply || !currentReward || !difficultyTarget) {
        return false;
    }

    uint8_t merkleRoot[32];
    Block_CalculateMerkleRoot(block, merkleRoot);
    memcpy(block->header.merkleRoot, merkleRoot, sizeof(block->header.merkleRoot));

    if (!MineBlock(block)) {
        fprintf(stderr, "failed to mine block within nonce range\n");
        return false;
    }

    if (!Chain_AddBlock(chain, block)) {
        fprintf(stderr, "failed to append block to chain\n");
        return false;
    }

    uint64_t coinbaseAmount = 0;
    if (block->transactions && DynArr_size(block->transactions) > 0) {
        signed_transaction_t* firstTx = (signed_transaction_t*)DynArr_at(block->transactions, 0);
        if (firstTx && Address_IsCoinbase(firstTx->transaction.senderAddress)) {
            coinbaseAmount = firstTx->transaction.amount1;
        }
    }

    (void)uint256_add_u64(currentSupply, coinbaseAmount);

    uint8_t canonicalHash[32];
    uint8_t powHash[32];
    Block_CalculateHash(block, canonicalHash);
    Block_CalculateAutolykos2Hash(block, powHash);

    char supplyStr[80];
    Uint256ToDecimal(currentSupply, supplyStr, sizeof(supplyStr));
    printf("Mined block height=%llu nonce=%llu reward=%llu supply=%s diff=%#x pow=%02x%02x%02x%02x... canonical=%02x%02x%02x%02x...\n",
        (unsigned long long)block->header.blockNumber,
        (unsigned long long)block->header.nonce,
        (unsigned long long)coinbaseAmount,
        supplyStr,
        (unsigned int)block->header.difficultyTarget,
        powHash[0], powHash[1], powHash[2], powHash[3],
        canonicalHash[0], canonicalHash[1], canonicalHash[2], canonicalHash[3]);

    *currentReward = CalculateBlockReward(*currentSupply, chain);

    if (Chain_Size(chain) % DIFFICULTY_ADJUSTMENT_INTERVAL == 0) {
        *difficultyTarget = Chain_ComputeNextTarget(chain, *difficultyTarget);
    }

    if (Chain_Size(chain) % EPOCH_LENGTH == 0 && Chain_Size(chain) > 0) {
        uint8_t dagSeed[32];
        GetNextDAGSeed(chain, dagSeed);
        (void)Block_RebuildAutolykos2Dag(CalculateTargetDAGSize(chain), dagSeed);
    }

    return true;
}

static void WipeChainFiles(const char* chainDataDir) {
    if (!chainDataDir) {
        return;
    }

    char path[512];

    snprintf(path, sizeof(path), "%s/chain.meta", chainDataDir);
    remove(path);

    snprintf(path, sizeof(path), "%s/chain.data", chainDataDir);
    remove(path);

    snprintf(path, sizeof(path), "%s/chain.table", chainDataDir);
    remove(path);

    snprintf(path, sizeof(path), "%s/balance_sheet.data", chainDataDir);
    remove(path);
}

static bool VerifyChainFully(blockchain_t* chain) {
    if (!chain || !chain->blocks) {
        return false;
    }

    size_t chainSize = Chain_Size(chain);
    // Build a lightweight previous-block-only chain to compute expected difficulty
    blockchain_t* prevChain = Chain_Create();
    if (!prevChain) { return false; }

    uint32_t expectedDifficulty = INITIAL_DIFFICULTY;
    for (size_t i = 0; i < chainSize; ++i) {
        block_t* blk = Chain_GetBlock(chain, i);
        if (!blk || !blk->transactions) {
            Chain_Destroy(prevChain);
            return false;
        }

        if (blk->header.blockNumber != (uint64_t)i) {
            Chain_Destroy(prevChain);
            return false;
        }

        if (i == 0) {
            uint8_t zeroHash[32] = {0};
            if (memcmp(blk->header.prevHash, zeroHash, sizeof(zeroHash)) != 0) {
                Chain_Destroy(prevChain);
                return false;
            }
        } else {
            block_t* prevBlk = Chain_GetBlock(chain, i - 1);
            if (!prevBlk) {
                Chain_Destroy(prevChain);
                return false;
            }

            uint8_t expectedPrevHash[32];
            Block_CalculateHash(prevBlk, expectedPrevHash);
            if (memcmp(blk->header.prevHash, expectedPrevHash, sizeof(expectedPrevHash)) != 0) {
                Chain_Destroy(prevChain);
                return false;
            }
        }

        // Determine expected difficulty for this block. TODO: Optimize to recompute at adjustment intervals only instead of every block.
        if (i < DIFFICULTY_ADJUSTMENT_INTERVAL) {
            expectedDifficulty = INITIAL_DIFFICULTY;
        } else if ((i % DIFFICULTY_ADJUSTMENT_INTERVAL) == 0) {
            // Compute target using previous blocks only (0..i-1)
            expectedDifficulty = Chain_ComputeNextTarget(prevChain, expectedDifficulty);
        }

        // Ensure the block's header difficulty matches the expected difficulty (can't cheat easier)
        if (blk->header.difficultyTarget != expectedDifficulty) {
            Chain_Destroy(prevChain);
            return false;
        }

        uint8_t powHash[32];
        if (!ComputeHistoricalAutolykosHashFromChain(chain, blk, (uint64_t)i, powHash)) {
            Chain_Destroy(prevChain);
            return false;
        }

        uint8_t target[32];
        if (!DecodeCompactTarget(blk->header.difficultyTarget, target)) {
            return false;
        }
        if (CompareHashToTarget(powHash, target) > 0) {
            return false;
        }

        if (!Block_AllTransactionsValid(blk)) {
            Chain_Destroy(prevChain);
            return false;
        }

        uint8_t expectedMerkle[32];
        Block_CalculateMerkleRoot(blk, expectedMerkle);
        if (memcmp(blk->header.merkleRoot, expectedMerkle, sizeof(expectedMerkle)) != 0) {
            Chain_Destroy(prevChain);
            return false;
        }

        // Transactions are persisted on disk. Once this block is fully verified,
        // release its in-memory transaction list to reduce peak memory usage.
        DynArr_destroy(blk->transactions);
        blk->transactions = NULL;

        // Push a header-only copy of this block into prevChain for future difficulty calculations.
        block_t headerOnly;
        memset(&headerOnly, 0, sizeof(headerOnly));
        headerOnly.header = blk->header;
        headerOnly.transactions = NULL;
        (void)DynArr_push_back(prevChain->blocks, &headerOnly);
    }

    Chain_Destroy(prevChain);
    return true;
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    signal(SIGINT, handle_sigint);
    srand((unsigned int)time(NULL));

    BalanceSheet_Init();
    const char* chainDataDir = CHAIN_DATA_DIR;

    uint256_t currentSupply = uint256_from_u64(0);

    net_node_t* node = Node_Create();
    if (!node) {
        BalanceSheet_Destroy();
        return 1;
    }

    blockchain_t* chain = Chain_Create();
    if (!chain) {
        fprintf(stderr, "failed to create chain\n");
        Node_Destroy(node);
        BalanceSheet_Destroy();
        return 1;
    }

    uint8_t lastSavedHash[32] = {0};
    if (!Chain_LoadFromFile(chain, chainDataDir, &currentSupply, &difficultyTarget, &currentReward, lastSavedHash, false)) {
        printf("No existing chain loaded from %s\n", chainDataDir);
    }

    if (!BalanceSheet_LoadFromFile(chainDataDir)) {
        printf("Failed to load the balance sheet or none existing\n");
    }

    const uint64_t effectivePhase1Blocks =
        (PHASE1_TARGET_BLOCKS / EMISSION_ACCELERATION_FACTOR) > 0
            ? (PHASE1_TARGET_BLOCKS / EMISSION_ACCELERATION_FACTOR)
            : 1;

    if ((uint64_t)Chain_Size(chain) < effectivePhase1Blocks || currentReward == 0) {
        currentReward = CalculateBlockReward(currentSupply, chain);
    }

    {
        uint8_t dagSeed[32];
        GetNextDAGSeed(chain, dagSeed);
        (void)Block_RebuildAutolykos2Dag(CalculateTargetDAGSize(chain), dagSeed);
        printf("Built initial DAG with seed %02x%02x%02x%02x... and size %zu bytes\n",
            dagSeed[0], dagSeed[1], dagSeed[2], dagSeed[3],
            CalculateTargetDAGSize(chain));
    }

    if (Chain_Size(chain) > 0) {
        if (Chain_IsValid(chain)) {
            printf("Loaded chain with %zu blocks from disk\n", Chain_Size(chain));
        } else {
            fprintf(stderr, "loaded chain is invalid, wiping persisted state.\n");
            WipeChainFiles(chainDataDir);
            Chain_Wipe(chain);
            BalanceSheet_Destroy();
            BalanceSheet_Init();
            currentSupply = uint256_from_u64(0);
            difficultyTarget = INITIAL_DIFFICULTY;
            currentReward = CalculateBlockReward(currentSupply, chain);
        }
    }

    uint8_t minerAddress[32];
    uint8_t minerPrivateKey[32];
    uint8_t minerCompressedPubkey[33];
    if (!GenerateTestMinerIdentity(minerPrivateKey, minerCompressedPubkey, minerAddress)) {
        fprintf(stderr, "failed to generate test miner keypair\n");
        Chain_Destroy(chain);
        Node_Destroy(node);
        Block_ShutdownPowContext();
        BalanceSheet_Destroy();
        return 1;
    }

    char minerAddressHex[65];
    AddressToHexString(minerAddress, minerAddressHex);
    printf("Test miner address: %s\n", minerAddressHex);

    char supplyStr[80];
    Uint256ToDecimal(&currentSupply, supplyStr, sizeof(supplyStr));
    printf("Current chain has %zu blocks, total supply %s\n", Chain_Size(chain), supplyStr);
    printf("Commands: mine <x>, send <address> <amount>, balance [address], connect <ipv4>, flushchain, fullverify, blockdetail <block number>, wipechain, genaddr, exit\n");

    char line[1024];
    while (true) {
        printf("> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            break;
        }

        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') {
            continue;
        }

        char* cmd = strtok(line, " \t");
        if (!cmd) {
            continue;
        }

        if (strcmp(cmd, "mine") == 0) {
            char* blocksStr = strtok(NULL, " \t");
            if (!blocksStr) {
                printf("usage: mine <x>\n");
                continue;
            }

            char* endptr = NULL;
            unsigned long long requested = strtoull(blocksStr, &endptr, 10);
            if (*blocksStr == '\0' || blocksStr[0] == '-' || (endptr && *endptr != '\0')) {
                printf("invalid block count\n");
                continue;
            }

            printf("Mining %llu block(s)...\n", requested);
            bool minedAll = true;
            for (unsigned long long i = 0; i < requested; ++i) {
                block_t* block = BuildNextBlock(chain, difficultyTarget);
                if (!block) {
                    fprintf(stderr, "failed to create block\n");
                    minedAll = false;
                    break;
                }

                AddCoinbaseTransaction(block, minerAddress, currentReward);

                if (!MineAndAppendBlock(chain, block, &currentSupply, &currentReward, &difficultyTarget)) {
                    Block_Destroy(block);
                    minedAll = false;
                    break;
                }

                free(block); // Chain stores block by value and owns copied transaction array.

                if (i % 50 == 0) {
                    // Mid-mine flush
                    (void)FlushChainAndSheet(chain, chainDataDir, currentSupply, currentReward);
                }
            }

            

            if (minedAll) {
                (void)FlushChainAndSheet(chain, chainDataDir, currentSupply, currentReward);
                printf("mine finished and chain flushed\n");
            }
            continue;
        }

        if (strcmp(cmd, "send") == 0) {
            char* addressStr = strtok(NULL, " \t");
            char* amountStr = strtok(NULL, " \t");
            if (!addressStr || !amountStr) {
                printf("usage: send <address> <amount>\n");
                continue;
            }

            uint8_t recipientAddress[32];
            if (!ParseHexAddress32(addressStr, recipientAddress)) {
                printf("invalid address: expected 64 hex chars (optionally prefixed with 0x)\n");
                continue;
            }

            char* endptr = NULL;
            unsigned long long amount = strtoull(amountStr, &endptr, 10);
            if (*amountStr == '\0' || amountStr[0] == '-' || (endptr && *endptr != '\0') || amount == 0) {
                printf("invalid amount\n");
                continue;
            }

            balance_sheet_entry_t senderEntry;
            if (!BalanceSheet_Lookup(minerAddress, &senderEntry)) {
                printf("send failed: miner address has no balance\n");
                continue;
            }

            uint256_t spend = uint256_from_u64((uint64_t)amount);
            if (uint256_cmp(&senderEntry.balance, &spend) < 0) {
                printf("send failed: insufficient balance\n");
                continue;
            }

            block_t* block = BuildNextBlock(chain, difficultyTarget);
            if (!block) {
                fprintf(stderr, "failed to create block\n");
                continue;
            }

            AddCoinbaseTransaction(block, minerAddress, currentReward);

            signed_transaction_t spendTx;
            Transaction_Init(&spendTx);
            spendTx.transaction.version = 1;
            spendTx.transaction.fee = 0;
            spendTx.transaction.amount1 = (uint64_t)amount;
            spendTx.transaction.amount2 = 0;
            memcpy(spendTx.transaction.senderAddress, minerAddress, sizeof(minerAddress));
            memcpy(spendTx.transaction.recipientAddress1, recipientAddress, sizeof(recipientAddress));
            memset(spendTx.transaction.recipientAddress2, 0, sizeof(spendTx.transaction.recipientAddress2));
            memcpy(spendTx.transaction.compressedPublicKey, minerCompressedPubkey, sizeof(minerCompressedPubkey));
            Transaction_Sign(&spendTx, minerPrivateKey);

            Block_AddTransaction(block, &spendTx);
            printf("Created transaction sending %llu pebble(s) to ", (unsigned long long)amount);
            char recipientHex[65];
            AddressToHexString(recipientAddress, recipientHex);
            printf("%s\n\nMining block...\n", recipientHex);
            
            if (!MineAndAppendBlock(chain, block, &currentSupply, &currentReward, &difficultyTarget)) {
                Block_Destroy(block);
                continue;
            }

            FlushChainAndSheet(chain, chainDataDir, currentSupply, currentReward);

            free(block);
            printf("send committed in mined block\n");
            continue;
        }

        if (strcmp(cmd, "blockdetail") == 0) {
            char* blockNumberStr = strtok(NULL, " \t");
            char* extra = strtok(NULL, " \t");
            if (!blockNumberStr || extra) {
                printf("usage: blockdetail <block number>\n");
                continue;
            }

            char* endptr = NULL;
            unsigned long long requestedBlock = strtoull(blockNumberStr, &endptr, 10);
            if (*blockNumberStr == '\0' || blockNumberStr[0] == '-' || (endptr && *endptr != '\0')) {
                printf("invalid block number\n");
                continue;
            }

            block_t* detailBlock = NULL;
            size_t txCount = 0;
            if (!Chain_LoadBlockFromFile(chainDataDir, (uint64_t)requestedBlock, false, &detailBlock, &txCount)) {
                printf("block %llu not found\n", requestedBlock);
                continue;
            }

            uint8_t canonicalHash[32];
            uint8_t powHash[32];
            Block_CalculateHash(detailBlock, canonicalHash);
            if (!ComputeHistoricalAutolykosHashFromDisk(chainDataDir, (uint64_t)requestedBlock, detailBlock, powHash)) {
                Block_Destroy(detailBlock);
                printf("failed to calculate block %llu proof hash\n", requestedBlock);
                continue;
            }

            PrintBlockDetail(detailBlock, txCount, canonicalHash, powHash);
            Block_Destroy(detailBlock);
            continue;
        }

        if (strcmp(cmd, "balance") == 0) {
            char* addressStr = strtok(NULL, " \t");
            char* extra = strtok(NULL, " \t");
            if (extra) {
                printf("usage: balance [address]\n");
                continue;
            }

            uint8_t queryAddress[32];
            uint8_t* effectiveAddress = minerAddress;

            if (addressStr) {
                if (strcmp(addressStr, "all") == 0) {
                    printf("All balances:\n");
                    BalanceSheet_Print();
                    continue;
                }

                if (!ParseHexAddress32(addressStr, queryAddress)) {
                    printf("invalid address: expected 64 hex chars (optionally prefixed with 0x)\n");
                    continue;
                }
                effectiveAddress = queryAddress;
            }

            balance_sheet_entry_t entry;
            char balanceStr[80];
            if (!BalanceSheet_Lookup(effectiveAddress, &entry)) {
                uint256_t zero = uint256_from_u64(0);
                Uint256ToDecimal(&zero, balanceStr, sizeof(balanceStr));
            } else {
                Uint256ToDecimal(&entry.balance, balanceStr, sizeof(balanceStr));
            }

            char addrHex[65];
            AddressToHexString(effectiveAddress, addrHex);
            printf("Balance %s: %s pebble(s)\n", addrHex, balanceStr);
            continue;
        }

        if (strcmp(cmd, "connect") == 0) {
            char* ipStr = strtok(NULL, " \t");
            char* extra = strtok(NULL, " \t");
            if (!ipStr || extra) {
                printf("usage: connect <ipv4>\n");
                continue;
            }

            if (!IsValidIPv4(ipStr)) {
                printf("invalid IPv4 address\n");
                continue;
            }

            if (Node_ConnectPeer(node, ipStr, LISTEN_PORT) != 0) {
                printf("failed to connect to %s:%u\n", ipStr, (unsigned int)LISTEN_PORT);
                continue;
            }

            printf("connect requested to %s:%u\n", ipStr, (unsigned int)LISTEN_PORT);
            continue;
        }

        if (strcmp(cmd, "flushchain") == 0) {
            if (FlushChainAndSheet(chain, chainDataDir, currentSupply, currentReward)) {
                printf("chain flushed\n");
            }
            continue;
        }

        if (strcmp(cmd, "fullverify") == 0) {
            blockchain_t* verifyChain = Chain_Create();
            if (!verifyChain) {
                printf("Chain Not OK\n");
                continue;
            }

            uint256_t verifySupply = uint256_from_u64(0);
            uint32_t verifyDifficulty = INITIAL_DIFFICULTY;
            uint64_t verifyReward = 0;
            uint8_t verifyLastHash[32] = {0};

            bool loaded = Chain_LoadFromFile(
                verifyChain,
                chainDataDir,
                &verifySupply,
                &verifyDifficulty,
                &verifyReward,
                verifyLastHash,
                true
            );

            bool ok = false;
            if (loaded) {
                ok = VerifyChainFully(verifyChain);
            }

            printf("%s\n", ok ? "Chain OK" : "Chain Not OK");
            Chain_Destroy(verifyChain);
            continue;
        }

        if (strcmp(cmd, "wipechain") == 0) {
            WipeChainFiles(chainDataDir);
            Chain_Wipe(chain);
            BalanceSheet_Destroy();
            BalanceSheet_Init();
            currentSupply = uint256_from_u64(0);
            difficultyTarget = INITIAL_DIFFICULTY;
            currentReward = CalculateBlockReward(currentSupply, chain);

            uint8_t dagSeed[32];
            memset(dagSeed, DAG_GENESIS_SEED, sizeof(dagSeed));
            (void)Block_RebuildAutolykos2Dag(DAG_BASE_SIZE, dagSeed);

            printf("chain data wiped\n");
            continue;
        }

        if (strcmp(cmd, "genaddr") == 0) {
            uint8_t testAddress[32];
            if (!GenerateRandomTestAddress(testAddress)) {
                printf("failed to generate address\n");
                continue;
            }

            char addrHex[65];
            AddressToHexString(testAddress, addrHex);
            printf("%s\n", addrHex);
            continue;
        }

        if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
            break;
        }

        printf("Unknown command. Available: mine, send, balance, connect, flushchain, fullverify, blockdetail, wipechain, genaddr, exit\n");
    }

    (void)FlushChainAndSheet(chain, chainDataDir, currentSupply, currentReward);

    Chain_Destroy(chain);
    Block_ShutdownPowContext();
    Node_Destroy(node);
    BalanceSheet_Destroy();

    return 0;
}
