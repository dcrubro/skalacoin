#include <nets/net_node.h>

net_node_t* Node_Create() {
    net_node_t* node = (net_node_t*)malloc(sizeof(net_node_t));
    if (!node) { return NULL; }

    node->server = TcpServer_Create();
    if (!node->server) {
        free(node);
        return NULL;
    }

    TcpServer_Init(node->server, LISTEN_PORT, "0.0.0.0"); // All interfaces
    
    // Register callbacks before starting the server
    node->server->on_connect = Node_Server_OnConnect;
    node->server->on_data = Node_Server_OnData;
    node->server->on_disconnect = Node_Server_OnDisconnect;
    
    TcpServer_Start(node->server, MAX_CONS);

    return node;
}

void Node_Destroy(net_node_t* node) {
    if (!node || !node->server) { return; }
    TcpServer_Stop(node->server);
    TcpServer_Destroy(node->server);

    free(node);
}

void Node_Server_OnConnect(tcp_connection_t* client) {
    printf("A node connected!\n");
}

void Node_Server_OnData(tcp_connection_t* client) {
    printf("A node sent data!\n");
}

void Node_Server_OnDisconnect(tcp_connection_t* client) {
    printf("A node disconnected!\n");
}
