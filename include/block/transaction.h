#ifndef TRANSACTION_H
#define TRANSACTION_H

#include <stdint.h>
#include <stdbool.h>
#include <crypto/crypto.h>

// Special sender/recipient address marker for coinbase logic: 32 bytes of 0xFF.
static inline bool Address_IsCoinbase(const uint8_t address[32]) {
    if (!address) {
        return false;
    }

    for (size_t i = 0; i < 32; ++i) {
        if (address[i] != 0xFF) {
            return false;
        }
    }

    return true;
}

// 178 bytes total for v1
typedef struct {
    uint8_t version;
    // Only one "input" sender address
    uint8_t senderAddress[32];

    // The "main" recepient address and amount. This is the only required output, and is used for calculating the transaction hash and signature.
    uint8_t recipientAddress1[32];
    uint64_t amount1;
    // The "extra" recepient address and amount. This can safely be NULL/0 if not used and has multiple uses:
    // - Sending zero: parital spend, sender keeps coins on the same address
    // - Sending to a different address: normal spend, sender's coins move to a new address, e.g. change address
    // - Private Transactions: Can nullify the whole original stealth address input (sender) and send change to a new stealth address (recipient 2) to obfuscate the transaction graph.
    // Note that coinbase will have this as NULL/0 (for now, but we could have multiple payouts in the future)
    uint8_t recipientAddress2[32];
    uint64_t amount2;

    uint64_t fee; // Rewarded to the miner; can be zero, but the miner may choose to ignore transactions with very low fees
    uint8_t compressedPublicKey[33];
    // Timestamp is dictated by the block
} transaction_t;

typedef struct {
    uint8_t txHash[32];
    uint8_t signature[64]; // Signature of the hash
} transaction_sig_t;

typedef struct {
    transaction_t transaction;
    transaction_sig_t signature;
} signed_transaction_t;

void Transaction_CalculateHash(const signed_transaction_t* tx, uint8_t* outHash);
void Transaction_Sign(signed_transaction_t* tx, const uint8_t* privateKey);
bool Transaction_Verify(const signed_transaction_t* tx);

#endif
