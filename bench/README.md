# fork_bench — Cygwin fork() latency benchmark

Measures fork() latency and throughput across five scenarios covering
the primary fork() usage patterns in POSIX shell and application
workloads.

This benchmark was developed to support the Cygwin fork-replay-opt
patch, which reduces fork() latency by skipping zero-initialised .bss
pages during the fork replay phase.

---

## Prerequisites

- Cygwin (any recent version) or Linux
- gcc
- /bin/true (standard on both)

---

## Build

```sh
cd bench
make
```

---

## Run

```sh
# All scenarios, default settings (100 iterations, 20 warmup forks)
./fork_bench

# Selected scenarios
./fork_bench fork-warm fork-exec

# More iterations for stable medians
./fork_bench --iterations=500 --warmup=50

# Pretty-print output (requires python3 or jq)
./fork_bench | python3 -m json.tool
./fork_bench | jq .
```

---

## Output format

Line-delimited JSON, schema `fork-bench-v1`:

```json
{"schema":"fork-bench-v1","scenarios":[
  {"name":"fork-warm","min_us":18420,"median_us":21304,"p95_us":28100,"max_us":45200,"n":100,"outliers":0},
  ...
]}
```

All latency values are in microseconds (µs).  `n` is the number of
valid samples used.  `outliers` counts samples where the measured
duration was zero (indicating a timing anomaly; these are excluded from
statistics).

---

## Scenarios

Each scenario's timing model is stated precisely: what clock reads
bracket the measurement and what is intentionally excluded.

### fork-warm

**What is timed:** `fork()` call in parent through `fork()` return in
parent.  `CLOCK_MONOTONIC` is read immediately before `fork()` and
immediately after it returns in the parent process.

**Excluded:** child lifetime (the child calls `_exit(0)` immediately),
`waitpid()` in the parent.

**What it models:** The per-fork syscall cost when fork caches are warm
— the condition after the first few forks in a process.  This is the
primary metric for evaluating fork() implementation overhead, and the
scenario most directly improved by the fork-replay-opt patch.

**Reference point:** A baseline Cygwin install on an Intel i9-11900H
(Windows 10.0.26200) measures approximately 28,000–32,000 µs median.
Native Linux fork() on comparable x86-64 hardware typically measures
50–200 µs.  The gap is the overhead of Cygwin's fork replay mechanism.

---

### fork-pipe

**What is timed:** `fork()` call in parent through the parent reading a
1-byte token written by the child.

**Excluded:** child `_exit()`, final `waitpid()` in parent.

**What it models:** fork() plus minimal inter-process communication —
the parent blocks until the child is running and has written to a pipe.
This approximates the startup cost of a shell pipeline stage (`cmd1 |
cmd2`).

---

### fork-exec

**What is timed:** `fork()` call in parent through `waitpid()` return
after the child exec'd `/bin/true` and exited.  Nothing is excluded —
this is the full fork+exec+exit+reap cycle.

**What it models:** The cost of running one external command, as a
shell does for each command in a script (`echo`, `test`, `grep`, etc.).
This is the dominant cost in shell-heavy workloads and build systems.

**Reference point:** On the same baseline Cygwin install, fork-exec
measures approximately 55,000–65,000 µs median.  The `spawn-det` patch
(separate branch) targets this scenario specifically via
`posix_spawn()`.

---

### fork-burst

**What is timed:** `BURST_FORKS` (20) `fork()` calls initiated
back-to-back, followed by reaping all children.  Returns the per-fork
average: `(t1 - t0) / BURST_FORKS`.  Nothing is excluded — full
fork+exit+reap per child.

**What it models:** Sustained fork throughput under concurrent child
load, as in `make -j` launching many short-lived subprocesses
simultaneously.  A lower per-fork average here indicates the
implementation handles concurrent children efficiently.

---

### fork-deep

**What is timed:** First `fork()` call in the outermost parent through
`waitpid()` return in the outermost parent after the full depth-8 fork
chain completes.  Nothing is excluded — the measurement spans all
intermediate forks and waits in the chain.

**What it models:** Total cost of a deeply nested subprocess chain, as
in autoconf `configure` scripts that source other scripts which invoke
subcommands (typical depth: 5–10 levels).

---

## Interpreting results

**Median is the primary metric.**  Min and p95 help identify variance;
max is informative but often reflects OS scheduling noise.

**For patch evaluation:** Run the benchmark against an unpatched Cygwin
DLL, then against the patched DLL, on the same machine with no other
load.  The median difference on `fork-warm` is the most stable signal
for the replay-opt patch.

**For comparing across machines:** Cygwin fork() latency varies
significantly with CPU generation, RAM speed, and Windows version.
Always report the hardware and OS version alongside results.  Values
from shared CI runners (GitHub-hosted Windows) will differ substantially
from dedicated hardware.

---

## CI notes

CI verifies:

- The benchmark builds without errors or warnings
- It runs to completion on all scenarios without hanging
- Output is valid JSON matching schema `fork-bench-v1`
- All `median_us` values are positive (sanity: no scenario silently
  produced zero output)

CI does **not** assert exact latency values.  GitHub-hosted Windows
runners exhibit ±20–40% timing variance depending on load and
scheduling.  Absolute numbers from CI runs are not comparable to
controlled-environment measurements.

To assert an improvement direction (e.g., "patched DLL is faster"),
run both builds in the same CI job on the same runner and compare
within-run ratios.  A ratio threshold of ≥30% is reliable on these
runners; smaller improvements require dedicated hardware.

---

## Archived results

Milestone measurements (patch submissions, release baselines) are
committed to `bench/results/`.  See `bench/results/README.md` for
format.

---

## Related patches

| Patch | Primary scenario | Branch |
|---|---|---|
| fork-replay-opt | fork-warm | `PATCH/fork-replay-opt.patch` (apply to `main`) |
| spawn-det | fork-exec | feature/spawn-det (separate work) |
