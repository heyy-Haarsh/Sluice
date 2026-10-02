# Socket Programming Warm-up — Level 1 (basic) → Level 2 (epoll)

Goal: get comfortable with the socket API and *feel* why multiplexing matters,
before the real problem statement drops.

## 0. One-time setup (do this tonight, in your WSL2 terminal)

```bash
# Check you're on a real Linux env inside WSL2
uname -a

# Make sure you have a C++ compiler and make
sudo apt update
sudo apt install -y build-essential

# Confirm
g++ --version
make --version
```

## 1. Build everything

```bash
cd socket-tutorial
make all
```

This builds:
- `level1_basic/server` and `level1_basic/client`
- `level2_epoll/server`

## 2. Level 1 — basic blocking server (understand the fundamentals first)

Open **two terminals** in WSL2.

**Terminal A:**
```bash
./level1_basic/server
```

**Terminal B:**
```bash
./level1_basic/client
```

Type a message in Terminal B, hit enter, watch it echo back. Now open a
**third terminal** and try running `./level1_basic/client` again while
Terminal B's client is still connected — notice it just hangs waiting to
connect. That's the limitation: the server is blocked inside `accept()`/
`recv()` for one client at a time.

Read `level1_basic/server.cpp` top to bottom — every socket call is commented
with what it does and why. Make sure your team can explain each of:
`socket()`, `bind()`, `listen()`, `accept()`, `recv()`, `send()`.

## 3. Level 2 — epoll-based multi-client server

**Terminal A:**
```bash
./level2_epoll/server
```

**Terminal B, C, D (as many as you want):**
```bash
nc 127.0.0.1 8080
```
(`nc` = netcat, a quick way to act as a raw TCP client without writing one)

Type messages in different terminals — notice the server handles all of them
from a single thread, no blocking, no waiting in line. Watch Terminal A's
output to see connect/disconnect/message events interleaved from multiple
clients.

Read `level2_epoll/server.cpp` and focus on:
- `epoll_create1`, `epoll_ctl`, `epoll_wait` — the three calls that do all the work
- why every socket is set to non-blocking (`set_nonblocking`)
- how a new connection (on `server_fd`) is handled differently from an
  existing client sending data

## 4. Once comfortable, try extending it yourselves

- Broadcast a message to *all* connected clients instead of echoing to just one
- Track how many clients are connected and print it periodically
- Handle a client sending a partial message (e.g. split across two `recv()` calls)
- Add a simple text protocol (e.g. commands like `NAME:Arch` before chat messages)

These are the kinds of small extensions that mirror what a real problem
statement might ask for — good practice before Friday.
