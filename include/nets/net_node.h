#ifndef NET_NODE_H
#define NET_NODE_H

#ifndef _WIN32
// POSIX
#include <tcpd/tcpconnection.h>
#include <tcpd/tcpserver.h>
#endif

#include <dynarr.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <constants.h>

typedef struct {
    tcp_server_t* server;
    // TODO: Add the list of clients as well
} net_node_t;

net_node_t* Node_Create();
void Node_Destroy(net_node_t* node);

// Callback logic
void Node_Server_OnConnect(tcp_connection_t* client);
void Node_Server_OnData(tcp_connection_t* client);
void Node_Server_OnDisconnect(tcp_connection_t* client);

#endif
