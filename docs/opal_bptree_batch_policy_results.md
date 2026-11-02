# OPAL-Inspired Bounded Batch Collection Experiment

## Experiment

This separate experiment compares the existing immediate-drain combiner with
an isolated source clone that spins on `CLOCK_MONOTONIC` for a nominal 1 μs
before detaching each nonempty update batch. The queue, callback execution,
MCS writer lock, and operation-aware paths otherwise match the main C11
prototype. The main `src/opal_bptree.c` implementation is used for the
immediate policy and remains unchanged by this experiment; the experimental
policy lives in `src/opal_bptree_collect1us_experiment.c`.

The window gives concurrent update callers a short opportunity to join the
same batch. It is a clock-based spin, not a sleep. Scheduling can make the
wall-clock delay longer than the nominal window. The comparison uses the
balanced 50/40/10 workload, four threads, and 10,000 operations per thread.
It performs one warm-up per policy and five measured pairs with alternating
order. Per-update call latency is sampled with the same clock instrumentation
in both builds; this sampling adds overhead to both policies.

## Results from this machine

The analyzer validated all 10 measurements. Values are medians over five
runs per policy, except singleton share, which is aggregated across all
measured batches.

| Metric | Immediate drain | 1 μs collection window |
|---|---:|---:|
| Throughput | 1,126,554.93 ops/s | 988,343.14 ops/s |
| Elapsed time | 0.035506480 s | 0.040471774 s |
| Update-call median latency | 5,006 ns | 6,909 ns |
| Update-call p95 latency | 17,775 ns | 19,489 ns |
| Median batches per run | 10,952 | 6,469 |
| Median singleton batches per run | 7,346 | 501 |
| Aggregate singleton share | 67.92% | 7.58% |
| Median maximum batch size | 4 | 4 |

The collection window reduced the number of batches and the singleton share,
but throughput was 0.877x the immediate policy in this run. Median update
latency increased by 38.01%, and p95 latency increased by 9.64%. This shows
the expected tradeoff for this particular fixed delay: more combining, with
additional wait time and lower measured throughput on this machine. It does
not show that other window sizes or workloads behave the same way, and this
is not an upstream OPAL implementation or a reproduction of the paper.

## Reproduction

Run from the project root:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -pthread -Iinclude \
  src/opal_bptree.c src/opal_bptree_batch_policy_benchmark.c \
  -o opal_bptree_immediate_policy_benchmark
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -pthread -Iinclude \
  src/opal_bptree_collect1us_experiment.c \
  src/opal_bptree_batch_policy_benchmark.c \
  -o opal_bptree_collect1us_policy_benchmark
bash benchmarks/run_opal_bptree_batch_policy_comparison.sh
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 \
  benchmarks/analyze_opal_bptree_batch_policy_comparison.c -lm \
  -o analyze_opal_bptree_batch_policy_comparison_check
./analyze_opal_bptree_batch_policy_comparison_check
```

The runner refuses to overwrite its output. Final files for the per-batch
window experiment:

- [`results/raw/opal_bptree_batch_policy_comparison_per_batch.csv`](../results/raw/opal_bptree_batch_policy_comparison_per_batch.csv)
- [`results/raw/opal_bptree_batch_policy_environment_20261028_225713.txt`](../results/raw/opal_bptree_batch_policy_environment_20261028_225713.txt)

An earlier exploratory capture used a window once per combiner-owner
activation. It is preserved at
[`results/raw/opal_bptree_batch_policy_comparison.csv`](../results/raw/opal_bptree_batch_policy_comparison.csv)
with its matching environment snapshot; it is not included in the table above.
