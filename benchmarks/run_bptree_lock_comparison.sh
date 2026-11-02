#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

PROGRAM=./bptree_lock_benchmark
THREADS=4
OPERATIONS_PER_THREAD=20000
READ_PERCENT=90
RUNS=5
EXPECTED_OPERATIONS=$((THREADS * OPERATIONS_PER_THREAD))
EXPECTED_READS=$((EXPECTED_OPERATIONS * READ_PERCENT / 100))
EXPECTED_INSERTS=$((EXPECTED_OPERATIONS - EXPECTED_READS))
INITIAL_KEYS=8192
CSV=results/raw/bptree_lock_comparison.csv

if [[ ! -x "$PROGRAM" ]]; then
    echo "ERROR: $PROGRAM is missing or not executable." >&2
    echo "Compile: gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -pthread -Iinclude src/concurrent_bptree.c src/bptree_lock_benchmark.c -o bptree_lock_benchmark" >&2
    exit 1
fi

mkdir -p results/raw
timestamp=$(date '+%Y%m%d_%H%M%S')
ENVIRONMENT="results/raw/bptree_lock_comparison_environment_${timestamp}.txt"
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
    printf 'workload=threads:%s,operations_per_thread:%s,read_percent:%s,expected_reads:%s,expected_inserts:%s,initial_keys:%s\n' "$THREADS" "$OPERATIONS_PER_THREAD" "$READ_PERCENT" "$EXPECTED_READS" "$EXPECTED_INSERTS" "$INITIAL_KEYS"
    printf 'strategies=tree-wide pthread mutex; pthread rwlock with shared lookup locks and exclusive insert locks\n'
    printf 'warm_up_policy=one run per strategy, excluded from CSV\n'
    printf 'measured_runs_per_strategy=%s\n' "$RUNS"
    printf 'measured_pair_order=mutex_then_rwlock; rwlock_then_mutex; mutex_then_rwlock; rwlock_then_mutex; mutex_then_rwlock\n'
    printf 'timer_scope=tree initialization excluded; timer includes worker thread creation and joining\n'
    printf 'script=%s\n' benchmarks/run_bptree_lock_comparison.sh
} > "$ENVIRONMENT"; then
    echo "ERROR: Could not create environment snapshot safely." >&2
    exit 1
fi
if ! printf 'pair,order,mode,run,threads,operations_per_thread,read_percent,completed_reads,completed_inserts,final_keys,expected_keys,elapsed_seconds,throughput_ops_sec\n' > "$CSV"; then
    echo "ERROR: Could not create CSV safely; environment snapshot is at $ENVIRONMENT." >&2
    exit 1
fi
set +o noclobber

field() {
    local label=$1 output=$2 count value
    count=$(printf '%s\n' "$output" | awk -F: -v label="$label" '$1 == label {n++} END {print n+0}')
    if [[ "$count" != 1 ]]; then
        echo "ERROR: Expected exactly one '$label' field; found $count." >&2
        return 1
    fi
    value=$(printf '%s\n' "$output" | awk -F: -v label="$label" '$1 == label {sub(/^[ \t]+/, "", $2); sub(/[ \t]+$/, "", $2); print $2}')
    printf '%s' "$value"
}

run_one() {
    local mode=$1 pair=$2 order=$3 run=$4 record=$5
    local output strategy threads operations read_percent reads inserts final_keys expected_keys elapsed throughput result
    if ! output=$("$PROGRAM" "$mode" "$THREADS" "$OPERATIONS_PER_THREAD" "$READ_PERCENT"); then
        echo "ERROR: $mode benchmark failed (pair $pair, run $run)." >&2
        printf '%s\n' "$output" >&2
        return 1
    fi
    if ! strategy=$(field Strategy "$output") ||
       ! threads=$(field Threads "$output") ||
       ! operations=$(field 'Operations per thread' "$output") ||
       ! read_percent=$(field 'Read percentage' "$output") ||
       ! reads=$(field 'Completed reads' "$output") ||
       ! inserts=$(field 'Completed inserts' "$output") ||
       ! final_keys=$(field 'Final keys' "$output") ||
       ! expected_keys=$(field 'Expected keys' "$output") ||
       ! elapsed=$(field 'Elapsed seconds' "$output") ||
       ! throughput=$(field 'Throughput ops/sec' "$output") ||
       ! result=$(field RESULT "$output"); then
        echo "ERROR: Could not parse $mode output (pair $pair, run $run)." >&2
        printf '%s\n' "$output" >&2
        return 1
    fi
    if [[ "$strategy" != "$mode" || "$threads" != "$THREADS" ||
          "$operations" != "$OPERATIONS_PER_THREAD" || "$read_percent" != "$READ_PERCENT" ||
          "$reads" != "$EXPECTED_READS" || "$inserts" != "$EXPECTED_INSERTS" ||
          "$final_keys" != "$expected_keys" || "$expected_keys" != "$((INITIAL_KEYS + EXPECTED_INSERTS))" ||
          "$result" != PASS || ! "$elapsed" =~ ^[0-9]+([.][0-9]+)?$ ||
          ! "$throughput" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
        echo "ERROR: Validation failed for $mode (pair $pair, run $run)." >&2
        printf '%s\n' "$output" >&2
        return 1
    fi
    if [[ "$record" == yes ]]; then
        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' "$pair" "$order" "$mode" "$run" "$threads" "$operations" "$read_percent" "$reads" "$inserts" "$final_keys" "$expected_keys" "$elapsed" "$throughput" >> "$CSV"
    fi
    printf 'PASS: mode=%s pair=%s run=%s reads=%s inserts=%s elapsed=%ss throughput=%sops/sec\n' "$mode" "$pair" "$run" "$reads" "$inserts" "$elapsed" "$throughput"
}

echo "B+ Tree Lock Comparison"
echo "Threads: $THREADS | Operations/thread: $OPERATIONS_PER_THREAD | Reads: $READ_PERCENT%"
echo "Warm-ups: one per strategy (excluded from CSV)"
echo "Measured runs per strategy: $RUNS"
echo "CSV: $CSV"
echo "Environment: $ENVIRONMENT"
echo
run_one mutex warmup warmup 0 no
run_one rwlock warmup warmup 0 no
for pair in 1 2 3 4 5; do
    if (( pair % 2 == 1 )); then
        order=mutex_then_rwlock
        first=mutex
        second=rwlock
    else
        order=rwlock_then_mutex
        first=rwlock
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
