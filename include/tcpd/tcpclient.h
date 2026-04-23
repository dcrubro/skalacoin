#ifndef TCPCLIENT_H
#define TCPCLIENT_H

#include <arpa/inet.h>
#include <stddef.h>

#include <tcpd/tcpconnection.h>

typedef struct {
    tcp_connection_t* connection;
    void (*on_connect)(tcp_connection_t* conn);
    void (*on_data)(tcp_connection_t* conn);
    void (*on_disconnect)(tcp_connection_t* conn);
    void* owner;
} tcp_client_t;

int TcpClient_Init(tcp_client_t* client);
void TcpClient_Destroy(tcp_client_t* client);

int TcpClient_Connect(
    tcp_client_t* client,
    const char* peerIp,
    unsigned short peerPort,
    void (*on_connect)(tcp_connection_t* conn),
    void (*on_data)(tcp_connection_t* conn),
    void (*on_disconnect)(tcp_connection_t* conn),
    void* owner
);

int TcpClient_Send(tcp_client_t* client, const void* data, size_t len);
void TcpClient_Disconnect(tcp_client_t* client);

#endif
