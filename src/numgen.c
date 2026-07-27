#include <numgen.h>

#include <stdio.h>
#include <unistd.h>

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

uint64_t random_secure_eight_byte(void) {
    uint64_t x = 0;

    FILE* urandom = fopen("/dev/urandom", "rb");
    if (urandom) {
        size_t got = fread(&x, 1, sizeof(x), urandom);
        fclose(urandom);
        if (got == sizeof(x) && x != 0) {
            return x;
        }
    }

    // Fallback: srand() is seeded from the wall clock in whole seconds, so two nodes launched
    // together would draw identical values. Mix in the pid and the sub-second clock to separate them.
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        ts.tv_sec = 0;
        ts.tv_nsec = 0;
    }

    x = random_eight_byte();
    x ^= (uint64_t)ts.tv_nsec;
    x ^= ((uint64_t)ts.tv_sec) << 16;
    x ^= ((uint64_t)getpid()) << 40;

    return x ? x : 1; // 0 means "no identity advertised" on the wire
}
