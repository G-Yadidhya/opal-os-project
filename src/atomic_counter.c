#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>

#define ITERATIONS 1000000
#define THREAD_COUNT 4

atomic_int counter;

void *increment_counter(void *arg) {
    (void)arg;

    for (int i = 0; i < ITERATIONS; i++) {
        atomic_fetch_add_explicit(
            &counter, 1, memory_order_relaxed
        );
    }

    return NULL;
}

int main(void) {
    pthread_t threads[THREAD_COUNT];
    int created = 0;

    atomic_init(&counter, 0);

    for (int i = 0; i < THREAD_COUNT; i++) {
        if (pthread_create(&threads[i], NULL,
                           increment_counter, NULL) != 0) {
            fprintf(stderr, "Failed to create thread\n");
            break;
        }
        created++;
    }

    for (int i = 0; i < created; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            fprintf(stderr, "Failed to join thread\n");
            return 1;
        }
    }

    int actual = atomic_load_explicit(
        &counter, memory_order_relaxed
    );

    printf("Expected counter: %d\n",
           THREAD_COUNT * ITERATIONS);
    printf("Actual counter:   %d\n", actual);

    return created == THREAD_COUNT ? 0 : 1;
}
