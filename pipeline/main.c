/* Three-stage thread pipeline using condition variables.
 *
 *   producers ──> [ batches ] ──> workers ──> [ results ] ──> collector
 *
 * Producers split [2, limit) between them and cut their part into batches
 * of BATCH_SIZE numbers. Workers count the primes in each batch and pass
 * the result on. The collector adds the results up. Both queues are
 * bounded and built from a mutex and two condition variables, unlike the
 * semaphore-based queues elsewhere in this repo.
 *
 * Why batches: if every single number went through a queue, the threads
 * would spend their time fighting over the queue mutex instead of
 * computing, and adding workers would make it slower. With 1000 numbers
 * per item the locking cost is negligible and the work scales.
 *
 * Shutdown needs no sentinel values: when a stage is finished, main
 * closes the queue it was feeding, and consumers drain whatever is left
 * and exit.
 *
 * A barrier lines every thread up so they all start at the same moment.
 *
 *   ./pipeline [producers] [workers] [limit]     default: 2 4 100000
 *   (there are 9592 primes below 100000)
 */
#include "../common/common.h"

#define QUEUE_CAPACITY 64
#define BATCH_SIZE 1000
#define DEFAULT_PRODUCERS 2
#define DEFAULT_WORKERS 4
#define DEFAULT_LIMIT 100000
#define MAX_THREADS 64

/* ---- bounded blocking queue ------------------------------------------ */

/* One unit of work. Producers fill in the range; workers fill in the
 * result and send the same struct on to the collector. */
typedef struct
{
    long from, to; /* numbers [from, to) */
    long count;    /* primes found in the range */
    long largest;  /* largest prime in the range, 0 if none */
} batch_t;

typedef struct
{
    batch_t items[QUEUE_CAPACITY];
    int head;  /* index of the oldest item */
    int count; /* items currently stored */
    bool closed;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty; /* signalled after a push or on close */
    pthread_cond_t not_full;  /* signalled after a pop */
} queue_t;

void queue_init(queue_t* q)
{
    q->head = 0;
    q->count = 0;
    q->closed = false;
    if (pthread_mutex_init(&q->mutex, NULL) != 0)
        ERR("pthread_mutex_init");
    if (pthread_cond_init(&q->not_empty, NULL) != 0 || pthread_cond_init(&q->not_full, NULL) != 0)
        ERR("pthread_cond_init");
}

void queue_destroy(queue_t* q)
{
    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->mutex);
}

/* Blocks while the queue is full. */
void queue_push(queue_t* q, batch_t item)
{
    pthread_mutex_lock(&q->mutex);
    /* Always wait in a loop: wake-ups can be spurious, and another thread
     * may have filled the slot between the signal and us getting the lock. */
    while (q->count == QUEUE_CAPACITY)
        pthread_cond_wait(&q->not_full, &q->mutex);

    q->items[(q->head + q->count) % QUEUE_CAPACITY] = item;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
}

/* Blocks while the queue is empty. Returns false once the queue has been
 * closed and everything in it has been taken. */
bool queue_pop(queue_t* q, batch_t* item)
{
    pthread_mutex_lock(&q->mutex);
    while (q->count == 0 && !q->closed)
        pthread_cond_wait(&q->not_empty, &q->mutex);

    if (q->count == 0) /* closed and drained */
    {
        pthread_mutex_unlock(&q->mutex);
        return false;
    }

    *item = q->items[q->head];
    q->head = (q->head + 1) % QUEUE_CAPACITY;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
    return true;
}

/* No more pushes will follow. Broadcast, not signal: every waiting
 * consumer must wake up to see that the queue is done. */
void queue_close(queue_t* q)
{
    pthread_mutex_lock(&q->mutex);
    q->closed = true;
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
}

/* ---- pipeline stages -------------------------------------------------- */

typedef struct
{
    pthread_barrier_t* start;
    queue_t* in;  /* NULL for producers */
    queue_t* out; /* NULL for the collector */
    long from, to; /* producers: range [from, to) to cut into batches */
    long found;    /* collector: total number of primes */
    long largest;  /* collector: largest prime seen */
} stage_args_t;

bool is_prime(long n)
{
    if (n < 2)
        return false;
    for (long d = 2; d * d <= n; d++)
        if (n % d == 0)
            return false;
    return true;
}

void* producer(void* arg)
{
    stage_args_t* a = arg;
    pthread_barrier_wait(a->start);
    for (long from = a->from; from < a->to; from += BATCH_SIZE)
    {
        batch_t b = {.from = from, .to = from + BATCH_SIZE};
        if (b.to > a->to)
            b.to = a->to;
        queue_push(a->out, b);
    }
    return NULL;
}

void* worker(void* arg)
{
    stage_args_t* a = arg;
    pthread_barrier_wait(a->start);
    batch_t b;
    while (queue_pop(a->in, &b))
    {
        b.count = 0;
        b.largest = 0;
        for (long n = b.from; n < b.to; n++)
        {
            if (is_prime(n))
            {
                b.count++;
                b.largest = n;
            }
        }
        queue_push(a->out, b);
    }
    return NULL;
}

void* collector(void* arg)
{
    stage_args_t* a = arg;
    pthread_barrier_wait(a->start);
    batch_t b;
    while (queue_pop(a->in, &b))
    {
        a->found += b.count;
        if (b.largest > a->largest) /* batches finish out of order */
            a->largest = b.largest;
    }
    return NULL;
}

/* ---- main ------------------------------------------------------------- */

void usage(const char* name)
{
    fprintf(stderr, "USAGE: %s [producers 1-%d] [workers 1-%d] [limit >= 2]\n", name, MAX_THREADS, MAX_THREADS);
    exit(EXIT_FAILURE);
}

long parse_long(const char* s, long min, long max, const char* prog)
{
    char* end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || *end != '\0' || v < min || v > max)
        usage(prog);
    return v;
}

void start_thread(pthread_t* tid, void* (*fn)(void*), stage_args_t* args)
{
    int err = pthread_create(tid, NULL, fn, args);
    if (err != 0)
    {
        errno = err;
        ERR("pthread_create");
    }
}

void join_thread(pthread_t tid)
{
    int err = pthread_join(tid, NULL);
    if (err != 0)
    {
        errno = err;
        ERR("pthread_join");
    }
}

double elapsed_ms(const struct timespec* since)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - since->tv_sec) * 1e3 + (now.tv_nsec - since->tv_nsec) / 1e6;
}

int main(int argc, char** argv)
{
    if (argc > 4)
        usage(argv[0]);
    int producers = argc > 1 ? parse_long(argv[1], 1, MAX_THREADS, argv[0]) : DEFAULT_PRODUCERS;
    int workers = argc > 2 ? parse_long(argv[2], 1, MAX_THREADS, argv[0]) : DEFAULT_WORKERS;
    long limit = argc > 3 ? parse_long(argv[3], 2, 1L << 40, argv[0]) : DEFAULT_LIMIT;

    queue_t batches, results;
    queue_init(&batches);
    queue_init(&results);

    /* Every thread plus main waits at the barrier, so timing starts once
     * all threads exist. */
    pthread_barrier_t start;
    if (pthread_barrier_init(&start, NULL, producers + workers + 1 + 1) != 0)
        ERR("pthread_barrier_init");

    pthread_t producer_tids[MAX_THREADS], worker_tids[MAX_THREADS], collector_tid;
    stage_args_t producer_args[MAX_THREADS], worker_args, collector_args;

    long span = limit - 2;
    for (int i = 0; i < producers; i++)
    {
        producer_args[i] = (stage_args_t){.start = &start, .out = &batches};
        producer_args[i].from = 2 + span * i / producers;
        producer_args[i].to = 2 + span * (i + 1) / producers;
        start_thread(&producer_tids[i], producer, &producer_args[i]);
    }

    /* Workers only read their args, so they can all share one struct. */
    worker_args = (stage_args_t){.start = &start, .in = &batches, .out = &results};
    for (int i = 0; i < workers; i++)
        start_thread(&worker_tids[i], worker, &worker_args);

    collector_args = (stage_args_t){.start = &start, .in = &results};
    start_thread(&collector_tid, collector, &collector_args);

    pthread_barrier_wait(&start);
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    /* Shut down one stage at a time, front to back. */
    for (int i = 0; i < producers; i++)
        join_thread(producer_tids[i]);
    queue_close(&batches); /* no more batches: workers drain and exit */

    for (int i = 0; i < workers; i++)
        join_thread(worker_tids[i]);
    queue_close(&results); /* no more results: collector drains and exits */

    join_thread(collector_tid);

    printf("%d producers, %d workers: %ld primes below %ld (largest %ld) in %.1f ms\n", producers, workers,
           collector_args.found, limit, collector_args.largest, elapsed_ms(&t0));

    pthread_barrier_destroy(&start);
    queue_destroy(&results);
    queue_destroy(&batches);
    return EXIT_SUCCESS;
}
