#include <dynset.h>

DynSet* DynSet_Create(size_t elemSize) {
    DynSet* set = (DynSet*)malloc(sizeof(DynSet));
    if (!set) {
        return NULL;
    }
    set->arr = DynArr_create(elemSize, 1);
    if (!set->arr) {
        free(set);
        return NULL;
    }
    return set;
}

void DynSet_Destroy(DynSet* set) {
    if (set) {
        DynArr_destroy(set->arr);
        free(set);
    }
}

int DynSet_Insert(DynSet* set, const void* element) {
    if (DynSet_Contains(set, element)) {
        return 0; // Element already exists
    }
    return DynArr_push_back(set->arr, element) != NULL;
}

int DynSet_Contains(DynSet* set, const void* element) {
    size_t size = DynArr_size(set->arr);
    for (size_t i = 0; i < size; i++) {
        void* current = DynArr_at(set->arr, i);
        if (memcmp(current, element, set->arr->elemSize) == 0) {
            return 1; // Found
        }
    }
    return 0; // Not found
}

size_t DynSet_Size(DynSet* set) {
    return DynArr_size(set->arr);
}

void* DynSet_Get(DynSet* set, size_t index) {
    return DynArr_at(set->arr, index);
}

void DynSet_Remove(DynSet* set, const void* element) {
    size_t size = DynArr_size(set->arr);
    for (size_t i = 0; i < size; i++) {
        void* current = DynArr_at(set->arr, i);
        if (memcmp(current, element, set->arr->elemSize) == 0) {
            DynArr_remove(set->arr, i);
            return;
        }
    }
}
