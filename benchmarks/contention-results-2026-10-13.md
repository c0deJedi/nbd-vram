# Read latency during concurrent uploads

Compared `fd6170e` with the changes that upload full pages before locking their
mappings and release read locks before decompression. Both binaries used the
same `bench-contention.c`, compiled with `cc -O2 -g -Wall -Wextra -Werror`.

## Setup

- NVIDIA T1200 Laptop GPU, 4 GiB, display active.
- Kernel `7.2.8-200.fc44.x86_64`.
- Private 16 MiB VRAM allocation; the installed VRAM swap service remained active.
- LZ4, dedup off, blocking CUDA waits, one reader and one writer.
- Repeated 4 KiB reads of one page alongside 64 KiB writes of synthetic,
  compressible data to different pages.
- `alias`: the writer's pages share the reader's lock stripe.
- `disjoint`: the writer uses different lock stripes, as a control.
- Three runs of 10,000 reads per revision per case, alternating revision order:
  before/after, after/before, before/after.

## Results

Percentiles below are **medians of the three per-run percentiles**, not pooled
percentiles. Maximum latency and threshold counts cover all three runs.

| Case | Metric | Before | After |
|---|---|---:|---:|
| Shared lock stripe | p50 | 84.56 us | 24.07 us |
| Shared lock stripe | p99 | 186.09 us | 67.28 us |
| Shared lock stripe | p99.9 | 905.61 us | 94.06 us |
| Shared lock stripe | Maximum | 2,067.23 us | 632.75 us |
| Shared lock stripe | Reads over 1 ms / 30,000 | 23 | 0 |
| Separate lock stripes | p50 | 20.14 us | 21.32 us |
| Separate lock stripes | p99 | 61.85 us | 68.79 us |
| Separate lock stripes | p99.9 | 84.49 us | 106.51 us |
| Separate lock stripes | Maximum | 446.06 us | 452.36 us |
| Separate lock stripes | Reads over 1 ms / 30,000 | 0 | 0 |

All cases recorded zero reads over 5 or 10 ms. In the shared-stripe case,
background write throughput was 591–619 MiB/s before and 595–667 MiB/s after;
reader progress improved without starving the writer.

### Individual runs

| Case | Run | Revision | p99 us | p99.9 us | Max us | Reads >1 ms | Write MiB/s |
|---|---:|---|---:|---:|---:|---:|---:|
| alias | 1 | before | 184.36 | 639.80 | 1346.96 | 4 | 591.0 |
| alias | 1 | after | 72.92 | 115.37 | 632.75 | 0 | 611.3 |
| alias | 2 | before | 186.09 | 905.61 | 1499.20 | 4 | 594.2 |
| alias | 2 | after | 59.27 | 94.06 | 452.52 | 0 | 667.0 |
| alias | 3 | before | 203.48 | 1054.21 | 2067.23 | 15 | 618.6 |
| alias | 3 | after | 67.28 | 91.23 | 300.61 | 0 | 594.7 |
| disjoint | 1 | before | 55.70 | 81.04 | 318.11 | 0 | 795.0 |
| disjoint | 1 | after | 68.79 | 106.51 | 220.56 | 0 | 742.6 |
| disjoint | 2 | before | 61.85 | 84.49 | 302.97 | 0 | 761.2 |
| disjoint | 2 | after | 69.29 | 121.78 | 182.88 | 0 | 678.2 |
| disjoint | 3 | before | 70.23 | 106.75 | 446.06 | 0 | 714.0 |
| disjoint | 3 | after | 60.76 | 93.90 | 452.36 | 0 | 766.1 |

## Interpretation and limits

The shared-stripe case improved consistently across all three runs. The control
had higher median percentiles after the change, with overlapping per-run ranges
and improvement in the third pair. These short runs establish a benefit under
lock contention, not a universal latency improvement.

This exercises the storage functions directly and deliberately stresses lock
aliasing. It excludes NBD queuing, kernel reclaim, real swap data distributions,
and desktop frame timing. GPU clocks and other system activity were not held
constant. It does not establish that pointer stalls are eliminated.

Reproduce the current revision with:

```sh
make bench-contention BENCH_ARGS='lz4 10000 alias'
make bench-contention BENCH_ARGS='lz4 10000 disjoint'
```
