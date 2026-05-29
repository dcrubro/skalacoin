#ifndef TXMEMPOOL_H
#define TXMEMPOOL_H

#include <block/transaction.h>
#include <khash/khash.h>
#include <utils.h>
#include <uint256.h>

KHASH_INIT(tx_mempool_map_m, key32_t, signed_transaction_t, 1, hash_key32, eq_key32)
extern khash_t(tx_mempool_map_m)* txMempool;

void TxMempool_Init();
// Assumed that the transation was confirmed to be valid
int TxMempool_Insert(signed_transaction_t tx);
bool TxMempool_Lookup(uint8_t* txHash, signed_transaction_t* out);
bool TxMempool_Snapshot(signed_transaction_t** outTxs, size_t* outCount);
void TxMempool_Print();
// Remove a transaction from the mempool by its hash. Returns true if removed.
bool TxMempool_Remove(const uint8_t* txHash);
void TxMempool_Destroy();

#endif
