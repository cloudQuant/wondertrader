// Fault injection for the probe only: the first acl_get_fd_np call in each
// process (parent and every forked child) fails with EINVAL, as observed.
#include <sys/acl.h>
#include <errno.h>
#include <unistd.h>
static pid_t g_pid = 0;
acl_t wp1_acl_get_fd_np(int fd, acl_type_t type)
{
    pid_t p = getpid();
    if (p != g_pid) { g_pid = p; errno = EINVAL; return NULL; }
    return acl_get_fd_np(fd, type);
}
__attribute__((used)) static struct { const void* replacement; const void* replacee; } interposers[]
    __attribute__((section("__DATA,__interpose"))) = { { (const void*)wp1_acl_get_fd_np, (const void*)acl_get_fd_np } };
