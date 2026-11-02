#!/usr/bin/env bash

set -euo pipefail
cd "$(dirname "$0")/.."

PROGRAM="./writer_mutex_optimistic_reader_benchmark"
RUNS=3
READERS=4

if [[ ! -x "$PROGRAM" ]]; then
    echo "ERROR: $PROGRAM is missing or not executable." >&2
    echo "Compile it first:" >&2
    echo "gcc -std=c11 -Wall -Wextra -Wpedantic -O2 -pthread src/writer_mutex_optimistic_reader_benchmark.c -o writer_mutex_optimistic_reader_benchmark" >&2
    exit 1
fi

for writers in 2 16; do
    expected_reads=$((READERS * 200000))
    expected_updates=$((writers * 100000))
    expected_total=$((expected_reads + expected_updates))
    echo "Testing $READERS readers and $writers writers ($RUNS repetitions)..."

    for run in $(seq 1 "$RUNS"); do
        if ! output=$("$PROGRAM" "$READERS" "$writers"); then
            echo "ERROR: Benchmark failed for $READERS/$writers, run $run." >&2
            printf '%s\n' "$output" >&2
            exit 1
        fi

        reads=$(printf '%s\n' "$output" | awk -F: '/^Successful reads:/ {gsub(/[[:space:]]/, "", $2); print $2}')
        updates=$(printf '%s\n' "$output" | awk -F: '/^Expected updates:/ {gsub(/[[:space:]]/, "", $2); print $2}')
        total=$(printf '%s\n' "$output" | awk -F: '/^Total operations:/ {gsub(/[[:space:]]/, "", $2); print $2}')
        invalid=$(printf '%s\n' "$output" | awk -F: '/^Invalid snapshots:/ {gsub(/[[:space:]]/, "", $2); print $2}')
        final_version=$(printf '%s\n' "$output" | awk -F: '/^Final version:/ {gsub(/[[:space:]]/, "", $2); print $2}')

        if ! printf '%s\n' "$output" | grep -q '^RESULT: PASS$' ||
           [[ "$reads" != "$expected_reads" || "$updates" != "$expected_updates" ||
              "$total" != "$expected_total" || "$invalid" != 0 ||
              "$final_version" != "$((2 * expected_updates))" ]]; then
            echo "ERROR: Correctness check failed for $READERS/$writers, run $run." >&2
            printf '%s\n' "$output" >&2
            exit 1
        fi
        echo "  PASS run $run/$RUNS"
    done
done

echo "SUCCESS: all writer-mutex/optimistic-reader checks passed."
