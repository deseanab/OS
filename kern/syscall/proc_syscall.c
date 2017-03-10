#include <types.h>
#include <kern/errno.h>
#include <spl.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <vnode.h>
#include <kern/unistd.h>
#include <proc_syscall.h>
#include <kern/wait.h>

int pid_stack[PID_MAX/2];
int stack_index;
pid_t g_pid;

/* Returns the current process's ID */
pid_t sys_getpid(int32_t *retval){
	*retval = curproc->pid;
	return 0;
}

/* Curproc exits */
void sys_exit(int exitcode){
	int ppid = curproc->pid;
	
	/*
	 * Since this is exiting, if any children were
	 * forked from here, we must assign them a new parent.
	 * Assign the kproc (kernel) as the parent of each
	 */

	int index = 0;
	while(index < PROC_MAX){
		if(proc_table[index] != NULL){
			if(proc_table[index]->ppid == ppid){
				lock_acquire(proc_table[index]->lock);
				proc_table[index]->ppid = kproc->pid;
				lock_release(proc_table[index]->lock);
			}
		
		}
		
		index++;
	}
	
	curproc->exitcode = _MKWAIT_EXIT(exitcode);
	curproc->exited = true;

	/* Increment sem count - main/menu.c */	
	V(g_sem);
	thread_exit();
}


/* See thread_fork in thread.c 
 * At this point child is curproc
 */
static void child_entrypoint(void* data1, unsigned long data2) {
	struct trapframe tf;
	struct addrspace * addr;
	tf = *((struct trapframe *) data1);
	addr = (struct addrspace *) data2;
	
	/*Should be a new process, stolen from runprogram.c */
	KASSERT(proc_getas() == NULL);

	tf.tf_a3 = 0;
	tf.tf_v0 = 0;
	tf.tf_epc += 4;

	/* Set curproc address to that of passed in address (the parent's) */
	as_copy(addr,&curproc->p_addrspace);
	as_activate();

	/* No args yet */
	mips_usermode(&tf);
}

pid_t sys_fork(struct trapframe *tf_parent, int32_t *retval){
	struct proc *proc_child;
	struct trapframe *tf_temp;
	struct addrspace *addr_temp;
	int err;

	/*---Create proccess; assign ppid--- */

	proc_child = proc_create("Proc");
	proc_child->ppid = curproc->pid;

	/* Allocating space for address and copying into temp var */
	addr_temp = kmalloc(sizeof(*addr_temp));
	err = as_copy(curproc->p_addrspace, &addr_temp);
	if(err){
		*retval = -1;
		kfree(addr_temp);
		proc_destroy(proc_child);
		return ENOMEM;
	}
	
	/*---Allocating space for trapframe to be passed into child_forkentry---*/
	tf_temp = kmalloc(sizeof(*tf_temp));
	if(tf_temp == NULL){
		*retval = -1;
		kfree(tf_temp);
		proc_destroy(proc_child);
		return ENOMEM;
	}
	*tf_temp = *tf_parent;
	
	
	int index = 0;
	while(index < OPEN_MAX){
		
		if(curproc->file_table[index] != NULL){
			proc_child->file_table[index] = (struct file_handle *)kmalloc(sizeof(struct file_handle));
			proc_child->file_table[index]->lock = lock_create("child lock");
		
			proc_child->file_table[index]->vnode = curproc->file_table[index]->vnode;
	
			proc_child->file_table[index]->flags = curproc->file_table[index]->flags;

			proc_child->file_table[index]->count = curproc->file_table[index]->count;

			proc_child->file_table[index]->offset = curproc->file_table[index]->offset;

			proc_child->file_table[index]->lock = curproc->file_table[index]->lock;

	}

	index++;
	};
	
	/* Not enough args yet , not sure which trapframe gets passed here
	 * Ben says we copy the trapframe within sys_fork and then call thread fork
	* Does that mean we pass the child's trapframe?
	*/
	err = thread_fork("child thread", proc_child,
			(void*)child_entrypoint,tf_temp,(unsigned long)addr_temp);
	
	proc_child->thread->t_stack = curthread->t_stack;
	/* The parent is the curproc here */
	*retval = proc_child->pid;
	return 0;
}
