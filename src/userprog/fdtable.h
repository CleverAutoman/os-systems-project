#ifndef USERPROG_FDTABLE_H
#define USERPROG_FDTABLE_H

#include "threads/synch.h"
#include "filesys/file.h"
#include "lib/kernel/bitmap.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct fdtable {
  struct file** files;   // 指针数组：fd -> file*
  struct bitmap* bitmap; // 位图：0 空闲, 1 已用
  int size;              // 当前容量（例如 256, 1024, 4096...）
  int next_index;        // 上次分配到的位置，做环形扫描提速
  struct lock lock;      // 线程内共享时的保护
} fdtable;

void fdtable_create(fdtable* fdt, int max_fds);
int find_next_vacant_fd(fdtable* fdt);
int add_file_unlocked(struct file* file, fdtable* fdt);
struct file* remove_file_unlocked(int fd, fdtable* fdt);
struct file* get_file_unlocked(int fd, fdtable* fdt);
void free_table(fdtable* fdt);
void fork_fdtable(fdtable* dest, fdtable* src);

#endif /* USERPROG_FDTABLE_H */