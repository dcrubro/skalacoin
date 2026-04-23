#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

static inline void AddressToHexString(const uint8_t address[32], char out[65]) {
    if (!address || !out) {
        return;
    }
    to_hex(address, out);
}

#endif
