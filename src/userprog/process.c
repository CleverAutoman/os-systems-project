#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/fdtable.h"

static struct semaphore temporary;
static thread_func start_process NO_RETURN;
static thread_func start_pthread NO_RETURN;
static bool load(const char* file_name, void (**eip)(void), void** esp);
bool setup_thread(void (**eip)(void), void** esp);
static size_t file_name_length(const char* file_name);
static char** split_file_name(const char* file_name, size_t ct);

/* load helpers */
static bool install_page(void* upage, void* kpage, bool writable);

struct execute_aux {
  struct child_proc* parent_proc;
  char* file_name;
};

/* Initializes user programs in the system by ensuring the main
   thread has a minimal PCB so that it can execute and wait for
   the first user process. Any additions to the PCB should be also
   initialized here if main needs those members */
void userprog_init(void) {
  struct thread* t = thread_current();
  bool success;

  /* Allocate process control block
     It is imoprtant that this is a call to calloc and not malloc,
     so that t->pcb->pagedir is guaranteed to be NULL (the kernel's
     page directory) when t->pcb is assigned, because a timer interrupt
     can come at any time and activate our pagedir */
  t->pcb = calloc(sizeof(struct process), 1);
  success = t->pcb != NULL;

  /* initiliaze children lists */
  ASSERT(&t->pcb->children != NULL);
  list_init(&t->pcb->children);

  /* Kill the kernel if we did not succeed */
  ASSERT(success);
}

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   process id, or TID_ERROR if the thread cannot be created. */
pid_t process_execute(const char* file_name) {
  // printf("[MEM] free pages=%zu\n", palloc_get_free_cnt());
  char* fn_copy;
  tid_t tid;

  sema_init(&temporary, 0);
  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page(0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy(fn_copy, file_name, PGSIZE);

  /* 1. get the first token as file_name */
  size_t i = 0;
  for (; fn_copy[i] != '\0'; i++) {
    if (isspace((unsigned char)fn_copy[i])) {
      break;
    }
  }
  char* file_name_ = malloc(i + 1);
  if (i != 0) {
    memcpy(file_name_, &fn_copy[0], i);
    file_name_[i] = '\0';
  }

  /* 2. initialize child_proc for children processes  */
  struct child_proc* cp = calloc(sizeof(struct child_proc), 1);
  create_child_proc(cp);
  cp->refcnt += 1;

  struct execute_aux* e_aux = calloc(sizeof(struct execute_aux), 1);
  e_aux->file_name = fn_copy;
  e_aux->parent_proc = cp;

  /* 3. Create a new thread to execute FILE_NAME. */
  tid = thread_create(file_name_, PRI_DEFAULT, start_process, e_aux);
  sema_down(&cp->load_sema);
  if (!cp->load_ok) {
    // printf("load failed\n");
    sema_up(&cp->wait_sema);
    return -1;
  }

  /* 4. Push child_proc into cur_proc's children list. */
  cp->tid = tid;
  list_push_front(&process_current()->children, &cp->elem);
  struct process* p = process_current();
  // printf("EXEC parent pcb=%p children=%p pushed cp=%p elem=%p tid=%d\n",
  //      p, &p->children, cp, &cp->elem, tid);

  struct list_elem* e;
  for (e = list_begin(&process_current()->children); e != list_end(&process_current()->children);
       e = list_next(e)) {
    struct child_proc* c_proc = list_entry(e, struct child_proc, elem);
  }

  /* 5. reach parent process end && refct -= 1 && free */
  if (tid == TID_ERROR)
    palloc_free_page(fn_copy);

  free(file_name_);
  cp->refcnt -= 1;
  if (cp->refcnt == 0) {
    free(cp);
  }
  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void start_process(void* aux) {
  struct execute_aux* e_aux = (struct execute_aux*)aux;

  // char* file_name = palloc_get_page(0);
  // strlcpy(file_name, e_aux->file_name, strlen(e_aux->file_name) + 1);
  char* file_name = e_aux->file_name;
  printf("aux before: %p\n", file_name);

  struct child_proc* cp = e_aux->parent_proc;
  cp->refcnt += 1;
  // free(e_aux);

  struct thread* t = thread_current();
  struct intr_frame if_;
  bool success, pcb_success;

  /* Allocate process control block */
  struct process* new_pcb = calloc(sizeof(struct process), 1);
  success = pcb_success = new_pcb != NULL;

  /* Initialize process control block */
  if (success) {
    // Ensure that timer_interrupt() -> schedule() -> process_activate()
    // does not try to activate our uninitialized pagedir
    new_pcb->pagedir = NULL;
    t->pcb = new_pcb;

    // Continue initializing the PCB as normal
    t->pcb->main_thread = t;
    strlcpy(t->pcb->process_name, file_name, strlen(file_name) + 1);

    /* initialize fdtable */
    t->pcb->fdtable = malloc(sizeof(struct fdtable));
    fdtable_create(t->pcb->fdtable, 512);
    ASSERT(t->pcb->fdtable != NULL);

    /* initiliaze other propertiese */
    list_init(&t->pcb->children);
    ASSERT(&t->pcb->children != NULL);

    /* allocate parent */
    t->pcb->parent_proc = cp;
  }

  /* argument passing */
  /* 1. split file_name and args if_.esp == 3222319104 */

  size_t ct = file_name_length(file_name);
  char** str_lst = split_file_name(file_name, ct); // l = ct + 1(NULL) + 1(ct)

  /* Initialize interrupt frame and load executable. */
  if (success) {
    memset(&if_, 0, sizeof if_);
    if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
    if_.cs = SEL_UCSEG;
    if_.eflags = FLAG_IF | FLAG_MBS;
    success = load(str_lst[0], &if_.eip, &if_.esp);
    if (!success) {
      printf("failed\n");

      if (str_lst != NULL) {
        for (int i = 0; str_lst[i] != NULL; i++)
          free(str_lst[i]);
        free(str_lst);
      }
      cp->load_ok = false;
      sema_up(&cp->load_sema);
      process_exit();
    }
    // success = load(file_name, &if_.eip, &if_.esp);
  }
  printf("reached2\n");

  /* Handle failure with succesful PCB malloc. Must free the PCB */
  if (!success && pcb_success) {
    // Avoid race where PCB is freed before t->pcb is set to NULL
    // If this happens, then an unfortuantely timed timer interrupt
    // can try to activate the pagedir, but it is now freed memory

    struct process* pcb_to_free = t->pcb;
    t->pcb = NULL;
    free(pcb_to_free);
  }

  sema_up(&cp->load_sema);

  /* 2. write data into stack esp == 3221225472 */
  // move stack ptr
  size_t capacity = 0;

  for (int i = 0; i < ct; i++) {
    capacity += strlen(str_lst[i]) + 1;
  }
  // track addr of each var
  char* arg_addr[ct];

  if_.esp -= capacity;
  uint8_t* pt = (uint8_t*)if_.esp;
  // write on the stack
  for (int i = 0; i < ct; i++) {
    size_t l = strlen(str_lst[i]) + 1;
    memcpy(pt, str_lst[i], l);
    arg_addr[i] = (char*)pt;
    pt += l;
  }

  /* 3. padding in 4-byte */
  while (((uintptr_t)if_.esp) % 4 != 0) { // 3221225458
    if_.esp = (uint8_t*)if_.esp - 1;
    *(uint8_t*)if_.esp = 0;
  }

  /* 4. allocate stack for address and fill them 
        16-byte padding before argc and &argv */
  if_.esp -= (ct + 1) * sizeof(void*); // NULL ptr
  pt = (uint8_t*)if_.esp;
  for (int i = 0; i < ct; i++) {
    memcpy(pt, &arg_addr[i], sizeof(void*));
    pt += sizeof(void*);
  }
  *(void**)pt = NULL;          // fill NULL ptr
  void* argv = (void*)if_.esp; // track &argv

  // padding in 16-byte padding
  while (((uintptr_t)if_.esp - 2 * sizeof(void*)) % 16 != 0) {
    if_.esp = (uint8_t*)if_.esp - 1;
    *(uint8_t*)if_.esp = 0;
  }

  // fill &argv and argc
  if_.esp -= sizeof(void*);
  *(void**)if_.esp = argv;

  if_.esp -= sizeof(int);
  *(int*)if_.esp = ct;

  if_.esp -= sizeof(void*);
  *(void**)if_.esp = 0;

  // if_.esp -= sizeof(void*);

  /* Clean up. Exit on failure or jump to userspace */
  // printf("start_thread success with name = %s\n", file_name);
  // palloc_free_page(file_name);

  // // clean child_process
  // cp->refcnt -= 1;
  // if (cp->refcnt == 0) {
  //   free(cp);
  // }
  if (!success) {
    sema_up(&temporary);

    if (str_lst != NULL) {
      for (int i = 0; str_lst[i] != NULL; i++)
        free(str_lst[i]);
      free(str_lst);
    }
    palloc_free_page(e_aux->file_name);
    thread_exit();
  }

  if (str_lst != NULL) {
    for (int i = 0; str_lst[i] != NULL; i++)
      free(str_lst[i]);
    free(str_lst);
  }

  printf("aux after: %p\n", e_aux->file_name);
  palloc_free_page(e_aux->file_name);
  printf("reached\n");

  // hex_dump((uintptr_t)if_.esp, (void*)if_.esp, 512, true);

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&if_) : "memory");
  NOT_REACHED();
}

static size_t file_name_length(const char* file_name) {
  size_t ct = 0;
  bool has_word = false;
  for (size_t i = 0; file_name[i] != '\0'; i++) {
    if (isspace((unsigned char)file_name[i])) {
      if (has_word) {
        ct++;
        has_word = false;
      }
    } else {
      has_word = true;
    }
  }
  if (has_word)
    ct++;
  return ct;
}

static char** split_file_name(const char* file_name, size_t ct) {
  char** res = malloc((ct + 1) * sizeof(char*));
  size_t index = 0;
  if (!res) {
    printf("ERROR: malloc list failed!\n");
    return NULL;
  }

  size_t slow = 0;
  bool has_word = false;
  for (size_t i = 0; file_name[i] != '\0'; i++) {
    if (isspace((unsigned char)file_name[i])) {
      if (has_word) {
        // copy into list
        size_t len = i - slow;
        char* cur_str = malloc(len + 1);
        if (!cur_str) {
          printf("ERROR: malloc cur_Str failed!\n");
          return NULL;
        }
        // fill '\0'
        memcpy(cur_str, &file_name[slow], len);
        cur_str[len] = '\0';
        res[index++] = cur_str;
        has_word = false;
      }
      slow = i + 1;
    } else {
      has_word = true;
    }
  }
  size_t total_len = strlen(file_name);
  if (has_word && slow < total_len) {
    size_t len = total_len - slow;
    char* cur_str = malloc(len + 1);
    if (!cur_str) {
      printf("ERROR: malloc cur_str failed!\n");
      return NULL;
    }

    memcpy(cur_str, file_name + slow, len);
    cur_str[len] = '\0';
    res[index++] = cur_str;
  }
  res[index] = NULL;
  return res;
}

/* Waits for process with PID child_pid to die and returns its exit status.
   If it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If child_pid is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given PID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int process_wait(pid_t child_pid UNUSED) {
  /* check within children, whether has this child_pid */
  struct process* cur_proc = process_current();

  struct list_elem* e;
  for (e = list_begin(&process_current()->children); e != list_end(&process_current()->children);
       e = list_next(e)) {
    struct child_proc* c_proc = list_entry(e, struct child_proc, elem);
  }

  /* If this thread does not have a PCB, don't worry */
  if (cur_proc == NULL) {
    thread_exit();
    NOT_REACHED();
  }

  /* check within children, whether has this child_pid */
  struct child_proc* child_proc = find_child_with_tid(cur_proc, child_pid);
  if (!child_proc) {
    return -1;
  }
  if (child_proc->waited) {
    return -1;
  }

  /* start to wait */
  child_proc->refcnt += 1;
  child_proc->waited = true;
  sema_down(&child_proc->wait_sema);
  int exit_status = child_proc->exit_status;
  // printf("parent first get exit#: %d\n", exit_status);

  child_proc->refcnt -= 1;
  // clean list
  list_remove(&child_proc->elem);

  /* return child_p's exitStatus */
  if (child_proc->refcnt == 0) {
    free(child_proc);
  }
  if (child_proc->exit_status == -129) {
    printf("ERROR, waiting process hasn't changed its exit_code\n");
    return -1;
  }
  printf("parent return with exit#: %d\n", exit_status);
  return exit_status;
}

static bool copy_pagedir(uint32_t* child_pd, uint32_t* parent_pd) {
  ASSERT(parent_pd);
  ASSERT(child_pd);

  for (uint8_t* upage = 0; upage < (uint8_t*)PHYS_BASE; upage += PGSIZE) {
    void* parent_kpage = pagedir_get_page(parent_pd, upage);
    if (parent_kpage == NULL)
      continue;

    void* child_kpage = palloc_get_page(PAL_USER);
    if (child_kpage == NULL)
      return false;

    memcpy(child_kpage, parent_kpage, PGSIZE);

    bool writable = true;

    if (!pagedir_set_page(child_pd, upage, child_kpage, writable)) {
      palloc_free_page(child_kpage);
      return false;
    }
  }
  return true;
}

static void start_fork(void* aux) { // self->eax = 0
  struct fork_proc* fp = (struct fork_proc*)aux;
  bool success;
  bool pcb_success;
  struct thread* t = thread_current();

  /* 1. copy data */
  char* file_name = fp->file_name;
  uint32_t* pagedir = fp->pagedir;
  fdtable* fdtable = fp->fdtable;
  struct intr_frame* if_ = fp->if_;

  // printf("PARENT info passed in: filename=%s fdtable=%p files[2]=%p\n",
  // file_name,
  // fdtable,
  // fdtable->files[2]);

  /* Allocate process control block */
  struct process* new_pcb = calloc(sizeof(struct process), 1);
  success = pcb_success = new_pcb != NULL;

  /* Initialize process control block */
  if (success) {
    // Ensure that timer_interrupt() -> schedule() -> process_activate()
    // does not try to activate our uninitialized pagedir
    new_pcb->pagedir = NULL;

    // Continue initializing the PCB as normal
    t->pcb = new_pcb;
    t->pcb->main_thread = t;
    strlcpy(t->pcb->process_name, file_name, strlen(file_name) + 1);

    /* initialize fdtable */
    t->pcb->fdtable = malloc(sizeof(struct fdtable));
    fdtable_create(t->pcb->fdtable, 512);
    //     printf("DEBUG child raw fdtable=%p size=%zu\n",
    //        t->pcb->fdtable, sizeof(struct fdtable));
    // printf("DEBUG child fdtable->files=%p bitmap=%p next_index(before copy)=%d\n",
    //        t->pcb->fdtable->files,
    //        t->pcb->fdtable->bitmap,
    //        t->pcb->fdtable->next_index);

    ASSERT(t->pcb->fdtable != NULL);

    /* initiliaze other propertiese */
    list_init(&t->pcb->children);
    ASSERT(&t->pcb->children != NULL);

    t->pcb->parent_proc = fp->cp;
  }

  /* Handle failure with succesful PCB malloc. Must free the PCB */
  if (!success && pcb_success) {
    // Avoid race where PCB is freed before t->pcb is set to NULL
    // If this happens, then an unfortuantely timed timer interrupt
    // can try to activate the pagedir, but it is now freed memory

    struct process* pcb_to_free = t->pcb;
    t->pcb = NULL;
    free(pcb_to_free);
    fp->load_ok = false;
    sema_up(&fp->load_sema);
    process_exit();
  }

  /* 2. copy all data */
  // copy pagedir
  t->pcb->pagedir = pagedir_create();
  if (t->pcb->pagedir == NULL) {
    struct process* pcb_to_free = t->pcb;
    t->pcb = NULL;
    free(pcb_to_free);

    fp->load_ok = false;
    sema_up(&fp->load_sema);
    process_exit();
  }
  if (!copy_pagedir(t->pcb->pagedir, pagedir)) {
    // clean up (free pages you've already added if you track them,
    // or just kill the child for now)
    fp->load_ok = false;
    sema_up(&fp->load_sema);
    process_exit();
  }

  process_activate();
  // copy fdtable
  fork_fdtable(t->pcb->fdtable, fdtable);
  // printf("DEBUG after fork_fdtable: child fdtable->files[2]=%p parent fdtable->files[2]=%p\n",
  //      t->pcb->fdtable->files[2],
  //      fdtable->files[2]);

  sema_up(&fp->load_sema);

  // copy frame & set eas = 0
  struct intr_frame fork_if_ = *if_;
  fork_if_.eax = 0;

  // printf("FINAL child setup: pid=%d pcb=%p fdtable=%p files[2]=%p\n",
  //     t->tid,
  //     t->pcb,
  //     t->pcb->fdtable,
  //     t->pcb->fdtable->files[2]);

  /* 3. start to proceed */
  asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&fork_if_) : "memory");
  NOT_REACHED();
}

pid_t process_fork(struct intr_frame* f) {
  /* 1. create child process with aux including pagedir, fdtable, if_ */
  // get file_name...
  tid_t tid;

  struct process* cur_proc = process_current();

  struct child_proc* cp = calloc(sizeof(struct child_proc), 1);
  create_child_proc(cp);
  list_push_front(&process_current()->children, &cp->elem);

  char* file_name = cur_proc->process_name;
  uint32_t* pagedir = cur_proc->pagedir;
  fdtable* fdtable = cur_proc->fdtable;

  // assemble fork_aux
  struct fork_proc* fp = malloc(sizeof(struct fork_proc));
  create_fork_proc(fp);
  fp->file_name = file_name;
  fp->pagedir = pagedir;
  fp->fdtable = fdtable;
  fp->if_ = f;
  fp->cp = cp;

  //   printf("DEBUG parent pcb=%p fdtable=%p\n", cur_proc, cur_proc->fdtable);
  // printf("DEBUG parent fdtable struct size=%zu\n", sizeof(struct fdtable));
  // printf("DEBUG parent fdtable->files=%p bitmap=%p next_index=%d\n",
  //        cur_proc->fdtable->files,
  //        cur_proc->fdtable->bitmap,
  //        cur_proc->fdtable->next_index);

  tid = thread_create(file_name, PRI_DEFAULT, start_fork, fp);
  if (tid == TID_ERROR) {
    // free_fork_proc(fp);
    list_remove(&cp->elem);
    free(cp);
    free_fork_proc(fp);
    return -1;
  }

  /* 2. wait for fork load success */
  sema_down(&fp->load_sema);

  if (!fp->load_ok) {
    // free_fork_proc(fp);
    list_remove(&cp->elem);
    free(cp);
    free_fork_proc(fp);
    return -1;
  }

  /* 3. add child proc into waiting list */

  cp->tid = tid;

  free_fork_proc(fp);
  // printf("parent success\n");
  /* 3. return tid or -1 */
  return tid;
}

/* Free the current process's resources. */
void process_exit(void) {
  struct thread* cur = thread_current();
  uint32_t* pd;

  // 在实现 exit(status) 的地方
  printf("[EXIT] %s(pid=%d) status=%d\n", thread_current()->name, thread_current()->tid,
         cur->exit_status);

  /* If this thread does not have a PCB, don't worry */
  if (cur->pcb == NULL) {
    thread_exit();
    NOT_REACHED();
  }

  if (cur->exit_status < 0)
    cur->exit_status = -1;
  // printf("%s: exit(%d) @ thread: %d\n", cur->name, cur->exit_status, cur->tid);
  printf("%s: exit(%d)\n", cur->name, cur->exit_status);

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pcb->pagedir;
  if (pd != NULL) {
    /* Correct ordering here is crucial.  We must set
         cur->pcb->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
    cur->pcb->pagedir = NULL;
    pagedir_activate(NULL);
    pagedir_destroy(pd);
  }

  if (cur->pcb->fdtable) {
    free_table(cur->pcb->fdtable);
  }

  /* Free the PCB of this process and kill this thread
     Avoid race where PCB is freed before t->pcb is set to NULL
     If this happens, then an unfortuantely timed timer interrupt
     can try to activate the pagedir, but it is now freed memory */
  if (cur->pcb->parent_proc) {
    /* notify parent if have one */
    struct child_proc* parent_proc = cur->pcb->parent_proc;
    printf("gonna notify parent with exit#: %d\n", cur->exit_status);
    parent_proc->exit_status = cur->exit_status;
    sema_up(&parent_proc->wait_sema);
  }

  if (cur->pcb->exec_file) {
    file_allow_write(cur->pcb->exec_file);
    file_close(cur->pcb->exec_file);
    cur->pcb->exec_file = NULL;
  }

  struct process* pcb_to_free = cur->pcb;

  /* start to free current proc */
  cur->pcb = NULL;
  free(pcb_to_free);

  sema_up(&temporary);
  thread_exit();
}

/* Sets up the CPU for running user code in the current
   thread. This function is called on every context switch. */
void process_activate(void) {
  struct thread* t = thread_current();

  /* Activate thread's page tables. */
  if (t->pcb != NULL && t->pcb->pagedir != NULL)
    pagedir_activate(t->pcb->pagedir);
  else
    pagedir_activate(NULL);

  /* Set thread's kernel stack for use in processing interrupts.
     This does nothing if this is not a user process. */
  tss_update();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32 /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32 /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32 /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16 /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr {
  unsigned char e_ident[16];
  Elf32_Half e_type;
  Elf32_Half e_machine;
  Elf32_Word e_version;
  Elf32_Addr e_entry;
  Elf32_Off e_phoff;
  Elf32_Off e_shoff;
  Elf32_Word e_flags;
  Elf32_Half e_ehsize;
  Elf32_Half e_phentsize;
  Elf32_Half e_phnum;
  Elf32_Half e_shentsize;
  Elf32_Half e_shnum;
  Elf32_Half e_shstrndx;
};

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr {
  Elf32_Word p_type;
  Elf32_Off p_offset;
  Elf32_Addr p_vaddr;
  Elf32_Addr p_paddr;
  Elf32_Word p_filesz;
  Elf32_Word p_memsz;
  Elf32_Word p_flags;
  Elf32_Word p_align;
};

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL 0           /* Ignore. */
#define PT_LOAD 1           /* Loadable segment. */
#define PT_DYNAMIC 2        /* Dynamic linking info. */
#define PT_INTERP 3         /* Name of dynamic loader. */
#define PT_NOTE 4           /* Auxiliary info. */
#define PT_SHLIB 5          /* Reserved. */
#define PT_PHDR 6           /* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

static bool setup_stack(void** esp);
static bool validate_segment(const struct Elf32_Phdr*, struct file*);
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool load(const char* file_name, void (**eip)(void), void** esp) {
  struct thread* t = thread_current();
  struct Elf32_Ehdr ehdr;
  struct file* file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pcb->pagedir = pagedir_create();
  if (t->pcb->pagedir == NULL)
    goto done;
  process_activate();

  /* Open executable file. */
  file = filesys_open(file_name);
  if (file == NULL) {
    printf("load file failed with name: %s\n", file_name);
    printf("load: %s: open failed\n", file_name);
    goto done;
  }

  printf("load file success with name: %s\n", file_name);

  /* Read and verify executable header. */
  if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr ||
      memcmp(ehdr.e_ident, "\177ELF\1\1\1", 7) || ehdr.e_type != 2 || ehdr.e_machine != 3 ||
      ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Elf32_Phdr) || ehdr.e_phnum > 1024) {
    printf("load: %s: error loading executable\n", file_name);
    goto done;
  }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) {
    struct Elf32_Phdr phdr;

    if (file_ofs < 0 || file_ofs > file_length(file))
      goto done;
    file_seek(file, file_ofs);

    if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
      goto done;
    file_ofs += sizeof phdr;
    switch (phdr.p_type) {
      case PT_NULL:
      case PT_NOTE:
      case PT_PHDR:
      case PT_STACK:
      default:
        /* Ignore this segment. */
        break;
      case PT_DYNAMIC:
      case PT_INTERP:
      case PT_SHLIB:
        goto done;
      case PT_LOAD:
        if (validate_segment(&phdr, file)) {
          bool writable = (phdr.p_flags & PF_W) != 0;
          uint32_t file_page = phdr.p_offset & ~PGMASK;
          uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
          uint32_t page_offset = phdr.p_vaddr & PGMASK;
          uint32_t read_bytes, zero_bytes;
          if (phdr.p_filesz > 0) {
            /* Normal segment.
                     Read initial part from disk and zero the rest. */
            read_bytes = page_offset + phdr.p_filesz;
            zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
          } else {
            /* Entirely zero.
                     Don't read anything from disk. */
            read_bytes = 0;
            zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
          }
          if (!load_segment(file, file_page, (void*)mem_page, read_bytes, zero_bytes, writable))
            goto done;
        } else
          goto done;
        break;
    }
  }

  /* Set up stack. */
  if (!setup_stack(esp))
    goto done;

  /* Start address. */
  *eip = (void (*)(void))ehdr.e_entry;

  success = true;

done:
  /* We arrive here whether the load is successful or not. */

  t->pcb->exec_file = file;
  if (t->pcb->exec_file)
    file_deny_write(file);

  // file_close(file);
  return success;
}

/* load() helpers. */

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool validate_segment(const struct Elf32_Phdr* phdr, struct file* file) {
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off)file_length(file))
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz)
    return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr((void*)phdr->p_vaddr))
    return false;
  if (!is_user_vaddr((void*)(phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable) {
  ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT(pg_ofs(upage) == 0);
  ASSERT(ofs % PGSIZE == 0);

  file_seek(file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) {
    /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
    size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

    /* Get a page of memory. */
    uint8_t* kpage = palloc_get_page(PAL_USER);
    if (kpage == NULL)
      return false;

    /* Load this page. */
    if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes) {
      palloc_free_page(kpage);
      return false;
    }
    memset(kpage + page_read_bytes, 0, page_zero_bytes);

    /* Add the page to the process's address space. */
    if (!install_page(upage, kpage, writable)) {
      palloc_free_page(kpage);
      return false;
    }

    /* Advance. */
    read_bytes -= page_read_bytes;
    zero_bytes -= page_zero_bytes;
    upage += PGSIZE;
  }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool setup_stack(void** esp) {
  uint8_t* kpage;
  bool success = false;

  kpage = palloc_get_page(PAL_USER | PAL_ZERO);
  if (kpage != NULL) {
    success = install_page(((uint8_t*)PHYS_BASE) - PGSIZE, kpage, true);
    if (success)
      *esp = PHYS_BASE;
    else
      palloc_free_page(kpage);
  }
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool install_page(void* upage, void* kpage, bool writable) {
  struct thread* t = thread_current();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page(t->pcb->pagedir, upage) == NULL &&
          pagedir_set_page(t->pcb->pagedir, upage, kpage, writable));
}

/* Returns true if t is the main thread of the process p */
bool is_main_thread(struct thread* t, struct process* p) { return p->main_thread == t; }

/* Gets the PID of a process */
pid_t get_pid(struct process* p) { return (pid_t)p->main_thread->tid; }

/* Creates a new stack for the thread and sets up its arguments.
   Stores the thread's entry point into *EIP and its initial stack
   pointer into *ESP. Handles all cleanup if unsuccessful. Returns
   true if successful, false otherwise.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. You may find it necessary to change the
   function signature. */
bool setup_thread(void (**eip)(void) UNUSED, void** esp UNUSED) { return false; }

/* Starts a new thread with a new user stack running SF, which takes
   TF and ARG as arguments on its user stack. This new thread may be
   scheduled (and may even exit) before pthread_execute () returns.
   Returns the new thread's TID or TID_ERROR if the thread cannot
   be created properly.

   This function will be implemented in Project 2: Multithreading and
   should be similar to process_execute (). For now, it does nothing.
   */
tid_t pthread_execute(stub_fun sf UNUSED, pthread_fun tf UNUSED, void* arg UNUSED) { return -1; }

/* A thread function that creates a new user thread and starts it
   running. Responsible for adding itself to the list of threads in
   the PCB.

   This function will be implemented in Project 2: Multithreading and
   should be similar to start_process (). For now, it does nothing. */
static void start_pthread(void* exec_ UNUSED) {}

/* Waits for thread with TID to die, if that thread was spawned
   in the same process and has not been waited on yet. Returns TID on
   success and returns TID_ERROR on failure immediately, without
   waiting.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
tid_t pthread_join(tid_t tid UNUSED) { return -1; }

/* Free the current thread's resources. Most resources will
   be freed on thread_exit(), so all we have to do is deallocate the
   thread's userspace stack. Wake any waiters on this thread.

   The main thread should not use this function. See
   pthread_exit_main() below.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
void pthread_exit(void) {}

/* Only to be used when the main thread explicitly calls pthread_exit.
   The main thread should wait on all threads in the process to
   terminate properly, before exiting itself. When it exits itself, it
   must terminate the process in addition to all necessary duties in
   pthread_exit.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
void pthread_exit_main(void) {}

struct process* process_current(void) {
  return thread_current()->pcb;
}