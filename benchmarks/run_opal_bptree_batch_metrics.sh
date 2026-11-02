#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

PROGRAM=./opal_bptree_batch_metrics_benchmark
THREADS=4
OPERATIONS_PER_THREAD=10000
TOTAL_OPERATIONS=$((THREADS * OPERATIONS_PER_THREAD))
EXPECTED_UPDATES=$((TOTAL_OPERATIONS * 40 / 100))
RUNS=5
CSV=results/raw/opal_bptree_batch_metrics_low_overhead.csv
if [[ ! -x "$PROGRAM" ]]; then
  echo "ERROR: $PROGRAM is missing or not executable." >&2
  echo "Compile: gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -pthread -Iinclude src/opal_bptree.c src/opal_bptree_mix_benchmark.c -o opal_bptree_batch_metrics_benchmark" >&2
  exit 1
fi
mkdir -p results/raw
timestamp=$(date '+%Y%m%d_%H%M%S')
ENVIRONMENT="results/raw/opal_bptree_batch_metrics_environment_${timestamp}.txt"
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
  printf 'workload=balanced 50/40/10 lookup/update/insert; 4 threads; 10000 operations/thread\n'
  printf 'warm_up_policy=one run per strategy; excluded from CSV\nmeasured_pairs=%s\n' "$RUNS"
  printf 'pair_order=mutex_then_opal; opal_then_mutex; mutex_then_opal; opal_then_mutex; mutex_then_opal\n'
  printf 'batch_size_buckets=1;2;3-4;5-8;9+ callbacks\n'
  printf 'timer_scope=tree initialization excluded; worker creation and join included; post-run verification excluded\n'
  printf 'policy=existing immediate-drain combiner; counters only, no batching delay or policy change\nscript=%s\n' benchmarks/run_opal_bptree_batch_metrics.sh
} > "$ENVIRONMENT"
printf 'pair,order,mode,run,update_callbacks,update_batches,singleton_batches,max_batch_size,bucket_1,bucket_2,bucket_3_4,bucket_5_8,bucket_9_plus,elapsed_seconds,throughput_ops_sec\n' > "$CSV"
set +o noclobber

field() {
  local label=$1 output=$2 count value
  count=$(printf '%s\n' "$output" | awk -F: -v label="$label" '$1 == label {n++} END {print n+0}')
  [[ "$count" == 1 ]] || { echo "ERROR: Expected one '$label' field; found $count." >&2; return 1; }
  value=$(printf '%s\n' "$output" | awk -F: -v label="$label" '$1 == label {sub(/^[ \t]+/, "", $2); sub(/[ \t]+$/, "", $2); print $2}')
  printf '%s' "$value"
}
run_one() {
  local mode=$1 pair=$2 order=$3 run=$4 record=$5 output
  output=$("$PROGRAM" "$mode" "$THREADS" "$OPERATIONS_PER_THREAD" 50 40 10 uniform) || { printf '%s\n' "$output" >&2; return 1; }
  local actual_mode result callbacks batches singles maximum buckets elapsed throughput
  actual_mode=$(field Mode "$output"); result=$(field RESULT "$output")
  callbacks=$(field 'Update callbacks' "$output"); batches=$(field 'Update batches' "$output")
  singles=$(field 'Singleton update batches' "$output"); maximum=$(field 'Maximum update batch size' "$output")
  buckets=$(field 'Update batch size buckets (1,2,3-4,5-8,9+)' "$output")
  elapsed=$(field 'Elapsed seconds' "$output"); throughput=$(field 'Throughput ops/sec' "$output")
  IFS=, read -r b1 b2 b34 b58 b9 <<< "$buckets"
  if [[ "$actual_mode" != "$mode" || "$result" != PASS || "$callbacks" != "$EXPECTED_UPDATES" ||
        ! "$batches" =~ ^[0-9]+$ || ! "$singles" =~ ^[0-9]+$ || ! "$maximum" =~ ^[0-9]+$ ||
        ! "$b1" =~ ^[0-9]+$ || ! "$b2" =~ ^[0-9]+$ || ! "$b34" =~ ^[0-9]+$ ||
        ! "$b58" =~ ^[0-9]+$ || ! "$b9" =~ ^[0-9]+$ ||
        ! "$elapsed" =~ ^[0-9]+([.][0-9]+)?$ || ! "$throughput" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    echo "ERROR: Invalid output for $mode pair $pair." >&2; printf '%s\n' "$output" >&2; return 1
  fi
  if [[ "$mode" == mutex ]]; then
    [[ "$batches:$singles:$maximum:$b1:$b2:$b34:$b58:$b9" == 0:0:0:0:0:0:0:0 ]] || { echo 'ERROR: Mutex mode reported combiner metrics.' >&2; return 1; }
  else
    [[ "$batches" -gt 0 && "$maximum" -gt 0 && "$singles" == "$b1" &&
       $((b1+b2+b34+b58+b9)) -eq "$batches" ]] || { echo 'ERROR: OPAL batch metrics do not reconcile.' >&2; return 1; }
  fi
  if [[ "$record" == yes ]]; then
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
      "$pair" "$order" "$mode" "$run" "$callbacks" "$batches" "$singles" "$maximum" \
      "$b1" "$b2" "$b34" "$b58" "$b9" "$elapsed" "$throughput" >> "$CSV"
  fi
  printf 'PASS: mode=%s pair=%s callbacks=%s batches=%s singleton=%s max_batch=%s buckets=%s\n' \
    "$mode" "$pair" "$callbacks" "$batches" "$singles" "$maximum" "$buckets"
}

echo 'OPAL-Inspired B+ Tree Batch-Size Metrics'
echo "CSV: $CSV"
echo "Environment: $ENVIRONMENT"
run_one mutex warmup warmup 0 no
run_one opal warmup warmup 0 no
for pair in 1 2 3 4 5; do
  if (( pair % 2 == 1 )); then order=mutex_then_opal; first=mutex; second=opal
  else order=opal_then_mutex; first=opal; second=mutex; fi
  run_one "$first" "$pair" "$order" "$pair" yes
  run_one "$second" "$pair" "$order" "$pair" yes
done
echo 'SUCCESS: all warm-ups and measured batch-metrics runs passed.'
echo 'Measured CSV rows: 10'
echo "CSV saved to: $CSV"
echo "Environment snapshot saved to: $ENVIRONMENT"
