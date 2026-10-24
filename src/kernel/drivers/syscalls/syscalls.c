#include "syscalls.h"
#include "fd.h"
#include "uaccess.h"
#include <scheduler/process.h>
#include <drivers/fs/vfs/vfs.h>
#include <drivers/net/socket/socket_syscalls.h>
#include <drivers/net/core/netisr.h>
#include <os/errno.h>

extern void irq_interrupt_syscall();

void syscalls_initialize()
{
    idt_set_entry(SYSCALL_VECTOR, (uintptr_t)irq_interrupt_syscall);
    idt[SYSCALL_VECTOR].type_attr = 0xEE;
}

static long sys_read(const int fd, void* buf, const size_t len)
{
    const struct file_s* f = fd_get(fd);
    if (f == nullptr)
        return -EBADF;
    if (!uaccess_ok(buf, len))
        return -EFAULT;
    return vfs_read((int)f->id, buf, len);
}

static long sys_write(const int fd, const void* buf, const size_t len)
{
    const struct file_s* f = fd_get(fd);
    if (f == nullptr)
        return -EBADF;
    if (!uaccess_ok(buf, len))
        return -EFAULT;
    return vfs_write((int)f->id, buf, len);
}

static long socket_syscall(const interrupt_context* c)
{
    const int fd = (int)c->rdi;
    switch (c->rax) {
        case SYSCALL_SOCKET:
            return sys_socket((int)c->rdi, (int)c->rsi, (int)c->rdx);
        case SYSCALL_CONNECT:
            return sys_connect(fd, (const struct sockaddr*)c->rsi, (socklen_t)c->rdx);
        case SYSCALL_ACCEPT:
            return sys_accept(fd, (struct sockaddr*)c->rsi, (socklen_t*)c->rdx);
        case SYSCALL_SENDTO:
            return sys_sendto(fd, (const void*)c->rsi, c->rdx, (int)c->r10, (const struct sockaddr*)c->r8,
                              (socklen_t)c->r9);
        case SYSCALL_RECVFROM:
            return sys_recvfrom(fd, (void*)c->rsi, c->rdx, (int)c->r10, (struct sockaddr*)c->r8, (socklen_t*)c->r9);
        case SYSCALL_SENDMSG:
            return sys_sendmsg(fd, (const struct msghdr_s*)c->rsi, (int)c->rdx);
        case SYSCALL_RECVMSG:
            return sys_recvmsg(fd, (struct msghdr_s*)c->rsi, (int)c->rdx);
        case SYSCALL_SHUTDOWN:
            return sys_shutdown(fd, (int)c->rsi);
        case SYSCALL_BIND:
            return sys_bind(fd, (const struct sockaddr*)c->rsi, (socklen_t)c->rdx);
        case SYSCALL_LISTEN:
            return sys_listen(fd, (int)c->rsi);
        case SYSCALL_GETSOCKNAME:
            return sys_getsockname(fd, (struct sockaddr*)c->rsi, (socklen_t*)c->rdx);
        case SYSCALL_GETPEERNAME:
            return sys_getpeername(fd, (struct sockaddr*)c->rsi, (socklen_t*)c->rdx);
        case SYSCALL_SOCKETPAIR:
            return -EOPNOTSUPP;
        case SYSCALL_SETSOCKOPT:
            return sys_setsockopt(fd, (int)c->rsi, (int)c->rdx, (const void*)c->r10, (socklen_t)c->r8);
        case SYSCALL_GETSOCKOPT:
            return sys_getsockopt(fd, (int)c->rsi, (int)c->rdx, (void*)c->r10, (socklen_t*)c->r8);
        default:
            return -ENOSYS;
    }
}

void handle_syscall(interrupt_context* context)
{
    switch (context->rax) {
        case SYSCALL_WRITE:
            context->rax = (uint64_t)sys_write((int)context->rdi, (const void*)context->rsi, context->rdx);
            netisr_poll();
            break;
        case SYSCALL_READ:
            context->rax = (uint64_t)sys_read((int)context->rdi, (void*)context->rsi, context->rdx);
            break;
        case SYSCALL_OPEN:
            // Handle open syscall
            break;
        case SYSCALL_CLOSE:
            context->rax = (uint64_t)(int64_t)fd_close((int)context->rdi);
            netisr_poll();
            break;
        case SYSCALL_SOCKET ... SYSCALL_GETSOCKOPT:
            context->rax = (uint64_t)socket_syscall(context);
            netisr_poll();   // deliver what this call queued (loopback) before returning to user mode
            break;
        case SYSCALL_FORK: {
            process_t* current = scheduler_get()->current;
            if (current == nullptr) {
                context->rax = (uint64_t)-1;
                break;
            }
            current->frame = context;
            context->rax = (uint64_t)(int64_t)process_fork();
            break;
        }
        default:
            // Unknown syscall
            break;
    }
}