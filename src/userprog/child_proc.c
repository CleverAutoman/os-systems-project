
#include "process.h"
#include <debug.h>

static size_t cp_ct = 0;

void create_child_proc(struct child_proc* cp) {
  ASSERT(cp);
  cp->waited = false;
  cp->exited = false;
  cp->load_ok = false;
  cp->exit_status = 0;

  // semaphore
  sema_init(&cp->load_sema, 0);
  sema_init(&cp->wait_sema, 0);

  // list_elem
  cp_ct++;
  // printf("cp count = %lu\n", cp_ct);
}

struct child_proc* find_child_with_tid(struct process* cur_proc, tid_t tid) {
  if (!cur_proc) {
    return NULL;
  }
  struct list_elem* e;

  for (e = list_begin(&cur_proc->children); e != list_end(&cur_proc->children); e = list_next(e)) {
    struct child_proc* c_proc = list_entry(e, struct child_proc, elem);
    if (tid == c_proc->tid) {
      return c_proc;
    }
  }
  return NULL;
}

void create_fork_proc(struct fork_proc* fp) {
  ASSERT(fp);
  // printf("craeted!!!\n");
  fp->load_ok = false;

  // semaphore
}

void free_fork_proc(struct fork_proc* fp) {
  // printf("FREE_FORK: called with pid: %d\n", thread_current()->tid);
  if (!fp)
    return;
  free(fp);
}