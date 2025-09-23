# Now

- Add a lua_managed concept for automatic garbage collection of pool resources
    - e.g. tk_ivec_create(L or NULL, ...) and tk_iuset_create(L or NULL, ...)

# Next

- Use Futex

- Avoid pthreads, mutexes, etc entirely when running with threads <= 1
    - Lua coroutines? Manual setjmp?

# Consider

- Expose a Lua API like santoku-system, except threads instead of processes
    - Each thread get own lua state
    - Should work native and wasm/browser
