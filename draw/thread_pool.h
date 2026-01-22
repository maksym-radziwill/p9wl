/*
 * thread_pool.h - Generic worker thread pool
 *
 * A reusable thread pool for parallel batch processing.
 * Workers wait for work, process items in parallel, and signal completion.
 *
 * USAGE EXAMPLE:
 *
 *   // Define work context
 *   struct my_work {
 *       int *array;
 *       int multiplier;
 *   };
 *
 *   // Work function processes one item
 *   void process_item(void *user_data, int index) {
 *       struct my_work *w = user_data;
 *       w->array[index] *= w->multiplier;
 *   }
 *
 *   // Use the pool
 *   struct thread_pool pool;
 *   pool_create(&pool, 0);  // auto-detect thread count
 *   
 *   struct my_work work = { .array = data, .multiplier = 2 };
 *   pool_process(&pool, process_item, &work, array_size);
 *   
 *   pool_destroy(&pool);
 *
 * CURRENT USES:
 *   - scroll.c: Parallel FFT-based scroll detection across screen regions
 *
 * POTENTIAL FUTURE USES:
 *   - Parallel tile compression (requires refactoring send.c batching)
 *   - Parallel tile change detection
 *   - Image processing/filtering
 */

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <pthread.h>
#include <stdint.h>

/*
 * Work function signature.
 * Called for each work item with:
 *   - user_data: opaque pointer passed to pool_process()
 *   - item_index: index of the item to process (0 to num_items-1)
 */
typedef void (*pool_work_fn)(void *user_data, int item_index);

/*
 * Thread pool structure.
 * Allocate with pool_create(), free with pool_destroy().
 */
struct thread_pool {
    pthread_t *threads;
    int num_threads;
    pthread_mutex_t lock;
    pthread_cond_t work_available;
    pthread_cond_t work_done;
    
    /* Current work batch */
    void *user_data;
    pool_work_fn work_fn;
    int next_item;
    int total_items;
    int completed;
    
    int shutdown;
    int initialized;
};

/*
 * Create a thread pool with the specified number of worker threads.
 * If num_threads <= 0, automatically determines based on CPU count.
 * Returns 0 on success, -1 on failure.
 */
int pool_create(struct thread_pool *pool, int num_threads);

/*
 * Process a batch of work items in parallel.
 * Blocks until all items are processed.
 *
 * work_fn is called once for each item index from 0 to num_items-1.
 * user_data is passed to each work_fn invocation.
 */
void pool_process(struct thread_pool *pool, pool_work_fn work_fn, 
                  void *user_data, int num_items);

/*
 * Destroy the thread pool and free resources.
 * Waits for all workers to finish before returning.
 */
void pool_destroy(struct thread_pool *pool);

/*
 * Get the number of worker threads in the pool.
 */
int pool_get_num_threads(struct thread_pool *pool);

#endif /* THREAD_POOL_H */
