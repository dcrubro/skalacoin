#ifndef _WIN32

#include <tcpd/tcpconnection.h>

#include <arpa/inet.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int TcpConnection_Init(tcp_connection_t* conn, int sockFd, const struct sockaddr_in* peerAddr, tcp_connection_role_t role) {
    if (!conn || sockFd < 0 || !peerAddr) {
        return -1;
    }

    memset(conn, 0, sizeof(*conn));
    conn->sockFd = sockFd;
    conn->peerAddr = *peerAddr;
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

#endif
