#include <txmempool.h>

khash_t(tx_mempool_map_m)* txMempool = NULL;

void TxMempool_Init() {
    txMempool = kh_init(tx_mempool_map_m);
}

int TxMempool_Insert(signed_transaction_t tx) {
    if (!txMempool) { return -1; }

    uint8_t txHash[32];
    Transaction_CalculateHash(&tx.transaction, txHash);

    key32_t key;
    memcpy(key.bytes, txHash, 32);

    int ret;
    khiter_t k = kh_put(tx_mempool_map_m, txMempool, key, &ret);
    if (k == kh_end(txMempool)) {
        return -1;
    }

    kh_value(txMempool, k) = tx;

    return ret;
}

bool TxMempool_Lookup(uint8_t* txHash, signed_transaction_t* out) {
    if (!txMempool || !txHash || !out) { return false; }
    
    key32_t key;
    memcpy(key.bytes, txHash, 32);

    khiter_t k = kh_get(tx_mempool_map_m, txMempool, key);
    if (k != kh_end(txMempool)) {
        signed_transaction_t tx = kh_value(txMempool, k);
        memcpy(out, &tx, sizeof(signed_transaction_t));
        return true;
    }

    return false;
}

void TxMempool_Print() {
    if (!txMempool) { return; }

    khiter_t k;
    for (k = kh_begin(txMempool); k != kh_end(txMempool); ++k) {
        if (kh_exist(txMempool, k)) {
            signed_transaction_t tx = kh_val(txMempool, k);
            char senderHex[65];
            char recipient1Hex[65];
            char recipient2Hex[65];
            AddressToHexString(tx.transaction.senderAddress, senderHex);
            AddressToHexString(tx.transaction.recipientAddress1, recipient1Hex);
            AddressToHexString(tx.transaction.recipientAddress2, recipient2Hex);
            printf("TX in mempool: sender=%s recipient1=%s recipient2=%s amount1=%llu amount2=%llu fee=%llu\n",
                senderHex, recipient1Hex, recipient2Hex,
                (unsigned long long)tx.transaction.amount1,
                (unsigned long long)tx.transaction.amount2,
                (unsigned long long)tx.transaction.fee);
        }
    }
}

void TxMempool_Destroy() {
    if (txMempool) {
        kh_destroy(tx_mempool_map_m, txMempool);
    }
}
