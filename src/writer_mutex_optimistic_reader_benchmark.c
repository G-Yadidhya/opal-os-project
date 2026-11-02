#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <limits.h>

#define READER_ITERATIONS 200000L
#define WRITER_ITERATIONS 100000L

typedef struct {
    atomic_uint version;
    atomic_int value_a;
    atomic_int value_b;
    pthread_mutex_t writer_lock;

    atomic_long successful_reads;
    atomic_long read_retries;
    atomic_long invalid_snapshots;
} SharedData;

typedef struct {
    SharedData *shared;
    int is_writer;
} ThreadArgs;

static SharedData data;

static int lock_writer(pthread_mutex_t *lock)
{
    return pthread_mutex_lock(lock);
}

static int unlock_writer(pthread_mutex_t *lock)
{
    return pthread_mutex_unlock(lock);
}

static void *reader_thread(void *arg)
{
    ThreadArgs *args = arg;
    SharedData *shared = args->shared;

    for (long i = 0; i < READER_ITERATIONS; i++) {
        for (;;) {
            unsigned before = atomic_load_explicit(
                &shared->version, memory_order_seq_cst);

            if (before & 1U) {
                atomic_fetch_add_explicit(
                    &shared->read_retries, 1,
                    memory_order_relaxed);
                continue;
            }

            int a = atomic_load_explicit(
                &shared->value_a, memory_order_seq_cst);

            int b = atomic_load_explicit(
                &shared->value_b, memory_order_seq_cst);

            unsigned after = atomic_load_explicit(
                &shared->version, memory_order_seq_cst);

            if (before == after && !(after & 1U)) {
                if (b != -a) {
                    atomic_fetch_add_explicit(
                        &shared->invalid_snapshots, 1,
                        memory_order_relaxed);
                }

                atomic_fetch_add_explicit(
                    &shared->successful_reads, 1,
                    memory_order_relaxed);
                break;
            }

            atomic_fetch_add_explicit(
                &shared->read_retries, 1,
                memory_order_relaxed);
        }
    }

    return NULL;
}

static void *writer_thread(void *arg)
{
    ThreadArgs *args = arg;
    SharedData *shared = args->shared;

    for (long i = 0; i < WRITER_ITERATIONS; i++) {
        int rc = lock_writer(&shared->writer_lock);
        if (rc != 0) {
            fprintf(stderr, "Writer mutex lock failed: %d\n", rc);
            return (void *)1;
        }

        atomic_fetch_add_explicit(
            &shared->version, 1, memory_order_seq_cst);

        int current = atomic_load_explicit(
            &shared->value_a, memory_order_seq_cst);

        atomic_store_explicit(
            &shared->value_a, current + 1,
            memory_order_seq_cst);

        atomic_store_explicit(
            &shared->value_b, -(current + 1),
            memory_order_seq_cst);

        atomic_fetch_add_explicit(
            &shared->version, 1, memory_order_seq_cst);

        rc = unlock_writer(&shared->writer_lock);
        if (rc != 0) {
            fprintf(stderr, "Writer mutex unlock failed: %d\n", rc);
            return (void *)1;
        }
    }

    return NULL;
}

static double elapsed_seconds(
    struct timespec start,
    struct timespec end)
{
    return (double)(end.tv_sec - start.tv_sec)
         + (double)(end.tv_nsec - start.tv_nsec) / 1e9;
}

int main(int argc, char *argv[])
{
    int reader_count = 4;
    int writer_count = 2;

    if (argc == 3) {
        char *end1;
        char *end2;

        errno = 0;
        long r = strtol(argv[1], &end1, 10);
        int err1 = errno;

        errno = 0;
        long w = strtol(argv[2], &end2, 10);
        int err2 = errno;

        if (err1 != 0 || err2 != 0 ||
            *argv[1] == '\0' || *argv[2] == '\0' ||
            *end1 != '\0' || *end2 != '\0' ||
            r < 1 || r > 128 || w < 1 || w > 128) {
            fprintf(stderr,
                    "Usage: %s [readers writers]\n"
                    "Readers and writers must each be 1-128.\n",
                    argv[0]);
            return EXIT_FAILURE;
        }

        reader_count = (int)r;
        writer_count = (int)w;
    } else if (argc != 1) {
        fprintf(stderr,
                "Usage: %s [readers writers]\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    atomic_init(&data.version, 0);
    atomic_init(&data.value_a, 0);
    atomic_init(&data.value_b, 0);
    int mutex_rc = pthread_mutex_init(&data.writer_lock, NULL);
    if (mutex_rc != 0) {
        fprintf(stderr, "Writer mutex initialization failed: %d\n", mutex_rc);
        return EXIT_FAILURE;
    }

    atomic_init(&data.successful_reads, 0);
    atomic_init(&data.read_retries, 0);
    atomic_init(&data.invalid_snapshots, 0);

    int total_threads = reader_count + writer_count;

    pthread_t *threads = calloc(
        (size_t)total_threads, sizeof(*threads));

    ThreadArgs *args = calloc(
        (size_t)total_threads, sizeof(*args));

    if (threads == NULL || args == NULL) {
        perror("calloc");
        free(threads);
        free(args);
        pthread_mutex_destroy(&data.writer_lock);
        return EXIT_FAILURE;
    }

    int created = 0;
    int create_failed = 0;

    struct timespec start, end;

    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        perror("clock_gettime");
        free(threads);
        free(args);
        pthread_mutex_destroy(&data.writer_lock);
        return EXIT_FAILURE;
    }

    for (int i = 0; i < total_threads; i++) {
        args[i].shared = &data;
        args[i].is_writer = (i >= reader_count);

        void *(*routine)(void *) = args[i].is_writer
            ? writer_thread : reader_thread;

        int rc = pthread_create(
            &threads[i], NULL, routine, &args[i]);

        if (rc != 0) {
            fprintf(stderr,
                    "pthread_create failed: %d\n", rc);
            create_failed = 1;
            break;
        }

        created++;
    }

    int thread_failed = 0;
    for (int i = 0; i < created; i++) {
        void *thread_result = NULL;
        int rc = pthread_join(threads[i], &thread_result);
        if (rc != 0) {
            fprintf(stderr, "pthread_join failed: %d\n", rc);
            thread_failed = 1;
        } else if (thread_result != NULL) {
            thread_failed = 1;
        }
    }

    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
        perror("clock_gettime");
        free(threads);
        free(args);
        pthread_mutex_destroy(&data.writer_lock);
        return EXIT_FAILURE;
    }

    free(threads);
    free(args);

    if (create_failed || thread_failed) {
        pthread_mutex_destroy(&data.writer_lock);
        return EXIT_FAILURE;
    }

    long expected_reads =
        (long)reader_count * READER_ITERATIONS;

    long expected_updates =
        (long)writer_count * WRITER_ITERATIONS;

    long actual_reads = atomic_load_explicit(
        &data.successful_reads, memory_order_relaxed);

    long retries = atomic_load_explicit(
        &data.read_retries, memory_order_relaxed);

    long invalid = atomic_load_explicit(
        &data.invalid_snapshots, memory_order_relaxed);

    int final_a = atomic_load_explicit(
        &data.value_a, memory_order_seq_cst);

    int final_b = atomic_load_explicit(
        &data.value_b, memory_order_seq_cst);

    unsigned final_version = atomic_load_explicit(
        &data.version, memory_order_seq_cst);

    double seconds = elapsed_seconds(start, end);

    long total_operations = actual_reads + expected_updates;

    double throughput = seconds > 0.0
        ? (double)total_operations / seconds : 0.0;

    printf("Readers:              %d\n", reader_count);
    printf("Writers:              %d\n", writer_count);
    printf("Reader strategy:      optimistic version validation\n");
    printf("Writer strategy:      pthread mutex serialization\n");
    printf("Successful reads:     %ld\n", actual_reads);
    printf("Read retries:         %ld\n", retries);
    printf("Invalid snapshots:    %ld\n", invalid);
    printf("Expected updates:     %ld\n", expected_updates);
    printf("Final value A:        %d\n", final_a);
    printf("Final value B:        %d\n", final_b);
    printf("Final version:        %u\n", final_version);
    printf("Total operations:     %ld\n", total_operations);
    printf("Elapsed seconds:      %.6f\n", seconds);
    printf("Throughput ops/sec:   %.2f\n", throughput);

    int passed =
        actual_reads == expected_reads &&
        final_a == expected_updates &&
        final_b == -final_a &&
        final_version == 2U * (unsigned)expected_updates &&
        invalid == 0;

    printf("RESULT: %s\n", passed ? "PASS" : "FAIL");

    mutex_rc = pthread_mutex_destroy(&data.writer_lock);
    if (mutex_rc != 0) {
        fprintf(stderr, "Writer mutex destruction failed: %d\n", mutex_rc);
        return EXIT_FAILURE;
    }
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

