#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <stdlib.h>
#include "userprog/process.h"
#include "threads/interrupt.h"
#include "userprog/fdtable.h"
#include "userprog/sync_table.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include <limits.h>

void syscall_init(void);
// struct lock_t;
// struct sema_t;

/**
 * Syscall Signatures
 */
int practice(int i);

void halt(void);

void sys_exit(struct intr_frame* f);

pid_t exec(const char* cmd_line);

int wait(pid_t pid);

pid_t fork(struct intr_frame* f);

/**
 * File operations
 */
bool remove(const char* file);

bool create(const char* file, unsigned initial_size);

int open(const char* file);

int filesize(int fd);

int sys_read(int fd, void* buffer, unsigned size);

void seek(int fd, unsigned position);

int tell(int fd);

void close(int fd);

int write(int fd, const void* buffer, unsigned size);

/**
 *  Pthread operations
 */
tid_t sys_pthread_create(stub_fun sfun, pthread_fun tfun, const void* arg);

void sys_pthread_exit(void) NO_RETURN;

tid_t sys_pthread_join(tid_t tid);

bool sys_lock_init(char* lock);

bool sys_lock_acquire(char* lock);

bool sys_lock_release(char* lock);

bool sys_sema_init(char* sema, int val);

bool sys_sema_down(char* sema);

bool sys_sema_up(char* sema);

tid_t get_tid(void);

uint32_t sys_inumber(int fd);

/* Pthread SYNCH */

// struct lock_t {
//   int holder;      // or tid
//   int locked;
//   struct lock lock;
// };

// struct sema_t {
//   int holder;      // or tid
//   int locked;
//   struct semaphore sema;
// };

#endif /* userprog/syscall.h */
