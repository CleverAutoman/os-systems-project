/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
*/

#include "threads/synch.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

static void add_ordered_thread(struct semaphore* sema, struct thread* t);
static bool priority_compare(const struct list_elem* a, const struct list_elem* b, void* aux);
static bool lock_refresh(struct lock* lock);
static void update_thread_waiter_location(struct semaphore* sema, struct thread* thread);

// /* TODO unfinished !!!!!! */
// static void lock_refresh(struct lock *lock) {

// }

/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
     decrement it.

   - up or "V": increment the value (and wake up one waiting
     thread, if any). */
void sema_init(struct semaphore* sema, unsigned value) {
  ASSERT(sema != NULL);

  sema->value = value;
  list_init(&sema->waiters);
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. */

void sema_down(struct semaphore* sema) {
  enum intr_level old_level;

  ASSERT(sema != NULL);
  ASSERT(!intr_context());

  old_level = intr_disable();
  struct thread* t = thread_current();

  if (active_sched_policy == SCHED_PRIO) {
    while (sema->value == 0) {
      /* Find the most prioritized thread when upping */
      list_push_back(&sema->waiters, &t->elem);
      thread_block();
    }
  } else {
    while (sema->value == 0) {
      list_push_back(&sema->waiters, &t->elem);
      thread_block();
    }
  }

  sema->value--;
  intr_set_level(old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool sema_try_down(struct semaphore* sema) {
  enum intr_level old_level;
  bool success;

  ASSERT(sema != NULL);

  old_level = intr_disable();
  if (sema->value > 0) {
    sema->value--;
    success = true;
  } else
    success = false;
  intr_set_level(old_level);

  return success;
}

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
void sema_up(struct semaphore* sema) {
  enum intr_level old_level;

  ASSERT(sema != NULL);

  old_level = intr_disable();

  if (active_sched_policy == SCHED_PRIO) {
    /* Find the most prioritized thread and unblock it; To FIFO for same priority, only update when max < iterated*/
    struct thread* nxt_t = NULL;
    struct list_elem* max_e = NULL;
    int max_priority = PRI_MIN - 1;

    if (!list_empty(&sema->waiters)) {
      for (struct list_elem* e = list_begin(&sema->waiters); e != list_end(&sema->waiters);
           e = list_next(e)) {
        struct thread* tmp = list_entry(e, struct thread, elem);
        int tmp_effectie_priority = MAX(tmp->donated_priority, tmp->priority);
        if (max_priority < tmp_effectie_priority) {
          max_priority = tmp_effectie_priority;
          max_e = e;
        }
      }
    }

    /* Add sema_value no matter what */
    sema->value++;
    intr_set_level(old_level);

    if (max_e) {
      list_remove(max_e);
      nxt_t = list_entry(max_e, struct thread, elem);
      thread_unblock(nxt_t);
      /* Yield current thread if nxt_priority is bigger */
      struct thread* cur_t = thread_current();
      int nxt_priority = MAX(nxt_t->donated_priority, nxt_t->priority);
      int cur_priority = MAX(cur_t->donated_priority, cur_t->priority);
      if (nxt_priority > cur_priority) {
        if (intr_context()) {
          intr_yield_on_return();
        } else {
          thread_yield();
        }
      }
    }

  } else {
    if (!list_empty(&sema->waiters)) {
      thread_unblock(list_entry(list_pop_front(&sema->waiters), struct thread, elem));
    }
    sema->value++;
    intr_set_level(old_level);
  }
}

static void sema_test_helper(void* sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void sema_self_test(void) {
  struct semaphore sema[2];
  int i;

  printf("Testing semaphores...");
  sema_init(&sema[0], 0);
  sema_init(&sema[1], 0);
  thread_create("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
  for (i = 0; i < 10; i++) {
    sema_up(&sema[0]);
    sema_down(&sema[1]);
  }
  printf("done.\n");
}

/* Thread function used by sema_self_test(). */
static void sema_test_helper(void* sema_) {
  struct semaphore* sema = sema_;
  int i;

  for (i = 0; i < 10; i++) {
    sema_down(&sema[0]);
    sema_up(&sema[1]);
  }
}

/* SEMA Helper functions*/
static bool priority_compare(const struct list_elem* a, const struct list_elem* b, void* aux) {
  struct thread* a_t = list_entry(a, struct thread, elem);
  struct thread* b_t = list_entry(b, struct thread, elem);
  int a_priority = MAX(a_t->priority, a_t->donated_priority);
  int b_priority = MAX(b_t->priority, b_t->donated_priority);
  return a_priority < b_priority;
}

// static void add_ordered_thread(struct semaphore* sema, struct thread *t) {
//   list_insert_orderd(&sema->waiters, t->);
// }

/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void lock_init(struct lock* lock) {
  ASSERT(lock != NULL);

  lock->holder = NULL;
  sema_init(&lock->semaphore, 1);
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void lock_acquire(struct lock* lock) {
  ASSERT(lock != NULL);
  ASSERT(!intr_context());
  ASSERT(!lock_held_by_current_thread(lock));

  /* 
  If in strict-priority policy, doante current priority to lock holder,
  (Priority Donation) Keep donating until the requesting lock is NULL;
  */
  struct thread* cur_t = thread_current();

  if (active_sched_policy == SCHED_PRIO) {
    struct lock* tmp_lock = lock;
    struct thread* tmp_t = cur_t;               // Iterated thread
    struct thread* holder_t = tmp_lock->holder; // Next thread

    /* Keep donating priority to nxt thread, until reach the thread owns no lock */
    while (tmp_t) {
      if (!holder_t) {
        break;
      }
      /* Track requesting lock */
      tmp_t->requesting_lock = tmp_lock;

      /* Donate priority if cur_priority is bigger than the lock holder */
      int tmp_effective_priority = MAX(tmp_t->donated_priority, tmp_t->priority);
      int nxt_effective_priority = MAX(holder_t->donated_priority, holder_t->priority);
      if (tmp_effective_priority > nxt_effective_priority) {
        holder_t->donated_priority = tmp_effective_priority;
      }
      int nxt_new_effective_priority = MAX(holder_t->donated_priority, holder_t->priority);
      if (nxt_effective_priority < nxt_new_effective_priority) {
        thread_set_donated_priority(holder_t, nxt_effective_priority, nxt_new_effective_priority);
        /* Update location of holder_t in waiting_list */
        // update_thread_waiter_location(&lock->semaphore, holder_t);
      }

      /* Next Iteration */
      tmp_t = holder_t;
      tmp_lock = holder_t->requesting_lock;
      if (!tmp_lock) {
        break;
      }
      holder_t = tmp_lock->holder;
    }

  } else { // other priority policy
  }

  /* Wait for lock */
  sema_down(&lock->semaphore);

  cur_t->requesting_lock = NULL;
  /* Sucessfully acquire the lock & add cur thread into list */
  list_push_back(&cur_t->holding_locks, &lock->holding_lock_elem);
  lock->holder = cur_t;
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool lock_try_acquire(struct lock* lock) {
  bool success;

  ASSERT(lock != NULL);
  ASSERT(!lock_held_by_current_thread(lock));

  success = sema_try_down(&lock->semaphore);
  if (success)
    lock->holder = thread_current();
  return success;
}

/*
  TODO UNFINSIHED!!!!!!!

  Remove lock of current thread, 
  reset the donated_priority to the MAX value of all waiters 
  in all waiting locks
*/
static bool lock_refresh(struct lock* lock) {
  struct thread* t = thread_current();
  struct list* holding_locks = &t->holding_locks;
  int prev_prtority = MAX(t->donated_priority, t->priority);
  /* Close Thread Switch */
  enum intr_level old = intr_disable();
  /* Remove lock */
  list_remove(&lock->holding_lock_elem);
  /* Update the MAX donated_priority through iteration */
  int max_donated_priority = 0;
  for (struct list_elem* e = list_begin(holding_locks); e != list_end(holding_locks);
       e = list_next(e)) {
    struct lock* lock_ = list_entry(e, struct lock, holding_lock_elem);
    struct list* waiters = &lock_->semaphore.waiters;
    for (struct list_elem* e_ = list_begin(waiters); e_ != list_end(waiters); e_ = list_next(e_)) {
      struct thread* t = list_entry(e_, struct thread, elem);
      int tmp_max = MAX(t->priority, t->donated_priority);
      max_donated_priority = MAX(max_donated_priority, tmp_max);
    }
  }
  thread_current()->donated_priority = max_donated_priority;
  intr_set_level(old);

  return max_donated_priority != prev_prtority;
}

/* Releases LOCK, which must be owned by the current thread.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void lock_release(struct lock* lock) {
  ASSERT(lock != NULL);
  ASSERT(lock_held_by_current_thread(lock));

  if (active_sched_policy == SCHED_PRIO) {

    /* Refresh current locks & get new effecitve donated_priority */
    bool updated = lock_refresh(lock);
    // if (updated) {
    //   update_thread_waiter_location(&lock->semaphore, thread_current());
    // }

    /* Reset donated priority forwarding */
    struct thread* tmp_t = thread_current();
    struct lock* tmp_lock;

    while (true) {
      tmp_lock = tmp_t->requesting_lock;
      if (!tmp_lock) {
        break;
      }
      struct thread* nxt_t = tmp_lock->holder;

      /* Update next thread's donated priority */
      int tmp_prev_effective_priority = MAX(tmp_t->donated_priority, tmp_t->priority);
      int nxt_effective_priority = MAX(nxt_t->donated_priority, nxt_t->priority);
      if (tmp_prev_effective_priority > nxt_effective_priority) {
        nxt_t->donated_priority = tmp_prev_effective_priority;
      }
      int tmp_new_effective_priority = MAX(tmp_t->donated_priority, tmp_t->priority);

      /* Update nxt thread's buckets in scheduler */
      if (tmp_prev_effective_priority < tmp_new_effective_priority) {
        thread_set_donated_priority(tmp_t, tmp_prev_effective_priority, tmp_new_effective_priority);
        /* Update location of holder_t in waiting_list */
        // update_thread_waiter_location(&lock->semaphore, tmp_t);
      }
    }
  } else {
  }

  lock->holder = NULL;
  sema_up(&lock->semaphore);

  /* Yield thread if necessary */
  if (active_sched_policy == SCHED_PRIO) {
    if (thread_get_priority() < get_highest_priority()) {
      if (intr_context()) {
        intr_yield_on_return();
      } else {
        thread_yield();
      }
    }
  }
}

static void update_thread_waiter_location(struct semaphore* sema, struct thread* t) {
  list_remove(&t->elem);
  list_insert_ordered(&sema->waiters, &t->elem, priority_compare, NULL);
}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool lock_held_by_current_thread(const struct lock* lock) {
  ASSERT(lock != NULL);

  return lock->holder == thread_current();
}

/* Initializes a readers-writers lock */
void rw_lock_init(struct rw_lock* rw_lock) {
  lock_init(&rw_lock->lock);
  cond_init(&rw_lock->read);
  cond_init(&rw_lock->write);
  rw_lock->AR = rw_lock->WR = rw_lock->AW = rw_lock->WW = 0;
}

/* Acquire a writer-centric readers-writers lock */
void rw_lock_acquire(struct rw_lock* rw_lock, bool reader) {
  // Must hold the guard lock the entire time
  lock_acquire(&rw_lock->lock);

  if (reader) {
    // Reader code: Block while there are waiting or active writers
    while ((rw_lock->AW + rw_lock->WW) > 0) {
      rw_lock->WR++;
      cond_wait(&rw_lock->read, &rw_lock->lock);
      rw_lock->WR--;
    }
    rw_lock->AR++;
  } else {
    // Writer code: Block while there are any active readers/writers in the system
    while ((rw_lock->AR + rw_lock->AW) > 0) {
      rw_lock->WW++;
      cond_wait(&rw_lock->write, &rw_lock->lock);
      rw_lock->WW--;
    }
    rw_lock->AW++;
  }

  // Release guard lock
  lock_release(&rw_lock->lock);
}

/* Release a writer-centric readers-writers lock */
void rw_lock_release(struct rw_lock* rw_lock, bool reader) {
  // Must hold the guard lock the entire time
  lock_acquire(&rw_lock->lock);

  if (reader) {
    // Reader code: Wake any waiting writers if we are the last reader
    rw_lock->AR--;
    if (rw_lock->AR == 0 && rw_lock->WW > 0)
      cond_signal(&rw_lock->write, &rw_lock->lock);
  } else {
    // Writer code: First try to wake a waiting writer, otherwise all waiting readers
    rw_lock->AW--;
    if (rw_lock->WW > 0)
      cond_signal(&rw_lock->write, &rw_lock->lock);
    else if (rw_lock->WR > 0)
      cond_broadcast(&rw_lock->read, &rw_lock->lock);
  }

  // Release guard lock
  lock_release(&rw_lock->lock);
}

/* One semaphore in a list. */
struct semaphore_elem {
  struct list_elem elem;      /* List element. */
  struct semaphore semaphore; /* This semaphore. */
  struct thread* thread;      /* For priority */
};

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void cond_init(struct condition* cond) {
  ASSERT(cond != NULL);
  list_init(&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void cond_wait(struct condition* cond, struct lock* lock) {
  struct semaphore_elem waiter;

  ASSERT(cond != NULL);
  ASSERT(lock != NULL);
  ASSERT(!intr_context());
  ASSERT(lock_held_by_current_thread(lock));

  sema_init(&waiter.semaphore, 0);
  waiter.thread = thread_current();

  if (active_sched_policy == SCHED_PRIO) {
    // struct thread *t = thread_current();
    // int priority = MAX(t->priority, t->donated_priority);
    // list_push_back(&cond->priority_waiters[priority], &waiter.elem);
    list_push_back(&cond->waiters, &waiter.elem);
  } else {
    list_push_back(&cond->waiters, &waiter.elem);
  }

  lock_release(lock);
  sema_down(&waiter.semaphore);
  lock_acquire(lock);
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void cond_signal(struct condition* cond, struct lock* lock UNUSED) {
  ASSERT(cond != NULL);
  ASSERT(lock != NULL);
  ASSERT(!intr_context());
  ASSERT(lock_held_by_current_thread(lock));

  if (active_sched_policy == SCHED_PRIO) {
    // printf("entered\n");
    if (list_empty(&cond->waiters))
      return;
    struct list_elem* max_e = NULL;
    int max_priority = -1;
    for (struct list_elem* e = list_begin(&cond->waiters); e != list_end(&cond->waiters);
         e = list_next(e)) {
      struct semaphore_elem* se = list_entry(e, struct semaphore_elem, elem);
      int cur_priority = MAX(se->thread->priority, se->thread->donated_priority);
      if (max_priority < cur_priority) {
        max_e = e;
        max_priority = cur_priority;
      }
    }
    list_remove(max_e);

    sema_up(&list_entry(max_e, struct semaphore_elem, elem)->semaphore);
  } else {
    if (!list_empty(&cond->waiters))
      sema_up(&list_entry(list_pop_front(&cond->waiters), struct semaphore_elem, elem)->semaphore);
  }
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void cond_broadcast(struct condition* cond, struct lock* lock) {
  ASSERT(cond != NULL);
  ASSERT(lock != NULL);

  while (!list_empty(&cond->waiters))
    cond_signal(cond, lock);
}
