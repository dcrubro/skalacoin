#include <udpd/udpnode.h>
#include <udpd/udppackettype.h>
#include <utils.h>
#include <numgen.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <errno.h>

typedef struct {
    udp_node_t* node;
    int sockFd;
} udprecv_thread_args_t;

// Send a raw PING packet (nonce already chosen) to dest.
static void UdpNode_SendRawPing(udp_node_t* node, uint64_t nonce, const struct sockaddr_storage* dest) {
    unsigned char buf[UDP_PING_WIRE_SIZE];
    buf[0] = (unsigned char)UDP_PACKET_TYPE_PING;
    memcpy(buf + 1, &nonce, sizeof(nonce));

    int sock = -1;
    socklen_t addrLen = 0;

    if (dest->ss_family == AF_INET6 && node->sockFd >= 0) {
        sock = node->sockFd;
        addrLen = sizeof(struct sockaddr_in6);
    } else if (dest->ss_family == AF_INET && node->sockFdV4 >= 0) {
        sock = node->sockFdV4;
        addrLen = sizeof(struct sockaddr_in);
    }

    if (sock < 0) {
        return;
    }

    sendto(sock, buf, sizeof(buf), 0, (const struct sockaddr*)dest, addrLen);
}

static void UdpNode_HandlePacket(udp_node_t* node, int fromSock,
                                  const unsigned char* buf, ssize_t n,
                                  const struct sockaddr_storage* from) {
    if (n < 1) {
        return;
    }

    udp_packet_type_t type = (udp_packet_type_t)buf[0];

    switch (type) {
        case UDP_PACKET_TYPE_PING: {
            if (n < UDP_PING_WIRE_SIZE) {
                return;
            }
            uint64_t nonce;
            memcpy(&nonce, buf + 1, sizeof(nonce));

            // Build and send PONG
            unsigned char reply[UDP_PONG_WIRE_SIZE];
            reply[0] = (unsigned char)UDP_PACKET_TYPE_PONG;
            memcpy(reply + 1, &nonce, sizeof(nonce));
            int32_t protoVer = (int32_t)PROTO_VERSION;
            memcpy(reply + 1 + sizeof(nonce), &protoVer, sizeof(protoVer));

            socklen_t addrLen = (from->ss_family == AF_INET6)
                ? sizeof(struct sockaddr_in6)
                : sizeof(struct sockaddr_in);
            sendto(fromSock, reply, sizeof(reply), 0, (const struct sockaddr*)from, addrLen);
            break;
        }

        case UDP_PACKET_TYPE_PONG: {
            if (n < UDP_PONG_WIRE_SIZE) {
                return;
            }
            uint64_t nonce;
            int32_t protoVer;
            memcpy(&nonce, buf + 1, sizeof(nonce));
            memcpy(&protoVer, buf + 1 + sizeof(nonce), sizeof(protoVer));

            bool found = false;
            uint64_t rttMs = 0;
            pthread_mutex_lock(&node->pingsMutex);
            for (int i = 0; i < UDP_MAX_PENDING_PINGS; i++) {
                if (node->pendingPings[i].active && node->pendingPings[i].nonce == nonce) {
                    uint64_t nowMs = get_current_time_ms();
                    rttMs = (nowMs >= node->pendingPings[i].lastSentMs)
                        ? (nowMs - node->pendingPings[i].lastSentMs)
                        : 0;
                    node->pendingPings[i].active = false;
                    found = true;
                    break;
                }
            }
            pthread_mutex_unlock(&node->pingsMutex);

            if (found && node->on_pong) {
                node->on_pong(node, from, nonce, (int)protoVer, rttMs, node->callbackUser);
            }
            break;
        }

        default:
            break;
    }
}

static void* UdpNode_RecvThreadProc(void* arg) {
    udprecv_thread_args_t* args = (udprecv_thread_args_t*)arg;
    udp_node_t* node = args->node;
    int sock = args->sockFd;
    free(args);

    unsigned char buf[1500];

    while (node->isRunning) {
        struct sockaddr_storage from;
        socklen_t fromLen = sizeof(from);
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0,
                             (struct sockaddr*)&from, &fromLen);
        if (n < 1) {
            if (!node->isRunning) {
                break;
            }
            // Transient error — keep going
            continue;
        }
        UdpNode_HandlePacket(node, sock, buf, n, &from);
    }

    return NULL;
}

static void* UdpNode_RetryThreadProc(void* arg) {
    udp_node_t* node = (udp_node_t*)arg;

    struct {
        uint64_t nonce;
        struct sockaddr_storage dest;
    } timedOut[UDP_MAX_PENDING_PINGS];

    while (node->isRunning) {
        sleep_for_milliseconds(100);

        int timedOutCount = 0;

        pthread_mutex_lock(&node->pingsMutex);
        uint64_t now = get_current_time_ms();
        for (int i = 0; i < UDP_MAX_PENDING_PINGS; i++) {
            pending_ping_t* p = &node->pendingPings[i];
            if (!p->active) {
                continue;
            }
            if (now - p->lastSentMs < UDP_PING_RETRY_INTERVAL_MS) {
                continue;
            }
            if (p->retries >= UDP_PING_MAX_RETRIES) {
                timedOut[timedOutCount].nonce = p->nonce;
                timedOut[timedOutCount].dest  = p->dest;
                timedOutCount++;
                p->active = false;
            } else {
                UdpNode_SendRawPing(node, p->nonce, &p->dest);
                p->retries++;
                p->lastSentMs = now;
            }
        }
        pthread_mutex_unlock(&node->pingsMutex);

        for (int i = 0; i < timedOutCount; i++) {
            if (node->on_ping_timeout) {
                node->on_ping_timeout(node, &timedOut[i].dest,
                                      timedOut[i].nonce, node->callbackUser);
            }
        }
    }

    return NULL;
}

// Same borrowed/escaped-fd false positive as TcpServer_Init: both sockets are
// handed to the caller through node->sockFd / node->sockFdV4 and closed by
// UdpNode_Stop. -fanalyzer loses the first store across the second socket's
// branches and reports it as a leak.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-fd-leak"
#endif
int UdpNode_Init(udp_node_t* node, uint16_t port) {
    if (!node) {
        return -1;
    }

    memset(node, 0, sizeof(*node));
    node->sockFd = -1;
    node->sockFdV4 = -1;

    int opt = 1;

    // IPv6 (pure, not dual-stack)
    int fd6 = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd6 >= 0) {
        setsockopt(fd6, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        int v6only = 1;
        setsockopt(fd6, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));

        struct sockaddr_in6 a6;
        memset(&a6, 0, sizeof(a6));
        a6.sin6_family = AF_INET6;
        a6.sin6_port = htons(port);
        a6.sin6_addr = in6addr_any;

        if (bind(fd6, (struct sockaddr*)&a6, sizeof(a6)) == 0) {
            node->sockFd = fd6;
        } else {
            close(fd6);
        }
    }

    // IPv4
    int fd4 = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd4 >= 0) {
        setsockopt(fd4, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in a4;
        memset(&a4, 0, sizeof(a4));
        a4.sin_family = AF_INET;
        a4.sin_port = htons(port);
        a4.sin_addr.s_addr = INADDR_ANY;

        if (bind(fd4, (struct sockaddr*)&a4, sizeof(a4)) == 0) {
            node->sockFdV4 = fd4;
        } else {
            close(fd4);
        }
    }

    if (node->sockFd < 0 && node->sockFdV4 < 0) {
        return -1;
    }

    pthread_mutex_init(&node->pingsMutex, NULL);
    return 0;
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

void UdpNode_SetCallbacks(udp_node_t* node,
        void (*on_pong)(udp_node_t*, const struct sockaddr_storage*, uint64_t, int, uint64_t, void*),
        void (*on_ping_timeout)(udp_node_t*, const struct sockaddr_storage*, uint64_t, void*),
        void* user) {
    if (!node) {
        return;
    }
    node->on_pong = on_pong;
    node->on_ping_timeout = on_ping_timeout;
    node->callbackUser = user;
}

int UdpNode_Start(udp_node_t* node) {
    if (!node || node->isRunning) {
        return -1;
    }
    if (node->sockFd < 0 && node->sockFdV4 < 0) {
        return -1;
    }

    node->isRunning = 1;
    int anyStarted  = 0;

    if (node->sockFd >= 0) {
        udprecv_thread_args_t* args = (udprecv_thread_args_t*)malloc(sizeof(*args));
        if (args) {
            args->node = node;
            args->sockFd = node->sockFd;
            if (pthread_create(&node->recvThreadV6, NULL, UdpNode_RecvThreadProc, args) == 0) {
                anyStarted = 1;
            } else {
                free(args);
            }
        }
    }

    if (node->sockFdV4 >= 0) {
        udprecv_thread_args_t* args = (udprecv_thread_args_t*)malloc(sizeof(*args));
        if (args) {
            args->node = node;
            args->sockFd = node->sockFdV4;
            if (pthread_create(&node->recvThreadV4, NULL, UdpNode_RecvThreadProc, args) == 0) {
                anyStarted = 1;
            } else {
                free(args);
            }
        }
    }

    if (pthread_create(&node->retryThread, NULL, UdpNode_RetryThreadProc, node) == 0) {
        anyStarted = 1;
    }

    if (!anyStarted) {
        node->isRunning = 0;
        return -1;
    }

    return 0;
}

void UdpNode_Stop(udp_node_t* node) {
    if (!node || !node->isRunning) {
        return;
    }

    node->isRunning = 0;

    // Close sockets to unblock recvfrom in receive threads
    if (node->sockFd >= 0) {
        int fd = node->sockFd;
        node->sockFd = -1;
        close(fd);
    }
    if (node->sockFdV4 >= 0) {
        int fd = node->sockFdV4;
        node->sockFdV4 = -1;
        close(fd);
    }

    pthread_join(node->recvThreadV6, NULL);
    pthread_join(node->recvThreadV4, NULL);
    pthread_join(node->retryThread, NULL);
}

void UdpNode_Destroy(udp_node_t* node) {
    if (!node) {
        return;
    }

    if (node->sockFd >= 0) {
        close(node->sockFd);
        node->sockFd = -1;
    }
    if (node->sockFdV4 >= 0) {
        close(node->sockFdV4);
        node->sockFdV4 = -1;
    }

    pthread_mutex_destroy(&node->pingsMutex);
}

int UdpNode_SendPing(udp_node_t* node, const struct sockaddr_storage* dest) {
    if (!node || !dest) {
        return -1;
    }
    if (node->sockFd < 0 && node->sockFdV4 < 0) {
        return -1;
    }

    uint64_t nonce = random_eight_byte();
    uint64_t now = get_current_time_ms();

    pthread_mutex_lock(&node->pingsMutex);

    int slot = -1;
    for (int i = 0; i < UDP_MAX_PENDING_PINGS; i++) {
        if (!node->pendingPings[i].active) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        pthread_mutex_unlock(&node->pingsMutex);
        return -1;
    }

    node->pendingPings[slot].nonce = nonce;
    node->pendingPings[slot].dest = *dest;
    node->pendingPings[slot].lastSentMs = now;
    node->pendingPings[slot].retries = 0;
    node->pendingPings[slot].active = true;

    pthread_mutex_unlock(&node->pingsMutex);

    UdpNode_SendRawPing(node, nonce, dest);
    return 0;
}
