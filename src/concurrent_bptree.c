#define _POSIX_C_SOURCE 200809L

#include "bptree.h"

#include <pthread.h>
#include <stdlib.h>

#define BPTREE_MAX_KEYS 3

typedef struct BPlusNode {
    int is_leaf;
    int key_count;
    int keys[BPTREE_MAX_KEYS + 1];
    int values[BPTREE_MAX_KEYS + 1];
    struct BPlusNode *children[BPTREE_MAX_KEYS + 2];
    struct BPlusNode *next;
} BPlusNode;

struct BPlusTree {
    BPlusNode *root;
    size_t size;
    BPlusTreeLockMode lock_mode;
    union {
        pthread_mutex_t mutex;
        pthread_rwlock_t rwlock;
    } lock;
};

typedef struct {
    int split;
    int separator;
    BPlusNode *right;
} InsertResult;

static BPlusNode *node_create(int is_leaf)
{
    BPlusNode *node = calloc(1, sizeof(*node));
    if (node != NULL) {
        node->is_leaf = is_leaf;
    }
    return node;
}

static void node_destroy(BPlusNode *node)
{
    if (node == NULL) {
        return;
    }
    if (!node->is_leaf) {
        for (int i = 0; i <= node->key_count; i++) {
            node_destroy(node->children[i]);
        }
    }
    free(node);
}

static int leaf_lower_bound(const BPlusNode *node, int key)
{
    int index = 0;
    while (index < node->key_count && node->keys[index] < key) {
        index++;
    }
    return index;
}

static int tree_lock(BPlusTree *tree, int write)
{
    if (tree->lock_mode == BPTREE_LOCK_MUTEX) {
        return pthread_mutex_lock(&tree->lock.mutex);
    }
    return write ? pthread_rwlock_wrlock(&tree->lock.rwlock)
                 : pthread_rwlock_rdlock(&tree->lock.rwlock);
}

static int tree_unlock(BPlusTree *tree)
{
    if (tree->lock_mode == BPTREE_LOCK_MUTEX) {
        return pthread_mutex_unlock(&tree->lock.mutex);
    }
    return pthread_rwlock_unlock(&tree->lock.rwlock);
}

static int insert_recursive(BPlusNode *node, int key, int value,
                            InsertResult *result)
{
    result->split = 0;
    result->right = NULL;

    if (node->is_leaf) {
        int index = leaf_lower_bound(node, key);
        if (index < node->key_count && node->keys[index] == key) {
            node->values[index] = value;
            return 0;
        }

        BPlusNode *right = NULL;
        if (node->key_count == BPTREE_MAX_KEYS) {
            right = node_create(1);
            if (right == NULL) {
                return -1;
            }
        }

        for (int i = node->key_count; i > index; i--) {
            node->keys[i] = node->keys[i - 1];
            node->values[i] = node->values[i - 1];
        }
        node->keys[index] = key;
        node->values[index] = value;
        node->key_count++;

        if (right == NULL) {
            return 0;
        }

        int middle = node->key_count / 2;
        right->key_count = node->key_count - middle;
        for (int i = 0; i < right->key_count; i++) {
            right->keys[i] = node->keys[middle + i];
            right->values[i] = node->values[middle + i];
        }
        node->key_count = middle;
        right->next = node->next;
        node->next = right;

        result->split = 1;
        result->separator = right->keys[0];
        result->right = right;
        return 0;
    }

    int child_index = 0;
    while (child_index < node->key_count && key >= node->keys[child_index]) {
        child_index++;
    }

    BPlusNode *reserved_right = NULL;
    if (node->key_count == BPTREE_MAX_KEYS) {
        reserved_right = node_create(0);
        if (reserved_right == NULL) {
            return -1;
        }
    }

    InsertResult child_result;
    int status = insert_recursive(node->children[child_index], key, value,
                                  &child_result);
    if (status != 0) {
        free(reserved_right);
        return status;
    }
    if (!child_result.split) {
        free(reserved_right);
        return 0;
    }

    for (int i = node->key_count; i > child_index; i--) {
        node->keys[i] = node->keys[i - 1];
    }
    for (int i = node->key_count + 1; i > child_index + 1; i--) {
        node->children[i] = node->children[i - 1];
    }
    node->keys[child_index] = child_result.separator;
    node->children[child_index + 1] = child_result.right;
    node->key_count++;

    if (node->key_count <= BPTREE_MAX_KEYS) {
        return 0;
    }

    int promoted_index = node->key_count / 2;
    reserved_right->key_count = node->key_count - promoted_index - 1;
    for (int i = 0; i < reserved_right->key_count; i++) {
        reserved_right->keys[i] = node->keys[promoted_index + 1 + i];
    }
    for (int i = 0; i <= reserved_right->key_count; i++) {
        reserved_right->children[i] =
            node->children[promoted_index + 1 + i];
    }

    result->split = 1;
    result->separator = node->keys[promoted_index];
    result->right = reserved_right;
    node->key_count = promoted_index;
    return 0;
}

BPlusTree *bptree_create(void)
{
    return bptree_create_with_lock(BPTREE_LOCK_MUTEX);
}

BPlusTree *bptree_create_with_lock(BPlusTreeLockMode mode)
{
    if (mode != BPTREE_LOCK_MUTEX && mode != BPTREE_LOCK_RWLOCK) {
        return NULL;
    }
    BPlusTree *tree = calloc(1, sizeof(*tree));
    if (tree == NULL) {
        return NULL;
    }

    tree->root = node_create(1);
    if (tree->root == NULL) {
        free(tree);
        return NULL;
    }

    tree->lock_mode = mode;
    int rc = mode == BPTREE_LOCK_MUTEX
        ? pthread_mutex_init(&tree->lock.mutex, NULL)
        : pthread_rwlock_init(&tree->lock.rwlock, NULL);
    if (rc != 0) {
        node_destroy(tree->root);
        free(tree);
        return NULL;
    }
    return tree;
}

void bptree_destroy(BPlusTree *tree)
{
    if (tree == NULL) {
        return;
    }
    node_destroy(tree->root);
    if (tree->lock_mode == BPTREE_LOCK_MUTEX) {
        pthread_mutex_destroy(&tree->lock.mutex);
    } else {
        pthread_rwlock_destroy(&tree->lock.rwlock);
    }
    free(tree);
}

int bptree_insert(BPlusTree *tree, int key, int value)
{
    if (tree == NULL) {
        return -1;
    }
    if (tree_lock(tree, 1) != 0) {
        return -1;
    }

    BPlusNode *leaf = tree->root;
    while (!leaf->is_leaf) {
        int child_index = 0;
        while (child_index < leaf->key_count && key >= leaf->keys[child_index]) {
            child_index++;
        }
        leaf = leaf->children[child_index];
    }
    int found_index = leaf_lower_bound(leaf, key);
    int existed = found_index < leaf->key_count && leaf->keys[found_index] == key;

    BPlusNode *reserved_root = NULL;
    if (tree->root->key_count == BPTREE_MAX_KEYS) {
        reserved_root = node_create(0);
        if (reserved_root == NULL) {
            tree_unlock(tree);
            return -1;
        }
    }

    InsertResult result;
    int status = insert_recursive(tree->root, key, value, &result);
    if (status == 0 && result.split) {
        reserved_root->keys[0] = result.separator;
        reserved_root->key_count = 1;
        reserved_root->children[0] = tree->root;
        reserved_root->children[1] = result.right;
        tree->root = reserved_root;
        reserved_root = NULL;
    }
    free(reserved_root);
    if (status == 0 && !existed) {
        tree->size++;
    }

    int unlock_status = tree_unlock(tree);
    return status == 0 && unlock_status == 0 ? 0 : -1;
}

int bptree_find(BPlusTree *tree, int key, int *value, int *found)
{
    if (tree == NULL || value == NULL || found == NULL) {
        return -1;
    }
    if (tree_lock(tree, 0) != 0) {
        return -1;
    }

    BPlusNode *node = tree->root;
    while (!node->is_leaf) {
        int child_index = 0;
        while (child_index < node->key_count && key >= node->keys[child_index]) {
            child_index++;
        }
        node = node->children[child_index];
    }

    int index = leaf_lower_bound(node, key);
    *found = index < node->key_count && node->keys[index] == key;
    if (*found) {
        *value = node->values[index];
    }

    int rc = tree_unlock(tree);
    return rc == 0 ? 0 : -1;
}

size_t bptree_size(BPlusTree *tree)
{
    if (tree == NULL || tree_lock(tree, 0) != 0) {
        return 0;
    }
    size_t size = tree->size;
    tree_unlock(tree);
    return size;
}
