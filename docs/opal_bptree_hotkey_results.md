# OPAL-Inspired B+ Tree Hot-Key Comparison

## Experiment

This experiment holds the balanced operation mix at 50% lookups, 40% updates,
and 10% inserts, and varies the distribution of lookup and update keys. Each
configuration uses four threads and 10,000 operations per thread. One warm-up
per strategy and distribution is excluded from the CSV. Five measured pairs
per distribution alternate which strategy runs first. Tree setup and
post-run verification are outside the timer; worker creation and joining are
inside it.

Uniform access chooses among the 512 initial keys. The `skew80_20`
configuration routes 80% of lookup and update accesses to a hot set of 102
initial keys, with the other 20% routed to the remaining 410 initial keys.
Insert keys remain unique and do not use this distribution.

## Results from this machine

The analyzer validated all 20 measurements. Values are medians over five
runs per strategy and distribution.

| Key distribution | Mutex throughput (ops/s) | OPAL-inspired throughput (ops/s) | OPAL/mutex ratio | OPAL average retries/run | OPAL average batches/run |
|---|---:|---:|---:|---:|---:|
| Uniform | 827,401.37 | 1,028,236.48 | 1.24x | 96,145 | 11,697 |
| 80/20 hot set | 947,812.63 | 1,093,153.13 | 1.15x | 79,527 | 11,538 |

The operation-aware prototype had higher median throughput in both tested
distributions. The ratio was smaller in the hot-set run. These measurements
do not establish that skew itself caused the difference: this is one machine
and one run series, and the tree’s update batching and global version protocol
also affect the result. Retries decreased in the hot-set measurements, but
that counter is an implementation metric rather than a direct contention
measurement.

This remains a local C11 teaching adaptation, not the upstream OPAL
implementation or a reproduction of the paper. It uses global optimistic
version validation and a global MCS writer queue, and does not cover deletes,
reclamation, range scans, NUMA placement, or PiBench integration.

## Reproduction

From the project root:

```bash
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 -pthread -Iinclude \
  src/opal_bptree.c src/opal_bptree_mix_benchmark.c \
  -o opal_bptree_mix_benchmark
bash benchmarks/run_opal_bptree_hotkey_comparison.sh
gcc -std=c11 -Wall -Wextra -Wpedantic -Werror -O2 \
  benchmarks/analyze_opal_bptree_hotkey_comparison.c -lm \
  -o analyze_opal_bptree_hotkey_comparison
./analyze_opal_bptree_hotkey_comparison
```

The runner refuses to overwrite existing files. This run’s data and
environment snapshot are:

- [`results/raw/opal_bptree_hotkey_comparison.csv`](../results/raw/opal_bptree_hotkey_comparison.csv)
- [`results/raw/opal_bptree_hotkey_environment_20261028_220300.txt`](../results/raw/opal_bptree_hotkey_environment_20261028_220300.txt)

The workload-mix sweep remains documented separately in
[`opal_bptree_workload_mix_results.md`](opal_bptree_workload_mix_results.md).
