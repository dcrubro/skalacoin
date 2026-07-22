#ifndef UDP_PACKET_TYPE_H
#define UDP_PACKET_TYPE_H

typedef enum {
    UDP_PACKET_TYPE_NONE = 0,
    UDP_PACKET_TYPE_PING = 1,
    UDP_PACKET_TYPE_PONG = 2,
} udp_packet_type_t;

// Wire sizes in bytes
#define UDP_PING_WIRE_SIZE 9   // 1 (type) + 8 (nonce)
#define UDP_PONG_WIRE_SIZE 13  // 1 (type) + 8 (nonce) + 4 (proto_version)

#endif
