#ifndef _WIN32

#include <tcpd/tcpconnection.h>

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int TcpConnection_Init(tcp_connection_t* conn, int sockFd, const struct sockaddr_storage* peerAddr, tcp_connection_role_t role) {
    if (!conn || sockFd < 0 || !peerAddr) {
        return -1;
    }

    memset(conn, 0, sizeof(*conn));
    conn->sockFd = sockFd;
    conn->peerAddr = *peerAddr;
    conn->addrFamily = peerAddr->ss_family;
    conn->role = role;

    if (pthread_mutex_init(&conn->sendLock, NULL) != 0) {
        return -1;
    }

    if (pthread_mutex_init(&conn->stateLock, NULL) != 0) {
        pthread_mutex_destroy(&conn->sendLock);
        return -1;
    }

    conn->closing = false;
    conn->disconnectedNotified = false;
    conn->dataBuf = NULL;
    conn->dataBufLen = 0;
    conn->dataBufCap = 0;

    TcpConnection_ResetFramingState(conn);

    return 0;
}

void TcpConnection_Destroy(tcp_connection_t* conn) {
    if (!conn) {
        return;
    }

    if (conn->sockFd >= 0) {
        close(conn->sockFd);
        conn->sockFd = -1;
    }

    free(conn->dataBuf);
    conn->dataBuf = NULL;
    conn->dataBufLen = 0;
    conn->dataBufCap = 0;

    free(conn->frameBuf);
    conn->frameBuf = NULL;
    conn->frameBytesRead = 0;

    pthread_mutex_destroy(&conn->stateLock);
    pthread_mutex_destroy(&conn->sendLock);
}

int TcpConnection_SetDataBuffer(tcp_connection_t* conn, const unsigned char* data, size_t len) {
    if (!conn || (!data && len > 0)) {
        return -1;
    }

    if (len > conn->dataBufCap) {
        unsigned char* resized = (unsigned char*)realloc(conn->dataBuf, len);
        if (!resized) {
            return -1;
        }

        conn->dataBuf = resized;
        conn->dataBufCap = len;
    }

    if (len > 0) {
        memcpy(conn->dataBuf, data, len);
    }
    conn->dataBufLen = len;

    return 0;
}

void TcpConnection_ResetFramingState(tcp_connection_t* conn) {
    if (!conn) {
        return;
    }

    memset(conn->headerBuf, 0, sizeof(conn->headerBuf));
    conn->headerBytesRead = 0;
    conn->expectedPayloadLen = 0;
    conn->frameBytesRead = 0;

    free(conn->frameBuf);
    conn->frameBuf = NULL;
}

int TcpConnection_FeedFramedData(tcp_connection_t* conn, const unsigned char* input, size_t inputLen) {
    if (!conn || (!input && inputLen > 0)) {
        return -1;
    }

    size_t offset = 0;

    while (offset < inputLen) {
        if (conn->headerBytesRead < TCP_FRAME_HEADER_SIZE) {
            size_t needed = TCP_FRAME_HEADER_SIZE - conn->headerBytesRead;
            size_t take = (inputLen - offset < needed) ? (inputLen - offset) : needed;

            memcpy(conn->headerBuf + conn->headerBytesRead, input + offset, take);
            conn->headerBytesRead += take;
            offset += take;

            if (conn->headerBytesRead < TCP_FRAME_HEADER_SIZE) {
                continue;
            }

            uint32_t beLen = 0;
            memcpy(&beLen, conn->headerBuf, sizeof(beLen));
            conn->expectedPayloadLen = ntohl(beLen);

            if (conn->expectedPayloadLen > TCP_MAX_FRAME_PAYLOAD) {
                TcpConnection_ResetFramingState(conn);
                return -1;
            }

            if (conn->expectedPayloadLen == 0) {
                if (TcpConnection_SetDataBuffer(conn, NULL, 0) != 0) {
                    TcpConnection_ResetFramingState(conn);
                    return -1;
                }

                if (conn->on_data) {
                    conn->on_data(conn);
                }

                conn->headerBytesRead = 0;
                conn->expectedPayloadLen = 0;
                continue;
            }

            conn->frameBuf = (unsigned char*)malloc(conn->expectedPayloadLen);
            if (!conn->frameBuf) {
                TcpConnection_ResetFramingState(conn);
                return -1;
            }
            conn->frameBytesRead = 0;
        }

        size_t frameRemaining = conn->expectedPayloadLen - conn->frameBytesRead;
        size_t take = (inputLen - offset < frameRemaining) ? (inputLen - offset) : frameRemaining;

        memcpy(conn->frameBuf + conn->frameBytesRead, input + offset, take);
        conn->frameBytesRead += take;
        offset += take;

        if (conn->frameBytesRead == conn->expectedPayloadLen) {
            if (TcpConnection_SetDataBuffer(conn, conn->frameBuf, conn->expectedPayloadLen) != 0) {
                TcpConnection_ResetFramingState(conn);
                return -1;
            }

            if (conn->on_data) {
                conn->on_data(conn);
            }

            conn->headerBytesRead = 0;
            conn->expectedPayloadLen = 0;
            conn->frameBytesRead = 0;
            free(conn->frameBuf);
            conn->frameBuf = NULL;
        }
    }

    return 0;
}

int TcpConnection_SendRaw(int sockFd, const void* data, size_t len) {
    if (sockFd < 0 || (!data && len > 0)) {
        return -1;
    }

    size_t totalSent = 0;
    const unsigned char* ptr = (const unsigned char*)data;

    while (totalSent < len) {
        ssize_t sent = send(sockFd, ptr + totalSent, len - totalSent, 0);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (sent == 0) {
            return -1;
        }

        totalSent += (size_t)sent;
    }

    return 0;
}

int TcpConnection_SendFramed(tcp_connection_t* conn, const void* payload, size_t payloadLen) {
    if (!conn || (!payload && payloadLen > 0) || payloadLen > TCP_MAX_FRAME_PAYLOAD) {
        return -1;
    }

    uint32_t beLen = htonl((uint32_t)payloadLen);

    pthread_mutex_lock(&conn->sendLock);

    int rc = TcpConnection_SendRaw(conn->sockFd, &beLen, sizeof(beLen));
    if (rc == 0 && payloadLen > 0) {
        rc = TcpConnection_SendRaw(conn->sockFd, payload, payloadLen);
    }

    pthread_mutex_unlock(&conn->sendLock);

    return rc;
}

void TcpConnection_RequestClose(tcp_connection_t* conn) {
    if (!conn) {
        return;
    }

    pthread_mutex_lock(&conn->stateLock);
    if (!conn->closing) {
        conn->closing = true;
        if (conn->sockFd >= 0) {
            shutdown(conn->sockFd, SHUT_RDWR);
        }
    }
    pthread_mutex_unlock(&conn->stateLock);
}

void TcpConnection_MarkDisconnectNotified(tcp_connection_t* conn) {
    if (!conn) {
        return;
    }

    pthread_mutex_lock(&conn->stateLock);
    conn->disconnectedNotified = true;
    pthread_mutex_unlock(&conn->stateLock);
}

bool TcpConnection_IsDisconnectNotified(tcp_connection_t* conn) {
    if (!conn) {
        return true;
    }

    pthread_mutex_lock(&conn->stateLock);
    bool notified = conn->disconnectedNotified;
    pthread_mutex_unlock(&conn->stateLock);

    return notified;
}

static int extract_v4(const tcp_connection_t* conn, struct in_addr* v4out) {
    if (conn->addrFamily == AF_INET6) {
        const struct sockaddr_in6* a6 = (const struct sockaddr_in6*)&conn->peerAddr;
        if (IN6_IS_ADDR_V4MAPPED(&a6->sin6_addr)) {
            memcpy(v4out, &a6->sin6_addr.s6_addr[12], sizeof(*v4out));
            return 1;
        }
        return 0;
    }
    *v4out = ((const struct sockaddr_in*)&conn->peerAddr)->sin_addr;
    return 1;
}

const char* TcpConnection_GetPeerAddrStr(const tcp_connection_t* conn, char* buf, size_t bufLen) {
    if (!conn || !buf || bufLen == 0) {
        return NULL;
    }

    if (conn->addrFamily == AF_INET6) {
        const struct sockaddr_in6* a6 = (const struct sockaddr_in6*)&conn->peerAddr;
        if (IN6_IS_ADDR_V4MAPPED(&a6->sin6_addr)) {
            struct in_addr v4;
            memcpy(&v4, &a6->sin6_addr.s6_addr[12], sizeof(v4));
            return inet_ntop(AF_INET, &v4, buf, (socklen_t)bufLen);
        }
        return inet_ntop(AF_INET6, &a6->sin6_addr, buf, (socklen_t)bufLen);
    }

    const struct sockaddr_in* a4 = (const struct sockaddr_in*)&conn->peerAddr;
    return inet_ntop(AF_INET, &a4->sin_addr, buf, (socklen_t)bufLen);
}

int TcpConnection_PeerAddrEqual(const tcp_connection_t* a, const tcp_connection_t* b) {
    if (!a || !b) {
        return 0;
    }

    struct in_addr va, vb;
    int a_is_v4 = extract_v4(a, &va);
    int b_is_v4 = extract_v4(b, &vb);

    if (a_is_v4 && b_is_v4) {
        return va.s_addr == vb.s_addr;
    }

    if (!a_is_v4 && !b_is_v4) {
        const struct in6_addr* aa6 = &((const struct sockaddr_in6*)&a->peerAddr)->sin6_addr;
        const struct in6_addr* ab6 = &((const struct sockaddr_in6*)&b->peerAddr)->sin6_addr;
        return memcmp(aa6, ab6, sizeof(*aa6)) == 0;
    }

    return 0;
}

#endif
