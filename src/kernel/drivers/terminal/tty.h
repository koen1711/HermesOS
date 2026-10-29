#ifndef TERMINAL_TTY_H
#define TERMINAL_TTY_H

#include <os/stdint.h>

struct proc_files;
struct inode_s;

struct termios_k {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_line;
    uint8_t c_cc[19];
};

void tty_input(char c);
int tty_install_console(struct proc_files* files);
struct inode_s* tty_node();

#endif //TERMINAL_TTY_H
