#ifndef TCPSERVER_H
#define TCPSERVER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdint.h>

#include <tcpd/tcpconnection.h>
#include <numgen.h>
#include <dynarr.h>

typedef struct {
    int sockFd;
    struct sockaddr_in addr;
    int opt;

    // Called before the client thread runs
    void (*on_connect)(tcp_connection_t* client);
    // Called when data is received
    void (*on_data)(tcp_connection_t* client);
    // Called before the socket and client thread are killed; Do NOT free client manually
    void (*on_disconnect)(tcp_connection_t* client);

    // max clients
    size_t clients;
    tcp_connection_t** clientsArrPtr;

    pthread_t svrThread;
} tcp_server_t;

struct tcpclient_thread_args {
    tcp_connection_t* clientPtr;
    tcp_server_t* serverPtr;
};

typedef struct tcpclient_thread_args tcpclient_thread_args;

tcp_server_t* TcpServer_Create();
void TcpServer_Destroy(tcp_server_t* ptr);

void TcpServer_Init(tcp_server_t* ptr, unsigned short port, const char* addr);
void TcpServer_Start(tcp_server_t* ptr, int maxcons);
void TcpServer_Stop(tcp_server_t* ptr);
void TcpServer_Send(tcp_server_t* ptr, tcp_connection_t* cli, void* data, size_t len);
void Generic_SendSocket(int sock, void* data, size_t len);
void TcpServer_Disconnect(tcp_server_t* ptr, tcp_connection_t* cli);
void TcpServer_KillClient(tcp_server_t* ptr, tcp_connection_t* cli);

size_t Generic_FindClientInArrayByPtr(tcp_connection_t** arr, tcp_connection_t* ptr, size_t len);

#endif
