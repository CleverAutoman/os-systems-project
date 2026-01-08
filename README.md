# Operating System Project

## Overview

This project extends a basic OS skeleton with three major subsystems:
**user programs (processes & syscalls)**, **thread scheduling and synchronization**,
and a **file system redesign** featuring an FFS-style layout and a
Clock/Second-Chance buffer cache.

The primary focus is **correctness under concurrency**, **robust resource cleanup**,
and **measurable performance improvements**, validated through stress tests and
microbenchmarks.

---

## Features

### 1. User Programs (Processes & Syscalls)

#### Argument Passing
- Implemented user stack setup with System V–style `argc/argv` layout.
- Ensured **16-byte stack alignment** to satisfy calling convention requirements.

#### Multi-process Syscalls
- Implemented `exec`, `wait`, `fork`, and `exit`, including parent–child synchronization.
- Passed process-tree stress tests:
  - **fork_tree**: reached `depth=3, fanout=4`, creating **84 child processes** in a single run;
    **148 total processes** across all test cases, with correct exit-status propagation.
  - **multi-oom-mt** (resource exhaustion + crash recovery):
    achieved **34 recursive exec depth** consistently across **10 repetitions**,
    correctly terminating crashed children and reclaiming all kernel resources.

#### File-related Syscalls
- Implemented file syscalls including `create`, `remove`, `tell`, `open`, `read`,
  `write`, and `close`.
- Designed an **O(1) bitmap-based file descriptor table** to efficiently locate
  free slots for new descriptors.

---

### 2. Threading, Scheduling & Synchronization

#### Scheduling Policy
- Implemented **strict priority scheduling**.
- Added **priority donation**, including chained donation, to prevent priority inversion
  under lock contention.

#### Synchronization Primitives
- Implemented core primitives:
  - `semaphore`
  - `lock`
  - `condition variable (condvar)`
- Ensured correctness under preemption and priority-aware scheduling.

#### User Threads (pthread-like API)
- Implemented user thread syscalls including `pthread_create`, `pthread_join`,
  and `pthread_exit`.
- Adopted a **one-to-one threading model** instead of green threads to avoid
  process-wide blocking during I/O.
- Passed recursive multithreaded stress test (**search-arr**):
  - Each search spawns **31 threads** (binary recursion over 16 elements).
  - **48 searches**, creating **1,488 threads** total, validating per-thread user stacks,
    join semantics, and shared address-space behavior.

---

### 3. File System

Replaced the original list-based FAT design with an **FFS-style indexed allocation**
scheme and added a **buffer cache** using the **Second-Chance Clock** eviction algorithm.

#### Fast File System (FFS-style)
- Implemented indexed allocation using direct, indirect, and doubly indirect blocks,
  replacing list-based FAT traversal.
- Supports files up to approximately **8 MB** (512-byte blocks).
- Added **lazy allocation and lazy loading** for sparse file growth to reduce
  unnecessary disk I/O.

##### Performance Evaluation (FFS vs FAT)

###### Sequential Create + Sequential Read

- **Microbenchmark A:** `100B file × 200 loops`  
  - FAT: **2183 ticks** → FFS: **980 ticks**  
    (**55.1% improvement**, **2.23× speedup**)

- **Microbenchmark B:** `50B file × 400 loops`  
  - FAT: **7736 ticks** → FFS: **3040 ticks**  
    (**60.7% improvement**, **2.54× speedup**)

###### Sequential Create + Random Directory Read (Directory Locality Stress)

- **Microbenchmark A:** `100B file × 200 loops`  
  - FAT: **3364 ticks** → FFS: **963 ticks**  
    (**71.4% improvement**, **3.49× speedup**)

- **Microbenchmark B:** `50B file × 400 loops`  
  - FAT: **7472 ticks** → FFS: **1988 ticks**  
    (**73.4% improvement**, **3.76× speedup**)

**Key Findings**
1. **FFS consistently outperforms FAT across both sequential and random workloads**,
   achieving speedups ranging from **2.23× to 3.76×**.
2. **Small-file access benefits most from FFS-style indexed allocation**, indicating
   more efficient handling of small-file metadata.
3. **Random access over small files yields the largest overall speedup**, highlighting
   improved directory locality and metadata organization.

---

#### Buffer Cache (Second-Chance Clock)

- Implemented a block buffer cache with **Second-Chance Clock** replacement.
- Concurrency design:
  - A **global lock** protects cache-wide metadata.
  - **Per-entry locks** protect individual cache blocks.
  - A `busy` flag is used to avoid holding the global lock while waiting,
    reducing contention under concurrent access.
- Used a handle-based interface to enable **kernel-level zero-copy–style access**
  and reduce unnecessary memory copying.

##### Performance Evaluation (Buffer Cache)

Under a repeated small I/O and metadata-heavy workload  
(`100B accesses × 200 loops`), enabling the buffer cache significantly reduced
disk I/O by serving most requests directly from memory.

- **Cache OFF:** ~**983 ticks**
- **Cache ON (global lock):** ~**188 ticks**  
  (**~80–82% latency reduction**, **~5× throughput improvement**)

**Key Findings**
- The global-lock buffer cache achieves the best performance under Pintos’
  **single-threaded** execution model.
- A per-entry lock design using a `busy` condition variable was also evaluated:
  - Execution time increased slightly to **~203 ticks** due to additional locking
    and condition-variable overhead.
  - While more scalable for concurrent workloads, this design provides no benefit
    in the single-threaded case.
- Therefore, the **global-lock design** was chosen as the final implementation for
  optimal performance and simplicity.

---

## Validation & Stress Tests

- Process & syscalls: `fork_tree`, `multi-oom-mt`
- User threads: `search-arr`
- Additional correctness tests from the Pintos test suite

---

## Key Design Decisions

- Prioritized correctness and deterministic behavior, especially for:
  - `wait` semantics
  - crash handling
  - resource cleanup paths
- Designed for robustness under failure:
  - syscall failures do not panic the kernel
  - abnormal process termination does not leak kernel resources
- Integrated priority-aware scheduling and donation across all synchronization
  primitives.

---

## Challenges & Lessons Learned

- Kernel subsystems require well-defined invariants before implementation
  (process/thread lifecycles, ownership, and cleanup).
- Incremental development significantly reduces debugging complexity in OS projects.
- Fine-grained commits and continuous testing are critical for maintaining
  kernel stability.
