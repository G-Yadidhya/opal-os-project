#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_THREADS 256
#define MAX_RUNS 100
#define CSV_FILE "results/raw/sync_comparison.csv"

typedef struct {
    double elapsed[MAX_RUNS];
    double throughput[MAX_RUNS];
    int count;
} Measurements;

static Measurements data[2][MAX_THREADS];

static int compare_doubles(const void *a, const void *b)
{
    double x = *(const double *)a;
    double y = *(const double *)b;

    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

static double median(const double values[], int count)
{
    double sorted[MAX_RUNS];

    if (count <= 0 || count > MAX_RUNS) {
        return 0.0;
    }

    for (int i = 0; i < count; i++) {
        sorted[i] = values[i];
    }

    qsort(sorted, (size_t)count, sizeof(double), compare_doubles);

    if (count % 2 == 1) {
        return sorted[count / 2];
    }

    return (sorted[count / 2 - 1] + sorted[count / 2]) / 2.0;
}

int main(void)
{
    FILE *file = fopen(CSV_FILE, "r");

    if (file == NULL) {
        perror("Could not open CSV file");
        return EXIT_FAILURE;
    }

    char line[512];

    /* Skip the CSV header. */
    if (fgets(line, sizeof(line), file) == NULL) {
        fprintf(stderr, "Error: CSV file is empty.\n");
        fclose(file);
        return EXIT_FAILURE;
    }

    int line_number = 1;
    int valid_rows = 0;

    while (fgets(line, sizeof(line), file) != NULL) {
        line_number++;

        char mode[16];
        int threads;
        int run;
        long operations;
        double elapsed;
        double throughput;

        int fields = sscanf(
            line,
            "%15[^,],%d,%d,%ld,%lf,%lf",
            mode,
            &threads,
            &run,
            &operations,
            &elapsed,
            &throughput
        );

        if (fields != 6) {
            fprintf(stderr,
                    "Warning: Skipping malformed CSV line %d.\n",
                    line_number);
            continue;
        }

        int mode_index;

        if (strcmp(mode, "mutex") == 0) {
            mode_index = 0;
        } else if (strcmp(mode, "atomic") == 0) {
            mode_index = 1;
        } else {
            fprintf(stderr,
                    "Warning: Unknown mode on line %d.\n",
                    line_number);
            continue;
        }

        if (threads < 1 || threads >= MAX_THREADS ||
            elapsed <= 0.0 || throughput <= 0.0 ||
            operations <= 0) {
            fprintf(stderr,
                    "Warning: Invalid measurement on line %d.\n",
                    line_number);
            continue;
        }

        Measurements *m = &data[mode_index][threads];

        if (m->count >= MAX_RUNS) {
            fprintf(stderr,
                    "Warning: Too many runs for %s with %d threads.\n",
                    mode, threads);
            continue;
        }

        m->elapsed[m->count] = elapsed;
        m->throughput[m->count] = throughput;
        m->count++;

        valid_rows++;
    }

    fclose(file);

    if (valid_rows == 0) {
        fprintf(stderr, "Error: No valid measurements were found.\n");
        return EXIT_FAILURE;
    }

    printf("\n");
    printf("============================================================\n");
    printf("       MUTEX VS ATOMIC SYNCHRONIZATION ANALYSIS\n");
    printf("============================================================\n");
    printf("Valid measurements read: %d\n\n", valid_rows);

    printf("%-8s %7s %8s %18s %20s\n",
           "Mode", "Threads", "Runs",
           "Median time (s)", "Median throughput");

    printf("---------------------------------------------------------------------\n");

    for (int threads = 1; threads < MAX_THREADS; threads++) {
        for (int mode = 0; mode < 2; mode++) {
            Measurements *m = &data[mode][threads];

            if (m->count == 0) {
                continue;
            }

            double med_time = median(m->elapsed, m->count);
            double med_throughput = median(m->throughput, m->count);

            printf("%-8s %7d %8d %18.6f %20.2f\n",
                   mode == 0 ? "mutex" : "atomic",
                   threads,
                   m->count,
                   med_time,
                   med_throughput);
        }
    }

    printf("\n");
    printf("============================================================\n");
    printf("             ATOMIC SPEEDUP OVER MUTEX\n");
    printf("============================================================\n");
    printf("%-10s %22s %24s\n",
           "Threads", "Speedup", "Time reduction (%)");
    printf("------------------------------------------------------------\n");

    for (int threads = 1; threads < MAX_THREADS; threads++) {
        Measurements *mutex = &data[0][threads];
        Measurements *atomic = &data[1][threads];

        if (mutex->count == 0 || atomic->count == 0) {
            continue;
        }

        double mutex_time = median(mutex->elapsed, mutex->count);
        double atomic_time = median(atomic->elapsed, atomic->count);

        double mutex_throughput =
            median(mutex->throughput, mutex->count);

        double atomic_throughput =
            median(atomic->throughput, atomic->count);

        if (mutex_throughput <= 0.0 || mutex_time <= 0.0) {
            continue;
        }

        double speedup = atomic_throughput / mutex_throughput;
        double time_reduction =
            ((mutex_time - atomic_time) / mutex_time) * 100.0;

        printf("%-10d %18.2fx %21.2f%%\n",
               threads, speedup, time_reduction);
    }

    printf("\n");
    printf("Note: These results summarize your recorded benchmark runs.\n");
    printf("They are not universal performance guarantees.\n");

    return EXIT_SUCCESS;
}
