#ifndef FS_MEMFD_H
#define FS_MEMFD_H

#include <drivers/fs/vfs/driver.h>

inode_t* mem_inode_create();
void mem_inode_unlink(inode_t* node);

#endif //FS_MEMFD_H
