#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INPUT "results/raw/opal_bptree_comparison.csv"
#define RUNS 5
#define THREADS 4
#define OPS_PER_THREAD 10000
#define LOOKUPS 32000
#define UPDATES 6000
#define INSERTS 2000

typedef struct {
    double elapsed;
    double throughput;
    unsigned long retries;
    unsigned long callbacks;
    unsigned long batches;
} Measurement;

static int compare_double(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return (a > b) - (a < b);
}

static double median(double values[RUNS])
{
    qsort(values, RUNS, sizeof(values[0]), compare_double);
    return values[RUNS / 2];
}

static int validate_header(FILE *file)
{
    static const char expected[] =
        "pair,order,mode,run,threads,operations_per_thread,lookup_ops,update_ops,insert_ops,optimistic_retries,update_callbacks,update_batches,elapsed_seconds,throughput_ops_sec";
    char line[512];
    if (fgets(line, sizeof(line), file) == NULL) {
        return 0;
    }
    line[strcspn(line, "\r\n")] = '\0';
    return strcmp(line, expected) == 0;
}

int main(void)
{
    FILE *file = fopen(INPUT, "r");
    if (file == NULL) {
        perror(INPUT);
        return EXIT_FAILURE;
    }
    if (!validate_header(file)) {
        fprintf(stderr, "ERROR: CSV header does not match the expected schema.\n");
        fclose(file);
        return EXIT_FAILURE;
    }

    Measurement measurements[2][RUNS] = {{{0}}};
    int seen[2][RUNS] = {{0}};
    int rows = 0;
    char line[512];
    while (fgets(line, sizeof(line), file) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        int pair, run, threads, operations, lookups, updates, inserts;
        char order[32], mode[16];
        unsigned long retries, callbacks, batches;
        double elapsed, throughput;
        int consumed = 0;
        int fields = sscanf(line,
            "%d,%31[^,],%15[^,],%d,%d,%d,%d,%d,%d,%lu,%lu,%lu,%lf,%lf%n",
            &pair, order, mode, &run, &threads, &operations, &lookups,
            &updates, &inserts, &retries, &callbacks, &batches,
            &elapsed, &throughput, &consumed);
        if (fields != 14 || line[consumed] != '\0') {
            fprintf(stderr, "ERROR: Malformed CSV record at data row %d.\n", rows + 1);
            fclose(file);
            return EXIT_FAILURE;
        }
        int mode_index = strcmp(mode, "mutex") == 0 ? 0 :
                         strcmp(mode, "opal") == 0 ? 1 : -1;
        const char *expected_order = pair % 2 == 1
            ? "mutex_then_opal" : "opal_then_mutex";
        double expected_throughput = 40000.0 / elapsed;
        if (pair < 1 || pair > RUNS || run != pair || mode_index < 0 ||
            threads != THREADS || operations != OPS_PER_THREAD ||
            lookups != LOOKUPS || updates != UPDATES || inserts != INSERTS ||
            callbacks != UPDATES || strcmp(order, expected_order) != 0 ||
            elapsed <= 0.0 || throughput <= 0.0 ||
            fabs(throughput - expected_throughput) > 0.02 ||
            (mode_index == 0 && (retries != 0 || batches != 0)) ||
            (mode_index == 1 && batches == 0) ||
            seen[mode_index][pair - 1]) {
            fprintf(stderr, "ERROR: Validation failed at data row %d.\n", rows + 1);
            fclose(file);
            return EXIT_FAILURE;
        }
        seen[mode_index][pair - 1] = 1;
        measurements[mode_index][pair - 1] =
            (Measurement){elapsed, throughput, retries, callbacks, batches};
        rows++;
    }
    if (ferror(file)) {
        perror("CSV read");
        fclose(file);
        return EXIT_FAILURE;
    }
    fclose(file);

    if (rows != 2 * RUNS) {
        fprintf(stderr, "ERROR: Expected %d data rows, found %d.\n", 2 * RUNS, rows);
        return EXIT_FAILURE;
    }
    for (int mode = 0; mode < 2; mode++) {
        for (int run = 0; run < RUNS; run++) {
            if (!seen[mode][run]) {
                fprintf(stderr, "ERROR: Missing %s run %d.\n",
                        mode == 0 ? "mutex" : "opal", run + 1);
                return EXIT_FAILURE;
            }
        }
    }

    double elapsed[2][RUNS], throughput[2][RUNS];
    double retry_sum = 0.0, batch_sum = 0.0;
    for (int mode = 0; mode < 2; mode++) {
        for (int run = 0; run < RUNS; run++) {
            elapsed[mode][run] = measurements[mode][run].elapsed;
            throughput[mode][run] = measurements[mode][run].throughput;
            if (mode == 1) {
                retry_sum += measurements[mode][run].retries;
                batch_sum += measurements[mode][run].batches;
            }
        }
    }
    double mutex_elapsed = median(elapsed[0]);
    double opal_elapsed = median(elapsed[1]);
    double mutex_throughput = median(throughput[0]);
    double opal_throughput = median(throughput[1]);

    printf("OPAL-Inspired C11 B+ Tree Comparison\n");
    printf("-------------------------------------\n");
    printf("Validated measurements: %d\n", rows);
    printf("Strategy                         Runs   Median time (s)   Median throughput (ops/s)\n");
    printf("mutex baseline                     5      %.9f              %.2f\n",
           mutex_elapsed, mutex_throughput);
    printf("operation-aware C11 prototype      5      %.9f              %.2f\n",
           opal_elapsed, opal_throughput);
    printf("\nComparison (operation-aware relative to mutex):\n");
    printf("Throughput ratio:       %.2fx\n", opal_throughput / mutex_throughput);
    printf("Elapsed-time reduction: %.2f%%\n",
           (mutex_elapsed - opal_elapsed) * 100.0 / mutex_elapsed);
    printf("OPAL lookup retries:    %.0f average/run\n", retry_sum / RUNS);
    printf("Update batches:         %.0f average/run for %d update callbacks\n",
           batch_sum / RUNS, UPDATES);
    printf("\nScope: local teaching prototype, 4 threads, 80%% lookups, 15%% updates, 5%% inserts.\n");
    printf("This is not the upstream OPAL implementation or a reproduction of paper results.\n");
    printf("ANALYSIS: PASS\n");
    return EXIT_SUCCESS;
}
