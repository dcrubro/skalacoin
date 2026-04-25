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
void TxMempool_Print();
void TxMempool_Destroy();

#endif
