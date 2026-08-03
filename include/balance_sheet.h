#ifndef BALANCE_SHEET_H
#define BALANCE_SHEET_H

#include <stdint.h>
#include <dynarr.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <khash/khash.h>
#include <crypto/crypto.h>
#include <block/transaction.h>
#include <string.h>
#include <utils.h>
#include <uint256.h>

typedef struct {
    uint8_t address[32]; // For now just the SHA-256 of the public key; allows representation in different encodings (base58, bech32, etc) without changing the underlying data structure
    uint256_t balance;
    /**
     * Timestamp (unix ms) of the most recent transaction this address SENT that is in the chain.
     *
     * Replay protection. Without it any historical transaction could be rebroadcast and mined a
     * second time, debiting the sender again -- with UTXOs the spent inputs make that impossible,
     * but an account model has nothing to stop it. A non-coinbase transaction is only valid if its
     * timestamp is strictly greater than this, so a byte-identical replay (same timestamp, same
     * hash) can never be included twice. Enforced in Chain_AddBlockLocked; see the note there.
     *
     * Rebuilt for free by the rollback's balance-sheet replay, so a reorg cannot leave it stale.
     * Persisted with the rest of the entry -- note the file has no height marker, so a balance
     * sheet that is out of sync with the chain silently resets this to 0 for every account.
    **/
    uint64_t lastTxTimestamp;
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

bool BalanceSheet_SelectSpendableTransactions(
    const signed_transaction_t* candidates,
    size_t candidateCount,
    signed_transaction_t** outAccepted,
    size_t* outAcceptedCount,
    uint64_t* outTotalFees
);

#endif
