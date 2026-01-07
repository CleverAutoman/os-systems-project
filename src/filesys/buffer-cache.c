#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "buffer-cache.h"
#include "threads/malloc.h"
#include "threads/thread.h"

#define CACHE_SIZE 64
#define NO_SECTOR 0xFFFFFFFF

static struct buffer_cache bc;

void cache_init() {
  lock_init(&bc.global_lock);

  /* Malloc entry array inside buffer cache 
     Initialize cache entries inside buffer cache */
  bc.ce = calloc(CACHE_SIZE, sizeof *bc.ce);
  if (bc.ce == NULL)
    PANIC("cache_init: calloc failed");

  int i = 0;
  for (i = 0; i < CACHE_SIZE; i++) {
    // printf("init lock with ptr: %p and ce: %p\n", &bc.ce[i].entry_lock, &bc.ce[i]);
    lock_init(&bc.ce[i].entry_lock);
    cond_init(&bc.ce[i].busy_cond);

    bc.ce[i].valid = false;
    bc.ce[i].dirty = false;
    bc.ce[i].accessed = false;
    bc.ce[i].busy = false;
    bc.ce[i].magic = ENTRY_MAGIC;

    /* Fill blank value for each data */
    for (int j = 0; j < 128; j++) {
      bc.ce[i].data[j] = NO_SECTOR;
    }
  }
  return;

error:
  if (i > 0) {
    for (int j = 0; j < i; j++) {
      free(bc.ce);
    }
  }
}

static void flush_block(struct cache_entry* ce) {
  block_write(ce->block, ce->sector, ce->data);
  ce->dirty = false;
  ce->valid = false;
}

/* Assume global lock is held */
static struct cache_entry* find_victim() {
  int i = 0;
  while (1) {
    i = bc.hand;

    /* Find next invalid or accessed entry */
    if (!bc.ce[i].valid || !bc.ce[i].accessed) {
      /* Set current entry to valid */
      if (!bc.ce[i].accessed) {
        /* TODO: We need to flush back this cache entry */
        if (bc.ce[i].dirty) {
          flush_block(&bc.ce[i]);
        }
      }
      return &bc.ce[i];
    } else {
      bc.ce[i].accessed = false;
    }

    bc.hand += 1;
    if (bc.hand == CACHE_SIZE) {
      bc.hand = 0;
    }
  }
}

/* Opening and closing buffer. */
struct cache_entry* acquire_entry(struct block* block, block_sector_t sector) {
  /* First try to hold global lock until release entry */
  printf("ready to acquire lock\n");
  lock_acquire(&bc.global_lock);
  printf("acquire lock success\n");

  /* 1. Return entry if has one in the list */
  struct cache_entry* ce = NULL;
  for (int i = 0; i < CACHE_SIZE; i++) {
    if (bc.ce[i].valid && bc.ce[i].sector == sector) {
      ce = &bc.ce[i];
    }
  }
  if (ce) {
    /* Set current cache entry to accessed */
    ce->accessed = true;
    ce->block = block;
    /* Make sure lock is held before return */
    return ce;
  }

  /* 2. We need to find a victim for this sector id */
  ce = find_victim();
  if (!ce) {
    PANIC("cannot find an entry");
  }

  /* Set current cache entry to accessed */
  ce->block = block;
  ce->sector = sector;
  if (!ce->valid) {
    // printf();
    block_read(ce->block, ce->sector, ce->data);
    ce->valid = true;
  }
  /* Make sure lock is held before return */
  return ce;
}

struct cache_entry* release_entry(struct cache_entry* ce) {
  ASSERT(ce->magic == ENTRY_MAGIC);
  /* Release all locks held by current entry */
  /* Currently, just use global lock */
  lock_release(&bc.global_lock);
  printf("release lock success\n");
}

/* Reading and writing. Assuming entry lock is held  */
void read_entry(struct cache_entry* ce, void* buffer) {
  ASSERT(ce->magic == ENTRY_MAGIC);
  memcpy(buffer, ce->data, BLOCK_SECTOR_SIZE);
  ce->accessed = true;
}

void write_entry(struct cache_entry* ce, void* buffer) {
  ASSERT(ce->magic == ENTRY_MAGIC);
  memcpy(ce->data, buffer, BLOCK_SECTOR_SIZE);
  ce->accessed = true;
  ce->dirty = true;
}