# Distributed Job Runner — System Report (Phase 1)
**Course**: SOC 3010 Operating Systems — Inha University in Tashkent (Fall 2026)  
**Team**: Team 9  
**Team Leader**: ASRARXANOV ASADBEK  

**Team Members & Component Owners**:
- **ASRARXANOV ASADBEK**: Component 1 (Protocol and Client) — *Team Leader*
- **AZIMBOYEV ISFANDIYOR**: Component 2 (Scheduler and Policy)
- **AXMEDOV FERUZBEK**: Component 3 (Worker and Limits)
- **BAXTIYOROV BOTIRXO'JA**: Component 4 (Durability and Recovery)
- **AZIZOV ABRORXON**: Component 5 (Operations, Testing and Report)

---

### 1. System Architecture & Diagram

The system coordinates three C programs over TCP sockets using non-blocking I/O multiplexing (`poll`):

```
                        ┌─────────────────────────────────────────┐
                        │          Central Job Server             │
                        │        (POSIX poll Event Loop)          │
                        │                                         │
                        │  - Job Table & State Machine            │
                        │  - Priority with Aging Scheduler        │
                        │  - Worker Slot Pool (Capacity K)        │
                        └────────────▲───────────────▲────────────┘
                                     │               │
                      Client TCP     │               │ Worker TCP
                      (Port 5000)    │               │ (Port 5000)
                                     │               │
        ┌────────────────────────────┴─┐   ┌─────────┴──────────────────────────┐
        │         Client CLI           │   │            Worker Daemon           │
        │  ./client <host> <port>      │   │  ./worker <host> <port> [K]        │
        │                              │   │                                    │
        │  - submit "cmd" timeout [p]  │   │  - fork() child process            │
        │  - status <id>               │   │  - setpgid(0, 0) group isolation   │
        │  - cancel <id>               │   │  - dup2 unified stdout/stderr pipe │
        │  - fetch <id>                │   │  - kill(-pgid, SIGKILL) on timeout │
        └──────────────────────────────┘   └────────────────────────────────────┘
```

#### Key Architecture Principles
1. **Concurrency Without Blocking**: The server uses POSIX `poll()` over all listening, worker, and client sockets. Even if a client opens a connection and sends no bytes (e.g., `nc 127.0.0.1 5000`), the server immediately accepts and services other clients without delay.
2. **Strict Process Groups**: Jobs are launched under `/bin/sh -c "<cmd>"` after calling `setpgid(0, 0)`. When a timeout fires or a cancel is ordered, `kill(-pgid, SIGKILL)` terminates the shell and all children spawned by it (e.g. `sleep 100`).
3. **Descriptor Hygiene**: Unused pipe ends and closed sockets are reclaimed immediately. In children, all inherited file descriptors $> 2$ are closed prior to `exec`.

---

### 2. The Wire Protocol

All control messages use line framing (`\n`). Fetch output and job commands use length prefixes to support arbitrary binary streams, special characters, and multi-line strings.

#### Client $\leftrightarrow$ Server
- **Submit**:
  - `SUBMIT <timeout> <priority> <cmd_length>\n<cmd_bytes>`
  - Response: `JOB_ID <id>\n` (e.g. `JOB_ID T9-1`)
- **Status**:
  - `STATUS <id>\n`
  - Response: `STATE <state_detail>\n` (e.g. `STATE QUEUED`, `STATE RUNNING`, `STATE DONE 0`, `STATE KILLED TIMEOUT`, `STATE KILLED SIGNAL 9`, `STATE CANCELLED`) or `UNKNOWN\n`
- **Cancel**:
  - `CANCEL <id>\n`
  - Response: `CANCEL_OK\n`, `ALREADY_FINISHED\n`, or `UNKNOWN\n`
- **Fetch**:
  - `FETCH <id>\n`
  - Response: `FETCH_OK <len>\n<binary_bytes>`, `NOT_FINISHED\n`, or `UNKNOWN\n`

#### Worker $\leftrightarrow$ Server
- **Handshake**:
  - `WORKER_HELLO <K>\n` $\rightarrow$ `WORKER_OK\n`
- **Dispatch**:
  - `DISPATCH <id> <timeout> <cmd_length>\n<cmd_bytes>`
- **Report**:
  - `REPORT <id> <state_type> <state_arg> <output_len>\n<output_bytes>`
- **Cancel Relay**:
  - `CANCEL_JOB <id>\n`

---

### 3. Scheduling Policy: Priority with Aging

Our scheduler implements **Priority with Aging**:
- Base priority ranges from $0$ (lowest) to $9$ (highest). Default is $0$.
- To prevent indefinite starvation of lower-priority jobs, the server continuously computes each queued job's effective priority:
  $$\text{Effective Priority} = \text{Base Priority} + \left\lfloor \frac{\text{Current Time} - \text{Submit Time}}{\text{Aging Interval (5 seconds)}} \right\rfloor$$
- When a worker slot is freed, the scheduler selects the queued job with the highest effective priority. Ties are broken using FIFO (earliest submit time).

#### Proof of Policy Trace
Below is an excerpt from our scheduler trace demonstrating that high priority jobs jump ahead of low priority jobs, and aged low priority jobs eventually run:
```text
[T=0s] Job A submitted: priority 0, effective priority = 0
[T=1s] Job B submitted: priority 8, effective priority = 8
[T=1s] Worker slot available -> Job B dispatched (Priority 8 > Priority 0)
[T=15s] Job C submitted: priority 1
[T=15s] Job A has waited 15s -> aging bonus +3 -> effective priority = 3
[T=15s] Worker slot available -> Job A dispatched (Effective Priority 3 > Priority 1)
```

---

### 4. Process Management & Resource Hygiene

1. **Zero Zombies**: Worker monitors children with `waitpid(-1, &status, WNOHANG)` inside its polling loop. When any child exits, its status is captured, its pipe read to EOF and closed, and the slot is freed. No child remains in state `Z`.
2. **Zero Leaked Descriptors**:
   - Pipe read end is closed immediately in child.
   - Pipe write end is closed immediately in worker parent upon `fork()`.
   - All inherited descriptors ($3 \dots 1024$) are closed in child before `execl()`.
   - Verified via `ls /proc/<pid>/fd | wc -l`: identical count before and after 100 jobs.
3. **Clean SIGTERM Handling**:
   - `server`: Sets signal flag, interrupts poll, prints shutdown message, closes listening socket (configured with `SO_REUSEADDR`), and exits within 2 seconds. A new server binds immediately.
   - `worker`: Sends `kill(-pgid, SIGKILL)` to all running children, reaps them, sends `KILLED SIGNAL 9` reports to server, closes connections, and exits within 2 seconds.

---

### 5. Real Failure Analysis & What We Changed

- **Encountered Failure**: During initial development of multi-client testing, when 8 clients submitted simultaneously, child processes spawned by `/bin/sh` were occasionally orphaned when killed via `kill(pid, SIGKILL)` during cancel and timeout tests. For example, `sh -c "sleep 100"` spawned a distinct PID for `sleep`, and killing only the shell left `sleep` alive in the background.
- **Root Cause**: The shell creates a child process to run `sleep`. Delivering `SIGKILL` to the parent shell process does not cascade `SIGKILL` to grandchildren in Linux.
- **Resolution**:
  1. In the child process immediately after `fork()`, we invoked `setpgid(0, 0)` to create a new process group where the child is the process group leader.
  2. In worker timeout, cancel, and SIGTERM handlers, we invoked `kill(-job->pid, SIGKILL)` with the negative PID, directing the signal to the entire process group.
  3. This completely eliminated orphaned child processes, successfully passing Rows 4, 5, and 12 of §5.4.
