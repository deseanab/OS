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
#include <pagetable.h>
#include <vm.h>
#include <mips/tlb.h>

int pid_stack[PID_MAX/2];
int stack_index;
pid_t g_pid;
//char *arg_dest[ARG_MAX/64];
char char_buffer[ARG_MAX];

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
	if(exitcode >= 1 && exitcode <= 32){

		curproc->exitcode = _MKWAIT_SIG(exitcode);
	}
	else{
	curproc->exitcode = _MKWAIT_EXIT(exitcode);
	}

	curproc->exited = true;
	cv_broadcast(curproc->cv, curproc->lock);
	
	lock_release(curproc->lock);
	
	/* Increment sem count - main/menu.c */	
	V(g_sem);
	thread_exit(); //<---- we call proc_destroy in here  only on the program running
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
	VOP_INCREF(curproc->p_cwd);
	proc_child->ppid = curproc->pid;
	proc_child->p_cwd = curproc->p_cwd;
	lock_release(proc_child->lock);
	/* Allocating space for address and copying into temp var */
	
	err = as_copy(curproc->p_addrspace, &proc_child->p_addrspace);
	if(err){
		*retval = -1;
	//	proc_destroy(proc_child);
		return ENOMEM;
	}
	
	/*---Allocating space for trapframe to be passed into child_forkentry---*/
	tf_temp = kmalloc(sizeof(*tf_temp));
	if(tf_temp == NULL){
		*retval = -1;
		kfree(tf_temp);
	//	proc_destroy(proc_child);
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
	
	int index = 0;
	while(index < PROC_MAX){
		if(proc_table[index]){
			if(pid == proc_table[index]->pid){
				return proc_table[index];
			}
		}
		index++;
	}
	return NULL;
}

pid_t sys_waitpid(pid_t pid, int *status, int options, int32_t *retval){		
	int buffer;
	int err;
	struct proc *proc;
	
	if(status == NULL){
		*retval = 0;
		return 0;
	}
	
	if(options != 0){
		*retval = -1;
		return EINVAL;
	}

/*	 Checks for impossible PID's not PID's that don't exist yet */
	if (pid < PID_MIN || pid > PID_MAX){
		*retval = -1;
		return ESRCH;
	}else{
		proc = get_proc(pid);
		if(proc == NULL){
			*retval = -1;
			return ESRCH;
		}	
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
	lock_release(proc->lock);
	
	buffer = proc->exitcode;
	err = copyout((const char*)&buffer, (userptr_t)status, sizeof(int));

	if(err){
		*retval = -1;
		return EFAULT;
	}
	
	*retval = pid;
	if(proc->exited){
				
	/*
	 * Release the file handles of their misery/ cleanup pls
	 */
	 
		proc_destroy(proc);
	}
	//kfree(proc);	
	return 0;
}

int sys_execv(char* progname, char** args, int *retval){
	
/*
	unsigned int z;
	for(z = 0; z < sizeof(arg_dest); z++){
		arg_dest[z] = '\0';
	}	
*/
	
//	bzero(arg_dest, sizeof(arg_dest));
	bzero(char_buffer, sizeof(char_buffer));
	struct addrspace *as;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result, index, numindex, argc, char_buflen;
	size_t arglen, char_index, char_reset;
// arg_pointer_count;

	unsigned int testprogname = (unsigned int)progname;	
	unsigned int testargs = (unsigned int)args;
	unsigned int testarg;
	
	//char *arg_dest[ARG_MAX/64];
	
	if(progname == NULL){
		*retval = -1;
		return EFAULT;

	}

	if(args == NULL){
		*retval = -1;
		return EFAULT;

	}


	if(testprogname == 0x40000000 || testprogname >= 0x80000000){
		*retval = -1;
		return EFAULT;
	}

	
	if(testargs == 0x40000000 || testargs >= 0x80000000){
		*retval = -1;
		return EFAULT;
	}
	arglen = 0;
	index = 0;
	char_buflen = 0;
	char_index = 0;
//	arg_pointer_count = sizeof(args);

//	size_t pointer_index = 0;
//	char *arg_strings[64];
/*		
	result = copyin((const_userptr_t)args, (void*)&arg_strings[pointer_index], arg_pointer_count);
	if(result){
		*retval = -1;
		return ENOMEM;
	}
*/

	if(strlen(progname) == 0){
		*retval = -1;
		return EISDIR;
	
	}

	while(args[index] != NULL){

		testarg = (unsigned int)args[index];
		
		if(testarg == 0x40000000 || testarg >= 0x80000000){
			*retval = -1;
			return EFAULT;
		}
		
		arglen += strlen(args[index]);
		/* Will be used to make a buffer that can fit args and padding chars*/
		char_buflen += strlen(args[index])  +  (4 - (strlen(args[index])%4));
		index++; 	
	}
	
	//currently holds the count of arguments - excluding the progname
	argc = index;
//	char prog_dest[PATH_MAX];

//	char char_buffer[char_buflen];
	//int array for args word count in terms of 4byte words arg+padding 
	int num_of_4byte[argc];
	/*copy in progname (PATH)*/
/*
	result = copyinstr((const_userptr_t)progname, prog_dest, PATH_MAX,&proglen);
	
	if(result){
		*retval = -1;
		kfree(prog_dest);
		return ENOMEM;
	}
*/	
	/* Use copyin, since not a string.
	 * Is arg_dest (which is in the kernel) pointing to 
	 * args elements (in userspace) after copyin gets called?
	*/
/*
	lock_acquire(curproc->lock);	
	result = copyin((const_userptr_t)args, (void*)&arg_dest, arglen);
	if(result){
		*retval = -1;
		lock_release(curproc->lock);
		return ENOMEM;
	}
	lock_release(curproc->lock);
*/

	/* The one about the null padding 
	 * After this char_buffer is an array of chars
	 * with null padding	
	*/

	index = 0;
	numindex = 0;
	char_reset = 0;
	while(args[index] != NULL){
		size_t len = 4 - (strlen(args[index])%4);
		/*newlen includes null chars to be copied by concat_null*/
			
		size_t newlen = strlen(args[index]) + len;	
		char *temp = concat_null(args[index], newlen);
		//numof4bytes holds number of 4bytes that makes up temp
		int numof4byte = (strlen(temp) + len) / 4;
		while(char_reset< newlen){
			char_buffer[char_index] = temp[char_reset];
			char_index++;
			char_reset++;
		}
		kfree(temp);
		//add current args number of 4 bytes to array 
		num_of_4byte[numindex] = numof4byte;
		numindex++;
		index++;
		char_reset = 0; //start from beginning of new string
		newlen = 0;
	}
	/* Open the file. */
	result = vfs_open(progname, O_RDONLY, 0, &v);
	if (result) {
		*retval = -1;
//		kfree(prog_dest);
	//	kfree(arg_dest);
		
		return result;
	}

	/* Create a new address space. */
	as = as_create();
	if (as == NULL) {
		vfs_close(v);
		return ENOMEM;
	}

	//get old adderspace and destroy it
	struct addrspace *oldas = curproc->p_addrspace;
	if(oldas != NULL){
		as_destroy(oldas);
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
		
	/* Size of char buffer and char pointers array*/
	stackptr -= char_buflen;
	
	/* "/testbin/add 1 2" has 4 ptrs */
	void *userspace_args[argc+1];

	index = 0;
	
	while(index < argc){
		userspace_args[index] = (void*)stackptr;
		stackptr += 4*num_of_4byte[index];
		index++;
	}
	userspace_args[index] = NULL;
	/* Size of char pointer array and char buffer array */
	size_t usr_args_size = sizeof(userspace_args);
	size_t char_buffer_size = char_buflen;
	size_t copyout_data = usr_args_size + char_buffer_size;
	
	
	stackptr -= copyout_data;
	
	result = copyout((const void *)userspace_args, (userptr_t)stackptr,usr_args_size);

	if(result){
		*retval = -1;
		return ENOMEM;
	}
	stackptr += usr_args_size;
	
	
	result = copyout((const void*)char_buffer, (userptr_t)stackptr,char_buflen);
	stackptr += char_buffer_size;
	
	stackptr -= copyout_data;
	
	/* Warp to user mode. */
	enter_new_process(argc /*argc*/, (userptr_t)stackptr /*userspace addr of argv*/,
			  NULL /*userspace addr of environment*/,
			  stackptr, entrypoint);
	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;
}

char * concat_null(char * str, size_t buflen){
	size_t index = 0;
	char *temp = kmalloc(buflen);
	
	/* Null out buffer before it gets used */
	while(index < buflen){
		temp[index] = '\0';
		index++;
	}
	
	index = 0;	
	while(index < strlen(str)){
		temp[index] = str[index];
		index++;
	}
	
	return temp;
}

/*
int sys_sbrk(intptr_t amount, int *retval){
//	"break" is the end of heap region, retval set to old "break"

	//pointer to current addrspace
	struct addrspace *as = curproc->p_addrspace;
	
	//bounds of current heap region
	vaddr_t heap_s = as->heap_region->as_vbase;
	vaddr_t heap_e = as->heap_region->as_vend;
	vaddr_t new_heap_e = 0;
	
	//check if amount is page_aligned
	if(amount % 4){
		*retval = -1;
		return EINVAL;
	}	 

	//check if amount will move heap_e below heap_s
	if((amount + heap_e) < heap_s){
		*retval = -1;
		return EINVAL;
	}

	//check if amount will move heap_e into stack region
	vaddr_t stack_s = as->stack_region->as_vbase;
	
	if( amount > 0 && (amount + heap_e) > stack_s){
		*retval = -1;
		return ENOMEM;
	}
	else {
		if(amount < 0){
			if(((long)heap_e + amount) < (long)heap_s){
				*retval = -1;
				return EINVAL;
			}
		}
	}

	int i = 0;
	int npages = 0;
	int tlb_index = 0;
	vaddr_t vpn = 0;
//	paddr_t pas = 0;
	struct page_entry * pg_entry;
	new_heap_e = heap_e;
	//negative amount
	if(amount < 0){
		//amount changed to postive to get postive number of pages.
		amount *= -1;
		npages = amount / PAGE_SIZE;

		for(i = 0 ; i < npages; i++){
					
			new_heap_e -= PAGE_SIZE;
			vpn = new_heap_e & PAGE_FRAME;
			
			//update tlb
			tlb_index = tlb_probe(vpn,0);
			
			if(tlb_index > 0){
			

			tlb_write(TLBHI_INVALID(tlb_index),TLBLO_INVALID(),tlb_index);
			pg_entry = vpn_check(vpn, as->page_table);
			free_upages(pg_entry->pas);
		}


		}
		
		*retval = heap_e;
		curproc->p_addrspace->heap_region->as_vend -= amount;
		
	
	}
	
	//positive amount
	
	else{

	        int err = 0;
		npages = amount / PAGE_SIZE;
		
		for(i = 0 ; i < npages; i++){
					
			new_heap_e += PAGE_SIZE;
			vpn = new_heap_e & PAGE_FRAME;
			err = push_pte(&(as->page_table),vpn);
			if(err){
				return err;
			}
			//bzero((void*)PADDR_TO_KVADDR(as->page_table->pas), PAGE_SIZE);

		}
		*retval = heap_e;
		curproc->p_addrspace->heap_region->as_vend += amount;

	}
	return 0;
}

*/
