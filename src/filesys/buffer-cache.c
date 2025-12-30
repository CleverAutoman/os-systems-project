#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "buffer-cache.h"
#include "threads/malloc.h"
#include "threads/thread.h"

#define CACHE_SIZE 64

static struct buffer_cache bc;

const struct eviction_ops CLOCK_OPS;
// const struct eviction_ops SEC_CHANCE_LST_OPS;
// const struct eviction_ops LRU_OPS;

static struct cache_entry* entry_lookup(block_sector_t sector);
static void clock_on_access(struct buffer_cache* c, struct cache_entry* e);
static void clock_on_insert(struct buffer_cache* c, struct cache_entry* e, block_sector_t sector);
static void clock_on_remove(struct buffer_cache* c, struct cache_entry* e);
static struct cache_entry* clock_pick_victim(struct buffer_cache* c);

/* Public functions for cache ops */
void cache_init(enum cache_type type) {
  lock_init(&bc.global_lock);
  bc.type = type;

  /* Allocate policy state & poliy ops */
  switch (type) {
    case CLOCK: {
      bc.ops = &CLOCK_OPS;
      break;
    }
    case SECOND_CHANCE_LST: {
      // bc.ops = &SEC_CHANCE_LST_OPS;
      break;
    }
    case LRU: {
      // bc.ops = &LRU_OPS;
      break;
    }
  }

  /* Call init ops function */
  if (bc.ops) {
    bc.ops->init(&bc);
  }

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
  }
  return;

error:
  if (i > 0) {
    for (int j = 0; j < i; j++) {
      free(bc.ce);
    }
  }
}

void cache_done(void) {
  for (int i = 0; i < CACHE_SIZE; i++) {
    if (bc.ce)
      free(bc.ce);
  }
}

/* Acquire cache sector with lock holding, will update this sector  */
struct cache_entry* cache_acquire(block_sector_t sector) {
  // printf("cache acquire: %u\n", sector);
  // printf("cache_acquire: tring to acquire global lock: %p\n", &bc.global_lock);
  lock_acquire(&bc.global_lock);
  /* Find cache entry with sector_id == sector, ce is holding entry lock */
  struct cache_entry* ce = bc.ops->entry_lookup(sector);

  /* Find the next available slot if cant find one, then remove victim */
  if (!ce) {
    ce = bc.ops->pick_victim(&bc);
  }
  /* entry lock is held */

  /*  Have to make sure global lock is held when requesting entry lock
      to prevent two threads pick the same victim.
      Must release global lock if fail to acquire entry lock 
      before waiting for it, Otherwise, we can just acqure the lock */

  // if (!lock_try_acquire(&ce->entry_lock)) {
  //   printf("release global lock: %p\n", &bc.global_lock);
  //   lock_release(&bc.global_lock);
  //   printf("trying to acquire entry lock: %d, addr: %p\n", sector, &ce->entry_lock);
  //   lock_acquire(&ce->entry_lock);
  // } else {
  //   printf("release global lock: %p\n", &bc.global_lock);
  //   lock_release(&bc.global_lock);
  // }

  /* Wait until busy == false, this situation could happen 
      when working on background job while already released the lock to reduce lock-holding time */
  while (ce->busy) {
    // printf("start to wait\n");
    cond_wait(&ce->busy_cond, &ce->entry_lock);
  }
  // printf("wait passed\n");

  ce->busy = true; /* Set to BUSY asa this ce is no longer busy */

  /* If entry is victim, need to remove it before access new entry */
  if (ce->valid && !ce->accessed) {
    bc.ops->on_remove(&bc, ce);
  }
  bc.ops->on_access(&bc, ce);

  /* Actual Logic: need to insert entry to buffer cache */
  bc.ops->on_insert(&bc, ce, sector);

  /* Return current ce with busy == true, so we just release the entry lock */
  // printf("cache_acquire: release entry lock: %d, addr: %p\n", sector, &ce->entry_lock);
  return ce;
}

/* Called to set busy = false */
void cache_release(struct cache_entry* ce) {
  if (!ce)
    return;
  ASSERT(ce->magic == ENTRY_MAGIC);

  ce->busy = false;
  cond_broadcast(&ce->busy_cond, &ce->entry_lock);
  // printf("cache release entry lock: %d, addr: %p\n", ce->sector, &ce->entry_lock);
  lock_release(&ce->entry_lock);
  lock_release(&bc.global_lock);
}

/* This function should be called with entry_lock holding */
void cache_mark_dirty(struct cache_entry* e) { e->dirty = true; }

void cache_readahead(block_sector_t sector) {}

/* Flush all dirty buffer entries back to blocks */
void cache_flush_all(void) {
  lock_acquire(&bc.global_lock);
  for (int i = 0; i < CACHE_SIZE; i++) {
    struct cache_entry* ce = &bc.ce[i];

    if (!lock_try_acquire(&ce->entry_lock)) {
      lock_release(&bc.global_lock);
      lock_acquire(&ce->entry_lock);
    } else {
      lock_release(&bc.global_lock);
    }

    while (ce->busy) {
      cond_wait(&ce->busy_cond, &ce->entry_lock);
    }
    ce->busy = true; /* Set to BUSY asa this ce is no longer busy */

    if (ce->valid && ce->dirty) {
      /* Flush this cache entry back to block */
      flush_block(ce);
    }
  }
}

void cache_write(struct cache_entry* ce, void* bounce, block_sector_t size) {
  ASSERT(ce);
  memcpy(ce->data, bounce, size);
  ce->dirty = true;
  ce->valid = true;
}

void cache_read_ptr(struct cache_entry* ce) {
  ASSERT(ce);
  if (!ce->valid) {
    // printf("thread magic == %lu\n", thread_current()->magic);
    block_read(fs_device, ce->sector, ce->data);
    ce->valid = true;
  }
  ASSERT(thread_current()->magic == THREAD_MAGIC);
}

// void cache_read(struct cache_entry *ce, void *bounce) {
//   uint8_t *p;
//   cache_read_ptr(ce, &p);

// }

/* Cache Buffer eviction policy */
/* Clock Policy: 
    update e.accessed = true when setting
    Get: 
      for-loop entries to find id == sector

    Set: 
      if sector in entries: update src & dirty
      else:
        if has invalid in entries: set entry
        else:
          for-loop from clock_hand:
            if accessed == true: accessed = false
            else: evict this entry & flush 
*/
static void clock_init(struct buffer_cache* c) {
  c->policy_state = malloc(sizeof(struct clock_state));
  ((struct clock_state*)c->policy_state)->hand = 0;
}

static void clock_destroy(struct buffer_cache* c) {}

/* Find entry with sector == sector, assume global lock is held,
   return with entry lock held
   no nned to hold global lock when looking up since location is fixed*/
static struct cache_entry* clock_entry_lookup(block_sector_t sector) {
  struct cache_entry* ce = NULL;
  for (int i = 0; i < CACHE_SIZE; i++) {
    struct cache_entry* tmp = &bc.ce[i];
    /* Attempt to acquire lock and wait for busy */
    // printf("lookup: trying to acquire entry lock: %d, addr: %p\n", sector, &tmp->entry_lock);
    lock_acquire(&tmp->entry_lock);

    /* Wait until busy == false, this situation could happen 
        when working on background job while already released the lock to reduce lock-holding time */
    while (tmp->busy) {
      cond_wait(&tmp->busy_cond, &tmp->entry_lock);
    }

    if (sector == tmp->sector) {
      ce = tmp;
      // printf("lookup: release entry lock: %d, addr: %p\n", sector, &tmp->entry_lock);
      // lock_release(&tmp->entry_lock);
      break;
    }
    // printf("lookup: release entry lock: %d, addr: %p\n", sector, &tmp->entry_lock);
    lock_release(&tmp->entry_lock);
  }

  /* Re-acquire global lock before return */
  // printf("acquire global lock addr: %p\n", &bc.global_lock);
  // lock_acquire(&bc.global_lock);
  return ce;
}

/* Called when cache entry is accessed, aussuming entry lock is held */
static void clock_on_access(struct buffer_cache* c, struct cache_entry* ce) { ce->accessed = true; }

/* Insert cache entry into buffer cache, assume entry lock is held */
static void clock_on_insert(struct buffer_cache* c, struct cache_entry* ce, block_sector_t sector) {
  ce->sector = sector;
  ce->accessed = true;
}

/*  Called when remocing cache entry from buffer cache, flush to block if dirty
    assume entry lock is held */
static void clock_on_remove(struct buffer_cache* c, struct cache_entry* ce) {
  flush_block(ce);
  /* Reset status to no_data slot */
  ce->dirty = false;
  ce->valid = false;
  ce->accessed = false;
}

/* Find next victim, assume global lock is held, return with entry lock held */
static struct cache_entry* clock_pick_victim(struct buffer_cache* c) {
  /* */
  int i = 0;
  struct cache_entry* ce = NULL;
  while (true) {
    // printf("keep find next victim\n");
    i = ((struct clock_state*)c->policy_state)->hand;
    ce = &c->ce[i];

    /* Advance to next slot if cant acquire entry lock or is busy */

    /* !! Requesting entry lock with global lock */
    if ((!lock_try_acquire(&ce->entry_lock)) || ce->busy) {
      /* Advance */
      if (++((struct clock_state*)c->policy_state)->hand == CACHE_SIZE) {
        ((struct clock_state*)c->policy_state)->hand = 0;
      }
      continue;
    }

    // printf("pick victim: acquire entry lock successfully : %d, addr: %p\n", ce->sector, &ce->entry_lock);
    /* Already get entry lock and release global lock */

    /* Break if invalid or already visited */
    if (!ce->valid || !ce->accessed) {
      // printf("release entry lock: %d, addr: %p\n", ce->sector, &ce->entry_lock);
      // lock_release(&ce->entry_lock);
      break;
    }
    ce->accessed = false;

    /* Write 0xFFFFFFFF into zeros */
    for (int i = 0; i < BLOCK_SECTOR_SIZE / 4; i++) {
      ce->data[i] = NO_SECTOR;
    }

    /* Release entry lock and Advance */
    lock_release(&ce->entry_lock);
    // printf("pick victim: release entry lock: %d, addr: %p\n", ce->sector, &ce->entry_lock);

    if (++((struct clock_state*)c->policy_state)->hand == CACHE_SIZE) {
      ((struct clock_state*)c->policy_state)->hand = 0;
    }
  }
  // printf("pick victim: successfully find ce\n");

  return ce;
}

/* Called to flush entry into disk, lock is held by caller */
static bool flush_block(struct cache_entry* ce) {
  // printf("called flush block\n");
  if (!ce->dirty) {
    return false;
  }
  // printf("flush_block: release entry lock: %d, addr: %p\n", ce->sector, &ce->entry_lock);
  // lock_release(&ce->entry_lock);

  /* Write data back to block */
  block_write(fs_device, ce->sector, ce->data);
  // printf("flush done\n");

  /* Re acquire entry lock for next status update */
  // printf("flush_block: trying to acquire entry lock: %d, addr: %p\n", ce->sector, &ce->entry_lock);
  // lock_acquire(&ce->entry_lock);
  return true;
}

const struct eviction_ops CLOCK_OPS = {
    .init = clock_init,
    .destroy = NULL,
    .entry_lookup = clock_entry_lookup,
    .on_access = clock_on_access,
    .on_insert = clock_on_insert,
    .on_remove = clock_on_remove,
    .pick_victim = clock_pick_victim,
};

/* Second chance list eviction policy */

/* LRU eviction policy */

// static void clock_set(block_sector_t sector, void *src) {
//   /* Find target block with sector == sector OR
//       Find a valid slot
//   */
//  printf("called set with sector: %d\n", sector);
//   struct cache_entry *ce = NULL;
//   lock_acquire(&bc.global_lock);
//   for (int i = 0; i < CACHE_SIZE; i++) {
//     lock_acquire(&bc.ce[i].entry_lock);
//     if (bc.ce[i].valid && bc.ce[i].sector == sector) {

//       /* Wait until busy == false */
//       while (bc.ce[i].busy) {
//         printf("1 Wait until busy == false with sector: %d\n", bc.ce[i].sector);
//         cond_wait(&bc.ce[i].busy_cond, &bc.ce[i].entry_lock);
//       }

//       printf("1 set busy true with with sector: %d\n", bc.ce[i].sector);
//       bc.ce[i].busy = true; /* Set to BUSY asa found one */
//       ce = &bc.ce[i];
//       printf("1. ce: %p, before: %p\n", ce, &bc.ce[i]);
//       lock_release(&bc.ce[i].entry_lock);
//       break;
//     }
//     lock_release(&bc.ce[i].entry_lock);
//   }
//   lock_release(&bc.global_lock);

//   /* If not found, then we need to find a vacant slot */
//   if (!ce) {
//     /* Find next available block */
//     lock_acquire(&bc.global_lock);
//     int i = 0;
//     while (true) {
//       i = bc.clock_hand;
//       lock_acquire(&bc.ce[i].entry_lock);

//       if (bc.ce[i].busy) {
//         /* Move to next slot */
//         bc.clock_hand++;
//         if (bc.clock_hand == CACHE_SIZE)
//           bc.clock_hand = 0;
//         lock_release(&bc.ce[i].entry_lock);
//         continue;
//       }

//       if (!bc.ce[i].valid || !bc.ce[i].accessed) {
//         /* Fill a new cache entry */

//         /* Wait until busy == false */
//         while (bc.ce[i].busy) {
//           printf("2 Wait until busy == false with sector: %d\n", bc.ce[i].sector);
//           cond_wait(&bc.ce[i].busy_cond, &bc.ce[i].entry_lock);
//         }

//         ce = &bc.ce[i];
//         printf("2. ce: %p, before: %p\n", ce, &bc.ce[i]);
//         printf("2 set busy == true with sector: %d\n", bc.ce[i].sector);

//         ce->busy = true; /* Set to BUSY asa found one */

//         /* Evict the old one if has   */
//         if (!ce->accessed) {
//           lock_release(&ce->entry_lock);
//           flush_block(ce);
//         } else {
//           lock_release(&ce->entry_lock);
//         }
//         break;

//       } else {
//         bc.ce[i].accessed = false;
//       }
//       lock_release(&bc.ce[i].entry_lock);
//       /* Move to next slot */
//       bc.clock_hand++;
//       if (bc.clock_hand == CACHE_SIZE)
//         bc.clock_hand = 0;
//     }
//     lock_release(&bc.global_lock);

//   }

//   /* Update data block */
//   if (!ce) {
//     printf("NO cache entry found\n");
//   }
//   printf("trying to acquire lock with ptr: %p and ce: %p\n", &ce->entry_lock, ce);
//   lock_acquire(&ce->entry_lock);
//   ce->valid = true;
//   ce->accessed = true;
//   ce->dirty = true;

//   ce->busy = false;
//   printf("2 set busy == false with sector: %d\n", ce->sector);
//   cond_signal(&ce->busy_cond, &ce->entry_lock);

//   ce->sector = sector;
//   memcpy(ce->data, src, BLOCK_SECTOR_SIZE);
//   lock_release(&ce->entry_lock);
//   return;

// }

// static void clock_get(block_sector_t sector, void **dst) {

//   /* Init dst = NULL BLOCK before read data */
//   if (dst)
//     memset(dst, 0, BLOCK_SECTOR_SIZE);

//   /* Find cache entry with sector == sector */
//   struct cache_entry *ce = NULL;
//   lock_acquire(&bc.global_lock);
//   for (int i = 0; i < CACHE_SIZE; i++) {
//     lock_acquire(&bc.ce[i].entry_lock);
//     if (!bc.ce[i].valid) {
//       lock_release(&bc.ce[i].entry_lock);
//       continue;
//     }

//     /* Wait until busy == false */
//     while (bc.ce[i].busy) {
//       printf("3 Wait until busy == false with sector: %d\n", bc.ce[i].sector);
//       cond_wait(&bc.ce[i].busy_cond, &bc.ce[i].entry_lock);
//     }

//     if (bc.ce[i].sector == sector) {
//       ce = &bc.ce[i];
//       printf("3 set busy == true with sector: %d\n", bc.ce[i].sector);

//       bc.ce[i].busy = true;
//       lock_release(&bc.ce[i].entry_lock);
//       break;
//     }
//     lock_release(&bc.ce[i].entry_lock);
//   }
//   lock_release(&bc.global_lock);

//   /* Read cache entry if find one */
//   if (!ce) return;
//   lock_acquire(&ce->entry_lock);
//   if (ce->valid && ce->sector == sector) {
//     memcpy(dst, ce->data, BLOCK_SECTOR_SIZE);
//     ce->readers++;
//     ce->accessed = true;
//     ce->busy = false;
//     printf("3 set busy == false with sector: %d\n", ce->sector);
//     cond_signal(&ce->busy_cond, &ce->entry_lock);
//   }
//   lock_release(&ce->entry_lock);
// }