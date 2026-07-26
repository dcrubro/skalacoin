#ifndef NODEDISCOVERY_H
#define NODEDISCOVERY_H

#include <nets/net_node.h>
#include <udpd/udpnode.h>

// Create/destroy the peer-discovery state. Owns the known-peer table and its lock.
node_discovery_t* NodeDiscovery_Create(net_node_t* node, udp_node_t* udpNode);
void NodeDiscovery_Destroy(node_discovery_t* disc);

// Periodic tick (driven by the node maintenance thread): seed currently-connected peers,
// UDP-ping newly-learned ones, query a couple of connected peers for more, and connect to
// the reachable peers with the lowest ping until we reach the target connection count.
void NodeDiscovery_Iterate(node_discovery_t* disc);

// UDP latency callbacks (forwarded from the udp node via net_node thunks).
void NodeDiscovery_OnPong(node_discovery_t* disc, const struct sockaddr_storage* from, uint64_t nonce, uint64_t rttMs);
void NodeDiscovery_OnPingTimeout(node_discovery_t* disc, const struct sockaddr_storage* dest, uint64_t nonce);

// TCP peer-exchange handlers (called from the net_node packet dispatch).
// Build a PEERS response (a sample of our peers, excluding the requester) and send it over fromConn.
void NodeDiscovery_OnGetPeers(node_discovery_t* disc, tcp_connection_t* fromConn);
// Decode a received PEERS payload and fold a couple of its endpoints into the known-peer table.
void NodeDiscovery_OnPeersReceived(node_discovery_t* disc, tcp_connection_t* fromConn, const unsigned char* payload, size_t payloadLen);

// Strike a peer (by its listen endpoint) from the known-peer table. Called when a peer becomes
// logically disconnected (no remaining connection to it).
void NodeDiscovery_RemovePeer(node_discovery_t* disc, const struct sockaddr_storage* endpoint);

// Dump the known-peer table to stdout (for the CLI `peers` command).
void NodeDiscovery_PrintPeers(node_discovery_t* disc);

#endif
