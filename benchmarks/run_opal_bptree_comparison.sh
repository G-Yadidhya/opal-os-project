#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

PROGRAM=./opal_bptree_benchmark
THREADS=4
OPERATIONS_PER_THREAD=10000
RUNS=5
TOTAL_OPERATIONS=$((THREADS * OPERATIONS_PER_THREAD))
EXPECTED_LOOKUPS=$((TOTAL_OPERATIONS * 80 / 100))
EXPECTED_UPDATES=$((TOTAL_OPERATIONS * 15 / 100))
EXPECTED_INSERTS=$((TOTAL_OPERATIONS * 5 / 100))
INITIAL_KEYS=512
CSV=results/raw/opal_bptree_comparison.csv

if [[ ! -x "$PROGRAM" ]]; then
    echo "ERROR: $PROGRAM is missing or not executable." >&2
    echo "Compile: gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -pthread -Iinclude src/opal_bptree.c src/opal_bptree_benchmark.c -o opal_bptree_benchmark" >&2
    exit 1
fi

mkdir -p results/raw
timestamp=$(date '+%Y%m%d_%H%M%S')
ENVIRONMENT="results/raw/opal_bptree_comparison_environment_${timestamp}.txt"
if [[ -e "$CSV" || -e "$ENVIRONMENT" ]]; then
    echo "ERROR: Output already exists; refusing to overwrite." >&2
    [[ ! -e "$CSV" ]] || echo "  $CSV" >&2
    [[ ! -e "$ENVIRONMENT" ]] || echo "  $ENVIRONMENT" >&2
    exit 1
fi

os_release=unavailable
if [[ -r /etc/os-release ]]; then
    os_release=$(awk -F= '/^(PRETTY_NAME|NAME|VERSION)=/ {gsub(/^"|"$/, "", $2); if (found++) out=out ";"; out=out $1 "=" $2} END {print out}' /etc/os-release)
    [[ -n "$os_release" ]] || os_release=unavailable
fi
kernel=$(uname -srmo 2>/dev/null || printf unavailable)
cpu_details=unavailable
if command -v lscpu >/dev/null 2>&1; then
    cpu_details=$(lscpu 2>/dev/null | awk -F: '/^(Architecture|Vendor ID|Model name|CPU family|Model|Thread\(s\) per core|Core\(s\) per socket|Socket\(s\)|CPU max MHz|CPU min MHz):/ {gsub(/^[ \t]+|[ \t]+$/, "", $1); gsub(/^[ \t]+|[ \t]+$/, "", $2); print $1 "=" $2}')
    [[ -n "$cpu_details" ]] || cpu_details=unavailable
fi
logical_cpus=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf unavailable)
compiler_version=unavailable
if command -v gcc >/dev/null 2>&1; then compiler_version=$(gcc --version 2>/dev/null | head -n 1); fi

set -o noclobber
if ! {
    printf 'timestamp_local=%s\n' "$(date '+%Y-%m-%d %H:%M:%S %Z')"
    printf 'timestamp_utc=%s\n' "$(date -u '+%Y-%m-%d %H:%M:%S UTC')"
    printf 'operating_system=%s\n' "$os_release"
    printf 'kernel=%s\n' "$kernel"
    printf 'cpu_details:\n%s\n' "$cpu_details"
    printf 'logical_cpu_count=%s\n' "$logical_cpus"
    printf 'compiler=%s\n' "$compiler_version"
    printf 'compiler_flags=-std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -pthread -Iinclude\n'
    printf 'workload=threads:%s,operations_per_thread:%s,total_operations:%s,lookups:80%%,updates:15%%,inserts:5%%,initial_keys:%s\n' "$THREADS" "$OPERATIONS_PER_THREAD" "$TOTAL_OPERATIONS" "$INITIAL_KEYS"
    printf 'strategies=single pthread mutex for all operations; OPAL-inspired C11 teaching prototype with optimistic lookups, function-pointer update batching, MCS-serialized insert/split path\n'
    printf 'warm_up_policy=one run per strategy, excluded from CSV\n'
    printf 'measured_runs_per_strategy=%s\n' "$RUNS"
    printf 'measured_pair_order=mutex_then_opal; opal_then_mutex; mutex_then_opal; opal_then_mutex; mutex_then_opal\n'
    printf 'timer_scope=tree initialization excluded; timer includes worker creation and join; post-run validation excluded\n'
    printf 'limitations=educational adaptation, not upstream OPAL; single global version and MCS writer queue; no memory reclamation, delete, range scan, NUMA, or PiBench integration\n'
    printf 'script=%s\n' benchmarks/run_opal_bptree_comparison.sh
} > "$ENVIRONMENT"; then
    echo "ERROR: Could not create environment snapshot safely." >&2
    exit 1
fi
if ! printf 'pair,order,mode,run,threads,operations_per_thread,lookup_ops,update_ops,insert_ops,optimistic_retries,update_callbacks,update_batches,elapsed_seconds,throughput_ops_sec\n' > "$CSV"; then
    echo "ERROR: Could not create CSV safely; environment snapshot is at $ENVIRONMENT." >&2
    exit 1
fi
set +o noclobber

field() {
    local label=$1 output=$2 count value
    count=$(printf '%s\n' "$output" | awk -F: -v label="$label" '$1 == label {n++} END {print n+0}')
    if [[ "$count" != 1 ]]; then
        echo "ERROR: Expected one '$label' field; found $count." >&2
        return 1
    fi
    value=$(printf '%s\n' "$output" | awk -F: -v label="$label" '$1 == label {sub(/^[ \t]+/, "", $2); sub(/[ \t]+$/, "", $2); print $2}')
    printf '%s' "$value"
}

run_one() {
    local mode=$1 pair=$2 order=$3 run=$4 record=$5
    local output actual_mode threads operations lookups updates inserts retries callbacks batches elapsed throughput result
    if ! output=$("$PROGRAM" "$mode" "$THREADS" "$OPERATIONS_PER_THREAD"); then
        echo "ERROR: $mode run failed (pair $pair, run $run)." >&2
        printf '%s\n' "$output" >&2
        return 1
    fi
    if ! actual_mode=$(field Mode "$output") ||
       ! threads=$(field Threads "$output") ||
       ! operations=$(field 'Operations per thread' "$output") ||
       ! lookups=$(field 'Lookup operations' "$output") ||
       ! updates=$(field 'Update operations' "$output") ||
       ! inserts=$(field 'Insert operations' "$output") ||
       ! retries=$(field 'Optimistic retries' "$output") ||
       ! callbacks=$(field 'Update callbacks' "$output") ||
       ! batches=$(field 'Update batches' "$output") ||
       ! elapsed=$(field 'Elapsed seconds' "$output") ||
       ! throughput=$(field 'Throughput ops/sec' "$output") ||
       ! result=$(field RESULT "$output"); then
        echo "ERROR: Could not parse $mode output (pair $pair, run $run)." >&2
        printf '%s\n' "$output" >&2
        return 1
    fi
    if [[ "$actual_mode" != "$mode" || "$threads" != "$THREADS" ||
          "$operations" != "$OPERATIONS_PER_THREAD" || "$lookups" != "$EXPECTED_LOOKUPS" ||
          "$updates" != "$EXPECTED_UPDATES" || "$inserts" != "$EXPECTED_INSERTS" ||
          "$callbacks" != "$EXPECTED_UPDATES" || "$result" != PASS ||
          ! "$retries" =~ ^[0-9]+$ || ! "$batches" =~ ^[0-9]+$ ||
          ! "$elapsed" =~ ^[0-9]+([.][0-9]+)?$ ||
          ! "$throughput" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
        echo "ERROR: Validation failed for $mode (pair $pair, run $run)." >&2
        printf '%s\n' "$output" >&2
        return 1
    fi
    if [[ "$mode" == opal && "$batches" -lt 1 ]]; then
        echo "ERROR: OPAL mode reported no update batches." >&2
        return 1
    fi
    if [[ "$record" == yes ]]; then
        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
            "$pair" "$order" "$mode" "$run" "$threads" "$operations" "$lookups" "$updates" "$inserts" \
            "$retries" "$callbacks" "$batches" "$elapsed" "$throughput" >> "$CSV"
    fi
    printf 'PASS: mode=%s pair=%s run=%s lookups=%s updates=%s inserts=%s retries=%s batches=%s elapsed=%ss throughput=%sops/sec\n' \
        "$mode" "$pair" "$run" "$lookups" "$updates" "$inserts" "$retries" "$batches" "$elapsed" "$throughput"
}

echo "OPAL-Inspired B+ Tree Comparison"
echo "Threads: $THREADS | Operations/thread: $OPERATIONS_PER_THREAD | Mix: 80% lookup, 15% update, 5% insert"
echo "Warm-ups: one per strategy (excluded from CSV)"
echo "Measured runs per strategy: $RUNS"
echo "CSV: $CSV"
echo "Environment: $ENVIRONMENT"
echo
run_one mutex warmup warmup 0 no
run_one opal warmup warmup 0 no
for pair in 1 2 3 4 5; do
    if (( pair % 2 == 1 )); then
        order=mutex_then_opal
        first=mutex
        second=opal
    else
        order=opal_then_mutex
        first=opal
        second=mutex
    fi
    run_one "$first" "$pair" "$order" "$pair" yes
    run_one "$second" "$pair" "$order" "$pair" yes
done
echo
echo "SUCCESS: all warm-ups and measured runs passed."
echo "Measured CSV rows: $((RUNS * 2))"
echo "CSV saved to: $CSV"
echo "Environment snapshot saved to: $ENVIRONMENT"
