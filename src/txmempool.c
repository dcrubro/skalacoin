#include <txmempool.h>
#include <pthread.h>

static pthread_mutex_t g_txMempoolLock;
static bool g_txMempoolLockInitialized = false;

khash_t(tx_mempool_map_m)* txMempool = NULL;

void TxMempool_Init() {
    txMempool = kh_init(tx_mempool_map_m);
    pthread_mutex_init(&g_txMempoolLock, NULL);
    g_txMempoolLockInitialized = true;
}

int TxMempool_Insert(signed_transaction_t tx) {
    if (!txMempool) { return -1; }

    pthread_mutex_lock(&g_txMempoolLock);
    uint8_t txHash[32];
    Transaction_CalculateHash(&tx, txHash);

    key32_t key;
    memcpy(key.bytes, txHash, 32);

    int ret;
    khiter_t k = kh_put(tx_mempool_map_m, txMempool, key, &ret);
    if (k == kh_end(txMempool)) {
        pthread_mutex_unlock(&g_txMempoolLock);
        return -1;
    }

    kh_value(txMempool, k) = tx;

    pthread_mutex_unlock(&g_txMempoolLock);

    return ret;
}

bool TxMempool_Lookup(uint8_t* txHash, signed_transaction_t* out) {
    if (!txMempool || !txHash || !out) { return false; }
    
    pthread_mutex_lock(&g_txMempoolLock);
    key32_t key;
    memcpy(key.bytes, txHash, 32);

    khiter_t k = kh_get(tx_mempool_map_m, txMempool, key);
    if (k != kh_end(txMempool)) {
        signed_transaction_t tx = kh_value(txMempool, k);
        memcpy(out, &tx, sizeof(signed_transaction_t));
        pthread_mutex_unlock(&g_txMempoolLock);
        return true;
    }

    pthread_mutex_unlock(&g_txMempoolLock);
    return false;
}

bool TxMempool_Snapshot(signed_transaction_t** outTxs, size_t* outCount) {
    if (!outTxs || !outCount) {
        return false;
    }

    *outTxs = NULL;
    *outCount = 0;

    if (!txMempool) {
        return true;
    }

    pthread_mutex_lock(&g_txMempoolLock);

    size_t count = 0;
    khiter_t k;
    for (k = kh_begin(txMempool); k != kh_end(txMempool); ++k) {
        if (kh_exist(txMempool, k)) {
            ++count;
        }
    }

    if (count == 0) {
        pthread_mutex_unlock(&g_txMempoolLock);
        return true;
    }

    signed_transaction_t* snapshot = (signed_transaction_t*)malloc(count * sizeof(signed_transaction_t));
    if (!snapshot) {
        pthread_mutex_unlock(&g_txMempoolLock);
        return false;
    }

    size_t index = 0;
    for (k = kh_begin(txMempool); k != kh_end(txMempool); ++k) {
        if (kh_exist(txMempool, k)) {
            snapshot[index++] = kh_value(txMempool, k);
        }
    }

    pthread_mutex_unlock(&g_txMempoolLock);

    *outTxs = snapshot;
    *outCount = count;
    return true;
}

void TxMempool_Print() {
    if (!txMempool) { return; }

    pthread_mutex_lock(&g_txMempoolLock);
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
    pthread_mutex_unlock(&g_txMempoolLock);
}

void TxMempool_Destroy() {
    if (txMempool) {
        pthread_mutex_lock(&g_txMempoolLock);
        kh_destroy(tx_mempool_map_m, txMempool);
        txMempool = NULL;
        pthread_mutex_unlock(&g_txMempoolLock);
    }

    if (g_txMempoolLockInitialized) {
        pthread_mutex_destroy(&g_txMempoolLock);
        g_txMempoolLockInitialized = false;
    }
}
