#ifndef DYNARR_H
#define DYNARR_H

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define DYNARR_MAX_CAPACITY ((size_t)0x7FFFFFFF)

typedef struct {
    size_t size;
    size_t elemSize;
    size_t capacity;
    void* data;
} DynArr;

// Do not use; Use DYNARR_CREATE macro instead.
DynArr* DynArr_create(size_t elemSize, size_t capacity);

// Reserve n blocks in arary; New size will be n, NOT size + n; Reserving less memory that current will fail, use prune instead.
void DynArr_reserve(DynArr* p, size_t n);

// Push data into a new block at the end of the array
void* DynArr_push_back(DynArr* p, void* value);

// Remove the last block in the array.
void DynArr_pop_back(DynArr* p);

// Remove first block from array.
void DynArr_pop_front(DynArr* p);

// Remove index from array. This moves all blocks after the index block.
void DynArr_remove(DynArr* p, size_t index);

// Erase the array. This will not free unused blocks.
void DynArr_erase(DynArr* p);

// Prune and free unused blocks. If pruning to zero, ensure to reserve after.
void DynArr_prune(DynArr* p);

// Get a pointer to a block by index
void* DynArr_at(DynArr* p, size_t index);

// Get the index by block pointer
size_t DynArr_at_ptr(DynArr* p, void* ptr);

// Get size
size_t DynArr_size(DynArr* p);

// Get element size
size_t DynArr_elemSize(DynArr* p);

// Get capacity
size_t DynArr_capacity(DynArr* p);

void DynArr_destroy(DynArr* p);

// Note: Make sure to not overread or overwrite
void* DynArr_c_arr(DynArr* p);

#define DYNARR_CREATE(T, initialCapacity) DynArr_create(sizeof(T), initialCapacity)

#endif
