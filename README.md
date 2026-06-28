# Distributed Software Update Framework
### ENCS4330 – Real-Time Applications & Embedded Systems – Project #3
**Sockets & POSIX threads under Linux**

A client/server software-update service. Clients connect to a central update
server, report their installed version, and the server decides whether an
update is needed. If so, it streams the update package to the client, which
stores it, verifies its SHA-256 checksum, and simulates installation. The
server serves **many clients concurrently** using one POSIX thread per
connection, so no client ever waits for another.

---

## 1. Components

| Binary        | Source            | Role |
|---------------|-------------------|------|
| `server`      | `src/server.c`    | Multithreaded update server (thread-per-client). |
| `server_adv`  | `src/server_adv.c`| **Advanced** server: process-per-client (`fork`) + worker thread, System V shared-memory stats guarded by a semaphore, SIGCHLD zombie-reaping, and SCHED_FIFO real-time scheduling. |
| `client`      | `src/client.c`    | Client app; `main()` calls `CheckForUpdate()`. |
| `monitor_gl`  | `src/monitor_gl.c`| OpenGL/GLUT live dashboard that visualises server activity. |

Shared modules: `netutil.c` (robust socket I/O), `logger.c` (thread-safe
timestamped logging), `config.c` (key=value config loader), `sha256.c`
(self-contained checksum for integrity).

---

## 2. Build

Requires `gcc`, `make`, `pthread`, and freeglut for the monitor:

```bash
sudo apt-get install build-essential freeglut3-dev      # Debian/Ubuntu
make
```

This produces `bin/server`, `bin/client`, and `bin/monitor_gl`. The `-g` flag
is on, so you can debug with `gdb` as the spec suggests.

---

## 3. Configuration (no hard-coded values)

All settings come from text files passed as a command-line argument, so you
can change behaviour without recompiling.

**`config/server.conf`**
```
port           = 9090
latest_version = 5
repo_dir       = server_repo
update_file    = update_v5.bin
backlog        = 32
log_file       = logs/server.log
```

**`config/client.conf`**
```
server_host     = 127.0.0.1
port            = 9090
current_version = 2
download_dir    = client_storage
client_name     = client
client_log_file = logs/client.log
```

---

## 4. Run

**Terminal 1 – server:**
```bash
./bin/server config/server.conf
```

**Terminal 2 – client** (optional CLI overrides: version, name):
```bash
./bin/client config/client.conf            # uses current_version from config
./bin/client config/client.conf 1 alice    # pretend to be on v1, name "alice"
./bin/client config/client.conf 5 bob      # already up to date
```

**Terminal 3 – OpenGL monitor (optional GUI):**
```bash
./bin/monitor_gl config/server.conf        # press q / ESC to quit
```

Client exit codes: `0` = already up to date, `2` = update downloaded,
`1` = error.

---

## 5. Protocol (client ⇄ server)

```
Client → Server :  VERSION <int>
Server → Client :  UPTODATE
                |  UPDATE <latest_ver> <filename> <size_bytes> <sha256hex>
                |  ERROR <description>
Server → Client :  <size_bytes> raw payload   (only after an UPDATE line)
```

Control lines are newline-terminated ASCII; the binary payload length is
announced in the `UPDATE` line so the client reads exactly that many bytes.

---

## 6. Testing

```bash
./run_tests.sh
```

Covers every scenario the spec lists: single client, up-to-date client,
client ahead of latest, large-file transfer integrity, invalid request
(server must not crash), 20 simultaneous clients, and concurrent-download
integrity. All checks pass; transferred files are verified byte-identical to
the server copy, and each connection runs on its own thread id (visible in the
logs).

To create a different update package:
```bash
head -c 200000 /dev/urandom > server_repo/update_v5.bin
```
The server recomputes the checksum automatically on each request.

---

## 7. Logging

Every event is logged to file **and** console with the required fields:
`timestamp`, severity, `thread id`, client `ip:port`, and event description.
Logged events include startup/shutdown, connection attempts, successful
connections, version requests, update decisions, transfer completion, failed
downloads, disconnections, and errors.

---

## 8. Error handling & reliability

- `SIGPIPE` ignored so a client vanishing mid-transfer cannot kill the server.
- `SIGINT`/`SIGTERM` trigger a graceful shutdown (clean log + statistics).
- All socket I/O loops over partial reads/writes and retries on `EINTR`.
- Malformed/garbage requests are rejected with `ERROR`; the server keeps running.
- Each client thread is detached and opens the update file independently, so
  concurrent downloads never race on shared state.

---

## 9. Advanced features implemented

- **Checksum validation** – SHA-256 of the package is sent in the `UPDATE`
  header and re-verified by the client after download.
- **Performance statistics** – the server logs totals (connections, updates
  sent, up-to-date replies) on shutdown.
- **GUI** – the OpenGL monitor gives a live, simple, elegant dashboard.
- **Configurable everything** via external config files.

---

## 10. Advanced server (`server_adv`) — processes + real-time IPC

`server_adv` is a drop-in alternative to `server`. It speaks the exact same
protocol and uses the same config and client, but demonstrates a much wider
set of real-time / IPC techniques from the course:

| Technique | Where | Why |
|-----------|-------|-----|
| **`fork()` per client** | accept loop | Each client runs in its own *process*. A crash in one handler can never affect the server or other clients (stronger isolation than threads). |
| **Worker `pthread` inside each child** | `xfer_thread()` | Combines processes **and** threads: a dedicated thread streams the file while the child's main thread handles protocol/logging. |
| **System V shared memory** | `shmget`/`shmat` | All forked children update the *same* statistics block; without it each child would only see a private copy and totals would be lost. |
| **System V semaphore** | `semop` P/V | Mutual exclusion on the shared stats **across processes** (a pthread mutex cannot synchronise separate processes). |
| **`SIGCHLD` handler** | `on_sigchld()` | Reaps finished children with `waitpid(WNOHANG)` so no zombie processes accumulate. |
| **`SCHED_FIFO` real-time scheduling** | `try_realtime()` | Requests a soft real-time scheduling policy so update delivery is prioritised. Degrades gracefully to the default scheduler if not run as root, so it always runs. |

Run it exactly like the base server:

```bash
./bin/server_adv config/server.conf          # advanced (fork + IPC)
# or
sudo ./bin/server_adv config/server.conf     # to actually obtain SCHED_FIFO
```

You can watch the real processes with `ps --ppid <server_pid>` while clients
connect, and confirm no IPC leaks afterwards with `ipcs`. Each log line now
carries both `pid=` and `tid=` so the per-process / per-thread activity is
visible.

**Which one to demo?** `server` is the canonical answer to "use POSIX
threads" (Project #3's core requirement). `server_adv` is the extra mile,
showing `fork`, shared memory, semaphores, signals and real-time scheduling on
top. Both pass the same test suite.
