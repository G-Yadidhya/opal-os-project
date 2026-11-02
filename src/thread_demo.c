#include <stdio.h>
#include <pthread.h>

void *worker(void *arg) {
    int thread_id = *(int *)arg;

    printf("Hello from Thread %d\n", thread_id);

    return NULL;
}

int main(void) {
    pthread_t thread1, thread2;

    int id1 = 1;
    int id2 = 2;

    pthread_create(&thread1, NULL, worker, &id1);
    pthread_create(&thread2, NULL, worker, &id2);

    pthread_join(thread1, NULL);
    pthread_join(thread2, NULL);

    printf("Main thread: both workers finished.\n");

    return 0;
}
