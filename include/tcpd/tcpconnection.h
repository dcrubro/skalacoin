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

struct tcp_connection_t {
    int clientFd;
    struct sockaddr_in clientAddr;
    uint32_t clientId;

    unsigned char dataBuf[MTU];
    ssize_t dataBufLen;
    void (*on_data)(struct tcp_connection_t* client);
    void (*on_disconnect)(struct tcp_connection_t* client);

    pthread_t clientThread;
};

typedef struct tcp_connection_t tcp_connection_t;

#endif
