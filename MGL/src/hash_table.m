/*
 * Copyright (C) Michael Larson on 1/6/2022
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * hash_table.c
 * MGL
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include <assert.h>
#include <stdint.h>  // For SIZE_MAX and UINT_MAX
#include <limits.h>  // For UINT_MAX fallback
#include <string.h>  // For memcpy
#include <stdbool.h>

#ifdef __APPLE__
#include <Metal/Metal.h>
#endif

#include "hash_table.h"
#include "glm_context.h"

#define MGL_HASH_TABLE_MAX_CAPACITY (1u << 24)
#define MGL_HASH_LOAD_FACTOR_NUM 7u
#define MGL_HASH_LOAD_FACTOR_DEN 10u
#define MGL_HASH_STATE_EMPTY 0u
#define MGL_HASH_STATE_OCCUPIED 1u
#define MGL_HASH_STATE_DELETED 2u

#ifndef MGL_VERBOSE_HASH_LOGS
#define MGL_VERBOSE_HASH_LOGS 0
#endif
#define MGL_HASH_MIN_CAPACITY 64u
#define MGL_HASH_COOKIE_KEYS 0x4d474c5f48415348ULL
#define MGL_HASH_COOKIE_STATES 0x53544154455f4d47ULL
#define MGL_HASH_COOKIE_DATA_INDEX 0x444154415f494e44ULL

static int mglRehash(HashTable *table, size_t new_capacity);
static inline uintptr_t mglMakeCookie(const void *ptr, uintptr_t salt);
static inline int mglCookieMatches(const void *ptr, uintptr_t cookie, uintptr_t salt);
static inline int mglIsPow2Size(size_t value);
static size_t mglNextPow2(size_t value);

static void mglInvalidateContainsDataCache(HashTable *table)
{
    if (!table) {
        return;
    }

    memset(table->cached_valid_ptrs, 0, sizeof(table->cached_valid_ptrs));
    memset(table->cached_valid_gens, 0, sizeof(table->cached_valid_gens));
    table->cached_valid_next = 0u;
}

static int mglDataIndexLooksSane(const HashTable *table)
{
    if (!table) return 0;
    if (!table->data_index) {
        return table->data_index_size == 0u &&
               table->data_index_count == 0u &&
               table->data_index_deleted_count == 0u &&
               table->data_index_cookie == 0u;
    }
    return table->data_index_size >= MGL_HASH_MIN_CAPACITY &&
           mglIsPow2Size(table->data_index_size) &&
           table->data_index_size <= MGL_HASH_TABLE_MAX_CAPACITY &&
           table->data_index_count <= table->data_index_size &&
           table->data_index_deleted_count <=
               table->data_index_size - table->data_index_count &&
           mglCookieMatches(table->data_index,
                            table->data_index_cookie,
                            MGL_HASH_COOKIE_DATA_INDEX);
}

static void mglDropDataIndex(HashTable *table)
{
    if (!table) return;
    if (table->data_index &&
        mglCookieMatches(table->data_index,
                         table->data_index_cookie,
                         MGL_HASH_COOKIE_DATA_INDEX)) {
        free(table->data_index);
    }
    table->data_index = NULL;
    table->data_index_size = 0u;
    table->data_index_count = 0u;
    table->data_index_deleted_count = 0u;
    table->data_index_cookie = 0u;
}

static inline size_t mglHashDataPointer(const void *data)
{
    uintptr_t value = (uintptr_t)data;
#if UINTPTR_MAX > UINT32_MAX
    value ^= value >> 33;
    value *= (uintptr_t)0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    value *= (uintptr_t)0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33;
#else
    value ^= value >> 16;
    value *= (uintptr_t)0x7feb352dU;
    value ^= value >> 15;
    value *= (uintptr_t)0x846ca68bU;
    value ^= value >> 16;
#endif
    return (size_t)value;
}

static size_t mglFindDataIndexSlot(const MGLHashDataIndexEntry *index,
                                   size_t capacity,
                                   const void *data,
                                   int for_insert,
                                   int *found)
{
    size_t first_deleted = SIZE_MAX;
    if (found) *found = 0;
    if (!index || capacity == 0u || !data) return SIZE_MAX;

    size_t mask = capacity - 1u;
    size_t slot = mglHashDataPointer(data) & mask;
    for (size_t probe = 0; probe < capacity; probe++) {
        unsigned char state = index[slot].state;
        if (state == MGL_HASH_STATE_EMPTY) {
            return for_insert && first_deleted != SIZE_MAX ? first_deleted : slot;
        }
        if (state == MGL_HASH_STATE_OCCUPIED && index[slot].data == data) {
            if (found) *found = 1;
            return slot;
        }
        if (for_insert && state == MGL_HASH_STATE_DELETED && first_deleted == SIZE_MAX) {
            first_deleted = slot;
        }
        slot = (slot + 1u) & mask;
    }
    return for_insert ? first_deleted : SIZE_MAX;
}

static int mglInsertDataIndexReference(MGLHashDataIndexEntry *index,
                                       size_t capacity,
                                       const void *data,
                                       size_t references)
{
    if (!data || references == 0u) return 1;
    int found = 0;
    size_t slot = mglFindDataIndexSlot(index, capacity, data, 1, &found);
    if (slot == SIZE_MAX) return 0;
    if (found) {
        index[slot].references += references;
    } else {
        index[slot].data = data;
        index[slot].references = references;
        index[slot].state = MGL_HASH_STATE_OCCUPIED;
    }
    return 1;
}

static int mglRebuildDataIndex(HashTable *table)
{
    if (!table) return 0;
    if (mglDataIndexLooksSane(table) && table->data_index) return 1;
    if (table->data_index) mglDropDataIndex(table);

    size_t desired = table->count > SIZE_MAX / 2u
        ? MGL_HASH_TABLE_MAX_CAPACITY
        : (size_t)table->count * 2u;
    if (desired < MGL_HASH_MIN_CAPACITY) desired = MGL_HASH_MIN_CAPACITY;
    size_t capacity = mglNextPow2(desired);
    if (capacity > MGL_HASH_TABLE_MAX_CAPACITY) capacity = MGL_HASH_TABLE_MAX_CAPACITY;
    if (capacity > MGL_HASH_TABLE_MAX_CAPACITY ||
        capacity > SIZE_MAX / sizeof(MGLHashDataIndexEntry)) {
        return 0;
    }
    MGLHashDataIndexEntry *index = (MGLHashDataIndexEntry *)calloc(capacity, sizeof(*index));
    if (!index) return 0;

    if (table->keys && table->states && table->size > 0u) {
        for (size_t i = 0; i < table->size; i++) {
            if (table->states[i] != MGL_HASH_STATE_OCCUPIED || !table->keys[i].data) continue;
            if (!mglInsertDataIndexReference(index, capacity, table->keys[i].data, 1u)) {
                free(index);
                return 0;
            }
        }
    }

    table->data_index = index;
    table->data_index_size = capacity;
    table->data_index_count = 0u;
    table->data_index_deleted_count = 0u;
    for (size_t i = 0; i < capacity; i++) {
        if (index[i].state == MGL_HASH_STATE_OCCUPIED) table->data_index_count++;
    }
    table->data_index_cookie = mglMakeCookie(index, MGL_HASH_COOKIE_DATA_INDEX);
    return 1;
}

static int mglRehashDataIndex(HashTable *table, size_t capacity)
{
    if (!mglDataIndexLooksSane(table) || !table->data_index) return 0;
    size_t old_capacity = table->data_index_size;
    if (capacity < MGL_HASH_MIN_CAPACITY ||
        !mglIsPow2Size(capacity) ||
        capacity > MGL_HASH_TABLE_MAX_CAPACITY ||
        capacity > SIZE_MAX / sizeof(MGLHashDataIndexEntry)) {
        return 0;
    }
    MGLHashDataIndexEntry *index = (MGLHashDataIndexEntry *)calloc(capacity, sizeof(*index));
    if (!index) return 0;
    for (size_t i = 0; i < old_capacity; i++) {
        MGLHashDataIndexEntry *entry = &table->data_index[i];
        if (entry->state == MGL_HASH_STATE_OCCUPIED &&
            !mglInsertDataIndexReference(index, capacity, entry->data, entry->references)) {
            free(index);
            return 0;
        }
    }
    MGLHashDataIndexEntry *old_index = table->data_index;
    uintptr_t old_cookie = table->data_index_cookie;
    table->data_index = index;
    table->data_index_size = capacity;
    table->data_index_count = 0u;
    table->data_index_deleted_count = 0u;
    for (size_t i = 0; i < capacity; i++) {
        if (index[i].state == MGL_HASH_STATE_OCCUPIED) table->data_index_count++;
    }
    table->data_index_cookie = mglMakeCookie(index, MGL_HASH_COOKIE_DATA_INDEX);
    if (mglCookieMatches(old_index, old_cookie, MGL_HASH_COOKIE_DATA_INDEX)) free(old_index);
    return 1;
}

static int mglGrowDataIndex(HashTable *table)
{
    if (!mglDataIndexLooksSane(table) || !table->data_index) return 0;
    size_t old_capacity = table->data_index_size;
    if (old_capacity >= MGL_HASH_TABLE_MAX_CAPACITY ||
        old_capacity > SIZE_MAX / 2u) {
        return 0;
    }
    return mglRehashDataIndex(table, old_capacity * 2u);
}

static int mglCompactDataIndex(HashTable *table)
{
    return table && table->data_index
        ? mglRehashDataIndex(table, table->data_index_size)
        : 0;
}

static void mglAddDataIndexReference(HashTable *table, const void *data)
{
    if (!table || !data || !mglDataIndexLooksSane(table) || !table->data_index) return;
    int found = 0;
    size_t slot = mglFindDataIndexSlot(table->data_index, table->data_index_size,
                                       data, 1, &found);
    if (found) {
        table->data_index[slot].references++;
        return;
    }
    size_t usedCount = table->data_index_count + table->data_index_deleted_count;
    bool consumesEmptySlot = slot == SIZE_MAX ||
        table->data_index[slot].state == MGL_HASH_STATE_EMPTY;
    if (slot == SIZE_MAX ||
        (consumesEmptySlot &&
         (usedCount + 1u) * MGL_HASH_LOAD_FACTOR_DEN >=
             table->data_index_size * MGL_HASH_LOAD_FACTOR_NUM)) {
        bool grow = (table->data_index_count + 1u) * MGL_HASH_LOAD_FACTOR_DEN >=
                    table->data_index_size * MGL_HASH_LOAD_FACTOR_NUM;
        int rehashed = grow
            ? mglGrowDataIndex(table)
            : mglCompactDataIndex(table);
        if (!rehashed) {
            mglDropDataIndex(table);
            return;
        }
        slot = mglFindDataIndexSlot(table->data_index, table->data_index_size,
                                    data, 1, &found);
    }
    if (slot == SIZE_MAX || found) {
        mglDropDataIndex(table);
        return;
    }
    if (table->data_index[slot].state == MGL_HASH_STATE_DELETED &&
        table->data_index_deleted_count > 0u) {
        table->data_index_deleted_count--;
    }
    table->data_index[slot].data = data;
    table->data_index[slot].references = 1u;
    table->data_index[slot].state = MGL_HASH_STATE_OCCUPIED;
    table->data_index_count++;
}

static void mglRemoveDataIndexReference(HashTable *table, const void *data)
{
    if (!table || !data || !mglDataIndexLooksSane(table) || !table->data_index) return;
    int found = 0;
    size_t slot = mglFindDataIndexSlot(table->data_index, table->data_index_size,
                                       data, 0, &found);
    if (!found || slot == SIZE_MAX) {
        mglDropDataIndex(table);
        return;
    }
    MGLHashDataIndexEntry *entry = &table->data_index[slot];
    if (entry->references > 1u) {
        entry->references--;
    } else {
        entry->data = NULL;
        entry->references = 0u;
        entry->state = MGL_HASH_STATE_DELETED;
        table->data_index_count--;
        table->data_index_deleted_count++;
        if (table->data_index_count == 0u) {
            memset(table->data_index, 0,
                   table->data_index_size * sizeof(*table->data_index));
            table->data_index_deleted_count = 0u;
        }
    }
}

static inline uintptr_t mglMakeCookie(const void *ptr, uintptr_t salt)
{
    return ptr ? (((uintptr_t)ptr ^ salt) + 0x9e3779b97f4a7c15ULL) : 0u;
}

static inline int mglCookieMatches(const void *ptr, uintptr_t cookie, uintptr_t salt)
{
    return ptr && cookie && (cookie == mglMakeCookie(ptr, salt));
}

static inline int mglIsPow2Size(size_t value)
{
    return value != 0u && ((value & (value - 1u)) == 0u);
}

static int mglHashTableLooksSane(const HashTable *table)
{
    if (!table) {
        return 0;
    }

    if (table->size == 0u) {
        return table->count == 0u &&
               table->keys == NULL &&
               table->states == NULL;
    }

    if (!mglIsPow2Size(table->size) ||
        table->size > MGL_HASH_TABLE_MAX_CAPACITY ||
        table->count > table->size ||
        !table->keys ||
        !table->states) {
        return 0;
    }

    if (!mglCookieMatches(table->keys, table->keys_cookie, MGL_HASH_COOKIE_KEYS) ||
        !mglCookieMatches(table->states, table->states_cookie, MGL_HASH_COOKIE_STATES)) {
        return 0;
    }

    return 1;
}

static int mglRepairHashTableIfNeeded(HashTable *table, const char *where)
{
    GLuint saved_name;

    if (!table) {
        return 0;
    }

    if (mglHashTableLooksSane(table)) {
        return 1;
    }

    saved_name = table->current_name;
    fprintf(stderr,
            "MGL WARNING: repairing corrupt hash table at %s table=%p size=%zu count=%zu current=%u keys=%p states=%p keyCookie=0x%llx stateCookie=0x%llx\n",
            where ? where : "unknown",
            (void *)table,
            table->size,
            table->count,
            saved_name,
            (void *)table->keys,
            (void *)table->states,
            (unsigned long long)table->keys_cookie,
            (unsigned long long)table->states_cookie);

    table->keys = NULL;
    table->states = NULL;
    table->keys_cookie = 0u;
    table->states_cookie = 0u;
    table->size = 0u;
    table->count = 0u;
    table->current_name = saved_name;
    table->deletion_generation = 0u;
    mglInvalidateContainsDataCache(table);
    mglDropDataIndex(table);

    return mglRehash(table, MGL_HASH_MIN_CAPACITY);
}

int mglHashTableValidateStorage(HashTable *table, const char *where)
{
    return mglRepairHashTableIfNeeded(table, where ? where : "validate");
}

int mglHashTableContainsData(HashTable *table, const void *data)
{
    if (!data || !mglRepairHashTableIfNeeded(table, "contains")) {
        return 0;
    }

    if (!table->keys || !table->states || table->size == 0u) {
        return 0;
    }

    if (mglRebuildDataIndex(table)) {
        int found = 0;
        size_t slot = mglFindDataIndexSlot(table->data_index,
                                           table->data_index_size,
                                           data,
                                           0,
                                           &found);
        return found && slot != SIZE_MAX;
    }

    /* OOM fallback: preserve the previous bounded working-set cache before
     * scanning the name table. The reverse index is optional. */
    for (size_t index = 0u; index < MGL_HASH_VALID_CACHE_CAPACITY; index++) {
        if (data == table->cached_valid_ptrs[index] &&
            table->deletion_generation == table->cached_valid_gens[index]) {
            return 1;
        }
    }

    for (size_t i = 0; i < table->size; i++) {
        if (table->states[i] != MGL_HASH_STATE_OCCUPIED) {
            continue;
        }
        if (table->keys[i].data == data) {
            size_t cache_index = table->cached_valid_next % MGL_HASH_VALID_CACHE_CAPACITY;
            table->cached_valid_ptrs[cache_index] = data;
            table->cached_valid_gens[cache_index] = table->deletion_generation;
            table->cached_valid_next = (uint8_t)((cache_index + 1u) % MGL_HASH_VALID_CACHE_CAPACITY);
            return 1;
        }
    }

    return 0;
}

static inline void mglSetStorage(HashTable *table, HashObj *keys, unsigned char *states)
{
    if (!table) {
        return;
    }
    table->keys = keys;
    table->states = states;
    table->keys_cookie = mglMakeCookie(keys, MGL_HASH_COOKIE_KEYS);
    table->states_cookie = mglMakeCookie(states, MGL_HASH_COOKIE_STATES);
}

static void mglFreeStorageIfOwned(HashTable *table,
                                  HashObj *keys,
                                  unsigned char *states,
                                  uintptr_t keys_cookie,
                                  uintptr_t states_cookie,
                                  const char *reason)
{
    (void)table;

    if (keys) {
        if (mglCookieMatches(keys, keys_cookie, MGL_HASH_COOKIE_KEYS)) {
            free(keys);
        } else {
            fprintf(stderr,
                    "MGL WARNING: hash storage keys pointer ownership mismatch (%s) table=%p keys=%p cookie=0x%llx; skipping free\n",
                    reason ? reason : "unknown",
                    (void *)table,
                    (void *)keys,
                    (unsigned long long)keys_cookie);
        }
    }

    if (states) {
        if (mglCookieMatches(states, states_cookie, MGL_HASH_COOKIE_STATES)) {
            free(states);
        } else {
            fprintf(stderr,
                    "MGL WARNING: hash storage states pointer ownership mismatch (%s) table=%p states=%p cookie=0x%llx; skipping free\n",
                    reason ? reason : "unknown",
                    (void *)table,
                    (void *)states,
                    (unsigned long long)states_cookie);
        }
    }
}

static inline uint32_t mglHashName(GLuint name)
{
    uint32_t x = (uint32_t)name;
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static size_t mglNextPow2(size_t v)
{
    if (v <= 1u) {
        return 1u;
    }

    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
#if SIZE_MAX > UINT32_MAX
    v |= v >> 32;
#endif
    v++;
    return v;
}

static int mglAllocHashStorage(size_t capacity, HashObj **keys_out, unsigned char **states_out)
{
    HashObj *keys = NULL;
    unsigned char *states = NULL;

    if (!keys_out || !states_out || capacity == 0u) {
        return 0;
    }

    if (capacity > (SIZE_MAX / sizeof(HashObj))) {
        return 0;
    }

    keys = (HashObj *)calloc(capacity, sizeof(HashObj));
    if (!keys) {
        return 0;
    }

    states = (unsigned char *)calloc(capacity, sizeof(unsigned char));
    if (!states) {
        free(keys);
        return 0;
    }

    *keys_out = keys;
    *states_out = states;
    return 1;
}

static size_t mglFindSlot(const HashTable *table, GLuint name, int for_insert, int *found)
{
    size_t mask;
    size_t index;
    size_t first_deleted = SIZE_MAX;

    if (found) {
        *found = 0;
    }

    if (!table || !table->keys || !table->states || table->size == 0u) {
        return SIZE_MAX;
    }

    mask = table->size - 1u;
    index = (size_t)mglHashName(name) & mask;

    for (size_t probe = 0; probe < table->size; probe++) {
        unsigned char state = table->states[index];

        if (state == MGL_HASH_STATE_EMPTY) {
            if (for_insert) {
                return (first_deleted != SIZE_MAX) ? first_deleted : index;
            }
            return SIZE_MAX;
        }

        if (state == MGL_HASH_STATE_OCCUPIED && table->keys[index].name == name) {
            if (found) {
                *found = 1;
            }
            return index;
        }

        if (for_insert && state == MGL_HASH_STATE_DELETED && first_deleted == SIZE_MAX) {
            first_deleted = index;
        }

        index = (index + 1u) & mask;
    }

    return (for_insert && first_deleted != SIZE_MAX) ? first_deleted : SIZE_MAX;
}

static int mglRehash(HashTable *table, size_t new_capacity)
{
    HashObj *new_keys = NULL;
    unsigned char *new_states = NULL;
    HashObj *old_keys = NULL;
    unsigned char *old_states = NULL;
    uintptr_t old_keys_cookie = 0u;
    uintptr_t old_states_cookie = 0u;
    size_t moved = 0u;

    if (!table) {
        return 0;
    }

    if (new_capacity < MGL_HASH_MIN_CAPACITY) {
        new_capacity = MGL_HASH_MIN_CAPACITY;
    }

    new_capacity = mglNextPow2(new_capacity);
    if (new_capacity > MGL_HASH_TABLE_MAX_CAPACITY) {
        new_capacity = MGL_HASH_TABLE_MAX_CAPACITY;
    }

    if (!mglAllocHashStorage(new_capacity, &new_keys, &new_states)) {
        fprintf(stderr, "MGL ERROR: failed to allocate hash storage new_capacity=%zu\n", new_capacity);
        return 0;
    }

    old_keys = table->keys;
    old_states = table->states;
    old_keys_cookie = table->keys_cookie;
    old_states_cookie = table->states_cookie;

    if (table->keys && table->states && table->size > 0u) {
        for (size_t i = 0; i < table->size; i++) {
            if (table->states[i] != MGL_HASH_STATE_OCCUPIED || table->keys[i].data == NULL) {
                continue;
            }

            HashObj obj = table->keys[i];
            size_t mask = new_capacity - 1u;
            size_t idx = (size_t)mglHashName(obj.name) & mask;

            for (size_t probe = 0; probe < new_capacity; probe++) {
                if (new_states[idx] != MGL_HASH_STATE_OCCUPIED) {
                    new_keys[idx] = obj;
                    new_states[idx] = MGL_HASH_STATE_OCCUPIED;
                    moved++;
                    break;
                }
                idx = (idx + 1u) & mask;
            }
        }
    }

    mglSetStorage(table, new_keys, new_states);
    table->size = new_capacity;
    table->count = moved;
    mglFreeStorageIfOwned(table,
                          old_keys,
                          old_states,
                          old_keys_cookie,
                          old_states_cookie,
                          "rehash");

    return 1;
}

static int ensureHashTableCapacity(HashTable *table, GLuint name)
{
    size_t old_cap;
    size_t new_cap;

    if (!table) {
        return 0;
    }

    if (!mglRepairHashTableIfNeeded(table, "ensure")) {
        return 0;
    }

    if (!table->keys || !table->states || table->size == 0u) {
        return mglRehash(table, MGL_HASH_MIN_CAPACITY);
    }

    if (table->count + 1u < (table->size * MGL_HASH_LOAD_FACTOR_NUM) / MGL_HASH_LOAD_FACTOR_DEN) {
        return 1;
    }

    old_cap = table->size;
    new_cap = old_cap;
    while (new_cap < MGL_HASH_TABLE_MAX_CAPACITY &&
           (table->count + 1u) >= (new_cap * MGL_HASH_LOAD_FACTOR_NUM) / MGL_HASH_LOAD_FACTOR_DEN) {
        new_cap *= 2u;
    }

    if (new_cap == old_cap) {
        fprintf(stderr, "MGL ERROR: hash table cannot grow further table=%p key=%u count=%zu cap=%zu\n",
                (void *)table,
                name,
                table->count,
                table->size);
        return 0;
    }

    fprintf(stderr,
            "MGL HASH grow table=%p oldCap=%zu newCap=%zu count=%zu key=%u load=%.2f\n",
            (void *)table,
            old_cap,
            new_cap,
            table->count,
            name,
            old_cap ? ((double)table->count / (double)old_cap) : 0.0);

    return mglRehash(table, new_cap);
}

void initHashTable(HashTable *ptr, GLuint size)
{
    if (!ptr)
    {
        return;
    }

    ptr->keys = NULL;
    ptr->states = NULL;
    ptr->keys_cookie = 0u;
    ptr->states_cookie = 0u;
    ptr->current_name = 0;
    ptr->size = 0;
    ptr->count = 0;
    ptr->deletion_generation = 0u;
    mglInvalidateContainsDataCache(ptr);
    ptr->data_index = NULL;
    ptr->data_index_size = 0u;
    ptr->data_index_count = 0u;
    ptr->data_index_deleted_count = 0u;
    ptr->data_index_cookie = 0u;

    if (size > 0) {
        size_t desired = (size_t)size * 2u;
        if (desired < MGL_HASH_MIN_CAPACITY) {
            desired = MGL_HASH_MIN_CAPACITY;
        }
        if (!mglRehash(ptr, desired)) {
            fprintf(stderr, "MGL ERROR: initHashTable failed to allocate initial capacity %u\n", size);
        }
    }
}

HashTable *createHashTable(GLuint size)
{
    HashTable *table = (HashTable *)calloc(1, sizeof(HashTable));
    if (!table) {
        return NULL;
    }
    initHashTable(table, size);
    return table;
}

void destroyHashTable(HashTable *ptr)
{
    HashObj *keys;
    unsigned char *states;
    uintptr_t keys_cookie;
    uintptr_t states_cookie;

    if (!ptr) {
        return;
    }

    keys = ptr->keys;
    states = ptr->states;
    keys_cookie = ptr->keys_cookie;
    states_cookie = ptr->states_cookie;

    mglFreeStorageIfOwned(ptr, keys, states, keys_cookie, states_cookie, "destroy");
    mglDropDataIndex(ptr);

    ptr->keys = NULL;
    ptr->states = NULL;
    ptr->keys_cookie = 0u;
    ptr->states_cookie = 0u;
    ptr->size = 0u;
    ptr->count = 0u;
    ptr->current_name = 0u;
    ptr->deletion_generation = 0u;
    mglInvalidateContainsDataCache(ptr);
}

GLuint getNewName(HashTable *table)
{
    GLuint name;

    if (!table)
    {
        return 0;
    }

    if (table->current_name == UINT_MAX)
    {
        fprintf(stderr, "MGL ERROR: hash table name space exhausted\n");
        return 0;
    }

    name = ++table->current_name;

    // Pre-grow the table so callers using generated names never hit fixed-size limits.
    if (!ensureHashTableCapacity(table, name))
    {
        table->current_name--;
        return 0;
    }

    return name;
}

void *searchHashTable(HashTable *table, GLuint name)
{
    int found = 0;
    size_t slot;

    if (!table)
    {
        return NULL;
    }

    if (!mglRepairHashTableIfNeeded(table, "search")) {
        return NULL;
    }

    if (!table->keys || !table->states || table->size == 0)
    {
        return NULL;
    }

    slot = mglFindSlot(table, name, 0, &found);
    if (!found || slot == SIZE_MAX) {
        return NULL;
    }

    return table->keys[slot].data;
}

void insertHashElement(HashTable *table, GLuint name, void *data)
{
    if (!ensureHashTableCapacity(table, name))
    {
        fprintf(stderr, "MGL ERROR: insertHashElement failed grow table=%p name=%u data=%p\n",
                (void *)table, name, data);
        return;
    }

    int dataIndexReady = mglRebuildDataIndex(table);

    int found = 0;
    size_t slot = mglFindSlot(table, name, 1, &found);
    if (slot == SIZE_MAX) {
        fprintf(stderr, "MGL ERROR: insertHashElement failed slot lookup table=%p name=%u data=%p\n",
                (void *)table, name, data);
        return;
    }

    if (!found) {
        table->count++;
    } else {
        if (dataIndexReady) mglRemoveDataIndexReference(table, table->keys[slot].data);
        /* Replacing an existing name removes its old data pointer from the
         * table, so cached membership for that pointer is no longer valid. */
        table->deletion_generation++;
        mglInvalidateContainsDataCache(table);
    }

    table->keys[slot].name = name;
    table->keys[slot].data = data;
    table->states[slot] = MGL_HASH_STATE_OCCUPIED;
    if (dataIndexReady) mglAddDataIndexReference(table, data);

    if (MGL_VERBOSE_HASH_LOGS) {
        fprintf(stderr,
                "MGL HASH insert table=%p name=%u slot=%zu data=%p found=%d count=%zu cap=%zu\n",
                (void *)table,
                name,
                slot,
                data,
                found,
                table->count,
                table->size);
    }
}

void deleteHashElement(HashTable *table, GLuint name)
{
    int found = 0;
    size_t slot;

    if (!mglRepairHashTableIfNeeded(table, "delete")) {
        return;
    }

    if (!table->keys || !table->states || table->size == 0) {
        return;
    }

    int dataIndexReady = mglRebuildDataIndex(table);

    slot = mglFindSlot(table, name, 0, &found);
    if (!found || slot == SIZE_MAX) {
        return;
    }

    if (dataIndexReady) mglRemoveDataIndexReference(table, table->keys[slot].data);

    /* Metal object lifecycle is owned by the caller.  Previous code here
     * nullified shader/program/texture/buffer mtl_data fields WITHOUT
     * releasing them, which leaked the Metal objects AND prevented the
     * caller's own cleanup (e.g. mglFreeShader / mglFreeProgram) from
     * releasing them because the pointers were already NULL.
     *
     * Callers MUST release Metal objects before calling deleteHashElement:
     *   - Textures:  invalidateTexture() releases mtl_data / sampled_data /
     *                params.mtl_data before deleteHashElement.
     *   - Shaders:   mglFreeShader() releases function/library variants after
     *                deleteHashElement (deleteHashElement must not null them).
     *   - Programs:  mglFreeProgram() releases mtl_data after deleteHashElement.
     *   - Buffers:   mtl_data is saved/restored across deleteHashElement for
     *                tombstone lifetime; released later by context teardown.
     *   - Samplers:  caller releases mtl_data after deleteHashElement.
     */

    table->keys[slot].name = 0;
    table->keys[slot].data = NULL;
    table->states[slot] = MGL_HASH_STATE_DELETED;
    if (table->count > 0u) {
        table->count--;
    }
    /* Bump generation so cached pointer validations fall back to full scan. */
    table->deletion_generation++;
    mglInvalidateContainsDataCache(table);

    if (table->count == 0u && table->states) {
        memset(table->states, MGL_HASH_STATE_EMPTY, table->size * sizeof(unsigned char));
    }
}

void mglHashTableForEach(HashTable *table, MGLHashTableForEachFunc func, void *user)
{
    if (!func || !mglRepairHashTableIfNeeded(table, "foreach")) {
        return;
    }

    if (!table->keys || !table->states || table->size == 0) {
        return;
    }

    for (size_t i = 0; i < table->size; i++) {
        if (table->states[i] == MGL_HASH_STATE_OCCUPIED &&
            table->keys[i].name != 0u &&
            table->keys[i].data != NULL) {
            func(table->keys[i].name, table->keys[i].data, user);
        }
    }
}

void mglHashTableClearEntries(HashTable *table)
{
    if (!mglRepairHashTableIfNeeded(table, "clear-entries")) {
        return;
    }

    if (!table->keys || !table->states || table->size == 0) {
        return;
    }

    for (size_t i = 0; i < table->size; i++) {
        table->keys[i].name = 0u;
        table->keys[i].data = NULL;
        table->states[i] = MGL_HASH_STATE_EMPTY;
    }
    if (mglDataIndexLooksSane(table) && table->data_index) {
        memset(table->data_index, 0, table->data_index_size * sizeof(*table->data_index));
        table->data_index_count = 0u;
        table->data_index_deleted_count = 0u;
    } else {
        mglDropDataIndex(table);
    }
    table->count = 0u;
    /* Bump generation and invalidate cache on bulk clear. */
    table->deletion_generation++;
    mglInvalidateContainsDataCache(table);
}
