#ifndef LIST_H
#define LIST_H

#include <os/stddef.h>
#include <os/stdint.h>

struct map_entry {
    void* key;
    void* value;
    struct map_entry* next;
};

typedef struct {
    struct map_entry** buckets;
    uint64_t size;
    uint64_t capacity;

    size_t (*hash_func)(void* key);
    int (*key_compare)(void* key1, void* key2);
    void (*key_destructor)(void* key);
    void (*value_destructor)(void* value);
} full_map;

full_map* map_create(uint64_t capacity,
                size_t (*hash_func)(void* key),
                int (*key_compare)(void* key1, void* key2),
                void (*key_destructor)(void* key),
                void (*value_destructor)(void* value));
int map_put(full_map* map, void* key, void* value);
void* map_get(full_map* map, void* key);
int map_remove(full_map* map, void* key);
void map_destroy(full_map* map);

#endif //LIST_H
