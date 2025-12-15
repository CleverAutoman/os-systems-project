#ifndef USERPROG_SYNC_TABLE_H
#define USERPROG_SYNC_TABLE_H
#define SYNCH_MAX_COUNT 1 << 8

#include "threads/synch.h"

#include <stdbool.h>

/* Pthread SYNCH */
struct lock_t {
  int holder; // or tid
  bool locked;
  bool initialized;
  struct lock lock;
};

struct sema_t {
  int holder; // or tid
  bool locked;
  bool initialized;
  struct semaphore sema;
};

struct sync_table {

  struct lock_t lock_table[SYNCH_MAX_COUNT];
  struct sema_t sema_table[SYNCH_MAX_COUNT];
  uint8_t lock_id;
  uint8_t sema_id;
  struct lock tlock_lock;
  struct lock tsema_lock;
};

void initialize_sync_table(struct sync_table* table);

void create_new_lock(char* tlock_id);
void create_new_sema(char* tsema_id, int val);

bool acquire_tlock(char* lock_id);
bool release_tlock(char* lock_id);

bool sema_up_t(char* sema_id);
bool sema_down_t(char* sema_id);

#endif /* USERPROG_SYNC_TABLE_H */