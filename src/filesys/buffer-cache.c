#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "buffer-cache.h"
#include "threads/malloc.h"
#include "threads/thread.h"

#define CACHE_SIZE 128
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
  bc.hand = 0;
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
    memset(bc.ce[i].data, 0, BLOCK_SECTOR_SIZE);
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
}

/* Assume global lock is held */
static struct cache_entry* find_victim() {
  int i = 0;
  while (1) {
    i = bc.hand;

    /* Find next invalid or victim entry */
    if (bc.ce[i].busy) {
      bc.hand = (bc.hand + 1) % CACHE_SIZE;
      continue;
    }

    if (!bc.ce[i].valid || !bc.ce[i].accessed) {
      bc.hand = (bc.hand + 1) % CACHE_SIZE;
      return &bc.ce[i];
    } else {
      bc.ce[i].accessed = false;
    }

    bc.hand = (bc.hand + 1) % CACHE_SIZE;
  }
}

/* Opening and closing buffer. */
struct cache_entry* acquire_entry(struct block* block, block_sector_t sector) {
  lock_acquire(&bc.global_lock);

  /* hit */
  for (int i = 0; i < CACHE_SIZE; i++) {
    struct cache_entry* ce = &bc.ce[i];
    if (ce->valid && ce->sector == sector && ce->block == block) {
      while (ce->busy)
        cond_wait(&ce->busy_cond, &bc.global_lock);

      ce->busy = true;
      ce->accessed = true;
      lock_release(&bc.global_lock);

      lock_acquire(&ce->entry_lock);
      return ce;
    }
  }

  /* miss */
  struct cache_entry* ce = find_victim();
  ce->busy = true;
  lock_release(&bc.global_lock);

  lock_acquire(&ce->entry_lock);

  if (ce->valid && ce->dirty)
    flush_block(ce);

  ce->block = block;
  ce->sector = sector;

  lock_release(&ce->entry_lock);

  /* do i/o without entry-lock */
  block_read(block, sector, ce->data);

  lock_acquire(&ce->entry_lock);
  ce->valid = true;
  ce->dirty = false;
  ce->accessed = true;

  lock_release(&ce->entry_lock);

  lock_acquire(&bc.global_lock);
  ce->busy = false;
  cond_broadcast(&ce->busy_cond, &bc.global_lock);
  lock_release(&bc.global_lock);

  lock_acquire(&ce->entry_lock);
  return ce;
}

void release_entry(struct cache_entry* ce) {
  lock_release(&ce->entry_lock);
  lock_acquire(&bc.global_lock);
  ce->busy = false;
  cond_broadcast(&ce->busy_cond, &bc.global_lock);
  lock_release(&bc.global_lock);
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

void cache_flush_all(void) {
  lock_acquire(&bc.global_lock);
  for (int i = 0; i < CACHE_SIZE; i++) {
    struct cache_entry* ce = &bc.ce[i];
    if (ce->dirty) {
      flush_block(ce);
    }
  }
  lock_release(&bc.global_lock);
}