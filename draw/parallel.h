/*
 * parallel.h - Simple parallel for loop with persistent workers
 *
 * Provides a lightweight thread pool for parallel iteration over
 * work items. Worker threads are created lazily on first use and
 * persist until parallel_cleanup() is called.
 *
 * Architecture Overview:
 *
 *   The pool uses a work-stealing model where workers compete for
 *   indices from a shared counter protected by a mutex:
 *
 *     Caller Thread              Worker Threads
 *     ─────────────              ──────────────
 *     parallel_for() ──┐         ┌─── worker 0
 *       set fn, ctx    │    ┌────┼─── worker 1
 *       set count      ├────┤    ├─── worker 2
 *       broadcast ─────┤    │    └─── worker 3
 *       wait done ◄────┘    │         ...
 *                           └─── workers claim next_idx under lock
 *
 *   Each worker:
 *     1. Waits on work_cond for work
 *     2. Claims next_idx under pool.lock (increment + read)
 *     3. Calls fn(ctx, idx) outside the lock
 *     4. Increments done_count and signals if complete
 *     5. Loops back to wait for more work
 *
 * Thread Count:
 *
 *   Workers = min(MAX_WORKERS, sysconf(_SC_NPROCESSORS_ONLN) / 2)
 *
 *   Using half the CPU count leaves headroom for the main thread,
 *   send thread, and drain thread. MAX_WORKERS (8) caps the pool
 *   to avoid excessive context switching on high-core systems.
 *
 * Lazy Initialization:
 *
 *   The thread pool is created on the first call to parallel_for():
 *
 *     1. Initialize mutex, condition variables
 *     2. Determine thread count from CPU cores
 *     3. Create worker threads (all start waiting)
 *     4. Set initialized flag
 *
 *   Subsequent calls reuse the existing pool with no allocation.
 *
 * Synchronization:
 *
 *   pool.lock (mutex) protects:
 *     - fn, ctx (work function and context)
 *     - count (total work items)
 *     - next_idx (next unclaimed index)
 *     - done_count (completed items)
 *     - shutdown flag
 *
 *   pool.work_cond:
 *     - Workers wait here when idle
 *     - Broadcast when new work arrives
 *     - Broadcast on shutdown
 *
 *   pool.done_cond:
 *     - Caller waits here for completion
 *     - Signaled when done_count == count
 *
 * Serialization:
 *
 *   Only one parallel_for() can execute at a time. If multiple
 *   threads call parallel_for() concurrently, they are serialized
 *   by pool.lock. This is intentional - the pool is designed for
 *   a single producer (send thread) with parallel consumers.
 *
 * Work Function Requirements:
 *
 *   The work function fn(ctx, idx) must be:
 *     - Thread-safe (may run concurrently for different indices)
 *     - Non-blocking (workers should not wait on each other)
 *     - Deterministic (same idx should produce same result)
 *
 *   The function should access ctx in a thread-safe manner:
 *     - Read-only access is always safe
 *     - Per-index writes to separate locations are safe
 *     - Shared writes require external synchronization
 *
 * Performance Characteristics:
 *
 *   - Zero allocation per call (pool persists)
 *   - Minimal lock contention (workers unlock before fn())
 *   - Good load balancing (work-stealing via shared counter)
 *   - Low latency (workers stay warm, no thread creation)
 *
 *   Optimal for:
 *     - Many small work items (tiles)
 *     - Uniform work size
 *     - CPU-bound tasks
 *
 *   Less optimal for:
 *     - Very few items (< thread count)
 *     - I/O-bound tasks
 *     - Highly variable work sizes
 *
 * Usage Example:
 *
 *   struct compress_ctx {
 *       struct tile_work *tiles;
 *       struct tile_result *results;
 *   };
 *
 *   void compress_one(void *ctx, int idx) {
 *       struct compress_ctx *c = ctx;
 *       // Compress tile at index idx
 *       // Write result to c->results[idx]
 *   }
 *
 *   void compress_all(struct tile_work *tiles,
 *                     struct tile_result *results,
 *                     int count) {
 *       struct compress_ctx ctx = { tiles, results };
 *       parallel_for(count, compress_one, &ctx);
 *       // All results are now populated
 *   }
 */

#ifndef PARALLEL_H
#define PARALLEL_H

/*
 * Maximum number of worker threads.
 *
 * Caps the pool size regardless of CPU count. 8 workers is sufficient
 * for typical tile compression workloads (hundreds of tiles).
 */
#define MAX_WORKERS 8

/*
 * Function signature for parallel work items.
 *
 * ctx: user-provided context pointer (passed to parallel_for)
 * idx: index of the work item (0 to count-1)
 *
 * The function is called exactly once for each index, but the order
 * of calls is undefined and calls may overlap in time across threads.
 *
 * Thread safety requirements:
 *   - Must not modify ctx in ways visible to other indices
 *   - Must not depend on execution order of other indices
 *   - May write to ctx[idx] if ctx is an array of per-index data
 */
typedef void (*parallel_fn)(void *ctx, int idx);

/*
 * Execute a function in parallel for indices 0 to count-1.
 *
 * Distributes work across the thread pool. Workers claim indices
 * from a shared counter under mutex protection. Each index is
 * processed exactly once. The call blocks until all work items
 * complete.
 *
 * On first call, initializes the thread pool (lazy initialization).
 * Thread pool persists across calls for efficiency.
 *
 * count: number of iterations (work items)
 * fn:    function to call for each index (must be thread-safe)
 * ctx:   context pointer passed to each fn invocation
 *
 * Behavior:
 *   - If count <= 0, returns immediately without calling fn
 *   - If count == 1, still uses thread pool (no single-thread optimization)
 *   - Blocks until all indices are processed
 *
 * Thread-safety:
 *   - Multiple threads can call parallel_for(), but calls are serialized
 *   - Only one parallel_for() executes at a time (others block)
 *   - This is by design for the send thread use case
 *
 * Performance:
 *   - First call incurs thread creation overhead
 *   - Subsequent calls have minimal overhead (~microseconds)
 *   - Work distribution is dynamic (good load balancing)
 */
void parallel_for(int count, parallel_fn fn, void *ctx);

/*
 * Shutdown the thread pool and release resources.
 *
 * Performs clean shutdown:
 *   1. Sets shutdown flag
 *   2. Broadcasts to wake all workers
 *   3. Joins all worker threads
 *   4. Destroys mutex and condition variables
 *   5. Clears initialized flag
 *
 * Any work in progress completes before threads exit.
 * After this call, the next parallel_for() will reinitialize the pool.
 *
 * Safe to call even if pool was never initialized (no-op).
 * Safe to call multiple times (subsequent calls are no-ops).
 *
 * Call at program shutdown for clean resource release.
 */
void parallel_cleanup(void);

#endif /* PARALLEL_H */
