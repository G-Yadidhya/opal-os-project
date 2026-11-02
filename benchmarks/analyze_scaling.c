#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_RUNS 100
#define MAX_THREADS 256

typedef struct {
    double elapsed[MAX_RUNS];
    double throughput[MAX_RUNS];
    int count;
} Measurements;

static Measurements results[MAX_THREADS + 1];

static int compare_double(const void *a, const void *b) {
    double x = *(const double *)a;
    double y = *(const double *)b;

    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

static double median(double values[], int count) {
    qsort(values, (size_t)count, sizeof(double), compare_double);

    if (count % 2 == 1) {
        return values[count / 2];
    }

    return (values[count / 2 - 1] + values[count / 2]) / 2.0;
}

int main(void) {
    const char *filename = "results/raw/thread_scaling.csv";

    FILE *file = fopen(filename, "r");

    if (file == NULL) {
        perror("Unable to open CSV file");
        return 1;
    }

    char line[512];

    /* Skip the CSV header. */
    if (fgets(line, sizeof(line), file) == NULL) {
        fprintf(stderr, "CSV file is empty.\n");
        fclose(file);
        return 1;
    }

    int line_number = 1;

    while (fgets(line, sizeof(line), file) != NULL) {
        line_number++;

        int threads;
        int run;
        long operations;
        double elapsed;
        double throughput;
        char extra;

        int fields = sscanf(
            line,
            "%d,%d,%ld,%lf,%lf %c",
            &threads,
            &run,
            &operations,
            &elapsed,
            &throughput,
            &extra
        );

        if (fields != 5) {
            fprintf(stderr, "Invalid CSV row at line %d.\n",
                    line_number);
            fclose(file);
            return 1;
        }

        if (threads < 1 || threads > MAX_THREADS ||
            elapsed <= 0.0 || throughput <= 0.0 ||
            operations <= 0) {
            fprintf(stderr, "Invalid values at line %d.\n",
                    line_number);
            fclose(file);
            return 1;
        }

        Measurements *m = &results[threads];

        if (m->count >= MAX_RUNS) {
            fprintf(stderr, "Too many runs for %d threads.\n",
                    threads);
            fclose(file);
            return 1;
        }

        int index = m->count;

        m->elapsed[index] = elapsed;
        m->throughput[index] = throughput;
        m->count++;
    }

    fclose(file);

    printf("\nThread Scaling Analysis\n");
    printf("------------------------------------------------------------\n");

    printf("%8s %8s %20s %25s\n",
           "Threads", "Runs", "Median Time (s)",
           "Median Throughput (ops/s)");

    for (int threads = 1; threads <= MAX_THREADS; threads++) {
        Measurements *m = &results[threads];

        if (m->count == 0) {
            continue;
        }

        double median_time = median(m->elapsed, m->count);
        double median_throughput =
            median(m->throughput, m->count);

        printf("%8d %8d %20.6f %25.2f\n",
               threads,
               m->count,
               median_time,
               median_throughput);
    }

    return 0;
}
