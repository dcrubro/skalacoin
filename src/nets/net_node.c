#include <nets/net_node.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static net_node_t* Node_FromConnection(tcp_connection_t* conn) {
    if (!conn) {
        return NULL;
    }

    return (net_node_t*)conn->owner;
}

static int Node_DecodePacket(const tcp_connection_t* conn, packet_type_t* outType, const unsigned char** outPayload, size_t* outPayloadLen) {
    if (!conn || !outType || !outPayload || !outPayloadLen || conn->dataBufLen < 1 || !conn->dataBuf) {
        return -1;
    }

    uint8_t packetType = conn->dataBuf[0];
    if (!PacketType_IsValid(packetType)) {
        return -1;
    }

    *outType = (packet_type_t)packetType;
    *outPayload = conn->dataBuf + 1;
    *outPayloadLen = conn->dataBufLen - 1;
    return 0;
}

static void Node_ForwardConnect(net_node_t* node, tcp_connection_t* conn) {
    if (node && node->on_connect) {
        node->on_connect(conn, node->callbackUser);
    }
}

static void Node_ForwardDisconnect(net_node_t* node, tcp_connection_t* conn) {
    if (node && node->on_disconnect) {
        node->on_disconnect(conn, node->callbackUser);
    }
}

static void Node_ForwardData(net_node_t* node, tcp_connection_t* conn, const unsigned char* payload, size_t payloadLen) {
    if (node && node->on_data) {
        node->on_data(conn, payload, payloadLen, node->callbackUser);
    }
}

net_node_t* Node_Create() {
    net_node_t* node = (net_node_t*)malloc(sizeof(net_node_t));
    if (!node) {
        return NULL;
    }

    memset(node, 0, sizeof(*node));

    node->server = TcpServer_Create();
    if (!node->server) {
        free(node);
        return NULL;
    }

    for (size_t i = 0; i < MAX_CONS; ++i) {
        if (TcpClient_Init(&node->outboundClients[i]) != 0) {
            Node_Destroy(node);
            return NULL;
        }
    }

    TcpServer_Init(node->server, LISTEN_PORT, "0.0.0.0");

    node->server->owner = node;
    node->server->on_connect = Node_Server_OnConnect;
    node->server->on_data = Node_Server_OnData;
    node->server->on_disconnect = Node_Server_OnDisconnect;

    TcpServer_Start(node->server, MAX_CONS);

    return node;
}

void Node_Destroy(net_node_t* node) {
    if (!node) {
        return;
    }

    for (size_t i = 0; i < MAX_CONS; ++i) {
        TcpClient_Destroy(&node->outboundClients[i]);
    }
    node->outboundCount = 0;

    if (node->server) {
        TcpServer_Stop(node->server);
        TcpServer_Destroy(node->server);
    }

    free(node);
}

void Node_SetCallbacks(
    net_node_t* node,
    void (*on_connect)(tcp_connection_t* conn, void* user),
    void (*on_data)(tcp_connection_t* conn, const unsigned char* data, size_t len, void* user),
    void (*on_disconnect)(tcp_connection_t* conn, void* user),
    void* user
) {
    if (!node) {
        return;
    }

    node->on_connect = on_connect;
    node->on_data = on_data;
    node->on_disconnect = on_disconnect;
    node->callbackUser = user;
}

int Node_ConnectPeer(net_node_t* node, const char* ip, unsigned short port) {
    if (!node || !ip) {
        return -1;
    }

    for (size_t i = 0; i < MAX_CONS; ++i) {
        if (node->outboundClients[i].connection == NULL) {
            if (TcpClient_Connect(
                &node->outboundClients[i],
                ip,
                port,
                Node_Client_OnConnect,
                Node_Client_OnData,
                Node_Client_OnDisconnect,
                node
            ) == 0) {
                node->outboundCount++;
                return 0;
            }
            return -1;
        }
    }

    return -1;
}

int Node_ConnectStartupPeers(net_node_t* node, const char** ips, const unsigned short* ports, size_t peersCount) {
    if (!node || !ips || !ports) {
        return -1;
    }

    int successes = 0;
    for (size_t i = 0; i < peersCount; ++i) {
        if (Node_ConnectPeer(node, ips[i], ports[i]) == 0) {
            successes++;
        }
    }

    return successes;
}

int Node_SendPacket(net_node_t* node, tcp_connection_t* conn, packet_type_t packetType, const void* payload, size_t payloadLen) {
    if (!node || !conn || !PacketType_IsValid((uint8_t)packetType) || (!payload && payloadLen > 0)) {
        return -1;
    }

    if (conn->role == TCP_CONNECTION_ROLE_INBOUND && packetType != PACKET_TYPE_RESPONSE) {
        return -1;
    }

    if (conn->role == TCP_CONNECTION_ROLE_OUTBOUND && packetType != PACKET_TYPE_REQUEST) {
        return -1;
    }

    size_t framePayloadLen = payloadLen + 1;
    unsigned char* framed = (unsigned char*)malloc(framePayloadLen);
    if (!framed) {
        return -1;
    }

    framed[0] = (unsigned char)packetType;
    if (payloadLen > 0) {
        memcpy(framed + 1, payload, payloadLen);
    }

    int rc = TcpConnection_SendFramed(conn, framed, framePayloadLen);
    free(framed);

    return rc;
}

void Node_Server_OnConnect(tcp_connection_t* client) {
    net_node_t* node = Node_FromConnection(client);
    Node_ForwardConnect(node, client);
    printf("Inbound node connected: %u\n", client ? client->connectionId : 0U);
}

void Node_Server_OnData(tcp_connection_t* client) {
    packet_type_t packetType;
    const unsigned char* payload = NULL;
    size_t payloadLen = 0;

    if (!client || Node_DecodePacket(client, &packetType, &payload, &payloadLen) != 0) {
        return;
    }

    if (packetType != PACKET_TYPE_REQUEST) {
        return;
    }

    net_node_t* node = Node_FromConnection(client);
    Node_ForwardData(node, client, payload, payloadLen);
}

void Node_Server_OnDisconnect(tcp_connection_t* client) {
    net_node_t* node = Node_FromConnection(client);
    Node_ForwardDisconnect(node, client);
    printf("Inbound node disconnected: %u\n", client ? client->connectionId : 0U);
}

void Node_Client_OnConnect(tcp_connection_t* client) {
    net_node_t* node = Node_FromConnection(client);
    Node_ForwardConnect(node, client);
    printf("Outbound node connected: %u\n", client ? client->connectionId : 0U);
}

void Node_Client_OnData(tcp_connection_t* client) {
    packet_type_t packetType;
    const unsigned char* payload = NULL;
    size_t payloadLen = 0;

    if (!client || Node_DecodePacket(client, &packetType, &payload, &payloadLen) != 0) {
        return;
    }

    if (packetType != PACKET_TYPE_RESPONSE) {
        return;
    }

    net_node_t* node = Node_FromConnection(client);
    Node_ForwardData(node, client, payload, payloadLen);
}

void Node_Client_OnDisconnect(tcp_connection_t* client) {
    net_node_t* node = Node_FromConnection(client);
    if (node && node->outboundCount > 0) {
        node->outboundCount--;
    }

    Node_ForwardDisconnect(node, client);
    printf("Outbound node disconnected: %u\n", client ? client->connectionId : 0U);
}
