/*
 * thread_pool.c - Generic worker thread pool
 *
 * A reusable thread pool for parallel batch processing.
 * Workers wait for work, process items in parallel, and signal completion.
 *
 * Usage:
 *   struct thread_pool pool;
 *   pool_create(&pool, 0);  // auto-detect thread count
 *   pool_process(&pool, my_work_fn, my_data, num_items);
 *   pool_destroy(&pool);
 */

#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <unistd.h>
#include <wlr/util/log.h>

#include "thread_pool.h"

#define MIN_POOL_SIZE 1
#define MAX_POOL_SIZE 16

/*
 * Determine optimal thread count based on CPU cores.
 * For CPU-bound work, using more threads than physical cores doesn't help.
 * Most systems have 2 threads per core (SMT), so nprocs/2 approximates
 * physical cores. Cap at 8 to avoid diminishing returns.
 */
static int get_optimal_threads(void) {
    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    
    int pool_size = (nprocs > 1) ? (nprocs / 2) : 1;
    if (pool_size < MIN_POOL_SIZE) pool_size = MIN_POOL_SIZE;
    if (pool_size > MAX_POOL_SIZE) pool_size = MAX_POOL_SIZE;

    return pool_size;
}

/*
 * Worker thread function.
 * Loops waiting for work, processes items, signals completion.
 */
static void *pool_worker(void *arg) {
    struct thread_pool *pool = arg;
    
    while (1) {
        pthread_mutex_lock(&pool->lock);
        
        /* Wait for work or shutdown */
        while (pool->next_item >= pool->total_items && !pool->shutdown) {
            pthread_cond_wait(&pool->work_available, &pool->lock);
        }
        
        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->lock);
            break;
        }
        
        /* Grab the next item to process */
        int item = pool->next_item++;
        pool_work_fn work_fn = pool->work_fn;
        void *user_data = pool->user_data;
        int total = pool->total_items;
        
        pthread_mutex_unlock(&pool->lock);
        
        /* Process item (outside lock) */
        if (item < total && work_fn) {
            work_fn(user_data, item);
            
            pthread_mutex_lock(&pool->lock);
            pool->completed++;
            if (pool->completed >= pool->total_items) {
                pthread_cond_signal(&pool->work_done);
            }
            pthread_mutex_unlock(&pool->lock);
        }
    }
    
    return NULL;
}

int pool_create(struct thread_pool *pool, int num_threads) {
    if (!pool) return -1;
    if (pool->initialized) return 0;  /* Already initialized */
    
    if (num_threads <= 0) {
        num_threads = get_optimal_threads();
    }
    if (num_threads > MAX_POOL_SIZE) {
        num_threads = MAX_POOL_SIZE;
    }
    
    pool->threads = calloc(num_threads, sizeof(pthread_t));
    if (!pool->threads) {
        wlr_log(WLR_ERROR, "Thread pool: failed to allocate threads");
        return -1;
    }
    
    pool->num_threads = num_threads;
    
    if (pthread_mutex_init(&pool->lock, NULL) != 0) {
        wlr_log(WLR_ERROR, "Thread pool: failed to init mutex");
        free(pool->threads);
        pool->threads = NULL;
        return -1;
    }
    
    if (pthread_cond_init(&pool->work_available, NULL) != 0) {
        wlr_log(WLR_ERROR, "Thread pool: failed to init work_available cond");
        pthread_mutex_destroy(&pool->lock);
        free(pool->threads);
        pool->threads = NULL;
        return -1;
    }
    
    if (pthread_cond_init(&pool->work_done, NULL) != 0) {
        wlr_log(WLR_ERROR, "Thread pool: failed to init work_done cond");
        pthread_cond_destroy(&pool->work_available);
        pthread_mutex_destroy(&pool->lock);
        free(pool->threads);
        pool->threads = NULL;
        return -1;
    }
    
    pool->shutdown = 0;
    pool->next_item = 0;
    pool->total_items = 0;
    pool->completed = 0;
    pool->work_fn = NULL;
    pool->user_data = NULL;
    
    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&pool->threads[i], NULL, pool_worker, pool) != 0) {
            wlr_log(WLR_ERROR, "Thread pool: failed to create worker %d", i);
            
            /* Signal shutdown to already-created threads */
            pthread_mutex_lock(&pool->lock);
            pool->shutdown = 1;
            pthread_cond_broadcast(&pool->work_available);
            pthread_mutex_unlock(&pool->lock);
            
            /* Join already-created threads */
            for (int j = 0; j < i; j++) {
                pthread_join(pool->threads[j], NULL);
            }
            
            /* Clean up all resources */
            pthread_cond_destroy(&pool->work_done);
            pthread_cond_destroy(&pool->work_available);
            pthread_mutex_destroy(&pool->lock);
            free(pool->threads);
            pool->threads = NULL;
            pool->num_threads = 0;
            
            return -1;
        }
    }
    
    pool->initialized = 1;
    wlr_log(WLR_INFO, "Thread pool initialized with %d workers", num_threads);
    
    return 0;
}

void pool_process(struct thread_pool *pool, pool_work_fn work_fn,
                  void *user_data, int num_items) {
    if (!pool || !pool->initialized || !work_fn || num_items <= 0) {
        return;
    }
    
    pthread_mutex_lock(&pool->lock);
    
    pool->work_fn = work_fn;
    pool->user_data = user_data;
    pool->next_item = 0;
    pool->total_items = num_items;
    pool->completed = 0;
    
    /* Wake all workers */
    pthread_cond_broadcast(&pool->work_available);
    
    /* Wait for completion */
    while (pool->completed < num_items) {
        pthread_cond_wait(&pool->work_done, &pool->lock);
    }
    
    /* Reset for next batch */
    pool->work_fn = NULL;
    pool->user_data = NULL;
    
    pthread_mutex_unlock(&pool->lock);
}

void pool_destroy(struct thread_pool *pool) {
    if (!pool || !pool->initialized) return;
    
    pthread_mutex_lock(&pool->lock);
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->work_available);
    pthread_mutex_unlock(&pool->lock);
    
    for (int i = 0; i < pool->num_threads; i++) {
        pthread_join(pool->threads[i], NULL);
    }
    
    free(pool->threads);
    pool->threads = NULL;
    
    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->work_available);
    pthread_cond_destroy(&pool->work_done);
    
    pool->initialized = 0;
    wlr_log(WLR_INFO, "Thread pool destroyed");
}

int pool_get_num_threads(struct thread_pool *pool) {
    if (!pool || !pool->initialized) return 0;
    return pool->num_threads;
}
