# OPAL-Inspired B+ Tree Update Batch Metrics

## Scope

This checkpoint adds counters to observe the existing function-pointer
combiner. It does not change the queueing policy, add a delay, change the
writer lock, or alter which operation uses which synchronization path. The
combiner counts each request as it already walks the detached batch to run its
callback, then records the batch in one of five buckets: 1, 2, 3-4, 5-8, or
9+ requests. It also records singleton count and maximum batch size.

The additional atomic counter updates add some measurement overhead. These
throughput values describe the instrumented build and should not be compared
directly with earlier uninstrumented benchmark results. The instrumentation
does not insert another traversal of the request list or a collection wait.

## Experiment and results

The benchmark uses the balanced 50/40/10 lookup/update/insert workload, four
threads, and 10,000 operations per thread. It performs one warm-up per mode,
then five measured pairs with alternating order. The analyzer validated all
10 measured rows, the 16,000 update callbacks in each row, pair order, bucket
totals, singleton counts, maximum-size consistency, and throughput values.

| OPAL metric | Result |
|---|---:|
| Median batches per run | 11,548 |
| Median singleton batches per run | 8,257 |
| Singleton share of measured batches | 72.01% |
| Median maximum batch size | 4 requests |
| Mean batches/run of size 1 | 8,335.4 |
| Mean batches/run of size 2 | 2,168.4 |
| Mean batches/run of size 3-4 | 1,071.6 |
| Mean batches/run of size 5-8 | 0 |
| Mean batches/run of size 9+ | 0 |

The immediate-drain combiner mostly handles singleton batches in this run;
the largest batch observed had four requests. This supports measuring a
bounded collection policy as a separate future experiment, but does not
establish that delaying updates would improve end-to-end performance. Such a
policy would change the implementation and should be evaluated separately
with update latency as well as throughput.

## Reproduction

Run from the project root:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -pthread -Iinclude \
  src/opal_bptree.c tests/test_opal_bptree.c \
  -o opal_bptree_correctness_batch_metrics_check
./opal_bptree_correctness_batch_metrics_check
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -pthread -Iinclude \
  src/opal_bptree.c src/opal_bptree_mix_benchmark.c \
  -o opal_bptree_batch_metrics_benchmark
bash benchmarks/run_opal_bptree_batch_metrics.sh
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 \
  benchmarks/analyze_opal_bptree_batch_metrics.c -lm \
  -o analyze_opal_bptree_batch_metrics_check
./analyze_opal_bptree_batch_metrics_check
```

The runner refuses to overwrite the CSV or environment snapshot. Final raw
files for this run:

- [`results/raw/opal_bptree_batch_metrics_low_overhead.csv`](../results/raw/opal_bptree_batch_metrics_low_overhead.csv)
- [`results/raw/opal_bptree_batch_metrics_environment_20261028_223219.txt`](../results/raw/opal_bptree_batch_metrics_environment_20261028_223219.txt)

The earlier draft capture, which counted each batch in an extra pass, is
preserved separately at
[`results/raw/opal_bptree_batch_metrics.csv`](../results/raw/opal_bptree_batch_metrics.csv).
The final run above uses the single-pass counter collection.
