#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <errno.h>
#include <limits.h>

#define ITERATIONS 1000000

atomic_long counter;

void *increment_counter(void *arg) {
    (void)arg;

    for (int i = 0; i < ITERATIONS; i++) {
        atomic_fetch_add_explicit(
            &counter, 1, memory_order_relaxed
        );
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s THREAD_COUNT\n", argv[0]);
        return 1;
    }

    char *end = NULL;
    errno = 0;
    long thread_count = strtol(argv[1], &end, 10);

    if (errno != 0 || end == argv[1] || *end != '\0' ||
        thread_count < 1 || thread_count > 256) {
        fprintf(stderr, "Thread count must be between 1 and 256.\n");
        return 1;
    }

    pthread_t *threads = calloc(
        (size_t)thread_count, sizeof(*threads)
    );

    if (threads == NULL) {
        perror("calloc");
        return 1;
    }

    atomic_init(&counter, 0);

    struct timespec start, finish;

    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        perror("clock_gettime");
        free(threads);
        return 1;
    }

    long created = 0;

    for (long i = 0; i < thread_count; i++) {
        int rc = pthread_create(
            &threads[i], NULL, increment_counter, NULL
        );

        if (rc != 0) {
            fprintf(stderr, "pthread_create failed: %d\n", rc);
            break;
        }

        created++;
    }

    for (long i = 0; i < created; i++) {
        int rc = pthread_join(threads[i], NULL);

        if (rc != 0) {
            fprintf(stderr, "pthread_join failed: %d\n", rc);
            free(threads);
            return 1;
        }
    }

    if (clock_gettime(CLOCK_MONOTONIC, &finish) != 0) {
        perror("clock_gettime");
        free(threads);
        return 1;
    }

    long expected = created * (long)ITERATIONS;
    long actual = atomic_load_explicit(
        &counter, memory_order_relaxed
    );

    double elapsed_seconds =
        (finish.tv_sec - start.tv_sec) +
        (finish.tv_nsec - start.tv_nsec) / 1e9;

    printf("Requested threads: %ld\n", thread_count);
    printf("Created threads:   %ld\n", created);
    printf("Expected counter:  %ld\n", expected);
    printf("Actual counter:    %ld\n", actual);
    printf("Elapsed time:      %.6f seconds\n", elapsed_seconds);

    free(threads);

    if (created != thread_count || actual != expected) {
        return 1;
    }

    return 0;
}
