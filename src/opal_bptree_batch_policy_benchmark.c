#define _POSIX_C_SOURCE 200809L

#include "opal_bptree.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define INITIAL_KEYS 512
#define INITIAL_VALUE 100
#define LOOKUP_PERCENT 50
#define UPDATE_PERCENT 40
#define INSERT_PERCENT 10

typedef struct {
    OpalBPlusTree *tree;
    int id;
    int operations;
    atomic_int *start;
    atomic_int *failed;
    uint64_t *update_latency_ns;
    int latency_count;
    long lookups;
    long updates;
    long inserts;
} WorkerArgs;

static void await_start(atomic_int *start)
{
    while (!atomic_load_explicit(start, memory_order_acquire)) {
    }
}

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static void *worker(void *opaque)
{
    WorkerArgs *args = opaque;
    await_start(args->start);
    for (int i = 0; i < args->operations; i++) {
        unsigned int sequence = (unsigned int)args->id *
                                (unsigned int)args->operations + (unsigned int)i;
        unsigned int category = sequence % 100U;
        if (category < LOOKUP_PERCENT) {
            int key = (int)((sequence * 31U) % INITIAL_KEYS);
            int value = 0, found = 0;
            if (opal_bptree_lookup(args->tree, key, &value, &found) != 0 ||
                !found || value < INITIAL_VALUE) {
                atomic_store(args->failed, 1);
                return NULL;
            }
            args->lookups++;
        } else if (category < LOOKUP_PERCENT + UPDATE_PERCENT) {
            int key = (int)((sequence * 17U) % INITIAL_KEYS);
            uint64_t begin = monotonic_ns();
            int status = opal_bptree_update(args->tree, key, 1);
            uint64_t end = monotonic_ns();
            if (status != 0 || begin == 0 || end < begin) {
                atomic_store(args->failed, 1);
                return NULL;
            }
            args->update_latency_ns[args->latency_count++] = end - begin;
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

static int compare_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static int parse_int(const char *text, int min, int max, int *result)
{
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || text[0] == '\0' || *end || value < min || value > max) {
        return -1;
    }
    *result = (int)value;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3 || (strcmp(argv[1], "immediate") != 0 &&
                      strcmp(argv[1], "collect1us") != 0)) {
        fprintf(stderr, "Usage: %s <immediate|collect1us> <operations_per_thread>\n", argv[0]);
        return EXIT_FAILURE;
    }
    int operations;
    if (parse_int(argv[2], 100, 1000000, &operations) != 0 || operations % 100) {
        fprintf(stderr, "Operations must be 100-1000000 and divisible by 100.\n");
        return EXIT_FAILURE;
    }
    const int threads_count = 4;
    const long total = (long)threads_count * operations;
    OpalBPlusTree *tree = opal_bptree_create(OPAL_BPTREE_OPERATION_AWARE);
    if (tree == NULL) {
        fprintf(stderr, "Tree initialization failed.\n");
        return EXIT_FAILURE;
    }
    for (int key = 0; key < INITIAL_KEYS; key++) {
        if (opal_bptree_insert(tree, key, INITIAL_VALUE) != 0) {
            fprintf(stderr, "Tree initialization failed at key %d.\n", key);
            opal_bptree_destroy(tree);
            return EXIT_FAILURE;
        }
    }
    pthread_t threads[4];
    WorkerArgs args[4] = {{0}};
    atomic_int start = 0, failed = 0;
    int created = 0;
    struct timespec begin, end;
    if (clock_gettime(CLOCK_MONOTONIC, &begin) != 0) {
        perror("clock_gettime"); opal_bptree_destroy(tree); return EXIT_FAILURE;
    }
    for (int i = 0; i < threads_count; i++) {
        args[i] = (WorkerArgs){.tree=tree, .id=i, .operations=operations,
                               .start=&start, .failed=&failed};
        args[i].update_latency_ns = calloc((size_t)operations, sizeof(uint64_t));
        if (args[i].update_latency_ns == NULL ||
            pthread_create(&threads[i], NULL, worker, &args[i]) != 0) {
            fprintf(stderr, "Worker allocation or creation failed.\n");
            atomic_store(&failed, 1);
            break;
        }
        created++;
    }
    atomic_store_explicit(&start, 1, memory_order_release);
    for (int i = 0; i < created; i++) {
        if (pthread_join(threads[i], NULL) != 0) atomic_store(&failed, 1);
    }
    int timed = created == threads_count && clock_gettime(CLOCK_MONOTONIC, &end) == 0;
    if (!timed) atomic_store(&failed, 1);

    long lookups=0, updates=0, inserts=0, latency_count=0;
    for (int i=0;i<created;i++) { lookups+=args[i].lookups; updates+=args[i].updates; inserts+=args[i].inserts; latency_count+=args[i].latency_count; }
    long expected_lookups=total*LOOKUP_PERCENT/100;
    long expected_updates=total*UPDATE_PERCENT/100;
    long expected_inserts=total*INSERT_PERCENT/100;
    size_t expected_keys=INITIAL_KEYS+(size_t)expected_inserts;
    OpalBPlusTreeStats stats;
    opal_bptree_get_stats(tree,&stats);
    long long sum=0;
    if (!atomic_load(&failed)) {
        for(int key=0;key<INITIAL_KEYS;key++){int value=0,found=0;if(opal_bptree_lookup(tree,key,&value,&found)||!found){atomic_store(&failed,1);break;}sum+=value;}
        for(long sequence=0;sequence<total&&!atomic_load(&failed);sequence++){
            if((sequence%100)<LOOKUP_PERCENT+UPDATE_PERCENT)continue;
            int key=INITIAL_KEYS+(int)sequence,value=0,found=0;
            if(opal_bptree_lookup(tree,key,&value,&found)||!found||value!=INITIAL_VALUE){atomic_store(&failed,1);break;}sum+=value;
        }
    }
    long long expected_sum=(long long)expected_keys*INITIAL_VALUE+expected_updates;
    int pass=!atomic_load(&failed)&&timed&&lookups==expected_lookups&&updates==expected_updates&&
             inserts==expected_inserts&&latency_count==expected_updates&&
             opal_bptree_size(tree)==expected_keys&&sum==expected_sum&&
             stats.update_callbacks==(size_t)expected_updates&&stats.update_batches>0;
    uint64_t *latencies=calloc((size_t)(latency_count?latency_count:1),sizeof(*latencies));
    if(latencies==NULL)pass=0;
    long position=0;
    for(int i=0;i<created;i++)for(int j=0;j<args[i].latency_count;j++)latencies[position++]=args[i].update_latency_ns[j];
    uint64_t median_ns=0,p95_ns=0;
    if(pass){qsort(latencies,(size_t)latency_count,sizeof(*latencies),compare_u64);median_ns=latencies[(latency_count-1)/2];p95_ns=latencies[((latency_count*95+99)/100)-1];}
    double elapsed=timed?elapsed_seconds(begin,end):0.0;
    double throughput=elapsed>0?total/elapsed:0.0;
    printf("Policy: %s\nThreads: %d\nOperations per thread: %d\n",argv[1],threads_count,operations);
    printf("Lookup operations: %ld\nUpdate operations: %ld\nInsert operations: %ld\n",lookups,updates,inserts);
    printf("Update callbacks: %zu\nUpdate batches: %zu\nSingleton update batches: %zu\n",stats.update_callbacks,stats.update_batches,stats.singleton_update_batches);
    printf("Maximum update batch size: %zu\n",stats.maximum_update_batch_size);
    printf("Update batch size buckets (1,2,3-4,5-8,9+): %zu,%zu,%zu,%zu,%zu\n",stats.update_batch_size_buckets[0],stats.update_batch_size_buckets[1],stats.update_batch_size_buckets[2],stats.update_batch_size_buckets[3],stats.update_batch_size_buckets[4]);
    printf("Update latency median ns: %llu\nUpdate latency p95 ns: %llu\n",(unsigned long long)median_ns,(unsigned long long)p95_ns);
    printf("Elapsed seconds: %.9f\nThroughput ops/sec: %.2f\nRESULT: %s\n",elapsed,throughput,pass?"PASS":"FAIL");
    free(latencies);for(int i=0;i<4;i++)free(args[i].update_latency_ns);opal_bptree_destroy(tree);
    return pass?EXIT_SUCCESS:EXIT_FAILURE;
}
