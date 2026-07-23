#include <nets/net_node.h>
#include <nets/nodediscovery.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <runtime_state.h>
#include <balance_sheet.h>
#include <inttypes.h>
#include <nets/orphan_pool.h>
#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>
#include <txmempool.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static net_node_t* Node_FromConnection(tcp_connection_t* conn) {
    if (!conn) {
        return NULL;
    }

    return (net_node_t*)conn->owner;
}

static uint64_t Node_GetCurrentBlockHeight(void) {
    if (currentChain) {
        return (uint64_t)Chain_Size(currentChain);
    }

    return currentBlockHeight;
}

// Compares two listen endpoints (family + IP + port).
static int NetNode_EndpointEqual(const struct sockaddr_storage* a, const struct sockaddr_storage* b) {
    if (a->ss_family != b->ss_family) return 0;
    if (a->ss_family == AF_INET) {
        const struct sockaddr_in* x = (const struct sockaddr_in*)a;
        const struct sockaddr_in* y = (const struct sockaddr_in*)b;
        return x->sin_port == y->sin_port &&
               memcmp(&x->sin_addr, &y->sin_addr, sizeof(struct in_addr)) == 0;
    }
    if (a->ss_family == AF_INET6) {
        const struct sockaddr_in6* x = (const struct sockaddr_in6*)a;
        const struct sockaddr_in6* y = (const struct sockaddr_in6*)b;
        return x->sin6_port == y->sin6_port &&
               memcmp(&x->sin6_addr, &y->sin6_addr, sizeof(struct in6_addr)) == 0;
    }
    return 0;
}

// Builds a listen endpoint from an IP string + port, normalising IPv4-mapped IPv6 to plain IPv4
// so it compares equal to Node_ConnListenEndpoint output. Returns non-zero on success.
static int NetNode_MakeEndpoint(const char* ip, unsigned short port, struct sockaddr_storage* out) {
    if (!ip || !out) return 0;
    memset(out, 0, sizeof(*out));

    struct in_addr a4;
    if (inet_pton(AF_INET, ip, &a4) == 1) {
        struct sockaddr_in* o = (struct sockaddr_in*)out;
        o->sin_family = AF_INET;
        o->sin_addr = a4;
        o->sin_port = htons(port);
        return 1;
    }

    struct in6_addr a6;
    if (inet_pton(AF_INET6, ip, &a6) == 1) {
        if (IN6_IS_ADDR_V4MAPPED(&a6)) {
            struct sockaddr_in* o = (struct sockaddr_in*)out;
            o->sin_family = AF_INET;
            memcpy(&o->sin_addr, ((const uint8_t*)&a6) + 12, sizeof(struct in_addr));
            o->sin_port = htons(port);
        } else {
            struct sockaddr_in6* o = (struct sockaddr_in6*)out;
            o->sin6_family = AF_INET6;
            o->sin6_addr = a6;
            o->sin6_port = htons(port);
        }
        return 1;
    }
    return 0;
}

// Returns non-zero if we already hold an outbound connection to the given listen endpoint.
static int Node_HasOutboundTo(net_node_t* node, const struct sockaddr_storage* endpoint) {
    int found = 0;
    pthread_mutex_lock(&node->outboundLock);
    for (size_t i = 0; i < MAX_CONS; ++i) {
        tcp_connection_t* c = node->outboundClients[i].connection;
        if (!c) continue;
        struct sockaddr_storage ep;
        if (Node_ConnListenEndpoint(c, &ep) && NetNode_EndpointEqual(&ep, endpoint)) {
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&node->outboundLock);
    return found;
}

// Returns non-zero if some inbound connection OTHER than `self` already has the given listen
// endpoint (used to reject a duplicate inbound once we learn the peer's advertised listen port).
static int Node_HasOtherInboundFrom(net_node_t* node, const tcp_connection_t* self, const struct sockaddr_storage* endpoint) {
    if (!node->server) return 0;
    int found = 0;
    pthread_mutex_lock(&node->server->clientsMutex);
    for (size_t i = 0; i < node->server->maxClients; ++i) {
        tcp_connection_t* other = node->server->clientsArrPtr ? node->server->clientsArrPtr[i] : NULL;
        if (!other || other == self) continue;
        struct sockaddr_storage ep;
        if (Node_ConnListenEndpoint(other, &ep) && NetNode_EndpointEqual(&ep, endpoint)) {
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&node->server->clientsMutex);
    return found;
}

int Node_ConnListenEndpoint(const tcp_connection_t* conn, struct sockaddr_storage* out) {
    if (!conn || !out) return 0;
    memset(out, 0, sizeof(*out));

    // Determine the peer's listen port. For an outbound connection the port we dialed already
    // is the peer's listen port; for an inbound one it is the port advertised in HELLO.
    unsigned short listenP;
    if (conn->role == TCP_CONNECTION_ROLE_OUTBOUND) {
        listenP = (conn->addrFamily == AF_INET6)
            ? ntohs(((const struct sockaddr_in6*)&conn->peerAddr)->sin6_port)
            : ntohs(((const struct sockaddr_in*)&conn->peerAddr)->sin_port);
    } else {
        listenP = conn->peerListenPort;
    }
    if (listenP == 0) return 0; // unknown listen port -> not a usable endpoint

    if (conn->addrFamily == AF_INET) {
        const struct sockaddr_in* a = (const struct sockaddr_in*)&conn->peerAddr;
        struct sockaddr_in* o = (struct sockaddr_in*)out;
        o->sin_family = AF_INET;
        o->sin_addr = a->sin_addr;
        o->sin_port = htons(listenP);
        return 1;
    }
    if (conn->addrFamily == AF_INET6) {
        const struct sockaddr_in6* a = (const struct sockaddr_in6*)&conn->peerAddr;
        if (IN6_IS_ADDR_V4MAPPED(&a->sin6_addr)) {
            // Normalise IPv4-mapped IPv6 to plain IPv4.
            struct sockaddr_in* o = (struct sockaddr_in*)out;
            o->sin_family = AF_INET;
            memcpy(&o->sin_addr, ((const uint8_t*)&a->sin6_addr) + 12, sizeof(struct in_addr));
            o->sin_port = htons(listenP);
        } else {
            struct sockaddr_in6* o = (struct sockaddr_in6*)out;
            o->sin6_family = AF_INET6;
            o->sin6_addr = a->sin6_addr;
            o->sin6_port = htons(listenP);
        }
        return 1;
    }
    return 0;
}

size_t Node_GetPeerEndpoints(net_node_t* node, struct sockaddr_storage* outEndpoints, size_t maxOut) {
    if (!node || !outEndpoints || maxOut == 0) return 0;
    size_t count = 0;

    // Outbound connections
    pthread_mutex_lock(&node->outboundLock);
    for (size_t i = 0; i < MAX_CONS && count < maxOut; ++i) {
        tcp_connection_t* c = node->outboundClients[i].connection;
        if (!c) continue;
        struct sockaddr_storage ep;
        if (!Node_ConnListenEndpoint(c, &ep)) continue;
        int dup = 0;
        for (size_t k = 0; k < count; ++k) {
            if (NetNode_EndpointEqual(&outEndpoints[k], &ep)) { dup = 1; break; }
        }
        if (!dup) outEndpoints[count++] = ep;
    }
    pthread_mutex_unlock(&node->outboundLock);

    // Inbound connections
    if (node->server) {
        pthread_mutex_lock(&node->server->clientsMutex);
        for (size_t i = 0; i < node->server->maxClients && count < maxOut; ++i) {
            tcp_connection_t* c = node->server->clientsArrPtr ? node->server->clientsArrPtr[i] : NULL;
            if (!c) continue;
            struct sockaddr_storage ep;
            if (!Node_ConnListenEndpoint(c, &ep)) continue;
            int dup = 0;
            for (size_t k = 0; k < count; ++k) {
                if (NetNode_EndpointEqual(&outEndpoints[k], &ep)) { dup = 1; break; }
            }
            if (!dup) outEndpoints[count++] = ep;
        }
        pthread_mutex_unlock(&node->server->clientsMutex);
    }

    return count;
}

// Thunks routing UDP ping/pong events into the discovery state.
static void Node_OnPongThunk(udp_node_t* udp, const struct sockaddr_storage* from,
                             uint64_t nonce, int protoVersion, uint64_t rttMs, void* user) {
    (void)udp; (void)protoVersion;
    net_node_t* node = (net_node_t*)user;
    if (node && node->discovery) {
        NodeDiscovery_OnPong(node->discovery, from, nonce, rttMs);
    }
}

static void Node_OnPingTimeoutThunk(udp_node_t* udp, const struct sockaddr_storage* dest,
                                    uint64_t nonce, void* user) {
    (void)udp;
    net_node_t* node = (net_node_t*)user;
    if (node && node->discovery) {
        NodeDiscovery_OnPingTimeout(node->discovery, dest, nonce);
    }
}

typedef enum {
    NODE_BLOCK_REJECTED = 0,
    NODE_BLOCK_ORPHAN_QUEUED = 1,
    NODE_BLOCK_ACCEPTED = 2
} node_block_accept_result_t;

static void* Node_MaintenanceThread(void* arg) {
    net_node_t* n = (net_node_t*)arg;
    if (!n) return NULL;
    while (n->maintenanceRunning) {
        if (currentChain) {
            size_t attached = OrphanPool_AttemptAttach(currentChain);
            if (attached > 0) {
                printf("Maintenance: attached %zu orphan(s)\n", attached);
                Chain_SaveToFile(currentChain, chainDataDir, currentSupply, currentReward);
                BalanceSheet_SaveToFile(chainDataDir);
            }
        }
        // Peer discovery tick: ping/query connected peers and connect to the best-ping discoveries.
        if (n->discovery) {
            NodeDiscovery_Iterate(n->discovery);
        }
        sleep_for_milliseconds((uint64_t)n->maintenanceIntervalMs);
    }
    return NULL;
}

static int Node_DecodePacket(const tcp_connection_t* conn, packet_type_t* outType, const unsigned char** outPayload, size_t* outPayloadLen) {
    if (!conn || !outType || !outPayload || !outPayloadLen || conn->dataBufLen < 1 || !conn->dataBuf) {
        return -1;
    }

    uint8_t packetType = conn->dataBuf[0];
    if (!PacketType_IsValid(packetType)) {
        return -1;
    }

    *outType = (packet_type_t)packetType;
    *outPayload = conn->dataBuf + 1;
    *outPayloadLen = conn->dataBufLen - 1;
    return 0;
}

static node_block_accept_result_t Node_ParseAndAcceptBlock(const unsigned char* payload, size_t payloadLen, bool persist) {
    if (!payload) { return NODE_BLOCK_REJECTED; }

    size_t offset = 0;
    if (payloadLen < sizeof(uint64_t) + sizeof(block_header_t) + sizeof(uint64_t)) { return NODE_BLOCK_REJECTED; }

    uint64_t blockHeight = 0;
    memcpy(&blockHeight, payload + offset, sizeof(blockHeight));
    offset += sizeof(blockHeight);

    block_t* blk = (block_t*)calloc(1, sizeof(block_t));
    if (!blk) { return NODE_BLOCK_REJECTED; }

    memcpy(&blk->header, payload + offset, sizeof(blk->header));
    blk->header.blockNumber = blockHeight;
    offset += sizeof(blk->header);

    uint64_t txCount = 0;
    memcpy(&txCount, payload + offset, sizeof(txCount));
    offset += sizeof(txCount);

    blk->transactions = DYNARR_CREATE(signed_transaction_t, txCount == 0 ? 1 : (size_t)txCount);
    if (!blk->transactions) { free(blk); return NODE_BLOCK_REJECTED; }

    for (uint64_t i = 0; i < txCount; ++i) {
        if (offset + sizeof(signed_transaction_t) > payloadLen) {
            DynArr_destroy(blk->transactions);
            free(blk);
            return NODE_BLOCK_REJECTED;
        }
        signed_transaction_t tx;
        memcpy(&tx, payload + offset, sizeof(tx));
        offset += sizeof(tx);
        if (!DynArr_push_back(blk->transactions, &tx)) {
            DynArr_destroy(blk->transactions);
            free(blk);
            return NODE_BLOCK_REJECTED;
        }
    }

    // Validate block
    if (!Block_IsFullyValid(blk)) {
        printf("Rejected BLOCK_DATA at height %" PRIu64 " during validation\n", blockHeight);
        DynArr_destroy(blk->transactions);
        free(blk);
        return NODE_BLOCK_REJECTED;
    }

    if (!currentChain) {
        printf("Rejected BLOCK_DATA at height %" PRIu64 ": no active chain\n", blockHeight);
        DynArr_destroy(blk->transactions);
        free(blk);
        return NODE_BLOCK_REJECTED;
    }

    // Temporary debug mode: force network-received blocks through the orphan pool to exercise reorg handling.
    if (forceOrphanReorgEnabled && blk->header.blockNumber > 0) {
        OrphanPool_Insert(blk, blockHeight);
        printf("Forced orphan BLOCK_DATA at height %" PRIu64 "\n", blockHeight);
        return NODE_BLOCK_ORPHAN_QUEUED;
    }

    // If parent is missing, insert into orphan pool instead of rejecting immediately.
    uint64_t chainSize = Chain_Size(currentChain);
    if (blk->header.blockNumber > chainSize) {
        // Parent(s) missing; queue as orphan
        OrphanPool_Insert(blk, blockHeight);
        printf("Queued orphan BLOCK_DATA at height %" PRIu64 "\n", blockHeight);
        return NODE_BLOCK_ORPHAN_QUEUED;
    } else if (blk->header.blockNumber < chainSize) {
        // Older block than current chain tip: reject
        printf("Rejected BLOCK_DATA at height %" PRIu64 ": older than current chain\n", blockHeight);
        DynArr_destroy(blk->transactions);
        free(blk);
        return NODE_BLOCK_REJECTED;
    } else {
        // blk->header.blockNumber == chainSize -> candidate to append. Ensure prevHash matches current tip.
        if (chainSize > 0) {
            block_t* last = NULL;
            if (!Chain_GetBlockCopy(currentChain, (size_t)(chainSize - 1), &last) || !last) {
                // Can't verify parent; queue as orphan conservatively
                OrphanPool_Insert(blk, blockHeight);
                printf("Queued orphan BLOCK_DATA at height %" PRIu64 " (unable to verify parent)\n", blockHeight);
                if (last) Block_Destroy(last);
                return NODE_BLOCK_ORPHAN_QUEUED;
            }
            uint8_t lastHash[32];
            Block_CalculateHash(last, lastHash);
            if (memcmp(lastHash, blk->header.prevHash, 32) != 0) {
                // Conflicting block at same height; queue as orphan until resolved by a subsequent extension.
                OrphanPool_Insert(blk, blockHeight);
                Block_Destroy(last);
                printf("Queued conflicting BLOCK_DATA at same height %" PRIu64 " as orphan\n", blockHeight);
                return NODE_BLOCK_ORPHAN_QUEUED;
            }
            Block_Destroy(last);
        }
    }

    if (!Chain_AddBlock(currentChain, blk)) {
        // Chain_AddBlock failed; cleanup
        printf("Rejected BLOCK_DATA at height %" PRIu64 " during chain add\n", blockHeight);
        if (blk->transactions) {
            DynArr_destroy(blk->transactions);
        }
        free(blk);
        return NODE_BLOCK_REJECTED;
    }

    uint64_t coinbaseAmount = 0;
    if (blk->transactions) {
        for (size_t i = 0; i < DynArr_size(blk->transactions); ++i) {
            signed_transaction_t* tx = (signed_transaction_t*)DynArr_at(blk->transactions, i);
            if (tx && Address_IsCoinbase(tx->transaction.senderAddress)) {
                coinbaseAmount = tx->transaction.amount1;
                break;
            }
        }
    }

    (void)uint256_add_u64(&currentSupply, coinbaseAmount);
    currentReward = CalculateBlockReward(currentSupply, currentChain);

    // Persist on accept if requested
    if (persist) {
        Chain_SaveToFile(currentChain, chainDataDir, currentSupply, currentReward);
        BalanceSheet_SaveToFile(chainDataDir);
    }

    // Chain_AddBlock copied the block into the chain; free our temporary wrapper but do NOT destroy transactions (they are freed by Chain_SaveToFile when persisted)
    free(blk);
    // Attempt to attach any orphans that may now have their parents present.
    size_t attached = OrphanPool_AttemptAttach(currentChain);
    if (attached > 0) {
        printf("Attached %zu orphan(s) after accepting block\n", attached);
        // Persist after attaching orphans
        Chain_SaveToFile(currentChain, chainDataDir, currentSupply, currentReward);
        BalanceSheet_SaveToFile(chainDataDir);
    }
    return NODE_BLOCK_ACCEPTED;
}

static void Node_ForwardConnect(net_node_t* node, tcp_connection_t* conn) {
    if (node && node->on_connect) {
        node->on_connect(conn, node->callbackUser);
    }
}

static void Node_ForwardDisconnect(net_node_t* node, tcp_connection_t* conn) {
    if (node && node->on_disconnect) {
        node->on_disconnect(conn, node->callbackUser);
    }
}

static void Node_ForwardData(net_node_t* node, tcp_connection_t* conn, const unsigned char* payload, size_t payloadLen) {
    if (node && node->on_data) {
        node->on_data(conn, payload, payloadLen, node->callbackUser);
    }
}

net_node_t* Node_Create() {
    net_node_t* node = (net_node_t*)malloc(sizeof(net_node_t));
    if (!node) {
        return NULL;
    }

    memset(node, 0, sizeof(*node));

    node->server = TcpServer_Create();
    if (!node->server) {
        free(node);
        return NULL;
    }

    for (size_t i = 0; i < MAX_CONS; ++i) {
        if (TcpClient_Init(&node->outboundClients[i]) != 0) {
            Node_Destroy(node);
            return NULL;
        }
    }

    // Initialize outbound lock and seen-block cache
    pthread_mutex_init(&node->seenLock, NULL);
    pthread_mutex_init(&node->outboundLock, NULL);
    node->seenBlocks = DynSet_Create(32); // 32-byte canonical hashes
    TxMempool_Init();

    TcpServer_Init(node->server, listenPort, "::");

    node->server->owner = node;
    node->server->on_connect = Node_Server_OnConnect;
    node->server->on_data = Node_Server_OnData;
    node->server->on_disconnect = Node_Server_OnDisconnect;

    TcpServer_Start(node->server, MAX_CONS);

    OrphanPool_Init();

    // Start the UDP ping/pong daemon (latency oracle) and peer discovery. Non-fatal on failure;
    // the node still works without discovery, it just won't crawl for new peers.
    node->udpNode = (udp_node_t*)malloc(sizeof(udp_node_t));
    if (node->udpNode) {
        if (UdpNode_Init(node->udpNode, (uint16_t)listenPort) == 0) {
            UdpNode_SetCallbacks(node->udpNode, Node_OnPongThunk, Node_OnPingTimeoutThunk, node);
            if (UdpNode_Start(node->udpNode) == 0) {
                node->discovery = NodeDiscovery_Create(node, node->udpNode);
            } else {
                UdpNode_Destroy(node->udpNode);
                free(node->udpNode);
                node->udpNode = NULL;
            }
        } else {
            free(node->udpNode);
            node->udpNode = NULL;
        }
    }

    // Start maintenance thread
    node->maintenanceRunning = 1;
    node->maintenanceIntervalMs = 1000; // 1s
    if (pthread_create(&node->maintenanceThread, NULL, Node_MaintenanceThread, node) != 0) {
        // Failed to start maintenance thread; continue without it
        node->maintenanceRunning = 0;
    }

    return node;
}

void Node_Destroy(net_node_t* node) {
    if (!node) {
        return;
    }

    for (size_t i = 0; i < MAX_CONS; ++i) {
        TcpClient_Destroy(&node->outboundClients[i]);
    }
    node->outboundCount = 0;

    if (node->server) {
        TcpServer_Stop(node->server);
        TcpServer_Destroy(node->server);
    }

    // Stop maintenance thread (no more discovery ticks after this)
    if (node->maintenanceRunning) {
        node->maintenanceRunning = 0;
        pthread_join(node->maintenanceThread, NULL);
    }

    // Tear down UDP + discovery. Stop UDP first so no pong/timeout callback races the destroy.
    if (node->udpNode) {
        UdpNode_Stop(node->udpNode);
    }
    if (node->discovery) {
        NodeDiscovery_Destroy(node->discovery);
        node->discovery = NULL;
    }
    if (node->udpNode) {
        UdpNode_Destroy(node->udpNode);
        free(node->udpNode);
        node->udpNode = NULL;
    }

    OrphanPool_Destroy();
    TxMempool_Destroy();

    if (node->seenBlocks) {
        DynSet_Destroy(node->seenBlocks);
        node->seenBlocks = NULL;
    }
    pthread_mutex_destroy(&node->seenLock);
    pthread_mutex_destroy(&node->outboundLock);

    free(node);
}

void Node_SetCallbacks(
    net_node_t* node,
    void (*on_connect)(tcp_connection_t* conn, void* user),
    void (*on_data)(tcp_connection_t* conn, const unsigned char* data, size_t len, void* user),
    void (*on_disconnect)(tcp_connection_t* conn, void* user),
    void* user
) {
    if (!node) {
        return;
    }

    node->on_connect = on_connect;
    node->on_data = on_data;
    node->on_disconnect = on_disconnect;
    node->callbackUser = user;
}

int Node_ConnectPeer(net_node_t* node, const char* ip, unsigned short port) {
    if (!node || !ip) {
        return -1;
    }

    // Enforce a single outbound connection per endpoint: if we already have an outbound to this
    // (ip, port), do not open a second one. (Inbound from the same endpoint is still allowed - that
    // is the peer's own outbound to us.)
    struct sockaddr_storage target;
    if (NetNode_MakeEndpoint(ip, port, &target) && Node_HasOutboundTo(node, &target)) {
        return 0; // already connected outbound to this endpoint
    }

    for (size_t i = 0; i < MAX_CONS; ++i) {
        if (node->outboundClients[i].connection == NULL) {
            if (TcpClient_Connect(
                &node->outboundClients[i],
                ip,
                port,
                Node_Client_OnConnect,
                Node_Client_OnData,
                Node_Client_OnDisconnect,
                node
            ) == 0) {
                node->outboundCount++;
                return 0;
            }
            return -1;
        }
    }

    return -1;
}

int Node_ConnectStartupPeers(net_node_t* node, const char** ips, const unsigned short* ports, size_t peersCount) {
    if (!node || !ips || !ports) {
        return -1;
    }

    int successes = 0;
    for (size_t i = 0; i < peersCount; ++i) {
        if (Node_ConnectPeer(node, ips[i], ports[i]) == 0) {
            successes++;
        }
    }

    return successes;
}

int Node_SendPacket(net_node_t* node, tcp_connection_t* conn, packet_type_t packetType, const void* payload, size_t payloadLen) {
    if (!node || !conn || !PacketType_IsValid((uint8_t)packetType) || (!payload && payloadLen > 0)) {
        return -1;
    }

    /*
    if (conn->role == TCP_CONNECTION_ROLE_INBOUND && packetType != PACKET_TYPE_RESPONSE) {
        return -1;
    }

    if (conn->role == TCP_CONNECTION_ROLE_OUTBOUND && packetType != PACKET_TYPE_REQUEST) {
        return -1;
    }
    */

    size_t framePayloadLen = payloadLen + 1;
    unsigned char* framed = (unsigned char*)malloc(framePayloadLen);
    if (!framed) {
        return -1;
    }

    framed[0] = (unsigned char)packetType;
    if (payloadLen > 0) {
        memcpy(framed + 1, payload, payloadLen);
    }

    int rc = TcpConnection_SendFramed(conn, framed, framePayloadLen);
    free(framed);

    return rc;
}

int Node_BroadcastTransaction(net_node_t* node, signed_transaction_t* tx, tcp_connection_t* excludeNode) {
    if (!node || !tx) {
        return -1;
    }

    // Serialize transaction into payload
    size_t payloadLen = sizeof(signed_transaction_t);
    unsigned char* payload = (unsigned char*)malloc(payloadLen);
    if (!payload) {
        return -1;
    }
    memcpy(payload, tx, sizeof(signed_transaction_t));

    // Broadcast to all outbound peers
    pthread_mutex_lock(&node->outboundLock);
    for (size_t i = 0; i < MAX_CONS; ++i) {
        tcp_connection_t* connection = node->outboundClients[i].connection;
        if (connection && connection != excludeNode) {
            (void)Node_SendPacket(node, connection, PACKET_TYPE_BROADCAST_TX, payload, payloadLen);
        }
    }
    pthread_mutex_unlock(&node->outboundLock);

    free(payload);
    return 0;
}

void Node_Server_OnConnect(tcp_connection_t* client) {
    net_node_t* node = Node_FromConnection(client);
    Node_ForwardConnect(node, client);
    printf("Inbound node connected: %u\n", client ? client->connectionId : 0U);

    if (echoPeersEnabled && node && client) {
        // Attempt to create an outbound connection back to the peer's IP on our configured port.
        // We avoid connecting if we already have an outbound to the same IP.
        char ipbuf[INET6_ADDRSTRLEN];
        if (TcpConnection_GetPeerAddrStr(client, ipbuf, sizeof(ipbuf))) {
            // Use the configured port as the target port for the peer's listening service.
            unsigned short targetPort = listenPort;

            int shouldConnect = 1;
            pthread_mutex_lock(&node->outboundLock);
            for (size_t i = 0; i < MAX_CONS; ++i) {
                if (node->outboundClients[i].connection) {
                    if (TcpConnection_PeerAddrEqual(node->outboundClients[i].connection, client)) {
                        shouldConnect = 0;
                        break;
                    }
                }
            }
            pthread_mutex_unlock(&node->outboundLock);

            if (shouldConnect) {
                // Try to connect; ignore failure silently
                (void)Node_ConnectPeer(node, ipbuf, targetPort);
            }
        }
    }
}

void Node_Server_OnData(tcp_connection_t* client) {
    packet_type_t packetType;
    const unsigned char* payload = NULL;
    size_t payloadLen = 0;

    if (!client || Node_DecodePacket(client, &packetType, &payload, &payloadLen) != 0) {
        return;
    }

    switch (packetType) {
        case PACKET_TYPE_HELLO: {
            // Decode HELLO
            if (payloadLen < sizeof(uint32_t) + sizeof(uint64_t)) {
                return;
            }

            uint32_t protoVersion;
            uint64_t blockHeight;
            memcpy(&protoVersion, payload, sizeof(protoVersion));
            memcpy(&blockHeight, payload + sizeof(protoVersion), sizeof(blockHeight));

            // Optional trailing listen port. This inbound peer's source port is ephemeral,
            // so we record the port it actually listens on to make it discoverable/reachable.
            // Length-guarded so older peers that omit it still work.
            if (client && payloadLen >= sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint16_t)) {
                uint16_t peerListenPort;
                memcpy(&peerListenPort, payload + sizeof(protoVersion) + sizeof(blockHeight), sizeof(peerListenPort));
                client->peerListenPort = peerListenPort;
            }

            printf("Received HELLO from node %u: protoVersion=%u, blockHeight=%" PRIu64 ", listenPort=%u\n",
                client ? client->connectionId : 0U, protoVersion, blockHeight,
                client ? client->peerListenPort : 0U);

            // Enforce a single inbound connection per endpoint. Now that we know this peer's listen
            // port, drop this connection if another inbound from the same endpoint already exists
            // (keep the established one). An outbound to the same endpoint is unaffected - that is
            // this node's own connection to the peer.
            if (client && client->peerListenPort != 0) {
                net_node_t* dupNode = Node_FromConnection(client);
                struct sockaddr_storage myEp;
                if (dupNode && Node_ConnListenEndpoint(client, &myEp) &&
                    Node_HasOtherInboundFrom(dupNode, client, &myEp)) {
                    printf("Rejecting duplicate inbound connection %u (already have an inbound from this endpoint)\n",
                        client->connectionId);
                    TcpConnection_RequestClose(client);
                    return;
                }
            }

            // Craft and send ACK_HELLO (echo protoVersion, our height, and our own listen port)
            uint8_t ackBuf[100];
            uint8_t* ackData = ackBuf;
            size_t ackOffset = 0;
            memcpy(ackData + ackOffset, &protoVersion, sizeof(protoVersion));
            ackOffset += sizeof(protoVersion);
            uint64_t currentHeight = Node_GetCurrentBlockHeight();
            memcpy(ackData + ackOffset, &currentHeight, sizeof(currentHeight));
            ackOffset += sizeof(currentHeight);
            uint16_t myListenPort = (uint16_t)listenPort;
            memcpy(ackData + ackOffset, &myListenPort, sizeof(myListenPort));
            ackOffset += sizeof(myListenPort);

            Node_SendPacket(Node_FromConnection(client), client, PACKET_TYPE_ACK_HELLO, ackData, ackOffset);

            break;
        }
        case PACKET_TYPE_ACK_HELLO: {
            // This is illegal
            printf("Received unexpected ACK_HELLO packet from node %u\n", client ? client->connectionId : 0U);

            // Send the error and kill the connection
            const char* msg = "You can't ACK_HELLO me! I'm a server!";
            Node_SendPacket(Node_FromConnection(client), client, PACKET_TYPE_ERROR, msg, strlen(msg));
            TcpConnection_RequestClose(client);
            return;
        }
        case PACKET_TYPE_FETCH_BLOCK: {
            // Decode FETCH_BLOCK - payload is the block height as uint64_t
            if (payloadLen != sizeof(uint64_t)) {
                return;
            }

            uint64_t requestedHeight;
            memcpy(&requestedHeight, payload, sizeof(requestedHeight));

            printf("Received FETCH_BLOCK for height %" PRIu64 " from node %u\n", requestedHeight, client ? client->connectionId : 0U);
            if (requestedHeight > Node_GetCurrentBlockHeight()) {
                printf("Requested block height %" PRIu64 " is higher than current height, ignoring\n", requestedHeight);

                // Error the client, but don't kill
                const char* msg = "Requested block height is higher than my current height!";
                Node_SendPacket(Node_FromConnection(client), client, PACKET_TYPE_ERROR, msg, strlen(msg));

                return;
            }

            // Find the block (deep-copy it for safe access)
            block_t* block = NULL;
            bool loadedFromDisk = false;
            if (!Chain_GetBlockCopy(currentChain, (size_t)requestedHeight, &block) || !block) {
                // Try loading from disk directly
                if (!Chain_LoadBlockFromFile(chainDataDir, requestedHeight, true, &block, NULL) || !block) {
                    printf("Requested block height %" PRIu64 " not found, ignoring\n", requestedHeight);
                    const char* msg = "Requested block not found!";
                    Node_SendPacket(Node_FromConnection(client), client, PACKET_TYPE_ERROR, msg, strlen(msg));
                    return;
                }
                loadedFromDisk = true;
            } else if (!block->transactions) {
                // In-memory chain may be compacted to headers only after persistence.
                block_t* fullBlock = NULL;
                if (Chain_LoadBlockFromFile(chainDataDir, requestedHeight, true, &fullBlock, NULL) && fullBlock) {
                    Block_Destroy(block);
                    block = fullBlock;
                    loadedFromDisk = true;
                }
            }

            if (!block || !block->transactions) {
                printf("Requested block height %" PRIu64 " has no transaction data available\n", requestedHeight);
                const char* msg = "Requested block missing transactions!";
                Node_SendPacket(Node_FromConnection(client), client, PACKET_TYPE_ERROR, msg, strlen(msg));
                if (block) {
                    Block_Destroy(block);
                }
                return;
            }

            if (loadedFromDisk) {
                printf("Serving block %" PRIu64 " from disk with %zu transaction(s)\n",
                    requestedHeight,
                    DynArr_size(block->transactions));
            }

            // Serialize into a BLOCK_DATA packet [block header][tx count - 8 bytes][transactions...]
            size_t txCount = block->transactions ? DynArr_size(block->transactions) : 0;
            size_t blockDataSize = sizeof(uint64_t) + sizeof(block_header_t) + sizeof(uint64_t) + (txCount * sizeof(signed_transaction_t));
            unsigned char* blockData = (unsigned char*)malloc(blockDataSize);
            if (!blockData) {
                // Generic error response
                printf("Failed to allocate memory for block data response to node %u\n", client ? client->connectionId : 0U);
                const char* msg = "Generic error for block data!";
                Node_SendPacket(Node_FromConnection(client), client, PACKET_TYPE_ERROR, msg, strlen(msg));
                Block_Destroy(block);
                return;
            }

            size_t offset = 0;
            // Write height first
            uint64_t heightLE = requestedHeight;
            memcpy(blockData + offset, &heightLE, sizeof(heightLE));
            offset += sizeof(heightLE);
            memcpy(blockData + offset, &block->header, sizeof(block_header_t));
            offset += sizeof(block_header_t);
            uint64_t txCount64 = (uint64_t)txCount;
            memcpy(blockData + offset, &txCount64, sizeof(txCount64));
            offset += sizeof(txCount64);
            if (block->transactions && txCount > 0) {
                for (size_t ti = 0; ti < txCount; ++ti) {
                    signed_transaction_t* tx = (signed_transaction_t*)DynArr_at(block->transactions, ti);
                    memcpy(blockData + offset, tx, sizeof(signed_transaction_t));
                    offset += sizeof(signed_transaction_t);
                }
            }

            // Send the block data
            Node_SendPacket(Node_FromConnection(client), client, PACKET_TYPE_BLOCK_DATA, blockData, offset);
            free(blockData);
            Block_Destroy(block);
            break;
        }
        case PACKET_TYPE_BLOCK_DATA: {
            // Server can't receive these!
            printf("Received unexpected packet type %u from node %u\n", (unsigned int)packetType, client ? client->connectionId : 0U);

            // Send the error and kill the connection
            const char* msg = "You can't send me BLOCK_DATA! I'm a server!";
            Node_SendPacket(Node_FromConnection(client), client, PACKET_TYPE_ERROR, msg, strlen(msg));
            TcpConnection_RequestClose(client);
            return;
        }
        case PACKET_TYPE_BROADCAST_BLOCK: {
            // Accept broadcast blocks from peers and try to append
            if (payloadLen >= sizeof(uint64_t)) {
                uint64_t blockHeight = 0;
                memcpy(&blockHeight, payload, sizeof(blockHeight));
                node_block_accept_result_t result = Node_ParseAndAcceptBlock(payload, payloadLen, true);
                if (result == NODE_BLOCK_ACCEPTED) {
                    printf("Accepted BROADCAST_BLOCK from node %u\n", client ? client->connectionId : 0U);
                    net_node_t* node = Node_FromConnection(client);
                    if (node) {
                        Node_BroadcastChainRange(node, (size_t)blockHeight, client);
                    }
                } else if (result == NODE_BLOCK_ORPHAN_QUEUED) {
                    printf("Queued orphan BROADCAST_BLOCK from node %u\n", client ? client->connectionId : 0U);
                } else {
                    printf("Rejected BROADCAST_BLOCK from node %u\n", client ? client->connectionId : 0U);
                }
            }
            break;
        }
        case PACKET_TYPE_ACK_BLOCK:
        case PACKET_TYPE_BROADCAST_TX: {
            // Decode the block or transaction data inside
            if (payloadLen == sizeof(signed_transaction_t)) {
                signed_transaction_t tx;
                memcpy(&tx, payload, sizeof(tx));
                uint8_t txHash[32];
                char txHashHex[65];
                Transaction_CalculateHash(&tx, txHash);
                to_hex(txHash, txHashHex);
                printf("Received packet type %u from node %u with transaction sending %llu pebble(s)\n",
                    (unsigned int)packetType, client ? client->connectionId : 0U, (unsigned long long)tx.transaction.amount1);

                if (!Transaction_Verify(&tx)) {
                    printf("Received invalid transaction from node %u\n", client ? client->connectionId : 0U);
                    return;
                }

                // Push to mempool if it's not already present
                if (!TxMempool_Lookup(txHash, &tx)) {
                    if (TxMempool_Insert(tx) >= 0) {
                        printf("Added transaction %s from node %u to mempool\n", txHashHex, client ? client->connectionId : 0U);
                        // Broadcast to other peers
                        net_node_t* node = Node_FromConnection(client);
                        if (node) {
                            Node_BroadcastTransaction(node, &tx, client);
                        }
                    } else {
                        printf("Failed to add transaction %s from node %u to mempool\n", txHashHex, client ? client->connectionId : 0U);
                    }
                } else {
                    printf("Transaction %s from node %u already seen!\n", txHashHex, client ? client->connectionId : 0U);
                }

            } else {
                printf("Received packet type %u from node %u with invalid payload length %zu\n",
                    (unsigned int)packetType, client ? client->connectionId : 0U, payloadLen);
                
                // TODO: Ignoring for now, might error node later if we want to be strict about malformed messages
            }

            break;
        }
        case PACKET_TYPE_ACK_TX:
        case PACKET_TYPE_ERROR: {
            // Decode the message inside as text
            char* text = (char*)malloc(payloadLen + 1);
            if (!text) {
                return;
            }
            memcpy(text, payload, payloadLen);
            text[payloadLen] = '\0';
            printf("Received packet type %u from node %u with message: %s\n",
                (unsigned int)packetType, client ? client->connectionId : 0U, text);
            free(text);

            break;
        }
        case PACKET_TYPE_GET_PEERS: {
            net_node_t* dnode = Node_FromConnection(client);
            if (dnode && dnode->discovery) {
                NodeDiscovery_OnGetPeers(dnode->discovery, client);
            }
            break;
        }
        case PACKET_TYPE_PEERS: {
            net_node_t* dnode = Node_FromConnection(client);
            if (dnode && dnode->discovery) {
                NodeDiscovery_OnPeersReceived(dnode->discovery, client, payload, payloadLen);
            }
            break;
        }
        default:
            return;
    }

    net_node_t* node = Node_FromConnection(client);
    Node_ForwardData(node, client, payload, payloadLen);
}

void Node_Server_OnDisconnect(tcp_connection_t* client) {
    net_node_t* node = Node_FromConnection(client);
    Node_ForwardDisconnect(node, client);
    printf("Inbound node disconnected: %u\n", client ? client->connectionId : 0U);
}

void Node_Client_OnConnect(tcp_connection_t* client) {
    net_node_t* node = Node_FromConnection(client);
    Node_ForwardConnect(node, client);
    printf("Outbound node connected: %u\n", client ? client->connectionId : 0U);

    // Construct and send HELLO
    if (node) {
        uint8_t buf[100];
        uint8_t* data = buf;
        
        size_t offset = 0;
        uint32_t protoVersion = PROTO_VERSION; // little-endian
        uint64_t blockHeight = Node_GetCurrentBlockHeight();
        memcpy((unsigned char*)data + offset, &protoVersion, sizeof(protoVersion)); // This is technically "unsafe", but I honestly just don't give a shit at this point
        offset += sizeof(protoVersion);
        memcpy((unsigned char*)data + offset, &blockHeight, sizeof(blockHeight));
        offset += sizeof(blockHeight);
        // Advertise the port we listen on so the peer can share us with others (and reach us back)
        uint16_t myListenPort = (uint16_t)listenPort;
        memcpy((unsigned char*)data + offset, &myListenPort, sizeof(myListenPort));
        offset += sizeof(myListenPort);

        Node_SendPacket(node, client, PACKET_TYPE_HELLO, data, offset);
    }
}

void Node_Client_OnData(tcp_connection_t* client) {
    packet_type_t packetType;
    const unsigned char* payload = NULL;
    size_t payloadLen = 0;

    if (!client || Node_DecodePacket(client, &packetType, &payload, &payloadLen) != 0) {
        return;
    }

    switch (packetType) {
        case PACKET_TYPE_HELLO: {
            // This is illegal
            printf("Received unexpected HELLO packet from node %u\n", client ? client->connectionId : 0U);

            // Send the error and kill the connection
            const char* msg = "You can't HELLO me! I'm a client!";
            Node_SendPacket(Node_FromConnection(client), client, PACKET_TYPE_ERROR, msg, strlen(msg));
            TcpConnection_RequestClose(client);
            return;
        }
        case PACKET_TYPE_ACK_HELLO: {
            // Decode ACK_HELLO
            if (payloadLen < sizeof(uint32_t) + sizeof(uint64_t)) {
                return;
            }

            uint32_t protoVersion;
            uint64_t blockHeight;
            memcpy(&protoVersion, payload, sizeof(protoVersion));
            memcpy(&blockHeight, payload + sizeof(protoVersion), sizeof(blockHeight));

            // Optional trailing listen port (for outbound peers the dialed port is already the
            // listen port, but record the advertised one for consistency). Length-guarded.
            if (client && payloadLen >= sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint16_t)) {
                uint16_t peerListenPort;
                memcpy(&peerListenPort, payload + sizeof(protoVersion) + sizeof(blockHeight), sizeof(peerListenPort));
                client->peerListenPort = peerListenPort;
            }

            printf("Received ACK_HELLO from node %u with protoVersion %u and blockHeight %" PRIu64 "\n", client ? client->connectionId : 0U, protoVersion, blockHeight);

            // Store peer-advertised height on matching outbound client
            net_node_t* node = Node_FromConnection(client);
            if (node) {
                pthread_mutex_lock(&node->outboundLock);
                for (size_t i = 0; i < MAX_CONS; ++i) {
                    if (node->outboundClients[i].connection == client) {
                        node->outboundClients[i].peerBlockHeight = blockHeight;
                        break;
                    }
                }
                pthread_mutex_unlock(&node->outboundLock);
            }
            break;
        }
        case PACKET_TYPE_FETCH_BLOCK: {
            // A client can't serve a block!
            printf("Received unexpected FETCH_BLOCK packet from node %u\n", client ? client->connectionId : 0U);

            // Send the error and kill the connection (this might be too aggressive)
            const char* msg = "You can't FETCH_BLOCK from me! I'm a client!";
            Node_SendPacket(Node_FromConnection(client), client, PACKET_TYPE_ERROR, msg, strlen(msg));
            TcpConnection_RequestClose(client);
            return;
        }
        case PACKET_TYPE_BLOCK_DATA: {
            if (payloadLen >= sizeof(uint64_t)) {
                uint64_t blockHeight = 0;
                memcpy(&blockHeight, payload, sizeof(blockHeight));
                node_block_accept_result_t result = Node_ParseAndAcceptBlock(payload, payloadLen, true);
                if (result == NODE_BLOCK_ACCEPTED) {
                    printf("Accepted BLOCK_DATA from node %u\n", client ? client->connectionId : 0U);
                    net_node_t* node = Node_FromConnection(client);
                    if (node) {
                        // Update peer advertised height
                        pthread_mutex_lock(&node->outboundLock);
                        for (size_t i = 0; i < MAX_CONS; ++i) {
                            if (node->outboundClients[i].connection == client) {
                                if (node->outboundClients[i].peerBlockHeight < blockHeight) {
                                    node->outboundClients[i].peerBlockHeight = blockHeight;
                                }
                                break;
                            }
                        }
                        pthread_mutex_unlock(&node->outboundLock);

                        Node_BroadcastChainRange(node, (size_t)blockHeight, client);
                    }
                } else if (result == NODE_BLOCK_ORPHAN_QUEUED) {
                    printf("Queued orphan BLOCK_DATA from node %u\n", client ? client->connectionId : 0U);
                } else {
                    printf("Rejected BLOCK_DATA from node %u\n", client ? client->connectionId : 0U);
                }
            }
            break;
        }
        case PACKET_TYPE_BROADCAST_BLOCK: {
            if (payloadLen >= sizeof(uint64_t)) {
                uint64_t blockHeight = 0;
                memcpy(&blockHeight, payload, sizeof(blockHeight));
                node_block_accept_result_t result = Node_ParseAndAcceptBlock(payload, payloadLen, true);
                if (result == NODE_BLOCK_ACCEPTED) {
                    printf("Accepted BROADCAST_BLOCK from node %u\n", client ? client->connectionId : 0U);
                    net_node_t* node = Node_FromConnection(client);
                    if (node) {
                        // Update peer advertised height
                        pthread_mutex_lock(&node->outboundLock);
                        for (size_t i = 0; i < MAX_CONS; ++i) {
                            if (node->outboundClients[i].connection == client) {
                                if (node->outboundClients[i].peerBlockHeight < blockHeight) {
                                    node->outboundClients[i].peerBlockHeight = blockHeight;
                                }
                                break;
                            }
                        }
                        pthread_mutex_unlock(&node->outboundLock);

                        Node_BroadcastChainRange(node, (size_t)blockHeight, client);
                    }
                } else if (result == NODE_BLOCK_ORPHAN_QUEUED) {
                    printf("Queued orphan BROADCAST_BLOCK from node %u\n", client ? client->connectionId : 0U);
                } else {
                    printf("Rejected BROADCAST_BLOCK from node %u\n", client ? client->connectionId : 0U);
                }
            }
            break;
        }
        case PACKET_TYPE_ACK_BLOCK:
        case PACKET_TYPE_BROADCAST_TX: {
            // Client can't receive these!
            printf("Received unexpected packet type %u from node %u\n", (unsigned int)packetType, client ? client->connectionId : 0U);

            break;
        }
        case PACKET_TYPE_ACK_TX:
        case PACKET_TYPE_ERROR: {
            // Decode the message inside as text
            char* text = (char*)malloc(payloadLen + 1);
            if (!text) {
                return;
            }
            memcpy(text, payload, payloadLen);
            text[payloadLen] = '\0';
            printf("Received packet type %u from node %u with message: %s\n",
                (unsigned int)packetType, client ? client->connectionId : 0U, text);
            free(text);

            break;
        }
        case PACKET_TYPE_GET_PEERS: {
            net_node_t* dnode = Node_FromConnection(client);
            if (dnode && dnode->discovery) {
                NodeDiscovery_OnGetPeers(dnode->discovery, client);
            }
            break;
        }
        case PACKET_TYPE_PEERS: {
            net_node_t* dnode = Node_FromConnection(client);
            if (dnode && dnode->discovery) {
                NodeDiscovery_OnPeersReceived(dnode->discovery, client, payload, payloadLen);
            }
            break;
        }
        default:
            return;
    }

    net_node_t* node = Node_FromConnection(client);
    Node_ForwardData(node, client, payload, payloadLen);
}

void Node_Client_OnDisconnect(tcp_connection_t* client) {
    net_node_t* node = Node_FromConnection(client);
    if (node) {
        // Clear peer advertised height for this outbound slot
        pthread_mutex_lock(&node->outboundLock);
        for (size_t i = 0; i < MAX_CONS; ++i) {
            if (node->outboundClients[i].connection == client) {
                node->outboundClients[i].peerBlockHeight = 0;
                break;
            }
        }
        pthread_mutex_unlock(&node->outboundLock);

        if (node->outboundCount > 0) {
            node->outboundCount--;
        }
    }

    Node_ForwardDisconnect(node, client);
    printf("Outbound node disconnected: %u\n", client ? client->connectionId : 0U);
}

int Node_GetBestOutboundPeer(net_node_t* node, tcp_connection_t** outConn, uint64_t* outHeight) {
    if (!node || !outConn || !outHeight) return -1;

    tcp_connection_t* best = NULL;
    uint64_t bestH = 0;

    pthread_mutex_lock(&node->outboundLock);
    for (size_t i = 0; i < MAX_CONS; ++i) {
        if (node->outboundClients[i].connection) {
            if (node->outboundClients[i].peerBlockHeight > bestH || best == NULL) {
                best = node->outboundClients[i].connection;
                bestH = node->outboundClients[i].peerBlockHeight;
            }
        }
    }
    pthread_mutex_unlock(&node->outboundLock);

    if (!best) return -1;
    *outConn = best;
    *outHeight = bestH;
    return 0;
}

void Node_BroadcastChainRange(net_node_t* node, size_t startHeightInclusive, tcp_connection_t* sourceConn) {
    if (!node || !currentChain) return;

    size_t chainSize = Chain_Size(currentChain);
    if (startHeightInclusive >= chainSize) return;

    for (size_t h = startHeightInclusive; h < chainSize; ++h) {
        block_t* blk = NULL;
        if (!Chain_GetBlockCopy(currentChain, h, &blk) || !blk) {
            if (!Chain_LoadBlockFromFile(chainDataDir, h, true, &blk, NULL) || !blk) {
                continue;
            }
        } else if (!blk->transactions) {
            block_t* full = NULL;
            if (Chain_LoadBlockFromFile(chainDataDir, h, true, &full, NULL) && full) {
                Block_Destroy(blk);
                blk = full;
            }
        }

        if (!blk || !blk->transactions) {
            if (blk) Block_Destroy(blk);
            continue;
        }

        unsigned char hash[32];
        Block_CalculateHash(blk, hash);

        // Dedupe using seenBlocks
        int seen = 0;
        pthread_mutex_lock(&node->seenLock);
        if (DynSet_Contains(node->seenBlocks, hash)) {
            seen = 1;
        } else {
            DynSet_Insert(node->seenBlocks, hash);
        }
        pthread_mutex_unlock(&node->seenLock);

        if (seen) {
            Block_Destroy(blk);
            continue;
        }

        // Serialize payload: [uint64_t height][block_header_t][uint64_t txCount][transactions...]
        size_t txCount = DynArr_size(blk->transactions);
        size_t payloadLen = sizeof(uint64_t) + sizeof(block_header_t) + sizeof(uint64_t) + (txCount * sizeof(signed_transaction_t));
        unsigned char* payload = (unsigned char*)malloc(payloadLen);
        if (!payload) {
            Block_Destroy(blk);
            continue;
        }
        size_t off = 0;
        uint64_t h64 = (uint64_t)h;
        memcpy(payload + off, &h64, sizeof(h64)); off += sizeof(h64);
        memcpy(payload + off, &blk->header, sizeof(block_header_t)); off += sizeof(block_header_t);
        uint64_t txCount64 = (uint64_t)txCount;
        memcpy(payload + off, &txCount64, sizeof(txCount64)); off += sizeof(txCount64);
        for (size_t ti = 0; ti < txCount; ++ti) {
            signed_transaction_t* tx = (signed_transaction_t*)DynArr_at(blk->transactions, ti);
            memcpy(payload + off, tx, sizeof(signed_transaction_t)); off += sizeof(signed_transaction_t);
        }

        // Snapshot outbound clients and send
        pthread_mutex_lock(&node->outboundLock);
        for (size_t i = 0; i < MAX_CONS; ++i) {
            tcp_connection_t* conn = node->outboundClients[i].connection;
            if (!conn) continue;
            if (conn == sourceConn) continue;
            if (sourceConn && TcpConnection_PeerAddrEqual(conn, sourceConn)) continue;
            Node_SendPacket(node, conn, PACKET_TYPE_BROADCAST_BLOCK, payload, off);
        }
        pthread_mutex_unlock(&node->outboundLock);

        free(payload);
        Block_Destroy(blk);
    }
}

void Node_GetClientList(net_node_t* node, tcp_connection_t** outClients, size_t* outCount) {
    if (!node || !outClients || !outCount) return;

    pthread_mutex_lock(&node->outboundLock);
    size_t count = 0;
    for (size_t i = 0; i < MAX_CONS; ++i) {
        if (node->outboundClients[i].connection) {
            outClients[count++] = node->outboundClients[i].connection;
        }
    }
    pthread_mutex_unlock(&node->outboundLock);

    *outCount = count;
}
