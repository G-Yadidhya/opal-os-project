#define _POSIX_C_SOURCE 200809L

#include "opal_bptree.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define INITIAL_KEYS 512
#define INITIAL_VALUE 100
#define HOT_KEYS (INITIAL_KEYS / 5)

typedef enum { DIST_UNIFORM, DIST_SKEW80_20 } KeyDistribution;

typedef struct {
    OpalBPlusTree *tree;
    int id;
    int operations;
    int lookup_percent;
    int update_percent;
    KeyDistribution distribution;
    atomic_int *start;
    atomic_int *failed;
    long lookups;
    long updates;
    long inserts;
} WorkerArgs;

static void await_start(atomic_int *start)
{
    while (!atomic_load_explicit(start, memory_order_acquire)) {
    }
}

static int existing_key(unsigned int sequence, KeyDistribution distribution)
{
    if (distribution == DIST_UNIFORM) {
        return (int)(sequence % INITIAL_KEYS);
    }
    unsigned int hash = sequence * 2654435761U;
    if (hash % 100U < 80U) {
        return (int)(hash % HOT_KEYS);
    }
    return HOT_KEYS + (int)(hash % (INITIAL_KEYS - HOT_KEYS));
}

static void *worker(void *opaque)
{
    WorkerArgs *args = opaque;
    await_start(args->start);
    for (int i = 0; i < args->operations; i++) {
        unsigned int sequence = (unsigned int)args->id *
                                (unsigned int)args->operations + (unsigned int)i;
        unsigned int category = sequence % 100U;
        if ((int)category < args->lookup_percent) {
            int key = existing_key(sequence * 31U, args->distribution);
            int value = 0, found = 0;
            if (opal_bptree_lookup(args->tree, key, &value, &found) != 0 ||
                !found || value < INITIAL_VALUE) {
                atomic_store(args->failed, 1);
                return NULL;
            }
            args->lookups++;
        } else if ((int)category < args->lookup_percent + args->update_percent) {
            int key = existing_key(sequence * 17U, args->distribution);
            if (opal_bptree_update(args->tree, key, 1) != 0) {
                atomic_store(args->failed, 1);
                return NULL;
            }
            args->updates++;
        } else {
            int key = INITIAL_KEYS + (int)sequence;
            if (opal_bptree_insert(args->tree, key, INITIAL_VALUE) != 0) {
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

static int parse_int(const char *text, int min, int max, int *result)
{
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || text[0] == '\0' || *end != '\0' ||
        value < min || value > max) {
        return -1;
    }
    *result = (int)value;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 7 && argc != 8) {
        fprintf(stderr,
            "Usage: %s <mutex|opal> <threads> <operations_per_thread> <lookup_pct> <update_pct> <insert_pct> [uniform|skew80_20]\n",
            argv[0]);
        return EXIT_FAILURE;
    }
    OpalBPlusTreeMode mode;
    if (strcmp(argv[1], "mutex") == 0) {
        mode = OPAL_BPTREE_MUTEX_BASELINE;
    } else if (strcmp(argv[1], "opal") == 0) {
        mode = OPAL_BPTREE_OPERATION_AWARE;
    } else {
        fprintf(stderr, "Mode must be mutex or opal.\n");
        return EXIT_FAILURE;
    }
    KeyDistribution distribution = DIST_UNIFORM;
    if (argc == 8) {
        if (strcmp(argv[7], "uniform") == 0) {
            distribution = DIST_UNIFORM;
        } else if (strcmp(argv[7], "skew80_20") == 0) {
            distribution = DIST_SKEW80_20;
        } else {
            fprintf(stderr, "Distribution must be uniform or skew80_20.\n");
            return EXIT_FAILURE;
        }
    }
    int threads_count, operations, lookup_percent, update_percent, insert_percent;
    if (parse_int(argv[2], 1, 64, &threads_count) != 0 ||
        parse_int(argv[3], 100, 1000000, &operations) != 0 || operations % 100 != 0 ||
        parse_int(argv[4], 0, 100, &lookup_percent) != 0 ||
        parse_int(argv[5], 0, 100, &update_percent) != 0 ||
        parse_int(argv[6], 0, 100, &insert_percent) != 0 ||
        lookup_percent + update_percent + insert_percent != 100) {
        fprintf(stderr,
            "Threads: 1-64; operations: 100-1000000 and divisible by 100; percentages must sum to 100.\n");
        return EXIT_FAILURE;
    }

    OpalBPlusTree *tree = opal_bptree_create(mode);
    if (tree == NULL) {
        fprintf(stderr, "Tree initialization failed.\n");
        return EXIT_FAILURE;
    }
    for (int key = 0; key < INITIAL_KEYS; key++) {
        if (opal_bptree_insert(tree, key, INITIAL_VALUE) != 0) {
            fprintf(stderr, "Initial tree population failed at key %d.\n", key);
            opal_bptree_destroy(tree);
            return EXIT_FAILURE;
        }
    }

    pthread_t *threads = calloc((size_t)threads_count, sizeof(*threads));
    WorkerArgs *args = calloc((size_t)threads_count, sizeof(*args));
    if (threads == NULL || args == NULL) {
        perror("calloc");
        free(threads);
        free(args);
        opal_bptree_destroy(tree);
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
        opal_bptree_destroy(tree);
        return EXIT_FAILURE;
    }
    for (int i = 0; i < threads_count; i++) {
        args[i] = (WorkerArgs){
            .tree = tree, .id = i, .operations = operations,
            .lookup_percent = lookup_percent, .update_percent = update_percent,
            .distribution = distribution, .start = &start, .failed = &failed
        };
        if (pthread_create(&threads[i], NULL, worker, &args[i]) != 0) {
            fprintf(stderr, "pthread_create failed.\n");
            atomic_store(&failed, 1);
            break;
        }
        created++;
    }
    atomic_store_explicit(&start, 1, memory_order_release);
    for (int i = 0; i < created; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            atomic_store(&failed, 1);
        }
    }
    int end_time_valid = created == threads_count &&
                         clock_gettime(CLOCK_MONOTONIC, &end) == 0;
    if (!end_time_valid) {
        perror("clock_gettime or thread creation");
        atomic_store(&failed, 1);
    }

    long lookups = 0, updates = 0, inserts = 0;
    for (int i = 0; i < created; i++) {
        lookups += args[i].lookups;
        updates += args[i].updates;
        inserts += args[i].inserts;
    }
    long total = (long)threads_count * operations;
    long expected_lookups = total * lookup_percent / 100;
    long expected_updates = total * update_percent / 100;
    long expected_inserts = total * insert_percent / 100;
    size_t expected_keys = INITIAL_KEYS + (size_t)expected_inserts;
    size_t actual_size = opal_bptree_size(tree);
    OpalBPlusTreeStats stats;
    opal_bptree_get_stats(tree, &stats);

    long long sum = 0;
    if (!atomic_load(&failed)) {
        for (int key = 0; key < INITIAL_KEYS; key++) {
            int value = 0, found = 0;
            if (opal_bptree_lookup(tree, key, &value, &found) != 0 || !found) {
                atomic_store(&failed, 1);
                break;
            }
            sum += value;
        }
        for (long sequence = 0; sequence < total && !atomic_load(&failed); sequence++) {
            unsigned int category = (unsigned int)(sequence % 100);
            if ((int)category < lookup_percent + update_percent) {
                continue;
            }
            int key = INITIAL_KEYS + (int)sequence;
            int value = 0, found = 0;
            if (opal_bptree_lookup(tree, key, &value, &found) != 0 ||
                !found || value != INITIAL_VALUE) {
                atomic_store(&failed, 1);
                break;
            }
            sum += value;
        }
    }
    long long expected_sum = (long long)expected_keys * INITIAL_VALUE + expected_updates;
    int pass = !atomic_load(&failed) && end_time_valid &&
               lookups == expected_lookups && updates == expected_updates &&
               inserts == expected_inserts && actual_size == expected_keys &&
               sum == expected_sum &&
               (mode != OPAL_BPTREE_OPERATION_AWARE ||
                stats.update_callbacks == (size_t)expected_updates);
    double elapsed = end_time_valid ? elapsed_seconds(begin, end) : 0.0;
    double throughput = elapsed > 0.0 ? total / elapsed : 0.0;

    printf("Mode: %s\n", mode == OPAL_BPTREE_MUTEX_BASELINE ? "mutex" : "opal");
    printf("Threads: %d\n", threads_count);
    printf("Operations per thread: %d\n", operations);
    printf("Lookup percentage: %d\n", lookup_percent);
    printf("Update percentage: %d\n", update_percent);
    printf("Insert percentage: %d\n", insert_percent);
    printf("Key distribution: %s\n", distribution == DIST_UNIFORM ? "uniform" : "skew80_20");
    printf("Lookup operations: %ld\n", lookups);
    printf("Update operations: %ld\n", updates);
    printf("Insert operations: %ld\n", inserts);
    printf("Final keys: %zu\n", actual_size);
    printf("Expected keys: %zu\n", expected_keys);
    printf("Optimistic retries: %zu\n", stats.optimistic_retries);
    printf("Update callbacks: %zu\n", stats.update_callbacks);
    printf("Update batches: %zu\n", stats.update_batches);
    printf("Singleton update batches: %zu\n", stats.singleton_update_batches);
    printf("Maximum update batch size: %zu\n", stats.maximum_update_batch_size);
    printf("Update batch size buckets (1,2,3-4,5-8,9+): %zu,%zu,%zu,%zu,%zu\n",
           stats.update_batch_size_buckets[0], stats.update_batch_size_buckets[1],
           stats.update_batch_size_buckets[2], stats.update_batch_size_buckets[3],
           stats.update_batch_size_buckets[4]);
    printf("Elapsed seconds: %.9f\n", elapsed);
    printf("Throughput ops/sec: %.2f\n", throughput);
    printf("RESULT: %s\n", pass ? "PASS" : "FAIL");

    free(threads);
    free(args);
    opal_bptree_destroy(tree);
    return pass ? EXIT_SUCCESS : EXIT_FAILURE;
}
