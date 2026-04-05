#ifndef FS_H
#define FS_H

#include "fs_utils.h"

extern int register_filesystem(struct file_system_type *);
extern int unregister_filesystem(struct file_system_type *);

#endif //FS_H
