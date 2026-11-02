#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_RUNS 100

typedef struct {
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

int main(void)
{
    const char *filename =
        "results/raw/optimistic_benchmark.csv";

    FILE *file = fopen(filename, "r");

    if (file == NULL) {
        perror("Unable to open benchmark CSV");
        return EXIT_FAILURE;
    }

    Measurement measurements[MAX_RUNS];
    double times[MAX_RUNS];
    double throughputs[MAX_RUNS];

    char line[512];
    int count = 0;
    int line_number = 0;
    double total_retries = 0.0;

    /* Skip the CSV header. */
    if (fgets(line, sizeof(line), file) == NULL) {
        fprintf(stderr, "ERROR: CSV file is empty.\n");
        fclose(file);
        return EXIT_FAILURE;
    }

    line_number++;

    while (fgets(line, sizeof(line), file) != NULL) {
        Measurement m;

        line_number++;

        if (count >= MAX_RUNS) {
            fprintf(stderr, "ERROR: Too many measurements.\n");
            fclose(file);
            return EXIT_FAILURE;
        }

        int fields = sscanf(
            line,
            "%d,%d,%d,%ld,%ld,%ld,%ld,%ld,%lf,%lf",
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

        if (fields != 10) {
            fprintf(stderr,
                    "ERROR: Invalid CSV row at line %d.\n",
                    line_number);
            fclose(file);
            return EXIT_FAILURE;
        }

        if (m.readers < 1 ||
            m.writers < 1 ||
            m.successful_reads != (long)m.readers * 200000L ||
            m.updates != (long)m.writers * 100000L ||
            m.total_operations !=
                m.successful_reads + m.updates ||
            m.invalid_snapshots != 0 ||
            m.elapsed_seconds <= 0.0 ||
            m.throughput <= 0.0) {
            fprintf(stderr,
                    "ERROR: Invalid measurement at line %d.\n",
                    line_number);
            fclose(file);
            return EXIT_FAILURE;
        }

        measurements[count] = m;
        times[count] = m.elapsed_seconds;
        throughputs[count] = m.throughput;

        total_retries += (double)m.read_retries;
        count++;
    }

    fclose(file);

    if (count == 0) {
        fprintf(stderr, "ERROR: No measurements found.\n");
        return EXIT_FAILURE;
    }

    double median_time = median(times, count);
    double median_throughput = median(throughputs, count);

    double minimum_throughput = throughputs[0];
    double maximum_throughput = throughputs[0];

    for (int i = 1; i < count; i++) {
        if (throughputs[i] < minimum_throughput) {
            minimum_throughput = throughputs[i];
        }

        if (throughputs[i] > maximum_throughput) {
            maximum_throughput = throughputs[i];
        }
    }

    printf("\nOPTIMISTIC-LOCKING BENCHMARK ANALYSIS\n");
    printf("------------------------------------\n");
    printf("Valid measurements:       %d\n", count);
    printf("Readers per run:          %d\n",
           measurements[0].readers);
    printf("Writers per run:          %d\n",
           measurements[0].writers);
    printf("Operations per run:       %ld\n",
           measurements[0].total_operations);
    printf("Median elapsed time:      %.6f seconds\n",
           median_time);
    printf("Median throughput:        %.2f ops/sec\n",
           median_throughput);
    printf("Minimum throughput:       %.2f ops/sec\n",
           minimum_throughput);
    printf("Maximum throughput:       %.2f ops/sec\n",
           maximum_throughput);
    printf("Average read retries:     %.2f\n",
           total_retries / count);

    printf("\nIndividual measurements:\n");
    printf("Run    Time (s)       Throughput (ops/sec)\n");

    for (int i = 0; i < count; i++) {
        printf("%-6d %-14.6f %.2f\n",
               measurements[i].run,
               measurements[i].elapsed_seconds,
               measurements[i].throughput);
    }

    printf("\nANALYSIS: PASS\n");

    return EXIT_SUCCESS;
}
