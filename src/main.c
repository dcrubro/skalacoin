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
#include <inttypes.h>
#include <numgen.h>
#include <txmempool.h>

#include <constants.h>
#include <runtime_state.h>
#include <autolykos2/autolykos2.h>

#include <nets/net_node.h>
#include <nets/nodediscovery.h>
#include <nets/fetch_scheduler.h>
#include <nets/orphan_pool.h>

#ifndef CHAIN_DATA_DIR
#define CHAIN_DATA_DIR "chain_data"
#endif

blockchain_t* currentChain = NULL;
const char* chainDataDir = CHAIN_DATA_DIR;
unsigned short listenPort = LISTEN_PORT;
bool echoPeersEnabled = ECHO_PEERS != 0;
bool forceOrphanReorgEnabled = false;
uint256_t currentSupply = {{0, 0, 0, 0}};
uint64_t currentReward = 750000000000ULL;
uint64_t localNodeId = 0; // Randomised in main() before the node comes up

// Define the synchronization primitives declared in runtime_state.h
pthread_rwlock_t chainLock;
pthread_mutex_t balanceSheetLock;

void handle_sigint(int sig) {
    printf("Caught signal %d, exiting...\n", sig);
    Block_ShutdownPowContext();
    BalanceSheet_Destroy();
    exit(0);
}

static void ApplyRuntimeConfigFromEnv(void) {
    const char* dataDir = getenv("SKALACOIN_CHAIN_DATA_DIR");
    if (dataDir && dataDir[0] != '\0') {
        chainDataDir = dataDir;
    }

    const char* portStr = getenv("SKALACOIN_LISTEN_PORT");
    if (portStr && portStr[0] != '\0') {
        char* end = NULL;
        long parsed = strtol(portStr, &end, 10);
        if (end != portStr && *end == '\0' && parsed > 0 && parsed <= 65535) {
            listenPort = (unsigned short)parsed;
        }
    }

    const char* echoStr = getenv("SKALACOIN_ECHO_PEERS");
    if (echoStr && echoStr[0] != '\0') {
        echoPeersEnabled = (strcmp(echoStr, "0") != 0);
    }

    const char* forceOrphanStr = getenv("SKALACOIN_FORCE_ORPHAN_REORG");
    if (forceOrphanStr && forceOrphanStr[0] != '\0') {
        forceOrphanReorgEnabled = (strcmp(forceOrphanStr, "0") != 0);
    }
}

uint32_t difficultyTarget = INITIAL_DIFFICULTY;

static bool MineBlock(blockchain_t* chain, block_t* block) {
    if (!chain || !block) {
        return false;
    }

    // Resolve the epoch parameters ONCE. Doing it per nonce would take chainLock millions of times
    // per block and contend with every network thread.
    size_t dagBytes = 0;
    uint8_t seed[32];
    if (!Chain_DagParamsForHeight(chain, block->header.blockNumber, &dagBytes, seed)) {
        fprintf(stderr, "failed to resolve epoch DAG parameters for height %llu\n",
            (unsigned long long)block->header.blockNumber);
        return false;
    }

    // Build the DAG for this block's epoch up front. It is only a speedup -- the check falls back
    // to deriving the same lanes from the seed -- so a machine that cannot allocate one still
    // mines, just slower.
    const uint64_t epochIndex = block->header.blockNumber / (uint64_t)EPOCH_LENGTH;
    if (!Block_EnsureAutolykos2Dag(epochIndex, dagBytes, seed)) {
        fprintf(stderr, "could not build the epoch %llu DAG (%zu bytes); mining via the slow path\n",
            (unsigned long long)epochIndex, dagBytes);
    }

    // Whatever BuildNextBlock stamped is only the time the search STARTED, so the header timestamp
    // is refreshed as we go and the nonce sweep restarts against the new header. See
    // MINING_TIMESTAMP_REFRESH_MS.
    uint64_t stampedAt = block->header.timestamp;

    for (;;) {
        for (uint64_t nonce = 0;; ++nonce) {
            block->header.nonce = nonce;
            if (Block_HasValidProofOfWorkWithParams(block, epochIndex, dagBytes, seed)) {
                return true;
            }

            if (nonce == UINT64_MAX) {
                return false;
            }

            if (((nonce + 1) % MINING_TIMESTAMP_CHECK_NONCES) != 0) {
                continue;
            }

            // Only ever move the timestamp forward. CLOCK_REALTIME can step backwards under NTP,
            // and backdating the block we are mining is exactly what the median-time-past rule
            // treats as a node faking being behind.
            const uint64_t now = get_current_time_ms();
            if (now > stampedAt && (now - stampedAt) >= MINING_TIMESTAMP_REFRESH_MS) {
                block->header.timestamp = now;
                stampedAt = now;
                break; // New header, so the nonces tried against the old one are worth retrying
            }
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

/**
 * This node's DAG-size vote, stamped into every block it mines. DAG_VOTE_GROW is the default and
 * means "let the schedule run" -- there is deliberately no vote that makes the DAG grow faster, so
 * the only thing a miner can express is to brake or reverse it. Settable at runtime via `dagvote`.
**/
static uint8_t g_dagVote = (uint8_t)DAG_VOTE_GROW;

static block_t* BuildNextBlock(blockchain_t* chain, uint32_t difficultyTarget) {
    block_t* block = Block_Create();
    if (!block) {
        return NULL;
    }

    block->header.reserved[0] = g_dagVote;
    block->header.reserved[1] = 0;
    block->header.reserved[2] = 0;
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
    coinbaseTx.transaction.timestamp = get_current_time_ms();
    Block_AddTransaction(block, &coinbaseTx);
}

static int CompareTransactionPriority(const void* lhs, const void* rhs) {
    const signed_transaction_t* left = (const signed_transaction_t*)lhs;
    const signed_transaction_t* right = (const signed_transaction_t*)rhs;

    if (left->transaction.fee > right->transaction.fee) {
        return -1;
    }
    if (left->transaction.fee < right->transaction.fee) {
        return 1;
    }

    uint8_t leftHash[32];
    uint8_t rightHash[32];
    Transaction_CalculateHash(left, leftHash);
    Transaction_CalculateHash(right, rightHash);
    return memcmp(leftHash, rightHash, sizeof(leftHash));
}

static bool BuildSpendableMempoolSelection(
    signed_transaction_t** outAcceptedTxs,
    size_t* outAcceptedCount,
    uint64_t* outTotalFees
) {
    if (!outAcceptedTxs || !outAcceptedCount || !outTotalFees) {
        return false;
    }

    *outAcceptedTxs = NULL;
    *outAcceptedCount = 0;
    *outTotalFees = 0;

    signed_transaction_t* snapshot = NULL;
    size_t snapshotCount = 0;
    if (!TxMempool_Snapshot(&snapshot, &snapshotCount)) {
        return false;
    }

    if (snapshot && snapshotCount > 1) {
        qsort(snapshot, snapshotCount, sizeof(signed_transaction_t), CompareTransactionPriority);
    }

    signed_transaction_t* acceptedTxs = NULL;
    size_t acceptedCount = 0;
    uint64_t totalFees = 0;
    bool ok = BalanceSheet_SelectSpendableTransactions(snapshot, snapshotCount, &acceptedTxs, &acceptedCount, &totalFees);
    free(snapshot);
    if (!ok) {
        free(acceptedTxs);
        return false;
    }

    *outAcceptedTxs = acceptedTxs;
    *outAcceptedCount = acceptedCount;
    *outTotalFees = totalFees;
    return true;
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

static bool ComputeHistoricalAutolykosHashFromChain(const blockchain_t* chain, const block_t* block, uint64_t blockHeight, uint8_t outHash[32]) {
    if (!chain || !block || !outHash) {
        return false;
    }

    // Same single source of truth the accept path uses, so a block that verifies here verifies
    // there. This used to be a second, independent implementation of the epoch size and seed rules
    // that disagreed with the one in constants.h -- notably at exactly height EPOCH_LENGTH.
    uint8_t seed[32];
    size_t dagBytes = 0;
    if (!Chain_DagParamsForHeight((blockchain_t*)chain, blockHeight, &dagBytes, seed)) {
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

static bool Block_GetCoinbaseAndFeeTotals(const block_t* block, uint64_t* outCoinbaseAmount, uint64_t* outTotalFees) {
    if (!block || !block->transactions || !outCoinbaseAmount || !outTotalFees) {
        return false;
    }

    bool hasCoinbase = false;
    uint64_t coinbaseAmount = 0;
    uint64_t totalFees = 0;

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
            coinbaseAmount = tx->transaction.amount1;
            continue;
        }

        if (UINT64_MAX - totalFees < tx->transaction.fee) {
            return false;
        }
        totalFees += tx->transaction.fee;
    }

    if (!hasCoinbase) {
        return false;
    }

    *outCoinbaseAmount = coinbaseAmount;
    *outTotalFees = totalFees;
    return true;
}

/**
 * Ask `peerConn` for its blocks in [topHeight - REORG_FETCH_DEPTH, topHeight], newest first.
 *
 * Replies arrive asynchronously as BLOCK_DATA and go through Node_ParseAndAcceptBlock, which routes
 * blocks below our tip into the orphan pool rather than dropping them. The pool then locates the
 * common ancestor by prevHash linkage and Chain_ReplaceBranch decides whether to adopt. FETCH_BLOCK
 * already answers from the peer's own chain, so finding a fork needs no new packet type.
**/
/**
 * Walk backwards from `topHeight` pulling the peer's blocks until we reach common ground, so the
 * orphan pool holds a branch that links to a block we already have.
 *
 * Two things this does that a flat "ask for REORG_FETCH_DEPTH blocks and sleep" did not:
 *
 *  - It STOPS at the fork point. A block the peer returns that we already hold means the chains
 *    agree there and everything below is shared, so there is nothing left to ask for. The old
 *    version always requested the full depth; on a shallow fork the overwhelming majority came
 *    back as duplicates, were discarded without even entering the pool, and cost the peer a full
 *    block send each.
 *
 *  - It waits on delivery receipts instead of a fixed sleep. The flat wait was a race against the
 *    peer's serving rate -- at ~10 blocks/s a 128-block window cannot land in 5s -- so the attach
 *    that followed ran against a half-filled pool and reported a failure that was not real.
 *
 * Returns true if a shared block was found (so the pool should have a linkable branch).
**/
static bool RequestForkWindow(net_node_t* node, tcp_connection_t* peerConn, uint64_t topHeight) {
    if (!node || !peerConn) {
        return false;
    }

    const uint64_t floorHeight = (topHeight > REORG_FETCH_DEPTH) ? (topHeight - REORG_FETCH_DEPTH) : 0ULL;
    printf("Walking back from %" PRIu64 " (floor %" PRIu64 ") to locate the fork point\n",
        topHeight, floorHeight);

    uint64_t batch[64];
    node_delivery_status_t status[64];
    bool got[64];
    int batchMax = MAX_PARALLEL_FETCHES;
    if (batchMax > (int)(sizeof(batch) / sizeof(batch[0]))) {
        batchMax = (int)(sizeof(batch) / sizeof(batch[0]));
    }

    uint64_t totalRequested = 0;
    uint64_t next = topHeight;
    bool exhausted = false;

    while (!exhausted) {
        int batchCount = 0;
        while (batchCount < batchMax) {
            uint64_t req = next;
            if (Node_SendPacket(node, peerConn, PACKET_TYPE_FETCH_BLOCK, &req, sizeof(req)) != 0) {
                exhausted = true;
                break;
            }
            batch[batchCount] = req;
            got[batchCount] = false;
            status[batchCount] = NODE_DELIVERY_REJECTED;
            batchCount++;
            totalRequested++;

            if (req == floorHeight) {
                exhausted = true;
                break;
            }
            next = req - 1ULL;
        }

        if (batchCount == 0) {
            break;
        }

        // Wait for this batch specifically, rather than guessing how long the peer needs.
        const uint64_t deadline = get_current_time_ms() + SYNC_REQUEST_TIMEOUT_MS;
        int outstanding = batchCount;
        while (outstanding > 0 && get_current_time_ms() < deadline) {
            for (int i = 0; i < batchCount; ++i) {
                if (!got[i] && Node_TakeBlockDelivery(batch[i], &status[i])) {
                    got[i] = true;
                    outstanding--;
                }
            }
            if (outstanding > 0) {
                sleep_for_milliseconds(20);
            }
        }

        // The batch descends, so the first height we already hold is the boundary between the
        // shared prefix and the competing branch.
        for (int i = 0; i < batchCount; ++i) {
            if (got[i] && (status[i] == NODE_DELIVERY_DUPLICATE || status[i] == NODE_DELIVERY_APPENDED)) {
                printf("fork walk: chains agree at height %" PRIu64 " after %" PRIu64 " request(s)\n",
                    batch[i], totalRequested);
                return true;
            }
        }

        if (outstanding > 0) {
            printf("fork walk: %d of %d block(s) unanswered; stopping the descent\n", outstanding, batchCount);
            return false;
        }
    }

    printf("fork walk: no shared block within %" PRIu64 " request(s) of height %" PRIu64 "\n",
        totalRequested, topHeight);
    return false;
}

static bool MineAndAppendBlock(blockchain_t* chain,
                               block_t* block,
                               uint256_t* currentSupply,
                               uint64_t* currentReward) {
    if (!chain || !block || !currentSupply || !currentReward) {
        return false;
    }

    uint8_t merkleRoot[32];
    Block_CalculateMerkleRoot(block, merkleRoot);
    memcpy(block->header.merkleRoot, merkleRoot, sizeof(block->header.merkleRoot));

    if (!MineBlock(chain, block)) {
        fprintf(stderr, "failed to mine block within nonce range\n");
        return false;
    }

    // Read the coinbase BEFORE handing the block to the chain. Chain_AddBlock takes ownership of
    // the transaction array and clears our pointer to it, so this has to happen first.
    uint64_t coinbaseAmount = 0;
    if (block->transactions && DynArr_size(block->transactions) > 0) {
        signed_transaction_t* firstTx = (signed_transaction_t*)DynArr_at(block->transactions, 0);
        if (firstTx && Address_IsCoinbase(firstTx->transaction.senderAddress)) {
            coinbaseAmount = firstTx->transaction.amount1;
        }
    }

    if (!Chain_AddBlock(chain, block)) {
        fprintf(stderr, "failed to append block to chain\n");
        return false;
    }

    /* Debug proof removed: miner printed proof that coinbase == baseReward + totalFees during debugging. */

    // After successfully appending a block, attempt to attach any orphans.
    size_t attached = OrphanPool_AttemptAttach(chain);
    if (attached > 0) {
        printf("Attached %zu orphan(s) after mining/appending block\n", attached);
        // Persist chain/sheet after attaching orphans
        Chain_SaveToFile(chain, chainDataDir, *currentSupply, *currentReward);
        BalanceSheet_SaveToFile(chainDataDir);
    }

    uint8_t canonicalHash[32];
    uint8_t powHash[32];
    Block_CalculateHash(block, canonicalHash);
    memset(powHash, 0, sizeof(powHash));
    {
        // For the log line only. Resolved fresh rather than carried out of MineBlock because the
        // block is on the chain by now, so this also confirms it hashes the same from the tip.
        size_t dagBytes = 0;
        uint8_t seed[32];
        if (Chain_DagParamsForHeight(chain, block->header.blockNumber, &dagBytes, seed)) {
            (void)Block_PowHashLight(block, dagBytes, seed, powHash);
        }
    }

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

    // Supply, reward, the difficulty retarget and the epoch DAG rebuild all happen inside
    // Chain_AddBlock, so that blocks we receive from peers advance them exactly like ones we mine.

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

    uint256_t replaySupply = uint256_from_u64(0);
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

        // Determine expected difficulty for this block. The retarget window (i-INTERVAL..i-1) is
        // already present in `chain`, so it reads from there rather than the replay copy.
        if (i < DIFFICULTY_ADJUSTMENT_INTERVAL) {
            expectedDifficulty = INITIAL_DIFFICULTY;
        } else if ((i % DIFFICULTY_ADJUSTMENT_INTERVAL) == 0) {
            expectedDifficulty = Chain_ComputeTargetAtHeight(chain, (uint64_t)i, expectedDifficulty);
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

        uint64_t expectedReward = 0;
        uint64_t savedReward = currentReward;
        expectedReward = CalculateBlockReward(replaySupply, prevChain);
        currentReward = savedReward;

        if (!Block_AllTransactionsValid(blk)) {
            Block_Destroy(blk);
            Chain_Destroy(prevChain);
            return false;
        }

        uint64_t coinbaseAmount = 0;
        uint64_t totalFees = 0;
        if (!Block_GetCoinbaseAndFeeTotals(blk, &coinbaseAmount, &totalFees)) {
            Block_Destroy(blk);
            Chain_Destroy(prevChain);
            return false;
        }

        if (UINT64_MAX - expectedReward < totalFees || coinbaseAmount != (expectedReward + totalFees)) {
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

        (void)uint256_add_u64(&replaySupply, coinbaseAmount);

        Block_Destroy(blk);
    }

    Chain_Destroy(prevChain);
    return true;
}

// Use when error
void KillEverythingAndExit(net_node_t* node, blockchain_t* chain) {
    Node_Destroy(node);
    currentChain = NULL;
    Chain_Destroy(chain);
    Block_ShutdownPowContext();
    BalanceSheet_Destroy();
    exit(1);
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

    ApplyRuntimeConfigFromEnv();

    signal(SIGINT, handle_sigint);
    // Ignore SIGPIPE so a write to a socket whose peer has already disconnected returns EPIPE
    // (handled by the send paths) instead of terminating the whole process. Peers connecting and
    // disconnecting is normal p2p behaviour and must never take the node down.
    signal(SIGPIPE, SIG_IGN);
    // Mix the pid into the seed: nodes launched within the same second would otherwise draw
    // identical sequences, so every rand()-derived value (connection ids and the like) would
    // collide across them.
    srand((unsigned int)time(NULL) ^ ((unsigned int)getpid() << 16));

    // Pick this run's node identity before the node (and with it the listener) comes up, so every
    // handshake can carry it. Peers are identified by this nonce rather than by an (ip, port)
    // endpoint, which a multi-homed host has several of.
    localNodeId = random_secure_eight_byte();
    printf("Node identity: %016" PRIx64 "\n", localNodeId);

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
    } else {
        // Recompute runtime supply/reward from loaded blocks to avoid trusting stale meta values.
        if (!Chain_RecomputeRuntimeState(chain)) {
            fprintf(stderr, "Failed to recompute runtime state from loaded chain\n");
        }

        // chain.meta stores the tip's own target, which is not the next block's target when the tip
        // sits on an adjustment boundary. Derive it from the chain instead.
        Chain_OnTipAdvanced(chain);
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
        // Report the epoch parameters the next block will use. The DAG itself is NOT generated
        // here: it is a mining accelerator, so MineBlock builds it on demand and a node that never
        // mines never pays for it. This used to build one from a seed derived from the tip block
        // rather than the epoch boundary, which meant a node restarted mid-epoch mined against a
        // different DAG than one that had run straight through the boundary.
        size_t dagBytes = 0;
        uint8_t dagSeed[32];
        const uint64_t nextHeight = (uint64_t)Chain_Size(chain);
        if (Chain_DagParamsForHeight(chain, nextHeight, &dagBytes, dagSeed)) {
            printf("Epoch %llu DAG: seed %02x%02x%02x%02x... size %zu bytes\n",
                (unsigned long long)(nextHeight / (uint64_t)EPOCH_LENGTH),
                dagSeed[0], dagSeed[1], dagSeed[2], dagSeed[3],
                dagBytes);
        } else {
            fprintf(stderr, "Failed to resolve epoch DAG parameters for height %llu\n",
                (unsigned long long)nextHeight);
        }
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

    // TODO: Separate loading into its own header
    // Load the wallet from disk or generate new random identity 
    
    uint8_t minerAddress[32];
    uint8_t minerPrivateKey[32];
    uint8_t minerCompressedPubkey[33];
    bool loadedWallet = false;

    // Attempt load
    char* path = "chain_data/wallet.data"; // TODO: Don't hardcode path
    FILE* walletFile = fopen(path, "rb");
    if (walletFile) {
        size_t read = fread(minerPrivateKey, 1, 32, walletFile);
        if (read != 32) {
            fprintf(stderr, "failed to read wallet file\n");
            fclose(walletFile);
        }

        read = fread(minerCompressedPubkey, 1, 33, walletFile);
        if (read != 33) {
            fprintf(stderr, "failed to read wallet file\n");
            fclose(walletFile);
        }

        read = fread(minerAddress, 1, 32, walletFile);
        if (read != 32) {
            fprintf(stderr, "failed to read wallet file\n");
            fclose(walletFile);
        }

        fclose(walletFile);
        loadedWallet = true;
    } else if (errno != ENOENT || errno != EISDIR || errno != EACCES || errno != EROFS || !loadedWallet) {
        fprintf(stderr, "failed to open wallet file: %s\n generating new wallet...\n", strerror(errno));
        if (!GenerateRandomTestAddress(minerAddress, minerPrivateKey, minerCompressedPubkey)) {
            fprintf(stderr, "failed to generate test miner keypair\n");
            KillEverythingAndExit(node, chain);
        }

        // Save the generated wallet to disk for future runs
        walletFile = fopen(path, "wb");
        if (!walletFile) {
            fprintf(stderr, "failed to create wallet file: %s\n", strerror(errno));
            KillEverythingAndExit(node, chain); 
        }

        size_t written = fwrite(minerPrivateKey, 1, 32, walletFile);
        if (written != 32) {
            fprintf(stderr, "failed to write wallet file\n");
            fclose(walletFile);
            KillEverythingAndExit(node, chain);
        }

        written = fwrite(minerCompressedPubkey, 1, 33, walletFile);
        if (written != 33) {
            fprintf(stderr, "failed to write wallet file\n");
            fclose(walletFile);
            KillEverythingAndExit(node, chain);
        }

        written = fwrite(minerAddress, 1, 32, walletFile);
        if (written != 32) {
            fprintf(stderr, "failed to write wallet file\n");
            fclose(walletFile);
            KillEverythingAndExit(node, chain);
        }

        fclose(walletFile);
    }

    /*uint8_t minerAddress[32];
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
    }*/

    char minerAddressHex[65];
    AddressToHexString(minerAddress, minerAddressHex);
    printf("Test miner address: %s\n", minerAddressHex);

    char supplyStr[80];
    Uint256ToDecimal(&currentSupply, supplyStr, sizeof(supplyStr));
    printf("Current chain has %zu blocks, total supply %s\n", Chain_Size(chain), supplyStr);
    printf("Commands: mine <x>, send <address> <amount> [fee], txpooldetail <txhash>, balance [address], connect <ipv4> [port], peers, sync [force] (requires nodes), dagvote <grow|hold|down>, flushchain, fullverify, blockdetail <block number>, wipechain, genaddr, exit\n");

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

        if (strcmp(cmd, "dagvote") == 0) {
            char* voteStr = strtok(NULL, " \t");
            if (!voteStr) {
                printf("dag vote is %u (%s)\n", (unsigned)g_dagVote,
                    g_dagVote == DAG_VOTE_HOLD ? "hold" : (g_dagVote == DAG_VOTE_DOWN ? "down" : "grow"));
                printf("usage: dagvote <grow|hold|down>\n");
                continue;
            }

            if (strcmp(voteStr, "grow") == 0) {
                g_dagVote = (uint8_t)DAG_VOTE_GROW;
            } else if (strcmp(voteStr, "hold") == 0) {
                g_dagVote = (uint8_t)DAG_VOTE_HOLD;
            } else if (strcmp(voteStr, "down") == 0) {
                g_dagVote = (uint8_t)DAG_VOTE_DOWN;
            } else {
                printf("usage: dagvote <grow|hold|down>\n");
                continue;
            }

            // Only affects blocks this node mines from here on; it cannot change how already-mined
            // blocks are counted, since the vote is committed to inside the hashed header.
            printf("dag vote set to %s\n", voteStr);
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
                signed_transaction_t* acceptedTxs = NULL;
                size_t acceptedTxCount = 0;
                uint64_t totalFees = 0;
                if (!BuildSpendableMempoolSelection(&acceptedTxs, &acceptedTxCount, &totalFees)) {
                    fprintf(stderr, "failed to select spendable transactions from mempool\n");
                    minedAll = false;
                    break;
                }

                block_t* block = BuildNextBlock(chain, Chain_GetTargetForHeight(chain, (uint64_t)Chain_Size(chain)));
                if (!block) {
                    fprintf(stderr, "failed to create block\n");
                    free(acceptedTxs);
                    minedAll = false;
                    break;
                }

                uint64_t coinbaseAmount = currentReward;
                if (UINT64_MAX - coinbaseAmount < totalFees) {
                    free(acceptedTxs);
                    Block_Destroy(block);
                    minedAll = false;
                    break;
                }
                coinbaseAmount += totalFees;

                AddCoinbaseTransaction(block, minerAddress, coinbaseAmount);

                for (size_t txIndex = 0; txIndex < acceptedTxCount; ++txIndex) {
                    Block_AddTransaction(block, &acceptedTxs[txIndex]);
                }
                free(acceptedTxs);

                if (!MineAndAppendBlock(chain, block, &currentSupply, &currentReward)) {
                    Block_Destroy(block);
                    minedAll = false;
                    break;
                }

                Block_Destroy(block); // Chain_AddBlock already took the transaction array.

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
            char* feeStr = strtok(NULL, " \t");
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

            unsigned long long fee = 0;
            if (feeStr) {
                char* endptr2 = NULL;
                fee = strtoull(feeStr, &endptr2, 10);
                if (*feeStr == '\0' || feeStr[0] == '-' || (endptr2 && *endptr2 != '\0')) {
                    printf("invalid fee\n");
                    continue;
                }
            }

            if (fee > UINT64_MAX - amount) {
                printf("invalid fee: overflow\n");
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

            block_t* block = BuildNextBlock(chain, Chain_GetTargetForHeight(chain, (uint64_t)Chain_Size(chain)));
            if (!block) {
                fprintf(stderr, "failed to create block\n");
                continue;
            }

            uint64_t coinbaseAmount = currentReward;
            AddCoinbaseTransaction(block, minerAddress, coinbaseAmount);

            signed_transaction_t spendTx;
            Transaction_Init(&spendTx);
            spendTx.transaction.version = 1;
            spendTx.transaction.fee = (uint64_t)fee;
            spendTx.transaction.amount1 = (uint64_t)amount;
            spendTx.transaction.amount2 = 0;
            spendTx.transaction.timestamp = get_current_time_ms();
            memcpy(spendTx.transaction.senderAddress, minerAddress, sizeof(minerAddress));
            memcpy(spendTx.transaction.recipientAddress1, recipientAddress, sizeof(recipientAddress));
            memset(spendTx.transaction.recipientAddress2, 0, sizeof(spendTx.transaction.recipientAddress2));
            memcpy(spendTx.transaction.compressedPublicKey, minerCompressedPubkey, sizeof(minerCompressedPubkey));
            Transaction_Sign(&spendTx, minerPrivateKey);

            /*
            Block_AddTransaction(block, &spendTx);
            printf("Created transaction sending %llu pebble(s) to ", (unsigned long long)amount);
            char recipientHex[65];
            AddressToHexString(recipientAddress, recipientHex);
            printf("%s\n\nMining block...\n", recipientHex);
            
            if (!MineAndAppendBlock(chain, block, &currentSupply, &currentReward)) {
                Block_Destroy(block);
                continue;
            }

            FlushChainAndSheet(chain, chainDataDir, currentSupply, currentReward);

            Block_Destroy(block); // the chain took the transaction array; this frees the wrapper
            if (node) {
                Node_BroadcastChainRange(node, Chain_Size(chain) - 1, NULL);
            }
            printf("send committed in mined block\n");
            */

            // Insert into txmempool
            if (TxMempool_Insert(spendTx) < 0) {
                printf("failed to add transaction to mempool, transaction rejected\n");
                continue;
            }
            
            printf("transaction added to mempool, broadcasting...\n");

            if (Node_BroadcastTransaction(node, &spendTx, NULL) == 0) {
                printf("transaction broadcast to peers\n");
            } else {
                printf("failed to broadcast transaction to peers\n");
            }

            continue;
        }

        if (strcmp(cmd, "sync") == 0) {
            if (!node) {
                printf("no node available\n");
                continue;
            }

            // `sync force` skips the reorg delay penalty for this sync only. The penalty is served
            // by local chain growth, so a node that is neither mining nor stale enough to count as
            // catching up cannot clear it on its own -- this is the operator's way out when they
            // know their branch is the wrong one. Work comparison and linkage still apply.
            bool forceSync = false;
            {
                const char* syncArg = strtok(NULL, " \t");
                if (syncArg && strcmp(syncArg, "force") == 0) {
                    forceSync = true;
                } else if (syncArg) {
                    printf("usage: sync [force]\n");
                    continue;
                }
            }
            if (forceSync) {
                printf("sync force: the reorg delay penalty will be skipped for this sync\n");
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
            int forkProbes = 0;

            // Fork state for this sync. The backward walk runs once; after that the window keeps
            // marching forward so the competing branch accumulates in the pool, which is what lets
            // its length past our tip satisfy the reorg delay.
            bool forkPointLocated = false;
            uint64_t pooledSinceAttach = 0;

            // Drop receipts left over from an earlier sync, so a stale one cannot be mistaken for
            // an answer to a request this run has not sent yet.
            Node_ResetBlockDeliveries();

            while (true) {
                uint64_t localHeight = (uint64_t)Chain_Size(chain);

            // Whether we are catching up rather than following the tip. Derived from our own chain
            // only: this used to key off the peer's advertised height, which let any peer claiming
            // localHeight + INITIAL_SYNC_HEIGHT_DIFF switch off reorg handling for the session.
            bool isInitialSync = Chain_IsInitialBlockDownload(chain);

            // The reorg penalty is NOT applied to the fetch window. It is a delay on adopting a
            // competing branch (enforced in Chain_ReplaceBranch), not on catching up: penalizing
            // the height gap to a peer only throttled honest sync, and for gaps of 4-50 it
            // collapsed the window to a single block per pass.
            printf("syncing: peerHeight=%" PRIu64 " local=%" PRIu64 " initialSync=%s\n",
                peerHeight, localHeight, isInitialSync ? "yes" : "no");

                // Windowed parallel fetch
                uint64_t start = localHeight;
                uint64_t end = peerHeight; // exclusive target height
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
                            // Ask the peer for a window of blocks below the divergence so the orphan
                            // pool can assemble its branch and find the true common ancestor by
                            // prevHash linkage. FETCH_BLOCK answers from the peer's own chain, and
                            // Node_ParseAndAcceptBlock now routes sub-tip blocks into the pool
                            // instead of dropping them, so no protocol change is needed.
                            //
                            // We deliberately do NOT roll back here. The swap happens in
                            // Chain_ReplaceBranch, which compares cumulative work, enforces the
                            // Horizen reorg penalty, and restores our chain if the branch fails to
                            // apply. The old code rolled back to height 0 whenever it could not find
                            // the parent -- a full chain wipe, genesis included, that any peer could
                            // trigger with a single unlinked block.
                            printf("Divergence at height %" PRIu64 "; probing for the fork point\n", h);
                            RequestForkWindow(node, peerConn, h);

                            size_t reattached = OrphanPool_AttemptAttachForced(chain, forceSync);
                            if (reattached > 0) {
                                printf("Reorg attached %zu block(s) from the peer's branch\n", reattached);
                            } else {
                                printf("Reorg candidate not adopted (lighter branch, or still serving its reorg penalty)\n");
                            }

                            // Free fetched block and reset the window against whatever our tip is now
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

                    // The peer answered, but the block never joined our chain -- it is on a
                    // competing branch and now sits in the orphan pool. Retrying cannot change
                    // that, and the old code could not tell this case from a dropped packet: it
                    // burned MAX_SYNC_RETRIES plus a timeout on EVERY block, then slid the window
                    // forward and did it again, which is what made syncing to a forked peer crawl
                    // and made the peer re-serve the whole chain several times over.
                    node_delivery_status_t deliveryStatus = NODE_DELIVERY_REJECTED;
                    if (Node_TakeBlockDelivery(h, &deliveryStatus) && deliveryStatus != NODE_DELIVERY_APPENDED) {
                        // Locate the fork point ONCE. After that the branch just needs to keep
                        // accumulating: the delay is satisfied by the branch extending past our tip
                        // (see Chain_ReplaceBranch), so the window must march FORWARD pooling
                        // blocks. Resetting it to our tip on every forked delivery -- which is what
                        // this used to do -- re-requested the same eight heights forever, so the
                        // pool never grew past the window size and a node that was far behind could
                        // never accumulate enough of the branch to adopt it.
                        if (!forkPointLocated) {
                            forkProbes++;
                            printf("block %" PRIu64 " arrived but does not extend our chain; "
                                   "probing for the fork point\n", h);

                            forkPointLocated = RequestForkWindow(node, peerConn, h);
                            if (!forkPointLocated) {
                                printf("No shared block found with this peer within %" PRIu64
                                       " blocks; its branch cannot be linked to ours\n", REORG_FETCH_DEPTH);
                                inFlight = 0;
                                nextReq = end; // this peer is unreachable by extension
                                break;
                            }
                        }

                        // Delivered, so drop it from the in-flight set and let the window advance.
                        for (int j = i; j < inFlight - 1; ++j) {
                            requestedHeights[j] = requestedHeights[j + 1];
                            retryCount[j] = retryCount[j + 1];
                            sentAtMs[j] = sentAtMs[j + 1];
                        }
                        inFlight--;
                        pooledSinceAttach++;
                        progressed = true;

                        // Retry adoption as the branch grows, rather than once per probe. Each
                        // attempt walks the pool, so do it per window-full instead of per block.
                        if (pooledSinceAttach >= (uint64_t)maxInFlight) {
                            pooledSinceAttach = 0;
                            size_t reattached = OrphanPool_AttemptAttachForced(chain, forceSync);
                            if (reattached > 0) {
                                printf("Reorg attached %zu block(s) from the peer's branch\n", reattached);

                                // The chain moved; realign the window and the expected parent hash.
                                nextReq = Chain_Size(chain);
                                inFlight = 0;
                                forkPointLocated = false; // a further divergence would need a new walk
                                if (Chain_Size(chain) > 0) {
                                    block_t* tip = NULL;
                                    if (Chain_GetBlockCopy(chain, Chain_Size(chain) - 1, &tip) && tip) {
                                        Block_CalculateHash(tip, expectedPrevHash);
                                        Block_Destroy(tip);
                                    }
                                } else {
                                    memset(expectedPrevHash, 0, sizeof(expectedPrevHash));
                                }
                            }
                        }

                        break;
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

            // The window drained. Anything pooled since the last attempt has not been tried yet --
            // without this a branch whose final partial batch never reached the retry threshold
            // would sit in the pool unadopted until the maintenance thread happened to pick it up.
            if (pooledSinceAttach > 0) {
                pooledSinceAttach = 0;
                size_t reattached = OrphanPool_AttemptAttachForced(chain, forceSync);
                if (reattached > 0) {
                    printf("Reorg attached %zu block(s) from the peer's branch\n", reattached);
                }
            }

            // After the window completes, check progress and possibly refresh peer height.
            // This flag tracks THIS iteration only: it used to be set once and never cleared, so
            // after a single productive pass the "no progress -> stop" guard below could never fire
            // again and the outer loop could spin forever holding the REPL.
            uint64_t newLocal = (uint64_t)Chain_Size(chain);
            madeProgressOverall = (newLocal > localHeight);
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

            // If no progress was made in this iteration, stop to avoid a tight loop -- but first
            // consider that the peer may be ahead on a branch that forks BELOW our tip. In that
            // case every block we asked for is unappendable and lands in the orphan pool, so the
            // window completes having achieved nothing. Probe downwards for the fork point before
            // giving up; this is the only trigger that fires for a genuine sub-tip fork, because
            // the divergence check above can only see blocks that made it into our chain.
            if (!madeProgressOverall) {
                if (peerHeight > newLocal && forkProbes < MAX_FORK_PROBE_ROUNDS) {
                    forkProbes++;
                    printf("No progress but peer is ahead (%" PRIu64 " > %" PRIu64 "); probing for a fork point\n",
                        peerHeight, newLocal);
                    const bool foundCommon = RequestForkWindow(node, peerConn, newLocal);

                    size_t attached = foundCommon ? OrphanPool_AttemptAttachForced(chain, forceSync) : 0;
                    if (attached > 0) {
                        printf("Fork probe adopted %zu block(s) from the peer's branch\n", attached);
                        continue;
                    }
                    if (!foundCommon) {
                        printf("Fork probe found no shared block with this peer\n");
                    } else {
                        printf("Fork probe did not complete a reorg; branch stays pooled for retry\n");
                    }
                }
                break;
            }

            // Re-evaluate loop condition: continue while local < peerHeight
            if ((uint64_t)Chain_Size(chain) >= peerHeight) break;
            continue;
            }

            // Sync loop finished with this peer; release the pin taken by Node_GetBestOutboundPeer so
            // the reaper may reclaim the slot if the peer has since disconnected.
            TcpConnection_Unpin(peerConn);
            continue;
        }

        if (strcmp(cmd, "txpooldetail") == 0) {
            char* hashStr = strtok(NULL, " \t");
            if (!hashStr) {
                printf("usage: txpooldetail <txhash>\n");
                continue;
            }

            uint8_t txHash[32];
            if (!ParseHexAddress32(hashStr, txHash)) {
                printf("invalid tx hash: expected 64 hex chars\n");
                continue;
            }

            signed_transaction_t tx;
            if (!TxMempool_Lookup(txHash, &tx)) {
                printf("transaction not found in mempool\n");
                continue;
            }

            char senderHex[65];
            char recip1Hex[65];
            char recip2Hex[65];
            AddressToHexString(tx.transaction.senderAddress, senderHex);
            AddressToHexString(tx.transaction.recipientAddress1, recip1Hex);
            AddressToHexString(tx.transaction.recipientAddress2, recip2Hex);

            uint8_t calcHash[32];
            Transaction_CalculateHash(&tx, calcHash);

            printf("Transaction details:\n");
            printf("  TxHash: "); PrintHexBytes(calcHash, 32); printf("\n");
            printf("  Sender: %s%s\n", senderHex, Address_IsCoinbase(tx.transaction.senderAddress) ? " (coinbase)" : "");
            printf("  Recipient1: %s\n", recip1Hex);
            printf("  Recipient2: %s\n", recip2Hex);
            printf("  Amount1: %llu\n", (unsigned long long)tx.transaction.amount1);
            printf("  Amount2: %llu\n", (unsigned long long)tx.transaction.amount2);
            printf("  Fee: %llu\n", (unsigned long long)tx.transaction.fee);
            printf("  Timestamp: %llu\n", (unsigned long long)tx.transaction.timestamp);
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
            char* portStr = strtok(NULL, " \t");
            char* extra = strtok(NULL, " \t");
            if (!ipStr || extra) {
                printf("usage: connect <ipv4/ipv6> [port]\n");
                continue;
            }

            if (!IsValidIPv4(ipStr) && !IsValidIPv6(ipStr)) {
                printf("invalid IPv4 or IPv6 address\n");
                continue;
            }

            unsigned short peerPort = listenPort;
            if (portStr) {
                char* end = NULL;
                long parsedPort = strtol(portStr, &end, 10);
                if (*portStr == '\0' || portStr[0] == '-' || (end && *end != '\0') || parsedPort <= 0 || parsedPort > 65535) {
                    printf("invalid port\n");
                    continue;
                }
                peerPort = (unsigned short)parsedPort;
                if (strtok(NULL, " \t")) {
                    printf("usage: connect <ipv4/ipv6> [port]\n");
                    continue;
                }
            }

            if (Node_ConnectPeer(node, ipStr, peerPort) != 0) {
                if (errno == ETIMEDOUT) {
                    printf("failed to connect to %s:%u (timeout)\n", ipStr, (unsigned int)peerPort);
                } else {
                    printf("failed to connect to %s:%u\n", ipStr, (unsigned int)peerPort);
                }
                continue;
            }

            printf("connect requested to %s:%u\n", ipStr, (unsigned int)peerPort);
            continue;
        }

        if (strcmp(cmd, "peers") == 0) {
            if (strtok(NULL, " \t")) {
                printf("usage: peers\n");
                continue;
            }
            NodeDiscovery_PrintPeers(node->discovery);
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

            // No DAG rebuild needed: Chain_Wipe drops the memoised epoch table, and MineBlock
            // rebuilds the DAG on demand for whatever epoch it next mines in.

            printf("chain data wiped\n");
            continue;
        }

        if (strcmp(cmd, "genaddr") == 0) {
            uint8_t testAddress[32];
            if (!GenerateRandomTestAddress(testAddress, NULL, NULL)) {
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

        printf("Unknown command. Available: mine, send, sync, txpooldetail, blockdetail, balance, connect, peers, flushchain, fullverify, wipechain, genaddr, exit\n");
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
