# Shopping Sessions: native C++ pipeline

An exact, data-driven C++20/OpenMP pipeline. It reads the mounted CSV files,
resolves linked people, deduplicates events, forms sessions, computes credit
and assists, and writes DATE-typed Parquet directly with Apache Arrow/Parquet.
The runtime image contains **no Python interpreter** and uses no intermediate CSVs.

## Current implementation

- **Lossless compact IDs.** IDs of up to ten ASCII letters, digits or underscores
  use an injective 64-bit encoding. Case and leading zeros remain distinct.
  Every other string uses an exact dictionary fallback in a disjoint key space.
  This is not a probabilistic hash: IDs cannot collide. Original strings still
  determine the byte-wise minimum linked identity and click-ID tie breaks.
- **Compact union-find.** Flat integer-key lookups, union-by-size and path
  compression resolve all link chains, including IDs absent from events/clicks.
- **Balanced sequential reads.** Physical line boundaries divide event files
  into roughly 4 MiB chunks, preventing a large file from monopolizing a worker.
- **SSE2 CSV scanning.** Commas and quotes are scanned together. Exact field
  counts and whole-field quoting are enforced. CR remains a field character.
  SSE2 is part of the required x86-64 architecture; no AVX-only build is used.
- **Deduplication without per-row allocation.** Contiguous candidate arrays and
  flat pointer tables select the earliest ingestion using exact event IDs.
  Invalid candidates remain present until winner selection, so a later valid
  copy cannot rescue an invalid earliest copy.
- **Integer radix sorting.** Person/time ordering uses stable byte passes with
  constant-byte passes skipped. Small buckets use comparison sorting.
- **Linear session and click scans.** A sliding seven-day window tracks active
  campaigns and last-touch credit, without an events-times-clicks join.
- **Native Parquet output.** Typed builders write Snappy Parquet concurrently
  by user partition. There is no Python startup, CSV formatting or CSV reread.

Identical click duplicates are retained internally. This cannot change the
winning click or the active campaign set under the rules' identical-copy
contract. Campaign reference counts prevent duplicate assists.

## Correctness

The current code passed:

- All **36/36** organizer fixtures.
- Exact public seed-1 dev comparison: **117,038 user rows**, **27,400 campaign
  rows**, no missing/extra/differing rows or duplicate keys.
- Independent Python oracle comparisons for seeds **0, 2, 29 and 2026**, each
  with 10K requested events and 2.5K clicks.
- Additional packed-ID/fallback tests covering case, leading zeros, underscores,
  ten/eleven-character boundaries, spaces, Unicode, CR and long IDs.
- Complete comparison against the previous implementation on the seed-7 20M
  dataset: **2,242,393 user rows** and **28,014 campaign rows**, exact match.

GitHub Actions builds the linux/amd64 image and runs the organizer fixtures
with no network, a read-only root, uid 1000, 64 MiB `/tmp`, and process/memory
limits. CI uses 2 CPUs and 6 GiB RAM; it is a correctness check, not a benchmark.
It also runs the additional identifier oracle test. Check the latest Actions
run for this commit's status.

## Build and check

```sh
python3 -m pip install -r requirements.txt
make build
make fixtures
```

For native development on Linux with g++/OpenMP:

```sh
make native
python3 scripts/run_fixtures.py fixtures -- ./core
python3 scripts/check_extra.py
./core /path/to/data /path/to/empty-output
python3 scripts/benchmark.py /path/to/data /path/to/empty-output
```

PyArrow supplies the pinned Arrow/Parquet C++ headers and shared libraries at
build time. The final Debian image copies only the executable and the two
required Arrow/Parquet libraries. `pipeline/pipeline.py` remains a compatibility
launcher for local use and is not included in the runtime image.

Build the organizer's generator separately and put `generate` on PATH. Unzip
`dev-answer.zip` into `dev-answer/`, then:

```sh
make dev
make run-dev
make check-dev ANSWER=dev-answer
```

For independent small-dataset validation:

```sh
python3 scripts/reference.py dev-small reference-out
./core dev-small native-out
python3 scripts/compare.py native-out reference-out
```

The Python oracle intentionally uses simple, slow session/click scans. It is
only for small datasets and is never used by the runtime container.

## Benchmarks and limits

Official user-reported practice scores:

| Commit | Score |
| --- | ---: |
| `007005d66c62cf79ce4905dc6e1198c9556ccd09` | 10.07 s |
| `fb17dddd1b2405c0d0b9fccf1e8006e465ff748c` | 6.68 s |

Local alternating 20M benchmark runs in the same session:

| Implementation | Runs (seconds) | Median |
| --- | --- | ---: |
| Previous `fb17dddd` source | 9.183, 8.117, 6.944 | 8.117 s |
| Native rewrite | 3.280, 3.672, 3.260 | 3.280 s |

The previous implementation measured 6.142 s in an earlier session, illustrating
local timing/cache variability. These results are not a promised hidden score.

A full-size seed-29 run (60M requested events, 15M clicks) completed in **16.128 s**
with **6,995.6 MiB peak child process RSS**. It wrote 6,639,553 user rows and
28,014 campaign rows. Session and revenue totals agreed across tables, and the
assist-count consistency check passed. There is no public expected answer for
this seed. A full-output comparison with the previous implementation was
attempted but interrupted after it stopped making progress for several minutes
on this approximately 9.7 GiB workspace; that comparison remains incomplete.

The native source passed [container CI and additional oracle tests](https://github.com/aniketbadole/databricks-shopping-sessions/actions/runs/44896512703).

The current native rewrite has not yet received an official score. Local
measurements use an Intel Xeon Platinum 8573C with eight workers; they include
process startup, CSV reads, calculations and Parquet writes. Generation and
comparison are outside the timing. They do not reproduce the scorer's exact
pinned-vCPU allocation or guarantee cold-cache behavior. Peak process RSS is
not total container memory accounting.

The 60M input was generated locally with the public generator, seed 29 and 15M
requested clicks. A development-only generator change calls `shrink_to_fit` on
completed shard buffers so generation fits this smaller workspace. It changes
allocation capacity only. The modified generator's seed-1 1M dataset was
byte-for-byte identical to the unmodified generator. Neither the generator
modification nor generated data is used by or included in this submission.

## Submission

Submit the public repository URL and full commit SHA. `Dockerfile` is at the
root. Input data, expected dev answers and local executables are excluded;
small public fixture files are included for reproducibility. No precomputed
answers, dataset seeds or output values are used at runtime.

Previous scored commits remain in history. Do not force-push or rewrite them.
