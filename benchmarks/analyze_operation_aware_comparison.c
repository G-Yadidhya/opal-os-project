#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROWS 15
#define RUNS 5
#define READERS 4
#define WRITERS 2
#define READ_COUNT 800000L
#define UPDATE_COUNT 200000L
#define OPERATION_COUNT 1000000L

typedef struct {
    double elapsed[RUNS];
    double throughput[RUNS];
    long retries[RUNS];
    int count;
} Stats;

typedef struct {
    int pair;
    char order[48];
    char mode[40];
    int run;
    int readers;
    int writers;
    long reads;
    long retries;
    long invalid;
    long updates;
    long operations;
    double elapsed;
    double throughput;
} Row;

static const char *HEADER =
    "pair,order,mode,run,readers,writers,successful_reads,read_retries,"
    "invalid_snapshots,updates,total_operations,elapsed_seconds,throughput_ops_sec";

static const char *ORDERS[RUNS] = {
    "optimistic_hybrid_mutex", "mutex_hybrid_optimistic",
    "hybrid_optimistic_mutex", "optimistic_mutex_hybrid",
    "mutex_optimistic_hybrid"
};

static const char *MODES[RUNS][3] = {
    {"optimistic_spin", "optimistic_reader_mutex_writer", "mutex"},
    {"mutex", "optimistic_reader_mutex_writer", "optimistic_spin"},
    {"optimistic_reader_mutex_writer", "optimistic_spin", "mutex"},
    {"optimistic_spin", "mutex", "optimistic_reader_mutex_writer"},
    {"mutex", "optimistic_spin", "optimistic_reader_mutex_writer"}
};

static const char *STRATEGY_NAMES[3] = {
    "optimistic_spin", "optimistic_reader_mutex_writer", "mutex"
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

static int parse_row(char *line, Row *row)
{
    char *fields[13];
    int count = 0;
    char *token = strtok(line, ",\r\n");
    while (token != NULL && count < 13) {
        fields[count++] = token;
        token = strtok(NULL, ",\r\n");
    }
    if (count != 13 || token != NULL ||
        strlen(fields[1]) >= sizeof(row->order) ||
        strlen(fields[2]) >= sizeof(row->mode)) return 0;

    long values[9];
    const int columns[9] = {0, 3, 4, 5, 6, 7, 8, 9, 10};
    for (int i = 0; i < 9; i++) {
        if (!parse_long(fields[columns[i]], &values[i])) return 0;
    }
    if (!parse_double(fields[11], &row->elapsed) ||
        !parse_double(fields[12], &row->throughput)) return 0;

    if (values[0] < 1 || values[0] > RUNS || values[1] < 1 || values[1] > RUNS ||
        values[2] < 1 || values[2] > 128 || values[3] < 1 || values[3] > 128) return 0;
    row->pair = (int)values[0];
    row->run = (int)values[1];
    row->readers = (int)values[2];
    row->writers = (int)values[3];
    row->reads = values[4];
    row->retries = values[5];
    row->invalid = values[6];
    row->updates = values[7];
    row->operations = values[8];
    strcpy(row->order, fields[1]);
    strcpy(row->mode, fields[2]);
    return 1;
}

static int strategy_index(const char *mode)
{
    for (int i = 0; i < 3; i++) {
        if (strcmp(mode, STRATEGY_NAMES[i]) == 0) return i;
    }
    return -1;
}

static double median_double(double values[RUNS])
{
    for (int i = 0; i < RUNS - 1; i++) {
        for (int j = i + 1; j < RUNS; j++) {
            if (values[j] < values[i]) {
                double temporary = values[i];
                values[i] = values[j];
                values[j] = temporary;
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
                long temporary = values[i];
                values[i] = values[j];
                values[j] = temporary;
            }
        }
    }
    return values[RUNS / 2];
}

int main(void)
{
    const char *filename = "results/raw/operation_aware_comparison.csv";
    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        perror("Unable to open operation-aware CSV");
        return EXIT_FAILURE;
    }

    char line[1024];
    if (fgets(line, sizeof(line), file) == NULL) {
        fprintf(stderr, "ERROR: CSV file is empty.\n");
        fclose(file);
        return EXIT_FAILURE;
    }
    line[strcspn(line, "\r\n")] = '\0';
    if (strcmp(line, HEADER) != 0) {
        fprintf(stderr, "ERROR: Unexpected operation-aware CSV header.\n");
        fclose(file);
        return EXIT_FAILURE;
    }

    Stats stats[3] = {0};
    int pair_rows[RUNS] = {0};
    int rows = 0;
    int line_number = 1;
    while (fgets(line, sizeof(line), file) != NULL) {
        Row row = {0};
        line_number++;
        if ((strchr(line, '\n') == NULL && !feof(file)) || rows >= ROWS ||
            !parse_row(line, &row)) {
            fprintf(stderr, "ERROR: Malformed or extra row at line %d.\n", line_number);
            fclose(file);
            return EXIT_FAILURE;
        }
        int pair_index = row.pair - 1;
        int position = pair_rows[pair_index];
        int index = strategy_index(row.mode);
        if (index < 0 || position >= 3 || row.pair != rows / 3 + 1 ||
            row.run != row.pair || strcmp(row.order, ORDERS[pair_index]) != 0 ||
            strcmp(row.mode, MODES[pair_index][position]) != 0 ||
            row.readers != READERS || row.writers != WRITERS ||
            row.reads != READ_COUNT || row.retries < 0 || row.invalid != 0 ||
            row.updates != UPDATE_COUNT || row.operations != OPERATION_COUNT ||
            row.elapsed <= 0.0 || row.throughput <= 0.0 ||
            (index == 2 && row.retries != 0)) {
            fprintf(stderr, "ERROR: Invalid counts or order at line %d.\n", line_number);
            fclose(file);
            return EXIT_FAILURE;
        }
        if (stats[index].count >= RUNS) {
            fprintf(stderr, "ERROR: Too many rows for %s.\n", row.mode);
            fclose(file);
            return EXIT_FAILURE;
        }
        int sample = stats[index].count++;
        stats[index].elapsed[sample] = row.elapsed;
        stats[index].throughput[sample] = row.throughput;
        stats[index].retries[sample] = row.retries;
        pair_rows[pair_index]++;
        rows++;
    }
    fclose(file);

    if (rows != ROWS) {
        fprintf(stderr, "ERROR: Expected %d measurement rows.\n", ROWS);
        return EXIT_FAILURE;
    }
    for (int i = 0; i < 3; i++) {
        if (stats[i].count != RUNS) {
            fprintf(stderr, "ERROR: Expected five measurements for %s.\n", STRATEGY_NAMES[i]);
            return EXIT_FAILURE;
        }
    }
    for (int i = 0; i < RUNS; i++) {
        if (pair_rows[i] != 3) {
            fprintf(stderr, "ERROR: Pair %d does not contain all three strategies.\n", i + 1);
            return EXIT_FAILURE;
        }
    }

    double med_time[3];
    double med_throughput[3];
    printf("OPERATION-AWARE LOCKING COMPARISON\n");
    printf("----------------------------------\n");
    printf("Validated measurements: %d\n\n", rows);
    printf("%-34s %5s %18s %22s %18s %18s\n",
           "Strategy", "Runs", "Median time (s)", "Median throughput",
           "Avg. retries", "Median retries");
    for (int i = 0; i < 3; i++) {
        med_time[i] = median_double(stats[i].elapsed);
        med_throughput[i] = median_double(stats[i].throughput);
        long retry_values[RUNS];
        long retry_sum = 0;
        memcpy(retry_values, stats[i].retries, sizeof(retry_values));
        for (int j = 0; j < RUNS; j++) retry_sum += stats[i].retries[j];
        long retry_median = median_long(retry_values);
        printf("%-34s %5d %18.6f %22.2f %18.2f %18ld\n",
               STRATEGY_NAMES[i], stats[i].count, med_time[i], med_throughput[i],
               (double)retry_sum / RUNS, retry_median);
    }

    printf("\nHybrid relative to optimistic spin-writer mode: %.2fx throughput\n",
           med_throughput[1] / med_throughput[0]);
    printf("Hybrid relative to all-mutex mode: %.2fx throughput\n",
           med_throughput[1] / med_throughput[2]);
    printf("\nANALYSIS: PASS\n");
    return EXIT_SUCCESS;
}
