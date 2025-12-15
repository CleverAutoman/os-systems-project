
#include "fdtable.h"
#include "threads/synch.h"
#include <debug.h>
#include "threads/thread.h"

/* An open file. */
struct file {
  struct inode* inode; /* File's inode. */
  off_t pos;           /* Current position. */
  bool deny_write;     /* Has file_deny_write() been called? */
};

void fdtable_create(fdtable* fdt, int max_fds) {

  fdt->bitmap = bitmap_create(max_fds);
  ASSERT(fdt->bitmap != NULL);

  fdt->size = max_fds;
  fdt->next_index = 2; // leave 0 1 for stdin stdout

  fdt->files = calloc(fdt->size, sizeof(*fdt->files));
  lock_init(&fdt->lock);
}

int find_next_vacant_fd(fdtable* fdt) {
  ASSERT(fdt != NULL);

  size_t i = bitmap_scan(fdt->bitmap, fdt->next_index, 1, false);
  return i;
}

int add_file_unlocked(struct file* file, fdtable* fdt) {
  ASSERT(fdt != NULL);
  if (!file) {
    // printf("not file exited\n");
    return -1;
  }

  if (fdt->next_index == BITMAP_ERROR) {
    return -1;
  }

  // add file
  fdt->files[fdt->next_index] = file;

  // move to next index
  bitmap_set(fdt->bitmap, fdt->next_index, true);
  size_t i = fdt->next_index;

  fdt->next_index = bitmap_scan(fdt->bitmap, 2, 1, false);

  return i; // file's index
}

struct file* get_file_unlocked(int fd, fdtable* fdt) {
  // printf("get file with fd: %d\n", fd);
  // printf("get file with totol size: %d\n", fdt->size);
  // printf("cur size of fdt: %d\n", bitmap_size(fdt->bitmap));
  ASSERT(fdt != NULL);
  if (fd > fdt->size) {
    return NULL;
  }

  // check fd valid first
  if (!bitmap_test(fdt->bitmap, fd)) {
    return NULL;
  }
  struct file* f = fdt->files[fd];

  return f;
}

struct file* remove_file_unlocked(int fd, fdtable* fdt) {
  ASSERT(fdt != NULL);
  if (fd >= fdt->size) {
    return NULL;
  }

  /* check fd valid first */
  ASSERT(bitmap_test(fdt->bitmap, fd));
  struct file* f = fdt->files[fd];
  printf("CLOSE pid=%d fd=%d file*=%p deny=%d inode=%p\n", thread_current()->tid, fd, f,
         f ? f->deny_write : -1, f ? f->inode : NULL);
  /* free *file & flip bitmap */
  // file_close(f);
  fdt->files[fd] = NULL;
  bitmap_set(fdt->bitmap, fd, false);

  return f;
}

void free_table(fdtable* fdt) {
  if (!fdt) {
    return;
  }

  if (fdt->files) {
    for (size_t i = 0; i < fdt->size; i++) {
      struct file* fp = fdt->files[i];
      if (fp) {
        // lock_acquire(&filesys_lock);
        file_close(fp);
        // lock_release(&filesys_lock);

        fdt->files[i] = NULL;
      }
    }
    free(fdt->files);
    fdt->files = NULL;
  }
  if (fdt->bitmap) {
    bitmap_destroy(fdt->bitmap);
    fdt->bitmap = NULL;
  }
  free(fdt);
  fdt = NULL;
}

void fork_fdtable(fdtable* dest, fdtable* src) {
  ASSERT(src);
  for (size_t i = 0; i < 512; i++) {
    struct file* f = src->files[i];
    if (f == NULL) {
      dest->files[i] = NULL;
    } else {
      dest->files[i] = file_reopen(f); // 新 file*
      bitmap_set(dest->bitmap, i, true);
    }
  }
  dest->next_index = src->next_index;
}