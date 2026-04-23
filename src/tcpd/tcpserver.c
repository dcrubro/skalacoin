#ifndef _WIN32

#include <tcpd/tcpserver.h>

#include <errno.h>
#include <numgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void TcpServer_RemoveClientByPtrUnlocked(tcp_server_t* svr, tcp_connection_t* cli) {
    if (!svr || !svr->clientsArrPtr || !cli) {
        return;
    }

    size_t idx = Generic_FindClientInArrayByPtr(svr->clientsArrPtr, cli, svr->maxClients);
    if (idx != SIZE_MAX) {
        svr->clientsArrPtr[idx] = NULL;
    }
}

static void* TcpServer_clientthreadprocess(void* ptr) {
    tcpclient_thread_args* args = (tcpclient_thread_args*)ptr;
    if (!args || !args->clientPtr || !args->serverPtr) {
        free(args);
        return NULL;
    }

    tcp_connection_t* cli = args->clientPtr;
    tcp_server_t* svr = args->serverPtr;
    free(args);

    unsigned char ioBuf[TCP_IO_BUFFER_SIZE];

    while (1) {
        ssize_t n = recv(cli->sockFd, ioBuf, sizeof(ioBuf), 0);
        if (n == 0) {
            break;
        }

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (TcpConnection_FeedFramedData(cli, ioBuf, (size_t)n) != 0) {
            break;
        }
    }

    TcpConnection_RequestClose(cli);

    if (!TcpConnection_IsDisconnectNotified(cli) && cli->on_disconnect) {
        TcpConnection_MarkDisconnectNotified(cli);
        cli->on_disconnect(cli);
    }

    pthread_mutex_lock(&svr->clientsMutex);
    TcpServer_RemoveClientByPtrUnlocked(svr, cli);
    pthread_mutex_unlock(&svr->clientsMutex);

    TcpConnection_Destroy(cli);
    free(cli);

    return NULL;
}

static void* TcpServer_threadprocess(void* ptr) {
    tcp_server_t* svr = (tcp_server_t*)ptr;
    if (!svr) {
        return NULL;
    }

    while (svr->isRunning) {
        struct sockaddr_in clientAddr;
        socklen_t clientSize = sizeof(clientAddr);
        int clientFd = accept(svr->sockFd, (struct sockaddr*)&clientAddr, &clientSize);

        if (clientFd < 0) {
            if (!svr->isRunning) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            continue;
        }

        tcp_connection_t* heapCli = (tcp_connection_t*)malloc(sizeof(*heapCli));
        if (!heapCli) {
            close(clientFd);
            continue;
        }

        if (TcpConnection_Init(heapCli, clientFd, &clientAddr, TCP_CONNECTION_ROLE_INBOUND) != 0) {
            close(clientFd);
            free(heapCli);
            continue;
        }

        heapCli->connectionId = random_four_byte();
        heapCli->on_data = svr->on_data;
        heapCli->on_disconnect = svr->on_disconnect;
        heapCli->owner = svr->owner;

        pthread_mutex_lock(&svr->clientsMutex);

        size_t insertIdx = SIZE_MAX;
        for (size_t i = 0; i < svr->maxClients; ++i) {
            if (svr->clientsArrPtr[i] == NULL) {
                insertIdx = i;
                break;
            }
        }

        if (insertIdx == SIZE_MAX) {
            pthread_mutex_unlock(&svr->clientsMutex);
            struct linger so_linger;
            so_linger.l_onoff = 1;
            so_linger.l_linger = 0;
            setsockopt(heapCli->sockFd, SOL_SOCKET, SO_LINGER, &so_linger, sizeof(so_linger));
            TcpConnection_Destroy(heapCli);
            free(heapCli);
            continue;
        }

        svr->clientsArrPtr[insertIdx] = heapCli;
        pthread_mutex_unlock(&svr->clientsMutex);

        if (svr->on_connect) {
            svr->on_connect(heapCli);
        }

        tcpclient_thread_args* arg = (tcpclient_thread_args*)malloc(sizeof(*arg));
        if (!arg) {
            TcpServer_Disconnect(svr, heapCli);
            continue;
        }

        arg->clientPtr = heapCli;
        arg->serverPtr = svr;

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, TCP_THREAD_STACK_SIZE);

        if (pthread_create(&heapCli->ioThread, &attr, TcpServer_clientthreadprocess, arg) != 0) {
            free(arg);
            TcpServer_Disconnect(svr, heapCli);
            pthread_attr_destroy(&attr);
            continue;
        }
        pthread_attr_destroy(&attr);
    }

    return NULL;
}

tcp_server_t* TcpServer_Create() {
    tcp_server_t* svr = (tcp_server_t*)malloc(sizeof(*svr));
    if (!svr) {
        return NULL;
    }

    memset(svr, 0, sizeof(*svr));
    svr->sockFd = -1;
    svr->svrThread = 0;
    svr->isRunning = 0;
    svr->maxClients = 0;
    svr->clientsArrPtr = NULL;

    if (pthread_mutex_init(&svr->clientsMutex, NULL) != 0) {
        free(svr);
        return NULL;
    }

    return svr;
}

void TcpServer_Destroy(tcp_server_t* ptr) {
    if (!ptr) {
        return;
    }

    TcpServer_Stop(ptr);

    free(ptr->clientsArrPtr);
    ptr->clientsArrPtr = NULL;

    pthread_mutex_destroy(&ptr->clientsMutex);
    free(ptr);
}

void TcpServer_Init(tcp_server_t* ptr, unsigned short port, const char* addr) {
    if (!ptr || !addr) {
        return;
    }

    ptr->sockFd = socket(AF_INET, SOCK_STREAM, 0);
    if (ptr->sockFd < 0) {
        return;
    }

    ptr->opt = 1;
    setsockopt(ptr->sockFd, SOL_SOCKET, SO_REUSEADDR, &ptr->opt, sizeof(int));

    memset(&ptr->addr, 0, sizeof(ptr->addr));
    ptr->addr.sin_family = AF_INET;
    ptr->addr.sin_port = htons(port);
    inet_pton(AF_INET, addr, &ptr->addr.sin_addr);

    if (bind(ptr->sockFd, (struct sockaddr*)&ptr->addr, sizeof(ptr->addr)) < 0) {
        close(ptr->sockFd);
        ptr->sockFd = -1;
    }
}

void TcpServer_Start(tcp_server_t* ptr, int maxcons) {
    if (!ptr || ptr->sockFd < 0 || maxcons <= 0 || ptr->isRunning) {
        return;
    }

    if (listen(ptr->sockFd, maxcons) < 0) {
        return;
    }

    pthread_mutex_lock(&ptr->clientsMutex);

    ptr->maxClients = (size_t)maxcons;
    ptr->clientsArrPtr = (tcp_connection_t**)malloc(sizeof(tcp_connection_t*) * ptr->maxClients);
    if (!ptr->clientsArrPtr) {
        ptr->maxClients = 0;
        pthread_mutex_unlock(&ptr->clientsMutex);
        return;
    }

    for (size_t i = 0; i < ptr->maxClients; ++i) {
        ptr->clientsArrPtr[i] = NULL;
    }

    ptr->isRunning = 1;
    pthread_mutex_unlock(&ptr->clientsMutex);

    if (pthread_create(&ptr->svrThread, NULL, TcpServer_threadprocess, ptr) != 0) {
        pthread_mutex_lock(&ptr->clientsMutex);
        ptr->isRunning = 0;
        free(ptr->clientsArrPtr);
        ptr->clientsArrPtr = NULL;
        ptr->maxClients = 0;
        pthread_mutex_unlock(&ptr->clientsMutex);
    }
}

void TcpServer_Stop(tcp_server_t* ptr) {
    if (!ptr || !ptr->isRunning) {
        return;
    }

    ptr->isRunning = 0;

    if (ptr->sockFd >= 0) {
        shutdown(ptr->sockFd, SHUT_RDWR);
        close(ptr->sockFd);
        ptr->sockFd = -1;
    }

    if (ptr->svrThread != 0 && !pthread_equal(ptr->svrThread, pthread_self())) {
        pthread_join(ptr->svrThread, NULL);
    }
    ptr->svrThread = 0;

    pthread_mutex_lock(&ptr->clientsMutex);
    size_t maxClients = ptr->maxClients;
    tcp_connection_t** local = ptr->clientsArrPtr;
    pthread_mutex_unlock(&ptr->clientsMutex);

    for (size_t i = 0; i < maxClients; ++i) {
        tcp_connection_t* cli = local[i];
        if (!cli) {
            continue;
        }

        TcpConnection_RequestClose(cli);
    }

    for (size_t i = 0; i < maxClients; ++i) {
        tcp_connection_t* cli = local[i];
        if (!cli) {
            continue;
        }

        if (!pthread_equal(cli->ioThread, pthread_self())) {
            pthread_join(cli->ioThread, NULL);
        }
    }

    pthread_mutex_lock(&ptr->clientsMutex);
    free(ptr->clientsArrPtr);
    ptr->clientsArrPtr = NULL;
    ptr->maxClients = 0;
    pthread_mutex_unlock(&ptr->clientsMutex);
}

int TcpServer_Send(tcp_server_t* ptr, tcp_connection_t* cli, const void* data, size_t len) {
    if (!ptr || !cli || !data || len == 0) {
        return -1;
    }

    return TcpConnection_SendFramed(cli, data, len);
}

void Generic_SendSocket(int sock, const void* data, size_t len) {
    (void)TcpConnection_SendRaw(sock, data, len);
}

void TcpServer_Disconnect(tcp_server_t* ptr, tcp_connection_t* cli) {
    if (!ptr || !cli) {
        return;
    }

    TcpConnection_RequestClose(cli);

    if (!pthread_equal(cli->ioThread, pthread_self())) {
        pthread_join(cli->ioThread, NULL);
    }
}

void TcpServer_KillClient(tcp_server_t* ptr, tcp_connection_t* cli) {
    if (!ptr || !cli) {
        return;
    }

    struct linger so_linger;
    so_linger.l_onoff = 1;
    so_linger.l_linger = 0;
    setsockopt(cli->sockFd, SOL_SOCKET, SO_LINGER, &so_linger, sizeof(so_linger));

    TcpServer_Disconnect(ptr, cli);
}

size_t Generic_FindClientInArrayByPtr(tcp_connection_t** arr, tcp_connection_t* ptr, size_t len) {
    if (!arr || !ptr) {
        return SIZE_MAX;
    }

    for (size_t i = 0; i < len; ++i) {
        if (arr[i] == ptr) {
            return i;
        }
    }

    return SIZE_MAX;
}

#endif
