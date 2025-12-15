
#include "sync_table.h"
#include "threads/synch.h"
#include "userprog/process.h"

/* Initialize the sync table for currrent process */
void initialize_sync_table(struct sync_table* table) {
  lock_init(&table->tlock_lock);
  lock_init(&table->tsema_lock);
  table->lock_id = 0;
  table->sema_id = 0;
  memset(&table->lock_table, 0, sizeof(table->lock_table));
  memset(&table->sema_table, 0, sizeof(table->sema_table));

  for (int i = 0; i < SYNCH_MAX_COUNT; i++) {
    struct sema_t* st = &table->sema_table[i];
    st->initialized = false;
    st->holder = -1;
    struct lock_t* lt = &table->lock_table[i];
    lt->holder = -1;
    lt->initialized = false;
  }
}

/* Create user-level lock */
void create_new_lock(char* tlock_id) {
  struct thread* ct = thread_current();
  struct process* p = ct->pcb;

  lock_acquire(&p->sync_table->tlock_lock);

  uint8_t* id = &p->sync_table->lock_id;
  struct lock_t* lt = &p->sync_table->lock_table[*id];

  lt->holder = -1;
  lt->initialized = true;
  lt->locked = false;
  lock_init(&lt->lock);

  *tlock_id = *(char*)id;
  *id += 1;

  lock_release(&p->sync_table->tlock_lock);
}

/* Create user-level semaphore */
void create_new_sema(char* tsema_id, int val) {
  struct thread* ct = thread_current();
  struct process* p = ct->pcb;

  lock_acquire(&p->sync_table->tsema_lock);

  uint8_t* id = &p->sync_table->sema_id;
  struct sema_t* st = &p->sync_table->sema_table[*id];

  st->holder = ct->tid;
  st->locked = false;
  st->initialized = true;
  sema_init(&st->sema, val);

  *tsema_id = *(char*)id;
  *id += 1;

  lock_release(&p->sync_table->tsema_lock);
}

/* Acquire user-level lock */
bool acquire_tlock(char* lock_id) {
  struct thread* t = thread_current();
  struct process* p = t->pcb;

  uint8_t id = *(uint8_t*)lock_id;
  if (id < 0 || id >= SYNCH_MAX_COUNT) {
    return false;
  }
  struct lock_t* lock_t = &p->sync_table->lock_table[id];

  if (!lock_t->initialized) {
    return false;
  }
  if (lock_t->holder == t->tid) {
    return false;
  }
  // if (lock_t->holder != -1 || lock_t->locked) {
  //   return false;
  // }
  lock_acquire(&lock_t->lock);

  lock_t->holder = t->tid;
  lock_t->locked = true;
  return true;
}

/* Release user-level lock */
bool release_tlock(char* lock_id) {
  struct thread* t = thread_current();
  struct process* p = t->pcb;

  uint8_t id = *(uint8_t*)lock_id;
  if (id < 0 || id >= SYNCH_MAX_COUNT) {
    return false;
  }
  struct lock_t* lock_t = &p->sync_table->lock_table[id];

  if (!lock_t->initialized || lock_t->holder == -1) {
    return false;
  }
  if (lock_t->holder != t->tid || !lock_t->locked) {
    return false;
  }

  lock_t->holder = -1;
  lock_t->locked = false;

  lock_release(&lock_t->lock);
  return true;
}

/* Up user-level semaphore */
bool sema_up_t(char* sema_id) {
  struct thread* t = thread_current();
  struct process* p = t->pcb;

  uint8_t id = *(uint8_t*)sema_id;
  if (id < 0 || id >= SYNCH_MAX_COUNT) {
    return false;
  }
  struct sema_t* sema_t = &p->sync_table->sema_table[id];

  if (!sema_t->initialized) {
    return false;
  }
  // if (sema_t->holder == -1) {
  //   return false;
  // }
  // if (sema_t->holder == -1) {
  //   sema_t->holder = t->tid;
  // }
  sema_t->locked = true;
  sema_up(&sema_t->sema);
  return true;
}

/* Down user-level semaphore */
bool sema_down_t(char* sema_id) {
  struct thread* t = thread_current();
  struct process* p = t->pcb;

  uint8_t id = *(uint8_t*)sema_id;
  if (id < 0 || id >= SYNCH_MAX_COUNT) {
    return false;
  }
  struct sema_t* sema_t = &p->sync_table->sema_table[id];

  if (!sema_t->initialized) {
    return false;
  }
  // if (sema_t->holder == -1) {
  //   return false;
  // }

  sema_t->locked = false;
  sema_down(&sema_t->sema);
  return true;
}
