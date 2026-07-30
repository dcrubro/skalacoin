#ifndef NET_NODE_H
#define NET_NODE_H

#ifndef _WIN32
// POSIX
#include <tcpd/tcpconnection.h>
#include <tcpd/tcpclient.h>
#include <tcpd/tcpserver.h>
#endif

#include <constants.h>
#include <packettype.h>
#include <udpd/udpnode.h>

// Forward declaration - the discovery state is defined in nodediscovery.c (opaque here).
typedef struct node_discovery node_discovery_t;

#include <stddef.h>

#include <dynarr.h>
#include <dynset.h>

#include <pthread.h>

#include <block/block.h>
#include <block/chain.h>
#include <block/transaction.h>
#include <stdatomic.h>

typedef struct {
    tcp_server_t* server;
    tcp_client_t outboundClients[MAX_CONS];
    size_t outboundCount;
    // Dedup cache for recently seen block hashes (canonical 32-byte hash)
    DynSet* seenBlocks;
    // Protects seenBlocks
    pthread_mutex_t seenLock;
    // Protects outboundClients snapshots and peerBlockHeight writes
    pthread_mutex_t outboundLock;
    void (*on_connect)(tcp_connection_t* conn, void* user);
    void (*on_data)(tcp_connection_t* conn, const unsigned char* data, size_t len, void* user);
    void (*on_disconnect)(tcp_connection_t* conn, void* user);
    void* callbackUser;
    // Maintenance thread for periodic tasks (orphan attach, pruning, metrics)
    pthread_t maintenanceThread;
    // Cross-thread stop flag: written by Node_Destroy on the main thread, read by the maintenance
    // thread's loop condition. `volatile` stops the compiler hoisting the load but provides neither
    // atomicity nor ordering, so this has to be a real atomic (and TSan rightly flagged it).
    _Atomic int maintenanceRunning;
    int maintenanceIntervalMs;
    // UDP ping/pong daemon (latency oracle) and peer discovery state
    udp_node_t* udpNode;
    node_discovery_t* discovery;
} net_node_t;

net_node_t* Node_Create();
void Node_Destroy(net_node_t* node);

void Node_SetCallbacks(
    net_node_t* node,
    void (*on_connect)(tcp_connection_t* conn, void* user),
    void (*on_data)(tcp_connection_t* conn, const unsigned char* data, size_t len, void* user),
    void (*on_disconnect)(tcp_connection_t* conn, void* user),
    void* user
);

int Node_ConnectPeer(net_node_t* node, const char* ip, unsigned short port);
int Node_ConnectStartupPeers(net_node_t* node, const char** ips, const unsigned short* ports, size_t peersCount);

int Node_SendPacket(net_node_t* node, tcp_connection_t* conn, packet_type_t packetType, const void* payload, size_t payloadLen);
int Node_BroadcastTransaction(net_node_t* node, signed_transaction_t* tx, tcp_connection_t* excludeNode);

// Helpers for outbound peer selection and block broadcast
int Node_GetBestOutboundPeer(net_node_t* node, tcp_connection_t** outConn, uint64_t* outHeight);

/**
 * Delivery receipts for windowed sync.
 *
 * A FETCH_BLOCK reply is handled on the peer's io thread and may legitimately never reach the
 * chain: a block belonging to a competing branch is filed in the orphan pool instead. A sync loop
 * that infers arrival from the chain growing therefore cannot tell "arrived but forked" from "lost
 * in transit", so it re-requests until it times out. Against a peer on a fork that costs one full
 * retry-and-timeout cycle for EVERY block, which is why syncing to a forked peer used to crawl.
 *
 * DUPLICATE is what makes a backwards fork walk terminate: it means we already hold exactly that
 * block, so the two chains agree at that height and there is no reason to keep descending.
**/
typedef enum {
    NODE_DELIVERY_APPENDED = 0,  // joined our chain
    NODE_DELIVERY_DUPLICATE = 1, // we already held this exact block -- common ground
    NODE_DELIVERY_ORPHANED = 2,  // belongs to a competing branch; now in the orphan pool
    NODE_DELIVERY_REJECTED = 3   // failed validation
} node_delivery_status_t;

void Node_NoteBlockDelivered(uint64_t height, node_delivery_status_t status);
bool Node_TakeBlockDelivery(uint64_t height, node_delivery_status_t* outStatus);
void Node_ResetBlockDeliveries(void);
void Node_BroadcastChainRange(net_node_t* node, size_t startHeightInclusive, tcp_connection_t* sourceConn);

// Callback logic
void Node_Server_OnConnect(tcp_connection_t* client);
void Node_Server_OnData(tcp_connection_t* client);
void Node_Server_OnDisconnect(tcp_connection_t* client);
void Node_Client_OnConnect(tcp_connection_t* client);
void Node_Client_OnData(tcp_connection_t* client);
void Node_Client_OnDisconnect(tcp_connection_t* client);

void Node_GetClientList(net_node_t* node, tcp_connection_t** outClients, size_t* outCount);

// Computes a connection's peer listen endpoint (IP + advertised/dialed listen port) into *out.
// Outbound: the dialed peerAddr port already is the listen port. Inbound: uses peerListenPort.
// Returns non-zero on success (usable endpoint with a known, non-zero port), zero otherwise.
int Node_ConnListenEndpoint(const tcp_connection_t* conn, struct sockaddr_storage* out);

// Returns the node identity advertised by a connection's peer, or 0 if it is not known yet.
uint64_t Node_ConnPeerNodeId(const tcp_connection_t* conn);

// Fills outEndpoints with the listen endpoints of all current connections (inbound + outbound),
// deduped by IP+port, and outNodeIds (optional, may be NULL) with the matching peer identities.
// Returns the number of endpoints written (<= maxOut).
size_t Node_GetPeerEndpoints(net_node_t* node, struct sockaddr_storage* outEndpoints, uint64_t* outNodeIds, size_t maxOut);

#endif
