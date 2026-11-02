#!/usr/bin/env bash

set -euo pipefail

# Move to the project root directory
cd "$(dirname "$0")/.."

PROGRAM="./sync_comparison"
OUTPUT="results/raw/sync_comparison.csv"

# Experiment configuration
THREAD_COUNTS=(1 2 4 8)
MODES=(mutex atomic)
ITERATIONS=500000
RUNS=5

# Create output directory
mkdir -p results/raw

# Verify that the program exists
if [[ ! -x "$PROGRAM" ]]; then
    echo "ERROR: $PROGRAM is missing or not executable."
    echo "Compile it using:"
    echo "gcc -std=c11 -Wall -Wextra -Wpedantic -O2 src/sync_comparison.c -o sync_comparison -pthread"
    exit 1
fi

# Create a fresh CSV file for this experiment
printf 'mode,threads,run,operations,elapsed_seconds,throughput_ops_sec\n' \
    > "$OUTPUT"

echo "=========================================="
echo " Mutex vs Atomic Synchronization Benchmark"
echo "=========================================="
echo "Thread counts: ${THREAD_COUNTS[*]}"
echo "Iterations per thread: $ITERATIONS"
echo "Runs per configuration: $RUNS"
echo

TOTAL_RUNS=0

for mode in "${MODES[@]}"; do
    for threads in "${THREAD_COUNTS[@]}"; do
        for run in $(seq 1 "$RUNS"); do

            echo "Running mode=$mode, threads=$threads, run=$run..."

            # Execute the benchmark
            output=$("$PROGRAM" "$mode" "$threads" "$ITERATIONS")

            # Extract expected counter
            expected=$(printf '%s\n' "$output" |
                awk '/Expected counter:/ {print $3; exit}')

            # Extract actual counter
            actual=$(printf '%s\n' "$output" |
                awk '/Actual counter:/ {print $3; exit}')

            # Extract elapsed time
            elapsed=$(printf '%s\n' "$output" |
                awk '/Elapsed seconds:/ {print $3; exit}')

            # Check that the required values were found
            if [[ -z "$expected" || -z "$actual" || -z "$elapsed" ]]; then
                echo "ERROR: Could not parse benchmark output."
                echo "Program output:"
                printf '%s\n' "$output"
                exit 1
            fi

            # Verify counter correctness
            if [[ "$expected" != "$actual" ]]; then
                echo "ERROR: Incorrect counter."
                echo "Mode: $mode"
                echo "Threads: $threads"
                echo "Expected: $expected"
                echo "Actual: $actual"
                exit 1
            fi

            # Calculate throughput without Python
            throughput=$(awk \
                -v operations="$actual" \
                -v elapsed="$elapsed" '
                BEGIN {
                    if (elapsed + 0 > 0)
                        printf "%.2f", operations / elapsed
                    else
                        exit 1
                }
            ') || {
                echo "ERROR: Invalid elapsed time: $elapsed"
                exit 1
            }

            # Save the measurement
            printf '%s,%s,%s,%s,%s,%s\n' \
                "$mode" "$threads" "$run" "$actual" \
                "$elapsed" "$throughput" >> "$OUTPUT"

            echo "  Counter: $actual (correct)"
            echo "  Elapsed: $elapsed seconds"
            echo "  Throughput: $throughput ops/s"
            echo

            TOTAL_RUNS=$((TOTAL_RUNS + 1))

        done
    done
done

echo "=========================================="
echo "Benchmark completed successfully."
echo "Total measurements: $TOTAL_RUNS"
echo "Results saved to: $OUTPUT"
echo "=========================================="
