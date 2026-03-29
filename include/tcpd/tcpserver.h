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

#include <tcpd/tcpclient.h>
#include <numgen.h>
#include <dynarr.h>

typedef struct {
    int sockFd;
    struct sockaddr_in addr;
    int opt;

    // Called before the client thread runs
    void (*on_connect)(TcpClient* client);
    // Called when data is received
    void (*on_data)(TcpClient* client);
    // Called before the socket and client thread are killed; Do NOT free client manually
    void (*on_disconnect)(TcpClient* client);

    // max clients
    size_t clients;
    TcpClient** clientsArrPtr;

    pthread_t svrThread;
} TcpServer;

struct tcpclient_thread_args {
    TcpClient* clientPtr;
    TcpServer* serverPtr;
};

typedef struct tcpclient_thread_args tcpclient_thread_args;

TcpServer* TcpServer_Create();
void TcpServer_Destroy(TcpServer* ptr);

void TcpServer_Init(TcpServer* ptr, unsigned short port, const char* addr);
void TcpServer_Start(TcpServer* ptr, int maxcons);
void TcpServer_Stop(TcpServer* ptr);
void TcpServer_Send(TcpServer* ptr, TcpClient* cli, void* data, size_t len);
void Generic_SendSocket(int sock, void* data, size_t len);
void TcpServer_Disconnect(TcpServer* ptr, TcpClient* cli);
void TcpServer_KillClient(TcpServer* ptr, TcpClient* cli);

size_t Generic_FindClientInArrayByPtr(TcpClient** arr, TcpClient* ptr, size_t len);

#endif
