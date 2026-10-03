/* threadpool.c - mutex/condition-variable work queue with pthread workers. */
#include "threadpool.h"

#include <pthread.h>
#include <stdlib.h>

typedef struct {
    task_fn fn;
    void *arg;
} task;

struct thread_pool {
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    task *queue;
    size_t capacity, head, count;
    int shutting_down;
    int nthreads;
    pthread_t *threads;
};

static void *worker_main(void *arg) {
    thread_pool *pool = arg;
    for (;;) {
        pthread_mutex_lock(&pool->lock);
        /* Loop guards against spurious wakeups. */
        while (pool->count == 0 && !pool->shutting_down)
            pthread_cond_wait(&pool->not_empty, &pool->lock);

        if (pool->count == 0 && pool->shutting_down) {
            pthread_mutex_unlock(&pool->lock);
            return NULL;
        }

        task t = pool->queue[pool->head];
        pool->head = (pool->head + 1) % pool->capacity;
        pool->count--;
        pthread_mutex_unlock(&pool->lock);

        t.fn(t.arg);
    }
}

thread_pool *threadpool_create(int nthreads, size_t queue_capacity) {
    thread_pool *pool = calloc(1, sizeof *pool);
    if (!pool) return NULL;
    pool->queue = calloc(queue_capacity, sizeof *pool->queue);
    pool->threads = calloc((size_t)nthreads, sizeof *pool->threads);
    if (!pool->queue || !pool->threads) goto fail;
    pool->capacity = queue_capacity;

    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->not_empty, NULL);

    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&pool->threads[i], NULL, worker_main, pool) != 0) {
            pool->nthreads = i;
            threadpool_destroy(pool);
            return NULL;
        }
    }
    pool->nthreads = nthreads;
    return pool;

fail:
    free(pool->queue);
    free(pool->threads);
    free(pool);
    return NULL;
}

int threadpool_submit(thread_pool *pool, task_fn fn, void *arg) {
    pthread_mutex_lock(&pool->lock);
    if (pool->shutting_down || pool->count == pool->capacity) {
        pthread_mutex_unlock(&pool->lock);
        return -1;
    }
    size_t tail = (pool->head + pool->count) % pool->capacity;
    pool->queue[tail] = (task){fn, arg};
    pool->count++;
    pthread_cond_signal(&pool->not_empty);
    pthread_mutex_unlock(&pool->lock);
    return 0;
}

void threadpool_destroy(thread_pool *pool) {
    if (!pool) return;
    pthread_mutex_lock(&pool->lock);
    pool->shutting_down = 1;
    pthread_cond_broadcast(&pool->not_empty);
    pthread_mutex_unlock(&pool->lock);

    for (int i = 0; i < pool->nthreads; i++) pthread_join(pool->threads[i], NULL);

    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->not_empty);
    free(pool->queue);
    free(pool->threads);
    free(pool);
}
