#ifndef PACKETTYPE_H
#define PACKETTYPE_H

#include <stdint.h>

typedef enum {
    PACKET_TYPE_NONE = 0,
    PACKET_TYPE_HELLO = 1, // Hello, let's connect! Here's who I am, and what I have!
    PACKET_TYPE_ACK_HELLO = 2, // I received your hello, here's who I am and what I have (response to hello)
    PACKET_TYPE_FETCH_BLOCK = 3, // I want a Block
    PACKET_TYPE_BLOCK_DATA = 4, // Here's a Block (response to fetch) - I don't care what you do with it, but you wanted it, so here it is!
    PACKET_TYPE_BROADCAST_BLOCK = 5, // Here's a new Block I want to share with the network (unsolicited - e.g. just mined it)
    PACKET_TYPE_ACK_BLOCK = 6, // I have received your block, here's what I did with it (response to broadcast)
    PACKET_TYPE_BROADCAST_TX = 7, // Here's a new transaction I want to share with the network
    PACKET_TYPE_ACK_TX = 8, // I have received your transaction, here's what I did with it (response to broadcast)
    PACKET_TYPE_ERROR = 9, // Something went wrong with the packet you sent me, here's an error message (can be response to any packet)
    PACKET_TYPE_MAX = 10
} packet_type_t;

static inline int PacketType_IsValid(uint8_t packetType) {
    return packetType > PACKET_TYPE_NONE && packetType < PACKET_TYPE_MAX;
}

#endif
