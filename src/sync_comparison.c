#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <errno.h>
#include <limits.h>

typedef enum {
    MODE_MUTEX,
    MODE_ATOMIC
} SyncMode;

typedef struct {
    long iterations;
    SyncMode mode;
} WorkerArgs;

static long mutex_counter = 0;
static atomic_long atomic_counter;
static pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_barrier_t start_barrier;

static void *worker(void *arg) {
    WorkerArgs *args = (WorkerArgs *)arg;

    /*
     * All workers wait here until the main thread
     * reaches the same barrier.
     */
    int rc = pthread_barrier_wait(&start_barrier);

    if (rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD) {
        return (void *)1;
    }

    for (long i = 0; i < args->iterations; i++) {
        if (args->mode == MODE_MUTEX) {
            pthread_mutex_lock(&counter_mutex);
            mutex_counter++;
            pthread_mutex_unlock(&counter_mutex);
        } else {
            atomic_fetch_add_explicit(
                &atomic_counter,
                1,
                memory_order_relaxed
            );
        }
    }

    return NULL;
}

static int parse_positive_long(const char *text, long *value) {
    char *end = NULL;

    errno = 0;
    long parsed = strtol(text, &end, 10);

    if (errno != 0 || end == text || *end != '\0' ||
        parsed <= 0) {
        return 0;
    }

    *value = parsed;
    return 1;
}

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(
            stderr,
            "Usage: %s <mutex|atomic> <threads> <iterations>\n",
            argv[0]
        );
        return 1;
    }

    SyncMode mode;

    if (strcmp(argv[1], "mutex") == 0) {
        mode = MODE_MUTEX;
    } else if (strcmp(argv[1], "atomic") == 0) {
        mode = MODE_ATOMIC;
    } else {
        fprintf(stderr, "Mode must be mutex or atomic.\n");
        return 1;
    }

    long thread_count;
    long iterations;

    if (!parse_positive_long(argv[2], &thread_count) ||
        !parse_positive_long(argv[3], &iterations)) {
        fprintf(
            stderr,
            "Thread count and iterations must be positive integers.\n"
        );
        return 1;
    }

    if (thread_count > 256 ||
        iterations > LONG_MAX / thread_count) {
        fprintf(stderr, "Requested workload is too large.\n");
        return 1;
    }

    pthread_t *threads = calloc(
        (size_t)thread_count, sizeof(*threads)
    );

    WorkerArgs *args = calloc(
        (size_t)thread_count, sizeof(*args)
    );

    if (threads == NULL || args == NULL) {
        perror("calloc");
        free(threads);
        free(args);
        return 1;
    }

    mutex_counter = 0;
    atomic_init(&atomic_counter, 0);

    int rc = pthread_barrier_init(
        &start_barrier, NULL, (unsigned)thread_count + 1
    );

    if (rc != 0) {
        fprintf(stderr, "Barrier initialization failed: %d\n", rc);
        free(threads);
        free(args);
        return 1;
    }

    long created = 0;

    for (long i = 0; i < thread_count; i++) {
        args[i].iterations = iterations;
        args[i].mode = mode;

        rc = pthread_create(
            &threads[i], NULL, worker, &args[i]
        );

        if (rc != 0) {
            fprintf(stderr, "Thread creation failed: %d\n", rc);
            break;
        }

        created++;
    }

    /*
     * The barrier was initialized for all requested workers
     * plus the main thread. If creation failed partway through,
     * the remaining workers cannot complete that barrier.
     * Exit rather than waiting forever.
     */
    if (created != thread_count) {
        fprintf(
            stderr,
            "Not all workers were created; aborting this run.\n"
        );

        /*
         * This is an introductory benchmark. The barrier
         * cannot be safely reused with a smaller participant
         * count, so terminate instead of attempting to continue.
         */
        exit(EXIT_FAILURE);
    }

    struct timespec start;
    struct timespec finish;

    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }

    rc = pthread_barrier_wait(&start_barrier);

    if (rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD) {
        fprintf(stderr, "Main barrier wait failed: %d\n", rc);
        exit(EXIT_FAILURE);
    }

    int join_failed = 0;

    for (long i = 0; i < thread_count; i++) {
        void *result = NULL;

        rc = pthread_join(threads[i], &result);

        if (rc != 0 || result != NULL) {
            fprintf(stderr, "Thread join or worker failed.\n");
            join_failed = 1;
        }
    }

    if (join_failed) {
        exit(EXIT_FAILURE);
    }

    if (clock_gettime(CLOCK_MONOTONIC, &finish) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }

    long actual = (mode == MODE_MUTEX)
        ? mutex_counter
        : atomic_load_explicit(
              &atomic_counter, memory_order_relaxed
          );

    long expected = thread_count * iterations;

    double elapsed =
        (finish.tv_sec - start.tv_sec) +
        (finish.tv_nsec - start.tv_nsec) / 1e9;

    double throughput = (double)actual / elapsed;

    printf("Mode:              %s\n", argv[1]);
    printf("Threads:           %ld\n", thread_count);
    printf("Iterations/thread: %ld\n", iterations);
    printf("Expected counter:  %ld\n", expected);
    printf("Actual counter:    %ld\n", actual);
    printf("Elapsed seconds:   %.6f\n", elapsed);
    printf("Throughput ops/s:  %.2f\n", throughput);

    pthread_barrier_destroy(&start_barrier);
    pthread_mutex_destroy(&counter_mutex);

    free(threads);
    free(args);

    return actual == expected ? 0 : 1;
}
