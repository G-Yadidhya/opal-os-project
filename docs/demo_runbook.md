# Project Demo Runbook

This demo uses the existing executables and preserved CSVs. It runs
correctness checks and analyzers, but it does not collect new benchmark
measurements or overwrite any raw result.

Run from the project root:

```bash
bash tests/check_counters.sh
bash tests/test_optimistic_lock.sh
bash tests/test_writer_mutex_optimistic_reader.sh
```

Expected indicators are `All counter correctness checks passed.`,
`SUCCESS: All 10 runs passed.`, and a hybrid-test `SUCCESS` summary. The
hybrid test checks both the 4-reader/2-writer case and the
4-reader/16-writer case.

Run one representative hybrid benchmark:

```bash
./writer_mutex_optimistic_reader_benchmark 4 2
```

Point out the optimistic reader strategy, pthread-mutex writer strategy,
the `value_b == -value_a` invariant, final even version, and `RESULT: PASS`.
Elapsed time and retry count vary between runs.

Show the separate OPAL-inspired B+ Tree prototype and preserved analyses:

```bash
./opal_bptree_correctness_batch_metrics_check
./analyze_opal_bptree_mix_sweep
./analyze_opal_bptree_hotkey_comparison
./analyze_opal_bptree_batch_metrics_check
./analyze_opal_bptree_batch_policy_comparison_check
./analyze_opal_bptree_thread_scaling_check
./analyze_opal_bptree_index_size_check
```

The B+ Tree correctness executable checks concurrent lookup, update, and
split-insert behavior. The analyzers validate the workload-mix, hot-key,
batch-size, and bounded-collection experiment CSVs without collecting new
measurements. The final two analyzers validate the thread-count and
index-size sweeps (50 and 30 rows). The report includes PNG and vector SVG
figures generated from preserved CSVs: operation-mix throughput, hot-key
throughput, the bounded-collection tradeoff, throughput across thread counts,
and throughput across initial tree sizes.

For the B+ Tree sanitizer evidence and the host-specific non-PIE/ASLR launch
requirements, see [`opal_bptree_tsan_results.md`](opal_bptree_tsan_results.md).

Show the preserved measurements and analysis:

```bash
./analyze_operation_aware_comparison
./analyze_locking_contention
head -n 3 results/raw/operation_aware_comparison.csv
head -n 3 results/raw/locking_comparison_balanced.csv
cat results/raw/operation_aware_environment_20261028_185945.txt
```

The first analyzer should validate 15 rows and show five measurements per
strategy. The second should validate four workload datasets. The CSV heads
show the stable schemas and raw rows; the environment snapshot records the
machine and configuration.

## Suggested explanation

1. The initial examples teach pthreads, races, mutexes, atomics, and critical
   sections.
2. The version-based benchmark validates optimistic reader snapshots and
   records retries separately from successful operations.
3. The workload-mix experiments hold four readers constant and increase
   writer count, showing how this prototype's retry counts and throughput
   changed in the recorded environment.
4. The hybrid comparison assigns optimistic validation to readers and a
   pthread mutex to writers. Its result is promising for one measured
   workload, but does not establish a general performance ranking.
5. The separate C11 B+ Tree teaching prototype adapts optimistic lookups,
   function-pointer update combining, and MCS-serialized update/insert paths.
   It is not the upstream OPAL index or its per-node locking design. The
   bounded collection-window result is a separate experimental clone. The
   thread sweep increases total work with thread count; the index-size sweep
   holds operation count fixed. Treat both as local measurements of this
   prototype.

The paper reports up to 2.43× throughput improvement and 80% latency
reduction in its own B+ Tree/ART evaluation. Keep those figures separate
from this project's measurements. See
[`project_report.md`](project_report.md) for the full report, results,
limitations, and citation.

The benchmark collection scripts refuse to overwrite their named CSVs. For
a fresh run, follow the isolated reproduction instructions in
[`benchmark_reproducibility.md`](benchmark_reproducibility.md) or use a new
experiment name and output path.
