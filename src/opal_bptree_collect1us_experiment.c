#define _POSIX_C_SOURCE 200809L

#include "opal_bptree.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>
#include <time.h>

#define NODE_MAX_KEYS 3
#define BATCH_COLLECTION_WINDOW_NS 1000L

typedef struct BPlusNode {
    atomic_int is_leaf;
    atomic_int key_count;
    atomic_int keys[NODE_MAX_KEYS + 1];
    atomic_int values[NODE_MAX_KEYS + 1];
    _Atomic(struct BPlusNode *) children[NODE_MAX_KEYS + 2];
    _Atomic(struct BPlusNode *) next;
} BPlusNode;

typedef struct MCSNode {
    _Atomic(struct MCSNode *) next;
    atomic_int waiting;
} MCSNode;

typedef struct UpdateRequest UpdateRequest;
struct OpalBPlusTree;
typedef int (*UpdateFunction)(struct OpalBPlusTree *, UpdateRequest *);

struct UpdateRequest {
    UpdateFunction function;
    int key;
    int delta;
    int result;
    atomic_int done;
    UpdateRequest *next;
};

struct OpalBPlusTree {
    OpalBPlusTreeMode mode;
    _Atomic(BPlusNode *) root;
    atomic_size_t size;
    atomic_uint version;
    pthread_mutex_t baseline_mutex;
    _Atomic(MCSNode *) mcs_tail;

    pthread_mutex_t batch_mutex;
    UpdateRequest *batch_head;
    UpdateRequest *batch_tail;
    int batch_active;

    atomic_size_t optimistic_retries;
    atomic_size_t update_callbacks;
    atomic_size_t update_batches;
    atomic_size_t singleton_update_batches;
    atomic_size_t maximum_update_batch_size;
    atomic_size_t update_batch_size_buckets[5];
    atomic_size_t structural_inserts;
};

typedef struct {
    int split;
    int separator;
    BPlusNode *right;
    int inserted;
} InsertResult;

static BPlusNode *node_create(int is_leaf)
{
    BPlusNode *node = calloc(1, sizeof(*node));
    if (node == NULL) {
        return NULL;
    }
    atomic_init(&node->is_leaf, is_leaf);
    atomic_init(&node->key_count, 0);
    for (int i = 0; i < NODE_MAX_KEYS + 1; i++) {
        atomic_init(&node->keys[i], 0);
        atomic_init(&node->values[i], 0);
    }
    for (int i = 0; i < NODE_MAX_KEYS + 2; i++) {
        atomic_init(&node->children[i], NULL);
    }
    atomic_init(&node->next, NULL);
    return node;
}

static void node_destroy(BPlusNode *node)
{
    if (node == NULL) {
        return;
    }
    if (!atomic_load_explicit(&node->is_leaf, memory_order_relaxed)) {
        int count = atomic_load_explicit(&node->key_count, memory_order_relaxed);
        for (int i = 0; i <= count; i++) {
            node_destroy(atomic_load_explicit(&node->children[i],
                                               memory_order_relaxed));
        }
    }
    free(node);
}

static int lower_bound(const BPlusNode *node, int key)
{
    int count = atomic_load_explicit(&node->key_count, memory_order_seq_cst);
    int i = 0;
    while (i < count && atomic_load_explicit(&node->keys[i],
                                              memory_order_seq_cst) < key) {
        i++;
    }
    return i;
}

static void mcs_lock(OpalBPlusTree *tree, MCSNode *mine)
{
    atomic_init(&mine->next, NULL);
    atomic_init(&mine->waiting, 1);
    MCSNode *predecessor = atomic_exchange_explicit(
        &tree->mcs_tail, mine, memory_order_seq_cst);
    if (predecessor == NULL) {
        atomic_store_explicit(&mine->waiting, 0, memory_order_seq_cst);
        return;
    }
    atomic_store_explicit(&predecessor->next, mine, memory_order_seq_cst);
    while (atomic_load_explicit(&mine->waiting, memory_order_seq_cst) != 0) {
    }
}

static void mcs_unlock(OpalBPlusTree *tree, MCSNode *mine)
{
    MCSNode *successor = atomic_load_explicit(&mine->next, memory_order_seq_cst);
    if (successor == NULL) {
        MCSNode *expected = mine;
        if (atomic_compare_exchange_strong_explicit(
                &tree->mcs_tail, &expected, NULL,
                memory_order_seq_cst, memory_order_seq_cst)) {
            return;
        }
        do {
            successor = atomic_load_explicit(&mine->next, memory_order_seq_cst);
        } while (successor == NULL);
    }
    atomic_store_explicit(&successor->waiting, 0, memory_order_seq_cst);
}

static int begin_writer(OpalBPlusTree *tree, MCSNode *mcs_node)
{
    if (tree->mode == OPAL_BPTREE_MUTEX_BASELINE) {
        if (pthread_mutex_lock(&tree->baseline_mutex) != 0) {
            return -1;
        }
    } else {
        mcs_lock(tree, mcs_node);
    }
    atomic_fetch_add_explicit(&tree->version, 1, memory_order_seq_cst);
    return 0;
}

static int end_writer(OpalBPlusTree *tree, MCSNode *mcs_node)
{
    atomic_fetch_add_explicit(&tree->version, 1, memory_order_seq_cst);
    if (tree->mode == OPAL_BPTREE_MUTEX_BASELINE) {
        return pthread_mutex_unlock(&tree->baseline_mutex) == 0 ? 0 : -1;
    }
    mcs_unlock(tree, mcs_node);
    return 0;
}

static int find_locked(BPlusNode *root, int key, int *value)
{
    BPlusNode *node = root;
    while (node != NULL &&
           atomic_load_explicit(&node->is_leaf, memory_order_seq_cst) == 0) {
        int count = atomic_load_explicit(&node->key_count, memory_order_seq_cst);
        int child = 0;
        while (child < count && key >= atomic_load_explicit(
                   &node->keys[child], memory_order_seq_cst)) {
            child++;
        }
        node = atomic_load_explicit(&node->children[child], memory_order_seq_cst);
    }
    if (node == NULL) {
        return 0;
    }
    int index = lower_bound(node, key);
    int count = atomic_load_explicit(&node->key_count, memory_order_seq_cst);
    if (index < count && atomic_load_explicit(&node->keys[index],
                                              memory_order_seq_cst) == key) {
        *value = atomic_load_explicit(&node->values[index], memory_order_seq_cst);
        return 1;
    }
    return 0;
}

static int insert_recursive(BPlusNode *node, int key, int value,
                            InsertResult *result)
{
    result->split = 0;
    result->right = NULL;
    result->inserted = 0;

    int is_leaf = atomic_load_explicit(&node->is_leaf, memory_order_seq_cst);
    int count = atomic_load_explicit(&node->key_count, memory_order_seq_cst);
    if (is_leaf) {
        int index = lower_bound(node, key);
        if (index < count && atomic_load_explicit(&node->keys[index],
                                                 memory_order_seq_cst) == key) {
            return 1;
        }
        BPlusNode *right = count == NODE_MAX_KEYS ? node_create(1) : NULL;
        if (count == NODE_MAX_KEYS && right == NULL) {
            return -1;
        }
        for (int i = count; i > index; i--) {
            atomic_store_explicit(&node->keys[i], atomic_load_explicit(
                &node->keys[i - 1], memory_order_seq_cst), memory_order_seq_cst);
            atomic_store_explicit(&node->values[i], atomic_load_explicit(
                &node->values[i - 1], memory_order_seq_cst), memory_order_seq_cst);
        }
        atomic_store_explicit(&node->keys[index], key, memory_order_seq_cst);
        atomic_store_explicit(&node->values[index], value, memory_order_seq_cst);
        atomic_store_explicit(&node->key_count, count + 1, memory_order_seq_cst);
        result->inserted = 1;
        if (right == NULL) {
            return 0;
        }
        int middle = (count + 1) / 2;
        int right_count = count + 1 - middle;
        for (int i = 0; i < right_count; i++) {
            atomic_store_explicit(&right->keys[i], atomic_load_explicit(
                &node->keys[middle + i], memory_order_seq_cst), memory_order_seq_cst);
            atomic_store_explicit(&right->values[i], atomic_load_explicit(
                &node->values[middle + i], memory_order_seq_cst), memory_order_seq_cst);
        }
        atomic_store_explicit(&right->key_count, right_count, memory_order_seq_cst);
        atomic_store_explicit(&right->next, atomic_load_explicit(
            &node->next, memory_order_seq_cst), memory_order_seq_cst);
        atomic_store_explicit(&node->next, right, memory_order_seq_cst);
        atomic_store_explicit(&node->key_count, middle, memory_order_seq_cst);
        result->split = 1;
        result->separator = atomic_load_explicit(&right->keys[0], memory_order_seq_cst);
        result->right = right;
        return 0;
    }

    int child_index = 0;
    while (child_index < count && key >= atomic_load_explicit(
               &node->keys[child_index], memory_order_seq_cst)) {
        child_index++;
    }
    BPlusNode *reserved_right = count == NODE_MAX_KEYS ? node_create(0) : NULL;
    if (count == NODE_MAX_KEYS && reserved_right == NULL) {
        return -1;
    }
    BPlusNode *child = atomic_load_explicit(&node->children[child_index],
                                            memory_order_seq_cst);
    if (child == NULL) {
        free(reserved_right);
        return -1;
    }
    InsertResult child_result;
    int status = insert_recursive(child, key, value, &child_result);
    if (status != 0) {
        free(reserved_right);
        return status;
    }
    result->inserted = child_result.inserted;
    if (!child_result.split) {
        free(reserved_right);
        return 0;
    }
    for (int i = count; i > child_index; i--) {
        atomic_store_explicit(&node->keys[i], atomic_load_explicit(
            &node->keys[i - 1], memory_order_seq_cst), memory_order_seq_cst);
    }
    for (int i = count + 1; i > child_index + 1; i--) {
        atomic_store_explicit(&node->children[i], atomic_load_explicit(
            &node->children[i - 1], memory_order_seq_cst), memory_order_seq_cst);
    }
    atomic_store_explicit(&node->keys[child_index], child_result.separator,
                          memory_order_seq_cst);
    atomic_store_explicit(&node->children[child_index + 1], child_result.right,
                          memory_order_seq_cst);
    atomic_store_explicit(&node->key_count, count + 1, memory_order_seq_cst);
    if (count + 1 <= NODE_MAX_KEYS) {
        return 0;
    }

    int promoted_index = (count + 1) / 2;
    int right_count = count + 1 - promoted_index - 1;
    for (int i = 0; i < right_count; i++) {
        atomic_store_explicit(&reserved_right->keys[i], atomic_load_explicit(
            &node->keys[promoted_index + 1 + i], memory_order_seq_cst), memory_order_seq_cst);
    }
    for (int i = 0; i <= right_count; i++) {
        atomic_store_explicit(&reserved_right->children[i], atomic_load_explicit(
            &node->children[promoted_index + 1 + i], memory_order_seq_cst), memory_order_seq_cst);
    }
    atomic_store_explicit(&reserved_right->key_count, right_count, memory_order_seq_cst);
    result->separator = atomic_load_explicit(&node->keys[promoted_index],
                                              memory_order_seq_cst);
    result->right = reserved_right;
    result->split = 1;
    atomic_store_explicit(&node->key_count, promoted_index, memory_order_seq_cst);
    return 0;
}

static int insert_under_writer(OpalBPlusTree *tree, int key, int value)
{
    BPlusNode *root = atomic_load_explicit(&tree->root, memory_order_seq_cst);
    BPlusNode *reserved_root = NULL;
    if (atomic_load_explicit(&root->key_count, memory_order_seq_cst) == NODE_MAX_KEYS) {
        reserved_root = node_create(0);
        if (reserved_root == NULL) {
            return -1;
        }
    }
    InsertResult result;
    int status = insert_recursive(root, key, value, &result);
    if (status == 0 && result.inserted) {
        atomic_fetch_add_explicit(&tree->size, 1, memory_order_seq_cst);
    }
    if (status == 0 && result.split) {
        atomic_store_explicit(&reserved_root->keys[0], result.separator,
                              memory_order_seq_cst);
        atomic_store_explicit(&reserved_root->children[0], root, memory_order_seq_cst);
        atomic_store_explicit(&reserved_root->children[1], result.right,
                              memory_order_seq_cst);
        atomic_store_explicit(&reserved_root->key_count, 1, memory_order_seq_cst);
        atomic_store_explicit(&tree->root, reserved_root, memory_order_seq_cst);
        reserved_root = NULL;
    }
    free(reserved_root);
    return status;
}

static int update_callback(struct OpalBPlusTree *tree, UpdateRequest *request)
{
    int value = 0;
    if (!find_locked(atomic_load_explicit(&tree->root, memory_order_seq_cst),
                     request->key, &value)) {
        return -1;
    }
    BPlusNode *node = atomic_load_explicit(&tree->root, memory_order_seq_cst);
    while (!atomic_load_explicit(&node->is_leaf, memory_order_seq_cst)) {
        int count = atomic_load_explicit(&node->key_count, memory_order_seq_cst);
        int child = 0;
        while (child < count && request->key >= atomic_load_explicit(
                   &node->keys[child], memory_order_seq_cst)) {
            child++;
        }
        node = atomic_load_explicit(&node->children[child], memory_order_seq_cst);
    }
    int index = lower_bound(node, request->key);
    if ((request->delta > 0 && value > INT_MAX - request->delta) ||
        (request->delta < 0 && value < INT_MIN - request->delta)) {
        return -1;
    }
    atomic_store_explicit(&node->values[index], value + request->delta,
                          memory_order_seq_cst);
    atomic_fetch_add_explicit(&tree->update_callbacks, 1, memory_order_seq_cst);
    return 0;
}

static int run_baseline_update(OpalBPlusTree *tree, UpdateRequest *request)
{
    MCSNode unused;
    if (begin_writer(tree, &unused) != 0) {
        return -1;
    }
    int result = request->function(tree, request);
    int unlock_status = end_writer(tree, &unused);
    return unlock_status == 0 ? result : -1;
}

static void collect_updates_for_bounded_window(void)
{
    struct timespec start, now;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        return;
    }
    long elapsed;
    do {
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            return;
        }
        elapsed = (long)(now.tv_sec - start.tv_sec) * 1000000000L +
                 now.tv_nsec - start.tv_nsec;
    } while (elapsed < BATCH_COLLECTION_WINDOW_NS);
}

static int enqueue_batched_update(OpalBPlusTree *tree, UpdateRequest *request)
{
    request->next = NULL;
    atomic_init(&request->done, 0);
    int owner = 0;
    if (pthread_mutex_lock(&tree->batch_mutex) != 0) {
        return -1;
    }
    if (tree->batch_tail == NULL) {
        tree->batch_head = request;
    } else {
        tree->batch_tail->next = request;
    }
    tree->batch_tail = request;
    if (!tree->batch_active) {
        tree->batch_active = 1;
        owner = 1;
    }
    pthread_mutex_unlock(&tree->batch_mutex);

    if (!owner) {
        while (atomic_load_explicit(&request->done, memory_order_seq_cst) == 0) {
        }
        return request->result;
    }

    for (;;) {
        if (pthread_mutex_lock(&tree->batch_mutex) != 0) {
            return -1;
        }
        if (tree->batch_head == NULL) {
            tree->batch_active = 0;
            pthread_mutex_unlock(&tree->batch_mutex);
            break;
        }
        pthread_mutex_unlock(&tree->batch_mutex);

        /* Wait only while work is queued; then coalesce arrivals before detach. */
        collect_updates_for_bounded_window();

        if (pthread_mutex_lock(&tree->batch_mutex) != 0) {
            return -1;
        }
        UpdateRequest *batch = tree->batch_head;
        tree->batch_head = NULL;
        tree->batch_tail = NULL;
        pthread_mutex_unlock(&tree->batch_mutex);

        MCSNode mcs_node;
        if (begin_writer(tree, &mcs_node) != 0) {
            for (UpdateRequest *it = batch; it != NULL;) {
                UpdateRequest *next = it->next;
                it->result = -1;
                atomic_store_explicit(&it->done, 1, memory_order_seq_cst);
                it = next;
            }
            return -1;
        }
        atomic_fetch_add_explicit(&tree->update_batches, 1, memory_order_seq_cst);
        size_t batch_size = 0;
        for (UpdateRequest *it = batch; it != NULL; it = it->next) {
            it->result = it->function(tree, it);
            batch_size++;
        }
        size_t bucket = batch_size == 1 ? 0 : batch_size == 2 ? 1 :
                        batch_size <= 4 ? 2 : batch_size <= 8 ? 3 : 4;
        atomic_fetch_add_explicit(&tree->update_batch_size_buckets[bucket], 1,
                                  memory_order_seq_cst);
        if (batch_size == 1) {
            atomic_fetch_add_explicit(&tree->singleton_update_batches, 1,
                                      memory_order_seq_cst);
        }
        size_t previous_max = atomic_load_explicit(&tree->maximum_update_batch_size,
                                                   memory_order_seq_cst);
        while (batch_size > previous_max &&
               !atomic_compare_exchange_weak_explicit(
                   &tree->maximum_update_batch_size, &previous_max, batch_size,
                   memory_order_seq_cst, memory_order_seq_cst)) {
        }
        end_writer(tree, &mcs_node);
        for (UpdateRequest *it = batch; it != NULL;) {
            UpdateRequest *next = it->next;
            atomic_store_explicit(&it->done, 1, memory_order_seq_cst);
            it = next;
        }
    }
    while (atomic_load_explicit(&request->done, memory_order_seq_cst) == 0) {
    }
    return request->result;
}

OpalBPlusTree *opal_bptree_create(OpalBPlusTreeMode mode)
{
    if (mode != OPAL_BPTREE_MUTEX_BASELINE && mode != OPAL_BPTREE_OPERATION_AWARE) {
        return NULL;
    }
    OpalBPlusTree *tree = calloc(1, sizeof(*tree));
    if (tree == NULL) {
        return NULL;
    }
    BPlusNode *root = node_create(1);
    if (root == NULL) {
        free(tree);
        return NULL;
    }
    tree->mode = mode;
    atomic_init(&tree->root, root);
    atomic_init(&tree->size, 0);
    atomic_init(&tree->version, 0);
    atomic_init(&tree->mcs_tail, NULL);
    atomic_init(&tree->optimistic_retries, 0);
    atomic_init(&tree->update_callbacks, 0);
    atomic_init(&tree->update_batches, 0);
    atomic_init(&tree->singleton_update_batches, 0);
    atomic_init(&tree->maximum_update_batch_size, 0);
    for (int i = 0; i < 5; i++) {
        atomic_init(&tree->update_batch_size_buckets[i], 0);
    }
    atomic_init(&tree->structural_inserts, 0);
    if (pthread_mutex_init(&tree->baseline_mutex, NULL) != 0) {
        node_destroy(root);
        free(tree);
        return NULL;
    }
    if (pthread_mutex_init(&tree->batch_mutex, NULL) != 0) {
        pthread_mutex_destroy(&tree->baseline_mutex);
        node_destroy(root);
        free(tree);
        return NULL;
    }
    return tree;
}

void opal_bptree_destroy(OpalBPlusTree *tree)
{
    if (tree == NULL) {
        return;
    }
    node_destroy(atomic_load_explicit(&tree->root, memory_order_seq_cst));
    pthread_mutex_destroy(&tree->baseline_mutex);
    pthread_mutex_destroy(&tree->batch_mutex);
    free(tree);
}

int opal_bptree_insert(OpalBPlusTree *tree, int key, int value)
{
    if (tree == NULL) {
        return -1;
    }
    MCSNode mcs_node;
    if (begin_writer(tree, &mcs_node) != 0) {
        return -1;
    }
    int status = insert_under_writer(tree, key, value);
    if (status == 0) {
        atomic_fetch_add_explicit(&tree->structural_inserts, 1, memory_order_seq_cst);
    }
    int unlock_status = end_writer(tree, &mcs_node);
    return unlock_status == 0 ? status : -1;
}

int opal_bptree_update(OpalBPlusTree *tree, int key, int delta)
{
    if (tree == NULL) {
        return -1;
    }
    UpdateRequest request = {0};
    request.function = update_callback;
    request.key = key;
    request.delta = delta;
    if (tree->mode == OPAL_BPTREE_MUTEX_BASELINE) {
        return run_baseline_update(tree, &request);
    }
    return enqueue_batched_update(tree, &request);
}

int opal_bptree_lookup(OpalBPlusTree *tree, int key, int *value, int *found)
{
    if (tree == NULL || value == NULL || found == NULL) {
        return -1;
    }
    if (tree->mode == OPAL_BPTREE_MUTEX_BASELINE) {
        if (pthread_mutex_lock(&tree->baseline_mutex) != 0) {
            return -1;
        }
        *found = find_locked(atomic_load_explicit(&tree->root, memory_order_seq_cst),
                             key, value);
        return pthread_mutex_unlock(&tree->baseline_mutex) == 0 ? 0 : -1;
    }

    for (;;) {
        unsigned before = atomic_load_explicit(&tree->version, memory_order_seq_cst);
        if (before & 1U) {
            atomic_fetch_add_explicit(&tree->optimistic_retries, 1,
                                      memory_order_seq_cst);
            continue;
        }
        int local_value = 0;
        int local_found = find_locked(
            atomic_load_explicit(&tree->root, memory_order_seq_cst), key,
            &local_value);
        unsigned after = atomic_load_explicit(&tree->version, memory_order_seq_cst);
        if (before == after && !(after & 1U)) {
            *found = local_found;
            if (local_found) {
                *value = local_value;
            }
            return 0;
        }
        atomic_fetch_add_explicit(&tree->optimistic_retries, 1,
                                  memory_order_seq_cst);
    }
}

size_t opal_bptree_size(OpalBPlusTree *tree)
{
    if (tree == NULL) {
        return 0;
    }
    return atomic_load_explicit(&tree->size, memory_order_seq_cst);
}

void opal_bptree_get_stats(OpalBPlusTree *tree, OpalBPlusTreeStats *stats)
{
    if (tree == NULL || stats == NULL) {
        return;
    }
    stats->optimistic_retries = atomic_load_explicit(
        &tree->optimistic_retries, memory_order_seq_cst);
    stats->update_callbacks = atomic_load_explicit(
        &tree->update_callbacks, memory_order_seq_cst);
    stats->update_batches = atomic_load_explicit(
        &tree->update_batches, memory_order_seq_cst);
    stats->singleton_update_batches = atomic_load_explicit(
        &tree->singleton_update_batches, memory_order_seq_cst);
    stats->maximum_update_batch_size = atomic_load_explicit(
        &tree->maximum_update_batch_size, memory_order_seq_cst);
    for (int i = 0; i < 5; i++) {
        stats->update_batch_size_buckets[i] = atomic_load_explicit(
            &tree->update_batch_size_buckets[i], memory_order_seq_cst);
    }
    stats->structural_inserts = atomic_load_explicit(
        &tree->structural_inserts, memory_order_seq_cst);
}
