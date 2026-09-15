/*
vector.h - Vectors for C
Version: 1.0.0

Description:
Header-only library for vectors in C. Memory safe against the mistakes it can
detect, but not thread-safe (you must implement your own synchronization
mechanisms).
Designed for C23 onwards, but should be compatible with older standards (tested
from C99 onwards).

Guarantees:
 - A vector_t must never be duplicated by copying the struct. Two vector_t values
   sharing one data pointer both believe they own it, and whichever is destroyed
   or moved from first leaves the other holding freed memory. Copying the pointer
   (vector_t* b = a;) is fine - that is one vector with two names, and it must be
   destroyed exactly once.
 - To hand a vector's data to another vector, use vector_move(). The source is
   emptied, freed and its pointer set to NULL, so there is only ever one owner.
 - To get a second, independent vector with the same contents, use
   vector_deep_copy(), which copies the data as well as the vector itself. Deep
   copies are only available for vectors without an element destructor, as a
   byte-wise copy of owning elements would result in a double free.
 - No gaps in the raw data array.
 - When a vector is resized, the data is reallocated to a new memory location,
   so any pointers into the old data are invalidated. There is no way to
   preserve them; re-fetch with vector_get() after the resize.
 - Order of elements is preserved.
 - It is safe to pass a pointer into a vector's own data as the source element
   of vector_push_back(), vector_insert() and vector_set(). Each of them handles
   the aliasing in whichever way suits it, so the value survives the reallocation
   or the shifting of elements. On a vector with an element destructor this is
   rejected with -1 instead, as a byte-wise copy would leave two slots owning the
   same memory. To duplicate an owning element, copy what it owns yourself and
   push that.
 - vector_take_at() and vector_take_back() transfer ownership of the element to the 
   caller, and do not call the destructor on it. It is the caller's responsibility
   to free whatever the element owns.

Element destructors:
A vector may be given an element destructor with vector_set_destructor(). It is
called for every element that leaves the vector, i.e. by vector_pop_back(),
vector_pop_at(), vector_set() (on the element being overwritten), vector_clear()
and vector_destroy().

The destructor is NOT called by vector_take_back() or vector_take_at(), as these
functions transfer ownership of the element to the caller. It is the caller's
responsibility to free whatever the element owns.

The destructor receives a pointer to the element's slot inside the vector's data
array - NOT a pointer that was returned by malloc(). It must free whatever the
element owns, and must never free the pointer it was handed, as that memory
belongs to the vector. For a vector of char* this means:

    void free_str(void* element) { free(*(char**)element); }

    vector_t* vec = vector_create(sizeof(char*));
    vector_set_destructor(vec, free_str);
    // ... use the vector ...
    vector_destroy(&vec); // Takes the address of your pointer, and NULLs it

A warning regarding misuse:
The vector manages its own buffer correctly: no leaks, no double frees of the data
array, no use of stale pointers internally. It cannot reason about what your
elements own — that is what the destructor is for, and it is your responsibility
to set one and to avoid duplicating owned pointers between slots.

License:
MIT License

Copyright (c) 2026 DcruBro

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#ifndef VECTOR_H
#define VECTOR_H

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef void (*vector_destructor_t)(void* element); // Function pointer type for element destructor

/*
 A vector. Do not construct, copy or modify this struct directly - the fields are
 visible only because the functions below are inline. Create vectors with
 vector_create() and use the accessors. A vector_t that did not come from
 vector_create() is not a valid vector, and the functions will reject it where
 they can and misbehave where they cannot.
*/
typedef struct {
    size_t size; // Number of elements in the vector
    size_t capacity; // Allocated capacity of the vector
    size_t element_size; // Size of each element in the vector
    void *data; // Pointer to the raw data array
    vector_destructor_t destructor; // Function pointer to the element destructor
} vector_t;

/* Forward declarations */
static inline int vector_is_aliased(const vector_t* vec, const void* ptr);
static inline vector_t* vector_create(size_t element_size);
static inline int vector_set_destructor(vector_t* vec, vector_destructor_t destructor);
static inline vector_destructor_t vector_get_destructor(const vector_t* vec);
static inline int vector_reserve(vector_t* vec, size_t new_capacity);
static inline int vector_grow(vector_t* vec);
static inline int vector_prune(vector_t* vec);
static inline int vector_push_back(vector_t* vec, const void* element);
static inline int vector_pop_back(vector_t* vec);
static inline int vector_pop_at(vector_t* vec, size_t index);
static inline int vector_take_at(vector_t* vec, size_t index, void* out);
static inline int vector_take_back(vector_t* vec, void* out);
static inline int vector_insert(vector_t* vec, size_t index, const void* element);
static inline int vector_clear(vector_t* vec);
static inline int vector_set(vector_t* vec, size_t index, const void* element);
static inline int vector_is_empty(const vector_t* vec);
static inline void* vector_front(vector_t* vec);
static inline const void* vector_front_const(const vector_t* vec);
static inline void* vector_back(vector_t* vec);
static inline const void* vector_back_const(const vector_t* vec);
static inline void* vector_get(vector_t* vec, size_t index);
static inline const void* vector_get_const(const vector_t* vec, size_t index);
static inline size_t vector_size(const vector_t* vec);
static inline size_t vector_capacity(const vector_t* vec);
static inline size_t vector_element_size(const vector_t* vec);
static inline const void* vector_as_c_array(const vector_t* vec);
static inline void* vector_as_c_array_mutable(vector_t* vec);
static inline int vector_move(vector_t* dest, vector_t** src);
static inline vector_t* vector_deep_copy(const vector_t* vec);
static inline int vector_destroy(vector_t** vec);

/*
 @brief Checks whether a pointer points inside the vector's own data array. Used internally to make the write functions safe against self-referential input.
 @param vec A pointer to the vector to check against.
 @param ptr The pointer to check.
 @return 1 if the pointer lies within the vector's allocated buffer, 0 otherwise.
 @attention This is an internal helper. You are not expected to call it directly, but it is harmless if you do.
*/
static inline int vector_is_aliased(const vector_t* vec, const void* ptr) {
    if (!vec || !ptr || !vec->data) {
        return 0; // Nothing to alias
    }

    // Compared as integers rather than pointers, as comparing pointers into
    // different objects is not well defined.
    uintptr_t base = (uintptr_t)vec->data;
    uintptr_t end = base + (vec->capacity * vec->element_size);
    uintptr_t target = (uintptr_t)ptr;

    return target >= base && target < end;
}

/*
 @brief Creates a new vector with the specified element size.
 @param element_size The size of each element in the vector. Call with sizeof(type)
 @return A pointer to the newly created vector, or NULL if allocation fails. The vector lives on the heap.
 @attention The vector must be destroyed with vector_destroy() to free its memory. Failing to do so will result in a memory leak.
 @attention The vector's initial capacity is set to 10. If you want to change the initial capacity globally, set the DLIBC_VECTOR_INITIAL_CAPACITY macro before including this header file. The initial capacity must be greater than 0.
 @attention The vector's element size must be greater than 0. If you pass 0, the function will return NULL.
 @attention The vector is created without an element destructor. If your elements own memory of their own, set one with vector_set_destructor().
*/
static inline vector_t* vector_create(size_t element_size) {
    if (element_size == 0) {
        return NULL; // Invalid element size
    }

    #ifndef DLIBC_VECTOR_INITIAL_CAPACITY
        size_t initial_capacity = 10; // Default initial capacity
    #else
        #if DLIBC_VECTOR_INITIAL_CAPACITY <= 0
            #error "DLIBC_VECTOR_INITIAL_CAPACITY must be greater than 0"
        #endif
        size_t initial_capacity = DLIBC_VECTOR_INITIAL_CAPACITY; // Use the macro if defined
    #endif

    if (SIZE_MAX / element_size < initial_capacity) {
        return NULL; // Prevent overflow
    }

    vector_t* vec = (vector_t*)malloc(sizeof(vector_t));
    if (!vec) {
        return NULL; // Allocation failed
    }

    vec->size = 0;
    vec->capacity = initial_capacity;
    vec->element_size = element_size;
    vec->destructor = NULL; // Initialize destructor to NULL
    vec->data = malloc(vec->capacity * vec->element_size);
    if (!vec->data) {
        free(vec);
        return NULL; // Allocation failed
    }

    return vec;
}

/*
 @brief Sets the destructor function for the vector's elements. This function will be called on each element as it leaves the vector, allowing for custom cleanup of dynamically allocated memory within the elements.
 @param vec A pointer to the vector for which to set the destructor.
 @param destructor A function pointer to the destructor function, or NULL to remove the current one. The function should take a single void* parameter, which will be a pointer to the element to be destroyed.
 @return 0 on success, -1 if the vector is NULL.
 @attention If you do not set a destructor function, the vector will not automatically free any dynamically allocated memory within its elements when it is destroyed. You must ensure that you free any such memory manually before destroying the vector to avoid memory leaks.
 @attention The destructor is passed a pointer to the element's slot inside the vector's data array, not a pointer returned by malloc(). It must free what the element owns and must never free the pointer it is given. For a vector of char*, the destructor body is free(*(char**)element);
 @attention This assumes that your destructor function is valid and safe to call. If it isn't, bad things will happen and it will not be nice to watch.
 @attention Set the destructor before adding any elements. Setting it on a vector that already holds elements is allowed, but elements that were removed beforehand will not have been destroyed.
*/
static inline int vector_set_destructor(vector_t* vec, vector_destructor_t destructor) {
    if (!vec) {
        return -1; // Invalid vector
    }

    vec->destructor = destructor;
    return 0; // Success
}

/*
 @brief Gets the destructor function currently set on the vector.
 @param vec A pointer to the vector whose destructor is to be retrieved.
 @return The vector's destructor function pointer, or NULL if the vector is NULL or has no destructor set.
*/
static inline vector_destructor_t vector_get_destructor(const vector_t* vec) {
    if (!vec) {
        return NULL; // Invalid vector
    }

    return vec->destructor;
}

/*
 @brief Ensures the vector has room for at least the specified number of elements.
 @param vec A pointer to the vector for which to reserve space.
 @param new_capacity The minimum capacity the vector should have.
 @return 0 on success, -1 if the vector is NULL, its element size is zero, or allocation fails.
 @attention Capacity never decreases. If the vector already has room, this is a no-op and reports success.
 @attention If the buffer does grow, it is reallocated, so any pointers into the vector's data are invalidated. Use vector_prune() to release unused memory.
*/
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 10
// GCC's -fanalyzer loses track of new_data once it is stored through a vector that the caller
// reached via a struct member (holder->vec), and reports the grown buffer as leaked when this
// returns. The buffer is owned by vec->data and released by vector_destroy(), so the report is a
// false positive. Scoped to this function only, so genuine leaks elsewhere are still reported.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
#endif
static inline int vector_reserve(vector_t* vec, size_t new_capacity) {
    if (!vec) {
        return -1; // Invalid vector
    }

    if (vec->element_size == 0) {
        return -1; // Not a usable vector; also guards the division below
    }

    if (new_capacity <= vec->capacity) {
        return 0; // Already have the room. Never shrink
    }

    if (new_capacity > SIZE_MAX / vec->element_size) {
        return -1; // Prevent overflow
    }

    void* new_data = realloc(vec->data, new_capacity * vec->element_size);
    if (!new_data) {
        return -1; // Allocation failed
    }

    vec->data = new_data;
    vec->capacity = new_capacity;

    return 0; // Success
}
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 10
#pragma GCC diagnostic pop
#endif

/*
 @brief Grows the vector's capacity to make room for at least one more element. Used internally by the functions that add elements.
 @param vec A pointer to the vector to grow.
 @return 0 on success, -1 if the vector is NULL or allocation fails.
 @attention This is an internal helper. You are not expected to call it directly.
*/
static inline int vector_grow(vector_t* vec) {
    if (!vec) {
        return -1; // Invalid vector
    }

    if (vec->capacity > SIZE_MAX / 2) {
        return -1; // Prevent overflow
    }

    size_t new_capacity = vec->capacity > 0 ? vec->capacity * 2 : 1; // Double the capacity, or set to 1 if it was 0
    return vector_reserve(vec, new_capacity);
}

/*
 @brief Prunes the vector to free unused memory. If the vector's size is less than its capacity, this function will reallocate the vector's data array to match its size, freeing any unused memory.
 @param vec A pointer to the vector to be pruned.
 @return 0 on success, -1 if the vector is NULL or allocation fails.
 @attention After calling this function, the vector's capacity will be equal to its size. Any pointers to the old data will be invalidated.
 @attention Capacity may never drop below 1, even if the vector is empty.
*/
static inline int vector_prune(vector_t* vec) {
    if (!vec) {
        return -1; // Invalid vector
    }

    if (vec->element_size == 0) {
        return -1; // Not a usable vector; also guards the division below
    }

    if (vec->size < vec->capacity) {
        size_t new_capacity = vec->size > 0 ? vec->size : 1; // Ensure capacity is at least 1

        if (new_capacity > SIZE_MAX / vec->element_size) {
            return -1; // Prevent overflow
        }

        void* new_data = realloc(vec->data, new_capacity * vec->element_size);
        if (!new_data) {
            return -1; // Allocation failed
        }

        vec->data = new_data;
        vec->capacity = new_capacity;
    }

    return 0; // Success
}

/*
 @brief Adds an element to the end of the vector.
 @param vec A pointer to the vector to which the element will be added.
 @param element A pointer to the element to be added. The element will be copied into the vector's data array.
 @return 0 on success, -1 if the vector is NULL, the element is NULL, if the element is an owning element that is aliased in the vector and a destructor is set, or if reservation fails.
 @attention The element must be a pointer to a valid memory location containing data of the same type as the vector's element type. The vector will make a copy of the data, so the original element can be modified or freed after this function returns.
 @attention The element may point into the vector's own data array. If growing the vector moves the data, the pointer is followed to its new location automatically.
*/
static inline int vector_push_back(vector_t* vec, const void* element) {
    if (!vec || !element) {
        return -1; // Invalid vector or element
    }

    if (vec->destructor && vector_is_aliased(vec, element)) {
        return -1; // Refuse to push an owning element that lives inside the vector
    }

    if (vec->size >= vec->capacity) {
        // If the element lives inside our own data array, remember where it sits
        // so that we can find it again after the data has been reallocated.
        // This must happen before the growth, while the old buffer is still valid
        size_t offset = 0;
        int aliased = vector_is_aliased(vec, element);
        if (aliased) {
            // Subtracted as integers, for the same reason vector_is_aliased() compares them that
            // way. Pointer subtraction is only defined within one object, which the check above
            // guarantees, but a static analyzer cannot see through the integer comparison and
            // reports the subtraction as undefined behaviour
            offset = (size_t)((uintptr_t)element - (uintptr_t)vec->data);
        }

        if (vector_grow(vec) != 0) {
            return -1; // Failed to grow the vector
        }

        if (aliased) {
            element = (const char*)vec->data + offset;
        }
    }

    // Copy the new element into the vector's data array
    memmove((char*)vec->data + (vec->size * vec->element_size), element, vec->element_size);
    vec->size++;

    return 0; // Success
}

/*
 @brief Removes the last element from the vector.
 @param vec A pointer to the vector from which the element will be removed.
 @return 0 on success, -1 if the vector is NULL or empty.
 @attention After calling this function, the vector's size will be reduced by one. The memory occupied by the removed element will not be freed automatically unless a destructor is set (in which case, the destructor will be called on the element being removed if it is set). Use vector_take_back() to retrieve the last element, remove it from the vector, and NOT call the destructor on it (hands ownership to the caller).
*/
static inline int vector_pop_back(vector_t* vec) {
    if (!vec || vec->size == 0) {
        return -1; // Invalid vector or empty vector
    }

    if (vec->destructor) {
        void* element = (char*)vec->data + ((vec->size - 1) * vec->element_size);
        vec->destructor(element); // Call the destructor for the last element
    }

    vec->size--;
    return 0; // Success
}

/*
 @brief Removes the element at the specified index from the vector.
 @param vec A pointer to the vector from which the element will be removed.
 @param index The index of the element to be removed.
 @return 0 on success, -1 if the vector is NULL or the index is out of bounds.
 @attention After calling this function, the vector's size will be reduced by one. The memory occupied by the removed element will not be freed automatically unless a destructor is set (in which case, the destructor will be called on the element being removed if it is set). Use vector_take_at() to retrieve the element at the specified index, remove it from the vector, and NOT call the destructor on it (hands ownership to the caller).
*/
static inline int vector_pop_at(vector_t* vec, size_t index) {
    if (!vec || index >= vec->size) {
        return -1; // Invalid vector or index out of bounds
    }

    if (vec->destructor) {
        void* element = (char*)vec->data + (index * vec->element_size);
        vec->destructor(element); // Call the destructor for the element to be removed
    }

    // Move elements after the index one position to the left
    if (index + 1 < vec->size) {
        memmove((char*)vec->data + (index * vec->element_size),
                (char*)vec->data + ((index + 1) * vec->element_size),
                (vec->size - index - 1) * vec->element_size);
    }

    vec->size--;
    return 0; // Success
}

/*
 @brief Removes the element at the specified index and transfers ownership of it to the caller.
 @param vec A pointer to the vector from which the element will be taken.
 @param index The index of the element to take.
 @param out A pointer to a buffer of at least vector_element_size(vec) bytes, which the element is copied into.
 @return 0 on success, -1 if the vector is NULL, out is NULL, out points into the vector's own data, or the index is out of bounds.
 @attention The element destructor is deliberately NOT called. Whatever the element owns becomes the caller's responsibility to free.
 @attention out must not point into the vector's own data array. Doing so would leave two slots owning the same memory and is rejected with -1.
*/
static inline int vector_take_at(vector_t* vec, size_t index, void* out) {
    if (!vec || index >= vec->size || !out) {
        return -1; // Invalid vector, index out of bounds, or output pointer is NULL
    }

    if (vector_is_aliased(vec, out)) {
        return -1; // Refuse to take an element into a pointer that lives inside the vector
    }

    void* slot = (char*)vec->data + (index * vec->element_size);
    memcpy(out, slot, vec->element_size); // Copy the element to the output pointer

    // Move elements after the index one position to the left
    if (index + 1 < vec->size) {
        memmove(slot,
                (char*)vec->data + ((index + 1) * vec->element_size),
                (vec->size - index - 1) * vec->element_size);
    }

    vec->size--;
    return 0; // Success
}

/*
 @brief Removes the last element from the vector and transfers ownership of it to the caller.
 @param vec A pointer to the vector from which the element will be taken.
 @param out A pointer to a buffer of at least vector_element_size(vec) bytes, which the element is copied into.
 @return 0 on success, -1 if the vector is NULL, empty, if out aliases the vector's data, or out is NULL.
 @attention The element destructor is deliberately NOT called. Whatever the element owns becomes the caller's responsibility to free.
*/
static inline int vector_take_back(vector_t* vec, void* out) {
    if (!vec || vec->size == 0 || !out) {
        return -1; // Invalid vector, empty vector, or output pointer is NULL
    }

    return vector_take_at(vec, vec->size - 1, out); // Take the last element
}

/*
 @brief Inserts an element at the specified index in the vector.
 @param vec A pointer to the vector into which the element will be inserted.
 @param index The index at which to insert the element.
 @param element A pointer to the element to be inserted. The element will be copied into the vector's data array.
 @return 0 on success, -1 if the vector is NULL, the element is NULL, the index is out of bounds, if the element is an owning element that is aliased in the vector and a destructor is set, or if reservation fails.
 @attention After calling this function, the vector's size will be increased by one and every element from the index onwards will have moved one position to the right. Any pointers into the vector are invalidated.
 @attention The element may point into the vector's own data array. Its value is copied aside first, so that neither the reallocation nor the shifting of elements can corrupt it.
*/
static inline int vector_insert(vector_t* vec, size_t index, const void* element) {
    if (!vec || !element || index > vec->size) {
        return -1; // Invalid vector, element, or index out of bounds
    }

    if (vec->destructor && vector_is_aliased(vec, element)) {
        return -1; // Refuse to insert an owning element that lives inside the vector
    }

    // If the element lives inside our own data array, take a copy of it now. Both
    // the reservation below and the shifting of elements would otherwise destroy
    // the value before it can be read
    void* temp = NULL;
    if (vector_is_aliased(vec, element)) {
        temp = malloc(vec->element_size);
        if (!temp) {
            return -1; // Allocation failed
        }

        memcpy(temp, element, vec->element_size);
        element = temp;
    }

    if (vec->size >= vec->capacity) {
        if (vector_grow(vec) != 0) {
            free(temp);
            return -1; // Failed to grow the vector
        }
    }

    // Move elements after the index one position to the right
    memmove((char*)vec->data + ((index + 1) * vec->element_size),
            (char*)vec->data + (index * vec->element_size),
            (vec->size - index) * vec->element_size);

    // Copy the new element into the vector's data array at the specified index
    memcpy((char*)vec->data + (index * vec->element_size), element, vec->element_size);
    vec->size++;

    free(temp); // Harmless if no copy was needed, as temp is NULL in that case
    return 0; // Success
}

/*
 @brief Clears all elements from the vector.
 @param vec A pointer to the vector to be cleared.
 @return 0 on success, -1 if the vector is NULL.
 @attention After calling this function, the vector's size will be zero. The memory occupied by the elements will not be freed automatically unless a destructor is set.
 @attention The vector's capacity is left untouched, so the vector may be refilled without reallocating. Call vector_prune() afterwards if you want the memory back.
*/
static inline int vector_clear(vector_t* vec) {
    if (!vec) {
        return -1; // Invalid vector
    }

    if (vec->destructor) {
        for (size_t i = 0; i < vec->size; ++i) {
            void* element = (char*)vec->data + (i * vec->element_size);
            vec->destructor(element); // Call the destructor for each element
        }
    }

    vec->size = 0;
    return 0; // Success
}

/*
 @brief Sets the element at the specified index in the vector, overwriting whatever was there before.
 @param vec A pointer to the vector in which to set the element.
 @param index The index of the element to set.
 @param element A pointer to the element to set. The element will be copied into the vector's data array.
 @return 0 on success, -1 if the vector is NULL, the element is NULL, if the element is an owning element that is aliased in the vector and a destructor is set, or if the index is out of bounds.
 @attention This never grows the vector. The index must be below the current size.
 @attention If a destructor is set, it is called on the element being overwritten before the new value is copied in.
 @attention Setting an element to itself is a no-op and reports success.
 @attention On a vector without a destructor, the element may point into the
   vector's own data array; the copy handles any overlap. On a vector with a
   destructor this is refused with -1, as it would leave two slots owning the
   same memory.
*/
static inline int vector_set(vector_t* vec, size_t index, const void* element) {
    if (!vec || !element || index >= vec->size) {
        return -1; // Invalid vector, element, or index out of bounds
    }

    void* slot = (char*)vec->data + (index * vec->element_size);
    if (slot == element) {
        return 0; // Setting the element to itself, no action needed
    }

    if (vec->destructor && vector_is_aliased(vec, element)) {
        return -1; // Refuse to set an owning element that lives inside the vector
    }

    if (vec->destructor) {
        vec->destructor(slot); // Call the destructor for the element being overwritten
    }

    memmove(slot, element, vec->element_size); // memmove, as the element may overlap the slot

    return 0; // Success
}

/*
 @brief Checks if the vector is empty.
 @param vec A pointer to the vector to check.
 @return 1 if the vector is empty, 0 if it is not empty.
 @attention If the vector is NULL, this function will return 1 (is empty) to indicate that the vector is invalid.
*/
static inline int vector_is_empty(const vector_t* vec) {
    if (!vec) {
        return 1; // Invalid vector
    }

    return vec->size == 0;
}

/*
 @brief Gets a pointer to the first element in the vector.
 @param vec A pointer to the vector from which to get the element.
 @return A pointer to the first element in the vector, or NULL if the vector is NULL or empty.
 @attention The returned pointer is a generic. You should cast it to the appropriate type before using it. The pointer will become invalid if the vector is resized or destroyed.
*/
static inline void* vector_front(vector_t* vec) {
    if (!vec || vec->size == 0) {
        return NULL; // Invalid vector or empty vector
    }

    return vec->data;
}

/*
 @brief Gets a const pointer to the first element in the vector. Cannot be used to modify the element.
 @param vec A pointer to the vector from which to get the element.
 @return A constant pointer to the first element in the vector, or NULL if the vector is NULL or empty.
 @attention The returned pointer is a generic. You should cast it to the appropriate type before using it. The pointer will become invalid if the vector is resized or destroyed.
*/
static inline const void* vector_front_const(const vector_t* vec) {
    if (!vec || vec->size == 0) {
        return NULL; // Invalid vector or empty vector
    }

    return vec->data;
}

/*
 @brief Gets a pointer to the last element in the vector.
 @param vec A pointer to the vector from which to get the element.
 @return A pointer to the last element in the vector, or NULL if the vector is NULL or empty.
 @attention The returned pointer is a generic. You should cast it to the appropriate type before using it. The pointer will become invalid if the vector is resized or destroyed.
*/
static inline void* vector_back(vector_t* vec) {
    if (!vec || vec->size == 0) {
        return NULL; // Invalid vector or empty vector
    }

    return (char*)vec->data + ((vec->size - 1) * vec->element_size);
}

/*
 @brief Gets a const pointer to the last element in the vector. Cannot be used to modify the element.
 @param vec A pointer to the vector from which to get the element.
 @return A constant pointer to the last element in the vector, or NULL if the vector is NULL or empty.
 @attention The returned pointer is a generic. You should cast it to the appropriate type before using it. The pointer will become invalid if the vector is resized or destroyed.
*/
static inline const void* vector_back_const(const vector_t* vec) {
    if (!vec || vec->size == 0) {
        return NULL; // Invalid vector or empty vector
    }

    return (const char*)vec->data + ((vec->size - 1) * vec->element_size);
}

/*
 @brief Gets a pointer to the element at the specified index in the vector.
 @param vec A pointer to the vector from which to get the element.
 @param index The index of the element to get.
 @return A pointer to the element at the specified index, or NULL if the vector is NULL or the index is out of bounds.
 @attention The returned pointer is a generic. You should cast it to the appropriate type before using it. The pointer will become invalid if the vector is resized or destroyed.
 @attention For most read-only operations, consider using vector_get_const() instead, which returns a const pointer to the element.
*/
static inline void* vector_get(vector_t* vec, size_t index) {
    if (!vec || index >= vec->size) {
        return NULL; // Invalid vector or index out of bounds
    }

    return (char*)vec->data + (index * vec->element_size);
}

/*
 @brief Gets a constant pointer to the element at the specified index in the vector. Cannot be used to modify the element.
 @param vec A pointer to the vector from which to get the element.
 @param index The index of the element to get.
 @return A constant pointer to the element at the specified index, or NULL if the vector is NULL or the index is out of bounds.
 @attention The returned pointer is a generic. You should cast it to the appropriate type before using it. The pointer will become invalid if the vector is resized or destroyed.
*/
static inline const void* vector_get_const(const vector_t* vec, size_t index) {
    if (!vec || index >= vec->size) {
        return NULL; // Invalid vector or index out of bounds
    }

    return (const char*)vec->data + (index * vec->element_size);
}

/*
 @brief Gets the size of the vector.
 @param vec A pointer to the vector whose size is to be retrieved.
 @return The size of the vector, or 0 if the vector is NULL.
*/
static inline size_t vector_size(const vector_t* vec) {
    if (!vec) {
        return 0; // Invalid vector
    }

    return vec->size;
}

/*
 @brief Gets the capacity of the vector.
 @param vec A pointer to the vector whose capacity is to be retrieved.
 @return The capacity of the vector, or 0 if the vector is NULL.
*/
static inline size_t vector_capacity(const vector_t* vec) {
    if (!vec) {
        return 0; // Invalid vector
    }

    return vec->capacity;
}

/*
 @brief Gets the size of each element in the vector.
 @param vec A pointer to the vector whose element size is to be retrieved.
 @return The size of each element in the vector, or 0 if the vector is NULL.
*/
static inline size_t vector_element_size(const vector_t* vec) {
    if (!vec) {
        return 0; // Invalid vector
    }

    return vec->element_size;
}

/*
 @brief Gets a pointer to the underlying C array of the vector. Cannot modify the vector through this pointer.
 @param vec A pointer to the vector whose underlying array is to be retrieved.
 @return A pointer to the underlying C array, or NULL if the vector is NULL.
 @attention The returned pointer is a generic. You should cast it to the appropriate type before using it. The pointer will become invalid if the vector is resized or destroyed.
*/
static inline const void* vector_as_c_array(const vector_t* vec) {
    if (!vec) {
        return NULL; // Invalid vector
    }

    return vec->data;
}

/*
 @brief Gets a pointer to the underlying C array of the vector. Can be used to modify the vector's elements.
 @param vec A pointer to the vector whose underlying array is to be retrieved.
 @return A pointer to the underlying C array, or NULL if the vector is NULL.
 @attention The returned pointer is a generic. You should cast it to the appropriate type before using it. The pointer will become invalid if the vector is resized or destroyed.
*/
static inline void* vector_as_c_array_mutable(vector_t* vec) {
    if (!vec) {
        return NULL; // Invalid vector
    }

    return vec->data;
}

/*
 @brief Moves a vector to another vector, transferring ownership of the data. The destination vector will take ownership of the source vector's data, its element size and its destructor.
 @param dest A pointer to the destination vector.
 @param src A pointer to the pointer holding the source vector. It will be set to NULL.
 @return 0 on success, -1 if the destination vector is NULL, the source pointer is NULL, the source vector is NULL, or both vectors share the same data pointer.
 @attention After calling this function, the source vector will be freed (excluding data) and the source pointer will be set to NULL, so it cannot be used again.
 @attention The destination vector's existing elements will be destroyed (using the destination's own destructor, if it has one) and its data freed. Ensure that you do not need the existing data before calling this function.
 @attention Moving a vector onto itself is a no-op and reports success, leaving the vector untouched.
 @attention Two vectors can only share a data pointer if a vector_t was duplicated by copying the struct, which is never valid. The move is refused rather than freeing a buffer the source still points at.
*/
static inline int vector_move(vector_t* dest, vector_t** src) {
    if (!dest || !src || !*src) {
        return -1; // Invalid vectors
    }

    if (dest == *src) {
        return 0; // Moving to itself, no action needed
    }

    if (dest->data == (*src)->data) {
        return -1; // Refuse to move a vector onto another vector that shares its data
                   // Realistically, this is unreachable via legal use, but... safety.
                   // Or something. You WILL trigger this if you do shallow copies. DO NOT!
    }

    // Destroy the destination vector's existing elements with its own destructor,
    // then free its data, as it is about to be replaced
    vector_clear(dest);
    free(dest->data);

    // Transfer ownership of the source vector's data to the destination vector
    dest->size = (*src)->size;
    dest->capacity = (*src)->capacity;
    dest->element_size = (*src)->element_size;
    dest->data = (*src)->data;
    dest->destructor = (*src)->destructor; // The elements keep the cleanup they came with

    // Reset the source vector to an empty state
    (*src)->size = 0;
    (*src)->capacity = 0;
    (*src)->element_size = 0;
    (*src)->data = NULL;
    (*src)->destructor = NULL;

    free(*src); // Free the source vector structure, but not its data (ownership transferred)
    *src = NULL;

    return 0; // Success
}

/*
 @brief Creates a deep copy of the vector, including its data.
 @param vec A pointer to the vector to be copied.
 @return A pointer to the newly created deep copy of the vector, or NULL if allocation fails, if the input vector is NULL, or if the input vector has a destructor set.
 @attention The returned vector must be destroyed with vector_destroy() to free its memory. Failing to do so will result in a memory leak.
 @attention Vectors with a destructor cannot be deep copied and this function will return NULL for them. The elements are copied byte for byte, so any memory they own would end up owned by both vectors and freed twice. If you need to copy such a vector, do it by hand: create a new vector and push copies of the elements into it yourself.
*/
static inline vector_t* vector_deep_copy(const vector_t* vec) {
    if (!vec) {
        return NULL; // Invalid vector
    }

    if (vec->destructor) {
        return NULL; // Refuse to byte-copy elements that own memory
    }

    vector_t* new_vec = vector_create(vec->element_size);
    if (!new_vec) {
        return NULL; // Allocation failed
    }

    int r = vector_reserve(new_vec, vec->capacity);
    if (r != 0) {
        vector_destroy(&new_vec);
        return NULL; // Allocation failed
    }

    if (vec->size > 0) {
        memcpy(new_vec->data, vec->data, vec->size * vec->element_size);
        new_vec->size = vec->size;
    } // vector_create() already sets size to 0 by default

    return new_vec;
}

/*
 @brief Destroys the vector and frees its memory.
 @param vec A pointer to the pointer holding the vector to be destroyed. It will be set to NULL.
 @return 0 on success, or if the vector was already NULL. -1 only if the pointer itself is NULL.
 @attention After calling this function, the vector pointer should not be used again (It will be set to NULL). Accessing it after destruction will lead to undefined behavior.
 @attention Only the pointer you pass in is set to NULL. Copies of that pointer held elsewhere are left dangling and must not be used.
 @attention If stored elements own memory of their own, set a destructor with vector_set_destructor() to have it cleaned up here. Otherwise, the vector will only free the memory allocated for the data array and the vector structure itself, but not any dynamically allocated memory within the elements.
*/
static inline int vector_destroy(vector_t** vec) {
    if (!vec) {
        return -1; // Invalid pointer
    }

    if (!(*vec)) {
        return 0; // Already NULL, nothing to destroy
    }

    vector_t* target = *vec;

    if (target->destructor) {
        for (size_t i = 0; i < target->size; ++i) {
            void* element = (char*)target->data + (i * target->element_size);
            target->destructor(element); // Call the destructor for each element
        }
    }

    free(target->data);
    free(target);
    *vec = NULL; // Set the pointer to NULL to avoid dangling references

    return 0; // Success
}

#endif // VECTOR_H
