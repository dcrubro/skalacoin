#include <nets/net_node.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <runtime_state.h>
#include <balance_sheet.h>
#include <inttypes.h>

static net_node_t* Node_FromConnection(tcp_connection_t* conn) {
    if (!conn) {
        return NULL;
    }

    return (net_node_t*)conn->owner;
}

static uint64_t Node_GetCurrentBlockHeight(void) {
    if (currentChain) {
        return (uint64_t)Chain_Size(currentChain);
    }

    return currentBlockHeight;
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

static bool Node_ParseAndAcceptBlock(const unsigned char* payload, size_t payloadLen, bool persist) {
    if (!payload) { return false; }

    size_t offset = 0;
    if (payloadLen < sizeof(uint64_t) + sizeof(block_header_t) + sizeof(uint64_t)) { return false; }

    uint64_t blockHeight = 0;
    memcpy(&blockHeight, payload + offset, sizeof(blockHeight));
    offset += sizeof(blockHeight);

    block_t* blk = (block_t*)calloc(1, sizeof(block_t));
    if (!blk) { return false; }

    memcpy(&blk->header, payload + offset, sizeof(blk->header));
    offset += sizeof(blk->header);

    uint64_t txCount = 0;
    memcpy(&txCount, payload + offset, sizeof(txCount));
    offset += sizeof(txCount);

    blk->transactions = DYNARR_CREATE(signed_transaction_t, txCount == 0 ? 1 : (size_t)txCount);
    if (!blk->transactions) { free(blk); return false; }

    for (uint64_t i = 0; i < txCount; ++i) {
        if (offset + sizeof(signed_transaction_t) > payloadLen) {
            DynArr_destroy(blk->transactions);
            free(blk);
            return false;
        }
        signed_transaction_t tx;
        memcpy(&tx, payload + offset, sizeof(tx));
        offset += sizeof(tx);
        if (!DynArr_push_back(blk->transactions, &tx)) {
            DynArr_destroy(blk->transactions);
            free(blk);
            return false;
        }
    }

    // Validate block
    if (!Block_IsFullyValid(blk)) {
        DynArr_destroy(blk->transactions);
        free(blk);
        return false;
    }

    if (!currentChain) {
        DynArr_destroy(blk->transactions);
        free(blk);
        return false;
    }

    if (!Chain_AddBlock(currentChain, blk)) {
        // Chain_AddBlock failed; cleanup
        if (blk->transactions) {
            DynArr_destroy(blk->transactions);
        }
        free(blk);
        return false;
    }

    // Persist on accept if requested
    if (persist) {
        Chain_SaveToFile(currentChain, chainDataDir, currentSupply, currentReward);
        BalanceSheet_SaveToFile(chainDataDir);
    }

    // Chain_AddBlock copied the block into the chain; free our temporary wrapper but do NOT destroy transactions (they are freed by Chain_SaveToFile when persisted)
    free(blk);
    return true;
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

    /*
    if (conn->role == TCP_CONNECTION_ROLE_INBOUND && packetType != PACKET_TYPE_RESPONSE) {
        return -1;
    }

    if (conn->role == TCP_CONNECTION_ROLE_OUTBOUND && packetType != PACKET_TYPE_REQUEST) {
        return -1;
    }
    */

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

    switch (packetType) {
        case PACKET_TYPE_HELLO: {
            // Decode HELLO
            if (payloadLen < sizeof(uint32_t) + sizeof(uint64_t)) {
                return;
            }

            uint32_t protoVersion;
            uint64_t blockHeight;
            memcpy(&protoVersion, payload, sizeof(protoVersion));
            memcpy(&blockHeight, payload + sizeof(protoVersion), sizeof(blockHeight));

            // TODO: Save these somewhere and maybe respond
            printf("Received HELLO from node %u: protoVersion=%u, blockHeight=%" PRIu64 "\n",
                client ? client->connectionId : 0U, protoVersion, blockHeight);

            // Craft and send ACK_HELLO
            uint8_t ackBuf[100];
            uint8_t* ackData = ackBuf;
            size_t ackOffset = 0;
            memcpy(ackData + ackOffset, &protoVersion, sizeof(protoVersion));
            ackOffset += sizeof(protoVersion);
            uint64_t currentHeight = Node_GetCurrentBlockHeight();
            memcpy(ackData + ackOffset, &currentHeight, sizeof(currentHeight));
            ackOffset += sizeof(currentHeight);

            Node_SendPacket(Node_FromConnection(client), client, PACKET_TYPE_ACK_HELLO, ackData, ackOffset);

            break;
        }
        case PACKET_TYPE_ACK_HELLO: {
            // This is illegal
            printf("Received unexpected ACK_HELLO packet from node %u\n", client ? client->connectionId : 0U);

            // Send the error and kill the connection
            const char* msg = "You can't ACK_HELLO me! I'm a server!";
            Node_SendPacket(Node_FromConnection(client), client, PACKET_TYPE_ERROR, msg, strlen(msg));
            TcpConnection_RequestClose(client);
            return;
        }
        case PACKET_TYPE_FETCH_BLOCK: {
            // Decode FETCH_BLOCK - payload is the block height as uint64_t
            if (payloadLen != sizeof(uint64_t)) {
                return;
            }

            uint64_t requestedHeight;
            memcpy(&requestedHeight, payload, sizeof(requestedHeight));

            printf("Received FETCH_BLOCK for height %" PRIu64 " from node %u\n", requestedHeight, client ? client->connectionId : 0U);
            if (requestedHeight > Node_GetCurrentBlockHeight()) {
                printf("Requested block height %" PRIu64 " is higher than current height, ignoring\n", requestedHeight);

                // Error the client, but don't kill
                const char* msg = "Requested block height is higher than my current height!";
                Node_SendPacket(Node_FromConnection(client), client, PACKET_TYPE_ERROR, msg, strlen(msg));

                return;
            }

            // Find the block
            block_t* block = Chain_GetBlock(currentChain, (size_t)requestedHeight);
            if (!block) {
                printf("Requested block height %" PRIu64 " not found, ignoring\n", requestedHeight);
                const char* msg = "Requested block not found!";
                Node_SendPacket(Node_FromConnection(client), client, PACKET_TYPE_ERROR, msg, strlen(msg));
                return;
            }

            if (block->transactions == NULL) {
                // Just reload from disk pure
                Chain_LoadBlockFromFile(chainDataDir, requestedHeight - 1, true, &block, NULL); // blockNumber = height - 1 because of 0-indexing
            }

            // Serialize into a BLOCK_DATA packet [block header][tx count - 8 bytes][transactions...]
            size_t blockDataSize = sizeof(block_header_t) + sizeof(uint64_t) + (block->transactions ? block->transactions->size * sizeof(signed_transaction_t) : 0);
            unsigned char* blockData = (unsigned char*)malloc(blockDataSize);
            if (!blockData) {
                // Generic error response
                printf("Failed to allocate memory for block data response to node %u\n", client ? client->connectionId : 0U);
                const char* msg = "Generic error for block data!";
                Node_SendPacket(Node_FromConnection(client), client, PACKET_TYPE_ERROR, msg, strlen(msg));
                return;
            }

            size_t offset = 0;
            memcpy(blockData + offset, &block->header, sizeof(block_header_t));
            offset += sizeof(block_header_t);
            uint64_t txCount = block->transactions ? (uint64_t)block->transactions->size : 0;
            memcpy(blockData + offset, &txCount, sizeof(txCount));
            offset += sizeof(txCount);
            if (block->transactions && block->transactions->size > 0) {
                memcpy(blockData + offset, block->transactions->data, block->transactions->size * sizeof(signed_transaction_t));
                offset += block->transactions->size * sizeof(signed_transaction_t);
            }

            // Send the block data
            Node_SendPacket(Node_FromConnection(client), client, PACKET_TYPE_BLOCK_DATA, blockData, offset);
            free(blockData);
        }
        case PACKET_TYPE_BLOCK_DATA: {
            // Server can't receive these!
            printf("Received unexpected packet type %u from node %u\n", (unsigned int)packetType, client ? client->connectionId : 0U);

            // Send the error and kill the connection
            const char* msg = "You can't send me BLOCK_DATA! I'm a server!";
            Node_SendPacket(Node_FromConnection(client), client, PACKET_TYPE_ERROR, msg, strlen(msg));
            TcpConnection_RequestClose(client);
            return;
        }
        case PACKET_TYPE_BROADCAST_BLOCK: {
            // Accept broadcast blocks from peers and try to append
            if (Node_ParseAndAcceptBlock(payload, payloadLen, true)) {
                printf("Accepted BROADCAST_BLOCK from node %u\n", client ? client->connectionId : 0U);
            } else {
                printf("Rejected BROADCAST_BLOCK from node %u\n", client ? client->connectionId : 0U);
            }
            break;
        }
        case PACKET_TYPE_ACK_BLOCK:
        case PACKET_TYPE_BROADCAST_TX:
        case PACKET_TYPE_ACK_TX:
        case PACKET_TYPE_ERROR: {
            // Decode the message inside as text
            char* text = (char*)malloc(payloadLen + 1);
            if (!text) {
                return;
            }
            memcpy(text, payload, payloadLen);
            text[payloadLen] = '\0';
            printf("Received packet type %u from node %u with message: %s\n",
                (unsigned int)packetType, client ? client->connectionId : 0U, text);
            free(text);
            
            break;
        }
        default:
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

    // Construct and send HELLO
    if (node) {
        uint8_t buf[100];
        uint8_t* data = buf;
        
        size_t offset = 0;
        uint32_t protoVersion = 1; // little-endian
        uint64_t blockHeight = Node_GetCurrentBlockHeight();
        memcpy((unsigned char*)data + offset, &protoVersion, sizeof(protoVersion)); // This is technically "unsafe", but I honestly just don't give a shit at this point
        offset += sizeof(protoVersion);
        memcpy((unsigned char*)data + offset, &blockHeight, sizeof(blockHeight));
        offset += sizeof(blockHeight);

        Node_SendPacket(node, client, PACKET_TYPE_HELLO, data, offset);
    }
}

void Node_Client_OnData(tcp_connection_t* client) {
    packet_type_t packetType;
    const unsigned char* payload = NULL;
    size_t payloadLen = 0;

    if (!client || Node_DecodePacket(client, &packetType, &payload, &payloadLen) != 0) {
        return;
    }

    switch (packetType) {
        case PACKET_TYPE_HELLO: {
            // This is illegal
            printf("Received unexpected HELLO packet from node %u\n", client ? client->connectionId : 0U);

            // Send the error and kill the connection
            const char* msg = "You can't HELLO me! I'm a client!";
            Node_SendPacket(Node_FromConnection(client), client, PACKET_TYPE_ERROR, msg, strlen(msg));
            TcpConnection_RequestClose(client);
            return;
        }
        case PACKET_TYPE_ACK_HELLO: {
            // Decode ACK_HELLO
            if (payloadLen < sizeof(uint32_t) + sizeof(uint64_t)) {
                return;
            }

            uint32_t protoVersion;
            uint64_t blockHeight;
            memcpy(&protoVersion, payload, sizeof(protoVersion));
            memcpy(&blockHeight, payload + sizeof(protoVersion), sizeof(blockHeight));

            printf("Received ACK_HELLO from node %u with protoVersion %u and blockHeight %" PRIu64 "\n", client ? client->connectionId : 0U, protoVersion, blockHeight);

            // Store peer-advertised height on matching outbound client
            net_node_t* node = Node_FromConnection(client);
            if (node) {
                for (size_t i = 0; i < MAX_CONS; ++i) {
                    if (node->outboundClients[i].connection == client) {
                        node->outboundClients[i].peerBlockHeight = blockHeight;
                        break;
                    }
                }
            }
            break;
        }
        case PACKET_TYPE_FETCH_BLOCK: {
            // A client can't serve a block!
            printf("Received unexpected FETCH_BLOCK packet from node %u\n", client ? client->connectionId : 0U);

            // Send the error and kill the connection (this might be too aggressive)
            const char* msg = "You can't FETCH_BLOCK from me! I'm a client!";
            Node_SendPacket(Node_FromConnection(client), client, PACKET_TYPE_ERROR, msg, strlen(msg));
            TcpConnection_RequestClose(client);
            return;
        }
        case PACKET_TYPE_BLOCK_DATA: {
            if (Node_ParseAndAcceptBlock(payload, payloadLen, true)) {
                printf("Accepted BLOCK_DATA from node %u\n", client ? client->connectionId : 0U);
            } else {
                printf("Rejected BLOCK_DATA from node %u\n", client ? client->connectionId : 0U);
            }
            break;
        }
        case PACKET_TYPE_BROADCAST_BLOCK: {
            if (Node_ParseAndAcceptBlock(payload, payloadLen, true)) {
                printf("Accepted BROADCAST_BLOCK from node %u\n", client ? client->connectionId : 0U);
            } else {
                printf("Rejected BROADCAST_BLOCK from node %u\n", client ? client->connectionId : 0U);
            }
            break;
        }
        case PACKET_TYPE_ACK_BLOCK:
        case PACKET_TYPE_BROADCAST_TX:
        case PACKET_TYPE_ACK_TX:
        case PACKET_TYPE_ERROR: {
            // Decode the message inside as text
            char* text = (char*)malloc(payloadLen + 1);
            if (!text) {
                return;
            }
            memcpy(text, payload, payloadLen);
            text[payloadLen] = '\0';
            printf("Received packet type %u from node %u with message: %s\n",
                (unsigned int)packetType, client ? client->connectionId : 0U, text);
            free(text);
            
            break;
        }
        default:
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
