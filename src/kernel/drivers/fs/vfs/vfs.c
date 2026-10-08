#include "vfs.h"
#include "drivers/terminal/terminal.h"
#include "utils/str/str.h"

static file* fd_table[MAX_OPEN_FILES];
static RegisteredDriver driver_table[MAX_DRIVERS];
static MountedFileSystem mount_table[MAX_MOUNTED_FILESYSTEMS];

static int driver_count = 0;

static RegisteredDriver* get_driver(const char* fs_name) {
    for (int i=0; i<driver_count; i++) {
        if ((strcmp((driver_table[i].name), fs_name) == 0)) {
            return &driver_table[i];
        }
    }
    return NULL;
}

static MountedFileSystem* find_mount(const char* path, const char** rest) {
    MountedFileSystem* best = NULL;
    size_t best_len = 0;

    for (int i = 0; i < MAX_MOUNTED_FILESYSTEMS; i++) {
        if (mount_table[i].root == NULL) continue;

        const size_t len = mount_table[i].mount_path_length;
        if (strncmp(path, mount_table[i].mount_path, len) != 0) { continue; }

        const char next = path[len];
        if (next != '\0' && next != '/' && len != 1) { continue; }

        if (best == NULL || len > best_len) {
            best = &mount_table[i];
            best_len = len;
        }
    }
    if (best != NULL) *rest = path + best_len;
    return best;
}

int vfs_register_driver(const char* name, FileSystemDriver* fsd) {
    if (name == NULL || fsd == NULL) {return -1;}
    if (driver_count >= MAX_DRIVERS) {return -1;}
    for (int i = 0; i < driver_count; i++) {
        if (strcmp(name, driver_table[i].name) == 0) {return -1;}
    }
    terminal_printf(driver_table[driver_count].name,
        sizeof(driver_table[driver_count].name), "%s", name);
    driver_table[driver_count].fsd = fsd;
    driver_count++;
    return 0;
}


int vfs_mount(const char* mount_path, const char* fs_name, void*blob) {
    RegisteredDriver* reg_driver = get_driver(fs_name);
    if(reg_driver == NULL){ return -1;}

    for (int i=0; i < MAX_MOUNTED_FILESYSTEMS; i++) {
        if (mount_table[i].root != NULL &&
            strcmp(mount_path, mount_table[i].mount_path) == 0) {return -1;}
    }

    inode* root_node = reg_driver->fsd->mount(blob);
    if (root_node == NULL){ return -1;}

    for (int i=0; i < MAX_MOUNTED_FILESYSTEMS; i++) {
        if (mount_table[i].root == NULL) {
            mount_table[i].driver = reg_driver;
            mount_table[i].root = root_node;
            terminal_printf(mount_table[i].mount_path, sizeof(mount_table[i].mount_path), "%s", mount_path);
            return 0;
        }
    }
    reg_driver->fsd->unmount(root_node);
    return -1;
}

int vfs_unmount(const char* mount_path) {
      for (int i=0; i < MAX_MOUNTED_FILESYSTEMS; i++) {
        if (mount_table[i].root != NULL && strcmp(mount_path, mount_table[i].mount_path) == 0){
            if (mount_table[i].open_count > 0) {
                return -1;
            }
            mount_table[i].driver->fsd->unmount(mount_table[i].root);
            memset(&mount_table[i], 0, sizeof(mount_table[i]));
            return 0;
        }
    }
    return -1;
}

static file* get_file(int fd) {
    if (fd < 0 || fd >= MAX_OPEN_FILES) {return NULL;}
    return fd_table[fd];}


int vfs_open(const char* path, int flags) {
    const char* rest;
    MountedFileSystem* mount = find_mount(path, &rest);

    if (mount == NULL) {return -1;}
    inode* current = mount->root;
    while (*rest != '\0') {
        while (*rest == '/') rest++;
        if (*rest == '\0') {break;}
        size_t n = 0;
        while (rest[n] != '/' && rest[n] != '\0') n++;

        char name[MAX_FILE_PATH];
        if (n >= sizeof(name)) return -1;
        memcpy(name, rest, n);
        name[n] = '\0';

        if (current->iops == NULL || current->iops->lookup(current,name) == NULL) {return -1;}
        current = current->iops->lookup(current,name);
        if (current == NULL) {return -1;}

        rest += n;
    }
    if (current->fops == NULL || current->fops->open == NULL) {return -1;}

    file* f = malloc(sizeof(file));
    if (f == NULL) {return -1;}
    memset(f, 0, sizeof(*f));
    f->node = current;
    f->flags = flags;
    f->file_position = 0;
    f->mount = mount;

    if (current->fops->open(current, f) < 0) {free(f); return -1;}
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (fd_table[i] == NULL) {
            f->id = i;
            fd_table[i] = f;
            mount->open_count++;
            return i;
        }
    }

    if (current->fops->close) current->fops->close(f);
    free(f);
    return -1;
}

ssize_t vfs_read(const int fd, void* buf, size_t len) {
    file* f = get_file(fd);
    if (f == NULL) {return -1;}

    if (!(f->flags & O_RDONLY)) { return -1; }

    if (f->node->fops == NULL || f->node->fops->read == NULL) {return -1;    }
    return f->node->fops->read(f, buf, len);

}
ssize_t vfs_write(const int fd, const void* buf, const size_t length) {
    file* f = get_file(fd);
    if (f == NULL) {return -1;}

    if (!(f->flags & O_WRONLY)) { return -1; }

    if (f->node->fops == NULL || f->node->fops->read == NULL) {return -1;}
    return f->node->fops->write(f, buf, length);
}


int vfs_close(const int fd) {
    file* f = get_file(fd);
    if (f == NULL) {return -1;}
    if (f->node->fops != NULL && f->node->fops->close != 0) {
        f->node->fops->close(f);
    }

    f->mount->open_count--;
    free(f);
    fd_table[fd] = NULL;
    return 0;
}

