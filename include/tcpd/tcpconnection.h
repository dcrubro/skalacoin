#ifndef TCPCONNECTION_H
#define TCPCONNECTION_H

#include <arpa/inet.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TCP_IO_BUFFER_SIZE 1500
#define TCP_FRAME_HEADER_SIZE 4U
#define TCP_MAX_FRAME_PAYLOAD (1024U * 1024U)

typedef enum {
    TCP_CONNECTION_ROLE_INBOUND = 0,
    TCP_CONNECTION_ROLE_OUTBOUND = 1
} tcp_connection_role_t;

typedef struct tcp_connection_t tcp_connection_t;

struct tcp_connection_t {
    // TODO: We should make it so only ONE of this needs to be available. 
    // Because of my temporary "I just need something that works" horseshit that I'm about to write, you'll need IPv4 and IPv6 is optional.
    // Note to self: Don't pull an IETF and some "NAT exists, we're fine" bullshit, because if we end up with our eqvivalent of Teredo or CGNAT, I'm gonna be fucking pissed.
    // And no, the solution isn't "eh, just bind to 0.0.0.0 and ignore it", because if we do that, we'll inevitably end up with a host that only has IPv6 and then we'll be fucked.
    // Honestly, I'm proud of whoever runs IPv6-only. Brave soul.
    int sockFd;
    struct sockaddr_in peerAddr;
#ifdef USE_IPV6
    int sockFd6; // For IPv6 support
    struct sockaddr_in6 peerAddr6; // For IPv6 support
#endif
    uint32_t connectionId;
    tcp_connection_role_t role;

    pthread_t ioThread;
    pthread_mutex_t sendLock;
    pthread_mutex_t stateLock;

    bool closing;
    bool disconnectedNotified;

    unsigned char* dataBuf;
    size_t dataBufLen;
    size_t dataBufCap;

    unsigned char headerBuf[TCP_FRAME_HEADER_SIZE];
    size_t headerBytesRead;
    uint32_t expectedPayloadLen;
    unsigned char* frameBuf;
    size_t frameBytesRead;

    void (*on_data)(tcp_connection_t* conn);
    void (*on_disconnect)(tcp_connection_t* conn);
    void* owner;
};

int TcpConnection_Init(tcp_connection_t* conn, int sockFd, const struct sockaddr_in* peerAddr, tcp_connection_role_t role);
void TcpConnection_Destroy(tcp_connection_t* conn);

int TcpConnection_SetDataBuffer(tcp_connection_t* conn, const unsigned char* data, size_t len);

void TcpConnection_ResetFramingState(tcp_connection_t* conn);
int TcpConnection_FeedFramedData(tcp_connection_t* conn, const unsigned char* input, size_t inputLen);

// This just takes a socket ID, so it's independent from the v4/v6 stuff. It works for both.
int TcpConnection_SendRaw(int sockFd, const void* data, size_t len);
int TcpConnection_SendFramed(tcp_connection_t* conn, const void* payload, size_t payloadLen);

void TcpConnection_RequestClose(tcp_connection_t* conn);
void TcpConnection_MarkDisconnectNotified(tcp_connection_t* conn);
bool TcpConnection_IsDisconnectNotified(tcp_connection_t* conn);

#endif
