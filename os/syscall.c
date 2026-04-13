#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"

#include "proc.h"
#include "vm.h"

uint64 sys_mmap(uint64 start, uint64 len, int port, int flag, int fd)
{
	struct proc *p = curr_proc();
	if (!p) return -1;

	// A zero-length mapping is treated as a successful no-op.
	if (len == 0) return 0;

	// The start address must be page-aligned.
	if (start % PGSIZE != 0) return -1; // start must be page-aligned
	if (len > (1UL << 30)) return -1; // limit the maximum mapping size to 1GB
	if ((port & ~0x7) != 0) return -1; // prot must be a combination of PROT_READ, PROT_WRITE, PROT_EXEC
	if ((port & 0X7) == 0) return -1; // at least one of PROT_READ, PROT_WRITE, PROT_EXEC must be set

	// Round the requested length up to a whole number of pages.
	uint64 sz = PGROUNDUP(len);

	// Build the PTE permission flags
	// PTE_U is required so the mapped pages are accessible in user mode.
	int perm = PTE_U;
	if (port & 0x1) perm |= PTE_R;
	if (port & 0x2) perm |= PTE_W;
	if (port & 0x4) perm |= PTE_X;

	// first pass: make sure no page is already mapped
	for (uint64 va = start; va < start + sz; va += PGSIZE) {
		pte_t *pte = walk(p->pagetable, va, 0);
		if (pte && (*pte & PTE_V) != 0) {
			return -1; // page already mapped
		}
	}

	// second pass: allocate and map page by page
	for (uint64 va = start; va < start + sz; va += PGSIZE) {
		void *pa = kalloc();
		if (pa == 0) {
			uint64 done = (va - start) / PGSIZE;
			if (done > 0) {
				uvmunmap(p->pagetable, start, done, 1); // unmap and free already mapped pages
			}
			return -1; // allocation failed
		}
		
		// Zero-fill the new page so the anonymous mapping starts clean.
		memset(pa, 0, PGSIZE);
		
		// Map the new physical page into the process page table.
		if (mappages(p->pagetable, va, PGSIZE, (uint64)pa, perm) < 0) {
			kfree(pa);
			
			// Clean up pages already mapped before returning failure.
			uint64 done = (va - start) / PGSIZE;
			if (done > 0) {
				uvmunmap(p->pagetable, start, done, 1); // un
			}
			return -1; // mapping failed
		}
	}
	return 0;	

}
	

uint64 sys_munmap(uint64 start, uint64 len)
{
	struct proc *p = curr_proc();
	if (!p) return -1;

	// A zero-length unmap is treated as a succesful no-op.
	if (len == 0) return 0;


	if (start % PGSIZE != 0) return -1; // start must be page-aligned

	// Round the length up so we unmap the full pages.
	uint64 sz = PGROUNDUP(len);
	uint64 npages = sz / PGSIZE;

	// verify the whole range is mapped first
	for (uint64 va = start; va < start + sz; va += PGSIZE) {
		pte_t *pte = walk(p->pagetable, va, 0);
		if (pte == 0 || (*pte & PTE_V) == 0) {
			return -1; // page not mapped
		}
	}

	// Remove the mappings and free the underlying physical pages.
	uvmunmap(p->pagetable, start, npages, 1); // unmap and free physical pages
	return 0;
}

uint64 console_write(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	tracef("write size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return len;
}

uint64 console_read(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	tracef("read size = %d", len);
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
}

uint64 sys_write(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_write(va, len);
	case FD_INODE:
		return inodewrite(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_read(va, len);
	case FD_INODE:
		return inoderead(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
}

__attribute__((noreturn)) void sys_exit(int code)
{
	exit(code);
	__builtin_unreachable();
}

uint64 sys_sched_yield()
{
	yield();
	return 0;
}

uint64 sys_gettimeofday(uint64 val, int _tz)
{
	struct proc *p = curr_proc(); // gets the currently running process
	if (!p || !val) return -1; // Return error if no process is running or the provided pointer is invalid

	// Translate the user virtual address into a kernel-usable physical address.
	uint64 dst = useraddr(p->pagetable, (uint64)val);
	if (dst == 0) return -1;


	uint64 cycle = get_cycle();
	TimeVal t;
	t.sec = cycle / CPU_FREQ;
	t.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	copyout(p->pagetable, val, (char *)&t, sizeof(TimeVal));

	// Write the result into user memory.
	*(TimeVal *)dst = t;

	return 0;
}


uint64 sys_task_info(TaskInfo *ti);
uint64 sys_mmap(uint64 start, uint64 len, int prot, int flag, int fd);
uint64 sys_munmap(uint64 start, uint64 len);


uint64 sys_getpid()
{
	return curr_proc()->pid;
}

uint64 sys_getppid()
{
	struct proc *p = curr_proc();
	return p->parent == NULL ? IDLE_PID : p->parent->pid;
}

uint64 sys_clone()
{
	debugf("fork!");
	return fork();
}

static inline uint64 fetchaddr(pagetable_t pagetable, uint64 va)
{
	uint64 *addr = (uint64 *)useraddr(pagetable, va);
	return *addr;
}

uint64 sys_exec(uint64 path, uint64 uargv)
{
	struct proc *p = curr_proc();
	char name[MAX_STR_LEN];
	copyinstr(p->pagetable, name, path, MAX_STR_LEN);
	uint64 arg;
	static char strpool[MAX_ARG_NUM][MAX_STR_LEN];
	char *argv[MAX_ARG_NUM];
	int i;
	for (i = 0; uargv && (arg = fetchaddr(p->pagetable, uargv));
	     uargv += sizeof(char *), i++) {
		copyinstr(p->pagetable, (char *)strpool[i], arg, MAX_STR_LEN);
		argv[i] = (char *)strpool[i];
	}
	argv[i] = NULL;
	return exec(name, (char **)argv);
}

uint64 sys_wait(int pid, uint64 va)
{
	struct proc *p = curr_proc();
	int *code = (int *)useraddr(p->pagetable, va);
	return wait(pid, code);
}

// System call wrapper for spawn
uint64 sys_spawn(uint64 va)
{
	// TODO: your job is to complete the sys call
	struct proc *p = curr_proc(); // get the current process
	char filename[200]; // buffer to hold the filename, assuming max length is 200

	// Copy the filename from user space to kernel space
	if (copyinstr(p->pagetable, filename, va, 200) < 0){
		return -1;
	}
	// Call the spawn function with the filename and return its result
	return spawn(filename);
}

// System call to set the process priority
uint64 sys_set_priority(long long prio)
{
	// TODO: your job is to complete the sys call
	struct proc *p = curr_proc(); // get the current process
	if (p == 0) return -1; // Return error if no process is running
	
	// Priority must be >= 2 
	if (prio < 2) return -1;

	p->priority = prio; // update process priority
    return prio; // Return the new priority
}

uint64 sys_openat(uint64 va, uint64 omode, uint64 _flags)
{
	struct proc *p = curr_proc();
	char path[200];
	copyinstr(p->pagetable, path, va, 200);
	return fileopen(path, omode);
}

uint64 sys_close(int fd)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d", fd);
		return -1;
	}
	fileclose(f);
	p->files[fd] = 0;
	return 0;
}

int sys_fstat(int fd,uint64 stat){
	//TODO: your job is to complete the syscall
	struct proc *p = curr_proc(); // get the current process

	// Validate the file descriptor
    if (fd < 0 || fd >= FD_BUFFER_SIZE) {
        return -1; // Invalid file descriptor
    }

	// Validate the stat pointer
    struct file *f = p->files[fd];
    if (f == 0) {
        return -1;
    }

    return filestat(f, stat); // Call the filestat function to fill the stat structure and return its result
}

int sys_linkat(int olddirfd, uint64 oldpath, int newdirfd, uint64 newpath, uint64 flags){
	//TODO: your job is to complete the syscall
	struct proc *p = curr_proc(); // get the current process
    char old[MAX_STR_LEN], new[MAX_STR_LEN]; // buffers to hold the old and new paths, assuming max length is MAX_STR_LEN

	// Copy the old and new paths from user space to kernel space
    if (copyinstr(p->pagetable, old, oldpath, MAX_STR_LEN) < 0) {
        return -1;
    }

    if (copyinstr(p->pagetable, new, newpath, MAX_STR_LEN) < 0) {
        return -1;
    }

    return filelink(old, new); // Call the filelink function to create a new link and return its result
}

int sys_unlinkat(int dirfd, uint64 path, uint64 flags){
	//TODO: your job is to complete the syscall
	struct proc *p = curr_proc(); // get the current process
    char name[MAX_STR_LEN]; // buffer to hold the filename, assuming max length is MAX_STR_LEN

	// Copy the filename from user space to kernel space
    if (copyinstr(p->pagetable, name, path, MAX_STR_LEN) < 0) {
        return -1; 
    }

    return fileunlink(name); // Call the fileunlink function to remove the file and return its result
}

extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);

	// Count how many times each syscall has been used by this process
	if (id < MAX_SYSCALL_NUM) {
		curr_proc()->syscall_times[id]++;
	}

	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_read:
		ret = sys_read(args[0], args[1], args[2]);
		break;
	case SYS_openat:
		ret = sys_openat(args[0], args[1], args[2]);
		break;
	case SYS_close:
		ret = sys_close(args[0]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday(args[0], args[1]);
		break;
	case SYS_getpid:
		ret = sys_getpid();
		break;
	case SYS_getppid:
		ret = sys_getppid();
		break;
	case SYS_clone: // SYS_fork
		ret = sys_clone();
		break;
	case SYS_execve:
		ret = sys_exec(args[0], args[1]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_fstat:
	    ret = sys_fstat(args[0],args[1]);
		break;
	case SYS_linkat:
	    ret = sys_linkat(args[0],args[1],args[2],args[3],args[4]);
		break;
	case SYS_unlinkat:
	    ret = sys_unlinkat(args[0],args[1],args[2]);
		break;
	case SYS_spawn:
		ret = sys_spawn(args[0]);
		break;

	// Return task status, syscall stats, and running time.
	case SYS_task_info:
		ret = sys_task_info((TaskInfo *)args[0]);
		break;

	// Create an anonmyous memory mapping in user space.
	case SYS_mmap:
		ret = sys_mmap(args[0], args[1], args[2], args[3], args[4]);
		break;

	// Remove a previously mapped user-space memory region.
	case SYS_munmap:
		ret = sys_munmap(args[0], args[1]);
		break;

	case SYS_setpriority:
		ret = sys_set_priority(args[0]);
		break;


	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}

uint64 sys_task_info(TaskInfo *ti)
{
	// Get the current running process
	struct proc *p = curr_proc();

	// Return error if no process is running or the provided pointer is invalid
	if (!p || !ti) return -1;

	// Translate the user virtual address into a kernel-usable physical address.
	uint64 dst = useraddr(p->pagetable, (uint64)ti);
	if (dst == 0) return -1;

	// Since we are querying the current task, we set the status to Running
	TaskInfo info;
	info.status = Running;

	// Copy the syscall times and calculate the running time of the process
	for (int i = 0; i < MAX_SYSCALL_NUM; i++) {
		info.syscall_times[i] = p->syscall_times[i];
	}

	// Calculate the running time of the process in milliseconds
	uint64 now = get_cycle();
	info.time = (int)((now - p->start_time) * 1000 / CPU_FREQ);

	*(TaskInfo *)dst = info;
	return 0; // Success
}