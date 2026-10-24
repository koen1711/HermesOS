#ifndef FS_PIPE_H
#define FS_PIPE_H

struct file_s;

#define PIPE_BUFFER_SIZE (64 * 1024)
#define PIPE_ATOMIC 4096

int pipe_create(struct file_s** read_end, struct file_s** write_end);

#endif //FS_PIPE_H
