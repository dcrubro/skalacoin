#ifndef TCPCLIENT_H
#define TCPCLIENT_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>
#include <dynarr.h>

#define MTU 1500

struct TcpClient {
    int clientFd;
    struct sockaddr_in clientAddr;
    uint32_t clientId;

    unsigned char dataBuf[MTU];
    ssize_t dataBufLen;
    void (*on_data)(struct TcpClient* client);
    void (*on_disconnect)(struct TcpClient* client);

    pthread_t clientThread;
};

typedef struct TcpClient TcpClient;

#endif
