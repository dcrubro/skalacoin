#include <block/transaction.h>
#include <string.h>

void Transaction_Init(signed_transaction_t* tx) {
    if (!tx) { return; }
    // Zero out everything
    memset(tx, 0, sizeof(signed_transaction_t));

    // NOTE: Other things might be added here
}

void Transaction_CalculateHash(const signed_transaction_t* tx, uint8_t* outHash) {
    if (!tx || !outHash) {
        return;
    }

    uint8_t buffer[sizeof(transaction_t)];
    memcpy(buffer, &tx->transaction, sizeof(transaction_t));

    SHA256(buffer, sizeof(buffer), outHash);
    SHA256(outHash, 32, outHash); // Double-Hash
}

void Transaction_Sign(signed_transaction_t* tx, const uint8_t* privateKey) {
    if (!tx || !privateKey) {
        return;
    }

    uint8_t txHash[32];
    Transaction_CalculateHash(tx, txHash);
    Crypto_SignData(
        txHash,
        32,
        privateKey,
        tx->signature.signature
    );
}

bool Transaction_Verify(const signed_transaction_t* tx) {
    if (!tx) {
        return false;
    }

    if (Address_IsCoinbase(tx->transaction.senderAddress)) {
        // Coinbase transactions are valid if the signature is correct for the block (handled in Block_Verify)
        return true;
    }

    uint8_t computeAddress[32];
    SHA256(tx->transaction.compressedPublicKey, 33, computeAddress); // Address is hash of public key
    if (memcmp(computeAddress, tx->transaction.senderAddress, 32) != 0) {
        return false; // Sender address does not match public key
    }

    if (tx->transaction.amount1 == 0) {
        return false; // Zero-amount transactions are not valid
    }

    if (tx->transaction.fee > tx->transaction.amount1) {
        return false; // Fee cannot exceed amount
    }

    if (tx->transaction.version != 1) {
        return false; // Unsupported version
    }

    if (Address_IsCoinbase(tx->transaction.recipientAddress1) || Address_IsCoinbase(tx->transaction.recipientAddress2)) {
        return false; // Cannot send to coinbase address
    }

    // Balance checks are stateful and are handled when a block is added to the chain.
    // Transaction_Verify only checks transaction structure, addresses, and signature material.

    if (tx->transaction.amount2 == 0) {
        // If amount2 is zero, address2 must be all zeros
        uint8_t zeroAddress[32] = {0};
        if (memcmp(tx->transaction.recipientAddress2, zeroAddress, 32) != 0) {
            return false; // amount2 is zero but address2 is not zeroed
        }
    }

    uint8_t txHash[32];
    Transaction_CalculateHash(tx, txHash);

    // If all checks pass, verify the signature
    return Crypto_VerifySignature(
        txHash,
        32,
        tx->signature.signature,
        tx->transaction.compressedPublicKey
    );
}
