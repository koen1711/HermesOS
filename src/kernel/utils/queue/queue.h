#ifndef UTILS_QUEUE_QUEUE_H
#define UTILS_QUEUE_QUEUE_H

#include <os/stddef.h>
#include <os/stdbool.h>

struct dlist_node {
    struct dlist_node* prev;
    struct dlist_node* next;
};

struct dlist {
    struct dlist_node* head;
    struct dlist_node* tail;
    size_t count;
};

#define dlist_entry(node, type, member) \
    ((type*)((char*)(node) - offsetof(type, member)))

void dlist_init(struct dlist* list);
void dlist_insert_head(struct dlist* list, struct dlist_node* node);
void dlist_insert_tail(struct dlist* list, struct dlist_node* node);
void dlist_insert_after(struct dlist* list, struct dlist_node* pos, struct dlist_node* node);
void dlist_insert_before(struct dlist* list, struct dlist_node* pos, struct dlist_node* node);
void dlist_remove(struct dlist* list, struct dlist_node* node);
bool dlist_empty(const struct dlist* list);
struct dlist_node* dlist_first(const struct dlist* list);
struct dlist_node* dlist_last(const struct dlist* list);
struct dlist_node* dlist_next(const struct dlist_node* node);
struct dlist_node* dlist_prev(const struct dlist_node* node);

#endif //UTILS_QUEUE_QUEUE_H
