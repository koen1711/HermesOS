#include "../vfs.h"

#include <hardware/memory/alloc.h>

#include "../fs_utils.h"
#include "hardware/disks/disk.h"

vfs_node* root_node = NULL; // Root node of the VFS
vfs_node* device_node = NULL; // Device node for special files

int vfs_init() {
    // Initialize the VFS
    root_node = (vfs_node*)malloc(sizeof(vfs_node));
    if (!root_node) return -1;

    root_node->fd = -1;
    root_node->path = "/";
    root_node->flags = 0;
    root_node->is_directory = true;
    root_node->children = NULL;

    device_node = (vfs_node*)malloc(sizeof(vfs_node));
    if (!device_node) {
        free(root_node);
        return -1;
    }
    device_node->fd = -1;
    device_node->path = "/dev";
    device_node->flags = 0;
    device_node->is_directory = true;
    device_node->children = NULL;

    return 0; // Success
}

int vfs_mount(const fs_node* device, const char* mount_point) {
    vfs_node* node = (vfs_node*)malloc(sizeof(vfs_node));
    if (!node) return -1; // Memory allocation failed
    device->mount(device->unique_id, node);

    return 0;
}

int vfs_unmount(const char* mount_point) {
    // Implementation for unmounting a filesystem
    // This function will typically involve flushing any cached data and
    // releasing resources associated with the mounted filesystem.
    return 0; // Placeholder return value
}

int vfs_open(const char* path, int flags) {
    // Implementation for opening a file
    // This function will typically involve looking up the file in the VFS
    // and returning a file descriptor.
    return 0; // Placeholder return value
}

int vfs_close(int fd) {
    // Implementation for closing a file
    // This function will typically involve releasing the file descriptor
    // and any associated resources.
    return 0; // Placeholder return value
}

int vfs_read(int fd, void* buffer, size_t size) {
    // Implementation for reading from a file
    // This function will typically involve reading data from the file
    // associated with the given file descriptor into the provided buffer.
    return 0; // Placeholder return value
}

int vfs_write(int fd, const void* buffer, size_t size) {
    // Implementation for writing to a file
    // This function will typically involve writing data from the provided
    // buffer to the file associated with the given file descriptor.
    return 0; // Placeholder return value
}