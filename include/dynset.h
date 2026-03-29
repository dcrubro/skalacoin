#ifndef DYNSET_H
#define DYNSET_H

#include <dynarr.h>

// Dynamic Set structure - basically DynArr with uniqueness enforced
typedef struct {
    DynArr* arr;
} DynSet;

// Function prototypes
DynSet* DynSet_Create(size_t elemSize);
void DynSet_Destroy(DynSet* set);
int DynSet_Insert(DynSet* set, const void* element);
int DynSet_Contains(DynSet* set, const void* element);
size_t DynSet_Size(DynSet* set);
void* DynSet_Get(DynSet* set, size_t index);
void DynSet_Remove(DynSet* set, const void* element);

#endif
