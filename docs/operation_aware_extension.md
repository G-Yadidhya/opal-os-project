# Operation-Aware Locking Extension

## Implemented scope

This project adds a small hybrid benchmark with two operation paths:

- Readers use optimistic version validation. They read the atomic version
  and data fields, then retry if a writer is active or the version changed.
- Writers use a `pthread_mutex_t` to serialize updates, while publishing
  changes through the same odd/even version counter.

The new mode is compared with the existing optimistic benchmark, which
serializes writers with an `atomic_flag`, and the all-mutex benchmark, which
locks readers and writers. The hybrid implementation keeps the shared data
fields atomic and retains sequentially consistent ordering for the version
and data operations.

This is a limited teaching prototype for choosing synchronization by
operation type. It does not implement the paper's Opal lock word, function-
pointer update batching, MCS queue locking, structural modification
operations, or a B+ Tree/ART index. It is not a reproduction of the paper's
system or performance results.

The project later added a separate C11 B+ Tree teaching prototype with
optimistic lookups, function-pointer update combining, and MCS-serialized
writer paths. That implementation and its experiments are documented in
[`opal_bptree_teaching_prototype.md`](opal_bptree_teaching_prototype.md).

## Correctness checks

The new benchmark checks the expected successful read count, update count,
final values, final even version, and zero invalid snapshots. The added test
script ran three repetitions each at 4 readers/2 writers and 4 readers/16
writers; all six runs passed. Clang is not installed in the recorded
environment, so this extension has not been checked with Clang ThreadSanitizer.
The repeated checks are evidence for these runs, not a formal proof.

Build and run the targeted checks from the project root:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -O2 -pthread \
  src/writer_mutex_optimistic_reader_benchmark.c \
  -o writer_mutex_optimistic_reader_benchmark
bash tests/test_writer_mutex_optimistic_reader.sh
```

Run the three-mode comparison and analyzer with:

```bash
bash benchmarks/run_operation_aware_comparison.sh
gcc -std=c11 -Wall -Wextra -Wpedantic -O2 \
  benchmarks/analyze_operation_aware_comparison.c \
  -o analyze_operation_aware_comparison
./analyze_operation_aware_comparison
```

The runner performs one warm-up per mode and five measured runs per mode in
rotated order. Its CSV refuses to overwrite existing data. The timer includes
thread creation and joining. Raw measurements and environment metadata are
preserved in:

- [`results/raw/operation_aware_comparison.csv`](../results/raw/operation_aware_comparison.csv)
- [`results/raw/operation_aware_environment_20261028_185945.txt`](../results/raw/operation_aware_environment_20261028_185945.txt)

## Local measurement

The recorded run used four readers and two writers: 800,000 successful reads
and 200,000 updates per measurement. The analyzer validated 15 records, with
five runs per strategy.

| Strategy | Median elapsed time (s) | Median throughput (ops/s) | Average read retries per run |
|---|---:|---:|---:|
| Optimistic readers, `atomic_flag` writer lock | 0.248739 | 4,020,273.24 | 2,078,360.20 |
| Optimistic readers, `pthread_mutex` writer lock | 0.138801 | 7,204,576.38 | 189,495.20 |
| `pthread_mutex` for readers and writers | 0.197612 | 5,060,410.42 | 0 |

For this run, the hybrid mode's median throughput was 1.79 times the
optimistic spin-writer mode and 1.42 times the all-mutex mode. The change
also coincided with fewer optimistic read retries than the spin-writer mode.
These measurements cover one workload on one machine. They do not establish
that writer mutexes or hybrid locking are generally faster; further workload
and machine measurements are needed.
