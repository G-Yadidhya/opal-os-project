# Balanced Locking Comparison: Reproduction Notes

This note describes the balanced optimistic-versus-mutex benchmark added
after the original side-by-side experiment. It records the local run and
gives commands for a fresh run without overwriting project files or raw
results.

## What the experiment measures

The experiment uses the existing C11 benchmark programs with four reader
threads and two writer threads. Each reader performs 200,000 successful
reads; each writer performs 100,000 updates. Both programs validate their
final state and report `RESULT: PASS` before the shell harness accepts a
measurement.

The harness performs one warm-up run per mode (optimistic, then mutex),
which is excluded from the CSV. It then performs five measured pairs in
this order:

1. Optimistic, then mutex.
2. Mutex, then optimistic.
3. Optimistic, then mutex.
4. Mutex, then optimistic.
5. Optimistic, then mutex.

The benchmark timer includes thread creation and joining. Retry counts are
recorded for optimistic reads; mutex retries are recorded as zero because
the mutex benchmark does not expose a retry counter. Retries are not
completed operations and are excluded from total operations.

## Reproduce in an isolated temporary directory

The project-worktree harness is designed to refuse an existing balanced
CSV. To keep the original project files and measurements intact, make a
fresh working copy containing only the source, harness, and analyzer, then
build and run there. This uses GCC flags for a fresh build; the build flags
of the executables used for the recorded run could not be recovered.

Run these commands from the project root:

```bash
REPRO_DIR=$(mktemp -d /tmp/opal-os-balanced-repro.XXXXXX)
mkdir -p "$REPRO_DIR/src" "$REPRO_DIR/benchmarks" "$REPRO_DIR/results/raw"
cp src/optimistic_benchmark.c src/mutex_benchmark.c "$REPRO_DIR/src/"
cp benchmarks/run_locking_comparison_balanced.sh \
   benchmarks/analyze_locking_comparison_balanced.c "$REPRO_DIR/benchmarks/"
cd "$REPRO_DIR"
gcc -std=c11 -Wall -Wextra -O2 -pthread \
    src/optimistic_benchmark.c -o optimistic_benchmark
gcc -std=c11 -Wall -Wextra -O2 -pthread \
    src/mutex_benchmark.c -o mutex_benchmark
bash benchmarks/run_locking_comparison_balanced.sh
gcc -std=c11 -Wall -Wextra -Wpedantic -O2 \
    benchmarks/analyze_locking_comparison_balanced.c \
    -o analyze_locking_comparison_balanced
./analyze_locking_comparison_balanced
```

The harness should report two warm-up passes and ten measured passes, then
`SUCCESS`. It writes `results/raw/locking_comparison_balanced.csv` and a
timestamped `locking_comparison_environment_*.txt` file in the temporary
copy. The analyzer should report ten validated measurements and
`ANALYSIS: PASS`. Keep the temporary directory if its raw output is needed;
no cleanup command is included here.

## Recorded local run

Raw measurements are preserved in
[`results/raw/locking_comparison_balanced.csv`](../results/raw/locking_comparison_balanced.csv).
The machine and experiment snapshot is
[`results/raw/locking_comparison_environment_20261028_183746.txt`](../results/raw/locking_comparison_environment_20261028_183746.txt).
The balanced harness is
[`benchmarks/run_locking_comparison_balanced.sh`](../benchmarks/run_locking_comparison_balanced.sh),
and the validator/analyzer is
[`benchmarks/analyze_locking_comparison_balanced.c`](../benchmarks/analyze_locking_comparison_balanced.c).

The recorded environment was Ubuntu 24.04.4 LTS, Linux kernel
7.0.0-34-generic, Intel Core i7-14700, 28 logical CPUs, and GCC 13.3.0.
The metadata file includes additional CPU details and timestamps. It also
states that original executable build flags were unavailable; the flags in
the reproduction commands above describe a fresh build, not the confirmed
flags of the recorded binaries.

The analyzer reported these medians from the ten preserved measurements:

| Strategy | Median elapsed time (s) | Median throughput (ops/s) | Average retries per run |
|---|---:|---:|---:|
| Optimistic | 0.268881 | 3,719,119.98 | 1,893,276 |
| Mutex | 0.200741 | 4,981,554.38 | 0 |

For this measured workload, mutex median throughput was 1.34 times the
optimistic median, and its median elapsed time was 25.34% lower. These are
measurements from this prototype and machine, not reproduced results from
the research paper. The programs are teaching benchmarks rather than the
paper's B+ Tree or ART implementation.

Alternating order addresses the earlier experiment's fixed strategy-order
issue, while warm-ups reduce first-run effects. These choices do not remove
all variation from scheduling, CPU frequency, thermal state, or background
load. Comparisons should keep workload, source version, compiler, build
flags, and timer scope in view.

## Follow-up workload-mix measurements

The follow-up experiments kept four readers and varied writer count. Each
reader performs 200,000 successful reads and each writer performs 100,000
updates. The original 4-reader/2-writer measurement is included as the
baseline. Thus the operation counts range from 8:1 reads-to-updates at one
writer, through 1:1 at eight writers, to 1:2 at sixteen writers. Each row
below comes from a separate CSV and five alternating measurement pairs.

| Workload | Writers | Optimistic median throughput (ops/s) | Mutex median throughput (ops/s) | Optimistic average retries/run | Retries per successful read |
|---|---:|---:|---:|---:|---:|
| Read-heavy | 1 | 5,656,652.22 | 4,821,280.00 | 985,725.20 | 1.232 |
| Baseline | 2 | 3,719,119.98 | 4,981,554.38 | 1,893,276.00 | 2.367 |
| Balanced | 8 | 1,143,459.71 | 3,855,746.25 | 2,176,451.20 | 2.721 |
| Update-heavy | 16 | 623,504.96 | 3,740,894.71 | 2,932,709.00 | 3.666 |

These runs show higher optimistic retry counts per successful read as
writer count increases, alongside lower optimistic throughput in the
heavier-update configurations. The retry count is a contention indicator,
not a count of failed operations. These configurations have different
update and total-operation counts, so strategy comparisons should be made
within each workload. The measurements do not establish a universal
ranking of synchronization strategies.

The four raw workload CSVs are
[`locking_comparison_read_heavy.csv`](../results/raw/locking_comparison_read_heavy.csv),
[`locking_comparison_balanced.csv`](../results/raw/locking_comparison_balanced.csv),
[`locking_comparison_balanced_mix.csv`](../results/raw/locking_comparison_balanced_mix.csv),
and
[`locking_comparison_update_heavy.csv`](../results/raw/locking_comparison_update_heavy.csv).
The cross-workload validator and summary is
[`analyze_locking_contention.c`](../benchmarks/analyze_locking_contention.c).
