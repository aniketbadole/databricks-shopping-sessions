# Shopping Sessions: native C++ pipeline

An exact, data-driven Shopping Sessions implementation. C++17/OpenMP handles
CSV parsing, deduplication, linked identities, sessions and attribution. Polars
only serializes final aggregates to typed Parquet. No generated data, expected
answers, precomputed results, or external services are used at runtime.

## Optimization update, 21 November 2026

The original submission scored **10.07 seconds** on the organizer's practice
run (user-reported result). The new implementation has not yet been scored.

Same-session local 20M benchmarks, three runs per implementation:

| Implementation | Runs (seconds) | Median |
| --- | --- | ---: |
| Original scored source | 8.476, 7.866, 7.779 | 7.866 s |
| Optimized source | 6.142, 6.144, 5.668 | 6.142 s |

This is about **22% lower elapsed time**, not a prediction of the hidden score.
Optimized peak child RSS ranged from 4,066 to 4,198 MiB. This costs more memory
than the original. Full 60M memory use remains unverified.

Changes: balanced 4 MiB line-aligned input chunks; sequential single-pass event
parsing; contiguous hash tables holding pointers into stable candidate arrays;
cached person hashes for sorting, with exact string collision resolution; and
a specialized parser for the specification's guaranteed-valid ingestion times.
Invalid candidates are retained through deduplication, so a later valid copy
cannot rescue an invalid earliest copy.

Validation was repeated: all 36 fixtures, the exact 1M published answer and all
four independent small-seed oracle comparisons passed. The entire 20M output
also matched the original (2,242,393 user rows and 28,014 campaign rows).
The earlier linked CI run covers the original source; check the latest Actions
run for the optimized commit's container result.

## Original baseline validation recorded on 21 November 2026

- All **36/36** supplied edge-case fixtures passed.
- Public dev set: seed 1, 1,000,000 requested events, 250,000 requested clicks.
  Exact match: **117,038 user rows** and **27,400 campaign rows**; no missing,
  extra, differing or duplicate-key rows.
- Independent Python oracle: all output rows matched on seeds **0, 2, 29, 2026**,
  each with 10,000 requested events and 2,500 requested clicks.

Local end-to-end timings include Python startup, native execution, input reads
and Parquet writes. Data generation and output comparison are excluded.

| Dataset | Time | Peak child process RSS |
| --- | ---: | ---: |
| Public dev, seed 1, 1M events / 250K clicks | 0.697 s | 167.3 MiB |
| Generated seed 7, 20M events / 5M clicks | 8.769 s | 3,288.6 MiB |

These are single local measurements on an Intel Xeon Platinum 8573C with eight
worker threads. They are **not official scores**, were not measured in Docker,
and do not establish cold-cache, pinned-vCPU or final-dataset performance.
Process RSS is not the same as total container memory accounting. The 20M set
has no published expected answer; only the correctness datasets listed above
were compared exactly. Hidden practice submission remains necessary.

The linux/amd64 Docker image was successfully built in GitHub Actions, and all
36 fixtures passed inside containers with no network, a read-only root, uid 1000,
a 64 MiB `/tmp`, and process/memory limits. CI used 2 CPUs and 6 GiB RAM; it was
not a performance benchmark. [Successful container validation](https://github.com/aniketbadole/databricks-shopping-sessions/actions/runs/44893703769).

The image uses a portable x86-64 compile (no `-march=native`), uid 1000 and a
multi-stage build. It writes only to `/scratch` and `/output` at runtime.

## Build and check with Docker

```sh
python3 -m pip install -r requirements.txt
make build
make fixtures
```

Build the organizer's generator separately, and put `generate` on PATH. Unzip
`dev-answer.zip` into `dev-answer/`, then:

```sh
make dev
make run-dev
make check-dev ANSWER=dev-answer
```

`make fixtures` invokes the supplied fixture checker. `scripts/docker_run.sh`
uses the supplied no-network, read-only-root, uid, memory and process limits.

## Native development without Docker

Requires Linux, Python, g++ and OpenMP (`libgomp`).

```sh
python3 -m pip install -r requirements.txt
make fixtures-native
SCRATCH_DIR="$PWD/scratch" NCPU=8 python3 pipeline/pipeline.py dev out
make check-dev ANSWER=dev-answer
```

For additional small generated datasets, use the independent slow oracle:

```sh
python3 scripts/reference.py dev-small reference-out
SCRATCH_DIR="$PWD/scratch" NCPU=8 python3 pipeline/pipeline.py dev-small native-out
python3 scripts/compare.py native-out reference-out
```

The oracle intentionally scans all a person's clicks for every session. Use it
on small datasets, not on the full benchmark.

## Implementation

1. Map input files read-only; parse physical LF-delimited lines with the exact
   restricted dialect. A CR remains field data. No trimming or case folding.
2. Resolve link components with union-by-size and path compression. Retain the
   byte-wise minimum ID, including IDs that have no events or clicks.
3. Parse balanced input chunks into candidate records and partition by exact
   event-ID hash. Retain invalid candidates through earliest-ingestion selection,
   then discard invalid winners. Full strings resolve all hash collisions.
4. Validate timestamp structure, calendar dates, offsets, digit-only numbers,
   purchase ranges and the late-event boundary explicitly. No permissive date
   or CSV inference is used for inputs.
5. Partition surviving events and valid clicks by canonical person. Sort events
   by person/time and clicks by person/time/descending click ID.
6. Scan sessions using gaps strictly greater than 30 minutes. Roll up every
   event onto the date of the session's first event.
7. Move two click pointers over the inclusive seven-day lookback. The final
   eligible click gives last-touch credit. Per-campaign counts track the active
   campaign set, so repeated clicks assist only once per session.
8. Aggregate user/day/category and campaign/day counts; serialize the small
   final CSV intermediates to Parquet with explicit schemas and a DATE column.

Identical click duplicates are intentionally retained internally. They cannot
change which click wins, and campaign reference counts preserve the exact set
of qualifying campaigns. This is equivalent to explicit click-ID deduplication
under the specification's guarantee that duplicate clicks are identical.

Whale users do not cause an events-times-clicks join. Assist processing visits
each active campaign once per session, which is necessary for those output
contributions. Session scans are linear after sorting. No input ordering,
identifier format, manifest dates, dirt rates or output values are hard-coded.

## Submission

Use a public repository with `Dockerfile` at its root. Keep generated inputs,
outputs and the local `core` executable out of Git. Tiny organizer fixtures are
included for reproducible correctness checks; the competition datasets and dev
answers are not included. `.dockerignore` restricts the image build context to
runtime sources.

```sh
git init
git add .
git commit -m "Implement native shopping sessions pipeline"
git branch -M main
git remote add origin https://github.com/YOUR_ACCOUNT/shopping-sessions.git
git push -u origin main
git rev-parse HEAD
```

Submit that repository URL and full commit SHA through the organizer's form.
Do not rewrite submitted history. The supplied brief gives a cutoff of Sunday
22 November 2026 at 16:00 GMT, and permits two practice submissions per hour.

The implementation is a validated baseline, not a claim to beat the sub-second
leaderboard. Use the official 20M practice timing to guide further optimization.

A 60M-event local test was attempted, but the organizer's generator was killed
with exit code 137, including on a one-thread retry, before it completed the
input dataset. This workspace has approximately 9.7 GiB RAM versus the scorer's
24 GiB. No 60M pipeline runtime or memory claim is made.
