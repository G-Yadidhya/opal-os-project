# OPAL-Inspired C11 B+ Tree Teaching Prototype

## Purpose and scope

This extension adds a small concurrent integer-key B+ Tree to the existing
OS/concurrency project. It follows the operation categories described in the
OPAL paper while staying within this project’s C11, pthread, and C11 atomics
constraints.

The local implementation is a teaching adaptation. It is not the upstream
OPAL lock or index, does not reproduce the paper’s implementation, and does
not establish the paper’s reported results.

## Synchronization paths

The operation-aware mode has three paths:

- **Lookup:** reads the global sequence version before and after traversing
  the tree and retries if a writer overlaps the lookup. Node keys, values,
  counts, and pointers are atomic so an overlapping optimistic traversal does
  not create C data races. Nodes are retained until the tree is destroyed.
- **Update:** callers enqueue function-pointer requests. One combiner drains
  the pending requests and executes the callbacks in a batch. The combiner
  enters the writer queue once for each drained batch.
- **Insert and possible split:** inserts use an MCS queue lock. The same global
  version tells optimistic readers to retry while the tree structure changes.

The baseline mode protects lookup, update, and insert with one pthread mutex.
Both modes use the same B+ Tree representation, workload, and correctness
checks.

The local design uses one global version and one global MCS writer queue. The
upstream design uses its own lock word and applies synchronization to index
nodes; this project does not reproduce those details. In this prototype,
updates also enter the global MCS writer queue while the combiner executes a
batch. Consequently, batching and queueing are demonstrated as operation
paths, but this does not match the upstream implementation’s finer-grained
coordination.

## Correctness invariants

- Keys remain ordered within leaves; internal separators route searches to the
  correct child; leaf and internal splits preserve all keys.
- Every successful unique insert increases the distinct-key count once.
- Updates change only the selected existing key’s value and do not alter tree
  shape.
- A lookup returns a value only after confirming that the global version was
  unchanged and even during the traversal.
- The mutex baseline serializes all operations. In operation-aware mode, all
  writers are serialized by the MCS queue; readers validate the version.
- No node is reclaimed while worker threads may still hold a pointer.

The test inserts and updates concurrently while readers check seeded keys. It
then verifies every key/value and the expected update total in both modes.

## Build and test

Run these commands from the project root:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -pthread -Iinclude \
  src/opal_bptree.c tests/test_opal_bptree.c -o opal_bptree_correctness_test
./opal_bptree_correctness_test
```

The test prints `RESULT: PASS` after validating the mutex baseline and the
OPAL-inspired mode.

## Benchmark and analysis

The benchmark uses four threads with 10,000 operations per thread: 80%
lookups, 15% updates, and 5% inserts. It performs one warm-up per strategy,
then five measured pairs in alternating order. Tree initialization and
post-run verification are outside the timer; worker thread creation and join
are inside it. The runner refuses to replace the output CSV.

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -pthread -Iinclude \
  src/opal_bptree.c src/opal_bptree_benchmark.c -o opal_bptree_benchmark
bash benchmarks/run_opal_bptree_comparison.sh
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 \
  benchmarks/analyze_opal_bptree_comparison.c -lm \
  -o analyze_opal_bptree_comparison
./analyze_opal_bptree_comparison
```

Raw measurements and environment metadata are preserved in:

- [`results/raw/opal_bptree_comparison.csv`](../results/raw/opal_bptree_comparison.csv)
- [`results/raw/opal_bptree_comparison_environment_20261028_200645.txt`](../results/raw/opal_bptree_comparison_environment_20261028_200645.txt)

The analyzer validated ten records and reported these medians for this one
workload and machine:

| Strategy | Median elapsed time (s) | Median throughput (ops/s) |
|---|---:|---:|
| Single-mutex baseline | 0.034505020 | 1,159,251.61 |
| OPAL-inspired C11 prototype | 0.018090631 | 2,211,089.26 |

The local throughput ratio was 1.91x, and median elapsed time was 47.57%
lower for the prototype in this run. The measured prototype averaged 95,653
optimistic retries and 4,986 batches for 6,000 update callbacks; therefore,
most measured batches contained only one callback. These figures describe
this build, workload, and machine only. They are not comparable to the
paper’s evaluation and do not establish a general performance advantage.

## Limitations and next work

This prototype has a global version, global writer serialization, no delete,
range scan, memory reclamation, NUMA placement, or PiBench integration. The
separate workload benchmark supports a controlled key-skew experiment. The
prototype does not implement the upstream lock word, per-node
OPAL lock instances, or the full B+ Tree/ART benchmark suite. AddressSanitizer
and UndefinedBehaviorSanitizer checks passed with leak detection disabled;
LeakSanitizer terminated with an environment-level fatal error. GCC
ThreadSanitizer completed the concurrent correctness test three consecutive
times without a race report when built non-PIE and launched with ASLR disabled
for that process. Other TSan launches failed during initialization with an
`unexpected memory mapping` error. Clang is unavailable, so the TSan evidence
is from GCC only; see the [TSan run note](opal_bptree_tsan_results.md).

The initial 80/15/5 measurement is complemented by the separate
[workload-mix sweep](opal_bptree_workload_mix_results.md),
[hot-key comparison](opal_bptree_hotkey_results.md),
[batch-size metrics](opal_bptree_batch_metrics.md),
[update-combiner review](opal_bptree_update_combiner_review.md), and
[bounded collection-window experiment](opal_bptree_batch_policy_results.md).
The [thread-count sweep](opal_bptree_thread_scaling_results.md) and
[index-size sweep](opal_bptree_index_size_results.md) add further local
measurements. These experiments measure this teaching adaptation; they do
not establish general performance conclusions. Further work includes
cross-compiler ThreadSanitizer evidence when a suitable Clang installation
is available.

## References

V. Gupta, M. Sanchez Lopez, V. Laforet, J.-P. Lozi, and S. Kashyap,
“Operation-Aware Hybrid Locking for Modern In-Memory Indexes,” *PVLDB*,
19(8):1804–1817, 2026. [Paper PDF](https://vishalgupta.cc/publications/2026-opal/opal.pdf),
DOI: 10.14778/3811243.3811253.

The [upstream OPAL repository](https://github.com/rs3lab/opal) describes its
index benchmark setup and dependencies; this C11 project does not build or run
that suite.
