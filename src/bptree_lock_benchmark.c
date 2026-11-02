#define _POSIX_C_SOURCE 200809L

#include "bptree.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define INITIAL_KEYS 8192

typedef struct {
    BPlusTree *tree;
    int id;
    int operations;
    int read_percent;
    atomic_int *start;
    atomic_int *failed;
    long reads;
    long inserts;
} WorkerArgs;

static void await_start(atomic_int *start)
{
    while (atomic_load_explicit(start, memory_order_acquire) == 0) {
    }
}

static void *worker(void *opaque)
{
    WorkerArgs *args = opaque;
    await_start(args->start);

    for (int i = 0; i < args->operations; i++) {
        unsigned int sequence = (unsigned int)i +
            (unsigned int)args->id * (unsigned int)args->operations;
        if ((int)(sequence % 100U) < args->read_percent) {
            int key = (int)((sequence * 2654435761U) % INITIAL_KEYS);
            int value = 0;
            int found = 0;
            if (bptree_find(args->tree, key, &value, &found) != 0 ||
                !found || value != key * 3) {
                atomic_store(args->failed, 1);
                return NULL;
            }
            args->reads++;
        } else {
            int key = INITIAL_KEYS + args->id * args->operations + i;
            if (bptree_insert(args->tree, key, key * 3) != 0) {
                atomic_store(args->failed, 1);
                return NULL;
            }
            args->inserts++;
        }
    }
    return NULL;
}

static double elapsed_seconds(struct timespec start, struct timespec end)
{
    return (double)(end.tv_sec - start.tv_sec) +
           (double)(end.tv_nsec - start.tv_nsec) / 1e9;
}

static int parse_positive(const char *text, long minimum, long maximum,
                          long *value)
{
    char *end = NULL;
    errno = 0;
    long parsed = strtol(text, &end, 10);
    if (errno != 0 || text[0] == '\0' || *end != '\0' ||
        parsed < minimum || parsed > maximum) {
        return -1;
    }
    *value = parsed;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr,
                "Usage: %s <mutex|rwlock> <threads> <operations_per_thread> <read_percent>\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    BPlusTreeLockMode mode;
    if (strcmp(argv[1], "mutex") == 0) {
        mode = BPTREE_LOCK_MUTEX;
    } else if (strcmp(argv[1], "rwlock") == 0) {
        mode = BPTREE_LOCK_RWLOCK;
    } else {
        fprintf(stderr, "Strategy must be 'mutex' or 'rwlock'.\n");
        return EXIT_FAILURE;
    }

    long thread_count, operations, read_percent;
    if (parse_positive(argv[2], 1, 64, &thread_count) != 0 ||
        parse_positive(argv[3], 1, 1000000, &operations) != 0 ||
        parse_positive(argv[4], 0, 100, &read_percent) != 0) {
        fprintf(stderr,
                "Threads must be 1-64, operations 1-1000000, read_percent 0-100.\n");
        return EXIT_FAILURE;
    }

    BPlusTree *tree = bptree_create_with_lock(mode);
    if (tree == NULL) {
        fprintf(stderr, "Could not initialize B+ Tree.\n");
        return EXIT_FAILURE;
    }
    for (int key = 0; key < INITIAL_KEYS; key++) {
        if (bptree_insert(tree, key, key * 3) != 0) {
            fprintf(stderr, "Could not initialize key %d.\n", key);
            bptree_destroy(tree);
            return EXIT_FAILURE;
        }
    }

    pthread_t *threads = calloc((size_t)thread_count, sizeof(*threads));
    WorkerArgs *args = calloc((size_t)thread_count, sizeof(*args));
    if (threads == NULL || args == NULL) {
        perror("calloc");
        free(threads);
        free(args);
        bptree_destroy(tree);
        return EXIT_FAILURE;
    }

    atomic_int start = 0;
    atomic_int failed = 0;
    int created = 0;
    struct timespec begin, end;
    if (clock_gettime(CLOCK_MONOTONIC, &begin) != 0) {
        perror("clock_gettime");
        free(threads);
        free(args);
        bptree_destroy(tree);
        return EXIT_FAILURE;
    }

    for (int i = 0; i < thread_count; i++) {
        args[i] = (WorkerArgs){tree, i, (int)operations, (int)read_percent,
                               &start, &failed, 0, 0};
        int rc = pthread_create(&threads[i], NULL, worker, &args[i]);
        if (rc != 0) {
            fprintf(stderr, "pthread_create failed: %d\n", rc);
            atomic_store(&failed, 1);
            break;
        }
        created++;
    }

    atomic_store_explicit(&start, 1, memory_order_release);
    for (int i = 0; i < created; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            fprintf(stderr, "pthread_join failed for thread %d.\n", i);
            atomic_store(&failed, 1);
        }
    }
    int end_time_valid = 0;
    if (created == thread_count) {
        if (clock_gettime(CLOCK_MONOTONIC, &end) == 0) {
            end_time_valid = 1;
        } else {
            perror("clock_gettime");
            atomic_store(&failed, 1);
        }
    } else {
        atomic_store(&failed, 1);
    }

    long completed_reads = 0;
    long completed_inserts = 0;
    for (int i = 0; i < created; i++) {
        completed_reads += args[i].reads;
        completed_inserts += args[i].inserts;
    }

    long expected_operations = thread_count * operations;
    size_t expected_keys = (size_t)INITIAL_KEYS + (size_t)completed_inserts;
    size_t final_keys = bptree_size(tree);
    double elapsed = end_time_valid
        ? elapsed_seconds(begin, end) : 0.0;
    double throughput = elapsed > 0.0 ? expected_operations / elapsed : 0.0;
    int pass = !atomic_load(&failed) &&
               completed_reads + completed_inserts == expected_operations &&
               final_keys == expected_keys;

    printf("Strategy: %s\n", mode == BPTREE_LOCK_MUTEX ? "mutex" : "rwlock");
    printf("Threads: %ld\n", thread_count);
    printf("Operations per thread: %ld\n", operations);
    printf("Read percentage: %ld\n", read_percent);
    printf("Completed reads: %ld\n", completed_reads);
    printf("Completed inserts: %ld\n", completed_inserts);
    printf("Final keys: %zu\n", final_keys);
    printf("Expected keys: %zu\n", expected_keys);
    printf("Elapsed seconds: %.9f\n", elapsed);
    printf("Throughput ops/sec: %.2f\n", throughput);
    printf("RESULT: %s\n", pass ? "PASS" : "FAIL");

    free(threads);
    free(args);
    bptree_destroy(tree);
    return pass ? EXIT_SUCCESS : EXIT_FAILURE;
}
