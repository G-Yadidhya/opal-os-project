#ifndef BPTREE_H
#define BPTREE_H

#include <stddef.h>

typedef struct BPlusTree BPlusTree;

typedef enum {
    BPTREE_LOCK_MUTEX = 0,
    BPTREE_LOCK_RWLOCK = 1
} BPlusTreeLockMode;

/* The tree owns its nodes. All operations are safe to call concurrently. */
BPlusTree *bptree_create(void);
BPlusTree *bptree_create_with_lock(BPlusTreeLockMode mode);
void bptree_destroy(BPlusTree *tree);

/* Insert or replace an integer key/value pair. Returns 0 on success. */
int bptree_insert(BPlusTree *tree, int key, int value);

/* Returns 0 on success and sets found to 0 or 1. */
int bptree_find(BPlusTree *tree, int key, int *value, int *found);

/* Returns the number of distinct keys, or 0 for a null tree/error. */
size_t bptree_size(BPlusTree *tree);

#endif
