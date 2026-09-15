#ifndef RUNTIME_STATE_H
#define RUNTIME_STATE_H

#include <stdint.h>

#include <uint256.h>

#include <block/chain.h>
#include <pthread.h>

extern uint64_t g_currentBlockHeight;
extern blockchain_t* g_currentChain;
extern uint256_t g_currentSupply;
extern uint64_t g_currentReward;
extern uint32_t g_difficultyTarget;
extern const char* g_chainDataDir;
extern unsigned short g_listenPort;
extern bool g_echoPeersEnabled;
extern bool g_forceOrphanReorgEnabled;
// Random per-run identity of this node, advertised in HELLO/ACK_HELLO. A host can be reachable
// under many addresses (especially over IPv6), so an (ip, port) endpoint is not a peer identity:
// this nonce is what lets us recognise our own connections and a peer we already talk to.
extern uint64_t g_localNodeId;

// Global synchronization primitives for runtime state
extern pthread_rwlock_t g_chainLock; // protects chain structure and related mutations
extern pthread_mutex_t g_balanceSheetLock; // protects balance sheet map

#endif
