#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INPUT "results/raw/opal_bptree_thread_scaling.csv"
#define THREAD_LEVELS 5
#define RUNS 5

static const int thread_counts[THREAD_LEVELS] = {1, 2, 4, 8, 16};
static const char header[] =
    "thread_count,pair,order,mode,run,operations_per_thread,total_operations,lookup_ops,update_ops,insert_ops,optimistic_retries,update_callbacks,update_batches,elapsed_seconds,throughput_ops_sec";

typedef struct {
    double elapsed;
    double throughput;
    unsigned long retries;
    unsigned long batches;
} Measurement;

static double median(double values[RUNS])
{
    for (int i = 0; i < RUNS; ++i)
        for (int j = i + 1; j < RUNS; ++j)
            if (values[j] < values[i]) {
                double temporary = values[i];
                values[i] = values[j];
                values[j] = temporary;
            }
    return values[RUNS / 2];
}

int main(void)
{
    FILE *file = fopen(INPUT, "r");
    if (file == NULL) { perror(INPUT); return EXIT_FAILURE; }
    char line[1024];
    if (fgets(line, sizeof line, file) == NULL) {
        fprintf(stderr, "ERROR: CSV is empty.\n"); fclose(file); return EXIT_FAILURE;
    }
    line[strcspn(line, "\r\n")] = '\0';
    if (strcmp(line, header) != 0) {
        fprintf(stderr, "ERROR: CSV header mismatch.\n"); fclose(file); return EXIT_FAILURE;
    }
    Measurement data[THREAD_LEVELS][2][RUNS] = {{{{0}}}};
    int seen[THREAD_LEVELS][2][RUNS] = {{{0}}};
    int rows = 0;
    while (fgets(line, sizeof line, file) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        int threads, pair, run, ops, total, lookups, updates, inserts, used = 0;
        char order[32], mode[16];
        unsigned long retries, callbacks, batches;
        double elapsed, throughput;
        int fields = sscanf(line,
            "%d,%d,%31[^,],%15[^,],%d,%d,%d,%d,%d,%d,%lu,%lu,%lu,%lf,%lf%n",
            &threads, &pair, order, mode, &run, &ops, &total, &lookups,
            &updates, &inserts, &retries, &callbacks, &batches, &elapsed,
            &throughput, &used);
        if (fields != 15 || line[used] != '\0') {
            fprintf(stderr, "ERROR: Malformed CSV row %d.\n", rows + 1); fclose(file); return EXIT_FAILURE;
        }
        int ti = -1;
        for (int i = 0; i < THREAD_LEVELS; ++i) if (threads == thread_counts[i]) ti = i;
        int mi = strcmp(mode, "mutex") == 0 ? 0 : strcmp(mode, "opal") == 0 ? 1 : -1;
        const char *expected_order = pair % 2 ? "mutex_then_opal" : "opal_then_mutex";
        int expected_total = threads * 10000;
        int expected_lookups = expected_total * 80 / 100;
        int expected_updates = expected_total * 15 / 100;
        int expected_inserts = expected_total * 5 / 100;
        if (ti < 0 || mi < 0 || pair < 1 || pair > RUNS || run != pair || ops != 10000 ||
            total != expected_total || lookups != expected_lookups || updates != expected_updates ||
            inserts != expected_inserts || callbacks != (unsigned long)expected_updates ||
            strcmp(order, expected_order) != 0 || elapsed <= 0 || throughput <= 0 ||
            fabs(throughput - total / elapsed) > 0.02 ||
            (mi == 0 && (retries != 0 || batches != 0)) || (mi == 1 && batches == 0) ||
            seen[ti][mi][pair - 1]) {
            fprintf(stderr, "ERROR: Validation failed at CSV row %d.\n", rows + 1); fclose(file); return EXIT_FAILURE;
        }
        seen[ti][mi][pair - 1] = 1;
        data[ti][mi][pair - 1] = (Measurement){elapsed, throughput, retries, batches};
        ++rows;
    }
    if (ferror(file)) { perror("CSV read"); fclose(file); return EXIT_FAILURE; }
    fclose(file);
    if (rows != THREAD_LEVELS * 2 * RUNS) {
        fprintf(stderr, "ERROR: Expected 50 measurements, found %d.\n", rows); return EXIT_FAILURE;
    }
    for (int t = 0; t < THREAD_LEVELS; ++t)
        for (int m = 0; m < 2; ++m)
            for (int r = 0; r < RUNS; ++r)
                if (!seen[t][m][r]) { fprintf(stderr, "ERROR: Missing measurement.\n"); return EXIT_FAILURE; }

    double med_throughput[THREAD_LEVELS][2], med_elapsed[THREAD_LEVELS][2];
    double retry_avg[THREAD_LEVELS] = {0};
    for (int t = 0; t < THREAD_LEVELS; ++t) {
        for (int m = 0; m < 2; ++m) {
            double throughputs[RUNS], elapsed[RUNS];
            for (int r = 0; r < RUNS; ++r) {
                throughputs[r] = data[t][m][r].throughput;
                elapsed[r] = data[t][m][r].elapsed;
                if (m == 1) retry_avg[t] += data[t][m][r].retries;
            }
            med_throughput[t][m] = median(throughputs);
            med_elapsed[t][m] = median(elapsed);
        }
        retry_avg[t] /= RUNS;
    }
    puts("OPAL-Inspired C11 B+ Tree Thread-Count Sweep");
    printf("Validated measurements: %d\n", rows);
    puts("Threads  Mutex median ops/s  OPAL median ops/s  OPAL/mutex  Mutex scale  OPAL scale  OPAL median time(s)  OPAL avg retries");
    for (int t = 0; t < THREAD_LEVELS; ++t)
        printf("%7d  %17.2f  %17.2f  %10.2fx  %11.2fx  %10.2fx  %18.6f  %.0f\n",
               thread_counts[t], med_throughput[t][0], med_throughput[t][1],
               med_throughput[t][1] / med_throughput[t][0],
               med_throughput[t][0] / med_throughput[0][0],
               med_throughput[t][1] / med_throughput[0][1], med_elapsed[t][1], retry_avg[t]);
    puts("\nScope: 10,000 operations per thread, 80/15/5 operation mix; total work increases with thread count.");
    puts("Local teaching prototype measurements only; not upstream OPAL or a paper reproduction.");
    puts("ANALYSIS: PASS");
    return EXIT_SUCCESS;
}
