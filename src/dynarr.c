#include <dynarr.h>

DynArr* DynArr_create(size_t elemSize, size_t capacity) {
    DynArr* p = (DynArr*)malloc(sizeof(DynArr));
    if (!p) return NULL;
    
    p->elemSize = elemSize;
    p->capacity = capacity;
    p->data = malloc(elemSize * capacity);
    if (!p->data) {
        free(p);
        return NULL;
    }
    p->size = 0;
    
    return p;
}

// Reserve n blocks in arary; New size will be n, NOT size + n; Reserving less memory that current will fail, use prune instead.
void DynArr_reserve(DynArr* p, size_t n) {
    if (n <= p->capacity) {
        printf("reserve ignored; attempted to reserve less or equal to current capacity\n");
        return;
    }

    if (n > DYNARR_MAX_CAPACITY) {
        printf("reserve ignored; attempted to reserve over 32 bits\n");
        return;      
    }

    void* new_data = realloc(p->data, n * p->elemSize);
    if (!new_data) {
        printf("reserve failed\n");
        exit(1);
    }
    p->data = new_data;
    p->capacity = n;
}

// Push data into a new block at the end of the array; If value is NULL, the new block will be zeroed.
void* DynArr_push_back(DynArr* p, void* value) {
    //if (value == NULL) {
    //    printf("push_back ignored; value is null");
    //    return NULL;
    //}
    
    if (p->size >= p->capacity) {
        size_t new_cap = (p->capacity == 0) ? 1 : p->capacity * 2;

        if (new_cap < p->capacity || new_cap > DYNARR_MAX_CAPACITY) {
            printf("push_back ignored; capacity overflow\n");
            return NULL;
        }

        void* new_data = realloc(p->data, new_cap * p->elemSize);
        if (!new_data) {
            printf("push failed\n");
            exit(1);
        }
        p->capacity = new_cap;
        p->data = new_data;
    }

    void* dst = (void*)((char*)p->data + (p->size * p->elemSize));

    if (value == NULL) {
        memset(dst, 0, p->elemSize); // Handle NULL value.
    } else {
        memcpy((char*)dst, value, p->elemSize);
    }

    p->size++;

    return dst;
}

// Remove the last block in the array.
void DynArr_pop_back(DynArr* p) {
    if (p->size == 0) {
        printf("pop_back ignored; size is 0\n");
        return;
    }

    p->size--; // Will automatically overwrite that memory naturally
}

// Remove first block from array.
void DynArr_pop_front(DynArr* p) {
    if (p->size == 0) {
        printf("pop_front ignored; size is 0\n");
        return;
    }

    memmove(
        (char*)p->data,
        (char*)p->data + p->elemSize,
        (p->size - 1) * p->elemSize
    );

    p->size--;
}

// Remove index from array. This moves all blocks after the index block.
void DynArr_remove(DynArr* p, size_t index) {
    if (index >= p->size) return;

    memmove(
        (char*)p->data + (index * p->elemSize),
        (char*)p->data + (index + 1) * p->elemSize,
        (p->size - index - 1) * p->elemSize
    );
    
    p->size--;
}

// Erase the array. This will not free unused blocks.
void DynArr_erase(DynArr* p) {
    p->size = 0;
}

// Prune and free unused blocks. If pruning to zero, ensure to reserve after.
void DynArr_prune(DynArr* p) {
    void* new_data = realloc(p->data, (p->size == 0 ? 1 : p->size) * p->elemSize);
    if (!new_data) {
        printf("pruning failed\n");
        exit(1);
    }

    p->data = new_data;
    p->capacity = p->size;
}

// Get a pointer to a block by index
void* DynArr_at(DynArr* p, size_t index) {
    if (index >= p->size) return NULL;
    return (char*)p->data + (index * p->elemSize);
}

// Get the index by block pointer
size_t DynArr_at_ptr(DynArr* p, void* ptr) {
    if (!p || !ptr) {
        printf("invalid pointer\n");
        exit(1);
    }

    for (size_t i = 0; i < p->size; i++) {
        if ((void*)(((char*)p->data) + (i * p->elemSize)) == ptr) {
            return i;
        }
    }

    // If for some reason the array has 2^64 elements in it, then fuck it, I guess we'll just crash, I don't care.
    return -1;
}

// Get size
size_t DynArr_size(DynArr* p) {
    return p->size;
}

// Get element size
size_t DynArr_elemSize(DynArr* p) {
    return p->elemSize;
}

// Get capacity
size_t DynArr_capacity(DynArr* p) {
    return p->capacity;
}

void DynArr_destroy(DynArr* p) {
    if (!p) return;
    free(p->data);
    free(p);
}
