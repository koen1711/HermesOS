#ifndef UTILS_RBTREE_RBTREE_H
#define UTILS_RBTREE_RBTREE_H

#include <os/stddef.h>
#include <os/stdbool.h>

enum rb_dir {
    RB_LEFT = 0,
    RB_RIGHT = 1
};

struct rb_node {
    struct rb_node* parent;
    struct rb_node* child[2];
    bool red;
};

struct rb_tree {
    struct rb_node* root;
};

#define rb_entry(node, type, member) \
    ((type*)((char*)(node) - offsetof(type, member)))

void rb_init(struct rb_tree* tree);

void rb_insert(struct rb_tree* tree, struct rb_node* parent, struct rb_node** link, struct rb_node* node);
void rb_remove(struct rb_tree* tree, struct rb_node* node);
struct rb_node* rb_first(const struct rb_tree* tree);
struct rb_node* rb_next(const struct rb_node* node);

#endif //UTILS_RBTREE_RBTREE_H
