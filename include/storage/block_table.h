#ifndef BLOCK_TABLE_H
#define BLOCK_TABLE_H

#include <stdint.h>
#include <stdlib.h>

typedef struct {
    uint64_t blockNumber;
    uint64_t byteNumber;
    uint64_t blockSize;
} block_table_entry_t;

#endif
