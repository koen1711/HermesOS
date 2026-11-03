#include <utils/rbtree/rbtree.h>

static bool is_red(const struct rb_node* node)
{
    return node && node->red;
}

static void transplant(struct rb_tree* tree, const struct rb_node* old, struct rb_node* node)
{
    struct rb_node* parent = old->parent;
    if (node)
        node->parent = parent;
    if (!parent)
        tree->root = node;
    else
        parent->child[parent->child[RB_RIGHT] == old] = node;
}

static void rotate(struct rb_tree* tree, struct rb_node* node, const enum rb_dir dir)
{
    const enum rb_dir other = !dir;
    struct rb_node* pivot = node->child[other];
    node->child[other] = pivot->child[dir];
    if (pivot->child[dir])
        pivot->child[dir]->parent = node;
    transplant(tree, node, pivot);
    pivot->child[dir] = node;
    node->parent = pivot;
}

void rb_init(struct rb_tree* tree)
{
    tree->root = nullptr;
}

void rb_insert(struct rb_tree* tree, struct rb_node* parent, struct rb_node** link, struct rb_node* node)
{
    node->parent = parent;
    node->child[RB_LEFT] = node->child[RB_RIGHT] = nullptr;
    node->red = true;
    *link = node;

    while (is_red(node->parent)) {
        parent = node->parent;
        struct rb_node* grand = parent->parent;
        const enum rb_dir dir = grand->child[RB_RIGHT] == parent;
        struct rb_node* uncle = grand->child[!dir];
        if (is_red(uncle)) {
            parent->red = uncle->red = false;
            grand->red = true;
            node = grand;
            continue;
        }
        if (parent->child[!dir] == node) {
            rotate(tree, parent, dir);
            parent = node;
        }
        parent->red = false;
        grand->red = true;
        rotate(tree, grand, !dir);
        break;
    }
    tree->root->red = false;
}

static void remove_fixup(struct rb_tree* tree, struct rb_node* node, struct rb_node* parent)
{
    while (node != tree->root && !is_red(node)) {
        const enum rb_dir dir = parent->child[RB_RIGHT] == node;
        const enum rb_dir other = !dir;
        struct rb_node* sibling = parent->child[other];
        if (sibling->red) {
            sibling->red = false;
            parent->red = true;
            rotate(tree, parent, dir);
            sibling = parent->child[other];
        }
        if (!is_red(sibling->child[RB_LEFT]) && !is_red(sibling->child[RB_RIGHT])) {
            sibling->red = true;
            node = parent;
            parent = node->parent;
            continue;
        }
        if (!is_red(sibling->child[other])) {
            sibling->child[dir]->red = false;
            sibling->red = true;
            rotate(tree, sibling, other);
            sibling = parent->child[other];
        }
        sibling->red = parent->red;
        parent->red = false;
        sibling->child[other]->red = false;
        rotate(tree, parent, dir);
        node = tree->root;
    }
    if (node)
        node->red = false;
}

void rb_remove(struct rb_tree* tree, struct rb_node* node)
{
    struct rb_node* child;
    struct rb_node* parent;
    bool removed_red;

    if (node->child[RB_LEFT] && node->child[RB_RIGHT]) {
        struct rb_node* next = node->child[RB_RIGHT];
        while (next->child[RB_LEFT])
            next = next->child[RB_LEFT];
        removed_red = next->red;
        child = next->child[RB_RIGHT];
        if (next->parent == node) {
            parent = next;
        } else {
            parent = next->parent;
            transplant(tree, next, child);
            next->child[RB_RIGHT] = node->child[RB_RIGHT];
            next->child[RB_RIGHT]->parent = next;
        }
        transplant(tree, node, next);
        next->child[RB_LEFT] = node->child[RB_LEFT];
        next->child[RB_LEFT]->parent = next;
        next->red = node->red;
    } else {
        child = node->child[RB_LEFT] ? node->child[RB_LEFT] : node->child[RB_RIGHT];
        parent = node->parent;
        removed_red = node->red;
        transplant(tree, node, child);
    }
    if (!removed_red)
        remove_fixup(tree, child, parent);
}

struct rb_node* rb_first(const struct rb_tree* tree)
{
    struct rb_node* node = tree->root;
    while (node && node->child[RB_LEFT])
        node = node->child[RB_LEFT];
    return node;
}

struct rb_node* rb_next(const struct rb_node* node)
{
    if (node->child[RB_RIGHT]) {
        struct rb_node* next = node->child[RB_RIGHT];
        while (next->child[RB_LEFT])
            next = next->child[RB_LEFT];
        return next;
    }
    struct rb_node* parent = node->parent;
    while (parent && parent->child[RB_RIGHT] == node) {
        node = parent;
        parent = node->parent;
    }
    return parent;
}
