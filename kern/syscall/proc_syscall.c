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
#include <lib.h>
#include <copyinout.h>
#include <kern/fcntl.h>
#include <vfs.h>
#include <syscall.h>
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
	
	/*
	 * Since this is exiting, if any children were
	 * forked from here, we must assign them a new parent.
	 * Assign the kproc (kernel) as the parent of each
	 */
		 
	lock_acquire(curproc->lock);

	int index = 0;
	while(index < PROC_MAX){
		if(proc_table[index] != NULL){
			if(proc_table[index]->ppid == curproc->pid){
				proc_table[index]->ppid = 0;
			}
		}
		index++;
	}

	curproc->exitcode = _MKWAIT_EXIT(exitcode);
	curproc->exited = true;
	cv_broadcast(curproc->cv, curproc->lock);
	
	lock_release(curproc->lock);
	
	/* Increment sem count - main/menu.c */	
	V(g_sem);
	thread_exit();
}


/* See thread_fork in thread.c 
 * At this point child is curproc
 */
static void child_entrypoint(void* data1, unsigned long data2) {
	
	struct trapframe tf;
	tf = *((struct trapframe *) data1);
	(void)data2;	
	kfree(data1);
	/*Should be a new process, stolen from runprogram.c */

	tf.tf_a3 = 0;
	tf.tf_v0 = 0;
	tf.tf_epc += 4;

	/* Set curproc address to that of passed in address (the parent's) */

	/* No args yet */
	mips_usermode(&tf);
}

pid_t sys_fork(struct trapframe *tf_parent, int32_t *retval){
	
	struct proc *proc_child;
	struct trapframe *tf_temp;
	int err;

	/*---Create proccess; assign ppid--- */
	proc_child = proc_create("Proc");
	if(proc_child == NULL){
		*retval = -1;
		return ENOMEM;
	}
	lock_acquire(proc_child->lock);
	proc_child->ppid = curproc->pid;
	proc_child->p_cwd = curproc->p_cwd;
	lock_release(proc_child->lock);
	/* Allocating space for address and copying into temp var */
	
	err = as_copy(curproc->p_addrspace, &proc_child->p_addrspace);
	if(err){
		*retval = -1;
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
			lock_acquire(curproc->file_table[index]->lock);
			curproc->file_table[index]->count++;
			lock_release(curproc->file_table[index]->lock);
		}
		proc_child->file_table[index] = curproc->file_table[index];
		index++;
	};
		
	err = thread_fork("child thread", proc_child,
			(void*)child_entrypoint,tf_temp,(unsigned long)NULL);
	
	/* The parent is the curproc here */
	lock_acquire(curproc->lock);
	*retval = proc_child->pid;
	lock_release(curproc->lock);
	return 0;
}

struct proc *get_proc(pid_t pid){
	
	int i = 0;
	for(; i < PROC_MAX; i++){
		if(pid == proc_table[i]->pid){
			return proc_table[i];
		}
	}
	return NULL;
}

pid_t sys_waitpid(pid_t pid, int *status, int options, int32_t *retval){		
	int buffer;
	int err;
	struct proc *proc;
	
/*	 Checks for impossible PID's not PID's that don't exist yet */
	if (pid < 0 || pid > PID_MAX){
		*retval = -1;
		return ESRCH;
	}else{
		lock_acquire(curproc->lock);
		proc = get_proc(pid);
		lock_release(curproc->lock);
	}
	
	if(proc == NULL){
		*retval = -1;
		return ESRCH;
	}

	if(options != 0){
		*retval = -1;
		return EINVAL;
	}
	
	if(pid == curproc->pid){
		*retval = -1;
		return ECHILD;
	}
		

	if(curproc->pid != proc->ppid){
		*retval = -1;
		return ECHILD;
	}	

	lock_acquire(proc->lock);	
	while(!proc->exited){
		cv_wait(proc->cv, proc->lock);
	}

	buffer = proc->exitcode;
	err = copyout((const char*)&buffer, (userptr_t)status, sizeof(int));

	if(err){
		*retval = -1;
		return EFAULT;
	}
	
	*retval = pid;

	lock_release(proc->lock);
//	kfree(proc);	
	return 0;
}


int sys_execv(char* progname, char** args, int *retval){
	
	struct addrspace *as;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result, index,numindex, char_reset, null_count, argcount, char_buflen;
	size_t proglen, arglen, char_index;
	
	arglen = 0;
	index = 0;
	null_count = 0;
	char_buflen = 0;
	char_index = 0;

	while(args[index] != NULL){
		arglen += strlen(args[index]);
		/* Will be used to make a buffer that can fit args and padding chars*/
		char_buflen += strlen(args[index])  +  (4 - (strlen(args[index])%4));
		index++; 	
	}
	
	//currently holds the count of arguments - aka number of pointers
	argcount = index;	
	null_count = arglen + index;

	char prog_dest[PATH_MAX];
	char *arg_dest[arglen], *userspace_args[index+1], char_buffer[char_buflen];
	//int array for args word count in terms of 4byte words arg+padding 
	int num_of_4byte[50];
	/*copy in progname (PATH)*/
	result = copyinstr((const_userptr_t)progname, prog_dest, PATH_MAX, &proglen);
	
	if(result){
		*retval = -1;
		kfree(prog_dest);
		return ENOMEM;
	}
	
	/* Use copyin, since not a string.
	 * Is arg_dest (which is in the kernel) pointing to 
	 * args elements (in userspace) after copyin gets called?
	*/
	
	result = copyin((const_userptr_t)args, (void*)&arg_dest, arglen);
	if(result){
		*retval = -1;
		return ENOMEM;
	}
	
	/* The one about the null padding 
	 * After this char_buffer is an array of chars
	 * with null padding	
	*/
	index = 0;
	while(arg_dest[index] != NULL){
		while(arg_dest[index] != NULL){
			size_t len = 4 - (strlen(arg_dest[index])%4);
			/*newlen includes null chars to be copied by concat_null*/
			
			size_t newlen = strlen(arg_dest[index]) + len;	
			char *temp = concat_null(arg_dest[index], len, newlen);
			//numof4bytes holds number of 4bytes that makes up temp
			int numof4byte = (strlen(temp) + 4) / 4;
			while(char_index < newlen){
				char_buffer[char_index] = temp[char_index];
				char_index++;
			}
		//add current args number of 4 bytes to array 
		num_of_4byte[numindex] = numof4byte;
		
		kprintf("numofbytes: %d\n", num_of_4byte[numindex]);
		numindex++;
		index++;	
		}
	}
	
	/**********************  TO BE REPURPOSED ********************/

	index = 0;
	char_index = 0;
	char_reset = 0;
	
	while(arg_dest[index] != NULL){
		userspace_args[index] = &char_buffer[char_index]; 
		while(arg_dest[index][char_reset] != '\0'){
			char_buffer[char_index] = arg_dest[index][char_reset];
			char_index++;
			char_reset++;
		}
		char_buffer[char_index] = ' ';//how we're separting these
		char_reset = 0;
	//	char_index++; //increment for null char
		index++;
	}
	userspace_args[index] = NULL; //NULL terminated array
	char_index = 0;
	
	/**********************  TO BE REPURPOSED ********************/
	
	/* Open the file. */
	result = vfs_open(progname, O_RDONLY, 0, &v);
	if (result) {
		*retval = -1;
		kfree(prog_dest);
		kfree(arg_dest);
		return result;
	}

	/* Create a new address space. */
	as = as_create();
	if (as == NULL) {
		vfs_close(v);
		return ENOMEM;
	}

	/* Switch to it and activate it. */
	proc_setas(as);
	as_activate();

	/* Load the executable. */
	result = load_elf(v, &entrypoint);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		vfs_close(v);
		*retval = -1;
		return result;
	}

	/* Done with the file now. */
	vfs_close(v);

	/* Define the user stack in the address space */
	result = as_define_stack(as, &stackptr);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		*retval = -1;
		return result;
	}	
	
	stackptr -= null_count;
	result = copyout((const void *)userspace_args, (userptr_t)stackptr, (size_t)null_count);

	if(result){
		*retval = -1;
		return ENOMEM;
	}

	/* Warp to user mode. */
	enter_new_process(argcount /*argc*/, (userptr_t)stackptr /*userspace addr of argv*/,
			  NULL /*userspace addr of environment*/,
			  stackptr, entrypoint);
	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;
}

char * concat_null(char * str, size_t len, size_t buflen){
	size_t index = 0;
	char temp[buflen];
	while(str[index] != '\0'){
		temp[index] = str[index];
		index++;
	}
	while(index < len){
		strcat(temp, "\0");
		index++;
	}
	char *rtrn = temp;
	return rtrn;
}
