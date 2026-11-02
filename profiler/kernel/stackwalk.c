#include "stackwalk.h"

static bool in_text(const struct stackwalk_bounds* b, uint64_t addr)
{
    return addr >= b->text_low && addr < b->text_high;
}

size_t stackwalk(uint64_t rbp, const struct stackwalk_bounds* b, uint64_t* out, size_t max, bool* truncated)
{
    size_t n = 0;
    *truncated = false;

    if (b->stack_high < b->stack_low || b->stack_high - b->stack_low < CTX_SIZE + 16)
        return 0;

    for (;;) {
        if ((rbp & 7) != 0 || rbp < b->stack_low || rbp >= b->stack_high - 16)
            break;

        uint64_t next = *(const uint64_t*)rbp;
        uint64_t ret = *(const uint64_t*)(rbp + 8);
        bool is_trap = ret == b->trap_return;

        if (!is_trap && !in_text(b, ret))
            break;
        if (n == max) {
            *truncated = true;
            break;
        }

        out[n++] = ret;

        if (is_trap) {
            // The context sits above the return address; a kernel-mode interrupted frame continues from the saved rbp.
            uint64_t ctx = rbp + 16;
            if (ctx > b->stack_high - CTX_SIZE)
                break;
            if ((*(const uint64_t*)(ctx + CTX_CS_OFFSET) & 3) != 0)
                break;
            uint64_t rip = *(const uint64_t*)(ctx + CTX_RIP_OFFSET);
            if (!in_text(b, rip))
                break;
            if (n == max) {
                *truncated = true;
                break;
            }
            out[n++] = rip;
            next = *(const uint64_t*)(ctx + CTX_RBP_OFFSET);
        }

        if (next <= rbp)
            break;
        rbp = next;
    }

    return n;
}
