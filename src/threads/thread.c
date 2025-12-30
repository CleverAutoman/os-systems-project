#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/vaddr.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list fifo_ready_list;

static struct p_bucket {
  int priority;
  struct list threads;
  struct list_elem elem; // for  prio_ready_list
};

/* List of priority list of p_bucket with same priority */
static struct list prio_buckets[PRI_BOUND]; // Effective Priority

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread* idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread* initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame {
  void* eip;             /* Return address. */
  thread_func* function; /* Function to call. */
  void* aux;             /* Auxiliary data for function. */
};

/* Statistics. */
static long long idle_ticks;   /* # of timer ticks spent idle. */
static long long kernel_ticks; /* # of timer ticks in kernel threads. */
static long long user_ticks;   /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4          /* # of timer ticks to give each thread. */
static unsigned thread_ticks; /* # of timer ticks since last yield. */

static void init_thread(struct thread*, const char* name, int priority);
static bool is_thread(struct thread*) UNUSED;
static void* alloc_frame(struct thread*, size_t size);
static void schedule(void);
static void thread_enqueue(struct thread* t);
static tid_t allocate_tid(void);
void thread_switch_tail(struct thread* prev);

static void kernel_thread(thread_func*, void* aux);
static void idle(void* aux UNUSED);
static struct thread* running_thread(void);

static struct thread* next_thread_to_run(void);
static struct thread* thread_schedule_fifo(void);
static struct thread* thread_schedule_prio(void);
static struct thread* thread_schedule_fair(void);
static struct thread* thread_schedule_mlfqs(void);
static struct thread* thread_schedule_reserved(void);

/* Strict Priority: */
static void prio_add_thread(struct thread* t);
static struct thread* prio_get_and_pop_next_thread(void);  // get & pop
static void remove_thread(struct thread* t, int priority); // remove_thread
static void update_thread(struct thread* t, int prev_effective_priority, int new_priority);
static void update_thread_donated_priority(struct thread* t, int prev_effective_priority,
                                           int new_priority);

/* Determines which scheduler the kernel should use.
   Controlled by the kernel command-line options
    "-sched=fifo", "-sched=prio",
    "-sched=fair". "-sched=mlfqs"
   Is equal to SCHED_FIFO by default. */
enum sched_policy active_sched_policy;

/* Selects a thread to run from the ready list according to
   some scheduling policy, and returns a pointer to it. */
typedef struct thread* scheduler_func(void);

/* Jump table for dynamically dispatching the current scheduling
   policy in use by the kernel. */
scheduler_func* scheduler_jump_table[8] = {thread_schedule_fifo,     thread_schedule_prio,
                                           thread_schedule_fair,     thread_schedule_mlfqs,
                                           thread_schedule_reserved, thread_schedule_reserved,
                                           thread_schedule_reserved, thread_schedule_reserved};

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void thread_init(void) {
  ASSERT(intr_get_level() == INTR_OFF);

  lock_init(&tid_lock);
  list_init(&fifo_ready_list);
  list_init(&all_list);

  /* STRICT PRIORITY: Initialize list inside prio_buckets */
  for (int8_t i = PRI_MIN; i < PRI_BOUND; i++) {
    list_init(&prio_buckets[i]);
  }

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread();
  init_thread(initial_thread, "main", PRI_DEFAULT);

  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid();
  initial_thread->requesting_lock = NULL;
}

/* Add tid into current pcb */
void add_current_thread_to_process(tid_t tid) {
  struct thread* t = thread_current();
  if (t->pcb) { // only for threads with PCB
    /* Create thread_end_status and Add current thtread to parent process */
    lock_acquire(&t->pcb->child_global_lock);
    struct thread_end_status* tes = calloc(sizeof(struct thread_end_status), 1);
    /* TODO Error Handling  */
    create_thread_end_status(tes, tid);
    tes->thread = t;
    list_push_back(&t->pcb->user_threads, &tes->user_elem);
    t->pcb->user_thread_count++;
    size_t ct = t->pcb->user_thread_count;
    tes->gen = t->pcb->gen++;

    lock_release(&t->pcb->child_global_lock);
    // printf("thread: %d, create tes with gen: %u\n", tid, tes->gen);
    // printf("having %u user thread cts after adding\n", ct);
  } else {
    // printf("thread: %d creating tes without pcb\n", tid);
  }
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void thread_start(void) {
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init(&idle_started, 0);
  tid_t tid = thread_create("idle", PRI_MIN, idle, &idle_started);
  /* Start preemptive thread scheduling. */
  intr_enable();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down(&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void thread_tick(void) {
  struct thread* t = thread_current();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pcb != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return();
}

/* Prints thread statistics. */
void thread_print_stats(void) {
  printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n", idle_ticks, kernel_ticks,
         user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t thread_create(const char* name, int priority, thread_func* function, void* aux) {
  struct thread* t;
  struct kernel_thread_frame* kf;
  struct switch_entry_frame* ef;
  struct switch_threads_frame* sf;
  tid_t tid;

  ASSERT(function != NULL);

  /* Allocate thread. */
  t = palloc_get_page(PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread(t, name, priority);
  tid = t->tid = allocate_tid();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame(t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame(t, sizeof *ef);
  ef->eip = (void (*)(void))kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame(t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* Add to run queue. */
  thread_unblock(t);

  /* Add into PCB's user_threads */
  /* !Unstable, should change func's signature, but gonna change too much*/
  struct thread* ct = thread_current();

  // if (function == start_pthread) { // create new user threads

  int current_priority = MAX(ct->priority, ct->donated_priority);
  if (current_priority < priority) {
    thread_yield();
  }
  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void thread_block(void) {
  ASSERT(!intr_context());
  ASSERT(intr_get_level() == INTR_OFF);

  thread_current()->status = THREAD_BLOCKED;
  schedule();
}

/* Places a thread on the ready structure appropriate for the
   current active scheduling policy.
   
   This function must be called with interrupts turned off. */
static void thread_enqueue(struct thread* t) {
  ASSERT(intr_get_level() == INTR_OFF);
  ASSERT(is_thread(t));

  switch (active_sched_policy) {
    case SCHED_FIFO: {
      list_push_back(&fifo_ready_list, &t->elem);
      break;
    }
    case SCHED_PRIO: {
      prio_add_thread(t);
      break;
    }
    default:
      PANIC("Unimplemented scheduling policy value: %d", active_sched_policy);
      break;
  }
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void thread_unblock(struct thread* t) {
  enum intr_level old_level;

  ASSERT(is_thread(t));

  old_level = intr_disable();
  ASSERT(t->status == THREAD_BLOCKED);
  thread_enqueue(t);
  t->status = THREAD_READY;
  intr_set_level(old_level);
}

/* Returns the name of the running thread. */
const char* thread_name(void) { return thread_current()->name; }

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread* thread_current(void) {
  struct thread* t = running_thread();

  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT(is_thread(t));
  ASSERT(t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t thread_tid(void) { return thread_current()->tid; }

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void thread_exit(void) {
  // printf("THREAD_EXIT: tid=%d\n", thread_current()->tid);
  ASSERT(!intr_context());

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_switch_tail(). */
  // printf("file_name before thread_exit: %s\n", thread_current()->pcb->process_name);
  intr_disable();
  list_remove(&thread_current()->allelem);
  thread_current()->status = THREAD_DYING;
  schedule();
  NOT_REACHED();
}

/* End user thread *t  */
void end_user_thread(struct thread* t) {
  ASSERT(!intr_context());
  intr_disable();
  list_remove(&t->allelem);

  t->status = THREAD_DYING;
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void thread_yield(void) {
  struct thread* cur = thread_current();
  enum intr_level old_level;

  ASSERT(!intr_context());

  old_level = intr_disable();
  if (cur != idle_thread)
    thread_enqueue(cur);
  cur->status = THREAD_READY;
  schedule();
  intr_set_level(old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void thread_foreach(thread_action_func* func, void* aux) {
  struct list_elem* e;

  ASSERT(intr_get_level() == INTR_OFF);

  for (e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e)) {
    struct thread* t = list_entry(e, struct thread, allelem);
    func(t, aux);
  }
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void thread_set_priority(int new_priority) {
  enum intr_level old_level = intr_disable();
  struct thread* t = thread_current();
  int prev_effective_priority = MAX(t->priority, t->donated_priority);
  update_thread(t, prev_effective_priority, new_priority);
  // printf("highest: %d\n", get_highest_priority());
  // printf("current name: %s, priority: %d\n", t->name, thread_get_priority());

  if (thread_get_priority() < get_highest_priority()) {
    // printf("yielded\n");
    thread_yield();
  }

  intr_set_level(old_level);
}

/* Move thread from prev_priority to new_priority */
void thread_set_donated_priority(struct thread* t, int prev_priority, int new_priority) {
  update_thread_donated_priority(t, prev_priority, new_priority);
}

/* Returns the current thread's priority. */
int thread_get_priority(void) {
  struct thread* t = thread_current();
  return MAX(t->priority, t->donated_priority);
}

/* Sets the current thread's nice value to NICE. */
void thread_set_nice(int nice UNUSED) { /* Not yet implemented. */
}

/* Returns the current thread's nice value. */
int thread_get_nice(void) {
  /* Not yet implemented. */
  return 0;
}

/* Returns 100 times the system load average. */
int thread_get_load_avg(void) {
  /* Not yet implemented. */
  return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int thread_get_recent_cpu(void) {
  /* Not yet implemented. */
  return 0;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void idle(void* idle_started_ UNUSED) {
  struct semaphore* idle_started = idle_started_;
  idle_thread = thread_current();
  sema_up(idle_started);

  for (;;) {
    /* Let someone else run. */
    intr_disable();
    thread_block();

    /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
    asm volatile("sti; hlt" : : : "memory");
  }
}

/* Function used as the basis for a kernel thread. */
static void kernel_thread(thread_func* function, void* aux) {
  ASSERT(function != NULL);

  intr_enable(); /* The scheduler runs with interrupts off. */

  function(aux); /* Execute the thread function. */
  thread_exit(); /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread* running_thread(void) {
  uint32_t* esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm("mov %%esp, %0" : "=g"(esp));
  return pg_round_down(esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool is_thread(struct thread* t) { return t != NULL && t->magic == THREAD_MAGIC; }

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void init_thread(struct thread* t, const char* name, int priority) {
  enum intr_level old_level;

  ASSERT(t != NULL);
  ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT(name != NULL);

  memset(t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy(t->name, name, sizeof t->name);
  t->stack = (uint8_t*)t + PGSIZE;
  t->priority = priority;
  t->pcb = NULL;
  t->magic = THREAD_MAGIC;
  t->exit_status = 0;
  list_init(&t->holding_locks); /* For locks holden by this thread */

  /* Multithreading: Initialize join sema */

  old_level = intr_disable();
  list_push_back(&all_list, &t->allelem);
  intr_set_level(old_level);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void* alloc_frame(struct thread* t, size_t size) {
  /* Stack data is always allocated in word-size units. */
  ASSERT(is_thread(t));
  ASSERT(size % sizeof(uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* First-in first-out scheduler */
static struct thread* thread_schedule_fifo(void) {
  if (!list_empty(&fifo_ready_list))
    return list_entry(list_pop_front(&fifo_ready_list), struct thread, elem);
  else
    return idle_thread;
}

/* Strict priority scheduler */
static struct thread* thread_schedule_prio(void) {
  // TODO support priority
  return prio_get_and_pop_next_thread();
  // PANIC("Unimplemented scheduler policy: \"-sched=prio\"");
}

/* Fair priority scheduler */
static struct thread* thread_schedule_fair(void) {
  PANIC("Unimplemented scheduler policy: \"-sched=fair\"");
}

/* Multi-level feedback queue scheduler */
static struct thread* thread_schedule_mlfqs(void) {
  PANIC("Unimplemented scheduler policy: \"-sched=mlfqs\"");
}

/* Not an actual scheduling policy — placeholder for empty
 * slots in the scheduler jump table. */
static struct thread* thread_schedule_reserved(void) {
  PANIC("Invalid scheduler policy value: %d", active_sched_policy);
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread* next_thread_to_run(void) {
  return (scheduler_jump_table[active_sched_policy])();
}

/* Strict Priority: */
/* Add new thread into strict priority waiting list */
static void prio_add_thread(struct thread* t) {
  if (!t)
    return;
  int t_priority = MAX(t->priority, t->donated_priority);
  if (t_priority > PRI_MAX) {
    return;
  }

  // printf("adding name:%s, priority: %d\n", t->name, t->priority);
  /* 1. Find bucket & add into list */
  /* 2. add into list */
  list_push_back(&prio_buckets[t_priority], &t->bucket_elem);
}

/* Get Most Signifact thread and pop and remove from bucket */
static void remove_thread(struct thread* t, int priority) {
  // int priority = MAX(t->donated_priority, t->priority);

  struct list* l = &prio_buckets[priority];
  if (list_empty(l)) {
    // printf("empty\n");
    return;
  }

  // printf("removing\n");
  for (struct list_elem* e = list_begin(l); e != list_end(l); e = list_next(e)) {
    struct thread* t_ = list_entry(e, struct thread, bucket_elem);
    if (t_ == t) {
      list_remove(e);
      return;
    }
  }
}

/* Get first thread and pop and remove bucket */
static struct thread* prio_get_and_pop_next_thread(void) {

  for (int8_t i = PRI_MAX; i >= PRI_MIN; i--) {
    struct list* l = &prio_buckets[i];
    if (list_empty(l)) {
      continue;
    }
    // struct list_elem *b_elem = list_begin(l);
    // struct thread *t = list_entry(b_elem, struct thread, bucket_elem);
    // /* Remove thread */
    // list_remove(b_elem);
    return list_entry(list_pop_front(l), struct thread, bucket_elem);
  }
  /* Return idelif no matched */
  return idle_thread;
}

/* Update thread's priority and change its location in list */
static void update_thread(struct thread* t, int prev_priority, int new_priority) {
  enum intr_level old_level = intr_disable();

  intr_set_level(old_level);
  if (t->status == THREAD_READY) {
    remove_thread(t, prev_priority);

    t->priority = new_priority;
    prio_add_thread(t);
  } else if (t->status == THREAD_RUNNING) {
    t->priority = new_priority;

  } else {
  }
  intr_set_level(old_level);
}
/* Update thread's donated_priority and change its location in list */
static void update_thread_donated_priority(struct thread* t, int prev_priority, int new_priority) {
  enum intr_level old_level = intr_disable();

  if (t->status == THREAD_READY) {
    remove_thread(t, prev_priority);

    t->donated_priority = new_priority;
    prio_add_thread(t);
  } else if (t->status == THREAD_RUNNING) {
    t->donated_priority = new_priority;

  } else {
    t->donated_priority = new_priority;
  }
  intr_set_level(old_level);
}

/* Get the highest priority in current list */
int get_highest_priority(void) {
  int priority = -1;
  for (int8_t i = PRI_MAX; i >= PRI_MIN; i--) {
    if (!list_empty(&prio_buckets[i])) {
      return i;
    }
  }
  return priority;
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_switch() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void thread_switch_tail(struct thread* prev) {
  struct thread* cur = running_thread();

  ASSERT(intr_get_level() == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) {
    ASSERT(prev != cur);
    palloc_free_page(prev);
  }
}

/* Schedules a new thread.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_switch_tail()
   has completed. */
static void schedule(void) {
  struct thread* cur = running_thread();
  struct thread* next = idle_thread;
  while (true) {
    next = next_thread_to_run();
    if (next != idle_thread && next->exited) {
      end_user_thread(next);
    } else {
      break;
    }
  }
  struct thread* prev = NULL;

  ASSERT(intr_get_level() == INTR_OFF);
  ASSERT(cur->status != THREAD_RUNNING);
  ASSERT(is_thread(next));

  if (cur != next)
    prev = switch_threads(cur, next);
  thread_switch_tail(prev);
}

/* Returns a tid to use for a new thread. */
static tid_t allocate_tid(void) {
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire(&tid_lock);
  tid = next_tid++;
  lock_release(&tid_lock);

  return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof(struct thread, stack);

/* Functions for thread_end_status */
void create_thread_end_status(struct thread_end_status* ti, tid_t tid) {
  ti->tid = tid;
  ti->exit_status = 0;
  ti->is_waited = false;
  ti->exited = false;
  ti->magic = 0x54455321;
  ti->gen = 0;
  lock_init(&ti->status_lock);
  sema_init(&ti->join_sema, 0);
}
