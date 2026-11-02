#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>

#define READER_ITERATIONS 200000L
#define WRITER_ITERATIONS 100000L

typedef struct {
    pthread_mutex_t lock;

    int value_a;
    int value_b;

    long successful_reads;
    long invalid_snapshots;
} SharedData;

typedef struct {
    SharedData *shared;
    int is_writer;
} ThreadArgs;

static SharedData data;

static void *reader_thread(void *arg)
{
    ThreadArgs *args = arg;
    SharedData *shared = args->shared;

    for (long i = 0; i < READER_ITERATIONS; i++) {
        int a;
        int b;

        int rc = pthread_mutex_lock(&shared->lock);
        if (rc != 0) {
            fprintf(stderr, "Reader mutex lock failed: %d\n", rc);
            return (void *)1;
        }

        a = shared->value_a;
        b = shared->value_b;

        if (b != -a) {
            shared->invalid_snapshots++;
        }

        shared->successful_reads++;

        rc = pthread_mutex_unlock(&shared->lock);
        if (rc != 0) {
            fprintf(stderr, "Reader mutex unlock failed: %d\n", rc);
            return (void *)1;
        }
    }

    return NULL;
}

static void *writer_thread(void *arg)
{
    ThreadArgs *args = arg;
    SharedData *shared = args->shared;

    for (long i = 0; i < WRITER_ITERATIONS; i++) {
        int rc = pthread_mutex_lock(&shared->lock);
        if (rc != 0) {
            fprintf(stderr, "Writer mutex lock failed: %d\n", rc);
            return (void *)1;
        }

        shared->value_a++;
        shared->value_b = -shared->value_a;

        rc = pthread_mutex_unlock(&shared->lock);
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

    data.value_a = 0;
    data.value_b = 0;
    data.successful_reads = 0;
    data.invalid_snapshots = 0;

    int rc = pthread_mutex_init(&data.lock, NULL);
    if (rc != 0) {
        fprintf(stderr, "Mutex initialization failed: %d\n", rc);
        return EXIT_FAILURE;
    }

    int total_threads = reader_count + writer_count;

    pthread_t *threads = calloc(
        (size_t)total_threads, sizeof(*threads));

    ThreadArgs *args = calloc(
        (size_t)total_threads, sizeof(*args));

    if (threads == NULL || args == NULL) {
        perror("calloc");
        free(threads);
        free(args);
        pthread_mutex_destroy(&data.lock);
        return EXIT_FAILURE;
    }

    int created = 0;
    int create_failed = 0;

    struct timespec start, end;

    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        perror("clock_gettime");
        free(threads);
        free(args);
        pthread_mutex_destroy(&data.lock);
        return EXIT_FAILURE;
    }

    for (int i = 0; i < total_threads; i++) {
        args[i].shared = &data;
        args[i].is_writer = (i >= reader_count);

        void *(*routine)(void *) = args[i].is_writer
            ? writer_thread : reader_thread;

        rc = pthread_create(
            &threads[i], NULL, routine, &args[i]);

        if (rc != 0) {
            fprintf(stderr, "pthread_create failed: %d\n", rc);
            create_failed = 1;
            break;
        }

        created++;
    }

    int thread_failed = 0;

    for (int i = 0; i < created; i++) {
        void *thread_result = NULL;

        rc = pthread_join(threads[i], &thread_result);

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
        pthread_mutex_destroy(&data.lock);
        return EXIT_FAILURE;
    }

    free(threads);
    free(args);

    if (create_failed || thread_failed) {
        pthread_mutex_destroy(&data.lock);
        return EXIT_FAILURE;
    }

    long expected_reads =
        (long)reader_count * READER_ITERATIONS;

    long expected_updates =
        (long)writer_count * WRITER_ITERATIONS;

    double seconds = elapsed_seconds(start, end);

    long total_operations =
        data.successful_reads + expected_updates;

    double throughput = seconds > 0.0
        ? (double)total_operations / seconds : 0.0;

    printf("Readers:              %d\n", reader_count);
    printf("Writers:              %d\n", writer_count);
    printf("Successful reads:     %ld\n", data.successful_reads);
    printf("Invalid snapshots:    %ld\n", data.invalid_snapshots);
    printf("Expected updates:     %ld\n", expected_updates);
    printf("Final value A:        %d\n", data.value_a);
    printf("Final value B:        %d\n", data.value_b);
    printf("Total operations:     %ld\n", total_operations);
    printf("Elapsed seconds:      %.6f\n", seconds);
    printf("Throughput ops/sec:   %.2f\n", throughput);

    int passed =
        data.successful_reads == expected_reads &&
        data.value_a == expected_updates &&
        data.value_b == -data.value_a &&
        data.invalid_snapshots == 0;

    printf("RESULT: %s\n", passed ? "PASS" : "FAIL");

    rc = pthread_mutex_destroy(&data.lock);
    if (rc != 0) {
        fprintf(stderr, "Mutex destruction failed: %d\n", rc);
        return EXIT_FAILURE;
    }

    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
