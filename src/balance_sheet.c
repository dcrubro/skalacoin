#include <balance_sheet.h>
#include <pthread.h>

khash_t(balance_sheet_map_m)* sheetMap = NULL;
static pthread_mutex_t g_sheetLock;

static bool BalanceSheet_GetSimEntry(
    khash_t(balance_sheet_map_m)* simMap,
    const uint8_t address[32],
    balance_sheet_entry_t* out
) {
    if (!simMap || !address || !out) {
        return false;
    }

    key32_t key;
    memcpy(key.bytes, address, 32);

    khiter_t k = kh_get(balance_sheet_map_m, simMap, key);
    if (k != kh_end(simMap)) {
        *out = kh_value(simMap, k);
        return true;
    }

    if (BalanceSheet_Lookup((uint8_t*)address, out)) {
        int ret = 0;
        k = kh_put(balance_sheet_map_m, simMap, key, &ret);
        if (k == kh_end(simMap)) {
            return false;
        }
        kh_value(simMap, k) = *out;
        return true;
    }

    memset(out, 0, sizeof(*out));
    memcpy(out->address, address, 32);
    out->balance = uint256_from_u64(0);

    int ret = 0;
    k = kh_put(balance_sheet_map_m, simMap, key, &ret);
    if (k == kh_end(simMap)) {
        return false;
    }

    kh_value(simMap, k) = *out;
    return true;
}

static bool BalanceSheet_StoreSimEntry(
    khash_t(balance_sheet_map_m)* simMap,
    const balance_sheet_entry_t* entry
) {
    if (!simMap || !entry) {
        return false;
    }

    key32_t key;
    memcpy(key.bytes, entry->address, 32);

    int ret = 0;
    khiter_t k = kh_put(balance_sheet_map_m, simMap, key, &ret);
    if (k == kh_end(simMap)) {
        return false;
    }

    kh_value(simMap, k) = *entry;
    return true;
}

static bool BalanceSheet_ApplyCandidateTransaction(
    khash_t(balance_sheet_map_m)* simMap,
    const signed_transaction_t* tx,
    uint64_t* outFee
) {
    if (!simMap || !tx) {
        return false;
    }

    if (Address_IsCoinbase(tx->transaction.senderAddress)) {
        return true;
    }

    if (!Transaction_Verify(tx)) {
        return false;
    }

    balance_sheet_entry_t senderEntry;
    if (!BalanceSheet_GetSimEntry(simMap, tx->transaction.senderAddress, &senderEntry)) {
        return false;
    }

    uint256_t spend = uint256_from_u64(0);
    if (uint256_add_u64(&spend, tx->transaction.amount1) ||
        uint256_add_u64(&spend, tx->transaction.amount2) ||
        uint256_add_u64(&spend, tx->transaction.fee)) {
        return false;
    }

    if (uint256_cmp(&senderEntry.balance, &spend) < 0) {
        return false;
    }

    if (!uint256_subtract(&senderEntry.balance, &spend)) {
        return false;
    }
    if (!BalanceSheet_StoreSimEntry(simMap, &senderEntry)) {
        return false;
    }

    balance_sheet_entry_t recipient1Entry;
    if (!BalanceSheet_GetSimEntry(simMap, tx->transaction.recipientAddress1, &recipient1Entry)) {
        return false;
    }
    if (uint256_add_u64(&recipient1Entry.balance, tx->transaction.amount1)) {
        return false;
    }
    if (!BalanceSheet_StoreSimEntry(simMap, &recipient1Entry)) {
        return false;
    }

    if (tx->transaction.amount2 > 0) {
        balance_sheet_entry_t recipient2Entry;
        if (!BalanceSheet_GetSimEntry(simMap, tx->transaction.recipientAddress2, &recipient2Entry)) {
            return false;
        }
        if (uint256_add_u64(&recipient2Entry.balance, tx->transaction.amount2)) {
            return false;
        }
        if (!BalanceSheet_StoreSimEntry(simMap, &recipient2Entry)) {
            return false;
        }
    }

    if (outFee) {
        *outFee = tx->transaction.fee;
    }

    return true;
}

static int BalanceSheet_InsertLocked(balance_sheet_entry_t entry) {
    if (!sheetMap) {
        return -1;
    }

    key32_t key;
    memcpy(key.bytes, entry.address, 32);

    int ret = 0;
    khiter_t k = kh_put(balance_sheet_map_m, sheetMap, key, &ret);
    if (k == kh_end(sheetMap)) {
        return -1;
    }

    kh_value(sheetMap, k) = entry;
    return ret;
}

void BalanceSheet_Init() {
    sheetMap = kh_init(balance_sheet_map_m);
    pthread_mutex_init(&g_sheetLock, NULL);
}

int BalanceSheet_Insert(balance_sheet_entry_t entry) {
    if (!sheetMap) { return -1; }

    pthread_mutex_lock(&g_sheetLock);
    int ret = BalanceSheet_InsertLocked(entry);
    pthread_mutex_unlock(&g_sheetLock);
    return ret;
}

bool BalanceSheet_Lookup(uint8_t* address, balance_sheet_entry_t* out) {
    if (!address || !out) { return false; }

    pthread_mutex_lock(&g_sheetLock);
    key32_t key;
    memcpy(key.bytes, address, 32);

    khiter_t k = kh_get(balance_sheet_map_m, sheetMap, key);
    if (k != kh_end(sheetMap)) {
        balance_sheet_entry_t entry = kh_value(sheetMap, k);
        memcpy(out, &entry, sizeof(balance_sheet_entry_t));
        pthread_mutex_unlock(&g_sheetLock);
        return true;
    }

    pthread_mutex_unlock(&g_sheetLock);
    return false;
}

bool BalanceSheet_SaveToFile(const char* outPath) {
    if (!sheetMap) { return false; }

    pthread_mutex_lock(&g_sheetLock);
    char outFile[512];
    strcpy(outFile, outPath);
    strcat(outFile, "/balance_sheet.data");
    FILE* file = fopen(outFile, "wb");
    if (!file) {
        pthread_mutex_unlock(&g_sheetLock);
        return false;
    }

    khiter_t k;
    for (k = kh_begin(sheetMap); k != kh_end(sheetMap); ++k) {
        if (kh_exist(sheetMap, k)) {
            balance_sheet_entry_t entry = kh_val(sheetMap, k);
            if (fwrite(&entry, sizeof(balance_sheet_entry_t), 1, file) != 1) {
                fclose(file);
                pthread_mutex_unlock(&g_sheetLock);
                return false;
            }
        }
    }

    fclose(file);
    pthread_mutex_unlock(&g_sheetLock);
    return true;
}

bool BalanceSheet_LoadFromFile(const char* inPath) {
    if (!sheetMap) { return false; }

    pthread_mutex_lock(&g_sheetLock);
    char inFile[512];
    strcpy(inFile, inPath);
    strcat(inFile, "/balance_sheet.data");
    FILE* file = fopen(inFile, "rb");
    if (!file) {
        pthread_mutex_unlock(&g_sheetLock);
        return false;
    }

    balance_sheet_entry_t entry;
    while (fread(&entry, sizeof(balance_sheet_entry_t), 1, file) == 1) {
        if (BalanceSheet_InsertLocked(entry) < 0) {
            fclose(file);
            pthread_mutex_unlock(&g_sheetLock);
            return false;
        }
    }

    fclose(file);
    pthread_mutex_unlock(&g_sheetLock);
    return true;
}

void BalanceSheet_Print() {
    if (!sheetMap) { return; }

    pthread_mutex_lock(&g_sheetLock);
    // Iterate through every entry
    khiter_t k;
    for (k = kh_begin(sheetMap); k != kh_end(sheetMap); ++k) {
        if (kh_exist(sheetMap, k)) {
            key32_t key = kh_key(sheetMap, k);
            balance_sheet_entry_t val = kh_val(sheetMap, k);

            char balanceStr[80];
            uint256_serialize(&val.balance, balanceStr);

            char addrHex[65];
            AddressToHexString(key.bytes, addrHex);

            // Print full address
            printf("Address %s: balance=%s pebble(s)\n",
                addrHex,
                balanceStr);
        }
    }
    pthread_mutex_unlock(&g_sheetLock);
}

void BalanceSheet_Destroy() {
    kh_destroy(balance_sheet_map_m, sheetMap);
    sheetMap = NULL;
    pthread_mutex_destroy(&g_sheetLock);
}

bool BalanceSheet_SelectSpendableTransactions(
    const signed_transaction_t* candidates,
    size_t candidateCount,
    signed_transaction_t** outAccepted,
    size_t* outAcceptedCount,
    uint64_t* outTotalFees
) {
    if (!outAccepted || !outAcceptedCount || !outTotalFees) {
        return false;
    }

    *outAccepted = NULL;
    *outAcceptedCount = 0;
    *outTotalFees = 0;

    if (!candidates || candidateCount == 0) {
        return true;
    }

    signed_transaction_t* accepted = (signed_transaction_t*)calloc(candidateCount, sizeof(signed_transaction_t));
    if (!accepted) {
        return false;
    }

    khash_t(balance_sheet_map_m)* simMap = kh_init(balance_sheet_map_m);
    if (!simMap) {
        free(accepted);
        return false;
    }

    size_t acceptedCount = 0;
    uint64_t totalFees = 0;

    for (size_t i = 0; i < candidateCount; ++i) {
        const signed_transaction_t* tx = &candidates[i];
        if (Address_IsCoinbase(tx->transaction.senderAddress)) {
            continue;
        }

        uint64_t fee = 0;
        if (!BalanceSheet_ApplyCandidateTransaction(simMap, tx, &fee)) {
            continue;
        }

        accepted[acceptedCount++] = *tx;
        totalFees += fee;
    }

    kh_destroy(balance_sheet_map_m, simMap);

    if (acceptedCount == 0) {
        free(accepted);
        accepted = NULL;
    }

    *outAccepted = accepted;
    *outAcceptedCount = acceptedCount;
    *outTotalFees = totalFees;
    return true;
}
