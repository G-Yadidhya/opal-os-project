#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPECTED_ROWS 10
#define EXPECTED_READERS 4
#define EXPECTED_WRITERS 16
#define EXPECTED_READS 800000L
#define EXPECTED_UPDATES 1600000L
#define EXPECTED_OPERATIONS 2400000L

typedef struct {
    int pair;
    char order[32];
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
    const char *name;
    double times[5];
    double throughputs[5];
    long retries[5];
    int count;
} StrategyStats;

static const char *EXPECTED_HEADER =
    "pair,order,mode,run,readers,writers,successful_reads,read_retries,"
    "invalid_snapshots,updates,total_operations,elapsed_seconds,throughput_ops_sec";

static int compare_double(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return (a > b) - (a < b);
}

static int compare_long(const void *left, const void *right)
{
    long a = *(const long *)left;
    long b = *(const long *)right;
    return (a > b) - (a < b);
}

static double median_double(double values[], int count)
{
    qsort(values, (size_t)count, sizeof(values[0]), compare_double);
    if (count % 2 == 1) return values[count / 2];
    return (values[count / 2 - 1] + values[count / 2]) / 2.0;
}

static double median_long(long values[], int count)
{
    qsort(values, (size_t)count, sizeof(values[0]), compare_long);
    if (count % 2 == 1) return (double)values[count / 2];
    return ((double)values[count / 2 - 1] +
            (double)values[count / 2]) / 2.0;
}

static int parse_long(const char *text, long *value)
{
    char *end;
    errno = 0;
    long parsed = strtol(text, &end, 10);
    if (errno != 0 || text == end || *end != '\0') {
        return 0;
    }
    *value = parsed;
    return 1;
}

static int parse_double(const char *text, double *value)
{
    char *end;
    errno = 0;
    double parsed = strtod(text, &end);
    if (errno != 0 || text == end || *end != '\0' || !isfinite(parsed)) {
        return 0;
    }
    *value = parsed;
    return 1;
}

static int parse_row(char *line, Measurement *measurement)
{
    char *fields[13];
    int count = 0;
    char *token = strtok(line, ",\r\n");

    while (token != NULL && count < 13) {
        fields[count++] = token;
        token = strtok(NULL, ",\r\n");
    }
    if (count != 13 || token != NULL) {
        return 0;
    }

    long numbers[10];
    const int integer_fields[] = {0, 3, 4, 5, 6, 7, 8, 9, 10};
    const int destinations[] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
    for (size_t i = 0; i < sizeof(integer_fields) / sizeof(integer_fields[0]); i++) {
        if (!parse_long(fields[integer_fields[i]], &numbers[destinations[i]])) {
            return 0;
        }
    }
    if (!parse_double(fields[11], &measurement->elapsed_seconds) ||
        !parse_double(fields[12], &measurement->throughput)) {
        return 0;
    }

    if (numbers[0] < 1 || numbers[0] > 5 ||
        numbers[1] < 1 || numbers[1] > 5 ||
        numbers[2] < 1 || numbers[2] > 128 ||
        numbers[3] < 1 || numbers[3] > 128) {
        return 0;
    }
    measurement->pair = (int)numbers[0];
    measurement->run = (int)numbers[1];
    measurement->readers = (int)numbers[2];
    measurement->writers = (int)numbers[3];
    measurement->successful_reads = numbers[4];
    measurement->read_retries = numbers[5];
    measurement->invalid_snapshots = numbers[6];
    measurement->updates = numbers[7];
    measurement->total_operations = numbers[8];
    if (strlen(fields[1]) >= sizeof(measurement->order) ||
        strlen(fields[2]) >= sizeof(measurement->mode)) {
        return 0;
    }
    strcpy(measurement->order, fields[1]);
    strcpy(measurement->mode, fields[2]);
    return 1;
}

int main(void)
{
    const char *filename = "results/raw/locking_comparison_update_heavy.csv";
    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        perror("Unable to open balanced comparison CSV");
        return EXIT_FAILURE;
    }

    char line[1024];
    if (fgets(line, sizeof(line), file) == NULL) {
        fprintf(stderr, "ERROR: CSV file is empty.\n");
        fclose(file);
        return EXIT_FAILURE;
    }
    line[strcspn(line, "\r\n")] = '\0';
    if (strcmp(line, EXPECTED_HEADER) != 0) {
        fprintf(stderr, "ERROR: CSV header does not match the expected schema.\n");
        fclose(file);
        return EXIT_FAILURE;
    }

    StrategyStats strategies[2] = {
        {.name = "optimistic"},
        {.name = "mutex"}
    };
    int pair_modes[5][2] = {{0}};
    const char *expected_orders[5] = {
        "optimistic_then_mutex", "mutex_then_optimistic",
        "optimistic_then_mutex", "mutex_then_optimistic",
        "optimistic_then_mutex"
    };
    const char *expected_modes[5][2] = {
        {"optimistic", "mutex"}, {"mutex", "optimistic"},
        {"optimistic", "mutex"}, {"mutex", "optimistic"},
        {"optimistic", "mutex"}
    };
    int rows = 0;
    int line_number = 1;

    while (fgets(line, sizeof(line), file) != NULL) {
        Measurement m = {0};
        line_number++;
        if (strchr(line, '\n') == NULL && !feof(file)) {
            fprintf(stderr, "ERROR: CSV row is too long at line %d.\n", line_number);
            fclose(file);
            return EXIT_FAILURE;
        }
        if (rows >= EXPECTED_ROWS || !parse_row(line, &m)) {
            fprintf(stderr, "ERROR: Malformed or extra CSV row at line %d.\n", line_number);
            fclose(file);
            return EXIT_FAILURE;
        }

        int strategy_index;
        if (strcmp(m.mode, "optimistic") == 0) strategy_index = 0;
        else if (strcmp(m.mode, "mutex") == 0) strategy_index = 1;
        else {
            fprintf(stderr, "ERROR: Unknown mode at line %d.\n", line_number);
            fclose(file);
            return EXIT_FAILURE;
        }

        int pair_index = m.pair - 1;
        int mode_bit = strategy_index == 0 ? 1 : 2;
        int position_in_pair = pair_modes[pair_index][0];
        if (position_in_pair >= 2 || m.run != m.pair || m.order[0] == '\0' ||
            strcmp(m.order, expected_orders[pair_index]) != 0 ||
            strcmp(m.mode, expected_modes[pair_index][position_in_pair]) != 0 ||
            (pair_modes[pair_index][1] & mode_bit) != 0 ||
            m.readers != EXPECTED_READERS || m.writers != EXPECTED_WRITERS ||
            m.successful_reads != EXPECTED_READS || m.read_retries < 0 ||
            m.invalid_snapshots != 0 || m.updates != EXPECTED_UPDATES ||
            m.total_operations != EXPECTED_OPERATIONS ||
            m.elapsed_seconds <= 0.0 || m.throughput <= 0.0 ||
            (strategy_index == 1 && m.read_retries != 0)) {
            fprintf(stderr, "ERROR: Invalid measurement or pair/order metadata at line %d.\n", line_number);
            fclose(file);
            return EXIT_FAILURE;
        }
        pair_modes[pair_index][0]++;
        pair_modes[pair_index][1] |= mode_bit;

        StrategyStats *stats = &strategies[strategy_index];
        if (stats->count >= 5) {
            fprintf(stderr, "ERROR: Too many %s rows.\n", stats->name);
            fclose(file);
            return EXIT_FAILURE;
        }
        int index = stats->count++;
        stats->times[index] = m.elapsed_seconds;
        stats->throughputs[index] = m.throughput;
        stats->retries[index] = m.read_retries;
        rows++;
    }
    fclose(file);

    if (rows != EXPECTED_ROWS || strategies[0].count != 5 ||
        strategies[1].count != 5) {
        fprintf(stderr, "ERROR: Expected ten rows and five runs per mode.\n");
        return EXIT_FAILURE;
    }
    for (int pair = 0; pair < 5; pair++) {
        if (pair_modes[pair][0] != 2 || pair_modes[pair][1] != 3) {
            fprintf(stderr, "ERROR: Pair %d does not have exactly one row per mode.\n", pair + 1);
            return EXIT_FAILURE;
        }
    }

    double median_times[2];
    double median_throughputs[2];
    long median_optimistic_retries;
    long retry_copy[5];
    memcpy(retry_copy, strategies[0].retries, sizeof(retry_copy));
    median_optimistic_retries = (long)median_long(retry_copy, 5);

    printf("UPDATE-HEAVY LOCKING STRATEGY COMPARISON\n");
    printf("----------------------------------------\n");
    printf("Validated measurements: %d\n\n", rows);
    printf("Pair execution order (first, second):\n");
    for (int pair = 0; pair < 5; pair++) {
        printf("  %d: %s\n", pair + 1, expected_orders[pair]);
    }
    printf("\n%-12s %5s %18s %22s %18s %18s\n",
           "Strategy", "Runs", "Median time (s)", "Median throughput",
           "Avg. retries", "Median retries");
    for (int i = 0; i < 2; i++) {
        median_times[i] = median_double(strategies[i].times, 5);
        median_throughputs[i] = median_double(strategies[i].throughputs, 5);
        long retry_sum = 0;
        for (int j = 0; j < 5; j++) retry_sum += strategies[i].retries[j];
        long retry_values[5];
        memcpy(retry_values, strategies[i].retries, sizeof(retry_values));
        long median_retries = i == 0
            ? median_optimistic_retries
            : (long)median_long(retry_values, 5);
        printf("%-12s %5d %18.6f %22.2f %18.2f %18ld\n",
               strategies[i].name, strategies[i].count,
               median_times[i], median_throughputs[i],
               (double)retry_sum / 5.0,
               median_retries);
    }

    double throughput_ratio = median_throughputs[1] / median_throughputs[0];
    double elapsed_reduction =
        (median_times[0] - median_times[1]) / median_times[0] * 100.0;
    printf("\nComparison (mutex relative to optimistic):\n");
    printf("Throughput ratio:       %.2fx\n", throughput_ratio);
    printf("Elapsed-time reduction: %.2f%%\n", elapsed_reduction);
    printf("\nANALYSIS: PASS\n");
    return EXIT_SUCCESS;
}
