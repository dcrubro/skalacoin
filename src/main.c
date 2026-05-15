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
#include <unistd.h>
#include <errno.h>


#include <constants.h>
#include <runtime_state.h>
#include <autolykos2/autolykos2.h>

#include <nets/net_node.h>
#include <nets/fetch_scheduler.h>
#include <nets/orphan_pool.h>

#ifndef CHAIN_DATA_DIR
#define CHAIN_DATA_DIR "chain_data"
#endif

blockchain_t* currentChain = NULL;
const char* chainDataDir = CHAIN_DATA_DIR;
uint256_t currentSupply = {{0, 0, 0, 0}};
uint64_t currentReward = 750000000000ULL;

// Define the synchronization primitives declared in runtime_state.h
pthread_rwlock_t chainLock;
pthread_mutex_t balanceSheetLock;

void handle_sigint(int sig) {
    printf("Caught signal %d, exiting...\n", sig);
    Block_ShutdownPowContext();
    BalanceSheet_Destroy();
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
        block_t* lastBlock = NULL;
        if (Chain_GetBlockCopy(chain, Chain_Size(chain) - 1, &lastBlock)) {
            Block_CalculateHash(lastBlock, block->header.prevHash);
            Block_Destroy(lastBlock);
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

    block_t* seedBlock = NULL;
    if (!Chain_GetBlockCopy((blockchain_t*)chain, (size_t)seedBlockNumber, &seedBlock)) {
        return false;
    }

    Block_CalculateHash(seedBlock, outSeed);
    Block_Destroy(seedBlock);
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

    block_t* lastBlock = NULL;
    block_t* epochStartBlock = NULL;
    if (!Chain_GetBlockCopy((blockchain_t*)chain, (size_t)lastBlockNumber, &lastBlock)) { return false; }
    if (!Chain_GetBlockCopy((blockchain_t*)chain, (size_t)epochStartBlockNumber, &epochStartBlock)) { Block_Destroy(lastBlock); return false; }

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
    Block_Destroy(lastBlock);
    Block_Destroy(epochStartBlock);
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

    // After successfully appending a block, attempt to attach any orphans.
    size_t attached = OrphanPool_AttemptAttach(chain);
    if (attached > 0) {
        printf("Attached %zu orphan(s) after mining/appending block\n", attached);
        // Persist chain/sheet after attaching orphans
        Chain_SaveToFile(chain, chainDataDir, *currentSupply, *currentReward);
        BalanceSheet_SaveToFile(chainDataDir);
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
        block_t* blk = NULL;
        if (!Chain_GetBlockCopy(chain, i, &blk) || !blk || !blk->transactions) {
            if (blk) Block_Destroy(blk);
            Chain_Destroy(prevChain);
            return false;
        }

        if (blk->header.blockNumber != (uint64_t)i) {
            Block_Destroy(blk);
            Chain_Destroy(prevChain);
            return false;
        }

        if (i == 0) {
            uint8_t zeroHash[32] = {0};
            if (memcmp(blk->header.prevHash, zeroHash, sizeof(zeroHash)) != 0) {
                Block_Destroy(blk);
                Chain_Destroy(prevChain);
                return false;
            }
        } else {
            block_t* prevBlk = NULL;
            if (!Chain_GetBlockCopy(chain, i - 1, &prevBlk) || !prevBlk) {
                if (prevBlk) Block_Destroy(prevBlk);
                Block_Destroy(blk);
                Chain_Destroy(prevChain);
                return false;
            }

            uint8_t expectedPrevHash[32];
            Block_CalculateHash(prevBlk, expectedPrevHash);
            if (memcmp(blk->header.prevHash, expectedPrevHash, sizeof(expectedPrevHash)) != 0) {
                Block_Destroy(prevBlk);
                Block_Destroy(blk);
                Chain_Destroy(prevChain);
                return false;
            }
            Block_Destroy(prevBlk);
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
            Block_Destroy(blk);
            Chain_Destroy(prevChain);
            return false;
        }

        uint8_t powHash[32];
        if (!ComputeHistoricalAutolykosHashFromChain(chain, blk, (uint64_t)i, powHash)) {
            Block_Destroy(blk);
            Chain_Destroy(prevChain);
            return false;
        }

        uint8_t target[32];
        if (!DecodeCompactTarget(blk->header.difficultyTarget, target)) {
            Block_Destroy(blk);
            Chain_Destroy(prevChain);
            return false;
        }
        if (CompareHashToTarget(powHash, target) > 0) {
            Block_Destroy(blk);
            Chain_Destroy(prevChain);
            return false;
        }

        if (!Block_AllTransactionsValid(blk)) {
            Block_Destroy(blk);
            Chain_Destroy(prevChain);
            return false;
        }

        uint8_t expectedMerkle[32];
        Block_CalculateMerkleRoot(blk, expectedMerkle);
        if (memcmp(blk->header.merkleRoot, expectedMerkle, sizeof(expectedMerkle)) != 0) {
            Block_Destroy(blk);
            Chain_Destroy(prevChain);
            return false;
        }

        // Transactions are persisted on disk. Once this block is fully verified,
        // release its in-memory transaction list to reduce peak memory usage.
        if (blk->transactions) {
            DynArr_destroy(blk->transactions);
            blk->transactions = NULL;
        }

        // Push a header-only copy of this block into prevChain for future difficulty calculations.
        block_t headerOnly;
        memset(&headerOnly, 0, sizeof(headerOnly));
        headerOnly.header = blk->header;
        headerOnly.transactions = NULL;
        (void)DynArr_push_back(prevChain->blocks, &headerOnly);

        Block_Destroy(blk);
    }

    Chain_Destroy(prevChain);
    return true;
}

int main(int argc, char* argv[]) {
    //(void)argc;
    //(void)argv;
    if (argc > 1) {
        // Check for potential startup args.
        if (strcmp(argv[1], "--throttle") == 0) {
            // Get throttle value in microseconds if provided, otherwise default to 1000 microseconds (1ms) between hash operations.
            uint64_t throttleUs = 1000;
            if (argc > 2) {
                char* endptr = NULL;
                throttleUs = strtoull(argv[2], &endptr, 10);
                if (*argv[2] == '\0' || argv[2][0] == '-' || (endptr && *endptr != '\0')) {
                    printf("invalid throttle value\n");
                    return 1;
                }
            }
            Autolykos2_SetSleepBetweenHashOperations(throttleUs);
            printf("Throttling hash operations with a sleep of %llu microseconds\n", (unsigned long long)throttleUs);
        } else {
            printf("Unknown argument: %s\n", argv[1]);
            return 1;
        }
    }

    signal(SIGINT, handle_sigint);
    srand((unsigned int)time(NULL));

    // Initialize runtime locks before any thread or helper can touch chain state.
    pthread_rwlock_init(&chainLock, NULL);
    pthread_mutex_init(&balanceSheetLock, NULL);

    BalanceSheet_Init();
    blockchain_t* chain = Chain_Create();
    if (!chain) {
        fprintf(stderr, "failed to create chain\n");
        BalanceSheet_Destroy();
        return 1;
    }

    currentChain = chain;

    net_node_t* node = Node_Create();
    if (!node) {
        currentChain = NULL;
        Chain_Destroy(chain);
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
        Node_Destroy(node);
        currentChain = NULL;
        Chain_Destroy(chain);
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
    printf("Commands: mine <x>, send <address> <amount>, balance [address], connect <ipv4>, sync (requires nodes), flushchain, fullverify, blockdetail <block number>, wipechain, genaddr, exit\n");

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

                // Broadcast newly mined block to outbound peers
                if (node) {
                    Node_BroadcastChainRange(node, Chain_Size(chain) - 1, NULL);
                }

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
            if (node) {
                Node_BroadcastChainRange(node, Chain_Size(chain) - 1, NULL);
            }
            printf("send committed in mined block\n");
            continue;
        }

        if (strcmp(cmd, "sync") == 0) {
            if (!node) {
                printf("no node available\n");
                continue;
            }

            // Choose the best outbound peer by advertised height
            tcp_connection_t* peerConn = NULL;
            uint64_t peerHeight = 0;
            if (Node_GetBestOutboundPeer(node, &peerConn, &peerHeight) != 0 || !peerConn) {
                printf("no outbound peers to sync from\n");
                continue;
            }

            // Continue syncing in a loop until we've caught up to the peer or no progress is made.
            bool madeProgressOverall = false;
            while (true) {
                uint64_t localHeight = (uint64_t)Chain_Size(chain);

            // Only penalize small near-tip gaps. Large gaps are treated as normal catch-up,
            // because a much taller peer on the same chain is not evidence of a reorg. TODO: Maybe look at this again some other day.
            bool isInitialSync = (localHeight == 0) || ((peerHeight > localHeight) && ((peerHeight - localHeight) > INITIAL_SYNC_HEIGHT_DIFF));

            // Compute penalty and adjusted peer height.
            uint64_t delay = (peerHeight > localHeight) ? (peerHeight - localHeight) : 0ULL;
            uint64_t penalty = isInitialSync ? 0ULL : FetchScheduler_ComputeReorgPenaltyBlocks(delay);
            uint64_t adjustedPeerHeight = (peerHeight > penalty) ? (peerHeight - penalty) : 0ULL;

            // Ensure we always make forward progress: if the penalty would reduce the
            // target below our current height, fetch at least the next block. This
            // lets us apply penalties for near-tip reorg risk while still allowing
            // normal syncing when the peer is ahead by a small amount.
            if (adjustedPeerHeight <= localHeight) {
                adjustedPeerHeight = localHeight + 1;
            }

            if (adjustedPeerHeight > peerHeight) {
                adjustedPeerHeight = peerHeight;
            }

            printf("syncing: peerHeight=%" PRIu64 " adjusted=%" PRIu64 " local=%" PRIu64 " penalty=%" PRIu64 "\n",
                peerHeight, adjustedPeerHeight, localHeight, penalty);

                // Windowed parallel fetch
                uint64_t start = localHeight;
                uint64_t end = adjustedPeerHeight; // exclusive target height
            uint64_t nextReq = start;

            const int maxInFlight = MAX_PARALLEL_FETCHES;
            uint64_t requestedHeights[64];
            int retryCount[64];
            uint64_t sentAtMs[64];
            int inFlight = 0;

            if (maxInFlight > (int)(sizeof(requestedHeights)/sizeof(requestedHeights[0]))) {
                printf("MAX_PARALLEL_FETCHES too large for local buffers\n");
                continue;
            }

            // Keep track of expected last-hash to detect reorgs. Initialize to our current tip.
            uint8_t expectedPrevHash[32];
            if (localHeight > 0) {
                block_t* lastBlock = NULL;
                if (Chain_GetBlockCopy(chain, localHeight - 1, &lastBlock)) {
                    Block_CalculateHash(lastBlock, expectedPrevHash);
                    Block_Destroy(lastBlock);
                } else {
                    memset(expectedPrevHash, 0, sizeof(expectedPrevHash));
                }
            } else {
                memset(expectedPrevHash, 0, sizeof(expectedPrevHash));
            }

            while (nextReq < end || inFlight > 0) {
                // Fill window
                while (inFlight < maxInFlight && nextReq < end) {
                    uint64_t req = nextReq;
                    if (Node_SendPacket(node, peerConn, PACKET_TYPE_FETCH_BLOCK, &req, sizeof(req)) != 0) {
                        printf("failed to send FETCH_BLOCK for %" PRIu64 "\n", req);
                        break;
                    }

                    requestedHeights[inFlight] = req;
                    retryCount[inFlight] = 0;
                    sentAtMs[inFlight] = get_current_time_ms();
                    inFlight++;
                    nextReq++;
                }

                // Poll for completions or timeouts
                if (inFlight == 0) {
                    // nothing in flight; small sleep to avoid busy-loop
                    sleep_for_milliseconds(100);
                    continue;
                }

                uint64_t now = get_current_time_ms();
                // Check earliest outstanding entry for completion/timeout
                bool progressed = false;
                for (int i = 0; i < inFlight; ++i) {
                    uint64_t h = requestedHeights[i];
                    if ((uint64_t)Chain_Size(chain) > h) {
                        // A new block at height h was applied. Retrieve it and verify parent.
                        block_t* fetched = NULL;
                        if (!Chain_GetBlockCopy(chain, (size_t)h, &fetched) || !fetched) {
                            // Shouldn't happen, but be robust.
                            printf("fetched block %" PRIu64 " applied but not found\n", h);
                            // remove entry
                            for (int j = i; j < inFlight - 1; ++j) {
                                requestedHeights[j] = requestedHeights[j + 1];
                                retryCount[j] = retryCount[j + 1];
                                sentAtMs[j] = sentAtMs[j + 1];
                            }
                            inFlight--;
                            progressed = true;
                            break;
                        }

                        // Check whether this block builds on our expected tip. If not, it's a reorg.
                        if (memcmp(fetched->header.prevHash, expectedPrevHash, sizeof(expectedPrevHash)) != 0) {
                            // Find matching ancestor in our current chain (if any)
                            ssize_t matchIndex = -1;
                            size_t chainSz = Chain_Size(chain);
                            uint8_t tmpHash[32];
                            for (size_t bi = 0; bi < chainSz; ++bi) {
                                block_t* b = NULL;
                                if (!Chain_GetBlockCopy(chain, bi, &b) || !b) continue;
                                Block_CalculateHash(b, tmpHash);
                                if (memcmp(tmpHash, fetched->header.prevHash, sizeof(tmpHash)) == 0) {
                                    matchIndex = (ssize_t)bi;
                                    Block_Destroy(b);
                                    break;
                                }
                                Block_Destroy(b);
                            }

                            uint64_t reorgDepth = 0ULL;
                            if (matchIndex >= 0) {
                                reorgDepth = (uint64_t)localHeight - ((uint64_t)matchIndex + 1ULL);
                            } else {
                                // No match found: treat as full reorg depth equal to localHeight
                                reorgDepth = localHeight;
                            }

                            if (!isInitialSync) {
                                uint64_t reorgPenalty = FetchScheduler_ComputeReorgPenaltyBlocks(reorgDepth);
                                printf("Reorg detected at height %" PRIu64 ": depth=%" PRIu64 " penalty=%" PRIu64 "\n",
                                       h, reorgDepth, reorgPenalty);

                                // Rollback our chain to the matching ancestor (or to 0 if none)
                                size_t rollbackTo = (matchIndex >= 0) ? (size_t)(matchIndex + 1) : 0;
                                if (!Chain_RollbackToHeight(chain, rollbackTo)) {
                                    printf("Failed to rollback to height %zu during reorg handling\n", rollbackTo);
                                    inFlight = 0; // abort sync
                                    break;
                                }

                                // Apply additional penalty by shrinking end and restart window from current Chain_Size
                                if (peerHeight > reorgPenalty) {
                                    end = peerHeight - reorgPenalty;
                                } else {
                                    end = start;
                                }
                            } else {
                                printf("Initial sync: reorg-like divergence ignored (height=%" PRIu64 ")\n", h);
                            }

                            // Free fetched block and reset window to pick up new adjusted end and expectedPrevHash
                            Block_Destroy(fetched);
                            nextReq = Chain_Size(chain);
                            inFlight = 0;
                            // Recompute expectedPrevHash to current tip
                            if (Chain_Size(chain) > 0) {
                                block_t* tip = NULL;
                                if (Chain_GetBlockCopy(chain, Chain_Size(chain) - 1, &tip) && tip) {
                                    Block_CalculateHash(tip, expectedPrevHash);
                                    Block_Destroy(tip);
                                }
                            } else {
                                memset(expectedPrevHash, 0, sizeof(expectedPrevHash));
                            }

                            progressed = true;
                            break; // restart loop
                        }

                        printf("fetched block %" PRIu64 "\n", h);
                        // Update expectedPrevHash to this fetched block's hash (for next block)
                        Block_CalculateHash(fetched, expectedPrevHash);
                        // remove entry i by shifting left
                        for (int j = i; j < inFlight - 1; ++j) {
                            requestedHeights[j] = requestedHeights[j + 1];
                            retryCount[j] = retryCount[j + 1];
                            sentAtMs[j] = sentAtMs[j + 1];
                        }
                        inFlight--;
                        progressed = true;
                        Block_Destroy(fetched);
                        break; // restart loop to re-evaluate
                    }

                    uint64_t elapsed = (now > sentAtMs[i]) ? (now - sentAtMs[i]) : 0ULL;
                    if (elapsed > SYNC_REQUEST_TIMEOUT_MS) {
                        if (retryCount[i] < MAX_SYNC_RETRIES) {
                            // retry with exponential backoff
                            retryCount[i]++;
                            uint64_t backoff = SYNC_BACKOFF_BASE_MS * (1ULL << (retryCount[i] - 1));
                            sleep_for_milliseconds(backoff);

                            uint64_t req = requestedHeights[i];
                            if (Node_SendPacket(node, peerConn, PACKET_TYPE_FETCH_BLOCK, &req, sizeof(req)) != 0) {
                                printf("retry: failed to send FETCH_BLOCK for %" PRIu64 "\n", req);
                            } else {
                                sentAtMs[i] = get_current_time_ms();
                                progressed = true;
                            }
                        } else {
                            printf("timed out fetching block %" PRIu64 ", giving up\n", requestedHeights[i]);
                            inFlight = 0; // abort sync on persistent failures
                            break;
                        }
                    }
                }

                if (!progressed) {
                    // small sleep to avoid spinning
                    sleep_for_milliseconds(50);
                }
            }

            // After the window completes, check progress and possibly refresh peer height
            uint64_t newLocal = (uint64_t)Chain_Size(chain);
            if (newLocal > localHeight) madeProgressOverall = true;
            printf("sync complete: localHeight=%" PRIu64 "\n", newLocal);

            // If we've caught up to the peer, stop. Otherwise refresh peerHeight and loop again.
            if (newLocal >= peerHeight) break;

            // Refresh advertised peer height for this connection (it may have been updated during fetch)
            pthread_mutex_lock(&node->outboundLock);
            for (size_t i = 0; i < MAX_CONS; ++i) {
                if (node->outboundClients[i].connection == peerConn) {
                    peerHeight = node->outboundClients[i].peerBlockHeight;
                    break;
                }
            }
            pthread_mutex_unlock(&node->outboundLock);

            // If no progress was made in this iteration, stop to avoid tight loop
            if (!madeProgressOverall) {
                break;
            }

            // Re-evaluate loop condition: continue while local < peerHeight
            if ((uint64_t)Chain_Size(chain) >= peerHeight) break;
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
                if (errno == ETIMEDOUT) {
                    printf("failed to connect to %s:%u (timeout)\n", ipStr, (unsigned int)LISTEN_PORT);
                } else {
                    printf("failed to connect to %s:%u\n", ipStr, (unsigned int)LISTEN_PORT);
                }
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

    }

    (void)FlushChainAndSheet(chain, chainDataDir, currentSupply, currentReward);

    Block_ShutdownPowContext();
    Node_Destroy(node);
    currentChain = NULL;
    Chain_Destroy(chain);
    BalanceSheet_Destroy();

    pthread_mutex_destroy(&balanceSheetLock);
    pthread_rwlock_destroy(&chainLock);

    return 0;
}
