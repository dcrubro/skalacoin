#include <balance_sheet.h>

khash_t(balance_sheet_map_m)* sheetMap = NULL;

void BalanceSheet_Init() {
    sheetMap = kh_init(balance_sheet_map_m);
}

int BalanceSheet_Insert(balance_sheet_entry_t entry) {
    if (!sheetMap) { return -1; }

    // Encapsulate key
    key32_t key;
    memcpy(key.bytes, entry.address, 32);

    int ret;
    khiter_t k = kh_put(balance_sheet_map_m, sheetMap, key, &ret);
    if (k == kh_end(sheetMap)) {
        return -1;
    }

    kh_value(sheetMap, k) = entry;

    return ret;
}

bool BalanceSheet_Lookup(uint8_t* address, balance_sheet_entry_t* out) {
    if (!address || !out) { return false; }
    
    key32_t key;
    memcpy(key.bytes, address, 32);

    khiter_t k = kh_get(balance_sheet_map_m, sheetMap, key);
    if (k != kh_end(sheetMap)) {
        balance_sheet_entry_t entry = kh_value(sheetMap, k);
        memcpy(out, &entry, sizeof(balance_sheet_entry_t));
        return true;
    }

    return false;
}

/* TODO */
bool BalanceSheet_SaveToFile(const char* outPath) {
    if (!sheetMap) { return false; }
    return true;
}

/* TODO */
bool BalanceSheet_LoadFromFile(const char* inPath) {
    if (!sheetMap) { return false; }
    return true;
}

void BalanceSheet_Print() {
    if (!sheetMap) { return; }

    // Iterate through every entry
    khiter_t k;
    size_t iter = 0;
    for (k = kh_begin(sheetMap); k != kh_end(sheetMap); ++k) {
        if (kh_exist(sheetMap, k)) {
            key32_t key = kh_key(sheetMap, k);
            balance_sheet_entry_t val = kh_val(sheetMap, k);

            printf("Sheet entry %llu: mapkey=%02x%02x%02x%02x... address=%02x%02x%02x%02x... balance=%llu\n",
                (unsigned long long)(iter),
                key.bytes[0], key.bytes[1], key.bytes[2], key.bytes[3],
                val.address[0], val.address[1], val.address[2], val.address[3],
                (unsigned long long)(val.balance),
                iter++);
        }
    }
}

void BalanceSheet_Destroy() {
    kh_destroy(balance_sheet_map_m, sheetMap);
    sheetMap = NULL;
}
