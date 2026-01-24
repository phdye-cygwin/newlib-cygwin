# POSIX Fork Compliance Test Suite

This directory contains standalone POSIX fork() compliance tests for validating
the RtlCloneUserProcess-based fork implementation in Cygwin.

## Canonical Location

**These tests have been copied to the official Cygwin testsuite:**

```
winsup/testsuite/winsup.api/fork/
```

This `issue/` directory remains as the development/investigation workspace with
additional diagnostic tools (benchmarks, mode detection) and build infrastructure
that are not part of the official testsuite.

## Source

Most tests are imported from the [Open POSIX Test Suite](https://github.com/emscripten-core/posixtestsuite) (GPL-2).

## Building

### For System DLL (stock Cygwin)

```bash
make        # Build all tests
make clean  # Remove built files
```

### For Custom DLL (development build)

```bash
make clean
make CYGWIN_BUILD=/home/phdyex/my-repos/newlib-cygwin/build-cygwin/x86_64-pc-cygwin
```

## Running Tests

### System DLL (from Cygwin bash)

```bash
# Run with current fork mode
./run_all.sh

# Run with specific modes
make check-legacy      # Test legacy fork
make check-rtlclone    # Test RtlClone fork
make compare           # Compare both modes
```

### Custom DLL (MUST use PowerShell)

**IMPORTANT:** Tests linked against the custom DLL cannot be run from within
Cygwin bash (different cygwin1.dll versions conflict). Use PowerShell:

```powershell
cd C:\-\cygwin\root\home\phdyex\my-repos\newlib-cygwin\issue\fork\rcup\posix-tests
.\run_custom_dll.ps1 -Mode compare     # Compare legacy vs rtlclone
.\run_custom_dll.ps1 -Mode legacy      # Test legacy mode only
.\run_custom_dll.ps1 -Mode rtlclone    # Test rtlclone mode only
```

## Test Files

### From Open POSIX Test Suite

| Test | POSIX Assertion | Priority |
|------|-----------------|----------|
| 4-1.c | Child PPID == parent PID | P1 |
| 3-1.c | Child PID is unique | P1 |
| 2-1.c | Process state copied (env, signals) | P1 |
| 6-1.c | Directory streams inherited | P2 |
| 8-1.c | tms_utime/tms_stime reset to 0 | P2 |
| 9-1.c | Alarm signals canceled in child | P2 |
| 12-1.c | Blocked signals inherited, pending cleared | P3 |
| 13-1.c | Interval timers reset | P4 |
| 22-1.c | CPU-time clocks init to 0 | P4 |
| 14-1.c | Semaphores inherited | P5 |
| 1-1.c | Fork concurrency (parent/child run) | P5 |
| 19-1.c | Message queues inherited | P5 |
| 16-1.c | Memory mappings (COW semantics) | P6 |
| 11-1.c | File locks NOT inherited | P6 |
| 21-1.c | Child has single thread | P6 |
| 18-1.c | Per-process timers NOT inherited | P6 |
| 17-1.c | SCHED_RR inherited | P7 |
| 17-2.c | SCHED_FIFO inherited | P7 |

### Custom Tests

| Test | Assertion |
|------|-----------|
| fork_fd_inherit.c | File descriptor inheritance (#5) |
| fork_return_values.c | Return values: 0 in child, PID in parent (#23) |

## Exit Codes

- `0` (PTS_PASS): Test passed
- `1` (PTS_FAIL): Test failed
- `2` (PTS_UNRESOLVED): Test could not determine pass/fail
- `4` (PTS_UNSUPPORTED): Feature not supported on this platform
- `5` (PTS_UNTESTED): Test was skipped
- `77`: Alternative skip code

## Fork Mode Environment Variable

Set the Cygwin fork mode:

```bash
export CYGWIN="fork_mode:legacy"     # Use traditional fork
export CYGWIN="fork_mode:rtlclone"   # Use RtlCloneUserProcess
export CYGWIN="fork_mode:auto"       # Auto-select based on environment
```

## Known Limitations

Some tests may not work on Cygwin due to platform differences:
- `17-1.c`, `17-2.c`: SCHED_RR/FIFO may not be fully supported
- `19-1.c`: POSIX message queues may not be available

## Current Status

**All tests pass identically for both fork modes (2026-01-24):**

| Mode | PASS | SKIP | FAIL |
|------|------|------|------|
| Legacy | 28 | 2 | 0 |
| RtlClone | 28 | 2 | 0 |

See `Phase-3-Findings.md` for historical test results during development.
See `../../Phase-4-Complete.verified.md` and `../../fork-via-RtlCloneUserProcess-handle-inheritance.md`
for details on how the issues were resolved.

## License

Tests from Open POSIX Test Suite are GPL-2 licensed.
Custom tests follow the same license as Cygwin (LGPL).
