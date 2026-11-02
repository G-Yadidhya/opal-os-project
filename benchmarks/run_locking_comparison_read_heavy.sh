#!/usr/bin/env bash

set -euo pipefail
cd "$(dirname "$0")/.."

OPTIMISTIC="./optimistic_benchmark"
MUTEX="./mutex_benchmark"
READERS=4
WRITERS=1
RUNS=5
EXPECTED_READS=$((READERS * 200000))
EXPECTED_UPDATES=$((WRITERS * 100000))
EXPECTED_TOTAL=$((EXPECTED_READS + EXPECTED_UPDATES))
CSV="results/raw/locking_comparison_read_heavy.csv"

for program in "$OPTIMISTIC" "$MUTEX"; do
    if [[ ! -x "$program" ]]; then
        echo "ERROR: $program is missing or not executable." >&2
        echo "Compile the benchmark programs before running this script." >&2
        exit 1
    fi
done

mkdir -p results/raw
timestamp=$(date '+%Y%m%d_%H%M%S')
ENVIRONMENT="results/raw/locking_comparison_read_heavy_environment_${timestamp}.txt"
if [[ -e "$CSV" || -e "$ENVIRONMENT" ]]; then
    echo "ERROR: Output already exists; refusing to overwrite." >&2
    [[ ! -e "$CSV" ]] || echo "  $CSV" >&2
    [[ ! -e "$ENVIRONMENT" ]] || echo "  $ENVIRONMENT" >&2
    exit 1
fi

os_release="unavailable"
if [[ -r /etc/os-release ]]; then
    os_release=$(awk -F= '/^(PRETTY_NAME|NAME|VERSION)=/ {gsub(/^"|"$/, "", $2); if (found++) out = out ";"; out = out $1 "=" $2} END {print out}' /etc/os-release)
    [[ -n "$os_release" ]] || os_release="unavailable"
fi
kernel=$(uname -srmo 2>/dev/null || printf 'unavailable')
cpu_details="unavailable"
if command -v lscpu >/dev/null 2>&1; then
    cpu_details=$(lscpu 2>/dev/null | awk -F: '/^(Architecture|Vendor ID|Model name|CPU family|Model|Thread\(s\) per core|Core\(s\) per socket|Socket\(s\)|CPU max MHz|CPU min MHz):/ {gsub(/^[ \t]+|[ \t]+$/, "", $1); gsub(/^[ \t]+|[ \t]+$/, "", $2); print $1 "=" $2}')
    [[ -n "$cpu_details" ]] || cpu_details="unavailable"
fi
logical_cpus=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf 'unavailable')
compiler_version="unavailable"
if command -v gcc >/dev/null 2>&1; then
    compiler_version=$(gcc --version 2>/dev/null | head -n 1)
fi

set -o noclobber
if ! {
    printf 'timestamp_local=%s\n' "$(date '+%Y-%m-%d %H:%M:%S %Z')"
    printf 'timestamp_utc=%s\n' "$(date -u '+%Y-%m-%d %H:%M:%S UTC')"
    printf 'operating_system=%s\n' "$os_release"
    printf 'kernel=%s\n' "$kernel"
    printf 'cpu_details:\n%s\n' "$cpu_details"
    printf 'logical_cpu_count=%s\n' "$logical_cpus"
    printf 'compiler=%s\n' "$compiler_version"
    printf 'compiler_flags=existing executables used; build flags are not recoverable; suggested build: -std=c11 -Wall -Wextra -O2 -pthread\n'
    printf 'workload=read-heavy,readers:%s,writers:%s,reader_iterations:200000,writer_iterations:100000,reads:800000,updates:100000,total_operations:900000\n' "$READERS" "$WRITERS"
    printf 'warm_up_policy=one run per mode (optimistic then mutex), excluded from CSV\n'
    printf 'measured_runs_per_mode=%s\n' "$RUNS"
    printf 'measured_order=optimistic,mutex; mutex,optimistic; optimistic,mutex; mutex,optimistic; optimistic,mutex\n'
    printf 'timer_scope=benchmark timer includes thread creation and join overhead\n'
    printf 'script=%s\n' "benchmarks/run_locking_comparison_read_heavy.sh"
} > "$ENVIRONMENT"; then
    echo "ERROR: Could not create environment snapshot safely." >&2
    exit 1
fi
if ! printf 'pair,order,mode,run,readers,writers,successful_reads,read_retries,invalid_snapshots,updates,total_operations,elapsed_seconds,throughput_ops_sec\n' > "$CSV"; then
    echo "ERROR: Could not create CSV safely; environment snapshot is at $ENVIRONMENT." >&2
    exit 1
fi
set +o noclobber

field() {
    local label=$1 output=$2 count value
    count=$(printf '%s\n' "$output" | awk -F: -v label="$label" '$1 == label { n++ } END { print n+0 }')
    if [[ "$count" != 1 ]]; then
        echo "ERROR: Expected exactly one '$label' field; found $count." >&2
        return 1
    fi
    value=$(printf '%s\n' "$output" | awk -F: -v label="$label" '$1 == label { sub(/^[ \t]+/, "", $2); sub(/[ \t]+$/, "", $2); print $2 }')
    printf '%s' "$value"
}

run_one() {
    local mode=$1 pair=$2 order=$3 run=$4 record=$5
    local program result readers writers successful_reads read_retries invalid_snapshots updates total_operations elapsed throughput pass
    if [[ "$mode" == optimistic ]]; then program=$OPTIMISTIC; else program=$MUTEX; fi

    if ! result=$("$program" "$READERS" "$WRITERS"); then
        echo "ERROR: $mode benchmark exited unsuccessfully (pair $pair, run $run)." >&2
        printf '%s\n' "$result" >&2
        return 1
    fi
    if ! readers=$(field 'Readers' "$result") ||
       ! writers=$(field 'Writers' "$result") ||
       ! successful_reads=$(field 'Successful reads' "$result") ||
       ! invalid_snapshots=$(field 'Invalid snapshots' "$result") ||
       ! updates=$(field 'Expected updates' "$result") ||
       ! total_operations=$(field 'Total operations' "$result") ||
       ! elapsed=$(field 'Elapsed seconds' "$result") ||
       ! throughput=$(field 'Throughput ops/sec' "$result") ||
       ! pass=$(field 'RESULT' "$result"); then
        echo "ERROR: Failed to parse benchmark output (pair $pair, run $run)." >&2
        printf '%s\n' "$result" >&2
        return 1
    fi
    if [[ "$mode" == optimistic ]]; then
        if ! read_retries=$(field 'Read retries' "$result"); then
            echo "ERROR: Optimistic output has no valid retry field." >&2
            printf '%s\n' "$result" >&2
            return 1
        fi
    else
        read_retries=0
    fi

    if [[ "$pass" != PASS || "$readers" != "$READERS" || "$writers" != "$WRITERS" ||
          "$successful_reads" != "$EXPECTED_READS" || "$updates" != "$EXPECTED_UPDATES" ||
          "$total_operations" != "$EXPECTED_TOTAL" || "$invalid_snapshots" != 0 ||
          ! "$read_retries" =~ ^[0-9]+$ ||
          ! "$elapsed" =~ ^[0-9]+([.][0-9]+)?$ ||
          ! "$throughput" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
        echo "ERROR: Validation failed for $mode (pair $pair, run $run)." >&2
        printf '%s\n' "$result" >&2
        return 1
    fi

    if [[ "$record" == yes ]]; then
        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
            "$pair" "$order" "$mode" "$run" "$readers" "$writers" \
            "$successful_reads" "$read_retries" "$invalid_snapshots" \
            "$updates" "$total_operations" "$elapsed" "$throughput" >> "$CSV"
    fi
    printf '%s\n' "PASS: mode=$mode pair=$pair run=$run elapsed=${elapsed}s throughput=${throughput}ops/sec retries=$read_retries"
}

echo "Read-heavy Locking Strategy Comparison"
echo "Readers: $READERS | Writers: $WRITERS"
echo "Warm-ups: one per mode (excluded from CSV)"
echo "Measured runs per mode: $RUNS"
echo "CSV: $CSV"
echo "Environment: $ENVIRONMENT"
echo

run_one optimistic warmup warmup 0 no
run_one mutex warmup warmup 0 no

for pair in 1 2 3 4 5; do
    if (( pair % 2 == 1 )); then
        first=optimistic
        order=optimistic_then_mutex
        second=mutex
    else
        first=mutex
        order=mutex_then_optimistic
        second=optimistic
    fi
    run_one "$first" "$pair" "$order" "$pair" yes
    run_one "$second" "$pair" "$order" "$pair" yes
done

echo
echo "SUCCESS: all warm-ups and measured runs passed."
echo "Measured CSV rows: $((RUNS * 2))"
echo "CSV saved to: $CSV"
echo "Environment snapshot saved to: $ENVIRONMENT"
