/*
 * parallel.c - Simple parallel for with persistent workers
 *
 * Threads are created once and reused. No per-call overhead.
 */

#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include "parallel.h"

#define MAX_WORKERS 8

static struct {
    pthread_t threads[MAX_WORKERS];
    int nthreads;
    
    parallel_fn fn;
    void *ctx;
    int count;
    int next_idx;
    int done_count;
    
    pthread_mutex_t lock;
    pthread_cond_t work_cond;
    pthread_cond_t done_cond;
    int shutdown;
    int initialized;
} pool;

static void *worker_func(void *arg) {
    (void)arg;
    
    pthread_mutex_lock(&pool.lock);
    for (;;) {
        while (pool.next_idx >= pool.count && !pool.shutdown)
            pthread_cond_wait(&pool.work_cond, &pool.lock);
        
        if (pool.shutdown) {
            pthread_mutex_unlock(&pool.lock);
            return NULL;
        }
        
        int idx = pool.next_idx++;
        if (idx < pool.count) {
            parallel_fn fn = pool.fn;
            void *ctx = pool.ctx;
            pthread_mutex_unlock(&pool.lock);
            
            fn(ctx, idx);
            
            pthread_mutex_lock(&pool.lock);
            if (++pool.done_count == pool.count)
                pthread_cond_signal(&pool.done_cond);
        }
    }
}

static void ensure_initialized(void) {
    if (pool.initialized) return;
    
    pthread_mutex_init(&pool.lock, NULL);
    pthread_cond_init(&pool.work_cond, NULL);
    pthread_cond_init(&pool.done_cond, NULL);
    
    pool.nthreads = sysconf(_SC_NPROCESSORS_ONLN) / 2;
    if (pool.nthreads < 1) pool.nthreads = 1;
    if (pool.nthreads > MAX_WORKERS) pool.nthreads = MAX_WORKERS;
    
    for (int i = 0; i < pool.nthreads; i++)
        pthread_create(&pool.threads[i], NULL, worker_func, NULL);
    
    pool.initialized = 1;
}

void parallel_for(int count, parallel_fn fn, void *ctx) {
    if (count <= 0) return;
    
    ensure_initialized();
    
    pthread_mutex_lock(&pool.lock);
    pool.fn = fn;
    pool.ctx = ctx;
    pool.count = count;
    pool.next_idx = 0;
    pool.done_count = 0;
    
    pthread_cond_broadcast(&pool.work_cond);
    
    while (pool.done_count < count)
        pthread_cond_wait(&pool.done_cond, &pool.lock);
    
    pthread_mutex_unlock(&pool.lock);
}

void parallel_cleanup(void) {
    if (!pool.initialized) return;
    
    pthread_mutex_lock(&pool.lock);
    pool.shutdown = 1;
    pthread_cond_broadcast(&pool.work_cond);
    pthread_mutex_unlock(&pool.lock);
    
    for (int i = 0; i < pool.nthreads; i++)
        pthread_join(pool.threads[i], NULL);
    
    pthread_mutex_destroy(&pool.lock);
    pthread_cond_destroy(&pool.work_cond);
    pthread_cond_destroy(&pool.done_cond);
    pool.initialized = 0;
}
