#ifndef PACKETTYPE_H
#define PACKETTYPE_H

#include <stdint.h>

typedef enum {
    PACKET_TYPE_NONE = 0,
    PACKET_TYPE_REQUEST = 1,
    PACKET_TYPE_RESPONSE = 2,
    PACKET_TYPE_MAX = 3
} packet_type_t;

static inline int PacketType_IsValid(uint8_t packetType) {
    return packetType > PACKET_TYPE_NONE && packetType < PACKET_TYPE_MAX;
}

#endif
