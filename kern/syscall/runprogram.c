/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Sample/test code for running a user program.  You can use this for
 * reference when implementing the execv() system call. Remember though
 * that execv() needs to do more than runprogram() does.
 */

#include <types.h>
#include <kern/errno.h>
#include <kern/fcntl.h>
#include <lib.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <vm.h>
#include <vfs.h>
#include <syscall.h>
#include <test.h>
#include <kern/unistd.h>

/*
 * Load program "progname" and start running it in usermode.
 * Does not return except on error.
 *
 * Calls vfs_open on progname and thus may destroy it.
 */

int filesys_init();
struct proc *proc_table[PROC_MAX];
int g_pid;

int
runprogram(char *progname)
{
	struct addrspace *as;
	struct vnode *v;
	vaddr_t entrypoint, stackptr;
	int result;

	/* Open the file. */
	result = vfs_open(progname, O_RDONLY, 0, &v);
	if (result) {
		return result;
	}

	/* We should be a new process. */
	KASSERT(proc_getas() == NULL);

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
		return result;
	}

	/* Done with the file now. */
	vfs_close(v);

	proc_init();	
	result = filesys_init();

	/* Define the user stack in the address space */
	result = as_define_stack(as, &stackptr);
	if (result) {
		/* p_addrspace will go away when curproc is destroyed */
		return result;
	}
	
	/* Warp to user mode. */
	enter_new_process(0 /*argc*/, NULL /*userspace addr of argv*/,
			  NULL /*userspace addr of environment*/,
			  stackptr, entrypoint);

	/* enter_new_process does not return. */
	panic("enter_new_process returned\n");
	return EINVAL;
}

void proc_init(){
	g_pid = 0;
	while(g_pid < PROC_MAX){
		proc_table[g_pid] = NULL;
		g_pid++;
	}
	g_pid = 0;
}	

int filesys_init(){

	/* Set STD files for first process */
	if(!curproc->file_table[STDIN_FILENO] && 
	   !curproc->file_table[STDOUT_FILENO] &&
	   !curproc->file_table[STDERR_FILENO]){
		curproc->file_table[STDIN_FILENO] = (struct file_handle *)kmalloc(sizeof(struct file_handle));	
		curproc->file_table[STDERR_FILENO] = (struct file_handle *)kmalloc(sizeof(struct file_handle));
		curproc->file_table[STDOUT_FILENO] = (struct file_handle *)kmalloc(sizeof(struct file_handle));
	
		struct vnode *v0;
		struct vnode *v1;
		struct vnode *v2;
	
		char con[] = "con:";
		char con1[] = "con:";
		char con2[] = "con:";
	
		int vfs_retval0 = vfs_open(con, O_RDONLY, 0064, &v0);
		curproc->file_table[STDIN_FILENO]->vnode = v0;
		curproc->file_table[STDIN_FILENO]->flags = O_RDONLY;
		curproc->file_table[STDIN_FILENO]->count = 1;
		curproc->file_table[STDIN_FILENO]->offset = 0;
		curproc->file_table[STDIN_FILENO]->lock = lock_create(con);
		
		int vfs_retval1 = vfs_open(con1, O_WRONLY, 0064, &v1);
                curproc->file_table[STDOUT_FILENO]->vnode = v0;
                curproc->file_table[STDOUT_FILENO]->flags = O_WRONLY;
                curproc->file_table[STDOUT_FILENO]->count = 1;
                curproc->file_table[STDOUT_FILENO]->offset = 0;
                curproc->file_table[STDOUT_FILENO]->lock = lock_create(con1);

        	int vfs_retval2 = vfs_open(con2, O_WRONLY, 0064, &v2);
	        curproc->file_table[STDERR_FILENO]->vnode = v0;
        	curproc->file_table[STDERR_FILENO]->flags = O_WRONLY;
	        curproc->file_table[STDERR_FILENO]->count = 1;
	        curproc->file_table[STDERR_FILENO]->offset = 0;
	        curproc->file_table[STDERR_FILENO]->lock = lock_create(con2);
		
		curproc->fd = 3;

		if(vfs_retval0)
			return vfs_retval0;
		if(vfs_retval1)
			return vfs_retval1;
		if(vfs_retval2)
			return vfs_retval2;
        }
	
	while(curproc->fd < OPEN_MAX){
		curproc->file_table[curproc->fd] = NULL;
		curproc->fd++;
	}

	return 0;
}
