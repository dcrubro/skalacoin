#ifndef RUNTIME_STATE_H
#define RUNTIME_STATE_H

#include <stdint.h>

#include <uint256.h>

#include <block/chain.h>
#include <pthread.h>

extern uint64_t currentBlockHeight;
extern blockchain_t* currentChain;
extern uint256_t currentSupply;
extern uint64_t currentReward;
extern uint32_t difficultyTarget;
extern const char* chainDataDir;
extern unsigned short listenPort;
extern bool echoPeersEnabled;
extern bool forceOrphanReorgEnabled;
// Random per-run identity of this node, advertised in HELLO/ACK_HELLO. A host can be reachable
// under many addresses (especially over IPv6), so an (ip, port) endpoint is not a peer identity:
// this nonce is what lets us recognise our own connections and a peer we already talk to.
extern uint64_t localNodeId;

// Global synchronization primitives for runtime state
extern pthread_rwlock_t chainLock; // protects chain structure and related mutations
extern pthread_mutex_t balanceSheetLock; // protects balance sheet map

#endif
