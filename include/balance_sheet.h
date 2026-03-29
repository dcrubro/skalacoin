#ifndef BALANCE_SHEET_H
#define BALANCE_SHEET_H

#include <stdint.h>

typedef struct {
    uint8_t address[32]; // For now just the SHA-256 of the public key; allows representation in different encodings (base58, bech32, etc) without changing the underlying data structure
    uint64_t balance;
} balance_sheet_entry_t;

#endif