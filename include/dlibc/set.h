/*
set.h - Sets for C
Version: 1.0.0

Description:
Header-only library for sets in C. Memory safe against the mistakes it can
detect, but not thread-safe (you must implement your own synchronization
mechanisms).
Designed for C23 onwards, but should be compatible with older standards.

A set is backed by the same kind of contiguous array as a vector, but it holds
every element at most once. Elements are compared with the set's equality rule,
described further down.

Guarantees:
 - Every element in the set is unique with respect to the set's equality rule.
   set_insert() reports 1 instead of adding a second copy.
 - A set_t must never be duplicated by copying the struct. Two set_t values
   sharing one data pointer both believe they own it, and whichever is destroyed
   or moved from first leaves the other holding freed memory. Copying the pointer
   (set_t* b = a;) is fine - that is one set with two names, and it must be
   destroyed exactly once.
 - To hand a set's data to another set, use set_move(). The source is emptied,
   freed and its pointer set to NULL, so there is only ever one owner.
 - To get a second, independent set with the same contents, use set_deep_copy(),
   which copies the data as well as the set itself. Deep copies are only
   available for sets without an element destructor, as a byte-wise copy of
   owning elements would result in a double free.
 - No gaps in the raw data array.
 - When a set is resized, the data is reallocated to a new memory location, so
   any pointers into the old data are invalidated. There is no way to preserve
   them; re-fetch with set_get_const() after the resize.
 - Order of elements is NOT preserved. A set is unordered, and removing an
   element moves the last element into the slot that was freed, so an element's
   index is only stable until the next removal. Indices exist to let you walk
   the set, not to identify an element - use the element's value for that.
 - It is safe to pass a pointer into a set's own data as the source element
   of set_insert() and set_remove(). The library detects this and either
   resolves it to an index before anything can invalidate it, or copies the
   value before it can be overwritten. On a set with an element destructor
   set_insert() rejects such a pointer with -1 instead, as a byte-wise copy
   would leave two slots owning the same memory. To duplicate an owning
   element, copy what it owns yourself and insert that.
 - set_take() and set_take_at() transfer ownership of the element to the caller,
   and do not call the destructor on it. It is the caller's responsibility to
   free whatever the element owns.

Element equality:
A set decides whether two elements are the same with its comparator, set through
set_set_comparator(). The comparator follows the memcmp() convention: it returns
0 when the two elements are equal, and anything else when they are not. Only
equality is used, so the sign of a nonzero result does not matter and strcmp()
may be handed over directly.

Without a comparator the set falls back to comparing the raw bytes of the
elements, i.e. memcmp(a, b, element_size). That is the right answer for integers
and other plain scalar types, but it is wrong more often than it looks:
 - For pointer elements such as char*, it compares the pointers themselves, so
   two distinct buffers holding the same string count as different elements.
 - For structs it also compares the padding between members, which the compiler
   never initializes. Two structs with identical members can compare unequal
   because of leftover garbage in the padding. Either give the set a comparator,
   or memset() your structs to 0 before filling them in.

Set the comparator before adding any elements. Setting it on a set that already
holds elements is allowed, but a comparator that is looser than the one in
effect before it may leave elements behind that it now considers equal. Call
set_dedupe() afterwards to restore uniqueness. Until you do, the set is not
unique under its own rule, and set_is_equal() may report two sets as equal when
they are not.

Two sets may only be combined or compared when they use the same comparator,
which is decided by comparing the function pointers themselves. Give your
comparator external linkage. A comparator declared static in a shared header
gets a distinct address in every translation unit, so sets built in different
translation units will silently refuse to combine.

Element destructors:
A set may be given an element destructor with set_set_destructor(). It is
called for every element that leaves the set, i.e. by set_remove(),
set_remove_at(), set_dedupe() (on the duplicates it drops), set_clear()
and set_destroy().

The destructor is NOT called by set_take() or set_take_at(), as these functions
transfer ownership of the element to the caller. It is the caller's
responsibility to free whatever the element owns. They are the only way to get
an owning element back out of a set, as the set has no mutable element access.

The destructor receives a pointer to the element's slot inside the set's data
array - NOT a pointer that was returned by malloc(). It must free whatever the
element owns, and must never free the pointer it was handed, as that memory
belongs to the set. For a set of char* this means:

    void free_str(void* element) { free(*(char**)element); }

    int cmp_str(const void* a, const void* b) {
        return strcmp(*(char* const*)a, *(char* const*)b);
    }

    set_t* set = set_create(sizeof(char*));
    set_set_comparator(set, cmp_str); // Compare the strings, not the pointers
    set_set_destructor(set, free_str);
    // ... use the set ...
    set_destroy(&set); // Takes the address of your pointer, and NULLs it

Differences from vector.h:
The functions that only make sense on an ordered, duplicate-tolerant container
are deliberately absent:
 - There is no mutable element access (no set_get(), no mutable array view).
   Writing to an element in place could turn it into a copy of another element,
   which the set has no way to notice. Change an element by removing it and
   inserting the new value. To get an owning element back out of the set rather
   than destroying it, use set_take() or set_take_at().
 - There is no front(), back() or pop_back(), as an unordered container has no
   meaningful ends.
 - There is no insertion or assignment by index, as a position in the array is
   not something a set lets you choose.

Performance:
The uniqueness check is a linear scan, so set_insert(), set_find(),
set_contains() and set_remove() are O(n) in the number of elements. Removal
itself is O(1) once the element has been found, as the last element is moved
into the freed slot rather than everything after it being shifted down.

A warning regarding misuse:
The set manages its own buffer correctly: no leaks, no double frees of the data
array, no use of stale pointers internally. It cannot reason about what your
elements own — that is what the destructor is for, and it is your responsibility
to set one and to avoid duplicating owned pointers between slots. It also cannot
check that your comparator is consistent; one that reports an element as unequal
to itself will let duplicates in.

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

#ifndef SET_H
#define SET_H

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define SET_NPOS ((size_t)-1) // Returned by set_find() when the element is not in the set

typedef void (*set_destructor_t)(void* element); // Function pointer type for element destructor
typedef int (*set_comparator_t)(const void* a, const void* b); // Function pointer type for element comparator, returns 0 when equal

/*
 A set. Do not construct, copy or modify this struct directly - the fields are
 visible only because the functions below are inline. Create sets with
 set_create() and use the accessors. A set_t that did not come from set_create()
 is not a valid set, and the functions will reject it where they can and
 misbehave where they cannot.
*/
typedef struct {
    size_t size; // Number of elements in the set
    size_t capacity; // Allocated capacity of the set
    size_t element_size; // Size of each element in the set
    void *data; // Pointer to the raw data array
    set_destructor_t destructor; // Function pointer to the element destructor
    set_comparator_t comparator; // Function pointer to the element comparator
} set_t;

/* Forward declarations */
static inline int set_is_aliased(const set_t* set, const void* ptr);
static inline int set_elements_equal(const set_t* set, const void* a, const void* b);
static inline set_t* set_create(size_t element_size);
static inline int set_set_destructor(set_t* set, set_destructor_t destructor);
static inline set_destructor_t set_get_destructor(const set_t* set);
static inline int set_set_comparator(set_t* set, set_comparator_t comparator);
static inline set_comparator_t set_get_comparator(const set_t* set);
static inline int set_reserve(set_t* set, size_t new_capacity);
static inline int set_grow(set_t* set);
static inline int set_prune(set_t* set);
static inline int set_insert(set_t* set, const void* element);
static inline size_t set_find(const set_t* set, const void* element);
static inline int set_contains(const set_t* set, const void* element);
static inline int set_remove(set_t* set, const void* element);
static inline int set_remove_at(set_t* set, size_t index);
static inline int set_take(set_t* set, const void* element, void* out);
static inline int set_take_at(set_t* set, size_t index, void* out);
static inline int set_dedupe(set_t* set);
static inline int set_clear(set_t* set);
static inline int set_is_empty(const set_t* set);
static inline const void* set_get_const(const set_t* set, size_t index);
static inline size_t set_size(const set_t* set);
static inline size_t set_capacity(const set_t* set);
static inline size_t set_element_size(const set_t* set);
static inline const void* set_as_c_array(const set_t* set);
static inline int set_move(set_t* dest, set_t** src);
static inline set_t* set_deep_copy(const set_t* set);
static inline int set_is_compatible(const set_t* a, const set_t* b);
static inline set_t* set_union(const set_t* a, const set_t* b);
static inline set_t* set_intersection(const set_t* a, const set_t* b);
static inline set_t* set_difference(const set_t* a, const set_t* b);
static inline int set_is_subset(const set_t* a, const set_t* b);
static inline int set_is_equal(const set_t* a, const set_t* b);
static inline int set_destroy(set_t** set);

/*
 @brief Checks whether a pointer points inside the set's own data array. Used internally to make the write functions safe against self-referential input.
 @param set A pointer to the set to check against.
 @param ptr The pointer to check.
 @return 1 if the pointer lies within the set's allocated buffer, 0 otherwise.
 @attention This is an internal helper. You are not expected to call it directly, but it is harmless if you do.
 @attention This is a best-effort check, not a portable guarantee. Comparing pointers into different objects is not defined by the standard, and going through uintptr_t is the usual practical workaround rather than a strictly correct one - the conversion is implementation-defined and uintptr_t is an optional type. It does the right thing on every mainstream platform.
*/
static inline int set_is_aliased(const set_t* set, const void* ptr) {
    if (!set || !ptr || !set->data) {
        return 0; // Nothing to alias
    }

    // Compared as integers rather than pointers, as comparing pointers into
    // different objects is not well defined. See the note above.
    uintptr_t base = (uintptr_t)set->data;
    uintptr_t end = base + (set->capacity * set->element_size);
    uintptr_t target = (uintptr_t)ptr;

    return target >= base && target < end;
}

/*
 @brief Checks whether two elements are equal according to the set's equality rule. Used internally by every function that has to look an element up.
 @param set A pointer to the set whose equality rule is to be applied.
 @param a A pointer to the first element.
 @param b A pointer to the second element.
 @return 1 if the elements are equal, 0 otherwise (including if any argument is NULL).
 @attention This is an internal helper. You are not expected to call it directly, but it is harmless if you do.
 @attention If the set has a comparator, it is called and its result compared against 0. Otherwise the raw bytes of the elements are compared with memcmp().
*/
static inline int set_elements_equal(const set_t* set, const void* a, const void* b) {
    if (!set || !a || !b) {
        return 0; // Nothing to compare
    }

    if (set->comparator) {
        return set->comparator(a, b) == 0;
    }

    return memcmp(a, b, set->element_size) == 0;
}

/*
 @brief Creates a new set with the specified element size.
 @param element_size The size of each element in the set. Call with sizeof(type)
 @return A pointer to the newly created set, or NULL if allocation fails. The set lives on the heap.
 @attention The set must be destroyed with set_destroy() to free its memory. Failing to do so will result in a memory leak.
 @attention The set's initial capacity is set to 10. If you want to change the initial capacity globally, set the DLIBC_SET_INITIAL_CAPACITY macro before including this header file. The initial capacity must be greater than 0.
 @attention The set's element size must be greater than 0. If you pass 0, the function will return NULL.
 @attention The set is created without an element comparator, so elements are compared byte for byte. If your elements are pointers or structs with padding, set one with set_set_comparator().
 @attention The set is created without an element destructor. If your elements own memory of their own, set one with set_set_destructor().
*/
static inline set_t* set_create(size_t element_size) {
    if (element_size == 0) {
        return NULL; // Invalid element size
    }

    #ifndef DLIBC_SET_INITIAL_CAPACITY
        size_t initial_capacity = 10; // Default initial capacity
    #else
        #if DLIBC_SET_INITIAL_CAPACITY <= 0
            #error "DLIBC_SET_INITIAL_CAPACITY must be greater than 0"
        #endif
        size_t initial_capacity = DLIBC_SET_INITIAL_CAPACITY; // Use the macro if defined
    #endif

    if (SIZE_MAX / element_size < initial_capacity) {
        return NULL; // Prevent overflow
    }

    set_t* set = (set_t*)malloc(sizeof(set_t));
    if (!set) {
        return NULL; // Allocation failed
    }

    set->size = 0;
    set->capacity = initial_capacity;
    set->element_size = element_size;
    set->destructor = NULL; // Initialize destructor to NULL
    set->comparator = NULL; // Initialize comparator to NULL, falling back to memcmp()
    set->data = malloc(set->capacity * set->element_size);
    if (!set->data) {
        free(set);
        return NULL; // Allocation failed
    }

    return set;
}

/*
 @brief Sets the destructor function for the set's elements. This function will be called on each element as it leaves the set, allowing for custom cleanup of dynamically allocated memory within the elements.
 @param set A pointer to the set for which to set the destructor.
 @param destructor A function pointer to the destructor function, or NULL to remove the current one. The function should take a single void* parameter, which will be a pointer to the element to be destroyed.
 @return 0 on success, -1 if the set is NULL.
 @attention If you do not set a destructor function, the set will not automatically free any dynamically allocated memory within its elements when it is destroyed. You must ensure that you free any such memory manually before destroying the set to avoid memory leaks.
 @attention The destructor is passed a pointer to the element's slot inside the set's data array, not a pointer returned by malloc(). It must free what the element owns and must never free the pointer it is given. For a set of char*, the destructor body is free(*(char**)element);
 @attention This assumes that your destructor function is valid and safe to call. If it isn't, bad things will happen and it will not be nice to watch.
 @attention Set the destructor before adding any elements. Setting it on a set that already holds elements is allowed, but elements that were removed beforehand will not have been destroyed.
*/
static inline int set_set_destructor(set_t* set, set_destructor_t destructor) {
    if (!set) {
        return -1; // Invalid set
    }

    set->destructor = destructor;
    return 0; // Success
}

/*
 @brief Gets the destructor function currently set on the set.
 @param set A pointer to the set whose destructor is to be retrieved.
 @return The set's destructor function pointer, or NULL if the set is NULL or has no destructor set.
*/
static inline set_destructor_t set_get_destructor(const set_t* set) {
    if (!set) {
        return NULL; // Invalid set
    }

    return set->destructor;
}

/*
 @brief Sets the comparator function for the set's elements. This function decides whether two elements are the same, and therefore which elements the set will refuse as duplicates.
 @param set A pointer to the set for which to set the comparator.
 @param comparator A function pointer to the comparator function, or NULL to fall back to comparing the raw bytes of the elements. The function takes two const void* parameters and returns 0 when they are equal, anything else when they are not.
 @return 0 on success, -1 if the set is NULL.
 @attention Only equality is used, so the sign of a nonzero result does not matter. strcmp() and memcmp() style comparators both work as-is.
 @attention Without a comparator, elements are compared with memcmp() over element_size bytes. That compares pointers rather than what they point to, and includes the uninitialized padding inside structs. Pass a comparator for anything other than plain scalar types.
 @attention This assumes that your comparator function is valid and safe to call, and that it is consistent - equal elements must always compare equal. A comparator that does not report an element as equal to itself will let duplicates into the set.
 @attention Set the comparator before adding any elements. Setting it on a set that already holds elements is allowed, but a looser comparator may leave behind elements that it now considers equal. Call set_dedupe() afterwards to restore uniqueness.
*/
static inline int set_set_comparator(set_t* set, set_comparator_t comparator) {
    if (!set) {
        return -1; // Invalid set
    }

    set->comparator = comparator;
    return 0; // Success
}

/*
 @brief Gets the comparator function currently set on the set.
 @param set A pointer to the set whose comparator is to be retrieved.
 @return The set's comparator function pointer, or NULL if the set is NULL or has no comparator set (in which case elements are compared with memcmp()).
*/
static inline set_comparator_t set_get_comparator(const set_t* set) {
    if (!set) {
        return NULL; // Invalid set
    }

    return set->comparator;
}

/*
 @brief Ensures the set has room for at least the specified number of elements.
 @param set A pointer to the set for which to reserve space.
 @param new_capacity The minimum capacity the set should have.
 @return 0 on success, -1 if the set is NULL, its element size is zero, or allocation fails.
 @attention Capacity never decreases. If the set already has room, this is a no-op and reports success.
 @attention If the buffer does grow, it is reallocated, so any pointers into the set's data are invalidated. Use set_prune() to release unused memory.
*/
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 10
// GCC's -fanalyzer loses track of new_data once it is stored through a set that the caller
// reached via a struct member (holder->set), and reports the grown buffer as leaked when this
// returns. The buffer is owned by set->data and released by set_destroy(), so the report is a
// false positive. Scoped to this function only, so genuine leaks elsewhere are still reported.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wanalyzer-malloc-leak"
#endif
static inline int set_reserve(set_t* set, size_t new_capacity) {
    if (!set) {
        return -1; // Invalid set
    }

    if (set->element_size == 0) {
        return -1; // Not a usable set; also guards the division below
    }

    if (new_capacity <= set->capacity) {
        return 0; // Already have the room. Never shrink
    }

    if (new_capacity > SIZE_MAX / set->element_size) {
        return -1; // Prevent overflow
    }

    void* new_data = realloc(set->data, new_capacity * set->element_size);
    if (!new_data) {
        return -1; // Allocation failed
    }

    set->data = new_data;
    set->capacity = new_capacity;

    return 0; // Success
}
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 10
#pragma GCC diagnostic pop
#endif

/*
 @brief Grows the set's capacity to make room for at least one more element. Used internally by the functions that add elements.
 @param set A pointer to the set to grow.
 @return 0 on success, -1 if the set is NULL or allocation fails.
 @attention This is an internal helper. You are not expected to call it directly.
*/
static inline int set_grow(set_t* set) {
    if (!set) {
        return -1; // Invalid set
    }

    if (set->capacity > SIZE_MAX / 2) {
        return -1; // Prevent overflow
    }

    size_t new_capacity = set->capacity > 0 ? set->capacity * 2 : 1; // Double the capacity, or set to 1 if it was 0
    return set_reserve(set, new_capacity);
}

/*
 @brief Prunes the set to free unused memory. If the set's size is less than its capacity, this function will reallocate the set's data array to match its size, freeing any unused memory.
 @param set A pointer to the set to be pruned.
 @return 0 on success, -1 if the set is NULL or allocation fails.
 @attention After calling this function, the set's capacity will be equal to its size. Any pointers into the set's data are invalidated; re-fetch them with set_get_const() afterwards.
 @attention Capacity may never drop below 1, even if the set is empty.
*/
static inline int set_prune(set_t* set) {
    if (!set) {
        return -1; // Invalid set
    }

    if (set->element_size == 0) {
        return -1; // Not a usable set; also guards the division below
    }

    if (set->size < set->capacity) {
        size_t new_capacity = set->size > 0 ? set->size : 1; // Ensure capacity is at least 1

        if (new_capacity > SIZE_MAX / set->element_size) {
            return -1; // Prevent overflow
        }

        void* new_data = realloc(set->data, new_capacity * set->element_size);
        if (!new_data) {
            return -1; // Allocation failed
        }

        set->data = new_data;
        set->capacity = new_capacity;
    }

    return 0; // Success
}

/*
 @brief Finds the index of an element in the set.
 @param set A pointer to the set to search.
 @param element A pointer to the element to look for. It is compared against the set's elements with the set's equality rule.
 @return The index of the element, or SET_NPOS if the set is NULL, the element is NULL, or the element is not in the set.
 @attention This is a linear scan, so it is O(n) in the number of elements.
 @attention The returned index is only valid until the next removal, as removing an element moves the last element into the slot that was freed.
*/
static inline size_t set_find(const set_t* set, const void* element) {
    if (!set || !element) {
        return SET_NPOS; // Invalid set or element
    }

    for (size_t i = 0; i < set->size; ++i) {
        const void* slot = (const char*)set->data + (i * set->element_size);
        if (set_elements_equal(set, slot, element)) {
            return i; // Found it
        }
    }

    return SET_NPOS; // Not in the set
}

/*
 @brief Checks whether an element is in the set.
 @param set A pointer to the set to search.
 @param element A pointer to the element to look for. It is compared against the set's elements with the set's equality rule.
 @return 1 if the element is in the set, 0 if it is not, the set is NULL, or the element is NULL.
 @attention This is a linear scan, so it is O(n) in the number of elements. If you need the element's index as well, use set_find() instead and avoid searching twice.
*/
static inline int set_contains(const set_t* set, const void* element) {
    return set_find(set, element) != SET_NPOS;
}

/*
 @brief Adds an element to the set, unless an equal element is already in it.
 @param set A pointer to the set to which the element will be added.
 @param element A pointer to the element to be added. The element will be copied into the set's data array.
 @return 0 if the element was added, 1 if an equal element was already in the set and nothing changed, -1 if the set is NULL, the element is NULL, if the element is an owning element that is aliased in the set and a destructor is set, or if reservation fails.
 @attention The element must be a pointer to a valid memory location containing data of the same type as the set's element type. The set will make a copy of the data, so the original element can be modified or freed after this function returns.
 @attention The element is placed at an unspecified position. Do not assume it lands at the end, and do not assume it stays where it lands.
 @attention The element may point into the set's own data array on a set without a destructor. An element-aligned pointer, i.e. one that set_get_const() returned, is by definition already in the set, so a consistent comparator makes this return 1 without touching anything. A pointer partway into an element, or a comparator that does not report an element as equal to itself, reaches the copy instead; the value is followed through any reallocation and copied with memmove(), so it survives either way.
 @attention Take note of the return value. A return of 1 means your value was NOT stored, and if it owns memory, you are still responsible for freeing it.
*/
static inline int set_insert(set_t* set, const void* element) {
    if (!set || !element) {
        return -1; // Invalid set or element
    }

    if (set->destructor && set_is_aliased(set, element)) {
        return -1; // Refuse to insert an owning element that lives inside the set
    }

    if (set_find(set, element) != SET_NPOS) {
        return 1; // Already in the set, nothing to do
    }

    if (set->size >= set->capacity) {
        // If the element lives inside our own data array, remember where it sits
        // so that we can find it again after the data has been reallocated.
        // A sane comparator will have reported it as a duplicate above, but a
        // broken one must not be allowed to cause a use-after-free
        size_t offset = 0;
        int aliased = set_is_aliased(set, element);
        if (aliased) {
            // Subtracted as integers, for the same reason set_is_aliased() compares them that
            // way. Pointer subtraction is only defined within one object, which the check above
            // guarantees, but a static analyzer cannot see through the integer comparison and
            // reports the subtraction as undefined behaviour
            offset = (size_t)((uintptr_t)element - (uintptr_t)set->data);
        }

        if (set_grow(set) != 0) {
            return -1; // Failed to grow the set
        }

        if (aliased) {
            element = (const char*)set->data + offset;
        }
    }

    // Copy the new element into the set's data array. memmove, as an interior
    // pointer into our own data slips past the duplicate check above and may
    // overlap the destination slot
    memmove((char*)set->data + (set->size * set->element_size), element, set->element_size);
    set->size++;

    return 0; // Success
}

/*
 @brief Removes the element at the specified index from the set.
 @param set A pointer to the set from which the element will be removed.
 @param index The index of the element to be removed.
 @return 0 on success, -1 if the set is NULL or the index is out of bounds.
 @attention After calling this function, the set's size will be reduced by one. The memory occupied by the removed element will not be freed automatically unless a destructor is set. Use set_take_at() to remove the element at the specified index and NOT call the destructor on it (hands ownership to the caller).
 @attention The last element is moved into the freed slot, so it is O(1), but the index of that last element changes. If you are removing elements while walking the set by index, do not advance the index after a removal, or you will skip the element that was moved in.
*/
static inline int set_remove_at(set_t* set, size_t index) {
    if (!set || index >= set->size) {
        return -1; // Invalid set or index out of bounds
    }

    void* slot = (char*)set->data + (index * set->element_size);
    if (set->destructor) {
        set->destructor(slot); // Call the destructor for the element to be removed
    }

    // Move the last element into the freed slot, as the order is not guaranteed
    size_t last = set->size - 1;
    if (index != last) {
        // memcpy, as the two slots cannot overlap when they are not the same slot
        memcpy(slot, (const char*)set->data + (last * set->element_size), set->element_size);
    }

    set->size--;
    return 0; // Success
}

/*
 @brief Removes an element from the set.
 @param set A pointer to the set from which the element will be removed.
 @param element A pointer to the element to be removed. It is compared against the set's elements with the set's equality rule.
 @return 0 if the element was removed, 1 if it was not in the set and nothing changed, -1 if the set is NULL or the element is NULL.
 @attention After a successful call, the set's size will be reduced by one. The memory occupied by the removed element will not be freed automatically unless a destructor is set. Use set_take() to remove the element and NOT call the destructor on it (hands ownership to the caller).
 @attention The last element is moved into the freed slot, so any index you were holding onto may now refer to a different element.
 @attention The element may point into the set's own data array, i.e. one returned by set_get_const(). Its index is resolved before anything is destroyed, so the pointer cannot be left dangling underneath this function.
*/
static inline int set_remove(set_t* set, const void* element) {
    if (!set || !element) {
        return -1; // Invalid set or element
    }

    // Resolve the index first, so that a pointer into our own data cannot be
    // invalidated by the destructor before we are done with it
    size_t index = set_find(set, element);
    if (index == SET_NPOS) {
        return 1; // Not in the set, nothing to do
    }

    return set_remove_at(set, index);
}

/*
 @brief Removes the element at the specified index and transfers ownership of it to the caller.
 @param set A pointer to the set from which the element will be taken.
 @param index The index of the element to take.
 @param out A pointer to a buffer of at least set_element_size(set) bytes, which the element is copied into.
 @return 0 on success, -1 if the set is NULL, out is NULL, out points into the set's own data, or the index is out of bounds.
 @attention The element destructor is deliberately NOT called. Whatever the element owns becomes the caller's responsibility to free.
 @attention out must not point into the set's own data array. Doing so would leave two slots owning the same memory and is rejected with -1.
 @attention As with set_remove_at(), the last element is moved into the freed slot, so the index of that last element changes.
 @attention This and set_take() are the only way to get an owning element out of a set with its contents intact, as the set has no mutable element access.
*/
static inline int set_take_at(set_t* set, size_t index, void* out) {
    if (!set || index >= set->size || !out) {
        return -1; // Invalid set, index out of bounds, or output pointer is NULL
    }

    if (set_is_aliased(set, out)) {
        return -1; // Refuse to take an element into a pointer that lives inside the set
    }

    void* slot = (char*)set->data + (index * set->element_size);
    memcpy(out, slot, set->element_size); // Hand the element over, destructor deliberately not called

    // Move the last element into the freed slot, as the order is not guaranteed
    size_t last = set->size - 1;
    if (index != last) {
        // memcpy, as the two slots cannot overlap when they are not the same slot
        memcpy(slot, (const char*)set->data + (last * set->element_size), set->element_size);
    }

    set->size--;
    return 0; // Success
}

/*
 @brief Removes an element from the set and transfers ownership of it to the caller.
 @param set A pointer to the set from which the element will be taken.
 @param element A pointer to the element to look for. It is compared against the set's elements with the set's equality rule.
 @param out A pointer to a buffer of at least set_element_size(set) bytes, which the element is copied into.
 @return 0 if the element was taken, 1 if it was not in the set and nothing changed, -1 if the set is NULL, the element is NULL, out is NULL, or out points into the set's own data.
 @attention The element destructor is deliberately NOT called. Whatever the element owns becomes the caller's responsibility to free.
 @attention The element may point into the set's own data array, i.e. one returned by set_get_const(). Its index is resolved before anything is copied, so the pointer cannot be left dangling underneath this function.
 @attention out must not point into the set's own data array. Doing so would leave two slots owning the same memory and is rejected with -1.
*/
static inline int set_take(set_t* set, const void* element, void* out) {
    if (!set || !element || !out) {
        return -1; // Invalid set, element, or output pointer
    }

    if (set_is_aliased(set, out)) {
        return -1; // Refuse to take an element into a pointer that lives inside the set
    }

    // Resolve the index first, exactly as set_remove() does, so that a pointer
    // into our own data cannot be invalidated before we are done with it
    size_t index = set_find(set, element);
    if (index == SET_NPOS) {
        return 1; // Not in the set, nothing to do
    }

    return set_take_at(set, index, out);
}

/*
 @brief Removes duplicate elements from the set, restoring uniqueness under the set's current equality rule.
 @param set A pointer to the set to be deduplicated.
 @return 0 on success, -1 if the set is NULL.
 @attention This is only needed after set_set_comparator() has been called on a set that already held elements, as a looser comparator may consider elements equal that the previous rule did not. A set that has only ever been filled through set_insert() is already unique and this will do nothing.
 @attention Of any group of equal elements, the one with the lowest index is kept and the rest are removed. If a destructor is set, it is called on each of the removed elements.
 @attention This is O(n^2) in the number of elements.
*/
static inline int set_dedupe(set_t* set) {
    if (!set) {
        return -1; // Invalid set
    }

    for (size_t i = 0; i + 1 < set->size; ++i) {
        const void* kept = (const char*)set->data + (i * set->element_size);

        size_t j = i + 1;
        while (j < set->size) {
            const void* candidate = (const char*)set->data + (j * set->element_size);
            if (set_elements_equal(set, kept, candidate)) {
                // The kept element is never moved by this, as it sits below j and
                // therefore below the last element that gets moved into the hole
                set_remove_at(set, j);
                continue; // Do not advance, as slot j now holds what used to be the last element
            }

            ++j;
        }
    }

    return 0; // Success
}

/*
 @brief Clears all elements from the set.
 @param set A pointer to the set to be cleared.
 @return 0 on success, -1 if the set is NULL.
 @attention After calling this function, the set's size will be zero. The memory occupied by the elements will not be freed automatically unless a destructor is set.
 @attention The set's capacity is left untouched, so the set may be refilled without reallocating. Call set_prune() afterwards if you want the memory back.
*/
static inline int set_clear(set_t* set) {
    if (!set) {
        return -1; // Invalid set
    }

    if (set->destructor) {
        for (size_t i = 0; i < set->size; ++i) {
            void* element = (char*)set->data + (i * set->element_size);
            set->destructor(element); // Call the destructor for each element
        }
    }

    set->size = 0;
    return 0; // Success
}

/*
 @brief Checks if the set is empty.
 @param set A pointer to the set to check.
 @return 1 if the set is empty, 0 if it is not empty.
 @attention If the set is NULL, this function will return 1 (is empty) to indicate that the set is invalid.
*/
static inline int set_is_empty(const set_t* set) {
    if (!set) {
        return 1; // Invalid set
    }

    return set->size == 0;
}

/*
 @brief Gets a constant pointer to the element at the specified index in the set. Cannot be used to modify the element.
 @param set A pointer to the set from which to get the element.
 @param index The index of the element to get.
 @return A constant pointer to the element at the specified index, or NULL if the set is NULL or the index is out of bounds.
 @attention The returned pointer is a generic. You should cast it to the appropriate type before using it. The pointer will become invalid if the set is resized or destroyed.
 @attention Indices exist so that you can walk the set, from 0 up to set_size(). They do not identify an element: removing an element moves the last element into the freed slot, and inserting may reallocate.
 @attention There is no mutable counterpart to this function by design. Writing to an element in place could turn it into a duplicate of another element without the set being able to notice. Remove the element and insert the new value instead.
*/
static inline const void* set_get_const(const set_t* set, size_t index) {
    if (!set || index >= set->size) {
        return NULL; // Invalid set or index out of bounds
    }

    return (const char*)set->data + (index * set->element_size);
}

/*
 @brief Gets the size of the set.
 @param set A pointer to the set whose size is to be retrieved.
 @return The size of the set, or 0 if the set is NULL.
*/
static inline size_t set_size(const set_t* set) {
    if (!set) {
        return 0; // Invalid set
    }

    return set->size;
}

/*
 @brief Gets the capacity of the set.
 @param set A pointer to the set whose capacity is to be retrieved.
 @return The capacity of the set, or 0 if the set is NULL.
*/
static inline size_t set_capacity(const set_t* set) {
    if (!set) {
        return 0; // Invalid set
    }

    return set->capacity;
}

/*
 @brief Gets the size of each element in the set.
 @param set A pointer to the set whose element size is to be retrieved.
 @return The size of each element in the set, or 0 if the set is NULL.
*/
static inline size_t set_element_size(const set_t* set) {
    if (!set) {
        return 0; // Invalid set
    }

    return set->element_size;
}

/*
 @brief Gets a pointer to the underlying C array of the set. Cannot modify the set through this pointer.
 @param set A pointer to the set whose underlying array is to be retrieved.
 @return A pointer to the underlying C array, or NULL if the set is NULL.
 @attention The returned pointer is a generic. You should cast it to the appropriate type before using it. The pointer will become invalid if the set is resized or destroyed.
 @attention The array holds set_size() elements with no gaps, but in no particular order.
 @attention There is no mutable counterpart to this function by design, for the same reason set_get_const() has none.
*/
static inline const void* set_as_c_array(const set_t* set) {
    if (!set) {
        return NULL; // Invalid set
    }

    return set->data;
}

/*
 @brief Moves a set to another set, transferring ownership of the data. The destination set will take ownership of the source set's data, its element size, its destructor and its comparator.
 @param dest A pointer to the destination set.
 @param src A pointer to the pointer holding the source set. It will be set to NULL.
 @return 0 on success, -1 if the destination set is NULL, the source pointer is NULL, the source set is NULL, or both sets share the same data pointer.
 @attention After calling this function, the source set will be freed (excluding data) and the source pointer will be set to NULL, so it cannot be used again.
 @attention The destination set's existing elements will be destroyed (using the destination's own destructor, if it has one) and its data freed. Ensure that you do not need the existing data before calling this function.
 @attention Moving a set onto itself is a no-op and reports success, leaving the set untouched.
 @attention Two sets can only share a data pointer if a set_t was duplicated by copying the struct, which is never valid. The move is refused rather than freeing a buffer the source still points at.
*/
static inline int set_move(set_t* dest, set_t** src) {
    if (!dest || !src || !*src) {
        return -1; // Invalid sets
    }

    if (dest == *src) {
        return 0; // Moving to itself, no action needed
    }

    if (dest->data == (*src)->data) {
        return -1; // Refuse to move a set onto another set that shares its data
                   // Realistically, this is unreachable via legal use, but... safety.
                   // Or something. You WILL trigger this if you do shallow copies. DO NOT!
    }

    // Destroy the destination set's existing elements with its own destructor,
    // then free its data, as it is about to be replaced
    set_clear(dest);
    free(dest->data);

    // Transfer ownership of the source set's data to the destination set
    dest->size = (*src)->size;
    dest->capacity = (*src)->capacity;
    dest->element_size = (*src)->element_size;
    dest->data = (*src)->data;
    dest->destructor = (*src)->destructor; // The elements keep the cleanup they came with
    dest->comparator = (*src)->comparator; // ...and the rule that made them unique

    // Reset the source set to an empty state
    (*src)->size = 0;
    (*src)->capacity = 0;
    (*src)->element_size = 0;
    (*src)->data = NULL;
    (*src)->destructor = NULL;
    (*src)->comparator = NULL;

    free(*src); // Free the source set structure, but not its data (ownership transferred)
    *src = NULL;

    return 0; // Success
}

/*
 @brief Creates a deep copy of the set, including its data.
 @param set A pointer to the set to be copied.
 @return A pointer to the newly created deep copy of the set, or NULL if allocation fails, if the input set is NULL, or if the input set has a destructor set.
 @attention The returned set must be destroyed with set_destroy() to free its memory. Failing to do so will result in a memory leak.
 @attention The copy inherits the original's comparator, so it enforces uniqueness by the same rule. It does not inherit a destructor, as sets with one cannot be copied here at all.
 @attention Sets with a destructor cannot be deep copied and this function will return NULL for them. The elements are copied byte for byte, so any memory they own would end up owned by both sets and freed twice. If you need to copy such a set, do it by hand: create a new set and insert copies of the elements into it yourself.
*/
static inline set_t* set_deep_copy(const set_t* set) {
    if (!set) {
        return NULL; // Invalid set
    }

    if (set->destructor) {
        return NULL; // Refuse to byte-copy elements that own memory
    }

    set_t* new_set = set_create(set->element_size);
    if (!new_set) {
        return NULL; // Allocation failed
    }

    new_set->comparator = set->comparator; // The copy keeps the rule that made the elements unique

    int r = set_reserve(new_set, set->capacity);
    if (r != 0) {
        set_destroy(&new_set);
        return NULL; // Allocation failed
    }

    if (set->size > 0) {
        // The source is already unique, so a byte-wise copy of it is too
        memcpy(new_set->data, set->data, set->size * set->element_size);
        new_set->size = set->size;
    } // set_create() already sets size to 0 by default

    return new_set;
}

/*
 @brief Checks whether two sets are compatible enough to be combined or compared. Used internally by the set operations.
 @param a A pointer to the first set.
 @param b A pointer to the second set.
 @return 1 if the sets may be combined, 0 otherwise.
 @attention This is an internal helper. You are not expected to call it directly, but it is harmless if you do.
 @attention Sets are compatible when neither is NULL, they hold elements of the same size, they use the same comparator, and neither has a destructor. The destructor requirement exists because the set operations copy elements byte for byte, exactly as set_deep_copy() does.
*/
static inline int set_is_compatible(const set_t* a, const set_t* b) {
    if (!a || !b) {
        return 0; // Invalid sets
    }

    if (a->element_size != b->element_size) {
        return 0; // Different element types
    }

    if (a->comparator != b->comparator) {
        return 0; // Different notions of equality, so the result would be ill-defined
    }

    if (a->destructor || b->destructor) {
        return 0; // Refuse to byte-copy elements that own memory
    }

    return 1; // Compatible
}

/*
 @brief Creates a new set containing every element that is in either of the two sets.
 @param a A pointer to the first set.
 @param b A pointer to the second set.
 @return A pointer to the newly created set, or NULL if either set is NULL, they are not compatible, or allocation fails.
 @attention The returned set must be destroyed with set_destroy() to free its memory. Failing to do so will result in a memory leak.
 @attention Both sets must hold elements of the same size, use the same comparator, and have no destructor. The elements are copied byte for byte, so any memory they own would end up owned by several sets and freed more than once. If your elements own memory, build the union by hand with set_insert() and copies you make yourself.
 @attention The inputs are left untouched.
*/
static inline set_t* set_union(const set_t* a, const set_t* b) {
    if (!set_is_compatible(a, b)) {
        return NULL; // Incompatible or invalid sets
    }

    set_t* result = set_create(a->element_size);
    if (!result) {
        return NULL; // Allocation failed
    }

    result->comparator = a->comparator;

    for (size_t i = 0; i < a->size; ++i) {
        if (set_insert(result, (const char*)a->data + (i * a->element_size)) < 0) {
            set_destroy(&result);
            return NULL; // Insertion failed
        }
    }

    for (size_t i = 0; i < b->size; ++i) {
        // Elements of b that are already in a report 1, which is not a failure
        if (set_insert(result, (const char*)b->data + (i * b->element_size)) < 0) {
            set_destroy(&result);
            return NULL; // Insertion failed
        }
    }

    return result;
}

/*
 @brief Creates a new set containing every element that is in both of the two sets.
 @param a A pointer to the first set.
 @param b A pointer to the second set.
 @return A pointer to the newly created set, or NULL if either set is NULL, they are not compatible, or allocation fails.
 @attention The returned set must be destroyed with set_destroy() to free its memory. Failing to do so will result in a memory leak.
 @attention Both sets must hold elements of the same size, use the same comparator, and have no destructor, for the same reason as set_union().
 @attention The inputs are left untouched.
*/
static inline set_t* set_intersection(const set_t* a, const set_t* b) {
    if (!set_is_compatible(a, b)) {
        return NULL; // Incompatible or invalid sets
    }

    set_t* result = set_create(a->element_size);
    if (!result) {
        return NULL; // Allocation failed
    }

    result->comparator = a->comparator;

    for (size_t i = 0; i < a->size; ++i) {
        const void* element = (const char*)a->data + (i * a->element_size);
        if (!set_contains(b, element)) {
            continue; // Only in a, so not in the intersection
        }

        if (set_insert(result, element) < 0) {
            set_destroy(&result);
            return NULL; // Insertion failed
        }
    }

    return result;
}

/*
 @brief Creates a new set containing every element that is in the first set but not in the second.
 @param a A pointer to the set to take elements from.
 @param b A pointer to the set of elements to leave out.
 @return A pointer to the newly created set, or NULL if either set is NULL, they are not compatible, or allocation fails.
 @attention The returned set must be destroyed with set_destroy() to free its memory. Failing to do so will result in a memory leak.
 @attention Both sets must hold elements of the same size, use the same comparator, and have no destructor, for the same reason as set_union().
 @attention This operation is not symmetric. set_difference(a, b) is not the same as set_difference(b, a).
 @attention The inputs are left untouched.
*/
static inline set_t* set_difference(const set_t* a, const set_t* b) {
    if (!set_is_compatible(a, b)) {
        return NULL; // Incompatible or invalid sets
    }

    set_t* result = set_create(a->element_size);
    if (!result) {
        return NULL; // Allocation failed
    }

    result->comparator = a->comparator;

    for (size_t i = 0; i < a->size; ++i) {
        const void* element = (const char*)a->data + (i * a->element_size);
        if (set_contains(b, element)) {
            continue; // In both, so not in the difference
        }

        if (set_insert(result, element) < 0) {
            set_destroy(&result);
            return NULL; // Insertion failed
        }
    }

    return result;
}

/*
 @brief Checks whether every element of the first set is also in the second set.
 @param a A pointer to the set that may be contained.
 @param b A pointer to the set that may contain it.
 @return 1 if every element of a is in b, 0 otherwise.
 @attention Both sets must hold elements of the same size and use the same comparator, or this returns 0. Unlike the set operations that build a new set, a destructor on either set is fine here, as nothing is copied.
 @attention If either set is NULL, this returns 0. It never returns a negative error code, so that it is safe to use directly in an if statement.
 @attention The empty set is a subset of every compatible set, so this returns 1 when a is empty.
*/
static inline int set_is_subset(const set_t* a, const set_t* b) {
    if (!a || !b) {
        return 0; // Invalid sets
    }

    if (a->element_size != b->element_size || a->comparator != b->comparator) {
        return 0; // Different element types or notions of equality
    }

    for (size_t i = 0; i < a->size; ++i) {
        if (!set_contains(b, (const char*)a->data + (i * a->element_size))) {
            return 0; // Found an element of a that is not in b
        }
    }

    return 1; // Every element of a is in b
}

/*
 @brief Checks whether two sets hold exactly the same elements.
 @param a A pointer to the first set.
 @param b A pointer to the second set.
 @return 1 if the sets hold the same elements, 0 otherwise.
 @attention The order of the elements is irrelevant, as a set has no order to compare.
 @attention Both sets must hold elements of the same size and use the same comparator, or this returns 0. A destructor on either set is fine, as nothing is copied.
 @attention If either set is NULL, this returns 0. It never returns a negative error code, so that it is safe to use directly in an if statement.
*/
static inline int set_is_equal(const set_t* a, const set_t* b) {
    if (!a || !b) {
        return 0; // Invalid sets
    }

    if (a->size != b->size) {
        return 0; // Different number of elements, so they cannot hold the same ones
    }

    // Containment is checked both ways. One way plus equal sizes would be enough
    // for two sets that are each unique under the shared comparator, but a
    // comparator loosened on a populated set leaves duplicates behind until
    // set_dedupe() is called, and {"A", "a"} would then compare equal to
    // {"A", "B"} under a case-insensitive rule
    return set_is_subset(a, b) && set_is_subset(b, a);
}

/*
 @brief Destroys the set and frees its memory.
 @param set A pointer to the pointer holding the set to be destroyed. It will be set to NULL.
 @return 0 on success, or if the set was already NULL. -1 only if the pointer itself is NULL.
 @attention After calling this function, the set pointer should not be used again (It will be set to NULL). Accessing it after destruction will lead to undefined behavior.
 @attention Only the pointer you pass in is set to NULL. Copies of that pointer held elsewhere are left dangling and must not be used.
 @attention If stored elements own memory of their own, set a destructor with set_set_destructor() to have it cleaned up here. Otherwise, the set will only free the memory allocated for the data array and the set structure itself, but not any dynamically allocated memory within the elements.
*/
static inline int set_destroy(set_t** set) {
    if (!set) {
        return -1; // Invalid pointer
    }

    if (!(*set)) {
        return 0; // Already NULL, nothing to destroy
    }

    set_t* target = *set;

    if (target->destructor) {
        for (size_t i = 0; i < target->size; ++i) {
            void* element = (char*)target->data + (i * target->element_size);
            target->destructor(element); // Call the destructor for each element
        }
    }

    free(target->data);
    free(target);
    *set = NULL; // Set the pointer to NULL to avoid dangling references

    return 0; // Success
}

#endif // SET_H
