#include "tty.h"

#include <drivers/terminal/terminal.h>
#include <drivers/scheduler/process.h>
#include <drivers/scheduler/signal.h>
#include <drivers/syscalls/uaccess.h>
#include <hardware/memory/alloc.h>
#include <hardware/interrupts/irq.h>
#include <os/errno.h>

#define TTY_BUFFER 1024
#define WAIT_CHANNEL_TTY 0x39000000
#define WAIT_CHANNEL_TTY_SPAWN 0x39100000
#define TTY_SPAWN_RETRY_TICKS 100

enum termios_flags {
    TTY_ICRNL = 0x0100,
    TTY_OPOST = 0x0001,
    TTY_ONLCR = 0x0004,
    TTY_ISIG = 0x0001,
    TTY_ICANON = 0x0002,
    TTY_ECHO = 0x0008,
    TTY_ECHOE = 0x0010,
    TTY_ECHOK = 0x0020,
    TTY_ECHOCTL = 0x0200,
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

struct tty {
    int index;
    char buf[TTY_BUFFER];
    size_t len;
    size_t cooked;
    size_t eof;
    struct termios_k termios;
    int pgrp;
    int shell_pid;
    bool spawning;
    struct file_s* file;
    inode_t inode;
};

static file_operations_t tty_fops;
static file_operations_t ctty_fops;
static struct tty ttys[TERMINAL_COUNT];

#define TTY_DEFAULT(i) {                                                                                    \
    .index = i,                                                                                             \
    .termios = {                                                                                            \
        .c_iflag = TTY_ICRNL,                                                                               \
        .c_oflag = TTY_OPOST | TTY_ONLCR,                                                                   \
        .c_cflag = 0xBF,                                                                                    \
        .c_lflag = TTY_ISIG | TTY_ICANON | TTY_ECHO | TTY_ECHOE | TTY_ECHOK | TTY_ECHOCTL | TTY_IEXTEN,     \
        .c_cc = { [VINTR] = 0x03, [VQUIT] = 0x1C, [VERASE] = 0x7F, [VKILL] = 0x15, [VEOF] = 0x04, [VMIN] = 1 } \
    },                                                                                                      \
    .inode = { .id = (4 << 8) | (i + 1), .type = INODE_CHAR, .fops = &tty_fops, .private_data = &ttys[i] }  \
}

static struct tty ttys[TERMINAL_COUNT] = {
    TTY_DEFAULT(0), TTY_DEFAULT(1), TTY_DEFAULT(2), TTY_DEFAULT(3), TTY_DEFAULT(4), TTY_DEFAULT(5),
};

static inode_t ctty_inode = { .id = (5 << 8) | 0, .type = INODE_CHAR, .fops = &ctty_fops };
static inode_t console_inode = { .id = (5 << 8) | 1, .type = INODE_CHAR, .fops = &tty_fops, .private_data = &ttys[0] };

static volatile uint32_t spawn_pending = 0;

static struct tty* tty_of(const struct file_s* f)
{
    return f->node->private_data;
}

static void tty_output(const struct tty* t, const char* buf, const size_t len)
{
    const bool onlcr = (t->termios.c_oflag & TTY_OPOST) && (t->termios.c_oflag & TTY_ONLCR);
    terminal_write(t->index, buf, len, onlcr);
}

static void tty_echo(const struct tty* t, const char c)
{
    if (!(t->termios.c_lflag & TTY_ECHO))
        return;
    const bool control = ((uint8_t)c < 0x20 && c != '\n' && c != '\t') || c == 0x7F;
    if (control && (t->termios.c_lflag & TTY_ECHOCTL)) {
        const char seq[2] = { '^', c == 0x7F ? '?' : (char)(c + 0x40) };
        tty_output(t, seq, sizeof(seq));
        return;
    }
    tty_output(t, &c, 1);
}

static void tty_erase(struct tty* t)
{
    if (t->len <= t->cooked)
        return;
    const char c = t->buf[--t->len];
    if (!(t->termios.c_lflag & TTY_ECHO))
        return;
    const bool wide = ((uint8_t)c < 0x20 || c == 0x7F) && c != '\t' && (t->termios.c_lflag & TTY_ECHOCTL);
    for (int i = 0; i < (wide ? 2 : 1); i++)
        tty_output(t, "\b \b", 3);
}

static bool tty_canonical(const struct tty* t)
{
    return t->termios.c_lflag & TTY_ICANON;
}

static size_t tty_readable(const struct tty* t)
{
    return tty_canonical(t) ? t->cooked : t->len;
}

static void tty_signal(struct tty* t, const char c, const int sig)
{
    t->len = t->cooked;
    tty_echo(t, c);
    tty_output(t, "\n", 1);
    signal_send(t->pgrp, sig);
    process_wakeup(WAIT_CHANNEL_TTY | t->index);
}

void tty_receive(const int index, char c)
{
    if (index < 0 || index >= TERMINAL_COUNT)
        return;
    struct tty* t = &ttys[index];
    const struct termios_k* tm = &t->termios;
    if (c == '\r' && (tm->c_iflag & TTY_ICRNL))
        c = '\n';

    if (tm->c_lflag & TTY_ISIG) {
        if (c == (char)tm->c_cc[VINTR]) {
            tty_signal(t, c, SIGINT);
            return;
        }
        if (c == (char)tm->c_cc[VQUIT]) {
            tty_signal(t, c, SIGQUIT);
            return;
        }
    }

    if (tty_canonical(t)) {
        if (c == '\b' || c == (char)tm->c_cc[VERASE]) {
            tty_erase(t);
            return;
        }
        if (c == (char)tm->c_cc[VKILL]) {
            while (t->len > t->cooked)
                tty_erase(t);
            return;
        }
        if (c == (char)tm->c_cc[VEOF]) {
            t->cooked = t->len;
            t->eof++;
            process_wakeup(WAIT_CHANNEL_TTY | t->index);
            process_wakeup(WAIT_CHANNEL_POLL);
            return;
        }
    }

    if (t->len >= TTY_BUFFER)
        return;
    t->buf[t->len++] = c;
    tty_echo(t, c);
    if (c == '\n' || !tty_canonical(t))
        t->cooked = t->len;
    process_wakeup(WAIT_CHANNEL_TTY | t->index);
    process_wakeup(WAIT_CHANNEL_POLL);
}

void tty_input(const char c)
{
    tty_receive(terminal_active(), c);
}

void tty_input_string(const char* s)
{
    const int index = terminal_active();
    while (*s)
        tty_receive(index, *s++);
}

static void tty_spawner(void* arg)
{
    (void)arg;
    for (;;) {
        const uint32_t pending = __atomic_exchange_n(&spawn_pending, 0, __ATOMIC_ACQ_REL);
        for (int i = 0; i < TERMINAL_COUNT; i++) {
            if (!(pending & (1u << i)))
                continue;
            struct tty* t = &ttys[i];
            t->len = t->cooked = t->eof = 0;
            const int pid = process_spawn_shell(i);
            if (pid < 0) {
                terminal_write(i, "tty: cannot start shell\n", 24, true);
            }
            else {
                t->shell_pid = pid;
                t->pgrp = pid;
            }
            t->spawning = false;
        }
        if (pending == 0)
            process_sleep_timeout(WAIT_CHANNEL_TTY_SPAWN, TTY_SPAWN_RETRY_TICKS);
    }
}

void tty_initialize()
{
    ttys[0].shell_pid = INIT_PROC_ID;
    ttys[0].pgrp = INIT_PROC_ID;
    if (process_create_kernel(tty_spawner, nullptr) == nullptr)
        terminal_printf("tty: cannot start spawner thread\n");
}

void tty_switch(const int index)
{
    if (index < 0 || index >= TERMINAL_COUNT)
        return;
    terminal_switch(index);

    struct tty* t = &ttys[index];
    if (t->spawning || (t->shell_pid != 0 && process_find(t->shell_pid) != nullptr))
        return;
    t->spawning = true;
    __atomic_or_fetch(&spawn_pending, 1u << index, __ATOMIC_ACQ_REL);
    process_wakeup(WAIT_CHANNEL_TTY_SPAWN);
}

static ssize_t tty_read(struct file_s* f, void* buf, const size_t len)
{
    struct tty* t = tty_of(f);
    while (tty_readable(t) == 0) {
        if (tty_canonical(t) && t->eof) {
            t->eof--;
            return 0;
        }
        if (f->status_flags & 0x800)
            return -EAGAIN;
        if (process_sleep_interruptible(WAIT_CHANNEL_TTY | t->index) < 0)
            return -ERESTARTSYS;
    }
    if (len == 0)
        return 0;

    const uint64_t flags = irq_save();
    size_t n = tty_readable(t) < len ? tty_readable(t) : len;
    if (tty_canonical(t)) {
        for (size_t i = 0; i < n; i++) {
            if (t->buf[i] == '\n') {
                n = i + 1;
                break;
            }
        }
    }
    char chunk[TTY_BUFFER];
    memcpy(chunk, t->buf, n);
    memmove(t->buf, t->buf + n, t->len - n);
    t->len -= n;
    t->cooked = t->cooked > n ? t->cooked - n : 0;
    irq_restore(flags);

    memcpy(buf, chunk, n);
    return (ssize_t)n;
}

static ssize_t tty_write(struct file_s* f, const void* buf, const size_t len)
{
    const struct tty* t = tty_of(f);
    char chunk[128];
    const char* p = buf;
    for (size_t done = 0; done < len;) {
        const size_t n = len - done < sizeof(chunk) ? len - done : sizeof(chunk);
        memcpy(chunk, p + done, n);
        tty_output(t, chunk, n);
        done += n;
    }
    return (ssize_t)len;
}

static int tty_poll(struct file_s* f)
{
    const struct tty* t = tty_of(f);
    return VFS_POLLOUT | (tty_readable(t) || (tty_canonical(t) && t->eof) ? VFS_POLLIN : 0);
}

static int tty_ioctl(struct file_s* f, const unsigned long request, void* arg)
{
    struct tty* t = tty_of(f);
    switch (request) {
    case TCGETS:
        return copy_to_user(arg, &t->termios, sizeof(t->termios));
    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
        struct termios_k tm;
        if (copy_from_user(&tm, arg, sizeof(tm)) < 0)
            return -EFAULT;
        t->termios = tm;
        if (request == TCSETSF)
            t->len = t->cooked = t->eof = 0;
        else if (!tty_canonical(t))
            t->cooked = t->len;
        return 0;
    }
    case TIOCGWINSZ: {
        struct winsize_k ws = { 0 };
        terminal_size(t->index, &ws.ws_col, &ws.ws_row);
        return copy_to_user(arg, &ws, sizeof(ws));
    }
    case TIOCSWINSZ:
        return 0;
    case TIOCGPGRP:
        return copy_to_user(arg, &t->pgrp, sizeof(t->pgrp));
    case TIOCSPGRP:
        return copy_from_user(&t->pgrp, arg, sizeof(t->pgrp));
    case TCFLSH:
        if ((unsigned long)arg != 1)
            t->len = t->cooked = t->eof = 0;
        return 0;
    case TIOCSCTTY:
        scheduler_get()->current->tty = t->index + 1;
        return 0;
    case TCSBRK:
        return 0;
    case FIONREAD: {
        const int n = (int)tty_readable(t);
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

static int ctty_open(struct inode_s* node, struct file_s* f)
{
    (void)node;
    const int tty = scheduler_get()->current->tty;
    f->node = &ttys[tty > 0 ? tty - 1 : 0].inode;
    return 0;
}

static file_operations_t tty_fops = {
    .open = tty_open,
    .read = tty_read,
    .write = tty_write,
    .poll = tty_poll,
    .ioctl = tty_ioctl,
};

static file_operations_t ctty_fops = {
    .open = ctty_open,
};

inode_t* tty_node(const int index)
{
    if (index == TTY_NODE_CTTY)
        return &ctty_inode;
    if (index == TTY_NODE_CONSOLE)
        return &console_inode;
    if (index < 0 || index >= TERMINAL_COUNT)
        return nullptr;
    return &ttys[index].inode;
}

int tty_install(struct proc_files* files, const int index)
{
    if (index < 0 || index >= TERMINAL_COUNT)
        return -ENXIO;
    struct tty* t = &ttys[index];
    if (t->file == nullptr) {
        struct file_s* f = malloc(sizeof(struct file_s));
        if (f == nullptr)
            return -ENOMEM;
        memset(f, 0, sizeof(*f));
        f->node = index == 0 ? &console_inode : &t->inode;
        f->flags = O_RDONLY | O_WRONLY;
        f->ref_count = 1;
        const char name[] = "/dev/tty0";
        memcpy(f->filename, name, sizeof(name));
        f->filename[sizeof(name) - 2] = (char)('1' + index);
        if (vfs_install(f) < 0) {
            free(f);
            return -ENFILE;
        }
        t->file = f;
    }
    for (int fd = 0; fd < 3; fd++) {
        if (files->file[fd] == nullptr) {
            files->file[fd] = t->file;
            t->file->ref_count++;
        }
    }
    return 0;
}

int tty_install_console(struct proc_files* files)
{
    return tty_install(files, 0);
}
