#include <nets/nodediscovery.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <ifaddrs.h>

#include <constants.h>
#include <dynarr.h>
#include <numgen.h>
#include <runtime_state.h>
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
    uint64_t nodeId;               // identity of the node behind this endpoint, 0 while unknown
    uint32_t hop;                  // distance from us (0 = directly connected)
    discovery_state_t state;
    int pingPending;               // 1 while a ping is outstanding (matched by address on pong/timeout).
                                   // The UDP layer generates its own nonce, so we can't match by nonce here.
    uint64_t lastPingMs;           // when we last sent a ping
    uint64_t lastQueryMs;          // when we last sent GET_PEERS to it
} discovered_peer_t;

// When we last dialed an endpoint. Kept outside the peer table on purpose: a peer entry is struck
// the moment its connection drops, and if the dial history went with it, an endpoint that hangs up
// on us would be re-learned through gossip and redialed on every single tick.
typedef struct {
    struct sockaddr_storage addr;
    uint64_t lastMs;
} discovery_attempt_t;

struct node_discovery {
    net_node_t* node;
    udp_node_t* udpNode;
    DynArr* peers;                 // of discovered_peer_t
    DynArr* selfEndpoints;         // of struct sockaddr_storage - our own listen endpoints
    DynArr* connectAttempts;       // of discovery_attempt_t
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

// Rejects endpoints that can never be dialed as written. IPv6 in particular hands us plenty of
// these: link-local addresses are meaningless without the scope id (which the wire format does not
// carry), and the unspecified/multicast ranges are never a peer. Loopback stays allowed so several
// nodes can still be run on one machine on different ports.
static int Discovery_IsUsableAddr(const struct sockaddr_storage* addr) {
    if (addr->ss_family == AF_INET) {
        const struct sockaddr_in* a = (const struct sockaddr_in*)addr;
        if (a->sin_port == 0) return 0;
        uint32_t host = ntohl(a->sin_addr.s_addr);
        if (host == INADDR_ANY || host == INADDR_BROADCAST) return 0;
        if ((host >> 28) == 0xE) return 0;          // 224.0.0.0/4 multicast
        if ((host & 0xFFFF0000u) == 0xA9FE0000u) return 0; // 169.254.0.0/16 link-local
        return 1;
    }
    if (addr->ss_family == AF_INET6) {
        const struct sockaddr_in6* a = (const struct sockaddr_in6*)addr;
        if (a->sin6_port == 0) return 0;
        if (IN6_IS_ADDR_UNSPECIFIED(&a->sin6_addr)) return 0;
        if (IN6_IS_ADDR_MULTICAST(&a->sin6_addr)) return 0;
        if (IN6_IS_ADDR_LINKLOCAL(&a->sin6_addr)) return 0;  // unusable without a scope id
        if (IN6_IS_ADDR_SITELOCAL(&a->sin6_addr)) return 0;  // deprecated fec0::/10
        return 1;
    }
    return 0;
}

// Rewrites an IPv4-mapped IPv6 endpoint (::ffff:a.b.c.d) as plain IPv4, so the same host never
// occupies two entries. Matches the normalisation Node_ConnListenEndpoint does.
static void Discovery_NormaliseAddr(struct sockaddr_storage* addr) {
    if (addr->ss_family != AF_INET6) return;
    struct sockaddr_in6* a = (struct sockaddr_in6*)addr;
    if (!IN6_IS_ADDR_V4MAPPED(&a->sin6_addr)) return;

    struct in_addr v4;
    memcpy(&v4, ((const uint8_t*)&a->sin6_addr) + 12, sizeof(v4));
    uint16_t port = a->sin6_port;

    memset(addr, 0, sizeof(*addr));
    struct sockaddr_in* o = (struct sockaddr_in*)addr;
    o->sin_family = AF_INET;
    o->sin_addr = v4;
    o->sin_port = port;
}

// Returns non-zero if addr is one of our own listen endpoints. Caller holds disc->lock.
static int Discovery_IsSelfUnlocked(node_discovery_t* disc, const struct sockaddr_storage* addr) {
    size_t n = DynArr_size(disc->selfEndpoints);
    for (size_t i = 0; i < n; ++i) {
        const struct sockaddr_storage* self = (const struct sockaddr_storage*)DynArr_at(disc->selfEndpoints, i);
        if (Discovery_AddrEqual(self, addr)) return 1;
    }
    return 0;
}

// Adds addr to the self set if not already there. Caller holds disc->lock.
static void Discovery_AddSelfUnlocked(node_discovery_t* disc, const struct sockaddr_storage* addr) {
    if (Discovery_IsSelfUnlocked(disc, addr)) return;
    DynArr_push_back(disc->selfEndpoints, (void*)addr);
}

// Seeds the self set with (local interface address, our listen port) for every address this host
// carries. A multi-homed host - the normal case under IPv6, where a machine holds a global, a
// temporary privacy and a link-local address at once - is otherwise unable to tell its own
// endpoints from a peer's when they come back around through peer exchange.
static void Discovery_SeedSelfEndpoints(node_discovery_t* disc) {
    struct ifaddrs* ifa = NULL;
    if (getifaddrs(&ifa) != 0 || !ifa) return;

    for (struct ifaddrs* it = ifa; it; it = it->ifa_next) {
        if (!it->ifa_addr) continue;

        struct sockaddr_storage ep;
        memset(&ep, 0, sizeof(ep));
        if (it->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in* o = (struct sockaddr_in*)&ep;
            memcpy(o, it->ifa_addr, sizeof(struct sockaddr_in));
            o->sin_port = htons(listenPort);
        } else if (it->ifa_addr->sa_family == AF_INET6) {
            struct sockaddr_in6* o = (struct sockaddr_in6*)&ep;
            memcpy(o, it->ifa_addr, sizeof(struct sockaddr_in6));
            o->sin6_port = htons(listenPort);
            o->sin6_scope_id = 0; // endpoints on the wire are scopeless; compare them the same way
        } else {
            continue;
        }

        Discovery_NormaliseAddr(&ep);
        Discovery_AddSelfUnlocked(disc, &ep);
    }

    freeifaddrs(ifa);
}

static discovered_peer_t* Discovery_FindPtr(node_discovery_t* disc, const struct sockaddr_storage* addr) {
    size_t n = DynArr_size(disc->peers);
    for (size_t i = 0; i < n; ++i) {
        discovered_peer_t* p = (discovered_peer_t*)DynArr_at(disc->peers, i);
        if (Discovery_AddrEqual(&p->addr, addr)) return p;
    }
    return NULL;
}

// Insert addr if not already present. Returns a pointer to the (existing or new) entry, or NULL if
// the address is unusable, is one of our own, or the table is full. Note: the returned pointer is
// invalidated by any later push_back.
static discovered_peer_t* Discovery_Upsert(node_discovery_t* disc, const struct sockaddr_storage* addr, uint32_t hop) {
    if (!Discovery_IsUsableAddr(addr)) return NULL;
    if (Discovery_IsSelfUnlocked(disc, addr)) return NULL; // never track, ping or dial ourselves

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
    np.nodeId = 0;
    np.hop = hop;
    np.state = DISCOVERY_STATE_NEW;
    DynArr_push_back(disc->peers, &np);
    return (discovered_peer_t*)DynArr_at(disc->peers, DynArr_size(disc->peers) - 1);
}

// Returns non-zero if addr may be dialed again, i.e. we have not tried it within the retry window.
// Caller holds disc->lock.
static int Discovery_ConnectCooledDown(node_discovery_t* disc, const struct sockaddr_storage* addr, uint64_t now) {
    size_t n = DynArr_size(disc->connectAttempts);
    for (size_t i = 0; i < n; ++i) {
        const discovery_attempt_t* a = (const discovery_attempt_t*)DynArr_at(disc->connectAttempts, i);
        if (Discovery_AddrEqual(&a->addr, addr)) {
            return (now - a->lastMs) >= DISCOVERY_CONNECT_RETRY_MS;
        }
    }
    return 1; // never dialed
}

// Stamps a dial attempt against addr, evicting the stalest record once the table is full.
// Caller holds disc->lock.
static void Discovery_NoteConnectAttempt(node_discovery_t* disc, const struct sockaddr_storage* addr, uint64_t now) {
    size_t n = DynArr_size(disc->connectAttempts);
    size_t oldestIdx = 0;
    uint64_t oldestMs = UINT64_MAX;

    for (size_t i = 0; i < n; ++i) {
        discovery_attempt_t* a = (discovery_attempt_t*)DynArr_at(disc->connectAttempts, i);
        if (Discovery_AddrEqual(&a->addr, addr)) {
            a->lastMs = now;
            return;
        }
        if (a->lastMs < oldestMs) {
            oldestMs = a->lastMs;
            oldestIdx = i;
        }
    }

    if (n >= DISCOVERY_MAX_KNOWN_PEERS) {
        discovery_attempt_t* victim = (discovery_attempt_t*)DynArr_at(disc->connectAttempts, oldestIdx);
        victim->addr = *addr;
        victim->lastMs = now;
        return;
    }

    discovery_attempt_t na;
    memset(&na, 0, sizeof(na));
    na.addr = *addr;
    na.lastMs = now;
    DynArr_push_back(disc->connectAttempts, &na);
}

// Drops the entry for addr, if any. Caller holds disc->lock.
static void Discovery_RemoveUnlocked(node_discovery_t* disc, const struct sockaddr_storage* addr) {
    size_t n = DynArr_size(disc->peers);
    for (size_t i = 0; i < n; ++i) {
        discovered_peer_t* p = (discovered_peer_t*)DynArr_at(disc->peers, i);
        if (Discovery_AddrEqual(&p->addr, addr)) {
            DynArr_remove(disc->peers, i);
            return;
        }
    }
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
        Discovery_NormaliseAddr(out); // a v4-mapped sender must not become a second entry
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
    disc->selfEndpoints = DYNARR_CREATE(struct sockaddr_storage, 8);
    if (!disc->selfEndpoints) {
        DynArr_destroy(disc->peers);
        free(disc);
        return NULL;
    }
    disc->connectAttempts = DYNARR_CREATE(discovery_attempt_t, 16);
    if (!disc->connectAttempts) {
        DynArr_destroy(disc->selfEndpoints);
        DynArr_destroy(disc->peers);
        free(disc);
        return NULL;
    }
    pthread_mutex_init(&disc->lock, NULL);

    // Nothing else is running yet, so the self set can be seeded without taking the lock.
    Discovery_SeedSelfEndpoints(disc);
    return disc;
}

void NodeDiscovery_Destroy(node_discovery_t* disc) {
    if (!disc) return;
    if (disc->peers) DynArr_destroy(disc->peers);
    if (disc->selfEndpoints) DynArr_destroy(disc->selfEndpoints);
    if (disc->connectAttempts) DynArr_destroy(disc->connectAttempts);
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

    // Snapshot our current peers' listen endpoints (inbound + outbound) and their identities.
    struct sockaddr_storage all[MAX_CONS * 2];
    uint64_t allIds[MAX_CONS * 2];
    size_t total = Node_GetPeerEndpoints(disc->node, all, allIds, sizeof(all) / sizeof(all[0]));

    struct sockaddr_storage reqEndpoint;
    int haveReq = Node_ConnListenEndpoint(fromConn, &reqEndpoint);
    uint64_t reqNodeId = Node_ConnPeerNodeId(fromConn);

    // Build the response payload: [uint16 count][entries...], capped and sampled for spread.
    unsigned char payload[sizeof(uint16_t) + DISCOVERY_PEERS_RESPONSE_CAP * DISCOVERY_WIRE_ENTRY_SIZE];
    size_t offset = sizeof(uint16_t);
    uint16_t count = 0;

    size_t startIdx = total ? (size_t)(random_four_byte() % total) : 0;
    for (size_t k = 0; k < total && count < DISCOVERY_PEERS_RESPONSE_CAP; ++k) {
        size_t idx = (startIdx + k) % total;
        // Don't tell them about themselves. Matching on identity as well as on the endpoint they
        // reached us from matters: a multi-homed peer is known to us under several addresses, and
        // handing one of its own back to it is what makes it discover, ping and dial itself.
        if (haveReq && Discovery_AddrEqual(&all[idx], &reqEndpoint)) continue;
        if (reqNodeId != 0 && allIds[idx] == reqNodeId) continue;
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

void NodeDiscovery_RemovePeer(node_discovery_t* disc, const struct sockaddr_storage* endpoint) {
    if (!disc || !endpoint) return;
    pthread_mutex_lock(&disc->lock);
    size_t n = DynArr_size(disc->peers);
    for (size_t i = 0; i < n; ++i) {
        discovered_peer_t* p = (discovered_peer_t*)DynArr_at(disc->peers, i);
        if (Discovery_AddrEqual(&p->addr, endpoint)) {
            char ip[INET6_ADDRSTRLEN] = {0};
            unsigned short port = 0;
            Discovery_AddrToIpPort(&p->addr, ip, sizeof(ip), &port);
            printf("NodeDiscovery: struck disconnected peer %s:%u from peer list\n", ip, port);
            DynArr_remove(disc->peers, i);
            break;
        }
    }
    pthread_mutex_unlock(&disc->lock);
}

void NodeDiscovery_NoteIdentity(node_discovery_t* disc, const struct sockaddr_storage* endpoint, uint64_t nodeId) {
    if (!disc || !endpoint || nodeId == 0) return;

    pthread_mutex_lock(&disc->lock);
    if (nodeId == localNodeId) {
        // The peer on the other end is us under one of our own addresses. Record it and drop it so
        // discovery stops treating it as a peer.
        Discovery_AddSelfUnlocked(disc, endpoint);
        Discovery_RemoveUnlocked(disc, endpoint);
    } else {
        // Learn the endpoint if we did not already know it - a peer that dialled us is a perfectly
        // good discovery candidate, and we now know both its listen endpoint and its identity.
        discovered_peer_t* p = Discovery_Upsert(disc, endpoint, 0);
        if (p) p->nodeId = nodeId;
    }
    pthread_mutex_unlock(&disc->lock);
}

void NodeDiscovery_MarkSelfEndpoint(node_discovery_t* disc, const struct sockaddr_storage* endpoint) {
    if (!disc || !endpoint) return;
    pthread_mutex_lock(&disc->lock);
    Discovery_AddSelfUnlocked(disc, endpoint);
    Discovery_RemoveUnlocked(disc, endpoint);
    pthread_mutex_unlock(&disc->lock);
}

int NodeDiscovery_IsSelfEndpoint(node_discovery_t* disc, const struct sockaddr_storage* endpoint) {
    if (!disc || !endpoint) return 0;
    pthread_mutex_lock(&disc->lock);
    int isSelf = Discovery_IsSelfUnlocked(disc, endpoint);
    pthread_mutex_unlock(&disc->lock);
    return isSelf;
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
    uint64_t outNodeIds[MAX_CONS];
    size_t outEpCount = 0;
    for (size_t i = 0; i < outCount; ++i) {
        struct sockaddr_storage ep;
        if (Node_ConnListenEndpoint(outConns[i], &ep)) {
            outNodeIds[outEpCount] = Node_ConnPeerNodeId(outConns[i]);
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
            if (outNodeIds[i] != 0) p->nodeId = outNodeIds[i];
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
                if (!Discovery_ConnectCooledDown(disc, &p->addr, now)) continue;
                int already = 0;
                for (size_t j = 0; j < outEpCount; ++j) {
                    if (Discovery_AddrEqual(&p->addr, &outEndpoints[j])) { already = 1; break; }
                }
                // Skip other addresses of a node we already have an outbound connection to. Only
                // outbound counts: an inbound connection from a peer is its own dial, and we still
                // want one of our own to it (broadcasts only travel outbound).
                if (!already && p->nodeId != 0) {
                    for (size_t j = 0; j < outEpCount; ++j) {
                        if (outNodeIds[j] == p->nodeId) { already = 1; break; }
                    }
                }
                if (already) continue;
                if (!best || p->pingMs < best->pingMs) best = p;
            }
            if (!best) break;

            Discovery_NoteConnectAttempt(disc, &best->addr, now); // reserve so it isn't picked again this tick
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
        char idStr[19];
        if (p->nodeId != 0) {
            snprintf(idStr, sizeof(idStr), "%016" PRIx64, p->nodeId);
        } else {
            snprintf(idStr, sizeof(idStr), "%-16s", "?");
        }
        if (p->pingMs == UINT64_MAX) {
            printf("  %-46s hop=%u state=%-11s id=%s ping=--\n", ip, p->hop, stateStr, idStr);
        } else {
            printf("  %-46s hop=%u state=%-11s id=%s ping=%" PRIu64 "ms\n", ip, p->hop, stateStr, idStr, p->pingMs);
        }
        (void)port; // port is part of ip endpoint identity; shown via connect logs
    }

    size_t selfCount = DynArr_size(disc->selfEndpoints);
    printf("Own endpoints (%zu):\n", selfCount);
    for (size_t i = 0; i < selfCount; ++i) {
        const struct sockaddr_storage* self = (const struct sockaddr_storage*)DynArr_at(disc->selfEndpoints, i);
        char ip[INET6_ADDRSTRLEN] = {0};
        unsigned short port = 0;
        Discovery_AddrToIpPort(self, ip, sizeof(ip), &port);
        printf("  %-46s port=%u\n", ip, port);
    }
    pthread_mutex_unlock(&disc->lock);
}
