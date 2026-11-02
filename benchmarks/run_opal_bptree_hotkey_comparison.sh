#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

PROGRAM=./opal_bptree_mix_benchmark
THREADS=4
OPERATIONS_PER_THREAD=10000
TOTAL_OPERATIONS=$((THREADS * OPERATIONS_PER_THREAD))
RUNS=5
CSV=results/raw/opal_bptree_hotkey_comparison.csv
if [[ ! -x "$PROGRAM" ]]; then
  echo "ERROR: $PROGRAM is missing or not executable." >&2
  echo "Compile with gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -pthread -Iinclude src/opal_bptree.c src/opal_bptree_mix_benchmark.c -o opal_bptree_mix_benchmark" >&2
  exit 1
fi
mkdir -p results/raw
timestamp=$(date '+%Y%m%d_%H%M%S')
ENVIRONMENT="results/raw/opal_bptree_hotkey_environment_${timestamp}.txt"
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
cpu_details=unavailable
if command -v lscpu >/dev/null 2>&1; then
  cpu_details=$(lscpu 2>/dev/null | awk -F: '/^(Architecture|Vendor ID|Model name|CPU family|Model|Thread\(s\) per core|Core\(s\) per socket|Socket\(s\)|CPU max MHz|CPU min MHz):/ {gsub(/^[ \t]+|[ \t]+$/, "", $1); gsub(/^[ \t]+|[ \t]+$/, "", $2); print $1 "=" $2}')
  [[ -n "$cpu_details" ]] || cpu_details=unavailable
fi
compiler_version=unavailable
if command -v gcc >/dev/null 2>&1; then compiler_version=$(gcc --version 2>/dev/null | head -n 1); fi
set -o noclobber
{
  printf 'timestamp_local=%s\ntimestamp_utc=%s\n' "$(date '+%Y-%m-%d %H:%M:%S %Z')" "$(date -u '+%Y-%m-%d %H:%M:%S UTC')"
  printf 'operating_system=%s\nkernel=%s\n' "$os_release" "$(uname -srmo 2>/dev/null || printf unavailable)"
  printf 'cpu_details:\n%s\nlogical_cpu_count=%s\ncompiler=%s\n' "$cpu_details" "$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf unavailable)" "$compiler_version"
  printf 'compiler_flags=-std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -pthread -Iinclude\n'
  printf 'threads=%s\noperations_per_thread=%s\noperation_mix=50/40/10 lookup/update/insert\n' "$THREADS" "$OPERATIONS_PER_THREAD"
  printf 'distributions=uniform over 512 existing keys; skew80_20 routes 80%% of lookup/update accesses to 20%% (102) hot keys and 20%% to remaining 410 keys\n'
  printf 'warm_up_policy=one run per strategy per distribution, excluded from CSV\nmeasured_pairs_per_distribution=%s\n' "$RUNS"
  printf 'pair_order=mutex_then_opal; opal_then_mutex; mutex_then_opal; opal_then_mutex; mutex_then_opal, repeated per distribution\n'
  printf 'timer_scope=tree initialization excluded; worker creation and join included; post-run verification excluded\n'
  printf 'script=%s\n' benchmarks/run_opal_bptree_hotkey_comparison.sh
} > "$ENVIRONMENT"
printf 'pair,order,distribution,mode,run,threads,operations_per_thread,lookup_pct,update_pct,insert_pct,lookup_ops,update_ops,insert_ops,optimistic_retries,update_callbacks,update_batches,elapsed_seconds,throughput_ops_sec\n' > "$CSV"
set +o noclobber

field() {
  local label=$1 output=$2 count value
  count=$(printf '%s\n' "$output" | awk -F: -v label="$label" '$1 == label {n++} END {print n+0}')
  [[ "$count" == 1 ]] || { echo "ERROR: Expected one '$label' field." >&2; return 1; }
  value=$(printf '%s\n' "$output" | awk -F: -v label="$label" '$1 == label {sub(/^[ \t]+/, "", $2); sub(/[ \t]+$/, "", $2); print $2}')
  printf '%s' "$value"
}
run_one() {
  local mode=$1 distribution=$2 pair=$3 order=$4 run=$5 record=$6
  local output actual_mode actual_dist lookups updates inserts retries callbacks batches elapsed throughput result
  output=$("$PROGRAM" "$mode" "$THREADS" "$OPERATIONS_PER_THREAD" 50 40 10 "$distribution") || { printf '%s\n' "$output" >&2; return 1; }
  actual_mode=$(field Mode "$output"); actual_dist=$(field 'Key distribution' "$output")
  lookups=$(field 'Lookup operations' "$output"); updates=$(field 'Update operations' "$output"); inserts=$(field 'Insert operations' "$output")
  retries=$(field 'Optimistic retries' "$output"); callbacks=$(field 'Update callbacks' "$output"); batches=$(field 'Update batches' "$output")
  elapsed=$(field 'Elapsed seconds' "$output"); throughput=$(field 'Throughput ops/sec' "$output"); result=$(field RESULT "$output")
  local expected_distribution=$distribution
  [[ "$distribution" == skew80_20 ]] && expected_distribution=skew80_20
  if [[ "$actual_mode" != "$mode" || "$actual_dist" != "$expected_distribution" ||
        "$lookups" != $((TOTAL_OPERATIONS * 50 / 100)) || "$updates" != $((TOTAL_OPERATIONS * 40 / 100)) ||
        "$inserts" != $((TOTAL_OPERATIONS * 10 / 100)) || "$callbacks" != $((TOTAL_OPERATIONS * 40 / 100)) ||
        "$result" != PASS || ! "$retries" =~ ^[0-9]+$ || ! "$batches" =~ ^[0-9]+$ ||
        ! "$elapsed" =~ ^[0-9]+([.][0-9]+)?$ || ! "$throughput" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    echo "ERROR: Run validation failed ($mode/$distribution, pair $pair)." >&2; printf '%s\n' "$output" >&2; return 1
  fi
  if [[ "$mode" == mutex && ( "$retries" != 0 || "$batches" != 0 ) ]] ||
     [[ "$mode" == opal && "$batches" -lt 1 ]]; then echo "ERROR: Strategy counters inconsistent." >&2; return 1; fi
  if [[ "$record" == yes ]]; then
    printf '%s,%s,%s,%s,%s,%s,%s,50,40,10,%s,%s,%s,%s,%s,%s,%s,%s\n' \
      "$pair" "$order" "$distribution" "$mode" "$run" "$THREADS" "$OPERATIONS_PER_THREAD" \
      "$lookups" "$updates" "$inserts" "$retries" "$callbacks" "$batches" "$elapsed" "$throughput" >> "$CSV"
  fi
  printf 'PASS: distribution=%s mode=%s pair=%s run=%s elapsed=%ss retries=%s batches=%s\n' "$distribution" "$mode" "$pair" "$run" "$elapsed" "$retries" "$batches"
}

for distribution in uniform skew80_20; do
  echo "Key distribution: $distribution (balanced 50/40/10)"
  run_one mutex "$distribution" warmup warmup 0 no
  run_one opal "$distribution" warmup warmup 0 no
  for pair in 1 2 3 4 5; do
    if (( pair % 2 == 1 )); then order=mutex_then_opal; first=mutex; second=opal
    else order=opal_then_mutex; first=opal; second=mutex; fi
    run_one "$first" "$distribution" "$pair" "$order" "$pair" yes
    run_one "$second" "$distribution" "$pair" "$order" "$pair" yes
  done
done
echo "SUCCESS: all hot-key warm-ups and measured runs passed."
echo "Measured CSV rows: $((2 * RUNS * 2))"
echo "CSV saved to: $CSV"
echo "Environment snapshot saved to: $ENVIRONMENT"
