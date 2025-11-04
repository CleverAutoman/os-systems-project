#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <stdlib.h>
#include "userprog/process.h"
#include "threads/interrupt.h"
#include "userprog/fdtable.h"
#include "threads/vaddr.h"
#include <limits.h>

void syscall_init(void);

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

#endif /* userprog/syscall.h */
