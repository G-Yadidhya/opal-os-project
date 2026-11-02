#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
PROGRAM=./opal_bptree_benchmark
THREAD_COUNTS=(1 2 4 8 16)
OPS=10000
RUNS=5
CSV=results/raw/opal_bptree_thread_scaling.csv
mkdir -p results/raw
stamp=$(date '+%Y%m%d_%H%M%S')
ENV="results/raw/opal_bptree_thread_scaling_environment_${stamp}.txt"
[[ -x "$PROGRAM" ]] || { echo "ERROR: $PROGRAM missing or not executable." >&2; exit 1; }
[[ ! -e "$CSV" && ! -e "$ENV" ]] || { echo 'ERROR: output exists; refusing to overwrite.' >&2; exit 1; }
os=unavailable; [[ -r /etc/os-release ]] && os=$(awk -F= '/^PRETTY_NAME=/ {gsub(/^"|"$/, "", $2); print $2}' /etc/os-release)
cpu=unavailable; command -v lscpu >/dev/null 2>&1 && cpu=$(lscpu 2>/dev/null | awk -F: '/^(Model name|CPU\(s\)|Thread\(s\) per core|Core\(s\) per socket|Socket\(s\)):/ {gsub(/^[ \t]+|[ \t]+$/, "", $1); gsub(/^[ \t]+|[ \t]+$/, "", $2); print $1 "=" $2}')
gcc_version=unavailable; command -v gcc >/dev/null 2>&1 && gcc_version=$(gcc --version | head -n1)
set -o noclobber
{
  printf 'timestamp_local=%s\ntimestamp_utc=%s\n' "$(date '+%Y-%m-%d %H:%M:%S %Z')" "$(date -u '+%Y-%m-%d %H:%M:%S UTC')"
  printf 'os=%s\nkernel=%s\ncpu_details:\n%s\nlogical_cpu_count=%s\ncompiler=%s\n' "$os" "$(uname -srmo 2>/dev/null || echo unavailable)" "$cpu" "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo unavailable)" "$gcc_version"
  printf 'binary_build_flags=unknown; pre-existing executable\nthread_counts=%s\noperations_per_thread=%s\nworkload_mix=80/15/5 lookup/update/insert\n' "${THREAD_COUNTS[*]}" "$OPS"
  printf 'warmups=one per strategy and thread count; excluded\nmeasured_pairs=%s per strategy and thread count\npair_order=mutex_then_opal; opal_then_mutex; alternating\ntimer=tree initialization and validation excluded; includes worker create/join\nscript=%s\n' "$RUNS" benchmarks/run_opal_bptree_thread_scaling.sh
} > "$ENV"
printf 'thread_count,pair,order,mode,run,operations_per_thread,total_operations,lookup_ops,update_ops,insert_ops,optimistic_retries,update_callbacks,update_batches,elapsed_seconds,throughput_ops_sec\n' > "$CSV"
set +o noclobber
field() { local n; n=$(printf '%s\n' "$2" | awk -F: -v k="$1" '$1==k{n++} END{print n+0}'); [[ $n == 1 ]] || { echo "ERROR: field $1 count=$n" >&2; return 1; }; printf '%s' "$2" | awk -F: -v k="$1" '$1==k{sub(/^[ \t]+/,"",$2);sub(/[ \t]+$/,"",$2);print $2}'; }
run_one() {
  local mode=$1 t=$2 pair=$3 order=$4 run=$5 record=$6 total=$((t*OPS)) l=$((t*OPS*80/100)) u=$((t*OPS*15/100)) i=$((t*OPS*5/100)) out
  out=$("$PROGRAM" "$mode" "$t" "$OPS") || { echo "ERROR: benchmark failed: $mode/$t/$pair/$run" >&2; printf '%s\n' "$out" >&2; return 1; }
  local m th op lo up ins fk ek retry cb batches sec rate result
  m=$(field Mode "$out"); th=$(field Threads "$out"); op=$(field 'Operations per thread' "$out")
  lo=$(field 'Lookup operations' "$out"); up=$(field 'Update operations' "$out"); ins=$(field 'Insert operations' "$out")
  fk=$(field 'Final keys' "$out"); ek=$(field 'Expected keys' "$out"); retry=$(field 'Optimistic retries' "$out")
  cb=$(field 'Update callbacks' "$out"); batches=$(field 'Update batches' "$out"); sec=$(field 'Elapsed seconds' "$out")
  rate=$(field 'Throughput ops/sec' "$out"); result=$(field RESULT "$out")
  [[ $m == "$mode" && $th == "$t" && $op == "$OPS" && $lo == "$l" && $up == "$u" && $ins == "$i" && $fk == $((512+i)) && $ek == "$fk" && $cb == "$u" && $result == PASS && $retry =~ ^[0-9]+$ && $batches =~ ^[0-9]+$ && $sec =~ ^[0-9]+([.][0-9]+)?$ && $rate =~ ^[0-9]+([.][0-9]+)?$ ]] || { echo "ERROR: validation failed: $mode/$t/$pair/$run" >&2; printf '%s\n' "$out" >&2; return 1; }
  if [[ $mode == mutex ]]; then [[ $retry == 0 && $batches == 0 ]] || return 1; else [[ $batches -gt 0 ]] || return 1; fi
  if [[ $record == yes ]]; then printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' "$t" "$pair" "$order" "$mode" "$run" "$OPS" "$total" "$l" "$u" "$i" "$retry" "$cb" "$batches" "$sec" "$rate" >> "$CSV"; fi
  printf 'PASS threads=%s pair=%s mode=%s run=%s total=%s elapsed=%ss throughput=%sops/s\n' "$t" "$pair" "$mode" "$run" "$total" "$sec" "$rate"
}
echo 'B+Tree thread scaling: 1,2,4,8,16 threads; 10,000 ops/thread; 80/15/5 mix; five pairs/mode'
for t in "${THREAD_COUNTS[@]}"; do
  echo "Thread count $t: warm-ups"
  run_one mutex "$t" warmup warmup 0 no
  run_one opal "$t" warmup warmup 0 no
  for pair in 1 2 3 4 5; do
    if (( pair % 2 )); then order=mutex_then_opal; first=mutex; second=opal; else order=opal_then_mutex; first=opal; second=mutex; fi
    run_one "$first" "$t" "$pair" "$order" "$pair" yes
    run_one "$second" "$t" "$pair" "$order" "$pair" yes
  done
done
echo 'SUCCESS: all warm-ups and measured runs passed.'
echo 'Measured CSV rows: 50'
echo "CSV saved to: $CSV"
echo "Environment snapshot saved to: $ENV"
