#ifndef _WIN32

#include <tcpd/tcpserver.h>

#include <errno.h>
#include <netinet/in.h>
#include <numgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    tcp_server_t* serverPtr;
    int listenFd;
} tcpaccept_thread_args_t;

// Returns non-zero if `cli` was still registered (and has now been unregistered). A zero return
// means someone else already claimed the slot -- see the detach logic in the client thread.
static int TcpServer_RemoveClientByPtrUnlocked(tcp_server_t* svr, tcp_connection_t* cli) {
    if (!svr || !svr->clientsArrPtr || !cli) {
        return 0;
    }

    size_t idx = Generic_FindClientInArrayByPtr(svr->clientsArrPtr, cli, svr->maxClients);
    if (idx != SIZE_MAX) {
        svr->clientsArrPtr[idx] = NULL;
        return 1;
    }

    return 0;
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

    // Unregister, decide who joins us, and free -- all under clientsMutex.
    //
    // The destroy/free used to happen after the lock was released, which left a window where
    // TcpServer_Stop could be holding this very pointer and about to use it. Doing it under the
    // same lock Stop uses to inspect the slots removes that window entirely.
    pthread_mutex_lock(&svr->clientsMutex);

    // If our slot was still ours, TcpServer_Stop has not claimed us and never will (we are leaving
    // the array now), so nobody is going to join this thread -- detach it or its resources leak.
    // If the slot was already cleared, Stop took our handle and is waiting in pthread_join, so we
    // must stay joinable.
    if (TcpServer_RemoveClientByPtrUnlocked(svr, cli)) {
        pthread_detach(pthread_self());
    }

    TcpConnection_Destroy(cli);
    free(cli);

    pthread_mutex_unlock(&svr->clientsMutex);

    return NULL;
}

static void* TcpServer_threadprocess(void* ptr) {
    tcpaccept_thread_args_t* args = (tcpaccept_thread_args_t*)ptr;
    if (!args || !args->serverPtr) {
        free(args);
        return NULL;
    }

    tcp_server_t* svr = args->serverPtr;
    int listenFd = args->listenFd;
    free(args);

    while (svr->isRunning) {
        struct sockaddr_storage clientAddr;
        socklen_t clientSize = sizeof(clientAddr);
        int clientFd = accept(listenFd, (struct sockaddr*)&clientAddr, &clientSize);

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
    svr->sockFdV4 = -1;
    svr->svrThread = 0;
    svr->svrThreadV4 = 0;
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

    ptr->opt = 1;

    // IPv6 (pure, not dual-stack — a dedicated IPv4 socket handles IPv4 clients)
    int fd6 = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd6 >= 0) {
        setsockopt(fd6, SOL_SOCKET, SO_REUSEADDR, &ptr->opt, sizeof(ptr->opt));
        int v6only = 1;
        setsockopt(fd6, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));

        struct sockaddr_in6 a6;
        memset(&a6, 0, sizeof(a6));
        a6.sin6_family = AF_INET6;
        a6.sin6_port = htons(port);
        a6.sin6_addr = in6addr_any;

        if (bind(fd6, (struct sockaddr*)&a6, sizeof(a6)) == 0) {
            ptr->sockFd = fd6;
        } else {
            close(fd6);
        }
    }

    // IPv4 (always attempted regardless of IPv6 result)
    int fd4 = socket(AF_INET, SOCK_STREAM, 0);
    if (fd4 >= 0) {
        setsockopt(fd4, SOL_SOCKET, SO_REUSEADDR, &ptr->opt, sizeof(ptr->opt));

        struct sockaddr_in a4;
        memset(&a4, 0, sizeof(a4));
        a4.sin_family = AF_INET;
        a4.sin_port = htons(port);
        if (inet_pton(AF_INET, addr, &a4.sin_addr) <= 0) {
            a4.sin_addr.s_addr = INADDR_ANY;
        }

        if (bind(fd4, (struct sockaddr*)&a4, sizeof(a4)) == 0) {
            ptr->sockFdV4 = fd4;
        } else {
            close(fd4);
        }
    }
}

void TcpServer_Start(tcp_server_t* ptr, int maxcons) {
    if (!ptr || (ptr->sockFd < 0 && ptr->sockFdV4 < 0) || maxcons <= 0 || ptr->isRunning) {
        return;
    }

    if (ptr->sockFd >= 0 && listen(ptr->sockFd, maxcons) < 0) {
        close(ptr->sockFd);
        ptr->sockFd = -1;
    }

    if (ptr->sockFdV4 >= 0 && listen(ptr->sockFdV4, maxcons) < 0) {
        close(ptr->sockFdV4);
        ptr->sockFdV4 = -1;
    }

    if (ptr->sockFd < 0 && ptr->sockFdV4 < 0) {
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

    int anyThreadStarted = 0;

    if (ptr->sockFd >= 0) {
        tcpaccept_thread_args_t* args = (tcpaccept_thread_args_t*)malloc(sizeof(*args));
        if (args) {
            args->serverPtr = ptr;
            args->listenFd = ptr->sockFd;
            if (pthread_create(&ptr->svrThread, NULL, TcpServer_threadprocess, args) == 0) {
                anyThreadStarted = 1;
            } else {
                free(args);
            }
        }
    }

    if (ptr->sockFdV4 >= 0) {
        tcpaccept_thread_args_t* args = (tcpaccept_thread_args_t*)malloc(sizeof(*args));
        if (args) {
            args->serverPtr = ptr;
            args->listenFd = ptr->sockFdV4;
            if (pthread_create(&ptr->svrThreadV4, NULL, TcpServer_threadprocess, args) == 0) {
                anyThreadStarted = 1;
            } else {
                free(args);
            }
        }
    }

    if (!anyThreadStarted) {
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

    if (ptr->sockFdV4 >= 0) {
        shutdown(ptr->sockFdV4, SHUT_RDWR);
        close(ptr->sockFdV4);
        ptr->sockFdV4 = -1;
    }

    if (ptr->svrThread != 0 && !pthread_equal(ptr->svrThread, pthread_self())) {
        pthread_join(ptr->svrThread, NULL);
    }
    ptr->svrThread = 0;

    if (ptr->svrThreadV4 != 0 && !pthread_equal(ptr->svrThreadV4, pthread_self())) {
        pthread_join(ptr->svrThreadV4, NULL);
    }
    ptr->svrThreadV4 = 0;

    // Ask every live client to close and copy out its thread handle, all under clientsMutex.
    //
    // This used to read the client slots with the lock released, which races with an exiting client
    // thread clearing its own slot -- and worse, that thread destroys and frees the connection right
    // afterwards, so the pointer read here could already be freed memory. Copying the pthread_t
    // while holding the lock means the join below never dereferences the connection at all, and the
    // client thread cannot free itself out from under us because it does that under the same lock.
    pthread_mutex_lock(&ptr->clientsMutex);
    size_t maxClients = ptr->maxClients;
    pthread_t* joinHandles = maxClients ? (pthread_t*)calloc(maxClients, sizeof(pthread_t)) : NULL;
    size_t joinCount = 0;

    if (ptr->clientsArrPtr) {
        for (size_t i = 0; i < maxClients; ++i) {
            tcp_connection_t* cli = ptr->clientsArrPtr[i];
            if (!cli) {
                continue;
            }

            TcpConnection_RequestClose(cli);

            if (joinHandles && !pthread_equal(cli->ioThread, pthread_self())) {
                joinHandles[joinCount++] = cli->ioThread;
                // Claim the slot: the client thread checks whether it is still registered to decide
                // whether to detach itself or stay joinable for the pthread_join below.
                ptr->clientsArrPtr[i] = NULL;
            }
        }
    }
    pthread_mutex_unlock(&ptr->clientsMutex);

    // Join outside the lock: a client thread needs clientsMutex to finish unregistering itself.
    for (size_t i = 0; i < joinCount; ++i) {
        pthread_join(joinHandles[i], NULL);
    }
    free(joinHandles);

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
