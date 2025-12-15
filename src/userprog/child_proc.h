#ifndef USERPROG_CHILD_PROC_H
#define USERPROG_CHILD_PROC_H

#include "threads/synch.h"
#include "filesys/file.h"
#include "lib/kernel/bitmap.h"
#include <stdbool.h>
#include <stddef.h>

typedef int tid_t;
typedef struct child_proc {
  tid_t tid;
  int exit_status;
  bool waited; // if already waited by some parent
  bool exited; // if this process already exited

  struct semaphore wait_sema; // to wait for children
  struct list_elem elem;      // for instrusive linkedlist
  struct process* parent_process;

  bool load_ok; // whether load ELF success, used for child's process exit status; true default
  struct semaphore load_sema; // 只用于 exec 等待“加载完成”
};

typedef struct fork_proc {
  tid_t tid;

  char* file_name;
  uint32_t* pagedir;
  fdtable* fdtable;
  struct intr_frame* if_;

  bool load_ok; // whether load ELF success, used for child's process exit status; true default
  struct child_proc* cp;
};

void create_child_proc(struct child_proc* cp);
struct child_proc* find_child_with_tid(struct process* cur_proc, tid_t tid);
// void free_child_proc(struct child_proc *cp);

void create_fork_proc(struct fork_proc* fp);
void free_child_proc(struct child_proc* cp);
void free_fork_proc(struct fork_proc* fp);

#endif /* USERPROG_CHILD_PROC_H */