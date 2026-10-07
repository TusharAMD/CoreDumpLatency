# Linux Core Dump Lifecycle & Supervisor Lag Study

A hands-on, reproducible experimental test suite exploring what happens inside the **Linux Kernel** when a process crashes (`SIGSEGV`), how **Core Dumps** are written, why supervisor watchers experience **restart lag**, and how to optimize recovery time in production systems.

---

## 📌 Executive Summary / The Big Question

> **When a process crashes with Segmentation Fault, does it die immediately, or does it wait while the core file is being written? When does a watcher/supervisor know it died?**

### The Answer:
1. **The crashing process DOES NOT die immediately.**
   The moment an invalid memory address is dereferenced (`nullptr`), the CPU hardware raises a page fault trap. The kernel intercepts `SIGSEGV` and enters its internal `do_coredump()` routine.
2. **The process is frozen in kernel space.**
   All threads are halted, and the kernel synchronously writes all dirty memory pages to disk. During this time, the process is still visible in the process table (`/proc/<pid>`).
3. **Supervisor / Watcher Notification is BLOCKED:**
   Standard Unix process management (`waitpid()`, `SIGCHLD`, systemd, Docker, Kubernetes) **will NOT be notified until the entire core dump finishes flushing to disk**.
4. **The Downtime Problem:**
   If a service with 2 GB to 32 GB of memory crashes on slow disks, your supervisor may be **blind and blocked for 15 to 60+ seconds** before it can restart the service!

---

## 🔬 Experimental Architecture

This project provides two programs and an automated demo runner:

```
+-------------------------------------------------------------------------------+
|                                WATCHER (PID A)                                |
|  - Sets rlimit core = unlimited                                              |
|  - Spawns crasher via fork() & waits on waitpid()                             |
|  - Listens on IPC pipe for exact microsecond of crash trigger (T1)           |
|  - Monitors /proc/<pid>/stat states (R, S, D, Z)                              |
|  - Catches waitpid() unblock timestamp (T2)                                   |
|  - Calculates Exact Core Dump Lag = T2 - T1                                  |
+-------------------------------------------------------------------------------+
                                      │
                                      ▼ fork() + execv()
+-------------------------------------------------------------------------------+
|                            HUGE_CRASHER (PID B)                               |
|  - Allocates requested memory (0.2 GB, 0.5 GB, 1 GB, 2 GB)                    |
|  - Genuinely dirties every 4KB page so memory is physically in RAM            |
|  - Runs active CPU math/hashing loop (100% 'R' state, no sleep)              |
|  - Selects random countdown (5 to 60s) simulating real traffic                |
|  - Sends 1-byte notification to Watcher pipe right before crash               |
|  - Dereferences nullptr: *ptr = 1234  ──> TRAPS TO KERNEL                     |
+-------------------------------------------------------------------------------+
                                      │
                                      ▼ Kernel do_coredump()
                      [ Target: RAM (/dev/shm) vs DISK (/tmp) ]
```

---

## 📊 Benchmark Results

Observed experimental data comparing **RAM (`/dev/shm`)** vs **DISK (`/tmp`)**:

| Test Scenario | Allocated RAM | Target Medium | Observed Dump Lag (Delay) | Visible Process State | Impact on Service Restart |
|---|---|---|---|---|---|
| **Small Heap** | **200 MB (0.2 GB)** | **RAM (`/dev/shm`)** | **0.043 seconds** (43 ms) ⚡ | `State: R` | **Instant** (< 50ms) |
| **Small Heap** | **200 MB (0.2 GB)** | **DISK (`/tmp`)** | **0.300 seconds** (300 ms) | `State: R` | ~7x slower |
| **Medium Heap** | **1,024 MB (1.0 GB)** | **RAM (`/dev/shm`)** | **1.077 seconds** | `State: R` | Fast |
| **Medium Heap** | **1,024 MB (1.0 GB)** | **DISK (`/tmp`)** | **3.378 seconds** | `State: R` | **3x slower** |
| **Large Heap** | **2,048 MB (2.0 GB)** | **DISK (`/tmp`)** | **15.800 seconds** 🚨 | **`State: D`** (Disk I/O Stall) | **Severe Downtime Lag** |

---

## 🔍 Understanding Linux Process States During a Crash

During the experiment, the watcher monitors `/proc/<pid>/stat` every 200ms. Here is what each state means:

- **`R` (Running / Runnable):**
  Active on the CPU. The crasher stays in `R` while doing math calculations, and the kernel stays in `R` while copying memory pages into buffers.
- **`S` (Interruptible Sleep):**
  Waiting on a software event or timer (e.g. `nanosleep()` or waiting for user input).
- **`D` (Uninterruptible Sleep / Disk I/O Wait):**
  **Crucial learning:** `D` means the CPU thread is frozen waiting for physical storage hardware (SSD/disk controller) to catch up because page cache buffers are full.
  - **Does `D` mean the process is killed?** **NO!** Normal database transactions (`fsync()`) enter `D` state every day. It only means the process is waiting on hardware.
- **`Z` (Zombie) / `DEAD`:**
  The process has officially finished execution, the core dump is 100% complete, and the kernel has cleaned up its memory. Only now does `waitpid()` unblock.

---

## 🚀 How to Run the Demo

### Prerequisites
- Linux or WSL 2 (Oracle Linux, Ubuntu, RHEL, Debian, etc.)
- `g++` compiler

### Step 1: Navigate to Project Directory
```bash
cd /mnt/c/Users/Admin/.gemini/antigravity/scratch/cpp_demo
```

### Step 2: Run the Demo
Use the automated runner script:

```bash
./run_demo.sh <target> [memory_in_GB]
```

#### Examples:

1. **Compare 200 MB in RAM vs Disk:**
   ```bash
   ./run_demo.sh ram 0.2
   ./run_demo.sh disk 0.2
   ```

2. **Compare 1 GB in RAM vs Disk:**
   ```bash
   ./run_demo.sh ram 1
   ./run_demo.sh disk 1
   ```

3. **Observe Disk Stall (`State: D`) with 2 GB:**
   ```bash
   ./run_demo.sh disk 2
   ```

---

## 💡 Production Best Practices: How to Eliminate Crash Lag

If your production service holds gigabytes of memory and crashes, waiting 15–30 seconds for a core dump to flush causes extended downtime. Here are the 3 industry-standard solutions:

### Solution 1: Write Core Dumps to RAM (`/dev/shm`)
Configure Linux to write core dumps to the shared memory RAM-disk (`tmpfs`):
```bash
sudo sysctl -w kernel.core_pattern="/dev/shm/core.%e.%p"
```
- **Benefit:** Core is written at memory bus speeds (~10+ GB/s). `waitpid()` unblocks in milliseconds.
- **Persistence Pattern:** When your supervisor restarts the service, have a background task move and compress the core file asynchronously:
  ```bash
  # Background worker (runs with low I/O priority so it doesn't slow down the restarted app)
  ionice -c 3 nice -n 19 zstd -q -1 --rm /dev/shm/core.<PID> -o /var/crash/core.<PID>.zst &
  ```

### Solution 2: Cap the Core File Size (`ulimit -c`)
If you only need stack traces and register states for GDB, tell Linux not to dump the multi-gigabyte heap:
```bash
# Limit core file size to 20 MB (20480 KB)
ulimit -c 20480
```
- Core dump writes in **~0.05 seconds**.
- Stack trace, crash line, and registers are completely preserved for GDB.

### Solution 3: In-Process Crash Handlers (Google Breakpad / Sentry)
Instead of relying on the kernel ELF dumper, use in-process signal handlers to write a tiny **Minidump** (~50 KB) and terminate immediately via `_exit(1)`.

---

## 🛠️ File Structure

- [huge_crasher.cpp](file:///C:/Users/Admin/.gemini/antigravity/scratch/cpp_demo/huge_crasher.cpp) — Configurable memory allocator, busy CPU worker, and null pointer dereference trigger.
- [watcher.cpp](file:///C:/Users/Admin/.gemini/antigravity/scratch/cpp_demo/watcher.cpp) — Supervisor process that monitors child states, measures kernel lag, and formats the summary table.
- [run_demo.sh](file:///C:/Users/Admin/.gemini/antigravity/scratch/cpp_demo/run_demo.sh) — One-line script to toggle between RAM (`/dev/shm`) and DISK (`/tmp`) testing.
- `crasher.log` / `watcher.log` — Timestamped audit logs generated on each run.
