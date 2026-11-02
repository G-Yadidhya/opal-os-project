#define _POSIX_C_SOURCE 200809L

#include "opal_bptree.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define INITIAL_KEYS 256
#define INSERTS_PER_WRITER 1000
#define WRITERS 2
#define UPDATERS 2
#define READERS 2
#define UPDATE_LOOPS 1000
#define READ_LOOPS 12000

typedef struct {
    OpalBPlusTree *tree;
    int id;
    atomic_int *start;
    atomic_int *failed;
} Args;

static void await_start(atomic_int *start)
{
    while (!atomic_load_explicit(start, memory_order_acquire)) {
    }
}

static void *insert_worker(void *opaque)
{
    Args *args = opaque;
    await_start(args->start);
    for (int i = 0; i < INSERTS_PER_WRITER; i++) {
        int offset = (i * 7919) % INSERTS_PER_WRITER;
        int key = INITIAL_KEYS + args->id * INSERTS_PER_WRITER + offset;
        if (opal_bptree_insert(args->tree, key, key + 10) != 0) {
            atomic_store(args->failed, 1);
            return NULL;
        }
    }
    return NULL;
}

static void *update_worker(void *opaque)
{
    Args *args = opaque;
    await_start(args->start);
    for (int i = 0; i < UPDATE_LOOPS; i++) {
        int key = (i * 17 + args->id * 11) % INITIAL_KEYS;
        if (opal_bptree_update(args->tree, key, 1) != 0) {
            atomic_store(args->failed, 1);
            return NULL;
        }
    }
    return NULL;
}

static void *read_worker(void *opaque)
{
    Args *args = opaque;
    await_start(args->start);
    for (int i = 0; i < READ_LOOPS; i++) {
        int key = (i * 37 + args->id * 13) % INITIAL_KEYS;
        int value = 0;
        int found = 0;
        if (opal_bptree_lookup(args->tree, key, &value, &found) != 0 ||
            !found || value < 10) {
            atomic_store(args->failed, 1);
            return NULL;
        }
    }
    return NULL;
}

static int run_mode(OpalBPlusTreeMode mode, const char *name)
{
    OpalBPlusTree *tree = opal_bptree_create(mode);
    if (tree == NULL) {
        fprintf(stderr, "FAIL: %s tree creation failed.\n", name);
        return 1;
    }
    for (int key = 0; key < INITIAL_KEYS; key++) {
        if (opal_bptree_insert(tree, key, 10) != 0) {
            fprintf(stderr, "FAIL: %s seed insertion failed at %d.\n", name, key);
            opal_bptree_destroy(tree);
            return 1;
        }
    }

    enum { TOTAL_THREADS = WRITERS + UPDATERS + READERS };
    pthread_t threads[TOTAL_THREADS];
    Args args[TOTAL_THREADS];
    atomic_int start = 0;
    atomic_int failed = 0;
    int created = 0;
    for (int i = 0; i < TOTAL_THREADS; i++) {
        args[i] = (Args){tree, i, &start, &failed};
        void *(*routine)(void *);
        if (i < WRITERS) {
            routine = insert_worker;
        } else if (i < WRITERS + UPDATERS) {
            args[i].id = i - WRITERS;
            routine = update_worker;
        } else {
            args[i].id = i - WRITERS - UPDATERS;
            routine = read_worker;
        }
        if (pthread_create(&threads[i], NULL, routine, &args[i]) != 0) {
            fprintf(stderr, "FAIL: %s thread creation failed.\n", name);
            atomic_store(&failed, 1);
            break;
        }
        created++;
    }
    atomic_store_explicit(&start, 1, memory_order_release);
    for (int i = 0; i < created; i++) {
        pthread_join(threads[i], NULL);
    }
    if (created != TOTAL_THREADS) {
        atomic_store(&failed, 1);
    }

    size_t expected_size = INITIAL_KEYS + WRITERS * INSERTS_PER_WRITER;
    if (opal_bptree_size(tree) != expected_size) {
        fprintf(stderr, "FAIL: %s size mismatch.\n", name);
        atomic_store(&failed, 1);
    }
    long long observed_sum = 0;
    for (int key = 0; key < (int)expected_size && !atomic_load(&failed); key++) {
        int value = 0;
        int found = 0;
        if (opal_bptree_lookup(tree, key, &value, &found) != 0 || !found) {
            fprintf(stderr, "FAIL: %s lookup failed for key %d.\n", name, key);
            atomic_store(&failed, 1);
            break;
        }
        observed_sum += value;
    }
    long long expected_sum = (long long)INITIAL_KEYS * 10;
    for (int key = INITIAL_KEYS; key < (int)expected_size; key++) {
        expected_sum += key + 10;
    }
    expected_sum += (long long)UPDATERS * UPDATE_LOOPS;
    OpalBPlusTreeStats stats;
    opal_bptree_get_stats(tree, &stats);
    if (observed_sum != expected_sum) {
        fprintf(stderr, "FAIL: %s value sum %lld != %lld.\n",
                name, observed_sum, expected_sum);
        atomic_store(&failed, 1);
    }
    if (stats.update_callbacks != (size_t)(UPDATERS * UPDATE_LOOPS)) {
        fprintf(stderr, "FAIL: %s callback count mismatch.\n", name);
        atomic_store(&failed, 1);
    }
    if (mode == OPAL_BPTREE_OPERATION_AWARE && stats.update_batches == 0) {
        fprintf(stderr, "FAIL: %s had no update batches.\n", name);
        atomic_store(&failed, 1);
    }

    if (!atomic_load(&failed)) {
        printf("PASS: %s lookups, batched updates, and split inserts\n", name);
        printf("  keys=%zu callbacks=%zu batches=%zu optimistic_retries=%zu\n",
               expected_size, stats.update_callbacks, stats.update_batches,
               stats.optimistic_retries);
    }
    opal_bptree_destroy(tree);
    return atomic_load(&failed) ? 1 : 0;
}

int main(void)
{
    int failed = 0;
    failed |= run_mode(OPAL_BPTREE_MUTEX_BASELINE, "mutex baseline");
    failed |= run_mode(OPAL_BPTREE_OPERATION_AWARE, "OPAL-inspired C11");
    if (failed) {
        return EXIT_FAILURE;
    }
    puts("RESULT: PASS");
    return EXIT_SUCCESS;
}
