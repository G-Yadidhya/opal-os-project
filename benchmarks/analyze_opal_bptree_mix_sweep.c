#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INPUT "results/raw/opal_bptree_mix_sweep.csv"
#define RUNS 5
#define WORKLOADS 3
#define MODES 2
#define THREADS 4
#define OPS_PER_THREAD 10000
#define TOTAL_OPS (THREADS * OPS_PER_THREAD)

static const char *workload_names[WORKLOADS] = {
    "read_heavy", "balanced", "update_heavy"
};
static const int percentages[WORKLOADS][3] = {
    {80, 15, 5}, {50, 40, 10}, {20, 70, 10}
};
static const char expected_header[] =
    "pair,order,workload,mode,run,threads,operations_per_thread,lookup_pct,update_pct,insert_pct,lookup_ops,update_ops,insert_ops,optimistic_retries,update_callbacks,update_batches,elapsed_seconds,throughput_ops_sec";

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

static int workload_index(const char *name)
{
    for (int i = 0; i < WORKLOADS; i++) {
        if (strcmp(name, workload_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

int main(void)
{
    FILE *file = fopen(INPUT, "r");
    if (file == NULL) {
        perror(INPUT);
        return EXIT_FAILURE;
    }
    char line[640];
    if (fgets(line, sizeof(line), file) == NULL) {
        fprintf(stderr, "ERROR: CSV is empty.\n");
        fclose(file);
        return EXIT_FAILURE;
    }
    line[strcspn(line, "\r\n")] = '\0';
    if (strcmp(line, expected_header) != 0) {
        fprintf(stderr, "ERROR: CSV header does not match the expected schema.\n");
        fclose(file);
        return EXIT_FAILURE;
    }

    Measurement data[WORKLOADS][MODES][RUNS] = {{{{0}}}};
    int seen[WORKLOADS][MODES][RUNS] = {{{0}}};
    int rows = 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        int pair, run, threads, ops, lookup_pct, update_pct, insert_pct;
        int lookups, updates, inserts;
        char order[32], workload[24], mode[16];
        unsigned long retries, callbacks, batches;
        double elapsed, throughput;
        int consumed = 0;
        int fields = sscanf(line,
            "%d,%31[^,],%23[^,],%15[^,],%d,%d,%d,%d,%d,%d,%d,%d,%d,%lu,%lu,%lu,%lf,%lf%n",
            &pair, order, workload, mode, &run, &threads, &ops,
            &lookup_pct, &update_pct, &insert_pct, &lookups, &updates,
            &inserts, &retries, &callbacks, &batches, &elapsed, &throughput,
            &consumed);
        if (fields != 18 || line[consumed] != '\0') {
            fprintf(stderr, "ERROR: Malformed CSV row %d.\n", rows + 1);
            fclose(file);
            return EXIT_FAILURE;
        }
        int w = workload_index(workload);
        int m = strcmp(mode, "mutex") == 0 ? 0 :
                strcmp(mode, "opal") == 0 ? 1 : -1;
        long expected_lookups = TOTAL_OPS * percentages[w >= 0 ? w : 0][0] / 100;
        long expected_updates = TOTAL_OPS * percentages[w >= 0 ? w : 0][1] / 100;
        long expected_inserts = TOTAL_OPS * percentages[w >= 0 ? w : 0][2] / 100;
        const char *expected_order = pair % 2 == 1
            ? "mutex_then_opal" : "opal_then_mutex";
        double expected_throughput = elapsed > 0.0 ? TOTAL_OPS / elapsed : 0.0;

        if (w < 0 || m < 0 || pair < 1 || pair > RUNS || run != pair ||
            threads != THREADS || ops != OPS_PER_THREAD ||
            lookup_pct != percentages[w][0] || update_pct != percentages[w][1] ||
            insert_pct != percentages[w][2] || lookups != expected_lookups ||
            updates != expected_updates || inserts != expected_inserts ||
            callbacks != (unsigned long)expected_updates ||
            strcmp(order, expected_order) != 0 || elapsed <= 0.0 || throughput <= 0.0 ||
            fabs(throughput - expected_throughput) > 0.02 ||
            (m == 0 && (retries != 0 || batches != 0)) ||
            (m == 1 && batches == 0) || seen[w][m][pair - 1]) {
            fprintf(stderr, "ERROR: Validation failed at CSV data row %d.\n", rows + 1);
            fclose(file);
            return EXIT_FAILURE;
        }
        seen[w][m][pair - 1] = 1;
        data[w][m][pair - 1] = (Measurement){elapsed, throughput, retries,
                                             callbacks, batches};
        rows++;
    }
    if (ferror(file)) {
        perror("CSV read");
        fclose(file);
        return EXIT_FAILURE;
    }
    fclose(file);

    if (rows != WORKLOADS * MODES * RUNS) {
        fprintf(stderr, "ERROR: Expected %d rows; found %d.\n",
                WORKLOADS * MODES * RUNS, rows);
        return EXIT_FAILURE;
    }
    for (int w = 0; w < WORKLOADS; w++) {
        for (int m = 0; m < MODES; m++) {
            for (int run = 0; run < RUNS; run++) {
                if (!seen[w][m][run]) {
                    fprintf(stderr, "ERROR: Missing %s %s run %d.\n",
                            workload_names[w], m == 0 ? "mutex" : "opal", run + 1);
                    return EXIT_FAILURE;
                }
            }
        }
    }

    printf("OPAL-Inspired C11 B+ Tree Workload Sweep\n");
    printf("Validated measurements: %d\n", rows);
    printf("Workload       Strategy       Median time (s)   Median throughput   Ratio vs mutex   Retry avg   Batches avg\n");
    for (int w = 0; w < WORKLOADS; w++) {
        double elapsed[MODES][RUNS], throughput[MODES][RUNS];
        double retries = 0.0, batches = 0.0;
        for (int m = 0; m < MODES; m++) {
            for (int run = 0; run < RUNS; run++) {
                elapsed[m][run] = data[w][m][run].elapsed;
                throughput[m][run] = data[w][m][run].throughput;
                if (m == 1) {
                    retries += data[w][m][run].retries;
                    batches += data[w][m][run].batches;
                }
            }
        }
        double mutex_time = median(elapsed[0]);
        double opal_time = median(elapsed[1]);
        double mutex_ops = median(throughput[0]);
        double opal_ops = median(throughput[1]);
        printf("%-14s mutex          %.9f       %.2f             1.00x            0           0\n",
               workload_names[w], mutex_time, mutex_ops);
        printf("%-14s OPAL-inspired  %.9f       %.2f             %.2fx         %.0f       %.0f\n",
               "", opal_time, opal_ops, opal_ops / mutex_ops,
               retries / RUNS, batches / RUNS);
    }
    puts("\nScope: one machine, four threads, 40,000 operations/run; local C11 teaching adaptation, not upstream OPAL.");
    puts("ANALYSIS: PASS");
    return EXIT_SUCCESS;
}
