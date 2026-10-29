#include "tty.h"

#include <drivers/terminal/terminal.h>
#include <drivers/scheduler/process.h>
#include <drivers/scheduler/signal.h>
#include <drivers/syscalls/uaccess.h>
#include <hardware/memory/alloc.h>
#include <os/errno.h>

#define TTY_BUFFER 1024
#define WAIT_CHANNEL_TTY 0x39000000

enum termios_flags {
    TTY_ICRNL = 0x0100,
    TTY_OPOST = 0x0001,
    TTY_ONLCR = 0x0004,
    TTY_ISIG = 0x0001,
    TTY_ICANON = 0x0002,
    TTY_ECHO = 0x0008,
    TTY_ECHOE = 0x0010,
    TTY_ECHOK = 0x0020,
    TTY_IEXTEN = 0x8000
};

enum termios_cc {
    VINTR = 0,
    VQUIT = 1,
    VERASE = 2,
    VKILL = 3,
    VEOF = 4,
    VTIME = 5,
    VMIN = 6
};

enum tty_ioctl {
    TCGETS = 0x5401,
    TCSETS = 0x5402,
    TCSETSW = 0x5403,
    TCSETSF = 0x5404,
    TCSBRK = 0x5409,
    TCFLSH = 0x540B,
    TIOCSCTTY = 0x540E,
    TIOCGPGRP = 0x540F,
    TIOCSPGRP = 0x5410,
    TIOCGWINSZ = 0x5413,
    TIOCSWINSZ = 0x5414,
    FIONREAD = 0x541B,
    FIONBIO = 0x5421
};

struct winsize_k {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

static struct {
    char buf[TTY_BUFFER];
    size_t len;
    size_t cooked;
    size_t eof;
    struct termios_k termios;
    struct winsize_k winsize;
    int pgrp;
    struct file_s* file;
} console = {
    .termios = {
        .c_iflag = TTY_ICRNL,
        .c_oflag = TTY_OPOST | TTY_ONLCR,
        .c_cflag = 0xBF,
        .c_lflag = TTY_ISIG | TTY_ICANON | TTY_ECHO | TTY_ECHOE | TTY_ECHOK | TTY_IEXTEN,
        .c_cc = { [VINTR] = 0x03, [VQUIT] = 0x1C, [VERASE] = 0x7F, [VKILL] = 0x15, [VEOF] = 0x04, [VMIN] = 1 }
    },
    .winsize = { .ws_row = 25, .ws_col = 80 },
    .pgrp = INIT_PROC_ID
};

static void tty_echo(const char c)
{
    if (console.termios.c_lflag & TTY_ECHO)
        terminal_printf("%c", c);
}

static bool tty_canonical()
{
    return console.termios.c_lflag & TTY_ICANON;
}

static size_t tty_readable()
{
    return tty_canonical() ? console.cooked : console.len;
}

void tty_input(char c)
{
    const struct termios_k* t = &console.termios;
    if (c == '\r' && (t->c_iflag & TTY_ICRNL))
        c = '\n';

    if ((t->c_lflag & TTY_ISIG) && c == (char)t->c_cc[VINTR]) {
        console.len = console.cooked;
        tty_echo('^');
        tty_echo('C');
        tty_echo('\n');
        signal_send(console.pgrp, SIGINT);
        process_wakeup(WAIT_CHANNEL_TTY);
        return;
    }

    if (tty_canonical()) {
        if (c == '\b' || c == (char)t->c_cc[VERASE]) {
            if (console.len > console.cooked) {
                console.len--;
                if (t->c_lflag & TTY_ECHO)
                    terminal_printf("\b \b");
            }
            return;
        }
        if (c == (char)t->c_cc[VEOF]) {
            console.cooked = console.len;
            console.eof++;
            process_wakeup(WAIT_CHANNEL_TTY);
            return;
        }
    }

    if (console.len >= TTY_BUFFER)
        return;
    console.buf[console.len++] = c;
    tty_echo(c);
    if (c == '\n')
        console.cooked = console.len;
    process_wakeup(WAIT_CHANNEL_TTY);
    process_wakeup(WAIT_CHANNEL_POLL);
}

static ssize_t tty_read(struct file_s* f, void* buf, const size_t len)
{
    while (tty_readable() == 0) {
        if (tty_canonical() && console.eof) {
            console.eof--;
            return 0;
        }
        if (f->status_flags & 0x800)
            return -EAGAIN;
        if (process_sleep_interruptible(WAIT_CHANNEL_TTY) < 0)
            return -ERESTARTSYS;
    }
    if (len == 0)
        return 0;

    size_t n = tty_readable() < len ? tty_readable() : len;
    if (tty_canonical()) {
        for (size_t i = 0; i < n; i++) {
            if (console.buf[i] == '\n') {
                n = i + 1;
                break;
            }
        }
    }
    memcpy(buf, console.buf, n);
    memmove(console.buf, console.buf + n, console.len - n);
    console.len -= n;
    console.cooked = console.cooked > n ? console.cooked - n : 0;
    return (ssize_t)n;
}

static ssize_t tty_write(struct file_s* f, const void* buf, const size_t len)
{
    (void)f;
    char chunk[129];
    const char* p = buf;
    for (size_t done = 0; done < len;) {
        size_t n = len - done < 128 ? len - done : 128;
        size_t k = 0;
        for (size_t i = 0; i < n; i++) {
            if (p[done + i] != '\0')
                chunk[k++] = p[done + i];
        }
        chunk[k] = '\0';
        terminal_printf("%s", chunk);
        done += n;
    }
    return (ssize_t)len;
}

static int tty_poll(struct file_s* f)
{
    (void)f;
    return VFS_POLLOUT | (tty_readable() || (tty_canonical() && console.eof) ? VFS_POLLIN : 0);
}

static int tty_ioctl(struct file_s* f, const unsigned long request, void* arg)
{
    switch (request) {
    case TCGETS:
        return copy_to_user(arg, &console.termios, sizeof(console.termios));
    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
        struct termios_k t;
        if (copy_from_user(&t, arg, sizeof(t)) < 0)
            return -EFAULT;
        console.termios = t;
        if (request == TCSETSF)
            console.len = console.cooked = console.eof = 0;
        else if (!tty_canonical())
            console.cooked = console.len;
        return 0;
    }
    case TIOCGWINSZ:
        return copy_to_user(arg, &console.winsize, sizeof(console.winsize));
    case TIOCSWINSZ:
        return copy_from_user(&console.winsize, arg, sizeof(console.winsize));
    case TIOCGPGRP:
        return copy_to_user(arg, &console.pgrp, sizeof(console.pgrp));
    case TIOCSPGRP:
        return copy_from_user(&console.pgrp, arg, sizeof(console.pgrp));
    case TCFLSH:
        if ((unsigned long)arg != 1)
            console.len = console.cooked = console.eof = 0;
        return 0;
    case TCSBRK:
    case TIOCSCTTY:
        return 0;
    case FIONREAD: {
        const int n = (int)tty_readable();
        return copy_to_user(arg, &n, sizeof(n));
    }
    case FIONBIO: {
        int on;
        if (copy_from_user(&on, arg, sizeof(on)) < 0)
            return -EFAULT;
        f->status_flags = on ? f->status_flags | 0x800 : f->status_flags & ~0x800;
        return 0;
    }
    default:
        return -ENOTTY;
    }
}

static int tty_open(struct inode_s* node, struct file_s* f)
{
    (void)node;
    (void)f;
    return 0;
}

static file_operations_t tty_fops = {
    .open = tty_open,
    .read = tty_read,
    .write = tty_write,
    .poll = tty_poll,
    .ioctl = tty_ioctl,
};

static inode_t tty_inode = {
    .id = (5 << 8) | 1,
    .type = INODE_CHAR,
    .fops = &tty_fops,
};

inode_t* tty_node()
{
    return &tty_inode;
}

int tty_install_console(struct proc_files* files)
{
    if (console.file == nullptr) {
        struct file_s* f = malloc(sizeof(struct file_s));
        if (f == nullptr)
            return -ENOMEM;
        memset(f, 0, sizeof(*f));
        f->node = &tty_inode;
        f->flags = O_RDONLY | O_WRONLY;
        f->ref_count = 1;
        memcpy(f->filename, "/dev/console", sizeof("/dev/console"));
        if (vfs_install(f) < 0) {
            free(f);
            return -ENFILE;
        }
        console.file = f;
    }
    for (int fd = 0; fd < 3; fd++) {
        if (files->file[fd] == nullptr) {
            files->file[fd] = console.file;
            console.file->ref_count++;
        }
    }
    return 0;
}
