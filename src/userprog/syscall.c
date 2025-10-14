#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "userprog/process.h"
#include "lib/kernel/console.h"
#include "threads/synch.h"
#include "filesys/file.h"
#include "lib/syscall-nr.h"

static void syscall_handler(struct intr_frame*);

static size_t MAX_BUFFER_SIZE = 1024;
static struct lock console_lock;

void syscall_init(void) {
  intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init(&console_lock);
}

static inline fdtable* cur_fdt(void) {
  struct process* p = process_current();
  ASSERT(p != NULL);
  return &p->fdtable;
}

static void syscall_handler(struct intr_frame* f UNUSED) {
  // printf("syscall entry: eax=%d\n", (int)f->eax);
  uint32_t* args = ((uint32_t*)f->esp);

  /*
   * The following print statement, if uncommented, will print out the syscall
   * number whenever a process enters a system call. You might find it useful
   * when debugging. It will cause tests to fail, however, so you should not
   * include it in your final submission.
   */

  // printf("System call number: %d\n", args[0]);

  if (args[0] == SYS_PRACTICE) {
    int n = *(int*)(f->esp + 4); // Pintos 32-bit, 第一个参数
    f->eax = n + 1;
  }

  // File Operations
  if (args[0] == SYS_CREATE) {
  }

  if (args[0] == SYS_REMOVE) {
  }

  if (args[0] == SYS_OPEN) {
  }

  if (args[0] == SYS_FILESIZE) {
  }

  if (args[0] == SYS_READ) {
  }

  if (args[0] == SYS_WRITE) {
    write(args[1], args[2], args[3]);
  }

  if (args[0] == SYS_SEEK) {
  }

  if (args[0] == SYS_TELL) {
  }

  if (args[0] == SYS_CLOSE) {
  }

  if (args[0] == SYS_EXIT) {
    f->eax = args[1];
    thread_current()->exit_status = args[1];
    // printf("%s: exit(%d)\n", thread_current()->pcb->process_name, args[1]);
    process_exit();
  }
}

/**
 * Syscall Signatures
 */
int practice(int i) {}

void halt(void) {}

void exit(int status) {}

pid_t exec(const char* cmd_line) {}

int wait(pid_t pid) {}

pid_t fork(void) {}

/**
 * File operations
 */
bool create(const char* file, unsigned initial_size) {}

bool remove(const char* file) {}

int open(const char* file) {}

int filesize(int fd) {}

int read(int fd, void* buffer, unsigned size) {}

void seek(int fd, unsigned position) {}

int tell(int fd) {}

void close(int fd) {}

int write(int fd, const void* buffer, unsigned size) {
  if (fd == 1) {
    if (size < MAX_BUFFER_SIZE) {
      putbuf(buffer, size);
    } else {
      // break into a few hunderd byte chunks
      lock_acquire(&console_lock);
      while (size > 0) {
        size_t cur_size = size < MAX_BUFFER_SIZE ? size : MAX_BUFFER_SIZE;
        putbuf(buffer, cur_size);
        size -= cur_size;
      }
      lock_release(&console_lock);
    }
    return 0;
  }
  /* write from fd */
  fdtable* fdt = cur_fdt();
  ASSERT(fdt != NULL);

  /* find file */
  struct file* f = get_file(fd, fdt);
  if (!f)
    return -1;

  size_t off = 0;
  while (off < size) {
    size_t r = file_write(f, (char*)buffer + off, size - off);
    if (r > 0) {
      off += (size_t)r;
      continue;
    }
    if (r == 0)
      break; // EOF
    if (off == (size_t)file_length(f))
      break; // write to the end of written file
    return -1;
  }
  if (off == 0) { // nothing to write
    return 0;
  }
  return (size_t)off;
}
