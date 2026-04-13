#ifndef PROC_H
#define PROC_H

#include "riscv.h"
#include "types.h"

#define NPROC (512)
#define FD_BUFFER_SIZE (16)

#define MAX_SYSCALL_NUM 500

#define BIG_STRIDE 65536 // to reduce integer division errors

struct file;

// Saved registers for kernel context switches.
struct context {
	uint64 ra;
	uint64 sp;

	// callee-saved
	uint64 s0;
	uint64 s1;
	uint64 s2;
	uint64 s3;
	uint64 s4;
	uint64 s5;
	uint64 s6;
	uint64 s7;
	uint64 s8;
	uint64 s9;
	uint64 s10;
	uint64 s11;
};

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// Per-process state
struct proc {
	enum procstate state; // Process state
	int pid; // Process ID
	pagetable_t pagetable; // User page table
	uint64 ustack; // Virtual address of kernel stack
	uint64 kstack; // Virtual address of kernel stack
	struct trapframe *trapframe; // data page for trampoline.S
	struct context context; // swtch() here to run process
	uint64 max_page;

	unsigned int syscall_times[MAX_SYSCALL_NUM]; // record the times of each system call
	uint64 start_time; // record the start time of the process	


	struct proc *parent; // Parent process
	uint64 exit_code;
	struct file *files
		[FD_BUFFER_SIZE]; //File descriptor table, using to record the files opened by the process

	// added for stride scheduling
	int priority; // priority of the process, higher means more CPU time
	int stride; // tracks how much CPU time the process has used
};

typedef enum{
	UnInit,
	Ready,
	Running,
	Exited,
} TaskStatus;

typedef struct{
	TaskStatus status; // current task state
	// Count how many times this process has invoked each syscall.
	unsigned int syscall_times[MAX_SYSCALL_NUM];
	int time; // running time in ms
} TaskInfo;

int cpuid();
struct proc *curr_proc();
void exit(int);
void proc_init();
void scheduler() __attribute__((noreturn));
void sched();
void yield();
int fork();
int spawn(char *filename);
int exec(char *, char **);
int wait(int, int *);
void add_task(struct proc *);
struct proc *pop_task();
struct proc *allocproc();
int fdalloc(struct file *);
int init_stdio(struct proc *);
int push_argv(struct proc *, char **);
// swtch.S
void swtch(struct context *, struct context *);

#endif // PROC_H