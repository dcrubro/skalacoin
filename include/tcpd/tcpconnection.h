#ifndef TCPCONNECTION_H
#define TCPCONNECTION_H

#include <arpa/inet.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#define TCP_IO_BUFFER_SIZE 1500
#define TCP_FRAME_HEADER_SIZE 4U
#define TCP_MAX_FRAME_PAYLOAD (1024U * 1024U)

typedef enum {
    TCP_CONNECTION_ROLE_INBOUND = 0,
    TCP_CONNECTION_ROLE_OUTBOUND = 1
} tcp_connection_role_t;

typedef struct tcp_connection_t tcp_connection_t;

struct tcp_connection_t {
    int sockFd;
    sa_family_t addrFamily;
    struct sockaddr_storage peerAddr;
    uint32_t connectionId;
    tcp_connection_role_t role;

    // Peer's advertised TCP/UDP listen port (learned from HELLO/ACK_HELLO). 0 until known.
    // For OUTBOUND connections the peerAddr port already is the listen port; this matters for INBOUND peers.
    uint16_t peerListenPort;

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

int TcpConnection_Init(tcp_connection_t* conn, int sockFd, const struct sockaddr_storage* peerAddr, tcp_connection_role_t role);
void TcpConnection_Destroy(tcp_connection_t* conn);

int TcpConnection_SetDataBuffer(tcp_connection_t* conn, const unsigned char* data, size_t len);

void TcpConnection_ResetFramingState(tcp_connection_t* conn);
int TcpConnection_FeedFramedData(tcp_connection_t* conn, const unsigned char* input, size_t inputLen);

// Returns the peer's canonical IP string (strips ::ffff: IPv4-mapped prefix).
// Writes at most bufLen bytes to buf. Returns buf on success, NULL on failure.
const char* TcpConnection_GetPeerAddrStr(const tcp_connection_t* conn, char* buf, size_t bufLen);

// Returns non-zero if both connections have the same peer IP address.
// Handles AF_INET vs AF_INET6 mismatches via IPv4-mapped normalisation.
int TcpConnection_PeerAddrEqual(const tcp_connection_t* a, const tcp_connection_t* b);

int TcpConnection_SendRaw(int sockFd, const void* data, size_t len);
int TcpConnection_SendFramed(tcp_connection_t* conn, const void* payload, size_t payloadLen);

void TcpConnection_RequestClose(tcp_connection_t* conn);
void TcpConnection_MarkDisconnectNotified(tcp_connection_t* conn);
bool TcpConnection_IsDisconnectNotified(tcp_connection_t* conn);

#endif
