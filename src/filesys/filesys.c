#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/buffer-cache.h"

/* Partition that contains the file system. */
struct block* fs_device;

static void do_format(void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void filesys_init(bool format) {
  /* Init buffer cache */
  cache_init(CLOCK);

  fs_device = block_get_role(BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC("No file system device found, can't initialize file system.");

  inode_init();
  free_map_init();

  if (format)
    do_format();

  free_map_open();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void filesys_done(void) { free_map_close(); }

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool filesys_create(const char* name, off_t initial_size) {
  block_sector_t inode_sector = 0;
  struct dir* dir = dir_open_root();
  printf("open success\n");
  // bool success = (dir != NULL && free_map_allocate(1, &inode_sector) &&
  //                 inode_create(inode_sector, initial_size) && dir_add(dir, name, inode_sector));
  /* New Logic: create inode with blocks only enought for metadata */
  bool allocate_map = free_map_allocate(1, &inode_sector);
  printf("allocate success with inode_Sector: %d\n", inode_sector);
  // printf("ALLOC: %d,  inode_sector=%u\n", allocate_map, inode_sector);

  bool inode_create = inode_create_with_zero_first(inode_sector);
  printf("create success\n");
  // printf("create: %d, inode inode_sector=%u\n", inode_create, inode_sector);

  bool add_dir = dir_add(dir, name, inode_sector);
  printf("dir add success\n");
  // printf("create: dir: %p, name: %s, sector: %d\n", dir, name, inode_sector);
  // printf("add dir: %d, inode_sector=%u\n", add_dir, inode_sector);

  bool success = (dir != NULL && allocate_map && inode_create && add_dir);
  if (!success && inode_sector != 0)
    free_map_release(inode_sector, 1);
  dir_close(dir);
  printf("dir close success\n");

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file* filesys_open(const char* name) {
  struct dir* dir = dir_open_root();
  struct inode* inode = NULL;

  if (dir != NULL) {
    bool ok = dir_lookup(dir, name, &inode);
    // printf("open: dir: %p, name: %s, inode: %p\n", dir, name, inode);
  }
  dir_close(dir);

  struct file* f = file_open(inode);
  return f;
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool filesys_remove(const char* name) {
  // printf("DEBUG: filesys_remove(%s)\n", name);
  struct dir* dir = dir_open_root();
  bool success = dir != NULL && dir_remove(dir, name);
  dir_close(dir);

  return success;
}

/* Formats the file system. */
static void do_format(void) {
  printf("Formatting file system...");
  free_map_create();
  if (!dir_create(ROOT_DIR_SECTOR, 16))
    PANIC("root directory creation failed");
  free_map_close();
  printf("done.\n");
}
