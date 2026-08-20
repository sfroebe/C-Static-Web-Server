# C HTTP Server

A lightweight HTTP/1.1 static file server built from scratch in C using POSIX sockets and Linux system calls. The project was developed incrementally to understand how production web servers handle networking, concurrency, file I/O, security, and performance.

The implementation evolved from a minimal blocking TCP server into a concurrent pthread-based server, followed by an experimental `epoll` event-driven implementation. Performance was measured throughout the project against NGINX and Apache HTTP Server using ApacheBench.

## C HTTP Server

The project began with the fundamental TCP server lifecycle:

```text
socket()
   ↓
bind()
   ↓
listen()
   ↓
accept()
   ↓
recv()
   ↓
send()
   ↓
close()
```

The initial server returned a hard-coded HTML response. It was then expanded into a functional static file server with:

* HTTP request parsing and routing
* Static file serving from `./public`
* MIME type detection
* `400`, `403`, `404`, and `405` responses
* Path traversal protection
* Structured request logging
* Server statistics
* Linux `sendfile()` for efficient file transmission

This progression provided a foundation for understanding how HTTP works on top of TCP and how filesystem operations interact with network I/O.

## pthread Concurrency

The original server handled one client at a time:

```text
accept()
   ↓
handle client
   ↓
send response
   ↓
accept next client
```

I implemented POSIX pthread concurrency so multiple clients could be processed simultaneously:

```text
                 ┌── Thread → Client 1
                 │
accept() ────────┼── Thread → Client 2
                 │
                 └── Thread → Client 3
```

Shared server statistics required synchronization between threads.

This significantly improved throughput under concurrent workloads and became the primary implementation used for benchmarking.

## epoll Experiment

After implementing pthreads, I experimented with replacing the thread-per-connection architecture with Linux `epoll`:

```text
epoll_wait()
     ↓
ready connections
     ↓
process requests
     ↓
return to event loop
```

The goal was to learn how event-driven servers handle many connections without creating a thread for each client.

However, the initial `epoll` implementation performed substantially worse than the pthread version under my benchmark workload. This demonstrated that **using a theoretically scalable architecture does not automatically result in better performance**; implementation details and workload characteristics matter.

I therefore rolled back the main implementation to the better-performing pthread architecture while retaining the epoll work as an experimental branch.

## Benchmarks & Rollback

I automated benchmarking with **ApacheBench (`ab`) and a Bash script** to eliminate repetitive manual setup and make performance comparisons quick and reproducible. The script automatically runs the benchmark against the custom server, NGINX, and Apache, saves individual results, and generates a summary.

| Server              | Requests/sec | Time/request | Failed |
| ------------------- | -----------: | -----------: | -----: |
| **NGINX**           | **6,852.13** | **1.459 ms** |      0 |
| **Apache**          | **4,377.40** | **2.284 ms** |      0 |
| **Custom C Server** | **2,256.63** | **4.431 ms** |      0 |

The custom server achieved approximately **33% of NGINX's throughput** and **52% of Apache's throughput** in this benchmark, with zero failed requests.

The benchmarks were particularly useful for evaluating architectural changes rather than assuming an optimization would improve performance. The `epoll` experiment is an example: despite being commonly used in high-performance servers, my implementation performed worse, leading me to retain pthreads as the primary architecture.

## What I Learned

* How TCP sockets and HTTP work together
* POSIX socket programming and Linux system calls
* Static file serving and HTTP response construction
* Request parsing and input validation
* Path traversal security
* `sendfile()` and kernel-level I/O
* Concurrent programming with pthreads
* Synchronization of shared state
* Event-driven I/O with `epoll`
* Benchmarking throughput and latency with ApacheBench
* Automating repetitive development and benchmarking tasks with Bash
* Using measured performance to evaluate architectural trade-offs

The main takeaway was that **performance engineering is iterative**: architecture, implementation, workload, and measurement all matter.
