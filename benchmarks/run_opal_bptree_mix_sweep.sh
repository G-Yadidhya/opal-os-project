#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

PROGRAM=./opal_bptree_mix_benchmark
THREADS=4
OPERATIONS_PER_THREAD=10000
RUNS=5
TOTAL_OPERATIONS=$((THREADS * OPERATIONS_PER_THREAD))
INITIAL_KEYS=512
CSV=results/raw/opal_bptree_mix_sweep.csv
WORKLOADS=(read_heavy balanced update_heavy)
LOOKUP_PCTS=(80 50 20)
UPDATE_PCTS=(15 40 70)
INSERT_PCTS=(5 10 10)

if [[ ! -x "$PROGRAM" ]]; then
    echo "ERROR: $PROGRAM is missing or not executable." >&2
    echo "Compile: gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -pthread -Iinclude src/opal_bptree.c src/opal_bptree_mix_benchmark.c -o opal_bptree_mix_benchmark" >&2
    exit 1
fi

mkdir -p results/raw
timestamp=$(date '+%Y%m%d_%H%M%S')
ENVIRONMENT="results/raw/opal_bptree_mix_sweep_environment_${timestamp}.txt"
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
    printf 'threads=%s\noperations_per_thread=%s\ntotal_operations=%s\n' "$THREADS" "$OPERATIONS_PER_THREAD" "$TOTAL_OPERATIONS"
    printf 'workload_mixes=read_heavy:80/15/5; balanced:50/40/10; update_heavy:20/70/10 (lookup/update/insert percent)\n'
    printf 'strategies=single pthread mutex for all operations; OPAL-inspired C11 prototype with optimistic lookups, function-pointer update batching, MCS-serialized insert/split path\n'
    printf 'warm_up_policy=one run per strategy per workload, excluded from CSV\n'
    printf 'measured_runs_per_strategy_per_workload=%s\n' "$RUNS"
    printf 'pair_order=mutex_then_opal; opal_then_mutex; mutex_then_opal; opal_then_mutex; mutex_then_opal (repeated within each workload)\n'
    printf 'timer_scope=tree initialization excluded; timer includes worker creation and join; post-run verification excluded\n'
    printf 'limitations=local C11 teaching adaptation, not upstream OPAL; single global version and MCS writer queue; no deletes, reclamation, range scan, NUMA, or PiBench integration\n'
    printf 'script=%s\n' benchmarks/run_opal_bptree_mix_sweep.sh
} > "$ENVIRONMENT"; then
    echo "ERROR: Could not create environment snapshot safely." >&2
    exit 1
fi
if ! printf 'pair,order,workload,mode,run,threads,operations_per_thread,lookup_pct,update_pct,insert_pct,lookup_ops,update_ops,insert_ops,optimistic_retries,update_callbacks,update_batches,elapsed_seconds,throughput_ops_sec\n' > "$CSV"; then
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
    local mode=$1 workload=$2 pair=$3 order=$4 run=$5 record=$6
    local lookup_pct=$7 update_pct=$8 insert_pct=$9
    local expected_lookups=$((TOTAL_OPERATIONS * lookup_pct / 100))
    local expected_updates=$((TOTAL_OPERATIONS * update_pct / 100))
    local expected_inserts=$((TOTAL_OPERATIONS * insert_pct / 100))
    local expected_keys=$((INITIAL_KEYS + expected_inserts))
    local output actual_mode threads operations actual_lookup_pct actual_update_pct actual_insert_pct
    local lookups updates inserts final_keys final_expected retries callbacks batches elapsed throughput result
    if ! output=$("$PROGRAM" "$mode" "$THREADS" "$OPERATIONS_PER_THREAD" "$lookup_pct" "$update_pct" "$insert_pct"); then
        echo "ERROR: $mode run failed for $workload (pair $pair, run $run)." >&2
        printf '%s\n' "$output" >&2
        return 1
    fi
    if ! actual_mode=$(field Mode "$output") ||
       ! threads=$(field Threads "$output") ||
       ! operations=$(field 'Operations per thread' "$output") ||
       ! actual_lookup_pct=$(field 'Lookup percentage' "$output") ||
       ! actual_update_pct=$(field 'Update percentage' "$output") ||
       ! actual_insert_pct=$(field 'Insert percentage' "$output") ||
       ! lookups=$(field 'Lookup operations' "$output") ||
       ! updates=$(field 'Update operations' "$output") ||
       ! inserts=$(field 'Insert operations' "$output") ||
       ! final_keys=$(field 'Final keys' "$output") ||
       ! final_expected=$(field 'Expected keys' "$output") ||
       ! retries=$(field 'Optimistic retries' "$output") ||
       ! callbacks=$(field 'Update callbacks' "$output") ||
       ! batches=$(field 'Update batches' "$output") ||
       ! elapsed=$(field 'Elapsed seconds' "$output") ||
       ! throughput=$(field 'Throughput ops/sec' "$output") ||
       ! result=$(field RESULT "$output"); then
        echo "ERROR: Could not parse $mode output for $workload (pair $pair, run $run)." >&2
        printf '%s\n' "$output" >&2
        return 1
    fi
    if [[ "$actual_mode" != "$mode" || "$threads" != "$THREADS" ||
          "$operations" != "$OPERATIONS_PER_THREAD" ||
          "$actual_lookup_pct" != "$lookup_pct" || "$actual_update_pct" != "$update_pct" ||
          "$actual_insert_pct" != "$insert_pct" || "$lookups" != "$expected_lookups" ||
          "$updates" != "$expected_updates" || "$inserts" != "$expected_inserts" ||
          "$final_keys" != "$expected_keys" || "$final_expected" != "$expected_keys" ||
          "$callbacks" != "$expected_updates" || "$result" != PASS ||
          ! "$retries" =~ ^[0-9]+$ || ! "$batches" =~ ^[0-9]+$ ||
          ! "$elapsed" =~ ^[0-9]+([.][0-9]+)?$ ||
          ! "$throughput" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
        echo "ERROR: Validation failed for $mode/$workload (pair $pair, run $run)." >&2
        printf '%s\n' "$output" >&2
        return 1
    fi
    if [[ "$mode" == opal && "$batches" -lt 1 ]]; then
        echo "ERROR: OPAL mode reported no update batches for $workload." >&2
        return 1
    fi
    if [[ "$record" == yes ]]; then
        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
          "$pair" "$order" "$workload" "$mode" "$run" "$threads" "$operations" \
          "$lookup_pct" "$update_pct" "$insert_pct" "$lookups" "$updates" "$inserts" \
          "$retries" "$callbacks" "$batches" "$elapsed" "$throughput" >> "$CSV"
    fi
    printf 'PASS: workload=%s mode=%s pair=%s run=%s ops=%s/%s/%s retries=%s batches=%s elapsed=%ss\n' \
      "$workload" "$mode" "$pair" "$run" "$lookups" "$updates" "$inserts" "$retries" "$batches" "$elapsed"
}

echo "OPAL-Inspired B+ Tree Workload Mix Sweep"
echo "Threads: $THREADS | Operations/thread: $OPERATIONS_PER_THREAD | Runs: $RUNS per mode and workload"
echo "CSV: $CSV"
echo "Environment: $ENVIRONMENT"
echo
for index in 0 1 2; do
    workload=${WORKLOADS[$index]}
    lookup_pct=${LOOKUP_PCTS[$index]}
    update_pct=${UPDATE_PCTS[$index]}
    insert_pct=${INSERT_PCTS[$index]}
    echo "Workload: $workload ($lookup_pct/$update_pct/$insert_pct lookup/update/insert)"
    run_one mutex "$workload" warmup warmup 0 no "$lookup_pct" "$update_pct" "$insert_pct"
    run_one opal "$workload" warmup warmup 0 no "$lookup_pct" "$update_pct" "$insert_pct"
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
        run_one "$first" "$workload" "$pair" "$order" "$pair" yes "$lookup_pct" "$update_pct" "$insert_pct"
        run_one "$second" "$workload" "$pair" "$order" "$pair" yes "$lookup_pct" "$update_pct" "$insert_pct"
    done
    echo
done
echo "SUCCESS: all workload warm-ups and measured runs passed."
echo "Measured CSV rows: $((3 * RUNS * 2))"
echo "CSV saved to: $CSV"
echo "Environment snapshot saved to: $ENVIRONMENT"
