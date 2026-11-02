#!/usr/bin/env bash

set -euo pipefail

cd "$(dirname "$0")/.."

PROGRAM="./optimistic_benchmark"
OUTPUT="results/raw/optimistic_benchmark.csv"

RUNS=5
READERS=4
WRITERS=2

if [[ ! -x "$PROGRAM" ]]; then
    echo "ERROR: $PROGRAM is missing or not executable."
    echo "Compile it first using:"
    echo "gcc -std=c11 -Wall -Wextra -Wpedantic -O2 src/optimistic_benchmark.c -o optimistic_benchmark -pthread"
    exit 1
fi

mkdir -p results/raw

printf 'run,readers,writers,successful_reads,read_retries,invalid_snapshots,updates,total_operations,elapsed_seconds,throughput_ops_sec\n' > "$OUTPUT"

echo "Optimistic-locking benchmark"
echo "Readers: $READERS"
echo "Writers: $WRITERS"
echo "Runs: $RUNS"
echo

for run in $(seq 1 "$RUNS"); do
    echo "========== Run $run/$RUNS =========="

    if ! result=$("$PROGRAM" "$READERS" "$WRITERS"); then
        echo "ERROR: Benchmark execution failed."
        printf '%s\n' "$result"
        exit 1
    fi

    printf '%s\n' "$result" |
        grep -E 'Successful reads:|Read retries:|Invalid snapshots:|Expected updates:|Total operations:|Elapsed seconds:|Throughput ops/sec:|RESULT:'

    if ! printf '%s\n' "$result" | grep -q '^RESULT: PASS$'; then
        echo "ERROR: Correctness check failed on run $run."
        exit 1
    fi

    successful_reads=$(printf '%s\n' "$result" | awk -F': *' '/^Successful reads:/ {print $2}')
    read_retries=$(printf '%s\n' "$result" | awk -F': *' '/^Read retries:/ {print $2}')
    invalid_snapshots=$(printf '%s\n' "$result" | awk -F': *' '/^Invalid snapshots:/ {print $2}')
    updates=$(printf '%s\n' "$result" | awk -F': *' '/^Expected updates:/ {print $2}')
    total_operations=$(printf '%s\n' "$result" | awk -F': *' '/^Total operations:/ {print $2}')
    elapsed_seconds=$(printf '%s\n' "$result" | awk -F': *' '/^Elapsed seconds:/ {print $2}')
    throughput=$(printf '%s\n' "$result" | awk -F': *' '/^Throughput ops\/sec:/ {print $2}')

    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
        "$run" "$READERS" "$WRITERS" \
        "$successful_reads" "$read_retries" "$invalid_snapshots" \
        "$updates" "$total_operations" "$elapsed_seconds" "$throughput" \
        >> "$OUTPUT"

    echo
done

echo "SUCCESS: All $RUNS benchmark runs passed."
echo "CSV saved to: $OUTPUT"
echo "Recorded measurements: $RUNS"
