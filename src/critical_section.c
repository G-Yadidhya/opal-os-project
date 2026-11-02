#include <stdio.h>
#include <pthread.h>

#define DEPOSITS 100000

typedef struct {
    long balance;
    long transaction_count;
} Account;

Account account = {0, 0};

pthread_mutex_t account_mutex = PTHREAD_MUTEX_INITIALIZER;

void *deposit(void *arg) {
    (void)arg;

    for (int i = 0; i < DEPOSITS; i++) {
        pthread_mutex_lock(&account_mutex);

        account.balance += 100;
        account.transaction_count++;

        pthread_mutex_unlock(&account_mutex);
    }

    return NULL;
}

int main(void) {
    pthread_t threads[2];

    for (int i = 0; i < 2; i++) {
        if (pthread_create(&threads[i], NULL,
                           deposit, NULL) != 0) {
            fprintf(stderr, "Thread creation failed\n");
            return 1;
        }
    }

    for (int i = 0; i < 2; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            fprintf(stderr, "Thread join failed\n");
            return 1;
        }
    }

    printf("Expected balance: %ld\n", 2L * DEPOSITS * 100);
    printf("Actual balance:   %ld\n", account.balance);
    printf("Expected transactions: %ld\n", 2L * DEPOSITS);
    printf("Actual transactions:   %ld\n",
           account.transaction_count);

    pthread_mutex_destroy(&account_mutex);

    return 0;
}
