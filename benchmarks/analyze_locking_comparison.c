#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_RUNS 100

typedef struct {
    char mode[16];
    int run;
    int readers;
    int writers;
    long successful_reads;
    long read_retries;
    long invalid_snapshots;
    long updates;
    long total_operations;
    double elapsed_seconds;
    double throughput;
} Measurement;

typedef struct {
    char name[16];
    double times[MAX_RUNS];
    double throughputs[MAX_RUNS];
    long total_retries;
    int count;
} StrategyStats;

static int compare_double(const void *a, const void *b)
{
    double x = *(const double *)a;
    double y = *(const double *)b;

    return (x > y) - (x < y);
}

static double median(double values[], int count)
{
    qsort(values, (size_t)count, sizeof(double), compare_double);

    if (count % 2 == 1) {
        return values[count / 2];
    }

    return (values[count / 2 - 1] + values[count / 2]) / 2.0;
}

static StrategyStats *find_strategy(
    StrategyStats strategies[],
    int strategy_count,
    const char *name)
{
    for (int i = 0; i < strategy_count; i++) {
        if (strcmp(strategies[i].name, name) == 0) {
            return &strategies[i];
        }
    }

    return NULL;
}

int main(void)
{
    const char *filename =
        "results/raw/locking_comparison.csv";

    FILE *file = fopen(filename, "r");

    if (file == NULL) {
        perror("Unable to open comparison CSV");
        return EXIT_FAILURE;
    }

    StrategyStats strategies[2] = {0};

    strcpy(strategies[0].name, "optimistic");
    strcpy(strategies[1].name, "mutex");

    char line[512];
    int line_number = 0;
    int valid_rows = 0;

    if (fgets(line, sizeof(line), file) == NULL) {
        fprintf(stderr, "ERROR: CSV file is empty.\n");
        fclose(file);
        return EXIT_FAILURE;
    }

    line_number++;

    while (fgets(line, sizeof(line), file) != NULL) {
        Measurement m;

        line_number++;

        int fields = sscanf(
            line,
            "%15[^,],%d,%d,%d,%ld,%ld,%ld,%ld,%ld,%lf,%lf",
            m.mode,
            &m.run,
            &m.readers,
            &m.writers,
            &m.successful_reads,
            &m.read_retries,
            &m.invalid_snapshots,
            &m.updates,
            &m.total_operations,
            &m.elapsed_seconds,
            &m.throughput
        );

        if (fields != 11) {
            fprintf(stderr,
                    "ERROR: Invalid CSV row at line %d.\n",
                    line_number);
            fclose(file);
            return EXIT_FAILURE;
        }

        StrategyStats *stats = find_strategy(
            strategies, 2, m.mode);

        if (stats == NULL ||
            stats->count >= MAX_RUNS ||
            m.readers != 4 ||
            m.writers != 2 ||
            m.successful_reads != 800000 ||
            m.updates != 200000 ||
            m.total_operations != 1000000 ||
            m.invalid_snapshots != 0 ||
            m.elapsed_seconds <= 0.0 ||
            m.throughput <= 0.0 ||
            m.read_retries < 0) {
            fprintf(stderr,
                    "ERROR: Invalid measurement at line %d.\n",
                    line_number);
            fclose(file);
            return EXIT_FAILURE;
        }

        if (strcmp(m.mode, "mutex") == 0 &&
            m.read_retries != 0) {
            fprintf(stderr,
                    "ERROR: Unexpected mutex retries at line %d.\n",
                    line_number);
            fclose(file);
            return EXIT_FAILURE;
        }

        int index = stats->count;

        stats->times[index] = m.elapsed_seconds;
        stats->throughputs[index] = m.throughput;
        stats->total_retries += m.read_retries;
        stats->count++;

        valid_rows++;
    }

    fclose(file);

    if (valid_rows != 10 ||
        strategies[0].count != 5 ||
        strategies[1].count != 5) {
        fprintf(stderr,
                "ERROR: Expected five valid runs per strategy.\n");
        return EXIT_FAILURE;
    }

    printf("\nLOCKING STRATEGY COMPARISON\n");
    printf("---------------------------\n");
    printf("Valid measurements: %d\n\n", valid_rows);

    printf("%-12s %5s %18s %22s %18s\n",
           "Strategy", "Runs", "Median time (s)",
           "Median throughput", "Avg. retries");

    double median_times[2];
    double median_throughputs[2];

    for (int i = 0; i < 2; i++) {
        StrategyStats *stats = &strategies[i];

        median_times[i] = median(
            stats->times, stats->count);

        median_throughputs[i] = median(
            stats->throughputs, stats->count);

        printf("%-12s %5d %18.6f %22.2f %18.2f\n",
               stats->name,
               stats->count,
               median_times[i],
               median_throughputs[i],
               (double)stats->total_retries / stats->count);
    }

    int optimistic_index = 0;
    int mutex_index = 1;

    double throughput_ratio =
        median_throughputs[mutex_index] /
        median_throughputs[optimistic_index];

    double time_reduction =
        (median_times[optimistic_index] -
         median_times[mutex_index]) /
        median_times[optimistic_index] * 100.0;

    printf("\nComparison (mutex relative to optimistic):\n");
    printf("Throughput ratio:       %.2fx\n", throughput_ratio);
    printf("Elapsed-time reduction: %.2f%%\n", time_reduction);

    printf("\nANALYSIS: PASS\n");

    return EXIT_SUCCESS;
}
