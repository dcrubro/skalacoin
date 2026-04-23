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

#include <nets/net_node.h>
#include <crypto/crypto.h>

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

static bool ParseHexAddress32(const char* in, uint8_t outAddress[32]) {
    if (!in || !outAddress) {
        return false;
    }

    const char* p = in;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }

    if (strlen(p) != 64) {
        return false;
    }

    for (size_t i = 0; i < 32; ++i) {
        char hi = p[i * 2];
        char lo = p[i * 2 + 1];
        int hiVal = (hi >= '0' && hi <= '9') ? (hi - '0') :
                    (hi >= 'a' && hi <= 'f') ? (10 + hi - 'a') :
                    (hi >= 'A' && hi <= 'F') ? (10 + hi - 'A') : -1;
        int loVal = (lo >= '0' && lo <= '9') ? (lo - '0') :
                    (lo >= 'a' && lo <= 'f') ? (10 + lo - 'a') :
                    (lo >= 'A' && lo <= 'F') ? (10 + lo - 'A') : -1;

        if (hiVal < 0 || loVal < 0) {
            return false;
        }

        outAddress[i] = (uint8_t)((hiVal << 4) | loVal);
    }

    return true;
}

static void AddressToHexString(const uint8_t address[32], char out[65]) {
    if (!address || !out) {
        return;
    }
    to_hex(address, out);
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
    block->header.timestamp = (uint64_t)time(NULL);
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

static bool GenerateRandomTestAddress(uint8_t outAddress[32]) {
    if (!outAddress) {
        return false;
    }

    uint8_t privateKey[32];
    uint8_t compressedPubkey[33];
    secp256k1_pubkey pubkey;

    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    if (!ctx) {
        return false;
    }

    for (size_t attempt = 0; attempt < 4096; ++attempt) {
        for (size_t i = 0; i < sizeof(privateKey); ++i) {
            privateKey[i] = (uint8_t)(rand() & 0xFF);
        }

        if (!secp256k1_ec_seckey_verify(ctx, privateKey)) {
            continue;
        }

        if (!secp256k1_ec_pubkey_create(ctx, &pubkey, privateKey)) {
            continue;
        }

        size_t pubLen = sizeof(compressedPubkey);
        if (!secp256k1_ec_pubkey_serialize(ctx, compressedPubkey, &pubLen, &pubkey, SECP256K1_EC_COMPRESSED) || pubLen != 33) {
            continue;
        }

        AddressFromCompressedPubkey(compressedPubkey, outAddress);
        secp256k1_context_destroy(ctx);
        return true;
    }

    secp256k1_context_destroy(ctx);
    return false;
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
    for (size_t i = 0; i < chainSize; ++i) {
        block_t* blk = Chain_GetBlock(chain, i);
        if (!blk || !blk->transactions) {
            return false;
        }

        if (blk->header.blockNumber != (uint64_t)i) {
            return false;
        }

        if (i == 0) {
            uint8_t zeroHash[32] = {0};
            if (memcmp(blk->header.prevHash, zeroHash, sizeof(zeroHash)) != 0) {
                return false;
            }
        } else {
            block_t* prevBlk = Chain_GetBlock(chain, i - 1);
            if (!prevBlk) {
                return false;
            }

            uint8_t expectedPrevHash[32];
            Block_CalculateHash(prevBlk, expectedPrevHash);
            if (memcmp(blk->header.prevHash, expectedPrevHash, sizeof(expectedPrevHash)) != 0) {
                return false;
            }
        }

        if (!Block_HasValidProofOfWork(blk)) {
            return false;
        }

        if (!Block_AllTransactionsValid(blk)) {
            return false;
        }

        uint8_t expectedMerkle[32];
        Block_CalculateMerkleRoot(blk, expectedMerkle);
        if (memcmp(blk->header.merkleRoot, expectedMerkle, sizeof(expectedMerkle)) != 0) {
            return false;
        }

        // Transactions are persisted on disk. Once this block is fully verified,
        // release its in-memory transaction list to reduce peak memory usage.
        DynArr_destroy(blk->transactions);
        blk->transactions = NULL;
    }

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
    printf("Commands: mine <x>, send <address> <amount>, balance [address], flushchain, fullverify, wipechain, genaddr, exit\n");

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

            if (!MineAndAppendBlock(chain, block, &currentSupply, &currentReward, &difficultyTarget)) {
                Block_Destroy(block);
                continue;
            }

            free(block);
            printf("send committed in mined block\n");
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
                uint8_t dagSeed[32];
                GetNextDAGSeed(verifyChain, dagSeed);
                (void)Block_RebuildAutolykos2Dag(CalculateTargetDAGSize(verifyChain), dagSeed);
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

        printf("Unknown command. Available: mine, send, balance, flushchain, fullverify, wipechain, genaddr, exit\n");
    }

    (void)FlushChainAndSheet(chain, chainDataDir, currentSupply, currentReward);

    Chain_Destroy(chain);
    Block_ShutdownPowContext();
    Node_Destroy(node);
    BalanceSheet_Destroy();

    return 0;
}
