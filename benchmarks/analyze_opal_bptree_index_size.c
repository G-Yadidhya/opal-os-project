#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIZES 3
#define RUNS 5
static const int index_sizes[SIZES] = {512, 4096, 32768};
static const char header[] =
    "initial_keys,pair,order,mode,run,threads,operations_per_thread,total_operations,lookup_ops,update_ops,insert_ops,optimistic_retries,update_callbacks,update_batches,elapsed_seconds,throughput_ops_sec";
typedef struct { double elapsed, throughput; unsigned long retries; } Measurement;

static double median(double values[RUNS])
{
    for (int i = 0; i < RUNS; ++i)
        for (int j = i + 1; j < RUNS; ++j)
            if (values[j] < values[i]) { double t = values[i]; values[i] = values[j]; values[j] = t; }
    return values[RUNS / 2];
}

int main(void)
{
    FILE *file = fopen("results/raw/opal_bptree_index_size.csv", "r");
    if (!file) { perror("results/raw/opal_bptree_index_size.csv"); return EXIT_FAILURE; }
    char line[1024];
    if (!fgets(line, sizeof line, file)) { fprintf(stderr, "ERROR: Empty CSV.\n"); fclose(file); return EXIT_FAILURE; }
    line[strcspn(line, "\r\n")] = '\0';
    if (strcmp(line, header)) { fprintf(stderr, "ERROR: CSV header mismatch.\n"); fclose(file); return EXIT_FAILURE; }
    Measurement data[SIZES][2][RUNS] = {{{{0}}}};
    int seen[SIZES][2][RUNS] = {{{0}}};
    int rows = 0;
    while (fgets(line, sizeof line, file)) {
        line[strcspn(line, "\r\n")] = '\0';
        int keys, pair, run, threads, ops, total, lookups, updates, inserts, used = 0;
        char order[32], mode[16]; unsigned long retries, callbacks, batches; double elapsed, throughput;
        int n = sscanf(line, "%d,%d,%31[^,],%15[^,],%d,%d,%d,%d,%d,%d,%d,%lu,%lu,%lu,%lf,%lf%n",
            &keys, &pair, order, mode, &run, &threads, &ops, &total, &lookups,
            &updates, &inserts, &retries, &callbacks, &batches, &elapsed, &throughput, &used);
        if (n != 16 || line[used] != '\0') { fprintf(stderr, "ERROR: Malformed CSV row %d.\n", rows + 1); fclose(file); return EXIT_FAILURE; }
        int si = -1; for (int i = 0; i < SIZES; ++i) if (keys == index_sizes[i]) si = i;
        int mi = !strcmp(mode, "mutex") ? 0 : !strcmp(mode, "opal") ? 1 : -1;
        int expected_total = 4 * 10000, expected_updates = expected_total * 15 / 100;
        const char *expected_order = pair % 2 ? "mutex_then_opal" : "opal_then_mutex";
        if (si < 0 || mi < 0 || pair < 1 || pair > RUNS || run != pair || threads != 4 || ops != 10000 ||
            total != expected_total || lookups != expected_total * 80 / 100 || updates != expected_updates ||
            inserts != expected_total * 5 / 100 || callbacks != (unsigned long)expected_updates ||
            strcmp(order, expected_order) || elapsed <= 0 || throughput <= 0 ||
            fabs(throughput - total / elapsed) > 0.02 ||
            (mi == 0 && (retries || batches)) || (mi == 1 && !batches) || seen[si][mi][pair - 1]) {
            fprintf(stderr, "ERROR: Validation failed at CSV row %d.\n", rows + 1); fclose(file); return EXIT_FAILURE;
        }
        seen[si][mi][pair - 1] = 1;
        data[si][mi][pair - 1] = (Measurement){elapsed, throughput, retries};
        ++rows;
    }
    if (ferror(file)) { perror("CSV read"); fclose(file); return EXIT_FAILURE; }
    fclose(file);
    if (rows != SIZES * 2 * RUNS) { fprintf(stderr, "ERROR: Expected 30 rows, found %d.\n", rows); return EXIT_FAILURE; }
    for (int s = 0; s < SIZES; ++s) for (int m = 0; m < 2; ++m) for (int r = 0; r < RUNS; ++r)
        if (!seen[s][m][r]) { fprintf(stderr, "ERROR: Missing measurement.\n"); return EXIT_FAILURE; }
    puts("OPAL-Inspired C11 B+ Tree Index-Size Comparison");
    printf("Validated measurements: %d\n", rows);
    puts("Initial keys  Mutex median ops/s  OPAL median ops/s  OPAL/mutex  OPAL median time(s)  OPAL avg retries");
    for (int s = 0; s < SIZES; ++s) {
        double rates[2][RUNS], elapsed[2][RUNS], retry_sum = 0;
        for (int m = 0; m < 2; ++m) for (int r = 0; r < RUNS; ++r) {
            rates[m][r] = data[s][m][r].throughput; elapsed[m][r] = data[s][m][r].elapsed;
            if (m == 1) retry_sum += data[s][m][r].retries;
        }
        double mutex_rate = median(rates[0]), opal_rate = median(rates[1]);
        printf("%12d  %17.2f  %17.2f  %10.2fx  %19.6f  %.0f\n", index_sizes[s], mutex_rate, opal_rate,
               opal_rate / mutex_rate, median(elapsed[1]), retry_sum / RUNS);
    }
    puts("\nScope: 4 threads, 10,000 operations/thread, 80/15/5 mix; initial tree size varied.");
    puts("Local teaching prototype measurements only; not upstream OPAL or a paper reproduction.");
    puts("ANALYSIS: PASS");
    return EXIT_SUCCESS;
}
