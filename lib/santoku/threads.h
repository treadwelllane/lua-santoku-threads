#define _GNU_SOURCE

#ifndef TK_THREADS_H
#define TK_THREADS_H

#include <santoku/lua/utils.h>

#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <sched.h>
#if __has_include(<numa.h>)
#define HAVE_NUMA 1
#include <numa.h>
#include <numaif.h>
#else
#define numa_available(...) 0
#define numa_free(p, s) free(p)
#define numa_alloc_interleaved(s) malloc(s)
#endif

typedef struct tk_threadpool_s tk_threadpool_t;
typedef struct tk_thread_s tk_thread_t;

typedef void (*tk_thread_worker_t)(void *, int);

struct tk_threadpool_s {
  pthread_mutex_t mutex;
  pthread_cond_t cond_stage;
  pthread_cond_t cond_done;
  unsigned int n_threads;
  unsigned int n_threads_done;
  unsigned int signal_index;
  int current_stage;
  int stage;
  tk_thread_t *threads;
  tk_thread_worker_t worker;
  unsigned int sigid;
};

struct tk_thread_s {
  void *data;
  tk_threadpool_t *pool;
  pthread_t thread;
  pthread_mutex_t child_mutex;
  pthread_cond_t child_cond;
  int waiting_for_ack;
  unsigned int index;
  unsigned int sigid;
};

static inline void tk_threads_notify_parent (
  tk_thread_t *thread
) {
  pthread_mutex_lock(&thread->pool->mutex);
  thread->pool->signal_index = thread->index;
  pthread_cond_signal(&thread->pool->cond_done);
  pthread_mutex_unlock(&thread->pool->mutex);

  pthread_mutex_lock(&thread->child_mutex);
  thread->waiting_for_ack = 1;
  while (thread->waiting_for_ack)
    pthread_cond_wait(&thread->child_cond, &thread->child_mutex);
  pthread_mutex_unlock(&thread->child_mutex);
}

static inline void tk_threads_wait (
  tk_threadpool_t *pool
) {
  pthread_mutex_lock(&pool->mutex);
  while (pool->n_threads_done < pool->n_threads)
    pthread_cond_wait(&pool->cond_done, &pool->mutex);
  pthread_mutex_unlock(&pool->mutex);
}

static inline int tk_threads_signal (
  tk_threadpool_t *pool,
  int stage,
  unsigned int *child
) {
  pthread_mutex_lock(&pool->mutex);
  if (stage != pool->current_stage) {
    pool->current_stage = stage;
    pool->sigid++;
    pool->stage = stage;
    pool->n_threads_done = 0;
    pool->signal_index = UINT_MAX;
    pthread_cond_broadcast(&pool->cond_stage);
  }
  while (pool->signal_index == UINT_MAX && pool->n_threads_done < pool->n_threads)
    pthread_cond_wait(&pool->cond_done, &pool->mutex);
  if (child) {
    if (pool->signal_index != UINT_MAX) {
      *child = pool->signal_index;
      pool->signal_index = UINT_MAX;
      pthread_mutex_unlock(&pool->mutex);
      pthread_cond_broadcast(&pool->cond_stage);
      return 1;
    }
    pthread_mutex_unlock(&pool->mutex);
    pthread_cond_broadcast(&pool->cond_stage);
    return 0;
  }
  pthread_mutex_unlock(&pool->mutex);
  tk_threads_wait(pool);
  pthread_cond_broadcast(&pool->cond_stage);
  return 0;
}

// static inline int tk_threads_signal (
//   tk_threadpool_t *pool,
//   int stage,
//   unsigned int *child
// ) {
//   pthread_mutex_lock(&pool->mutex);
//   pool->sigid++;
//   pool->stage = stage;
//   pool->n_threads_done = 0;
//   pool->signal_index  = UINT_MAX;
//   pthread_cond_broadcast(&pool->cond_stage);
//   // wait for either a notify or all threads to arrive
//   while (pool->signal_index == UINT_MAX && pool->n_threads_done < pool->n_threads)
//     pthread_cond_wait(&pool->cond_done, &pool->mutex);
//   // child-mode?
//   if (child) {
//     if (pool->signal_index != UINT_MAX) {
//       *child = pool->signal_index;
//       pool->signal_index = UINT_MAX;
//       pthread_mutex_unlock(&pool->mutex);
//       pthread_cond_broadcast(&pool->cond_stage);
//       return 1;// woke on a notify
//     }
//     // all threads done without notify
//     pthread_mutex_unlock(&pool->mutex);
//     pthread_cond_broadcast(&pool->cond_stage);
//     return 0;
//   }
//   // normal (no child pointer): full barrier
//   pthread_mutex_unlock(&pool->mutex);
//   tk_threads_wait(pool);
//   pthread_cond_broadcast(&pool->cond_stage);
//   return 0;
// }

static inline void tk_threads_pin (
  unsigned int thread_index,
  unsigned int n_threads
) {
#if defined(HAVE_NUMA)
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  unsigned int n_nodes = (unsigned int) numa_max_node() + 1;
  unsigned int threads_per_node = n_threads / n_nodes;
  if (threads_per_node == 0) threads_per_node = 1;
  unsigned int node = thread_index / threads_per_node;
  if (node >= n_nodes) node = n_nodes - 1;
  struct bitmask *cpus = numa_allocate_cpumask();
  if (numa_node_to_cpus((int) node, cpus) == 0) {
    unsigned int count = 0;
    for (unsigned int i = 0; i < cpus->size; ++i) {
      if (numa_bitmask_isbitset(cpus, i)) {
        count ++;
      }
    }
    if (count > 0) {
      unsigned int local_index =
        (thread_index - node * threads_per_node) % count;
      unsigned int found = 0;
      for (unsigned int i = 0; i < cpus->size; ++i) {
        if (numa_bitmask_isbitset(cpus, i)) {
          if (found == local_index) {
            CPU_SET(i, &cpuset);
            break;
          }
          found ++;
        }
      }
    }
  }
  numa_free_cpumask(cpus);
  pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
#else
  (void) thread_index;
  (void) n_threads;
#endif
}

static inline void tk_threads_acknowledge_child (
  tk_threadpool_t *pool,
  unsigned int child_index
) {
  tk_thread_t *child = &pool->threads[child_index];
  pthread_mutex_lock(&child->child_mutex);
  child->waiting_for_ack = 0;
  pthread_cond_signal(&child->child_cond);
  pthread_mutex_unlock(&child->child_mutex);
}

static void *tk_thread_worker (void *arg)
{
  tk_thread_t *data = (tk_thread_t *) arg;
#if defined(HAVE_NUMA)
  if (numa_available() != -1 && numa_max_node() > 0)
    tk_threads_pin(data->index, data->pool->n_threads);
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

static inline tk_threadpool_t *tk_threads_create (
  lua_State *L,
  unsigned int n_threads,
  tk_thread_worker_t worker
) {
  tk_threadpool_t *pool = tk_malloc(L, sizeof(tk_threadpool_t));
  // TODO: check errors
  pthread_mutex_init(&pool->mutex, NULL);
  pthread_cond_init(&pool->cond_stage, NULL);
  pthread_cond_init(&pool->cond_done, NULL);
  pool->n_threads = n_threads,
  pool->n_threads_done = 0;
  pool->sigid = 0;
  pool->current_stage = INT_MIN;
  pool->stage = -2;
  pool->threads = tk_malloc(L, pool->n_threads * sizeof(tk_thread_t));
  pool->worker = worker;
  pool->signal_index  = UINT_MAX;
  for (unsigned int i = 0; i < pool->n_threads; i ++) {
    tk_thread_t *data = pool->threads + i;
    data->pool = pool;
    data->index = i;
    data->sigid = 0;
    pthread_mutex_init(&data->child_mutex, NULL);
    pthread_cond_init(&data->child_cond, NULL);
    data->waiting_for_ack = 0;
    if (pthread_create(&data->thread, NULL, tk_thread_worker, data) != 0)
      tk_error(L, "pthread_create", errno);
  }
  tk_threads_wait(pool);
  return pool;
}

static inline void tk_threads_destroy (
  tk_threadpool_t *pool
) {
  tk_threads_signal(pool, -1, NULL);
  for (unsigned int i = 0; i < pool->n_threads; i ++) {
    assert(pthread_join(pool->threads[i].thread, NULL) == 0);
    pthread_mutex_destroy(&pool->threads[i].child_mutex);
    pthread_cond_destroy(&pool->threads[i].child_cond);
  }
  pthread_mutex_destroy(&pool->mutex);
  pthread_cond_destroy(&pool->cond_stage);
  pthread_cond_destroy(&pool->cond_done);
  free(pool->threads);
  free(pool);
}

#endif
