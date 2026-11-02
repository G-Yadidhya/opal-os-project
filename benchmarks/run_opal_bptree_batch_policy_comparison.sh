#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
IMMEDIATE=./opal_bptree_immediate_policy_benchmark
COLLECT=./opal_bptree_collect1us_policy_benchmark
THREADS=4
OPERATIONS_PER_THREAD=10000
TOTAL_OPERATIONS=$((THREADS * OPERATIONS_PER_THREAD))
EXPECTED_UPDATES=$((TOTAL_OPERATIONS * 40 / 100))
RUNS=5
CSV=results/raw/opal_bptree_batch_policy_comparison_per_batch.csv
for program in "$IMMEDIATE" "$COLLECT"; do
  if [[ ! -x "$program" ]]; then
    echo "ERROR: $program is missing or not executable." >&2
    echo "Build commands are listed in docs/opal_bptree_batch_policy_results.md." >&2
    exit 1
  fi
done
mkdir -p results/raw
timestamp=$(date '+%Y%m%d_%H%M%S')
ENVIRONMENT="results/raw/opal_bptree_batch_policy_environment_${timestamp}.txt"
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
  printf 'workload=balanced 50/40/10 lookup/update/insert; %s threads; %s operations/thread\n' "$THREADS" "$OPERATIONS_PER_THREAD"
  printf 'policy_immediate=existing OPAL-inspired combiner, drains immediately\npolicy_collect1us=separate experiment clone, waits up to 1000ns before each queued batch detach\n'
  printf 'warm_up_policy=one run per policy; excluded from CSV\nmeasured_pairs=%s\npair_order=immediate_then_collect1us; collect1us_then_immediate, alternating\n' "$RUNS"
  printf 'latency_scope=clock_gettime around each update call; instrumentation applies equally to both policies\n'
  printf 'timer_scope=tree initialization excluded; worker creation/join included; validation excluded\nscript=%s\n' benchmarks/run_opal_bptree_batch_policy_comparison.sh
} > "$ENVIRONMENT"
printf 'pair,order,policy,run,update_callbacks,update_batches,singleton_batches,max_batch_size,bucket_1,bucket_2,bucket_3_4,bucket_5_8,bucket_9_plus,update_latency_median_ns,update_latency_p95_ns,elapsed_seconds,throughput_ops_sec\n' > "$CSV"
set +o noclobber
field() {
  local label=$1 output=$2 count value
  count=$(printf '%s\n' "$output" | awk -F: -v label="$label" '$1 == label {n++} END {print n+0}')
  [[ "$count" == 1 ]] || { echo "ERROR: Expected one '$label' field; found $count." >&2; return 1; }
  value=$(printf '%s\n' "$output" | awk -F: -v label="$label" '$1 == label {sub(/^[ \t]+/, "", $2); sub(/[ \t]+$/, "", $2); print $2}')
  printf '%s' "$value"
}
run_one() {
  local policy=$1 pair=$2 order=$3 run=$4 record=$5 program label output
  if [[ "$policy" == immediate ]]; then program=$IMMEDIATE; label=immediate
  else program=$COLLECT; label=collect1us; fi
  output=$("$program" "$label" "$OPERATIONS_PER_THREAD") || { printf '%s\n' "$output" >&2; return 1; }
  local actual_policy result callbacks batches singles maximum buckets median_ns p95_ns elapsed throughput
  actual_policy=$(field Policy "$output"); result=$(field RESULT "$output")
  callbacks=$(field 'Update callbacks' "$output"); batches=$(field 'Update batches' "$output")
  singles=$(field 'Singleton update batches' "$output"); maximum=$(field 'Maximum update batch size' "$output")
  buckets=$(field 'Update batch size buckets (1,2,3-4,5-8,9+)' "$output")
  median_ns=$(field 'Update latency median ns' "$output"); p95_ns=$(field 'Update latency p95 ns' "$output")
  elapsed=$(field 'Elapsed seconds' "$output"); throughput=$(field 'Throughput ops/sec' "$output")
  IFS=, read -r b1 b2 b34 b58 b9 <<< "$buckets"
  if [[ "$actual_policy" != "$label" || "$result" != PASS || "$callbacks" != "$EXPECTED_UPDATES" ||
        ! "$batches" =~ ^[0-9]+$ || ! "$singles" =~ ^[0-9]+$ || ! "$maximum" =~ ^[0-9]+$ ||
        ! "$b1" =~ ^[0-9]+$ || ! "$b2" =~ ^[0-9]+$ || ! "$b34" =~ ^[0-9]+$ || ! "$b58" =~ ^[0-9]+$ || ! "$b9" =~ ^[0-9]+$ ||
        ! "$median_ns" =~ ^[0-9]+$ || ! "$p95_ns" =~ ^[0-9]+$ || "$median_ns" -le 0 || "$p95_ns" -lt "$median_ns" ||
        ! "$elapsed" =~ ^[0-9]+([.][0-9]+)?$ || ! "$throughput" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    echo "ERROR: Invalid output for $label pair $pair." >&2; printf '%s\n' "$output" >&2; return 1
  fi
  [[ "$batches" -gt 0 && "$singles" == "$b1" && $((b1+b2+b34+b58+b9)) -eq "$batches" ]] || { echo "ERROR: Batch counters do not reconcile for $label." >&2; return 1; }
  if [[ "$record" == yes ]]; then
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
      "$pair" "$order" "$label" "$run" "$callbacks" "$batches" "$singles" "$maximum" \
      "$b1" "$b2" "$b34" "$b58" "$b9" "$median_ns" "$p95_ns" "$elapsed" "$throughput" >> "$CSV"
  fi
  printf 'PASS: policy=%s pair=%s batches=%s singleton=%s max_batch=%s update_p50=%sns update_p95=%sns elapsed=%ss\n' \
    "$label" "$pair" "$batches" "$singles" "$maximum" "$median_ns" "$p95_ns" "$elapsed"
}
echo 'OPAL-Inspired B+ Tree Batch Collection Policy Comparison'
echo "CSV: $CSV"
echo "Environment: $ENVIRONMENT"
run_one immediate warmup warmup 0 no
run_one collect1us warmup warmup 0 no
for pair in 1 2 3 4 5; do
  if (( pair % 2 == 1 )); then order=immediate_then_collect1us; first=immediate; second=collect1us
  else order=collect1us_then_immediate; first=collect1us; second=immediate; fi
  run_one "$first" "$pair" "$order" "$pair" yes
  run_one "$second" "$pair" "$order" "$pair" yes
done
echo 'SUCCESS: all collection-policy warm-ups and measured runs passed.'
echo 'Measured CSV rows: 10'
echo "CSV saved to: $CSV"
echo "Environment snapshot saved to: $ENVIRONMENT"
