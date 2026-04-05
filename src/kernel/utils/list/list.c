#include "list.h"

#include <stddef.h>
#include <hardware/memory/alloc.h>

size_t default_hash(void* key) {
    return (size_t)key;
}

int default_compare(const void* key1, const void* key2) {
    return key1 == key2 ? 0 : 1;
}

void default_destructor(void* ptr) {
}

// Create a new map
full_map* map_create(const uint64_t capacity,
                size_t (*hash_func)(void*),
                int (*key_compare)(void*,void*),
                void (*key_destructor)(void*),
                void (*value_destructor)(void*)) {
    full_map* map = malloc(sizeof(full_map));
    if (!map) return NULL;

    map->capacity = capacity > 0 ? capacity : 16;
    map->size = 0;

    map->buckets = calloc(map->capacity, sizeof(struct map_entry*));
    if (!map->buckets) {
        free(map);
        return NULL;
    }

    // Set function pointers with defaults
    map->hash_func = hash_func ? hash_func : default_hash;
    map->key_compare = key_compare ? (void*)key_compare : (void*)default_compare;
    map->key_destructor = key_destructor ? key_destructor : default_destructor;
    map->value_destructor = value_destructor ? value_destructor : default_destructor;


    return map;
}

// Insert or update a key-value pair
int map_put(full_map* map, void* key, void* value) {
    if (!map || !key) return 0;

    // Calculate hash and bucket index
    const size_t hash = map->hash_func(key);
    const size_t index = hash % map->capacity;

    // Check if key already exists
    struct map_entry* current = map->buckets[index];
    while (current) {
        if (map->key_compare(current->key, key) == 0) {
            // Key exists, update value
            map->value_destructor(current->value);
            current->value = value;
            return 1;
        }
        current = current->next;
    }

    // Create new entry
    struct map_entry* new_entry = malloc(sizeof(struct map_entry));
    if (!new_entry) return 0;

    new_entry->key = key;
    new_entry->value = value;
    new_entry->next = map->buckets[index];
    map->buckets[index] = new_entry;
    map->size++;

    return 1;
}

// Get a value by key
void* map_get(full_map* map, void* key) {
    if (!map || !key) return NULL;

    const size_t hash = map->hash_func(key);
    const size_t index = hash % map->capacity;

    const struct map_entry* current = map->buckets[index];
    while (current) {
        if (map->key_compare(current->key, key) == 0) {
            return current->value;
        }
        current = current->next;
    }

    return NULL;
}

// Remove a key-value pair
int map_remove(full_map* map, void* key) {
    if (!map || !key) return 0;

    const size_t hash = map->hash_func(key);
    const size_t index = hash % map->capacity;

    struct map_entry* current = map->buckets[index];
    struct map_entry* prev = NULL;

    while (current) {
        if (map->key_compare(current->key, key) == 0) {
            // Remove entry
            if (prev) {
                prev->next = current->next;
            } else {
                map->buckets[index] = current->next;
            }

            // Destroy key and value
            map->key_destructor(current->key);
            map->value_destructor(current->value);
            free(current);
            map->size--;
            return 1;
        }
        prev = current;
        current = current->next;
    }

    return 0;
}

// Destroy the entire map
void map_destroy(full_map* map) {
    if (!map) return;

    for (int i = 0; i < map->capacity; i++) {
        struct map_entry* current = map->buckets[i];
        while (current) {
            struct map_entry* temp = current;
            current = current->next;

            // Destroy key and value
            map->key_destructor(temp->key);
            map->value_destructor(temp->value);
            free(temp);
        }
    }

    free(map->buckets);
    free(map);
}