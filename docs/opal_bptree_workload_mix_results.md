# OPAL-Inspired B+ Tree Workload Mix Sweep

## Experiment

This follow-up varies only the lookup/update/insert mix. Each strategy uses
four threads and 10,000 operations per thread. Every mix has one warm-up per
strategy and five measured pairs with alternating strategy order. The timer
excludes tree initialization and verification, and includes worker creation
and joining.

| Workload | Lookup | Update | Insert |
|---|---:|---:|---:|
| Read-heavy | 80% | 15% | 5% |
| Balanced | 50% | 40% | 10% |
| Update-heavy | 20% | 70% | 10% |

The same C11 B+ Tree teaching prototype is compared with the single-mutex
baseline. The OPAL-inspired mode uses global optimistic version validation
for lookups, function-pointer callback batching for updates, and an MCS queue
for inserts that may split the tree. All writer callbacks and inserts are
serialized by this prototype’s global MCS queue.

## Results from this machine

The analyzer validated all 30 records. Values below are medians over five
runs per strategy and workload.

| Workload | Mutex throughput (ops/s) | OPAL-inspired throughput (ops/s) | Throughput ratio | OPAL retry average/run | OPAL batches average/run |
|---|---:|---:|---:|---:|---:|
| Read-heavy | 1,163,917.23 | 2,229,041.36 | 1.92x | 97,723 | 5,063 |
| Balanced | 836,159.74 | 1,051,924.82 | 1.26x | 104,152 | 11,763 |
| Update-heavy | 737,864.95 | 650,019.44 | 0.88x | 45,339 | 19,259 |

The operation-aware prototype led on the read-heavy and balanced mixes in
this run, while the mutex baseline led on the update-heavy mix. The result is
consistent with the local design’s higher lookup concurrency and its
serialized writer path, but these measurements do not establish a general
performance rule. The retry counter increases with the global version
protocol; a write anywhere in the tree can make an overlapping lookup retry.

The batch counts are close to callback counts, so many measured batches
contained one callback. This benchmark demonstrates function-pointer
dispatch and opportunistic batching, but it does not establish a large
batching benefit. The prototype also uses a global version and writer queue,
unlike upstream OPAL’s per-index-node design. It is not the upstream
implementation and does not reproduce the paper’s results.

## Reproduction

Run from the project root:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -pthread -Iinclude \
  src/opal_bptree.c src/opal_bptree_mix_benchmark.c \
  -o opal_bptree_mix_benchmark
bash benchmarks/run_opal_bptree_mix_sweep.sh
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 \
  benchmarks/analyze_opal_bptree_mix_sweep.c -lm \
  -o analyze_opal_bptree_mix_sweep
./analyze_opal_bptree_mix_sweep
```

The runner refuses to overwrite existing output. This run’s raw data and
environment snapshot are:

- [`results/raw/opal_bptree_mix_sweep.csv`](../results/raw/opal_bptree_mix_sweep.csv)
- [`results/raw/opal_bptree_mix_sweep_environment_20261028_214803.txt`](../results/raw/opal_bptree_mix_sweep_environment_20261028_214803.txt)

## Follow-up work

The controlled hot-key experiment is documented separately in
[`opal_bptree_hotkey_results.md`](opal_bptree_hotkey_results.md). Later work
also reviewed and measured update batch sizes, then tested a separate bounded
collection-window variant; see
[`opal_bptree_update_combiner_review.md`](opal_bptree_update_combiner_review.md),
[`opal_bptree_batch_metrics.md`](opal_bptree_batch_metrics.md), and
[`opal_bptree_batch_policy_results.md`](opal_bptree_batch_policy_results.md).
Separate [thread-count](opal_bptree_thread_scaling_results.md) and
[index-size](opal_bptree_index_size_results.md) sweeps have since been
completed. GCC ThreadSanitizer validation is recorded in
[`opal_bptree_tsan_results.md`](opal_bptree_tsan_results.md); Clang was
unavailable in the recorded environment.
