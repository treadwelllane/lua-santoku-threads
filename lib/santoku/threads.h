#ifndef TK_THREADS_H
#define TK_THREADS_H

#include <santoku/lua/utils.h>

#include <unistd.h>
#include <assert.h>

// Detect pthread support
// Users can define TK_NO_PTHREAD to disable pthread even if header exists
// (useful when pthread.h is available but library is not linked)
#if !defined(TK_NO_PTHREAD)
  #if defined(__has_include)
    #if __has_include(<pthread.h>)
      #define TK_HAS_PTHREAD 1
    #endif
  #elif defined(__unix__) || defined(__APPLE__) || defined(__linux__)
    // Assume pthread.h exists on Unix-like systems if __has_include unavailable
    #define TK_HAS_PTHREAD 1
  #endif
#endif

#ifdef TK_HAS_PTHREAD
#include <pthread.h>
#include <sched.h>
#endif

// Detect pthread affinity support (requires pthread)
// Note: Android is excluded - pthread_setaffinity_np is unreliable/missing
#if defined(TK_HAS_PTHREAD) && !defined(__ANDROID__) && (defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__))
#define TK_HAS_PTHREAD_AFFINITY 1
#endif

#define TK_THREADPOOL_MT "tk_threadpool_t"
#define TK_THREADPOOL_EPH "tk_threadpool_eph"

// Lua 5.1 compatibility
#ifndef LUA_OK
#define LUA_OK 0
#endif

#if LUA_VERSION_NUM < 502
#define lua_resume_compat(L, from, narg) lua_resume(L, narg)
#else
#define lua_resume_compat(L, from, narg) lua_resume(L, from, narg)
#endif

// Stage values:
//   -2: Initial "never started" state (ensures first signal triggers starting)
//   -1: Shutdown signal (threads exit)
//   >= 0: Work stages (passed to worker function)

typedef struct tk_threadpool_s tk_threadpool_t;
typedef struct tk_thread_s tk_thread_t;

typedef void (*tk_thread_worker_t)(void *, int);

struct tk_threadpool_s {
  lua_State *L;
  bool use_coroutines;
#ifdef TK_HAS_PTHREAD
  pthread_mutex_t mutex;
  pthread_cond_t cond_stage;
  pthread_cond_t cond_done;
#endif
  unsigned int n_threads;
  unsigned int n_threads_done;
  int stage;
  tk_thread_t *threads;
  tk_thread_worker_t worker;
  unsigned int sigid;

  int notify_enabled;
  int notified_child;

  unsigned int n_threads_created;
  bool is_userdata;
  bool destroyed;
};

struct tk_thread_s {
  void *data;
  tk_threadpool_t *pool;
  union {
#ifdef TK_HAS_PTHREAD
    struct {
      pthread_t thread;
      pthread_mutex_t child_mutex;
      pthread_cond_t child_cond;
      int waiting_for_ack;
    } pt;
#endif
    struct {
      lua_State *coro;
      int coro_ref;
    } co;
  };
  unsigned int index;
  unsigned int sigid;
};

static inline void tk_threads_notify_parent (
  tk_thread_t *thread
) {
  if (thread->pool->use_coroutines) {
    if (!thread->pool->notify_enabled)
      return;
    thread->pool->notified_child = (int) thread->index;
    lua_yield(thread->co.coro, 0);
    return;
  }

#ifdef TK_HAS_PTHREAD
  pthread_mutex_lock(&thread->pool->mutex);
  int enabled = thread->pool->notify_enabled;
  pthread_mutex_unlock(&thread->pool->mutex);
  if (!enabled)
    return;
  pthread_mutex_lock(&thread->pt.child_mutex);
  thread->pt.waiting_for_ack = 1;
  pthread_mutex_unlock(&thread->pt.child_mutex);
  pthread_mutex_lock(&thread->pool->mutex);
  if (!thread->pool->notify_enabled) {
    pthread_mutex_unlock(&thread->pool->mutex);
    pthread_mutex_lock(&thread->pt.child_mutex);
    thread->pt.waiting_for_ack = 0;
    pthread_mutex_unlock(&thread->pt.child_mutex);
    return;
  }
  while (thread->pool->notified_child != -1)
    pthread_cond_wait(&thread->pool->cond_done, &thread->pool->mutex);
  thread->pool->notified_child = (int) thread->index;
  pthread_cond_signal(&thread->pool->cond_done);
  pthread_mutex_unlock(&thread->pool->mutex);
  pthread_mutex_lock(&thread->pt.child_mutex);
  while (thread->pt.waiting_for_ack)
    pthread_cond_wait(&thread->pt.child_cond, &thread->pt.child_mutex);
  pthread_mutex_unlock(&thread->pt.child_mutex);
#endif
}

static inline void tk_threads_wait (
  tk_threadpool_t *pool
) {
  if (pool->use_coroutines)
    return;

#ifdef TK_HAS_PTHREAD
  pthread_mutex_lock(&pool->mutex);
  while (pool->n_threads_done < pool->n_threads)
    pthread_cond_wait(&pool->cond_done, &pool->mutex);
  pthread_mutex_unlock(&pool->mutex);
#endif
}

static inline int tk_threads_signal (
  tk_threadpool_t *pool,
  int stage,
  unsigned int *child
) {
  if (pool->use_coroutines) {
    assert(pool->L != NULL);
    assert(pool->n_threads == 1);
    tk_thread_t *t = &pool->threads[0];

    int starting = (stage != pool->stage) || (pool->n_threads_done == pool->n_threads) || (pool->stage < 0);
    if (starting) {
      pool->sigid++;
      pool->stage = stage;
      pool->n_threads_done = 0;
      pool->notified_child = -1;
    }

    pool->notify_enabled = (child != NULL);

    if (!child) {
      while (pool->n_threads_done == 0) {
        int status = lua_resume_compat(t->co.coro, pool->L, 0);
        if (status != LUA_OK && status != LUA_YIELD) {
          lua_xmove(t->co.coro, pool->L, 1);
          lua_error(pool->L);
        }
      }
      pool->notify_enabled = 0;
      return 0;
    }

    while (1) {
      if (pool->notified_child != -1) {
        *child = (unsigned int) pool->notified_child;
        return 1;
      }
      if (pool->n_threads_done == pool->n_threads) {
        pool->notify_enabled = 0;
        return 0;
      }
      int status = lua_resume_compat(t->co.coro, pool->L, 0);
      if (status != LUA_OK && status != LUA_YIELD) {
        lua_xmove(t->co.coro, pool->L, 1);
        lua_error(pool->L);
      }
    }
  }

#ifdef TK_HAS_PTHREAD
  pthread_mutex_lock(&pool->mutex);
  int starting =
    (stage != pool->stage) || (pool->n_threads_done == pool->n_threads) || (pool->stage < 0);
  if (starting) {
    pool->sigid ++;
    pool->stage = stage;
    pool->n_threads_done = 0;
    pool->notified_child = -1;
    pthread_cond_broadcast(&pool->cond_stage);
  }
  pool->notify_enabled = (child != NULL);
  if (!child) {
    while (pool->n_threads_done < pool->n_threads)
      pthread_cond_wait(&pool->cond_done, &pool->mutex);
    pool->notify_enabled = 0;
    pthread_mutex_unlock(&pool->mutex);
    return 0;
  }
  for (;;) {
    if (pool->notified_child != -1) {
      *child = (unsigned int) pool->notified_child;
      pthread_mutex_unlock(&pool->mutex);
      return 1;
    }
    if (pool->n_threads_done == pool->n_threads) {
      pool->notify_enabled = 0;
      pthread_mutex_unlock(&pool->mutex);
      return 0;
    }
    pthread_cond_wait(&pool->cond_done, &pool->mutex);
  }
#else
  // Without pthreads, we should never reach here
  (void) pool;
  (void) stage;
  (void) child;
  return 0;
#endif
}

static inline void tk_threads_pin (
  unsigned int thread_index
) {
#ifdef TK_HAS_PTHREAD_AFFINITY
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  long n_cpus = sysconf(_SC_NPROCESSORS_ONLN);
  if (n_cpus > 0) {
    unsigned int cpu = thread_index % (unsigned int) n_cpus;
    CPU_SET(cpu, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
  }
#else
  (void) thread_index;
#endif
}

static inline void tk_threads_acknowledge_child (
  tk_threadpool_t *pool,
  unsigned int child_index
) {
  assert(child_index < pool->n_threads);

  if (pool->use_coroutines) {
    assert(pool->L != NULL);
    if (pool->notified_child == (int) child_index) {
      pool->notified_child = -1;
      tk_thread_t *child = &pool->threads[child_index];
      int status = lua_resume_compat(child->co.coro, pool->L, 0);
      if (status != LUA_OK && status != LUA_YIELD) {
        lua_xmove(child->co.coro, pool->L, 1);
        lua_error(pool->L);
      }
    }
    return;
  }

#ifdef TK_HAS_PTHREAD
  pthread_mutex_lock(&pool->mutex);
  if (pool->notified_child == (int) child_index) {
    pool->notified_child = -1;
    pthread_cond_broadcast(&pool->cond_done);
  }
  pthread_mutex_unlock(&pool->mutex);

  tk_thread_t *child = &pool->threads[child_index];
  pthread_mutex_lock(&child->pt.child_mutex);
  child->pt.waiting_for_ack = 0;
  pthread_cond_signal(&child->pt.child_cond);
  pthread_mutex_unlock(&child->pt.child_mutex);
#endif
}

#ifdef TK_HAS_PTHREAD
static void *tk_thread_worker (void *arg)
{
  tk_thread_t *data = (tk_thread_t *) arg;
#ifdef TK_HAS_PTHREAD_AFFINITY
  tk_threads_pin(data->index);
#endif
  pthread_mutex_lock(&data->pool->mutex);
  data->pool->n_threads_done ++;
  if (data->pool->n_threads_done == data->pool->n_threads)
    pthread_cond_signal(&data->pool->cond_done);
  pthread_mutex_unlock(&data->pool->mutex);
  while (1) {
    pthread_mutex_lock(&data->pool->mutex);
    while (data->sigid == data->pool->sigid)
      pthread_cond_wait(&data->pool->cond_stage, &data->pool->mutex);
    data->sigid = data->pool->sigid;
    int stage = data->pool->stage;
    pthread_mutex_unlock(&data->pool->mutex);
    if (stage >= 0)
      data->pool->worker(data->data, stage);
    pthread_mutex_lock(&data->pool->mutex);
    data->pool->n_threads_done ++;
    if (data->pool->n_threads_done == data->pool->n_threads)
      pthread_cond_signal(&data->pool->cond_done);
    pthread_mutex_unlock(&data->pool->mutex);
    if (stage < 0)
      break;
  }
  return NULL;
}
#endif

static inline void tk_thread_range (
  unsigned int thread_index,
  unsigned int n_threads,
  uint64_t n_items,
  uint64_t *first,
  uint64_t *last
) {
  uint64_t base = n_items / (uint64_t) n_threads;
  uint64_t extra = n_items % (uint64_t) n_threads;
  if ((uint64_t)thread_index < extra) {
    *first = thread_index * (base + 1);
    *last = *first + base;
  } else {
    *first = thread_index * base + extra;
    *last = *first + base - 1;
  }
  if (*first > *last)
    *last = *first - 1;
}

static inline unsigned int tk_threads_getn (
  lua_State *L, int i, char *name, char *field
) {
  long ts;
  unsigned int n_threads;
  if (field != NULL)
    n_threads = tk_lua_foptunsigned(L, i, name, field, 0);
  else
    n_threads = tk_lua_optunsigned(L, i, name, 0);
  if (n_threads)
    return n_threads;
  ts = sysconf(_SC_NPROCESSORS_ONLN) - 1;
  if (ts <= 0)
    return (unsigned int) tk_lua_verror(L, 3, name, "sysconf", errno);
  lua_pushinteger(L, ts);
  n_threads = tk_lua_checkunsigned(L, -1, "sysconf");
  lua_pop(L, 1);
  return n_threads;
}

static inline void tk_threads_destroy_internal (tk_threadpool_t *pool);

static int tk_thread_coroutine_wrapper (lua_State *L)
{
  tk_thread_t *thread = (tk_thread_t *) lua_touserdata(L, lua_upvalueindex(1));
  thread->pool->n_threads_done = 1;
  lua_yield(L, 0);
  while (1) {
    while (thread->sigid == thread->pool->sigid)
      lua_yield(L, 0);
    thread->sigid = thread->pool->sigid;
    int stage = thread->pool->stage;
    if (stage < 0)
      break;
    thread->pool->worker(thread->data, stage);
    thread->pool->n_threads_done = 1;
    lua_yield(L, 0);
  }
  return 0;
}

static inline int tk_threadpool_gc (lua_State *L)
{
  tk_threadpool_t *pool = (tk_threadpool_t *) luaL_checkudata(L, 1, TK_THREADPOOL_MT);
  if (pool->is_userdata)
    tk_threads_destroy_internal(pool);
  return 0;
}

static inline tk_threadpool_t *tk_threads_create (
  lua_State *L,
  unsigned int n_threads,
  tk_thread_worker_t worker
) {
  tk_threadpool_t *pool;
  if (L) {
    pool = tk_lua_newuserdata(L, tk_threadpool_t, TK_THREADPOOL_MT, NULL, tk_threadpool_gc);
    pool->is_userdata = true;
  } else {
    pool = malloc(sizeof(tk_threadpool_t));
    if (!pool)
      return NULL;
    memset(pool, 0, sizeof(tk_threadpool_t));
    pool->is_userdata = false;
  }

  pool->L = L;
  pool->use_coroutines = (n_threads == 1 && L != NULL);
  pool->worker = worker;
  pool->n_threads = n_threads;
  pool->n_threads_done = 0;
  pool->n_threads_created = 0;
  pool->sigid = 0;
  pool->stage = -2;
  pool->notify_enabled = 0;
  pool->notified_child = -1;
  pool->destroyed = false;
  pool->threads = tk_malloc(L, pool->n_threads * sizeof(tk_thread_t));

  if (pool->use_coroutines) {
    assert(L != NULL && n_threads == 1);
    tk_thread_t *t = pool->threads;
    memset(t, 0, sizeof(tk_thread_t));
    t->pool = pool;
    t->index = 0;
    t->sigid = 0;
    lua_State *coro = lua_newthread(L);
    t->co.coro = coro;
    t->co.coro_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pushlightuserdata(coro, t);
    lua_pushcclosure(coro, tk_thread_coroutine_wrapper, 1);
    int status = lua_resume_compat(coro, L, 0);
    if (status != LUA_OK && status != LUA_YIELD) {
      luaL_unref(L, LUA_REGISTRYINDEX, t->co.coro_ref);
      free(pool->threads);
      lua_error(L);
      return NULL;
    }
    assert(pool->n_threads_done == 1);
    return pool;
  }

#ifdef TK_HAS_PTHREAD
  pthread_mutex_init(&pool->mutex, NULL);
  pthread_cond_init(&pool->cond_stage, NULL);
  pthread_cond_init(&pool->cond_done, NULL);

  for (unsigned int i = 0; i < pool->n_threads; i ++) {
    tk_thread_t *data = pool->threads + i;
    memset(data, 0, sizeof(tk_thread_t));
    data->pool = pool;
    data->index = i;
    data->sigid = 0;
    pthread_mutex_init(&data->pt.child_mutex, NULL);
    pthread_cond_init(&data->pt.child_cond, NULL);
    data->pt.waiting_for_ack = 0;
    if (pthread_create(&data->pt.thread, NULL, tk_thread_worker, data) != 0) {
      if (L)
        tk_error(L, "pthread_create", errno);
      else
        return NULL;
    }
    pool->n_threads_created++;
  }
  tk_threads_wait(pool);
  return pool;
#else
  // Without pthreads, multi-threading is not supported
  if (L)
    return (tk_threadpool_t *) tk_lua_error(L, "pthread support not available");
  free(pool->threads);
  free(pool);
  return NULL;
#endif
}

static inline void tk_threads_destroy_internal (tk_threadpool_t *pool)
{
  if (!pool || pool->destroyed)
    return;

  if (pool->use_coroutines) {
    assert(pool->L != NULL);
    pool->stage = -1;
    pool->sigid++;
    pool->n_threads_done = 0;
    pool->notify_enabled = 0;
    pool->notified_child = -1;

    tk_thread_t *t = &pool->threads[0];
    int status = lua_resume_compat(t->co.coro, pool->L, 0);
    (void) status;

    for (unsigned int i = 0; i < pool->n_threads; i++) {
      luaL_unref(pool->L, LUA_REGISTRYINDEX, pool->threads[i].co.coro_ref);
    }
    if (pool->threads)
      free(pool->threads);
    if (!pool->is_userdata)
      free(pool);
    else
      pool->destroyed = true;
    return;
  }

#ifdef TK_HAS_PTHREAD
  if (pool->n_threads_created > 0) {
    tk_threads_signal(pool, -1, NULL);
    for (unsigned int i = 0; i < pool->n_threads_created; i ++) {
      pthread_join(pool->threads[i].pt.thread, NULL);
      pthread_mutex_destroy(&pool->threads[i].pt.child_mutex);
      pthread_cond_destroy(&pool->threads[i].pt.child_cond);
    }
  }
  pthread_mutex_destroy(&pool->mutex);
  pthread_cond_destroy(&pool->cond_stage);
  pthread_cond_destroy(&pool->cond_done);
  if (pool->threads)
    free(pool->threads);
  if (!pool->is_userdata)
    free(pool);
  else
    pool->destroyed = true;
#endif
}

static inline void tk_threads_destroy (
  tk_threadpool_t *pool
) {
  tk_threads_destroy_internal(pool);
}

#endif
