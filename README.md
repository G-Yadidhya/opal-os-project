# OPAL-Inspired OS Concurrency Project

This C11 project studies how synchronization choices affect correctness,
contention, retries, and throughput in shared-memory workloads. It includes
introductory pthreads and atomic examples, optimistic and mutex benchmarks,
a reader/writer hybrid, and a small concurrent B+ Tree teaching prototype
inspired by ideas in the OPAL research paper.

The B+ Tree is a local educational adaptation. It is **not** the upstream
OPAL implementation and does not reproduce the paper's full per-node locking
design or performance results.

## Start here

- [Project report](docs/project_report.md): motivation, design, results,
  figures, correctness evidence, and limitations.
- [Demo runbook](docs/demo_runbook.md): commands for the existing checks and
  analyzers. These commands do not collect new measurements.
- [Benchmark reproduction notes](docs/benchmark_reproducibility.md): how to
  rerun selected measurements while preserving the recorded data.
- [B+ Tree prototype notes](docs/opal_bptree_teaching_prototype.md): design
  and scope of the OPAL-inspired teaching implementation.

## Quick checks

Run these from the project root. The shell tests use the executables already
present in the project root; they do not rebuild or overwrite them.

```bash
bash tests/check_counters.sh
bash tests/test_optimistic_lock.sh
bash tests/test_writer_mutex_optimistic_reader.sh
./opal_bptree_correctness_batch_metrics_check
./analyze_opal_bptree_mix_sweep
./analyze_opal_bptree_hotkey_comparison
./analyze_opal_bptree_thread_scaling_check
./analyze_opal_bptree_index_size_check
```

For the complete demo and all saved-result analyzers, follow
[`docs/demo_runbook.md`](docs/demo_runbook.md).

To build and run one benchmark without replacing a project executable:

```bash
BUILD_DIR=$(mktemp -d /tmp/opal-os-demo.XXXXXX)
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -pthread -Iinclude \
    src/opal_bptree.c src/opal_bptree_benchmark.c \
    -o "$BUILD_DIR/opal_bptree_benchmark"
"$BUILD_DIR/opal_bptree_benchmark" opal 4 10000
```

The benchmark validates its final state and prints `RESULT: PASS`. The
temporary build directory is left in place so its executable is preserved.

## Project layout

- `src/`: C11 concurrency examples, benchmarks, and B+ Tree implementations.
- `tests/`: repeatable correctness tests, including concurrent tree tests.
- `benchmarks/`: measurement runners and C analyzers.
- `results/raw/`: preserved CSV measurements and environment snapshots.
- `results/figures/`: report figures generated from preserved measurements.
- `docs/`: report, runbook, reproduction notes, and experiment-specific
  results.

The latest documented checks include a fresh GCC build of all C programs and
analyzers, repeated correctness runs, and three clean GCC ThreadSanitizer
runs for the B+ Tree test using a non-PIE executable with per-process ASLR
disabled. TSan startup without those host-specific settings can fail with
`unexpected memory mapping`; sanitizer success is evidence, not proof.

## Research reference

V. Gupta, M. Sanchez Lopez, V. Laforet, J.-P. Lozi, and S. Kashyap,
“Operation-Aware Hybrid Locking for Modern In-Memory Indexes,” *Proceedings
of the VLDB Endowment*, 19(8), 1804–1817 (2026).
[DOI: 10.14778/3811243.3811253](https://doi.org/10.14778/3811243.3811253).
Paper-reported performance figures are kept separate from this project's
local measurements in the [report](docs/project_report.md).
