#ifndef _WIN32

#include <tcpd/tcpclient.h>

#include <errno.h>
#include <netinet/in.h>
#include <numgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>

static void* TcpClient_ThreadProc(void* arg) {
    tcp_client_t* client = (tcp_client_t*)arg;
    if (!client || !client->connection) {
        return NULL;
    }

    tcp_connection_t* conn = client->connection;
    unsigned char ioBuf[TCP_IO_BUFFER_SIZE];

    while (1) {
        ssize_t n = recv(conn->sockFd, ioBuf, sizeof(ioBuf), 0);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (TcpConnection_FeedFramedData(conn, ioBuf, (size_t)n) != 0) {
            break;
        }
    }

    if (!TcpConnection_IsDisconnectNotified(conn) && conn->on_disconnect) {
        TcpConnection_MarkDisconnectNotified(conn);
        conn->on_disconnect(conn);
    }

    return NULL;
}

int TcpClient_Init(tcp_client_t* client) {
    if (!client) {
        return -1;
    }

    memset(client, 0, sizeof(*client));
    client->connection = NULL;
    client->peerBlockHeight = 0;
    return 0;
}

void TcpClient_Destroy(tcp_client_t* client) {
    if (!client) {
        return;
    }

    TcpClient_Disconnect(client);
}

int TcpClient_Connect(
    tcp_client_t* client,
    const char* peerIp,
    unsigned short peerPort,
    void (*on_connect)(tcp_connection_t* conn),
    void (*on_data)(tcp_connection_t* conn),
    void (*on_disconnect)(tcp_connection_t* conn),
    void* owner
) {
    if (!client || !peerIp) {
        return -1;
    }

    if (client->connection) {
        return -1;
    }

    // Detect address family from the IP string
    struct sockaddr_in6 addr6;
    struct sockaddr_in addr4;
    struct sockaddr* pSockAddr;
    socklen_t sockAddrLen;
    int af;

    memset(&addr6, 0, sizeof(addr6));
    memset(&addr4, 0, sizeof(addr4));

    if (inet_pton(AF_INET6, peerIp, &addr6.sin6_addr) == 1) {
        af = AF_INET6;
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = htons(peerPort);
        pSockAddr = (struct sockaddr*)&addr6;
        sockAddrLen = sizeof(addr6);
    } else if (inet_pton(AF_INET, peerIp, &addr4.sin_addr) == 1) {
        af = AF_INET;
        addr4.sin_family = AF_INET;
        addr4.sin_port = htons(peerPort);
        pSockAddr = (struct sockaddr*)&addr4;
        sockAddrLen = sizeof(addr4);
    } else {
        return -1;
    }

    int sockFd = socket(af, SOCK_STREAM, 0);
    if (sockFd < 0) {
        return -1;
    }

    // Use non-blocking connect with a timeout to avoid long blocking in the CLI.
    int flags = fcntl(sockFd, F_GETFL, 0);
    if (flags == -1) flags = 0;
    fcntl(sockFd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(sockFd, pSockAddr, sockAddrLen);
    if (rc < 0) {
        if (errno != EINPROGRESS) {
            close(sockFd);
            return -1;
        }

        // Wait up to 5 seconds for the socket to become writable (connected)
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sockFd, &wfds);
        int sel = select(sockFd + 1, NULL, &wfds, NULL, &tv);
        if (sel <= 0) {
            // timeout or error
            if (sel == 0) {
                errno = ETIMEDOUT;
            }
            close(sockFd);
            return -1;
        }

        // Check for socket error
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        if (getsockopt(sockFd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0) {
            close(sockFd);
            return -1;
        }
        if (so_error != 0) {
            errno = so_error;
            close(sockFd);
            return -1;
        }
    }

    // Restore blocking mode
    fcntl(sockFd, F_SETFL, flags & ~O_NONBLOCK);

    // Pack the address into sockaddr_storage for TcpConnection_Init
    struct sockaddr_storage peerStorage;
    memset(&peerStorage, 0, sizeof(peerStorage));
    memcpy(&peerStorage, pSockAddr, sockAddrLen);

    tcp_connection_t* conn = (tcp_connection_t*)malloc(sizeof(*conn));
    if (!conn) {
        close(sockFd);
        return -1;
    }

    if (TcpConnection_Init(conn, sockFd, &peerStorage, TCP_CONNECTION_ROLE_OUTBOUND) != 0) {
        free(conn);
        close(sockFd);
        return -1;
    }

    conn->connectionId = random_four_byte();
    conn->on_data = on_data;
    conn->on_disconnect = on_disconnect;
    conn->owner = owner;

    client->connection = conn;
    client->on_connect = on_connect;
    client->on_data = on_data;
    client->on_disconnect = on_disconnect;
    client->owner = owner;

    if (client->on_connect) {
        client->on_connect(conn);
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, TCP_THREAD_STACK_SIZE);

    if (pthread_create(&conn->ioThread, &attr, TcpClient_ThreadProc, client) != 0) {
        TcpConnection_Destroy(conn);
        free(conn);
        client->connection = NULL;
        pthread_attr_destroy(&attr);
        return -1;
    }
    pthread_attr_destroy(&attr);

    return 0;
}

int TcpClient_Send(tcp_client_t* client, const void* data, size_t len) {
    if (!client || !client->connection) {
        return -1;
    }

    return TcpConnection_SendFramed(client->connection, data, len);
}

void TcpClient_Disconnect(tcp_client_t* client) {
    if (!client || !client->connection) {
        return;
    }

    tcp_connection_t* conn = client->connection;

    TcpConnection_RequestClose(conn);

    if (!pthread_equal(conn->ioThread, pthread_self())) {
        pthread_join(conn->ioThread, NULL);
    }

    if (!TcpConnection_IsDisconnectNotified(conn) && conn->on_disconnect) {
        TcpConnection_MarkDisconnectNotified(conn);
        conn->on_disconnect(conn);
    }

    TcpConnection_Destroy(conn);
    free(conn);

    client->connection = NULL;
}

#endif
