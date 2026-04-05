#ifndef VFS_UTILS_H
#define VFS_UTILS_H

#include <drivers/fs/fs_utils.h>

int vfs_find_node(const char* path, vfs_node* root, vfs_node* node);


#endif
