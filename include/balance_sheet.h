#ifndef BALANCE_SHEET_H
#define BALANCE_SHEET_H

#include <stdint.h>
#include <dynarr.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <khash/khash.h>
#include <crypto/crypto.h>
#include <string.h>
#include <utils.h>
#include <uint256.h>

typedef struct {
    uint8_t address[32]; // For now just the SHA-256 of the public key; allows representation in different encodings (base58, bech32, etc) without changing the underlying data structure
    uint256_t balance;
    // TODO: Additional things
} balance_sheet_entry_t;

KHASH_INIT(balance_sheet_map_m, key32_t, balance_sheet_entry_t, 1, hash_key32, eq_key32)
extern khash_t(balance_sheet_map_m)* sheetMap;

void BalanceSheet_Init();
int BalanceSheet_Insert(balance_sheet_entry_t entry);
bool BalanceSheet_Lookup(uint8_t* address, balance_sheet_entry_t* out);
bool BalanceSheet_SaveToFile(const char* outPath);
bool BalanceSheet_LoadFromFile(const char* inPath); 
void BalanceSheet_Print();
void BalanceSheet_Destroy();

#endif
