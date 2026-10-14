# Inha University in Tashkent — Operating Systems (SOC 3010)
## Term Project — Distributed Job Runner (Fall 2026)

**Team Number**: Team 12 (Job IDs: `T12-1`, `T12-2`, ...)  
**Team Leader**: AZIMBOYEV ISFANDIYOR (also owns Component 1: Protocol & Client)

---

### Component Ownership Declaration (Team of 5)

Per project requirements (§3), a team of five declares five components, with the operations owner taking testing and load:

| # | Component | Declared Owner | Deliverables & Responsibilities | Key Fair Defense Question & Answer |
|---|---|---|---|---|
| 1 | **Protocol and Client** | **AZIMBOYEV ISFANDIYOR** (Team Leader) | Wire format framing, client CLI program, message parsing, error codes. | *"A client dies halfway through a submit. Where does your read loop handle that?"*<br>**Answer**: The server reads asynchronously in `handle_client_request()`. If client disconnects early, `read()` returns 0 or error, and the server cleans up `g_conns[slot]` without affecting other clients or corrupting the job table. |
| 2 | **Scheduler and Policy** | **ASRARXANOV ASADBEK** | Job queue, Priority with Aging scheduling policy, dispatch logic, policy trace. | *"Switch to priority now. What starves, and what stops it?"*<br>**Answer**: Low priority jobs (priority 0) would starve under heavy load. To prevent starvation, our aging algorithm calculates `effective_priority = base_priority + (wait_time / 5s)`. Every 5 seconds spent queued increments effective priority by +1, guaranteeing eventually every job runs. |
| 3 | **Worker and Limits** | **AXMEDOV FERUZBEK** | Worker daemon, process groups (`setpgid`), `fork`/`exec` via `/bin/sh -c`, stdout+stderr pipe capture, timeout enforcement, honest exit reporting, zero zombies. | *"Which line turns a timeout kill into KILLED, not DONE 0?"*<br>**Answer**: In `worker.c`, when the timer fires, we mark `job->kill_reason = KILL_TIMEOUT` before issuing `kill(-job->pid, SIGKILL)`. When `waitpid()` reaps the child, it checks `kill_reason` first. If `KILL_TIMEOUT`, it reports `KILLED_TIMEOUT 0` instead of a normal exit. |
| 4 | **Durability and Recovery** | **BAXTIYOROV BOTIRXO'JA** | Central job table, state machine transitions (`QUEUED`, `RUNNING`, `DONE`, `KILLED`, `CANCELLED`), cancel handling across server & worker, worker goodbye. | *"I cancel a running job. Which processes notice, and how?"*<br>**Answer**: The server receives `CANCEL <id>`, identifies the worker hosting it, and sends `CANCEL_JOB <id>` to that worker over TCP. The worker sends `kill(-pgid, SIGKILL)` to the job's entire process group, reaps the child, and reports `CANCELLED` back to the server. |
| 5 | **Operations, Testing & Report** | **AZIZOV ABRORXON** | `Makefile`, repo history, build verification, `tests/self_check.sh`, `tests/stress_test.sh`, descriptor leak and zombie checks, project report. | *"Run the test that proves no descriptor leaks. Which check would have caught last week's bug?"*<br>**Answer**: `tests/self_check.sh` row 11 measures `/proc/<pid>/fd` before and after 100 jobs. An unclosed write end or socket would cause FD counts to increase, immediately failing row 11. |


---

### Project Architecture & Design

The distributed job runner consists of three cooperating C programs:
1. **`server`**: A multiplexed TCP server utilizing non-blocking POSIX `poll()`. Maintains a centralized state machine for all jobs, manages worker capacities, and implements **Priority with Aging** scheduling.
2. **`worker`**: A worker daemon connecting to the server over TCP. Runs up to `K` simultaneous jobs as child process groups (`setpgid(0, 0)`), capturing both stdout and stderr into a pipe, enforcing client-specified timeouts via group kill (`kill(-pgid, SIGKILL)`), and honest status reporting.
3. **`client`**: A command-line client supporting the four required operations: `submit`, `status`, `cancel`, and `fetch`.

```
                    ┌──────────────────────────────────────────────┐
                    │               Central Server                 │
                    │         (poll event loop, port 5000)         │
                    │  - Priority with Aging Queue (T12-*)         │
                    │  - Worker Capacity & Slot Tracking           │
                    └───────────▲──────────────────────▲───────────┘
                                │                      │
                 TCP (Requests) │                      │ TCP (Dispatch & Reports)
                                │                      │
        ┌───────────────────────┴──────┐      ┌────────┴───────────────────────────┐
        │        Client CLI            │      │          Worker Daemon             │
        │ ./client <host> <port> <verb>│      │  ./worker <host> <port> [K]        │
        │ - submit                     │      │  - fork() & exec /bin/sh -c "cmd"  │
        │ - status                     │      │  - setpgid(0, 0) process groups    │
        │ - cancel                     │      │  - stdout/stderr unified pipe      │
        │ - fetch                      │      │  - kill(-pgid, SIGKILL) on timeout │
        └──────────────────────────────┘      └────────────────────────────────────┘
```

---

### Building the Project

The project compiles with standard `gcc` on Ubuntu Linux:
```bash
make clean
make
```
This produces the three required executables in the root directory:
- `server`
- `worker`
- `client`

---

### Running the Programs

#### 1. Start the Server:
```bash
./server 5000
```
Outputs:
```text
server: listening on port 5000
```
Then prints one line per event (`client connected`, `job accepted`, `job sent`, `job finished`).

#### 2. Start a Worker:
```bash
./worker 127.0.0.1 5000 2
```
Outputs:
```text
worker: connected to 127.0.0.1:5000, K=2
```

#### 3. Client Commands:
```bash
# Submit a job with timeout (10s) and priority (5)
./client 127.0.0.1 5000 submit "echo hello" 10 5
# Outputs job ID, e.g.:
# T12-1

# Check status of a job
./client 127.0.0.1 5000 status T12-1
# Outputs:
# T12-1 DONE 0

# Fetch captured output byte-for-byte
./client 127.0.0.1 5000 fetch T12-1
# Outputs:
# hello

# Cancel a job
./client 127.0.0.1 5000 cancel T12-1
```

---

### Testing & Verification

Run the automated self-check test suite implementing all 12 rows of Section 5.4:
```bash
./tests/self_check.sh
```

**Results Verified**:
- **Row 1**: Clean build and correct startup output messages.
- **Row 2**: Submitting `echo hello` -> `T12-1 DONE 0`, fetched output `hello`.
- **Row 3**: `exit 3` -> `DONE 3`; `ls /nonexistent` -> `DONE 2` with captured error message.
- **Row 4**: `sleep 100` with 2s timeout -> `KILLED TIMEOUT`; all child processes killed.
- **Row 5**: Cancel running `sleep 30` -> reports `CANCELLED` and terminates background jobs.
- **Row 6**: Serialization with `K=1` -> max concurrent count strictly $\le 1$; all done in under 12s.
- **Row 7**: 8 parallel clients submitting in the same second -> 8 unique IDs in $<2$s, all done in $<25$s.
- **Row 8**: Inactive `nc` connection does not block submissions from other clients.
- **Row 9**: Querying unknown ID (`T12-99999`) exits with code 2 and prints `T12-99999 UNKNOWN`.
- **Row 10**: Zero zombie processes (`state Z`) in worker process tree.
- **Row 11**: Zero file descriptor leaks before and after 100 jobs (`ls /proc/<pid>/fd | wc -l` remains identical).
- **Row 12**: Clean SIGTERM handling: worker kills jobs, reports `KILLED SIGNAL 9`, and exits cleanly; server shuts down and allows immediate restart on same port without `Address already in use`.
