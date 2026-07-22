#include <nets/nodediscovery.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <constants.h>
#include <dynarr.h>
#include <numgen.h>
#include <utils.h>

// Wire layout of a single peer endpoint inside a PEERS payload:
//   [uint8 family (4 or 6)][uint8 ip[16]][uint16 port (host order)]  -> 19 bytes
// (Host-endian raw layout, consistent with the rest of the protocol.)
#define DISCOVERY_WIRE_ENTRY_SIZE (1 + 16 + 2)

typedef enum {
    DISCOVERY_STATE_NEW = 0,      // learned, not yet pinged
    DISCOVERY_STATE_PINGED,       // first ping in flight, reachability unknown
    DISCOVERY_STATE_REACHABLE,    // pong received, latency known
    DISCOVERY_STATE_CONNECTED,    // currently a live outbound connection
    DISCOVERY_STATE_UNREACHABLE   // ping timed out
} discovery_state_t;

typedef struct {
    struct sockaddr_storage addr;  // listen endpoint (port already set to the peer's listen port)
    uint64_t pingMs;               // measured UDP RTT, UINT64_MAX if unknown
    uint32_t hop;                  // distance from us (0 = directly connected)
    discovery_state_t state;
    int pingPending;               // 1 while a ping is outstanding (matched by address on pong/timeout).
                                   // The UDP layer generates its own nonce, so we can't match by nonce here.
    uint64_t lastPingMs;           // when we last sent a ping
    uint64_t lastQueryMs;          // when we last sent GET_PEERS to it
    uint64_t lastConnectMs;        // when we last attempted a connect to it
} discovered_peer_t;

struct node_discovery {
    net_node_t* node;
    udp_node_t* udpNode;
    DynArr* peers;                 // of discovered_peer_t
    pthread_mutex_t lock;
};

// ---- small helpers (most assume the caller holds disc->lock) ------------------------------

static int Discovery_AddrEqual(const struct sockaddr_storage* a, const struct sockaddr_storage* b) {
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

static discovered_peer_t* Discovery_FindPtr(node_discovery_t* disc, const struct sockaddr_storage* addr) {
    size_t n = DynArr_size(disc->peers);
    for (size_t i = 0; i < n; ++i) {
        discovered_peer_t* p = (discovered_peer_t*)DynArr_at(disc->peers, i);
        if (Discovery_AddrEqual(&p->addr, addr)) return p;
    }
    return NULL;
}

// Insert addr if not already present. Returns a pointer to the (existing or new) entry, or NULL
// if the table is full. Note: the returned pointer is invalidated by any later push_back.
static discovered_peer_t* Discovery_Upsert(node_discovery_t* disc, const struct sockaddr_storage* addr, uint32_t hop) {
    discovered_peer_t* existing = Discovery_FindPtr(disc, addr);
    if (existing) {
        if (hop < existing->hop) existing->hop = hop; // keep the shortest known distance
        return existing;
    }
    if (DynArr_size(disc->peers) >= DISCOVERY_MAX_KNOWN_PEERS) return NULL;

    discovered_peer_t np;
    memset(&np, 0, sizeof(np));
    np.addr = *addr;
    np.pingMs = UINT64_MAX;
    np.hop = hop;
    np.state = DISCOVERY_STATE_NEW;
    DynArr_push_back(disc->peers, &np);
    return (discovered_peer_t*)DynArr_at(disc->peers, DynArr_size(disc->peers) - 1);
}

static int Discovery_AddrToWire(const struct sockaddr_storage* addr, unsigned char out[DISCOVERY_WIRE_ENTRY_SIZE]) {
    memset(out, 0, DISCOVERY_WIRE_ENTRY_SIZE);
    if (addr->ss_family == AF_INET) {
        const struct sockaddr_in* a = (const struct sockaddr_in*)addr;
        out[0] = 4;
        memcpy(out + 1, &a->sin_addr, sizeof(struct in_addr));
        uint16_t port = ntohs(a->sin_port);
        memcpy(out + 1 + 16, &port, sizeof(port));
        return 1;
    }
    if (addr->ss_family == AF_INET6) {
        const struct sockaddr_in6* a = (const struct sockaddr_in6*)addr;
        out[0] = 6;
        memcpy(out + 1, &a->sin6_addr, sizeof(struct in6_addr));
        uint16_t port = ntohs(a->sin6_port);
        memcpy(out + 1 + 16, &port, sizeof(port));
        return 1;
    }
    return 0;
}

static int Discovery_WireToAddr(const unsigned char in[DISCOVERY_WIRE_ENTRY_SIZE], struct sockaddr_storage* out) {
    memset(out, 0, sizeof(*out));
    uint8_t fam = in[0];
    uint16_t port;
    memcpy(&port, in + 1 + 16, sizeof(port));
    if (fam == 4) {
        struct sockaddr_in* a = (struct sockaddr_in*)out;
        a->sin_family = AF_INET;
        memcpy(&a->sin_addr, in + 1, sizeof(struct in_addr));
        a->sin_port = htons(port);
        return port != 0;
    }
    if (fam == 6) {
        struct sockaddr_in6* a = (struct sockaddr_in6*)out;
        a->sin6_family = AF_INET6;
        memcpy(&a->sin6_addr, in + 1, sizeof(struct in6_addr));
        a->sin6_port = htons(port);
        return port != 0;
    }
    return 0;
}

static int Discovery_AddrToIpPort(const struct sockaddr_storage* addr, char* ipOut, size_t ipLen, unsigned short* portOut) {
    if (addr->ss_family == AF_INET) {
        const struct sockaddr_in* a = (const struct sockaddr_in*)addr;
        if (!inet_ntop(AF_INET, &a->sin_addr, ipOut, (socklen_t)ipLen)) return 0;
        *portOut = ntohs(a->sin_port);
        return 1;
    }
    if (addr->ss_family == AF_INET6) {
        const struct sockaddr_in6* a = (const struct sockaddr_in6*)addr;
        if (!inet_ntop(AF_INET6, &a->sin6_addr, ipOut, (socklen_t)ipLen)) return 0;
        *portOut = ntohs(a->sin6_port);
        return 1;
    }
    return 0;
}

// ---- lifecycle ---------------------------------------------------------------------------

node_discovery_t* NodeDiscovery_Create(net_node_t* node, udp_node_t* udpNode) {
    if (!node || !udpNode) return NULL;
    node_discovery_t* disc = (node_discovery_t*)malloc(sizeof(node_discovery_t));
    if (!disc) return NULL;
    memset(disc, 0, sizeof(*disc));
    disc->node = node;
    disc->udpNode = udpNode;
    disc->peers = DYNARR_CREATE(discovered_peer_t, 16);
    if (!disc->peers) {
        free(disc);
        return NULL;
    }
    pthread_mutex_init(&disc->lock, NULL);
    return disc;
}

void NodeDiscovery_Destroy(node_discovery_t* disc) {
    if (!disc) return;
    if (disc->peers) DynArr_destroy(disc->peers);
    pthread_mutex_destroy(&disc->lock);
    free(disc);
}

// ---- UDP latency callbacks ---------------------------------------------------------------

void NodeDiscovery_OnPong(node_discovery_t* disc, const struct sockaddr_storage* from, uint64_t nonce, uint64_t rttMs) {
    if (!disc || !from) return;
    (void)nonce; // UDP layer owns the nonce; we match the peer by its reply address instead.
    pthread_mutex_lock(&disc->lock);
    discovered_peer_t* p = Discovery_FindPtr(disc, from);
    if (p && p->pingPending) {
        p->pingMs = rttMs;
        p->pingPending = 0;
        if (p->state == DISCOVERY_STATE_PINGED) p->state = DISCOVERY_STATE_REACHABLE;
    }
    pthread_mutex_unlock(&disc->lock);
}

void NodeDiscovery_OnPingTimeout(node_discovery_t* disc, const struct sockaddr_storage* dest, uint64_t nonce) {
    if (!disc || !dest) return;
    (void)nonce; // matched by the destination address we pinged
    pthread_mutex_lock(&disc->lock);
    discovered_peer_t* p = Discovery_FindPtr(disc, dest);
    if (p && p->pingPending) {
        p->pingPending = 0;
        // Only demote a peer whose reachability was still unknown; a refresh ping that times
        // out on an already-connected/reachable peer must not drop it.
        if (p->state == DISCOVERY_STATE_PINGED) p->state = DISCOVERY_STATE_UNREACHABLE;
    }
    pthread_mutex_unlock(&disc->lock);
}

// ---- TCP peer exchange -------------------------------------------------------------------

void NodeDiscovery_OnGetPeers(node_discovery_t* disc, tcp_connection_t* fromConn) {
    if (!disc || !fromConn) return;

    // Snapshot our current peers' listen endpoints (inbound + outbound).
    struct sockaddr_storage all[MAX_CONS * 2];
    size_t total = Node_GetPeerEndpoints(disc->node, all, sizeof(all) / sizeof(all[0]));

    struct sockaddr_storage reqEndpoint;
    int haveReq = Node_ConnListenEndpoint(fromConn, &reqEndpoint);

    // Build the response payload: [uint16 count][entries...], capped and sampled for spread.
    unsigned char payload[sizeof(uint16_t) + DISCOVERY_PEERS_RESPONSE_CAP * DISCOVERY_WIRE_ENTRY_SIZE];
    size_t offset = sizeof(uint16_t);
    uint16_t count = 0;

    size_t startIdx = total ? (size_t)(random_four_byte() % total) : 0;
    for (size_t k = 0; k < total && count < DISCOVERY_PEERS_RESPONSE_CAP; ++k) {
        size_t idx = (startIdx + k) % total;
        if (haveReq && Discovery_AddrEqual(&all[idx], &reqEndpoint)) continue; // don't tell them about themselves
        unsigned char entry[DISCOVERY_WIRE_ENTRY_SIZE];
        if (!Discovery_AddrToWire(&all[idx], entry)) continue;
        memcpy(payload + offset, entry, DISCOVERY_WIRE_ENTRY_SIZE);
        offset += DISCOVERY_WIRE_ENTRY_SIZE;
        count++;
    }
    memcpy(payload, &count, sizeof(count));

    Node_SendPacket(disc->node, fromConn, PACKET_TYPE_PEERS, payload, offset);
}

void NodeDiscovery_OnPeersReceived(node_discovery_t* disc, tcp_connection_t* fromConn, const unsigned char* payload, size_t payloadLen) {
    if (!disc || !payload || payloadLen < sizeof(uint16_t)) return;

    uint16_t count;
    memcpy(&count, payload, sizeof(count));
    size_t need = sizeof(uint16_t) + (size_t)count * DISCOVERY_WIRE_ENTRY_SIZE;
    if (payloadLen < need) return; // malformed / truncated

    // Determine the hop distance of the peer that answered, so its peers land one hop further out.
    struct sockaddr_storage srcEndpoint;
    int haveSrc = fromConn ? Node_ConnListenEndpoint(fromConn, &srcEndpoint) : 0;

    pthread_mutex_lock(&disc->lock);

    uint32_t srcHop = 0;
    if (haveSrc) {
        discovered_peer_t* srcp = Discovery_FindPtr(disc, &srcEndpoint);
        if (srcp) srcHop = srcp->hop;
    }
    uint32_t newHop = srcHop + 1;

    // Fold in at most DISCOVERY_FANOUT *new* endpoints (a couple per node -> keeps the crawl spread).
    if (newHop <= DISCOVERY_MAX_HOPS) {
        int added = 0;
        for (uint16_t i = 0; i < count && added < DISCOVERY_FANOUT; ++i) {
            const unsigned char* entry = payload + sizeof(uint16_t) + (size_t)i * DISCOVERY_WIRE_ENTRY_SIZE;
            struct sockaddr_storage ep;
            if (!Discovery_WireToAddr(entry, &ep)) continue;
            if (Discovery_FindPtr(disc, &ep) != NULL) continue; // already known -> doesn't count toward fanout
            if (Discovery_Upsert(disc, &ep, newHop) != NULL) added++;
        }
    }

    pthread_mutex_unlock(&disc->lock);
}

// ---- periodic tick -----------------------------------------------------------------------

void NodeDiscovery_Iterate(node_discovery_t* disc) {
    if (!disc || !disc->node || !disc->udpNode) return;
    uint64_t now = get_current_time_ms();

    // Snapshot current outbound connections and their listen endpoints (used for querying and
    // for the "already connected?" checks below).
    tcp_connection_t* outConns[MAX_CONS];
    size_t outCount = 0;
    Node_GetClientList(disc->node, outConns, &outCount);

    struct sockaddr_storage outEndpoints[MAX_CONS];
    size_t outEpCount = 0;
    for (size_t i = 0; i < outCount; ++i) {
        struct sockaddr_storage ep;
        if (Node_ConnListenEndpoint(outConns[i], &ep)) {
            outEndpoints[outEpCount++] = ep;
        }
    }

    // Deferred network actions, collected under the lock and executed after releasing it
    // (Node_ConnectPeer / Node_SendPacket must not run while holding disc->lock).
    tcp_connection_t* toQuery[MAX_CONS];
    size_t toQueryCount = 0;
    struct { char ip[INET6_ADDRSTRLEN]; unsigned short port; } toConnect[MAX_CONS];
    size_t toConnectCount = 0;

    pthread_mutex_lock(&disc->lock);

    // 1. Seed: upsert connected (outbound) peers as CONNECTED at hop 0.
    for (size_t i = 0; i < outEpCount; ++i) {
        discovered_peer_t* p = Discovery_Upsert(disc, &outEndpoints[i], 0);
        if (p) {
            p->hop = 0;
            p->state = DISCOVERY_STATE_CONNECTED;
        }
    }
    // Demote entries still marked CONNECTED that are no longer in the outbound set.
    {
        size_t n = DynArr_size(disc->peers);
        for (size_t i = 0; i < n; ++i) {
            discovered_peer_t* p = (discovered_peer_t*)DynArr_at(disc->peers, i);
            if (p->state != DISCOVERY_STATE_CONNECTED) continue;
            int stillConnected = 0;
            for (size_t j = 0; j < outEpCount; ++j) {
                if (Discovery_AddrEqual(&p->addr, &outEndpoints[j])) { stillConnected = 1; break; }
            }
            if (!stillConnected) {
                p->state = (p->pingMs != UINT64_MAX) ? DISCOVERY_STATE_REACHABLE : DISCOVERY_STATE_NEW;
            }
        }
    }

    // 2. Ping NEW peers (and refresh stale REACHABLE ones), capped per tick.
    {
        int pings = 0;
        size_t n = DynArr_size(disc->peers);
        for (size_t i = 0; i < n && pings < DISCOVERY_MAX_PINGS_PER_TICK; ++i) {
            discovered_peer_t* p = (discovered_peer_t*)DynArr_at(disc->peers, i);
            int shouldPing = 0;
            if (!p->pingPending) {
                if (p->state == DISCOVERY_STATE_NEW) {
                    shouldPing = 1;
                } else if (p->state == DISCOVERY_STATE_REACHABLE &&
                           (now - p->lastPingMs) > DISCOVERY_PING_REFRESH_MS) {
                    shouldPing = 1;
                }
            }
            if (!shouldPing) continue;

            p->pingPending = 1;
            p->lastPingMs = now;
            if (p->state == DISCOVERY_STATE_NEW) p->state = DISCOVERY_STATE_PINGED;
            UdpNode_SendPing(disc->udpNode, &p->addr);
            pings++;
        }
    }

    // 3. Timeout backstop (in case the UDP layer's own timeout callback is missed).
    {
        size_t n = DynArr_size(disc->peers);
        for (size_t i = 0; i < n; ++i) {
            discovered_peer_t* p = (discovered_peer_t*)DynArr_at(disc->peers, i);
            if (p->state == DISCOVERY_STATE_PINGED && p->pingPending &&
                (now - p->lastPingMs) > DISCOVERY_PING_TIMEOUT_MS) {
                p->pingPending = 0;
                p->state = DISCOVERY_STATE_UNREACHABLE;
            }
        }
    }

    // 4. Query up to DISCOVERY_FANOUT connected peers with GET_PEERS, preferring the lowest ping
    //    and skipping ones we queried recently or that are already at the hop horizon.
    {
        int queries = 0;
        while (queries < DISCOVERY_FANOUT) {
            size_t bestIdx = outCount;         // sentinel = none
            uint64_t bestPing = UINT64_MAX;
            for (size_t i = 0; i < outCount; ++i) {
                struct sockaddr_storage ep;
                if (!Node_ConnListenEndpoint(outConns[i], &ep)) continue;
                discovered_peer_t* p = Discovery_FindPtr(disc, &ep);
                if (!p) continue;
                if (p->hop >= DISCOVERY_MAX_HOPS) continue;
                if (p->lastQueryMs != 0 && (now - p->lastQueryMs) < DISCOVERY_QUERY_INTERVAL_MS) continue;
                if (bestIdx == outCount || p->pingMs < bestPing) {
                    bestIdx = i;
                    bestPing = p->pingMs;
                }
            }
            if (bestIdx == outCount) break; // nothing eligible

            // Mark queried so it isn't picked again this tick, and queue the send.
            struct sockaddr_storage ep;
            if (Node_ConnListenEndpoint(outConns[bestIdx], &ep)) {
                discovered_peer_t* p = Discovery_FindPtr(disc, &ep);
                if (p) p->lastQueryMs = now;
            }
            toQuery[toQueryCount++] = outConns[bestIdx];
            queries++;
        }
    }

    // 5. Connect: pick REACHABLE, not-currently-connected, cooled-down peers with the lowest ping
    //    until we reach the target connection count.
    if (outCount < (size_t)DISCOVERY_TARGET_CONNECTIONS) {
        size_t slots = (size_t)DISCOVERY_TARGET_CONNECTIONS - outCount;
        for (size_t s = 0; s < slots && toConnectCount < MAX_CONS; ++s) {
            discovered_peer_t* best = NULL;
            size_t n = DynArr_size(disc->peers);
            for (size_t i = 0; i < n; ++i) {
                discovered_peer_t* p = (discovered_peer_t*)DynArr_at(disc->peers, i);
                if (p->state != DISCOVERY_STATE_REACHABLE) continue;
                if (p->lastConnectMs != 0 && (now - p->lastConnectMs) < DISCOVERY_CONNECT_RETRY_MS) continue;
                int already = 0;
                for (size_t j = 0; j < outEpCount; ++j) {
                    if (Discovery_AddrEqual(&p->addr, &outEndpoints[j])) { already = 1; break; }
                }
                if (already) continue;
                if (!best || p->pingMs < best->pingMs) best = p;
            }
            if (!best) break;

            best->lastConnectMs = now; // reserve so it isn't picked again this tick
            char ip[INET6_ADDRSTRLEN];
            unsigned short port = 0;
            if (Discovery_AddrToIpPort(&best->addr, ip, sizeof(ip), &port) && port != 0) {
                strncpy(toConnect[toConnectCount].ip, ip, INET6_ADDRSTRLEN - 1);
                toConnect[toConnectCount].ip[INET6_ADDRSTRLEN - 1] = '\0';
                toConnect[toConnectCount].port = port;
                toConnectCount++;
            }
        }
    }

    pthread_mutex_unlock(&disc->lock);

    // Execute the deferred network actions outside the lock.
    for (size_t i = 0; i < toQueryCount; ++i) {
        Node_SendPacket(disc->node, toQuery[i], PACKET_TYPE_GET_PEERS, NULL, 0);
    }
    for (size_t i = 0; i < toConnectCount; ++i) {
        printf("NodeDiscovery: connecting to discovered peer %s:%u\n", toConnect[i].ip, toConnect[i].port);
        (void)Node_ConnectPeer(disc->node, toConnect[i].ip, toConnect[i].port);
    }
}

// ---- diagnostics -------------------------------------------------------------------------

void NodeDiscovery_PrintPeers(node_discovery_t* disc) {
    if (!disc) {
        printf("NodeDiscovery: not active\n");
        return;
    }

    static const char* stateNames[] = { "NEW", "PINGED", "REACHABLE", "CONNECTED", "UNREACHABLE" };

    pthread_mutex_lock(&disc->lock);
    size_t n = DynArr_size(disc->peers);
    printf("Known peers (%zu):\n", n);
    for (size_t i = 0; i < n; ++i) {
        discovered_peer_t* p = (discovered_peer_t*)DynArr_at(disc->peers, i);
        char ip[INET6_ADDRSTRLEN] = {0};
        unsigned short port = 0;
        Discovery_AddrToIpPort(&p->addr, ip, sizeof(ip), &port);
        const char* stateStr = (p->state <= DISCOVERY_STATE_UNREACHABLE) ? stateNames[p->state] : "?";
        if (p->pingMs == UINT64_MAX) {
            printf("  %-46s hop=%u state=%-11s ping=--\n", ip, p->hop, stateStr);
        } else {
            printf("  %-46s hop=%u state=%-11s ping=%" PRIu64 "ms\n", ip, p->hop, stateStr, p->pingMs);
        }
        (void)port; // port is part of ip endpoint identity; shown via connect logs
    }
    pthread_mutex_unlock(&disc->lock);
}
