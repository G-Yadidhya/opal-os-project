#!/usr/bin/env bash

set -euo pipefail

cd "$(dirname "$0")/.."

OPTIMISTIC="./optimistic_benchmark"
MUTEX="./mutex_benchmark"
OUTPUT="results/raw/locking_comparison.csv"

RUNS=5
READERS=4
WRITERS=2

for program in "$OPTIMISTIC" "$MUTEX"; do
    if [[ ! -x "$program" ]]; then
        echo "ERROR: $program is missing or not executable."
        echo "Compile the benchmark programs before running this script."
        exit 1
    fi
done

mkdir -p results/raw

printf 'mode,run,readers,writers,successful_reads,read_retries,invalid_snapshots,updates,total_operations,elapsed_seconds,throughput_ops_sec\n' > "$OUTPUT"

echo "Locking Strategy Comparison"
echo "Readers: $READERS"
echo "Writers: $WRITERS"
echo "Runs per strategy: $RUNS"
echo

for mode in optimistic mutex; do
    if [[ "$mode" == "optimistic" ]]; then
        program="$OPTIMISTIC"
    else
        program="$MUTEX"
    fi

    echo "========================================"
    echo "Strategy: $mode"
    echo "========================================"

    for run in $(seq 1 "$RUNS"); do
        echo "Run $run/$RUNS"

        if ! result=$("$program" "$READERS" "$WRITERS"); then
            echo "ERROR: $mode benchmark failed."
            printf '%s\n' "$result"
            exit 1
        fi

        if ! printf '%s\n' "$result" | grep -q '^RESULT: PASS$'; then
            echo "ERROR: Correctness check failed."
            printf '%s\n' "$result"
            exit 1
        fi

        successful_reads=$(printf '%s\n' "$result" |
            awk -F': *' '/^Successful reads:/ {print $2}')

        if [[ "$mode" == "optimistic" ]]; then
            read_retries=$(printf '%s\n' "$result" |
                awk -F': *' '/^Read retries:/ {print $2}')
        else
            read_retries=0
        fi

        invalid_snapshots=$(printf '%s\n' "$result" |
            awk -F': *' '/^Invalid snapshots:/ {print $2}')

        updates=$(printf '%s\n' "$result" |
            awk -F': *' '/^Expected updates:/ {print $2}')

        total_operations=$(printf '%s\n' "$result" |
            awk -F': *' '/^Total operations:/ {print $2}')

        elapsed_seconds=$(printf '%s\n' "$result" |
            awk -F': *' '/^Elapsed seconds:/ {print $2}')

        throughput=$(printf '%s\n' "$result" |
            awk -F': *' '/^Throughput ops\/sec:/ {print $2}')

        if [[ -z "$successful_reads" ||
              -z "$read_retries" ||
              -z "$invalid_snapshots" ||
              -z "$updates" ||
              -z "$total_operations" ||
              -z "$elapsed_seconds" ||
              -z "$throughput" ]]; then
            echo "ERROR: Failed to parse benchmark output."
            printf '%s\n' "$result"
            exit 1
        fi

        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
            "$mode" "$run" "$READERS" "$WRITERS" \
            "$successful_reads" "$read_retries" \
            "$invalid_snapshots" "$updates" "$total_operations" \
            "$elapsed_seconds" "$throughput" >> "$OUTPUT"

        printf '  Time: %s s | Throughput: %s ops/sec | Retries: %s\n' \
            "$elapsed_seconds" "$throughput" "$read_retries"
    done

    echo
done

echo "SUCCESS: All benchmark runs passed."
echo "CSV saved to: $OUTPUT"
echo "Expected measurement rows: $((RUNS * 2))"
