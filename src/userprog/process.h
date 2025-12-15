#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include <stdint.h>
#include <stdbool.h>
#include "userprog/fdtable.h"
#include "userprog/sync_table.h"
#include "threads/thread.h"
#include "userprog/child_proc.h"

// At most 8MB can be allocated to the stack
// These defines will be used in Project 2: Multithreading
#define MAX_STACK_PAGES (1 << 11)
#define MAX_THREADS 127

/* Filesys lock */
extern struct lock filesys_lock;

/* PIDs and TIDs are the same type. PID should be
   the TID of the main thread of the process */
typedef tid_t pid_t;

/* Thread functions (Project 2: Multithreading) */
typedef void (*pthread_fun)(void*);
typedef void (*stub_fun)(pthread_fun, void*);
/* Pthread aux for data convoying */
struct pthread_aux {
  stub_fun sfun;
  pthread_fun tfun;
  void* arg;
  struct process* pcb;
  int is_user_thread;
};

/* The process control block for a given process. Since
   there can be multiple threads per process, we need a separate
   PCB from the TCB. All TCBs in a process will have a pointer
   to the PCB, and the PCB will have a pointer to the main thread
   of the process, which is `special`. */
struct process {
  /* Owned by process.c. */
  uint32_t* pagedir;          /* Page directory. */
  char process_name[16];      /* Name of the main thread */
  struct thread* main_thread; /* Pointer to main thread */

  /* User Program */
  fdtable* fdtable;
  struct list children; // child processes
  struct child_proc* parent_proc;
  struct file* exec_file;

  /* Multi-threading */
  struct list user_threads; // list of tes
  size_t user_thread_count;
  size_t next_free_page;      // Next page to allocate for child thread
  struct lock free_page_lock; // Lock when assigning new free page
  struct lock child_global_lock;
  /* Pthread Sync */
  struct sync_table* sync_table;
  struct semaphore process_exit_sema;
  bool killed;
  uint32_t gen;
};

void userprog_init(void);

pid_t process_execute(const char* file_name);
int process_wait(pid_t);
void process_exit(void);
void process_activate(void);
pid_t process_fork(struct intr_frame* f);

bool is_main_thread(struct thread*, struct process*);
pid_t get_pid(struct process*);

tid_t pthread_execute(stub_fun, pthread_fun, void*);
tid_t pthread_join(tid_t);
void pthread_exit(void);
void pthread_exit_main(void);

thread_func start_pthread NO_RETURN;

struct process* process_current(void);

#endif /* userprog/process.h */
