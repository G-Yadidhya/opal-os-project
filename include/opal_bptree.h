#ifndef OPAL_BPTREE_H
#define OPAL_BPTREE_H

#include <stddef.h>

typedef struct OpalBPlusTree OpalBPlusTree;

typedef enum {
    OPAL_BPTREE_MUTEX_BASELINE = 0,
    OPAL_BPTREE_OPERATION_AWARE = 1
} OpalBPlusTreeMode;

typedef struct {
    size_t optimistic_retries;
    size_t update_callbacks;
    size_t update_batches;
    size_t singleton_update_batches;
    size_t maximum_update_batch_size;
    size_t update_batch_size_buckets[5]; /* 1, 2, 3-4, 5-8, 9+ callbacks */
    size_t structural_inserts;
} OpalBPlusTreeStats;

OpalBPlusTree *opal_bptree_create(OpalBPlusTreeMode mode);
void opal_bptree_destroy(OpalBPlusTree *tree);

/* Insert a new key. Returns 0 on insert, 1 if the key already exists, -1 on error. */
int opal_bptree_insert(OpalBPlusTree *tree, int key, int value);

/* Increment the value of an existing key through the update path. */
int opal_bptree_update(OpalBPlusTree *tree, int key, int delta);

/* Read a key. Sets found to 0 or 1; retries are internal to OPAL lookups. */
int opal_bptree_lookup(OpalBPlusTree *tree, int key, int *value, int *found);

size_t opal_bptree_size(OpalBPlusTree *tree);
void opal_bptree_get_stats(OpalBPlusTree *tree, OpalBPlusTreeStats *stats);

#endif
