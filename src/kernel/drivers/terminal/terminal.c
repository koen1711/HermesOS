#include "terminal.h"

#include "hardware/interrupts/panic.h"
#include "hardware/memory/alloc.h"
#include <utils/str/vsnprintf.h>
#include <utils/sync/spinlock.h>

#include "os/stdarg.h"

#include "tty.h"
#include "procs/vga.h"
#include "procs/lfb.h"

static vt_t terminals[TERMINAL_COUNT];
static const terminal_backend* backend = nullptr;
static int active = 0;
static spinlock_t terminal_lock = { .name = "terminal" };

static void terminal_flush(vt_t* vt)
{
    for (size_t y = 0; y < vt->rows; y++) {
        if (!vt->dirty[y])
            continue;
        vt->dirty[y] = 0;
        for (size_t x = 0; x < vt->cols; x++)
            backend->draw(x, y, vt->cells[y * vt->cols + x]);
    }
    backend->cursor(vt->x, vt->y, vt->cursor_visible);
}

static void terminal_attach(const terminal_backend* next)
{
    backend = next;
    for (int i = 0; i < TERMINAL_COUNT; i++)
        vt_resize(&terminals[i], backend->cols, backend->rows);
    vt_mark_dirty(&terminals[active]);
    terminal_flush(&terminals[active]);
}

void terminal_initialize(const terminal_mode mode)
{
    const terminal_backend* initial = mode == VGA_LFB_MODE ? lfb_text_backend() : vga_text_backend();
    if (initial == nullptr)
        initial = vga_text_backend();

    for (int i = 0; i < TERMINAL_COUNT; i++) {
        if (vt_init(&terminals[i], initial->cols, initial->rows) != 0) {
            panic("Failed to allocate terminal", __FILE__, __LINE__);
            return;
        }
    }
    terminal_attach(initial);
}

void terminal_switch_mode(const terminal_mode mode)
{
    const terminal_backend* next = mode == VGA_LFB_MODE ? lfb_text_backend() : vga_text_backend();
    if (next == nullptr)
        return;

    uint64_t flags;
    spinlock_acquire_irqsave(&terminal_lock, &flags);
    terminal_attach(next);
    spinlock_release_irqrestore(&terminal_lock, flags);
}

void terminal_write(const int index, const char* buf, const size_t len, const bool onlcr)
{
    if (backend == nullptr || index < 0 || index >= TERMINAL_COUNT)
        return;

    vt_t* vt = &terminals[index];
    char response[VT_RESPONSE_MAX];
    uint64_t flags;
    spinlock_acquire_irqsave(&terminal_lock, &flags);
    for (size_t i = 0; i < len; i++) {
        if (onlcr && buf[i] == '\n')
            vt_putc(vt, '\r');
        vt_putc(vt, (uint8_t)buf[i]);
    }
    if (index == active)
        terminal_flush(vt);
    const size_t response_len = vt->response_len;
    memcpy(response, vt->response, response_len);
    vt->response_len = 0;
    spinlock_release_irqrestore(&terminal_lock, flags);

    for (size_t i = 0; i < response_len; i++)
        tty_receive(index, response[i]);
}

void terminal_printf(const char* format, ...)
{
    char small[256];
    va_list args;
    va_start(args, format);
    const int required_length = vsnprintf(nullptr, 0, format, args);
    va_end(args);

    if (required_length < 0)
        return;

    char* buf = (size_t)required_length < sizeof(small) ? small : malloc(required_length + 1);
    if (buf == nullptr)
        return;

    va_start(args, format);
    vsnprintf(buf, required_length + 1, format, args);
    va_end(args);

    terminal_write(0, buf, required_length, true);
    if (buf != small)
        free(buf);
}

void terminal_switch(const int index)
{
    if (backend == nullptr || index < 0 || index >= TERMINAL_COUNT)
        return;

    uint64_t flags;
    spinlock_acquire_irqsave(&terminal_lock, &flags);
    if (index != active) {
        active = index;
        vt_mark_dirty(&terminals[active]);
        terminal_flush(&terminals[active]);
    }
    spinlock_release_irqrestore(&terminal_lock, flags);
}

int terminal_active()
{
    return active;
}

void terminal_size(const int index, uint16_t* cols, uint16_t* rows)
{
    if (index < 0 || index >= TERMINAL_COUNT) {
        *cols = VGA_WIDTH;
        *rows = VGA_HEIGHT;
        return;
    }
    *cols = (uint16_t)terminals[index].cols;
    *rows = (uint16_t)terminals[index].rows;
}

void terminal_update()
{
    if (backend == nullptr)
        return;

    uint64_t flags;
    spinlock_acquire_irqsave(&terminal_lock, &flags);
    vt_mark_dirty(&terminals[active]);
    terminal_flush(&terminals[active]);
    spinlock_release_irqrestore(&terminal_lock, flags);
}
