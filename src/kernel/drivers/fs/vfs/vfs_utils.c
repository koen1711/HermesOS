#include "vfs_utils.h"
#include "hardware/memory/alloc.h"
#include "utils/str/str.h"

int vfs_find_node(const char* path, vfs_node* root, vfs_node* node) {
    if (root == NULL || path == NULL || node == NULL) {
        return -1; // Invalid parameters
    }

    // Check if the path is the root
    if (strcmp(path, "/") == 0) {
        *node = *root;
        return 0; // Found root node
    }

    // Split the path into components
    char* path_copy = strdup(path);
    char* token = strtok(path_copy, "/");
    vfs_node* current = root;

    while (token != NULL) {
        bool found = false;
        for (vfs_node* child = current->children; child != NULL; child = child->next) {
            if (strcmp(child->path, token) == 0) {
                current = child;
                found = true;
                break;
            }
        }
        if (!found) {
            free(path_copy);
            return -2; // Node not found
        }
        token = strtok(NULL, "/");
    }

    *node = *current;
    free(path_copy);
    return 0; // Node found successfully
}
