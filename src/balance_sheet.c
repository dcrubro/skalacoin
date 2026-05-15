#include <balance_sheet.h>
#include <pthread.h>

khash_t(balance_sheet_map_m)* sheetMap = NULL;
static pthread_mutex_t g_sheetLock;

void BalanceSheet_Init() {
    sheetMap = kh_init(balance_sheet_map_m);
    pthread_mutex_init(&g_sheetLock, NULL);
}

int BalanceSheet_Insert(balance_sheet_entry_t entry) {
    if (!sheetMap) { return -1; }

    pthread_mutex_lock(&g_sheetLock);

    // Encapsulate key
    key32_t key;
    memcpy(key.bytes, entry.address, 32);

    int ret;
    khiter_t k = kh_put(balance_sheet_map_m, sheetMap, key, &ret);
    if (k == kh_end(sheetMap)) {
        return -1;
    }

    kh_value(sheetMap, k) = entry;

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
        if (BalanceSheet_Insert(entry) < 0) {
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
