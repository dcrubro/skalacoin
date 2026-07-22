#ifndef UDP_NODE_H
#define UDP_NODE_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <netinet/in.h>

#include <udpd/udppackettype.h>

#define UDP_LISTEN_PORT 9393
#define UDP_PING_RETRY_INTERVAL_MS 1000
#define UDP_PING_MAX_RETRIES 3
#define UDP_MAX_PENDING_PINGS 64

typedef struct {
    uint64_t nonce;
    struct sockaddr_storage dest;
    uint64_t lastSentMs;
    int retries;
    bool active;
} pending_ping_t;

typedef struct udp_node {
    int sockFd; // AF_INET6, IPV6_V6ONLY=1
    int sockFdV4; // AF_INET

    volatile int isRunning;

    pthread_t recvThreadV6;
    pthread_t recvThreadV4;
    pthread_t retryThread;

    pending_ping_t pendingPings[UDP_MAX_PENDING_PINGS];
    pthread_mutex_t pingsMutex;

    void (*on_pong)(struct udp_node* node,
                    const struct sockaddr_storage* from,
                    uint64_t nonce, int protoVersion, uint64_t rttMs, void* user);
    void (*on_ping_timeout)(struct udp_node* node,
                            const struct sockaddr_storage* dest,
                            uint64_t nonce, void* user);
    void* callbackUser;
} udp_node_t;

int UdpNode_Init(udp_node_t* node, uint16_t port);
void UdpNode_SetCallbacks(udp_node_t* node,
        void (*on_pong)(udp_node_t*, const struct sockaddr_storage*, uint64_t, int, uint64_t, void*),
        void (*on_ping_timeout)(udp_node_t*, const struct sockaddr_storage*, uint64_t, void*),
        void* user);
int UdpNode_Start(udp_node_t* node);
void UdpNode_Stop(udp_node_t* node);
void UdpNode_Destroy(udp_node_t* node);
int UdpNode_SendPing(udp_node_t* node, const struct sockaddr_storage* dest);

#endif
