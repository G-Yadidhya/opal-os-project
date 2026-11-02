#include <stdio.h>
#include <pthread.h>

#define ITERATIONS 1000000
#define THREAD_COUNT 4

int counter = 0;
pthread_mutex_t counter_mutex = PTHREAD_MUTEX_INITIALIZER;

void *increment_counter(void *arg) {
    (void)arg;

    for (int i = 0; i < ITERATIONS; i++) {
        pthread_mutex_lock(&counter_mutex);

        counter++;

        pthread_mutex_unlock(&counter_mutex);
    }

    return NULL;
}

int main(void) {
    pthread_t threads[THREAD_COUNT];
    int created = 0;

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

    printf("Expected counter: %d\n",
           THREAD_COUNT * ITERATIONS);
    printf("Actual counter:   %d\n", counter);

    if (pthread_mutex_destroy(&counter_mutex) != 0) {
        fprintf(stderr, "Failed to destroy mutex\n");
        return 1;
    }

    return created == THREAD_COUNT ? 0 : 1;
}
