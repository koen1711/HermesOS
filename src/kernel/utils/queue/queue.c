#include <utils/queue/queue.h>

void dlist_init(struct dlist* list)
{
    list->head = list->tail = nullptr;
    list->count = 0;
}

void dlist_insert_head(struct dlist* list, struct dlist_node* node)
{
    node->prev = nullptr;
    node->next = list->head;
    if (list->head)
        list->head->prev = node;
    else
        list->tail = node;
    list->head = node;
    list->count++;
}

void dlist_insert_tail(struct dlist* list, struct dlist_node* node)
{
    node->next = nullptr;
    node->prev = list->tail;
    if (list->tail)
        list->tail->next = node;
    else
        list->head = node;
    list->tail = node;
    list->count++;
}

void dlist_insert_after(struct dlist* list, struct dlist_node* pos, struct dlist_node* node)
{
    node->prev = pos;
    node->next = pos->next;
    if (pos->next)
        pos->next->prev = node;
    else
        list->tail = node;
    pos->next = node;
    list->count++;
}

void dlist_insert_before(struct dlist* list, struct dlist_node* pos, struct dlist_node* node)
{
    node->next = pos;
    node->prev = pos->prev;
    if (pos->prev)
        pos->prev->next = node;
    else
        list->head = node;
    pos->prev = node;
    list->count++;
}

void dlist_remove(struct dlist* list, struct dlist_node* node)
{
    if (node->prev)
        node->prev->next = node->next;
    else
        list->head = node->next;
    if (node->next)
        node->next->prev = node->prev;
    else
        list->tail = node->prev;
    node->prev = node->next = nullptr;
    list->count--;
}

bool dlist_empty(const struct dlist* list)
{
    return list->head == nullptr;
}

struct dlist_node* dlist_first(const struct dlist* list)
{
    return list->head;
}

struct dlist_node* dlist_last(const struct dlist* list)
{
    return list->tail;
}

struct dlist_node* dlist_next(const struct dlist_node* node)
{
    return node->next;
}

struct dlist_node* dlist_prev(const struct dlist_node* node)
{
    return node->prev;
}
