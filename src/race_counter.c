#include <stdio.h>
#include <pthread.h>

#define ITERATIONS 1000000
#define THREAD_COUNT 4

int counter = 0;

void *increment_counter(void *arg) {
    (void)arg;

    for (int i = 0; i < ITERATIONS; i++) {
        counter++;
    }

    return NULL;
}

int main(void) {
    pthread_t threads[THREAD_COUNT];

    for (int i = 0; i < THREAD_COUNT; i++) {
        if (pthread_create(&threads[i], NULL,
                           increment_counter, NULL) != 0) {
            fprintf(stderr, "Failed to create thread\n");
            return 1;
        }
    }

    for (int i = 0; i < THREAD_COUNT; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            fprintf(stderr, "Failed to join thread\n");
            return 1;
        }
    }

    printf("Expected counter: %d\n",
           THREAD_COUNT * ITERATIONS);
    printf("Actual counter:   %d\n", counter);

    return 0;
}
