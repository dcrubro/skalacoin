#ifndef _WIN32

#include <tcpd/tcpclient.h>

#include <errno.h>
#include <numgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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

    int sockFd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockFd < 0) {
        return -1;
    }

    struct sockaddr_in peerAddr;
    memset(&peerAddr, 0, sizeof(peerAddr));
    peerAddr.sin_family = AF_INET;
    peerAddr.sin_port = htons(peerPort);

    if (inet_pton(AF_INET, peerIp, &peerAddr.sin_addr) <= 0) {
        close(sockFd);
        return -1;
    }

    if (connect(sockFd, (struct sockaddr*)&peerAddr, sizeof(peerAddr)) < 0) {
        close(sockFd);
        return -1;
    }

    tcp_connection_t* conn = (tcp_connection_t*)malloc(sizeof(*conn));
    if (!conn) {
        close(sockFd);
        return -1;
    }

    if (TcpConnection_Init(conn, sockFd, &peerAddr, TCP_CONNECTION_ROLE_OUTBOUND) != 0) {
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

    if (pthread_create(&conn->ioThread, NULL, TcpClient_ThreadProc, client) != 0) {
        TcpConnection_Destroy(conn);
        free(conn);
        client->connection = NULL;
        return -1;
    }

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
