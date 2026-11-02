#!/usr/bin/env bash

set -euo pipefail

cd "$(dirname "$0")/.."

PROGRAM="./optimistic_lock"
RUNS=10

if [[ ! -x "$PROGRAM" ]]; then
    echo "ERROR: $PROGRAM is missing or not executable."
    echo "Compile it first:"
    echo "gcc -std=c11 -Wall -Wextra -Wpedantic -O2 src/optimistic_lock.c -o optimistic_lock -pthread"
    exit 1
fi

echo "Testing optimistic version-based locking..."
echo "Number of runs: $RUNS"
echo

for run in $(seq 1 "$RUNS"); do
    echo "Run $run/$RUNS"

    output=$("$PROGRAM")

    if ! printf '%s\n' "$output" | grep -q '^RESULT: PASS$'; then
        echo "ERROR: Correctness test failed on run $run."
        printf '%s\n' "$output"
        exit 1
    fi

    printf '%s\n' "$output" |
        grep -E 'Actual value A:|Actual value B:|Final version:|Invalid snapshots:|RESULT:'

    echo
done

echo "SUCCESS: All $RUNS runs passed."
