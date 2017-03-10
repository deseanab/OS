#include <types.h>
#include <spinlock.h>
#include <limits.h>
#include <file_syscall.h>
#include <mips/trapframe.h>
#include <proc.h>
/* Global Semaphore for sys_exit & menu */
extern struct semaphore * g_sem;

pid_t sys_getpid(int32_t *retval);
void sys_exit(int exitcode);
pid_t sys_fork(struct trapframe *tf_parent, int32_t *retval); //trapframe