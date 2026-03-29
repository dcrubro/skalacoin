#include <tcpd/tcpserver.h>

TcpServer* TcpServer_Create() {
    TcpServer* svr = (TcpServer*)malloc(sizeof(TcpServer));

    if (!svr) {
        perror("tcpserver - creation failure");
        exit(1);
    }

    svr->sockFd = -1;
    svr->svrThread = 0;
    svr->on_connect = NULL;
    svr->on_data = NULL;
    svr->on_disconnect = NULL;

    svr->clients = 0;
    svr->clientsArrPtr = NULL;

    return svr;
}

void TcpServer_Destroy(TcpServer* ptr) {
    if (ptr) {
        if (ptr->clientsArrPtr) {
            for (size_t i = 0; i < ptr->clients; i++) {
                if (ptr->clientsArrPtr[i]) {
                    free(ptr->clientsArrPtr[i]);
                }
            }

            free(ptr->clientsArrPtr);
        }

        close(ptr->sockFd);
        free(ptr);
    }
}

void TcpServer_Init(TcpServer* ptr, unsigned short port, const char* addr) {
    if (ptr) {
        // Create socket
        ptr->sockFd = socket(AF_INET, SOCK_STREAM, 0);
        if (ptr->sockFd < 0) {
            perror("tcpserver - socket");
            exit(EXIT_FAILURE);
        }

        // Allow quick port resue
        ptr->opt = 1;
        setsockopt(ptr->sockFd, SOL_SOCKET, SO_REUSEADDR, &ptr->opt, sizeof(int));

        // Fill address structure
        memset(&ptr->addr, 0, sizeof(ptr->addr));
        ptr->addr.sin_family = AF_INET;
        ptr->addr.sin_port = htons(port);
        inet_pton(AF_INET, addr, &ptr->addr.sin_addr);

        // Bind
        if (bind(ptr->sockFd, (struct sockaddr*)&ptr->addr, sizeof(ptr->addr)) < 0) {
            perror("tcpserver - bind");
            close(ptr->sockFd);
            exit(EXIT_FAILURE);
        }
    }
}

// Do not call outside of func.
void* TcpServer_clientthreadprocess(void* ptr) {
    if (!ptr) {
        perror("Client ptr is null!\n");
        return NULL;
    }

    tcpclient_thread_args* args = (tcpclient_thread_args*)ptr;

    TcpClient* cli = args->clientPtr;
    TcpServer* svr = args->serverPtr;

    if (args) {
        free(args);
    }

    while (1) {
        memset(cli->dataBuf, 0, MTU); // Reset buffer
        ssize_t n = recv(cli->clientFd, cli->dataBuf, MTU, 0);
        cli->dataBufLen = n;

        if (n == 0) {
            break; // Client disconnected
        } else if (n > 0) {
            if (cli->on_data) {
                cli->on_data(cli);
            }
        }

        pthread_testcancel(); // Check for thread death
    }

    cli->on_disconnect(cli);

    // Close on exit
    close(cli->clientFd);

    // Destroy
    TcpClient** arr = svr->clientsArrPtr;
    size_t idx = Generic_FindClientInArrayByPtr(arr, cli, svr->clients);
    if (idx != SIZE_MAX) {
        if (arr[idx]) {
            free(arr[idx]);
            arr[idx] = NULL;
        }
    } else {
        perror("tcpserver (client thread) - something already freed the client!");
    }
    
    //free(ptr);

    return NULL;
}

// Do not call outside of func.
void* TcpServer_threadprocess(void* ptr) {
    if (!ptr) {
        perror("Client ptr is null!\n");
        return NULL;
    }
    
    TcpServer* svr = (TcpServer*)ptr;
    while (1) {
        TcpClient tempclient;
        socklen_t clientsize = sizeof(tempclient.clientAddr);
        int client = accept(svr->sockFd, (struct sockaddr*)&tempclient.clientAddr, &clientsize);
        if (client >= 0) {
            tempclient.clientFd = client;
            tempclient.on_data = svr->on_data;
            tempclient.on_disconnect = svr->on_disconnect;

            // I'm lazy, so I'm just copying the data for now (I should probably make this a better way)
            TcpClient* heapCli = (TcpClient*)malloc(sizeof(TcpClient));
            if (!heapCli) {
                perror("tcpserver - client failed to allocate");
                exit(EXIT_FAILURE); // Wtf just happened???
            }

            heapCli->clientAddr = tempclient.clientAddr;
            heapCli->clientFd = tempclient.clientFd;
            heapCli->on_data = tempclient.on_data;
            heapCli->on_disconnect = tempclient.on_disconnect;
            heapCli->clientId = random_four_byte();
            heapCli->dataBufLen = 0;

            size_t i;
            for (i = 0; i < svr->clients; i++) {
                if (svr->clientsArrPtr[i] == NULL) {
                    // Make use of that space
                    svr->clientsArrPtr[i] = heapCli; // We have now transfered the ownership :)
                    break;
                }
            }

            if (i == svr->clients) {
                // Not found
                // RST; Thread doesn't exist yet
                struct linger so_linger;
                so_linger.l_onoff = 1;
                so_linger.l_linger = 0;
                setsockopt(heapCli->clientFd, SOL_SOCKET, SO_LINGER, &so_linger, sizeof(so_linger));
                close(heapCli->clientFd);
                
                free(heapCli);
                heapCli = NULL;
                //svr->clientsArrPtr[i] = NULL;

                continue;
            }
            
            tcpclient_thread_args* arg = (tcpclient_thread_args*)malloc(sizeof(tcpclient_thread_args));
            arg->clientPtr = heapCli;
            arg->serverPtr = svr;

            if (svr->on_connect) {
                svr->on_connect(heapCli);
            }
            
            pthread_create(&heapCli->clientThread, NULL, TcpServer_clientthreadprocess, arg);
            pthread_detach(heapCli->clientThread); // May not work :(
        }

        pthread_testcancel(); // Check for thread death
    }

    return NULL;
}

void TcpServer_Start(TcpServer* ptr, int maxcons) {
    if (ptr) {
        if (listen(ptr->sockFd, maxcons) < 0) {
            perror("tcpserver - listen");
            close(ptr->sockFd);
            exit(EXIT_FAILURE);
        }

        ptr->clients = maxcons;
        ptr->clientsArrPtr = (TcpClient**)malloc(sizeof(TcpClient*) * maxcons);

        if (!ptr->clientsArrPtr) {
            perror("tcpserver - allocation of client space fatally errored");
            exit(EXIT_FAILURE);
        }

        // Fucking null out everything
        for (int i = 0; i < maxcons; i++) {
            ptr->clientsArrPtr[i] = NULL;
        }
    }

    // Spawn server thread
    pthread_create(&ptr->svrThread, NULL, TcpServer_threadprocess, ptr);
}

void TcpServer_Stop(TcpServer* ptr) {
    if (ptr && ptr->svrThread != 0) {
        // Stop server
        pthread_cancel(ptr->svrThread);
        pthread_join(ptr->svrThread, NULL);

        // Disconnect clients
        for (size_t i = 0; i < ptr->clients; i++) {
            TcpClient* cliPtr = ptr->clientsArrPtr[i];
            if (cliPtr) {
                close(cliPtr->clientFd);
                pthread_cancel(cliPtr->clientThread);
            }
        }

        ptr->svrThread = 0;
    }
}

void TcpServer_Send(TcpServer* ptr, TcpClient* cli, void* data, size_t len) {
    if (ptr && cli && data && len > 0) {
        size_t sent = 0;
        while (sent < len) {
            // Ensure that all data is sent. TCP can split sends.
            ssize_t n = send(cli->clientFd, (unsigned char*)data + sent, len - sent, 0);
            if (n < 0) {
                perror("tcpserver - send error");
                break;
            }
            sent += n;
        }
    }
}

void Generic_SendSocket(int sock, void* data, size_t len) {
    if (sock > 0 && data && len > 0) {
        size_t sent = 0;
        while (sent < len) {
            ssize_t n = send(sock, (unsigned char*)data + sent, len - sent, 0);
            if (n < 0) {
                perror("generic - send socket error");
                break;
            }
            sent += n;
        }
    }
}

void TcpServer_Disconnect(TcpServer* ptr, TcpClient* cli) {
    if (ptr && cli) {
        close(cli->clientFd);
        pthread_cancel(cli->clientThread);

        size_t idx = Generic_FindClientInArrayByPtr(ptr->clientsArrPtr, cli, ptr->clients);
        if (idx != SIZE_MAX) {
            if (ptr->clientsArrPtr[idx]) {
                free(ptr->clientsArrPtr[idx]);
            }
            ptr->clientsArrPtr[idx] = NULL;
        } else {
            perror("tcpserver - didn't find client to disconnect in array!");
        }
    }
}

void TcpServer_KillClient(TcpServer* ptr, TcpClient* cli) {
    if (ptr && cli) {
        // RST the connection
        struct linger so_linger;
        so_linger.l_onoff = 1;
        so_linger.l_linger = 0;
        setsockopt(cli->clientFd, SOL_SOCKET, SO_LINGER, &so_linger, sizeof(so_linger));
        close(cli->clientFd);
        pthread_cancel(cli->clientThread);
        
        size_t idx = Generic_FindClientInArrayByPtr(ptr->clientsArrPtr, cli, ptr->clients);
        if (idx != SIZE_MAX) {
            if (ptr->clientsArrPtr[idx]) {
                free(ptr->clientsArrPtr[idx]);
            }
            ptr->clientsArrPtr[idx] = NULL;
        } else {
            perror("tcpserver - didn't find client to kill in array!");
        }
    }
}

size_t Generic_FindClientInArrayByPtr(TcpClient** arr, TcpClient* ptr, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (arr[i] == ptr) {
            return i;
        }
    }

    return SIZE_MAX; // Returns max unsigned, likely improbable to be correct
}
