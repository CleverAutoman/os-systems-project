# Operating System Project

## Overview

> For a Chinese version of this document, see README.zh-CN.md

This project extends the Pintos OS skeleton with three major subsystems:
**user programs (processes & syscalls)**, **thread scheduling and synchronization**,
and a **file system redesign** (FFS-style layout + Clock/Second-Chance buffer cache).
The primary focus is correctness under concurrency, robust resource cleanup,
and measurable performance improvements.

---

## Features

### 1. User Programs (Processes & Syscalls)

#### Argument Passing
- Implemented user stack setup with System V–style `argv/argc` layout.
- Ensured **16-byte stack alignment** to satisfy calling convention requirements.

#### Multi-process Syscalls
- Implemented `exec`, `wait`, `fork`, and `exit`, including parent–child synchronization.
- Passed process-tree stress tests:
  - **fork_tree**: reached `depth=3, fanout=4`, creating **84 child processes** in a single run;
    **148 total processes** across all test cases, with correct exit-status propagation.
  - **multi-oom-mt** (resource exhaustion + crash recovery):
    achieved **34 recursive exec depth** consistently across **10 repetitions**,
    correctly ending the crashed children and reclaiming all kernel resources.

#### File-related Syscalls
- Implemented file syscalls including `create`, `remove`, `tell`, `open`, `read`,
  `write`, and `close`.
- Designed an **O(1) bitmap-based file descriptor table** to efficiently locate
  free slots for new FDs.

---

### 2. Threading, Scheduling & Synchronization

#### Scheduling Policy
- Implemented **strict priority scheduling**.
- Added **priority donation** (including chained donation) to prevent priority inversion
  under lock contention.

#### Synchronization Primitives
- Implemented core primitives:
  - `semaphore`
  - `lock`
  - `condition variable (condvar)`
- Ensured correctness under preemption and priority scheduling.

#### User Threads (pthread-like API)
- Implemented user thread syscalls including `pthread_create`, `pthread_join`,
  and `pthread_exit`, covering thread-level exit, main-thread termination,
  and process-level exit with a unified and correct resource cleanup path.
- Adopted a **one-to-one threading model** instead of green threads to avoid
  process-wide blocking during I/O, enabling better concurrency under I/O-heavy workloads.
- Passed recursive multithreaded stress test (**search-arr**):
  - Each search spawns **31 threads** (binary recursion over 16 elements).
  - **48 searches**, creating **1,488 threads** total, validating per-thread user stacks,
    join semantics, and shared address-space behavior.

---

### 3. File System

Replaced the original list-based FAT design with an **FFS-style indexed allocation**
and added a **buffer cache** using the **Second-Chance Clock** eviction algorithm.

#### Fast File System (FFS-style)
- Implemented indexed allocation using direct and indirect blocks,
  replacing list-based FAT traversal.
- Supports files up to approximately **8 MB** (512-byte blocks).
- Added **lazy allocation / lazy loading** for sparse file growth to reduce I/O.


##### **Performance:**

**Sequential create + sequential read:**

- Microbenchmark A: `100B file × 200 loops`
  - baseline (FAT): **2183 ticks** → FFS: **980 ticks**  
    (**55.1% improvement**, **2.23× speedup**)

- Microbenchmark B: `50B file × 400 loops`
  - baseline (FAT): **4736 ticks** → FFS: **3040 ticks**  
    (**35.8% improvement**, **1.56× speedup**)

**Sequential create + random directory read (directory-locality stress):**

- Microbenchmark A: `100B file × 200 loops`
  - baseline (FAT): **3364 ticks** → FFS: **963 ticks**  
    (**71.4% improvement**, **3.49× speedup**)

- Microbenchmark B: `50B file × 400 loops`
  - baseline (FAT): **7472 ticks** → FFS: **1988 ticks**  
    (**73.4% improvement**, **3.76× speedup**)

**Result Summary:**
> 1. **FFS consistently outperforms FAT on small-file workloads:** In sequential create + read microbenchmarks, FFS achieves up to **2.23× speedup** (100B × 200), and **1.56× speedup** (50B × 400), demonstrating clear benefits under simple access patterns.
> 2. **The performance gap widens significantly under directory-level random access:**  When randomly accessing multiple small files within the same directory, FFS reaches up to **3.76× speedup**, substantially larger than in sequential cases. Also this test exposes the core weakness of FAT-style designs, which improves **3.49× speedup** under same total bytes read.
> 3. 

#### Buffer Cache (Second-Chance Clock)
- Implemented a block buffer cache with **Second-Chance Clock** replacement.
- Concurrency design:
  - A **global lock** protects cache-wide metadata.
  - **Per-entry locks** protect individual cache blocks.
  - Introduced a `busy` flag to avoid holding the global lock while waiting,
    reducing contention.
- Used a handle-based interface to enable **kernel-level zero-copy–style access**
  and reduce unnecessary memory copying.

**Performance (TODO):**
- Cache ON vs OFF for repeated reads / metadata-heavy workloads:
  - **[____]% latency reduction** or **[____]× throughput improvement**
- Cache hit rate under workload X: **[____]%**

---

## Validation & Stress Tests

- Process & syscalls: `fork_tree`, `multi-oom-mt`
- User threads: `search-arr`
- Additional correctness tests as provided by the Pintos test suite

---

## Key Design Decisions

- Prioritized correctness and deterministic behavior, especially for:
  - `wait` semantics
  - crash handling
  - resource cleanup paths
- Designed for robustness under failure:
  - syscall failures do not panic the kernel
  - abnormal process termination does not leak kernel resources
- Integrated priority-aware scheduling and donation across synchronization primitives.

---

## Challenges & Lessons Learned

- Kernel subsystems require well-defined invariants before implementation
  (process/thread lifecycles, ownership, and cleanup).
- Building incrementally reduces debugging complexity in OS development.
- Fine-grained commits and continuous testing are critical for system stability.

---

## TODO / Next Steps

- Fill in measured performance numbers for FFS microbenchmarks.
- Add cache ON/OFF benchmark results with hit rate statistics.
- Automate benchmarks for reproducible performance evaluation.
