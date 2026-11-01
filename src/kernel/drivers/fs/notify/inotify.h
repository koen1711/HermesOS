#ifndef OS_FS_NOTIFY_INOTIFY_H
#define OS_FS_NOTIFY_INOTIFY_H

#include <os/stdbool.h>
#include <os/stddef.h>
#include <os/stdint.h>

#define IN_ACCESS 0x00000001
#define IN_MODIFY 0x00000002
#define IN_ATTRIB 0x00000004
#define IN_CLOSE_WRITE 0x00000008
#define IN_CLOSE_NOWRITE 0x00000010
#define IN_OPEN 0x00000020
#define IN_MOVED_FROM 0x00000040
#define IN_MOVED_TO 0x00000080
#define IN_CREATE 0x00000100
#define IN_DELETE 0x00000200
#define IN_DELETE_SELF 0x00000400
#define IN_MOVE_SELF 0x00000800
#define IN_ALL_EVENTS 0x00000fff

#define IN_Q_OVERFLOW 0x00004000
#define IN_IGNORED 0x00008000

#define IN_ONLYDIR 0x01000000
#define IN_DONT_FOLLOW 0x02000000
#define IN_EXCL_UNLINK 0x04000000
#define IN_MASK_ADD 0x20000000
#define IN_ISDIR 0x40000000
#define IN_ONESHOT 0x80000000

bool inotify_active(void);
void inotify_notify(const char* path, uint32_t mask, bool is_dir);
void inotify_deleted(const char* path, bool is_dir);
void inotify_moved(const char* old_path, const char* new_path, bool is_dir);

#endif //OS_FS_NOTIFY_INOTIFY_H
