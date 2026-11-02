#!/usr/bin/env bash

set -euo pipefail

# Move to the project root
cd "$(dirname "$0")/.."

PROGRAM="./thread_scaling"
OUTPUT="results/raw/thread_scaling.csv"

# Create output directory
mkdir -p results/raw

# Check benchmark executable
if [[ ! -x "$PROGRAM" ]]; then
    echo "ERROR: $PROGRAM is missing or not executable."
    echo "Compile it with:"
    echo "gcc -std=c11 -Wall -Wextra -Wpedantic -O2 src/thread_scaling.c -o thread_scaling -pthread"
    exit 1
fi

# Verify the benchmark program produces the expected output
echo "Checking benchmark program..."

if ! "$PROGRAM" 1; then
    echo "ERROR: The benchmark program failed."
    exit 1
fi

# CSV header
printf 'threads,run,operations,elapsed_seconds,throughput_ops_sec\n' > "$OUTPUT"

THREAD_COUNTS=(1 2 4 8)
RUNS=5

echo
echo "Starting thread-scaling benchmark..."
echo "Thread counts: ${THREAD_COUNTS[*]}"
echo "Runs per thread count: $RUNS"
echo

for threads in "${THREAD_COUNTS[@]}"; do
    for run in $(seq 1 "$RUNS"); do
        echo "Running threads=$threads, run=$run..."

        # Run benchmark and capture output
        output=$("$PROGRAM" "$threads")

        # Extract measurements
        expected=$(printf '%s\n' "$output" |
            awk '/Expected counter:/ {print $3; exit}')

        actual=$(printf '%s\n' "$output" |
            awk '/Actual counter:/ {print $3; exit}')

        elapsed=$(printf '%s\n' "$output" |
            awk '/Elapsed time:/ {print $3; exit}')

        # Check that all measurements were found
        if [[ -z "$expected" || -z "$actual" || -z "$elapsed" ]]; then
            echo "ERROR: Could not parse benchmark output."
            echo "Actual program output:"
            printf '%s\n' "$output"
            exit 1
        fi

        # Verify correctness
        if [[ "$expected" != "$actual" ]]; then
            echo "ERROR: Counter mismatch for $threads threads."
            echo "Expected: $expected"
            echo "Actual:   $actual"
            exit 1
        fi

        # Calculate throughput using awk; Python is not required
        throughput=$(awk -v operations="$actual" -v elapsed="$elapsed" '
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

        # Append result to CSV
        printf '%s,%s,%s,%s,%s\n' \
            "$threads" "$run" "$actual" "$elapsed" "$throughput" \
            >> "$OUTPUT"

        echo "  Counter: $actual (correct)"
        echo "  Elapsed: $elapsed seconds"
        echo "  Throughput: $throughput ops/s"
        echo
    done
done

echo "Benchmark completed successfully."
echo "Results saved to: $OUTPUT"
echo "Total measurement runs: $(( ${#THREAD_COUNTS[@]} * RUNS ))"
