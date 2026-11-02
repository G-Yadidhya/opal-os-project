#define _POSIX_C_SOURCE 200809L

#include "bptree.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEED_KEYS 128
#define WRITER_THREADS 4
#define READER_THREADS 2
#define KEYS_PER_WRITER 1500
#define READER_LOOPS 30000

typedef struct {
    BPlusTree *tree;
    int id;
    atomic_int *start;
    atomic_int *failed;
} WorkerArgs;

static void await_start(atomic_int *start)
{
    while (atomic_load_explicit(start, memory_order_acquire) == 0) {
    }
}

static void *writer_worker(void *opaque)
{
    WorkerArgs *args = opaque;
    await_start(args->start);

    for (int offset = 0; offset < KEYS_PER_WRITER; offset++) {
        int sequence = offset * WRITER_THREADS + args->id;
        int shuffled = (sequence * 7919) %
                       (WRITER_THREADS * KEYS_PER_WRITER);
        int key = SEED_KEYS + shuffled;
        if (bptree_insert(args->tree, key, key * 7) != 0) {
            atomic_store(args->failed, 1);
            return NULL;
        }
    }
    return NULL;
}

static void *reader_worker(void *opaque)
{
    WorkerArgs *args = opaque;
    await_start(args->start);

    for (int i = 0; i < READER_LOOPS; i++) {
        int key = (i * 37 + args->id * 19) % SEED_KEYS;
        int value = 0;
        int found = 0;
        if (bptree_find(args->tree, key, &value, &found) != 0 ||
            !found || value != key * 7) {
            atomic_store(args->failed, 1);
            return NULL;
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    BPlusTreeLockMode mode = BPTREE_LOCK_MUTEX;
    if (argc == 2 && strcmp(argv[1], "rwlock") == 0) {
        mode = BPTREE_LOCK_RWLOCK;
    } else if (argc != 1) {
        fprintf(stderr, "Usage: %s [rwlock]\n", argv[0]);
        return EXIT_FAILURE;
    }

    BPlusTree *tree = bptree_create_with_lock(mode);
    if (tree == NULL) {
        fprintf(stderr, "FAIL: could not create B+ Tree.\n");
        return EXIT_FAILURE;
    }

    for (int key = 0; key < SEED_KEYS; key++) {
        if (bptree_insert(tree, key, key * 7) != 0) {
            fprintf(stderr, "FAIL: seed insert failed for key %d.\n", key);
            bptree_destroy(tree);
            return EXIT_FAILURE;
        }
    }

    atomic_int start = 0;
    atomic_int failed = 0;
    pthread_t writers[WRITER_THREADS];
    pthread_t readers[READER_THREADS];
    WorkerArgs writer_args[WRITER_THREADS];
    WorkerArgs reader_args[READER_THREADS];
    int writers_created = 0;
    int readers_created = 0;

    for (int i = 0; i < WRITER_THREADS; i++) {
        writer_args[i] = (WorkerArgs){tree, i, &start, &failed};
        if (pthread_create(&writers[i], NULL, writer_worker,
                           &writer_args[i]) != 0) {
            atomic_store(&failed, 1);
            break;
        }
        writers_created++;
    }
    for (int i = 0; i < READER_THREADS && !atomic_load(&failed); i++) {
        reader_args[i] = (WorkerArgs){tree, i, &start, &failed};
        if (pthread_create(&readers[i], NULL, reader_worker,
                           &reader_args[i]) != 0) {
            atomic_store(&failed, 1);
            break;
        }
        readers_created++;
    }

    atomic_store_explicit(&start, 1, memory_order_release);
    for (int i = 0; i < writers_created; i++) {
        pthread_join(writers[i], NULL);
    }
    for (int i = 0; i < readers_created; i++) {
        pthread_join(readers[i], NULL);
    }

    if (writers_created != WRITER_THREADS || readers_created != READER_THREADS) {
        fprintf(stderr, "FAIL: thread creation failed.\n");
        atomic_store(&failed, 1);
    }

    size_t expected_size = SEED_KEYS + WRITER_THREADS * KEYS_PER_WRITER;
    if (!atomic_load(&failed) && bptree_size(tree) != expected_size) {
        fprintf(stderr, "FAIL: expected size %zu, got %zu.\n",
                expected_size, bptree_size(tree));
        atomic_store(&failed, 1);
    }

    for (int key = 0; key < (int)expected_size && !atomic_load(&failed); key++) {
        int value = 0;
        int found = 0;
        if (bptree_find(tree, key, &value, &found) != 0 ||
            !found || value != key * 7) {
            fprintf(stderr, "FAIL: lookup mismatch for key %d.\n", key);
            atomic_store(&failed, 1);
        }
    }

    int replacement = -1;
    int found = 0;
    if (!atomic_load(&failed) &&
        (bptree_insert(tree, 17, 123456) != 0 ||
         bptree_find(tree, 17, &replacement, &found) != 0 ||
         !found || replacement != 123456 ||
         bptree_size(tree) != expected_size)) {
        fprintf(stderr, "FAIL: duplicate-key replacement invariant failed.\n");
        atomic_store(&failed, 1);
    }

    bptree_destroy(tree);
    if (atomic_load(&failed)) {
        return EXIT_FAILURE;
    }

    printf("PASS: %s concurrent B+ Tree inserts/lookups, splits, and replacement\n",
           mode == BPTREE_LOCK_MUTEX ? "mutex" : "rwlock");
    printf("Keys validated: %zu\n", expected_size);
    printf("RESULT: PASS\n");
    return EXIT_SUCCESS;
}
