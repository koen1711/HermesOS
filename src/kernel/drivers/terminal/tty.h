#ifndef TERMINAL_TTY_H
#define TERMINAL_TTY_H

#include <os/stdint.h>
#include <os/stddef.h>

#define TTY_NODE_CTTY (-1)
#define TTY_NODE_CONSOLE (-2)

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

void tty_initialize();
void tty_input(char c);
void tty_input_string(const char* s);
void tty_receive(int index, char c);
void tty_switch(int index);
int tty_install(struct proc_files* files, int index);
int tty_install_console(struct proc_files* files);
struct inode_s* tty_node(int index);

#endif //TERMINAL_TTY_H
