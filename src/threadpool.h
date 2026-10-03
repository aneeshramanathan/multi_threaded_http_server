/*
 * threadpool.h - fixed-size pool of pthread workers fed by a bounded
 * ring-buffer queue, synchronized with a mutex and condition variable.
 */
#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <stddef.h>

typedef void (*task_fn)(void *arg);
typedef struct thread_pool thread_pool;

thread_pool *threadpool_create(int nthreads, size_t queue_capacity);

/* Non-blocking: returns -1 if the queue is full or the pool is shutting down. */
int threadpool_submit(thread_pool *pool, task_fn fn, void *arg);

/* Stops accepting work, lets workers drain the queue, joins and frees. */
void threadpool_destroy(thread_pool *pool);

#endif
