#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
// #include "threads/interrupt.h"
#include "threads/thread.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"
#include "devices/input.h"
#include "lib/kernel/console.h"
#include "threads/synch.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "lib/syscall-nr.h"

static void syscall_handler(struct intr_frame*);

bool is_valid_user_ptr(const void* uaddr);

bool is_valid_user_range(const void* uaddr, size_t range);

static size_t MAX_BUFFER_SIZE = 1024;
static struct lock console_lock;

void syscall_init(void) {
  intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init(&console_lock);
}

static inline fdtable* cur_fdt(void) {
  struct process* p = process_current();
  ASSERT(p != NULL);
  return p->fdtable;
}

static void copy_in(void* dest, const void* source, size_t size) {
  if (!is_valid_user_range(source, size)) {
    thread_current()->exit_status = -1;
    process_exit();
  }
  memcpy(dest, source, size);
}

static bool is_valid_str(const void* ptr) {
  const char* str = ptr;

  size_t i = 0;

  while (true) {
    if (!is_valid_user_ptr(str + i)) {
      return false;
    }
    if (str[i] == '\0') {
      break;
    }
    i++;
  }
  return true;
}

static void syscall_handler(struct intr_frame* f UNUSED) {
  // printf("syscall entry: eax=%d\n", (int)f->eax);
  uint32_t* args = ((uint32_t*)f->esp);
  // printf("about to read syscall num\n");
  // uint32_t num = args[0];
  // printf("syscall num = %u\n", num);

  if (!is_valid_user_range(args, sizeof(uint32_t))) {
    thread_current()->exit_status = -1;
    process_exit();
    return;
  }

  if (thread_current()->exited) {
    thread_exit();
  }

  /*
   * The following print statement, if uncommented, will print out the syscall
   * number whenever a process enters a system call. You might find it useful
   * when debugging. It will cause tests to fail, however, so you should not
   * include it in your final submission.
   */

  // printf("System call number: %d, with thread: %d\n", args[0], thread_current()->tid);
  switch (args[0]) {
    // File Operations
    case SYS_CREATE: {
      // printf("entered create\n");
      if (!is_valid_str(args[1])) {
        thread_current()->exit_status = -1;
        process_exit();
      }
      if (!is_valid_user_range(args + 1 * 2, sizeof(unsigned))) {
        thread_current()->exit_status = -1;
        process_exit();
      }
      const char* file = (const char*)(args[1]);
      unsigned initial_size = (unsigned)(args[2]);
      f->eax = create(file, initial_size);
      break;
    }
    case SYS_REMOVE: {
      if (!is_valid_str(args[1])) {
        thread_current()->exit_status = -1;
        process_exit();
      }

      const char* file = (const char*)args[1];
      f->eax = remove(file);
      break;
    }
    case SYS_OPEN: {
      if (!is_valid_str(args[1])) {
        // printf("open invalid and exited\n");
        thread_current()->exit_status = -1;
        process_exit();
      }
      const char* file = (const char*)(args[1]);
      if (!file || !is_valid_user_ptr(file)) {
        // printf("open invalid ptr\n");
        thread_current()->exit_status = -1;
        process_exit();
      }
      f->eax = open(file);

      // printf("thread opened success: %d with file: %s with fd == %d\n", thread_current()->tid, file, f->eax);
      break;
    }
    case SYS_FILESIZE: {
      if (!is_valid_user_range(args, 2 * sizeof(int))) {
        thread_current()->exit_status = -1;
        process_exit();
      }
      const int fd = (int)(args[1]);
      // if (!file) {
      //   process_exit();
      // }
      f->eax = filesize(fd);
      break;
    }
    case SYS_READ: {
      if (!is_valid_user_range(args, 4 * sizeof(int))) {
        thread_current()->exit_status = -1;
        process_exit();
      }
      const int fd = (int)(args[1]);
      void* buffer = args[2];
      unsigned initial_size = (unsigned)(args[3]);
      if (fd < 0 || fd > INT_MAX) {
        thread_current()->exit_status = -1;
        process_exit();
      }
      if (!is_valid_user_range(buffer, initial_size)) {
        thread_current()->exit_status = -1;
        process_exit();
      }
      // printf("filename: %d\n", fd);
      f->eax = read(fd, buffer, initial_size);
      break;
    }
    case SYS_WRITE: {
      if (!is_valid_user_range(args, 4 * sizeof(int))) {
        thread_current()->exit_status = -1;
        process_exit();
      }
      const int fd = (int)(args[1]);
      void* buffer = args[2];
      unsigned initial_size = (unsigned)(args[3]);
      if (fd < 0 || fd > INT_MAX) {
        thread_current()->exit_status = -1;
        process_exit();
      }
      if (!is_valid_user_range(buffer, initial_size)) {
        thread_current()->exit_status = -1;
        process_exit();
      }
      f->eax = write(fd, buffer, initial_size);

      break;
    }
    case SYS_SEEK: {
      if (!is_valid_user_range(args, 3 * sizeof(int))) {
        thread_current()->exit_status = -1;
        process_exit();
      }
      const int fd = (int)(args[1]);
      unsigned position = (unsigned)(args[2]);
      if (fd < 0 || fd > INT_MAX) {
        thread_current()->exit_status = -1;
        process_exit();
      }
      seek(fd, position);
      break;
    }
    case SYS_TELL: {
      if (!is_valid_user_range(args, 2 * sizeof(int))) {
        thread_current()->exit_status = -1;
        process_exit();
      }
      const int fd = (int)(args[1]);
      if (fd < 0 || fd > INT_MAX) {
        thread_current()->exit_status = -1;
        process_exit();
      }
      f->eax = tell(fd);
      break;
    }
    case SYS_CLOSE: {
      // printf("entered closed\n");
      if (!is_valid_user_range(args, 2 * sizeof(int))) {
        // printf("exited\n");
        thread_current()->exit_status = -1;
        process_exit();
      }
      const int fd = (int)(args[1]);
      if (fd < 0 || fd > INT_MAX) {
        // printf("invalid with id=%d\n", fd);
        thread_current()->exit_status = -1;
        process_exit();
      }
      // printf("close fd = %d\n", fd);
      close(fd);
      break;
    }

    // Process Control Operations
    case SYS_PRACTICE: {
      if (!is_valid_user_range(args, 2 * sizeof(int))) {
        thread_current()->exit_status = -1;
        process_exit();
      }
      const int input = (int)(args[1]);
      f->eax = input + 1;
      break;
    }
    case SYS_HALT:
      halt();
      break;
    case SYS_EXIT:
      sys_exit(f);
      break;
    case SYS_EXEC: {
      if (!is_valid_user_ptr(args + 4)) {
        thread_current()->exit_status = -1;
        process_exit();
      }
      if (!is_valid_str(args[1])) {
        thread_current()->exit_status = -1;
        process_exit();
      }
      const char* file = (const char*)(args[1]);
      f->eax = exec(file);
      break;
    }
    case SYS_WAIT: {
      if (!is_valid_user_range(args, 2 * sizeof(int))) {
        thread_current()->exit_status = -1;
        process_exit();
      }
      const int pid = (int)(args[1]);
      f->eax = wait(pid);
      break;
    }
    case SYS_FORK: {
      f->eax = fork(f);
      break;
    }

    /* Pthread Opeartions*/
    case SYS_PT_CREATE: {
      if (!is_valid_user_range(args, 4 * sizeof(int))) {
        thread_current()->exit_status = -1;
        process_exit();
      }
      stub_fun sfun = (stub_fun)(args[1]);
      pthread_fun pfun = (pthread_fun)(args[2]);
      void* arg = (void*)(args[3]);

      f->eax = sys_pthread_create(sfun, pfun, arg);
      break;
    }
    case SYS_PT_EXIT: {
      sys_pthread_exit();
      break;
    }
    case SYS_PT_JOIN: {
      if (!is_valid_user_range(args, 2 * sizeof(int))) {
        thread_current()->exit_status = -1;
        process_exit();
      }
      const int tid = (int)(args[1]);
      f->eax = sys_pthread_join(tid);
      break;
    }
    case SYS_LOCK_INIT: {
      if (!is_valid_user_range(args, 2 * sizeof(void*))) {
        thread_current()->exit_status = -1;
        process_exit();
      }
      char* lock = (char*)args[1];
      f->eax = sys_lock_init(lock);
      break;
    }
    case SYS_LOCK_ACQUIRE: {
      if (!is_valid_user_range(args, 2 * sizeof(void*))) {
        thread_current()->exit_status = -1;
        process_exit();
      }
      char* lock = (char*)args[1];
      f->eax = sys_lock_acquire(lock);
      break;
    }
    case SYS_LOCK_RELEASE: {
      if (!is_valid_user_range(args, 2 * sizeof(void*))) {
        thread_current()->exit_status = -1;
        process_exit();
      }
      char* lock = (char*)args[1];
      f->eax = sys_lock_release(lock);
      break;
    }
    case SYS_SEMA_INIT: {
      if (!is_valid_user_range(args, 3 * sizeof(void*))) {
        thread_current()->exit_status = -1;
        process_exit();
      }
      char* sema = (char*)args[1];
      int val = (int)args[2];
      f->eax = sys_sema_init(sema, val);
      break;
    }
    case SYS_SEMA_DOWN: {
      if (!is_valid_user_range(args, 2 * sizeof(void*))) {
        thread_current()->exit_status = -1;
        process_exit();
      }
      char* sema = (char*)args[1];
      f->eax = sys_sema_down(sema);
      break;
    }
    case SYS_SEMA_UP: {
      if (!is_valid_user_range(args, 2 * sizeof(void*))) {
        thread_current()->exit_status = -1;
        process_exit();
      }
      char* sema = (char*)args[1];
      f->eax = sys_sema_up(sema);
      break;
    }
    case SYS_GET_TID: {
      f->eax = get_tid();
      break;
    }
    case SYS_INUMBER: {
      if (!is_valid_user_range(args, 2 * sizeof(int))) {
        thread_current()->exit_status = -1;
        process_exit();
      }
      const int fd = (int)(args[1]);
      f->eax = sys_inumber(fd);
      break;
    }

    default:
      // printf("Unknown Syscall: %d\n", args[0]);
      break;
  }
}

bool is_valid_user_ptr(const void* uaddr) {
  return (uaddr && is_user_vaddr(uaddr) &&
          pagedir_get_page(thread_current()->pcb->pagedir, pg_round_down(uaddr)) != NULL);
}

bool is_valid_user_range(const void* uaddr, size_t range) {
  if (range < 0)
    return false;
  if (range == 0)
    return true;

  uintptr_t a = (uintptr_t)uaddr;
  uintptr_t b = a + (uintptr_t)range - 1;

  if (a > b)
    return false;

  for (size_t i = 0; i < range; i++) {
    uintptr_t a = (uintptr_t)uaddr + (uintptr_t)i;
    if (!is_valid_user_ptr((const void*)a))
      return false;
  }

  return true;
}

/**
 * Syscall Signatures
 */
void halt(void) { shutdown(); }

void sys_exit(struct intr_frame* f) {
  uint32_t* args = ((uint32_t*)f->esp);
  // printf("SYS_EXIT: tid=%d, status=%d\n", thread_current()->tid, args[1]);
  // f->eax = args[1];
  // thread_current()->exit_status = args[1];
  // printf("%s: exit(%d)\n", thread_current()->pcb->process_name, args[1]);
  /* Pass exit_status to main thread */
  struct process* pcb = thread_current()->pcb;
  pcb->main_thread->exit_status = args[1];
  thread_current()->exit_status = args[1];
  /* End all child threads*/
  process_exit();
  // process_exit();
}

pid_t exec(const char* cmd_line) {
  char* kpage = malloc(strlen(cmd_line) + 1);
  if (kpage == NULL)
    return -1;

  strlcpy(kpage, cmd_line, strlen(cmd_line) + 1);
  pid_t tid = process_execute(kpage);
  if (tid == TID_ERROR) {
    free(kpage);
    return -1;
  }
  free(kpage);

  return tid;
}

int wait(pid_t pid) {
  return process_wait(pid);
  // return 0;
}

pid_t fork(struct intr_frame* f) { return process_fork(f); }

/**
 * File operations
 */
bool create(const char* file, unsigned initial_size) {
  // printf("current file: %s\n", file);
  // printf("current size: %u\n", initial_size);

  if (!file || file[0] == '\0') {
    return false;
  }
  lock_acquire(&filesys_lock);
  bool res = filesys_create(file, initial_size);
  lock_release(&filesys_lock);
  // printf("create with size = %u\n" );
  // printf("create with size = %u\n" );
  return res;
}

bool remove(const char* file) {
  if (file == NULL) {
    return false; // 无效参数
  }

  lock_acquire(&filesys_lock);
  bool success = filesys_remove(file);
  lock_release(&filesys_lock);

  return success;
}

int open(const char* file) {
  /* get current's process's fdtable */
  fdtable* fdt = cur_fdt();
  ASSERT(fdt != NULL);

  // open file* from file_name
  lock_acquire(&filesys_lock);
  struct file* f = filesys_open(file);
  lock_release(&filesys_lock);

  // save &file into fdtable
  int fd = -1;
  lock_acquire(&fdt->lock);
  fd = add_file_unlocked(f, fdt);
  lock_release(&fdt->lock);

  // printf("fd: %d\n", fd);

  if (fd < 0) { // not enough fds in table
    filesys_remove(file);
    return -1;
  }

  return fd;
}

int read(int fd, void* buffer, unsigned size) {
  ASSERT(buffer != NULL);
  uint8_t* input_buffer = (uint8_t*)buffer;

  if (fd == 1)
    return -1;
  if (fd == 0) {
    unsigned i = 0;
    for (; i < size; i++) {
      uint8_t ch = input_getc();
      input_buffer[i] = ch;
    }
    return (int)i;
  }

  fdtable* fdt = cur_fdt();
  struct file* f = NULL;

  // A 阶段：只访问 fdt
  lock_acquire(&fdt->lock);
  f = get_file_unlocked(fd, fdt);
  lock_release(&fdt->lock);
  if (!f)
    return -1;

  // B 阶段：只做文件系统调用
  lock_acquire(&filesys_lock);
  int n = file_read(f, buffer, size);
  lock_release(&filesys_lock);
  return n;
}
int filesize(int fd) {
  fdtable* fdt = cur_fdt();
  struct file* f = NULL;
  lock_acquire(&fdt->lock);
  f = get_file_unlocked(fd, fdt);
  lock_release(&fdt->lock);
  if (!f)
    return -1;

  lock_acquire(&filesys_lock);
  int len = file_length(f);
  lock_release(&filesys_lock);
  return len;
}

void seek(int fd, unsigned pos) {
  fdtable* fdt = cur_fdt();
  struct file* f = NULL;
  lock_acquire(&fdt->lock);
  f = get_file_unlocked(fd, fdt);
  lock_release(&fdt->lock);
  if (!f)
    return;

  lock_acquire(&filesys_lock);
  file_seek(f, pos);
  lock_release(&filesys_lock);
}

int tell(int fd) {
  fdtable* fdt = cur_fdt();
  struct file* f = NULL;
  lock_acquire(&fdt->lock);
  f = get_file_unlocked(fd, fdt);
  lock_release(&fdt->lock);
  if (!f)
    return -1;

  lock_acquire(&filesys_lock);
  int res = file_tell(f);
  lock_release(&filesys_lock);
  return res;
}

void close(int fd) {
  fdtable* fdt = cur_fdt();
  if (!fdt) {
    return;
  }

  // printf("reached1\n");
  struct thread* cur = thread_current();
  // printf("CLOSE ENTRY pid=%d pcb=%p fdtable=%p files[2]=%p\n",
  // cur->tid,
  // cur->pcb,
  // cur->pcb->fdtable,
  // cur->pcb->fdtable->files[2]);

  lock_acquire(&fdt->lock);
  // printf("reached2\n");

  if (fd < 0 || fd >= fdt->size || !bitmap_test(fdt->bitmap, fd)) {
    lock_release(&fdt->lock);
    return;
  }

  // printf("reached3\n");
  struct file* f = fdt->files[fd];
  fdt->files[fd] = NULL;
  bitmap_set(fdt->bitmap, fd, false);
  // printf("reached4\n");

  lock_release(&fdt->lock);

  if (f == NULL) {
    // printf("return NULL\n");

    return;
  }

  lock_acquire(&filesys_lock);
  file_close(f);
  // printf("reached5\n");
  lock_release(&filesys_lock);
}

int write(int fd, const void* buffer, unsigned size) {
  if (fd == 0)
    return -1;
  if (fd == 1) {
    const uint8_t* p = (const uint8_t*)buffer;
    unsigned remain = size;
    while (remain > 0) {
      size_t chunk = remain < MAX_BUFFER_SIZE ? remain : MAX_BUFFER_SIZE;
      putbuf((const char*)p, chunk);
      p += chunk;
      remain -= chunk;
    }
    return (int)size;
  }
  fdtable* fdt = cur_fdt();
  struct file* f = NULL;

  lock_acquire(&fdt->lock);
  f = get_file_unlocked(fd, fdt);
  lock_release(&fdt->lock);
  if (!f)
    return -1;

  lock_acquire(&filesys_lock);
  int n = file_write(f, buffer, size);
  lock_release(&filesys_lock);
  return n;
}

/* Pthread Operations */
tid_t sys_pthread_create(stub_fun sfun, pthread_fun tfun, const void* arg) {
  // printf("pthread_create: parent=%d\n", thread_current()->tid);

  return pthread_execute(sfun, tfun, arg);
}

void sys_pthread_exit(void) {
  // printf("pthread_exit: tid=%d\n", thread_current()->tid);
  pthread_exit();
}

tid_t sys_pthread_join(tid_t tid) {
  // printf("pthread_join: waiter=%d waits_for=%d\n", thread_current()->tid, tid);

  return pthread_join(tid);
}

bool sys_lock_init(char* lock) {
  if (!lock) {
    return false;
  }
  create_new_lock(lock);
  return true;
}

bool sys_lock_acquire(char* lock) { return acquire_tlock(lock); }

bool sys_lock_release(char* lock) { return release_tlock(lock); }

bool sys_sema_init(char* sema, int val) {
  if (!sema || val < 0) {
    return false;
  }
  create_new_sema(sema, val);
  return true;
}

bool sys_sema_down(char* sema) { return sema_down_t(sema); }

bool sys_sema_up(char* sema) { return sema_up_t(sema); }

tid_t get_tid(void) { return thread_current()->tid; }

/* File system Operations */
uint32_t sys_inumber(int fd) {
  /* Find file based on fd */
  fdtable* fdt = cur_fdt();
  struct file* f = NULL;
  lock_acquire(&fdt->lock);
  f = get_file_unlocked(fd, fdt);
  lock_release(&fdt->lock);

  /* Get inode->inumber */
  struct inode* inode = file_get_inode(f);
  if (!inode) {
    return -1;
  }
  return inode_get_inumber(inode);
}