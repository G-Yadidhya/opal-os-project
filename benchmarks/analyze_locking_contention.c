#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RUNS 5
#define EXPECTED_READERS 4
#define EXPECTED_READS 800000L

typedef struct {
    double times[RUNS];
    double throughputs[RUNS];
    long retries[RUNS];
    int count;
} StrategyStats;

typedef struct {
    const char *label;
    const char *filename;
    int writers;
    long updates;
    long total_operations;
    StrategyStats strategies[2];
} Workload;

typedef struct {
    int pair;
    char order[32];
    char mode[16];
    int run;
    int readers;
    int writers;
    long successful_reads;
    long retries;
    long invalid_snapshots;
    long updates;
    long total_operations;
    double elapsed_seconds;
    double throughput;
} Measurement;

static const char *EXPECTED_HEADER =
    "pair,order,mode,run,readers,writers,successful_reads,read_retries,"
    "invalid_snapshots,updates,total_operations,elapsed_seconds,throughput_ops_sec";

static const char *EXPECTED_ORDERS[5] = {
    "optimistic_then_mutex", "mutex_then_optimistic",
    "optimistic_then_mutex", "mutex_then_optimistic",
    "optimistic_then_mutex"
};

static const char *EXPECTED_MODES[5][2] = {
    {"optimistic", "mutex"}, {"mutex", "optimistic"},
    {"optimistic", "mutex"}, {"mutex", "optimistic"},
    {"optimistic", "mutex"}
};

static int parse_long(const char *text, long *value)
{
    char *end;
    errno = 0;
    long parsed = strtol(text, &end, 10);
    if (errno != 0 || text == end || *end != '\0') return 0;
    *value = parsed;
    return 1;
}

static int parse_double(const char *text, double *value)
{
    char *end;
    errno = 0;
    double parsed = strtod(text, &end);
    if (errno != 0 || text == end || *end != '\0' || !isfinite(parsed)) return 0;
    *value = parsed;
    return 1;
}

static int parse_row(char *line, Measurement *m)
{
    char *fields[13];
    int count = 0;
    char *token = strtok(line, ",\r\n");
    while (token != NULL && count < 13) {
        fields[count++] = token;
        token = strtok(NULL, ",\r\n");
    }
    if (count != 13 || token != NULL) return 0;

    long values[9];
    const int column[] = {0, 3, 4, 5, 6, 7, 8, 9, 10};
    for (int i = 0; i < 9; i++) {
        if (!parse_long(fields[column[i]], &values[i])) return 0;
    }
    if (!parse_double(fields[11], &m->elapsed_seconds) ||
        !parse_double(fields[12], &m->throughput)) return 0;
    if (values[0] < 1 || values[0] > 5 || values[1] < 1 || values[1] > 5 ||
        values[2] < 1 || values[2] > 128 || values[3] < 1 || values[3] > 128 ||
        strlen(fields[1]) >= sizeof(m->order) || strlen(fields[2]) >= sizeof(m->mode)) return 0;

    m->pair = (int)values[0];
    m->run = (int)values[1];
    m->readers = (int)values[2];
    m->writers = (int)values[3];
    m->successful_reads = values[4];
    m->retries = values[5];
    m->invalid_snapshots = values[6];
    m->updates = values[7];
    m->total_operations = values[8];
    strcpy(m->order, fields[1]);
    strcpy(m->mode, fields[2]);
    return 1;
}

static double median_double(double values[RUNS])
{
    for (int i = 0; i < RUNS - 1; i++) {
        for (int j = i + 1; j < RUNS; j++) {
            if (values[j] < values[i]) {
                double tmp = values[i];
                values[i] = values[j];
                values[j] = tmp;
            }
        }
    }
    return values[RUNS / 2];
}

static long median_long(long values[RUNS])
{
    for (int i = 0; i < RUNS - 1; i++) {
        for (int j = i + 1; j < RUNS; j++) {
            if (values[j] < values[i]) {
                long tmp = values[i];
                values[i] = values[j];
                values[j] = tmp;
            }
        }
    }
    return values[RUNS / 2];
}

static int validate_workload(Workload *workload)
{
    FILE *file = fopen(workload->filename, "r");
    if (file == NULL) {
        perror(workload->filename);
        return 0;
    }

    char line[1024];
    if (fgets(line, sizeof(line), file) == NULL) {
        fprintf(stderr, "ERROR: %s is empty.\n", workload->filename);
        fclose(file);
        return 0;
    }
    line[strcspn(line, "\r\n")] = '\0';
    if (strcmp(line, EXPECTED_HEADER) != 0) {
        fprintf(stderr, "ERROR: Unexpected CSV header in %s.\n", workload->filename);
        fclose(file);
        return 0;
    }

    int pair_rows[5] = {0};
    int rows = 0;
    int line_number = 1;
    while (fgets(line, sizeof(line), file) != NULL) {
        Measurement m = {0};
        line_number++;
        if ((strchr(line, '\n') == NULL && !feof(file)) || rows >= 10 || !parse_row(line, &m)) {
            fprintf(stderr, "ERROR: Malformed or extra row in %s at line %d.\n",
                    workload->filename, line_number);
            fclose(file);
            return 0;
        }

        int pair_index = m.pair - 1;
        int position = pair_rows[pair_index];
        int strategy;
        if (strcmp(m.mode, "optimistic") == 0) strategy = 0;
        else if (strcmp(m.mode, "mutex") == 0) strategy = 1;
        else {
            fprintf(stderr, "ERROR: Unknown strategy in %s at line %d.\n",
                    workload->filename, line_number);
            fclose(file);
            return 0;
        }

        if (position >= 2 || m.pair != rows / 2 + 1 || m.run != m.pair ||
            strcmp(m.order, EXPECTED_ORDERS[pair_index]) != 0 ||
            strcmp(m.mode, EXPECTED_MODES[pair_index][position]) != 0 ||
            m.readers != EXPECTED_READERS || m.writers != workload->writers ||
            m.successful_reads != EXPECTED_READS || m.retries < 0 ||
            m.invalid_snapshots != 0 || m.updates != workload->updates ||
            m.total_operations != workload->total_operations ||
            m.elapsed_seconds <= 0.0 || m.throughput <= 0.0 ||
            (strategy == 1 && m.retries != 0)) {
            fprintf(stderr, "ERROR: Invalid counts or order metadata in %s at line %d.\n",
                    workload->filename, line_number);
            fclose(file);
            return 0;
        }

        StrategyStats *stats = &workload->strategies[strategy];
        if (stats->count >= RUNS) {
            fprintf(stderr, "ERROR: Too many rows for a strategy in %s.\n", workload->filename);
            fclose(file);
            return 0;
        }
        int index = stats->count++;
        stats->times[index] = m.elapsed_seconds;
        stats->throughputs[index] = m.throughput;
        stats->retries[index] = m.retries;
        pair_rows[pair_index]++;
        rows++;
    }
    fclose(file);

    if (rows != 10 || workload->strategies[0].count != RUNS ||
        workload->strategies[1].count != RUNS) {
        fprintf(stderr, "ERROR: %s must contain five rows per strategy.\n", workload->filename);
        return 0;
    }
    for (int pair = 0; pair < 5; pair++) {
        if (pair_rows[pair] != 2) {
            fprintf(stderr, "ERROR: Pair %d is incomplete in %s.\n",
                    pair + 1, workload->filename);
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    Workload workloads[] = {
        {.label = "read-heavy", .filename = "results/raw/locking_comparison_read_heavy.csv",
         .writers = 1, .updates = 100000L, .total_operations = 900000L},
        {.label = "baseline", .filename = "results/raw/locking_comparison_balanced.csv",
         .writers = 2, .updates = 200000L, .total_operations = 1000000L},
        {.label = "balanced", .filename = "results/raw/locking_comparison_balanced_mix.csv",
         .writers = 8, .updates = 800000L, .total_operations = 1600000L},
        {.label = "update-heavy", .filename = "results/raw/locking_comparison_update_heavy.csv",
         .writers = 16, .updates = 1600000L, .total_operations = 2400000L}
    };
    const int workload_count = (int)(sizeof(workloads) / sizeof(workloads[0]));

    for (int i = 0; i < workload_count; i++) {
        if (!validate_workload(&workloads[i])) return EXIT_FAILURE;
    }

    printf("WRITER CONTENTION SUMMARY\n");
    printf("-------------------------\n");
    printf("Validated datasets: %d (five alternating pairs each)\n\n", workload_count);
    printf("%-13s %7s %14s %14s %14s %16s %15s %15s\n",
           "Workload", "Writers", "Updates", "Opt. median t",
           "Opt. median ops", "Opt. avg retries", "Retries/read", "Mutex med ops");

    for (int i = 0; i < workload_count; i++) {
        Workload *w = &workloads[i];
        StrategyStats *opt = &w->strategies[0];
        StrategyStats *mutex = &w->strategies[1];
        double opt_time = median_double(opt->times);
        double opt_throughput = median_double(opt->throughputs);
        double mutex_throughput = median_double(mutex->throughputs);
        long retry_values[RUNS];
        long retry_sum = 0;
        memcpy(retry_values, opt->retries, sizeof(retry_values));
        for (int j = 0; j < RUNS; j++) retry_sum += opt->retries[j];
        double average_retries = (double)retry_sum / RUNS;
        double retries_per_read = average_retries / EXPECTED_READS;

        printf("%-13s %7d %14ld %14.6f %14.2f %16.2f %15.3f %15.2f\n",
               w->label, w->writers, w->updates, opt_time, opt_throughput,
               average_retries, retries_per_read, mutex_throughput);
        printf("  optimistic retry median: %ld | mutex/optimistic median throughput: %.2fx\n",
               median_long(retry_values), mutex_throughput / opt_throughput);
    }

    printf("\nANALYSIS: PASS\n");
    printf("Retries indicate optimistic read validation work; they are not failed completed operations.\n");
    printf("The workloads have different update and total-operation counts; compare strategies within each workload.\n");
    return EXIT_SUCCESS;
}
