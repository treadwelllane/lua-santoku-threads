# Now

- Use Futex

- Avoid pthreads, mutexes, etc entirely when running with threads <= 1
    - Lua coroutines? Manual setjmp?

# Next

- Expose a Lua API like santoku-system, except threads instead of processes
    - Each thread get own lua state
    - Should work native and wasm/browser
