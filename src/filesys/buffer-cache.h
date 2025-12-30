#include "devices/block.h"
#include <stdbool.h>
#include "threads/synch.h"
#include "filesys/filesys.h"
#include "devices/block.h"

#define ENTRY_MAGIC 0x494e4f44
#define NO_SECTOR 0xFFFFFFFF

struct eviction_ops;

struct cache_entry {
  struct lock entry_lock; /* Lock for this cache entry */
  struct condition busy_cond;

  block_sector_t sector; /* sector_id */
  bool valid;            /* Whethre this cache is able to read or not */
  bool dirty;            /* Need to flush when evicted */
  bool accessed;         /* Visited: true, else: false */
  // bool removed;      /* This entry is already removed, cannot access any more */
  // int refcnt; /* Refcnts, cannot evict if != 0 */
  bool
      busy; /* Unable to visit even w/o lock, used between cache_acquire and cache_release, cannot delete */
  uint8_t data[BLOCK_SECTOR_SIZE]; /* Actual Data */
  unsigned magic;                  /* Magic number. */
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
  void* policy_state;
};

struct eviction_ops {
  void (*init)(struct buffer_cache* c);
  void (*destroy)(struct buffer_cache* c);

  struct cache_entry* (*entry_lookup)(block_sector_t sector);

  // 命中/访问时（read/write 都算）
  void (*on_access)(struct buffer_cache* c, struct cache_entry* e);

  // entry 刚装入新 sector 后
  void (*on_insert)(struct buffer_cache* c, struct cache_entry* e, block_sector_t sector);

  // entry 即将被替换/淘汰前（从 policy 结构中摘掉它）
  void (*on_remove)(struct buffer_cache* c, struct cache_entry* e);

  // 选择一个可替换 entry（必须跳过 busy/pinned）
  struct cache_entry* (*pick_victim)(struct buffer_cache* c);
};

void cache_init(enum cache_type);
void cache_done(void);

struct cache_entry* cache_try_get(block_sector_t sector);
struct cache_entry* cache_acquire(block_sector_t sector);
void cache_release(struct cache_entry* e);

void cache_write(struct cache_entry* ce, void* bounce, block_sector_t size);
void cache_read_ptr(struct cache_entry* ce);

void cache_mark_dirty(struct cache_entry* e);

void cache_readahead(block_sector_t sector);

void cache_flush_all(void);
static bool flush_block(struct cache_entry* ce);

/* Clock eviction policy */
struct clock_state {
  size_t hand;
};
static struct cache_entry* entry_lookup(block_sector_t sector);
static void clock_on_access(struct buffer_cache* c, struct cache_entry* e);
static void clock_on_insert(struct buffer_cache* c, struct cache_entry* e, block_sector_t sector);
static void clock_on_remove(struct buffer_cache* c, struct cache_entry* e);
static struct cache_entry* clock_pick_victim(struct buffer_cache* c);

/* Second chance list eviction policy */
// struct sec_chance_lst_state;

/* LRU eviction policy */
// struct lru_state;
