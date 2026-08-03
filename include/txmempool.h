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

/**
 * Admission policy: should this transaction be held and relayed?
 *
 * LOCAL POLICY, NOT CONSENSUS. A block containing a transaction this rejects is still accepted --
 * see TX_MAX_FUTURE_DRIFT_MS / TX_EXPIRY_MS in constants.h for why the two are kept apart.
 *
 * Both bounds are measured against the node's own clock, NOT against the chain tip's timestamp.
 * Measuring "future" against the last block assumes blocks keep arriving: on a quiet chain the tip
 * can be hours old, and an honest transaction created right now would look hours ahead of it and be
 * refused. Sending would become impossible exactly when the chain is idle.
 *
 * Deliberately NOT applied when a rollback returns transactions to the pool: those were already in
 * the chain, so they are legitimate by definition and must not be dropped for looking old.
**/
bool TxMempool_PolicyAccepts(const signed_transaction_t* tx, uint64_t nowMs);

// Drop transactions older than TX_EXPIRY_MS. Returns how many were removed.
size_t TxMempool_PruneExpired(uint64_t nowMs);
void TxMempool_Destroy();

#endif
