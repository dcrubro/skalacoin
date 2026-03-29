#include <numgen.h>

unsigned char random_byte(void) {
    return (unsigned char)(rand() % 256);
}

uint16_t random_two_byte(void) {
    uint16_t x;
    unsigned char bytes[2];
    for (unsigned char i = 0; i < 2; i++) {
        bytes[i] = random_byte();
    }

    memcpy(&x, bytes, sizeof(x));

    return x;
}

uint32_t random_four_byte(void) {
    uint32_t x;
    unsigned char bytes[4];
    for (unsigned char i = 0; i < 4; i++) {
        bytes[i] = random_byte();
    }

    memcpy(&x, bytes, sizeof(x));

    return x;
}

uint64_t random_eight_byte(void) {
    uint64_t x;
    unsigned char bytes[8];
    for (unsigned char i = 0; i < 8; i++) {
        bytes[i] = random_byte();
    }

    memcpy(&x, bytes, sizeof(x));

    return x;
}
