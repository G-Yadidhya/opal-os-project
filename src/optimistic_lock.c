#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define READER_THREADS 4
#define WRITER_THREADS 2
#define READER_ITERATIONS 200000
#define WRITER_ITERATIONS 100000

/*
 * Teaching implementation of version-based optimistic reading.
 *
 * Even version: no writer is currently modifying the data.
 * Odd version:  a writer is modifying the data.
 *
 * All shared data fields are atomic to avoid C data races.
 */

static atomic_uint version = 0;

static atomic_int value_a = 0;
static atomic_int value_b = 0;

/* Serialize writers so only one writer modifies the record at a time. */
static atomic_flag writer_lock = ATOMIC_FLAG_INIT;

static atomic_ulong successful_reads = 0;
static atomic_ulong read_retries = 0;
static atomic_ulong invalid_snapshots = 0;

static void acquire_writer_lock(void)
{
    while (atomic_flag_test_and_set_explicit(
               &writer_lock, memory_order_seq_cst)) {
        /* Spin until the writer lock becomes available. */
    }
}

static void release_writer_lock(void)
{
    atomic_flag_clear_explicit(
        &writer_lock, memory_order_seq_cst);
}

/*
 * Writer:
 * 1. Acquire exclusive writer access.
 * 2. Change version to odd.
 * 3. Update both fields.
 * 4. Change version to even.
 * 5. Release writer access.
 */
static void write_record(void)
{
    acquire_writer_lock();

    atomic_fetch_add_explicit(
        &version, 1, memory_order_seq_cst);

    int next = atomic_load_explicit(
        &value_a, memory_order_seq_cst) + 1;

    atomic_store_explicit(
        &value_a, next, memory_order_seq_cst);

    atomic_store_explicit(
        &value_b, -next, memory_order_seq_cst);

    atomic_fetch_add_explicit(
        &version, 1, memory_order_seq_cst);

    release_writer_lock();
}

/*
 * Reader:
 * Read a snapshot and accept it only if the version remains
 * unchanged and even throughout the operation.
 */
static bool read_record(int *a, int *b)
{
    unsigned before;
    unsigned after;

    before = atomic_load_explicit(
        &version, memory_order_seq_cst);

    if (before & 1U) {
        atomic_fetch_add_explicit(
            &read_retries, 1, memory_order_relaxed);
        return false;
    }

    int first = atomic_load_explicit(
        &value_a, memory_order_seq_cst);

    int second = atomic_load_explicit(
        &value_b, memory_order_seq_cst);

    after = atomic_load_explicit(
        &version, memory_order_seq_cst);

    if (before != after || (after & 1U)) {
        atomic_fetch_add_explicit(
            &read_retries, 1, memory_order_relaxed);
        return false;
    }

    *a = first;
    *b = second;

    atomic_fetch_add_explicit(
        &successful_reads, 1, memory_order_relaxed);

    return true;
}

static void *reader_thread(void *arg)
{
    (void)arg;

    for (int i = 0; i < READER_ITERATIONS; i++) {
        int a;
        int b;

        while (!read_record(&a, &b)) {
            /* Retry until a valid snapshot is obtained. */
        }

        /*
         * The writer always maintains b == -a.
         * A valid snapshot must preserve that relationship.
         */
        if (b != -a) {
            atomic_fetch_add_explicit(
                &invalid_snapshots, 1, memory_order_relaxed);
        }
    }

    return NULL;
}

static void *writer_thread(void *arg)
{
    (void)arg;

    for (int i = 0; i < WRITER_ITERATIONS; i++) {
        write_record();
    }

    return NULL;
}

int main(void)
{
    pthread_t readers[READER_THREADS];
    pthread_t writers[WRITER_THREADS];

    int readers_created = 0;
    int writers_created = 0;

    for (int i = 0; i < READER_THREADS; i++) {
        int rc = pthread_create(
            &readers[i], NULL, reader_thread, NULL);

        if (rc != 0) {
            fprintf(stderr,
                    "Error: Could not create reader thread %d.\n", i);
            goto cleanup;
        }

        readers_created++;
    }

    for (int i = 0; i < WRITER_THREADS; i++) {
        int rc = pthread_create(
            &writers[i], NULL, writer_thread, NULL);

        if (rc != 0) {
            fprintf(stderr,
                    "Error: Could not create writer thread %d.\n", i);
            goto cleanup;
        }

        writers_created++;
    }

    for (int i = 0; i < writers_created; i++) {
        if (pthread_join(writers[i], NULL) != 0) {
            fprintf(stderr, "Error: Could not join writer thread.\n");
            return EXIT_FAILURE;
        }
    }

    for (int i = 0; i < readers_created; i++) {
        if (pthread_join(readers[i], NULL) != 0) {
            fprintf(stderr, "Error: Could not join reader thread.\n");
            return EXIT_FAILURE;
        }
    }

    {
        unsigned final_version = atomic_load_explicit(
            &version, memory_order_seq_cst);

        int final_a = atomic_load_explicit(
            &value_a, memory_order_seq_cst);

        int final_b = atomic_load_explicit(
            &value_b, memory_order_seq_cst);

        unsigned long expected_updates =
            (unsigned long)WRITER_THREADS * WRITER_ITERATIONS;

        unsigned long reads = atomic_load_explicit(
            &successful_reads, memory_order_relaxed);

        unsigned long retries = atomic_load_explicit(
            &read_retries, memory_order_relaxed);

        unsigned long invalid = atomic_load_explicit(
            &invalid_snapshots, memory_order_relaxed);

        printf("\n");
        printf("============================================\n");
        printf(" OPTIMISTIC VERSION-BASED LOCK DEMONSTRATION\n");
        printf("============================================\n");
        printf("Reader threads:       %d\n", READER_THREADS);
        printf("Writer threads:       %d\n", WRITER_THREADS);
        printf("Updates per writer:   %d\n", WRITER_ITERATIONS);
        printf("Expected final value: %lu\n", expected_updates);
        printf("Actual value A:       %d\n", final_a);
        printf("Actual value B:       %d\n", final_b);
        printf("Final version:        %u\n", final_version);
        printf("Successful reads:     %lu\n", reads);
        printf("Read retries:         %lu\n", retries);
        printf("Invalid snapshots:    %lu\n", invalid);

        bool passed =
            final_a == (int)expected_updates &&
            final_b == -(int)expected_updates &&
            final_version == 2U * expected_updates &&
            (final_version & 1U) == 0 &&
            invalid == 0 &&
            reads == (unsigned long)READER_THREADS *
                     READER_ITERATIONS;

        if (passed) {
            printf("\nRESULT: PASS\n");
            printf("All final-state and snapshot checks passed.\n");
            return EXIT_SUCCESS;
        }

        printf("\nRESULT: FAIL\n");
        printf("At least one correctness check failed.\n");
        return EXIT_FAILURE;
    }

cleanup:
    /*
     * Threads that were created may still be running.
     * Join them before exiting.
     */
    for (int i = 0; i < writers_created; i++) {
        pthread_join(writers[i], NULL);
    }

    for (int i = 0; i < readers_created; i++) {
        pthread_join(readers[i], NULL);
    }

    return EXIT_FAILURE;
}
