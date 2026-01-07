#ifndef FILESYS_BUFFER_CACHE_H
#define FILESYS_BUFFER_CACHE_H

#include "filesys/off_t.h"

#include "devices/block.h"
#include <stdbool.h>
#include "threads/synch.h"
#include "filesys/filesys.h"
#include "devices/block.h"

#define ENTRY_MAGIC 0x494e4f44

struct cache_entry {
  struct lock entry_lock; /* Lock for this cache entry */
  struct condition busy_cond;

  block_sector_t sector; /* sector_id */
  struct block* block;
  bool valid;    /* Whethre this cache is able to read or not */
  bool dirty;    /* Need to flush when evicted */
  bool accessed; /* Visited: true, else: false */
  // bool removed;      /* This entry is already removed, cannot access any more */
  // int refcnt; /* Refcnts, cannot evict if != 0 */
  bool
      busy; /* Unable to visit even w/o lock, used between cache_acquire and cache_release, cannot delete */
  uint32_t data[BLOCK_SECTOR_SIZE / 4]; /* Actual Data */
  unsigned magic;                       /* Magic number. */
};

enum cache_type {
  CLOCK,
  SECOND_CHANCE_LST,
  LRU,
};

struct buffer_cache {
  struct lock global_lock;
  struct cache_entry* ce; /* Entries list */
  enum cache_type type;   /* Type of block device. */
  const struct eviction_ops* ops;
  int hand;
};

void cache_init(void);

/* Opening and closing buffer. */
struct cache_entry* acquire_entry(struct block*, block_sector_t);
struct cache_entry* release_entry(struct cache_entry*);

/* Reading and writing. */
void read_entry(struct cache_entry*, void*);
void write_entry(struct cache_entry*, void*);

#endif /* filesys/buffer-cache.h */