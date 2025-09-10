# Santoku Threads

Low-level threading utilities for Lua providing efficient thread pool management with NUMA-aware CPU pinning and inter-thread communication.

## Overview

Santoku Threads is a C header library that provides thread pool functionality for Lua applications. It offers:

- Efficient thread pool management with pthreads
- NUMA-aware CPU pinning for optimal performance
- Inter-thread signaling and synchronization
- Child thread notification system
- Work distribution utilities

## C API Reference

### Data Structures

| Type | Description |
|------|-------------|
| `tk_threadpool_t` | Thread pool manager structure |
| `tk_thread_t` | Individual thread data structure |
| `tk_thread_worker_t` | Worker function type: `void (*)(void *data, int stage)` |

### Thread Pool Management

| Function | Arguments | Returns | Description |
|----------|-----------|---------|-------------|
| `tk_threads_create` | `L, n_threads, worker` | `tk_threadpool_t*` | Creates thread pool with worker function |
| `tk_threads_destroy` | `pool` | `-` | Destroys thread pool and joins all threads |
| `tk_threads_wait` | `pool` | `-` | Waits for all threads to complete current stage |
| `tk_threads_signal` | `pool, stage, &child` | `int` | Signals threads to start stage, optionally with child notifications |

### Thread Communication

| Function | Arguments | Returns | Description |
|----------|-----------|---------|-------------|
| `tk_threads_notify_parent` | `thread` | `-` | Child thread notifies parent and waits for acknowledgment |
| `tk_threads_acknowledge_child` | `pool, child_index` | `-` | Parent acknowledges child notification |

### Work Distribution

| Function | Arguments | Returns | Description |
|----------|-----------|---------|-------------|
| `tk_thread_range` | `thread_idx, n_threads, n_items, &first, &last` | `-` | Calculates work range for thread |
| `tk_threads_getn` | `L, idx, name, field` | `unsigned int` | Gets thread count from Lua or system |

### CPU Affinity

| Function | Arguments | Returns | Description |
|----------|-----------|---------|-------------|
| `tk_threads_pin` | `thread_index, n_threads` | `-` | Pins thread to CPU core (NUMA-aware) |

## License

Copyright 2025 Matthew Brooks

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.