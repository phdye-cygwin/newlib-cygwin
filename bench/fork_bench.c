/*
 * fork_bench.c — Cygwin fork() latency and throughput benchmark.
 *
 * Five scenarios covering the primary fork() usage patterns in POSIX
 * shell and application workloads.  Output is line-delimited JSON,
 * schema "fork-bench-v1", suitable for CI parsing.
 *
 * Build:
 *   gcc -O2 -o fork_bench fork_bench.c
 *
 * Run:
 *   ./fork_bench                        # all scenarios, defaults
 *   ./fork_bench fork-warm fork-exec    # selected scenarios
 *   ./fork_bench --iterations=500 --warmup=50
 *
 * Requires: Cygwin or Linux; CLOCK_MONOTONIC; /bin/true for fork-exec.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Configuration                                                        */
/* ------------------------------------------------------------------ */

#define DEFAULT_ITERATIONS  100
#define DEFAULT_WARMUP       20
#define FORK_DEEP_DEPTH       8
#define BURST_FORKS          20

/* ------------------------------------------------------------------ */
/* Timing                                                               */
/* ------------------------------------------------------------------ */

static uint64_t
now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------------ */
/* Statistics                                                           */
/* ------------------------------------------------------------------ */

static int
cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

typedef struct {
    uint64_t min_us;
    uint64_t median_us;
    uint64_t p95_us;
    uint64_t max_us;
    int      n;
    int      outliers;
} stats_t;

static stats_t
compute_stats(uint64_t *s, int n)
{
    stats_t r = {0};
    int good = 0;
    for (int i = 0; i < n; i++)
        if (s[i] > 0) s[good++] = s[i];
    r.outliers = n - good;
    r.n = good;
    if (good == 0) return r;
    qsort(s, good, sizeof *s, cmp_u64);
    r.min_us    = s[0]                    / 1000;
    r.median_us = s[good / 2]             / 1000;
    r.p95_us    = s[(int)(good * 0.95)]   / 1000;
    r.max_us    = s[good - 1]             / 1000;
    return r;
}

/* ------------------------------------------------------------------ */
/* Scenarios                                                            */
/* ------------------------------------------------------------------ */

/*
 * fork-warm
 *
 * Timed window: fork() call in parent -> fork() return in parent.
 * Excludes: child lifetime, waitpid.
 * Models: per-fork syscall cost when fork caches are warm (e.g.,
 *   the Nth fork in a shell script loop).
 */
static uint64_t
scn_fork_warm(void)
{
    uint64_t t0 = now_ns();
    pid_t pid = fork();
    uint64_t t1 = now_ns();
    if (pid == 0) _exit(0);
    if (pid < 0) return 0;
    int st; waitpid(pid, &st, 0);
    return t1 - t0;
}

/*
 * fork-pipe
 *
 * Timed window: fork() call in parent -> parent reads 1-byte IPC token
 *   written by child.
 * Excludes: child exit, final waitpid.
 * Models: fork() + minimal inter-process communication (shell pipeline
 *   startup: parent blocks until child is running and writing).
 */
static uint64_t
scn_fork_pipe(void)
{
    int fds[2];
    if (pipe(fds) != 0) return 0;
    uint64_t t0 = now_ns();
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        char b = 'x';
        write(fds[1], &b, 1);
        close(fds[1]);
        _exit(0);
    }
    if (pid < 0) { close(fds[0]); close(fds[1]); return 0; }
    close(fds[1]);
    char buf;
    read(fds[0], &buf, 1);
    uint64_t t1 = now_ns();
    close(fds[0]);
    int st; waitpid(pid, &st, 0);
    return t1 - t0;
}

/*
 * fork-exec
 *
 * Timed window: fork() call in parent -> waitpid returns after child
 *   exec'd /bin/true and exited.
 * Excludes: nothing -- full fork+exec+exit+reap cycle.
 * Models: cost of running one external command, as a shell does for
 *   each command in a script (e.g., `echo`, `test`, `grep`).
 */
static uint64_t
scn_fork_exec(void)
{
    uint64_t t0 = now_ns();
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/true", "true", (char *)NULL);
        _exit(127);
    }
    if (pid < 0) return 0;
    int st; waitpid(pid, &st, 0);
    uint64_t t1 = now_ns();
    return t1 - t0;
}

/*
 * fork-burst
 *
 * Timed window: BURST_FORKS fork() calls initiated back-to-back ->
 *   all children reaped.  Returns per-fork average.
 * Excludes: nothing -- full fork+exit+reap cycle per child.
 * Models: sustained fork throughput under concurrent child load,
 *   as in `make -j` launching many short-lived subprocesses.
 */
static uint64_t
scn_fork_burst(void)
{
    pid_t pids[BURST_FORKS];
    uint64_t t0 = now_ns();
    for (int i = 0; i < BURST_FORKS; i++) {
        pids[i] = fork();
        if (pids[i] == 0) _exit(0);
        if (pids[i] < 0)  pids[i] = -1;
    }
    for (int i = 0; i < BURST_FORKS; i++)
        if (pids[i] > 0) { int st; waitpid(pids[i], &st, 0); }
    uint64_t t1 = now_ns();
    return (t1 - t0) / BURST_FORKS;
}

/*
 * fork-deep
 *
 * Timed window: first fork() call in outermost parent -> waitpid
 *   returns in outermost parent after the full FORK_DEEP_DEPTH chain
 *   completes.
 * Excludes: nothing -- full chain including all intermediate forks
 *   and waits.
 * Models: total cost of a deeply nested subprocess chain, as in a
 *   shell script that sources other scripts which run subcommands
 *   (e.g., autoconf configure scripts at depth 8).
 */
static void __attribute__((noreturn))
fork_chain(int depth)
{
    if (depth == 0) _exit(0);
    pid_t pid = fork();
    if (pid == 0) fork_chain(depth - 1);
    if (pid < 0)  _exit(1);
    int st; waitpid(pid, &st, 0);
    _exit(0);
}

static uint64_t
scn_fork_deep(void)
{
    uint64_t t0 = now_ns();
    pid_t pid = fork();
    if (pid == 0) fork_chain(FORK_DEEP_DEPTH - 1);
    if (pid < 0) return 0;
    int st; waitpid(pid, &st, 0);
    uint64_t t1 = now_ns();
    return t1 - t0;
}

/* ------------------------------------------------------------------ */
/* Dispatch table                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    uint64_t  (*fn)(void);
} scenario_t;

static const scenario_t all_scenarios[] = {
    { "fork-warm",  scn_fork_warm  },
    { "fork-pipe",  scn_fork_pipe  },
    { "fork-exec",  scn_fork_exec  },
    { "fork-burst", scn_fork_burst },
    { "fork-deep",  scn_fork_deep  },
};
static const int n_scenarios =
    (int)(sizeof all_scenarios / sizeof all_scenarios[0]);

/* ------------------------------------------------------------------ */
/* Warmup                                                               */
/* ------------------------------------------------------------------ */

static void
warmup(int n)
{
    for (int i = 0; i < n; i++) {
        pid_t p = fork();
        if (p == 0) _exit(0);
        if (p > 0) { int st; waitpid(p, &st, 0); }
    }
}

/* ------------------------------------------------------------------ */
/* Output                                                               */
/* ------------------------------------------------------------------ */

static void
print_result(const char *name, const stats_t *s, int first)
{
    if (!first) fputc(',', stdout);
    printf("{\"name\":\"%s\","
           "\"min_us\":%llu,"
           "\"median_us\":%llu,"
           "\"p95_us\":%llu,"
           "\"max_us\":%llu,"
           "\"n\":%d,"
           "\"outliers\":%d}",
           name,
           (unsigned long long)s->min_us,
           (unsigned long long)s->median_us,
           (unsigned long long)s->p95_us,
           (unsigned long long)s->max_us,
           s->n,
           s->outliers);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

static void
usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options] [scenario...]\n"
        "\n"
        "Options:\n"
        "  --iterations=N    Measurement iterations per scenario (default %d)\n"
        "  --warmup=N        Warmup forks before measurement (default %d)\n"
        "  --help            Show this message\n"
        "\n"
        "Scenarios (default: all):\n"
        "  fork-warm   fork() syscall latency, warm caches\n"
        "  fork-pipe   fork() + minimal IPC roundtrip\n"
        "  fork-exec   fork() + exec(/bin/true) + reap\n"
        "  fork-burst  per-fork cost under concurrent child load\n"
        "  fork-deep   depth-%d recursive fork chain, full reap\n"
        "\n"
        "Output: line-delimited JSON, schema fork-bench-v1\n",
        prog, DEFAULT_ITERATIONS, DEFAULT_WARMUP, FORK_DEEP_DEPTH);
}

int
main(int argc, char **argv)
{
    int iterations = DEFAULT_ITERATIONS;
    int warmup_n   = DEFAULT_WARMUP;
    const char *selected[16];
    int n_selected = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strncmp(argv[i], "--iterations=", 13) == 0) {
            iterations = atoi(argv[i] + 13);
            if (iterations <= 0 || iterations > 100000) {
                fprintf(stderr, "error: --iterations out of range\n");
                return 1;
            }
        } else if (strncmp(argv[i], "--warmup=", 9) == 0) {
            warmup_n = atoi(argv[i] + 9);
            if (warmup_n < 0 || warmup_n > 10000) {
                fprintf(stderr, "error: --warmup out of range\n");
                return 1;
            }
        } else if (argv[i][0] != '-') {
            int found = 0;
            for (int j = 0; j < n_scenarios; j++) {
                if (strcmp(argv[i], all_scenarios[j].name) == 0) {
                    found = 1; break;
                }
            }
            if (!found) {
                fprintf(stderr, "error: unknown scenario '%s'\n", argv[i]);
                usage(argv[0]);
                return 1;
            }
            if (n_selected < 16)
                selected[n_selected++] = argv[i];
        } else {
            fprintf(stderr, "error: unknown option '%s'\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    uint64_t *samples = malloc((size_t)iterations * sizeof *samples);
    if (!samples) { fputs("error: out of memory\n", stderr); return 2; }

    warmup(warmup_n);

    printf("{\"schema\":\"fork-bench-v1\",\"scenarios\":[");
    int first = 1;

    for (int si = 0; si < n_scenarios; si++) {
        const scenario_t *scn = &all_scenarios[si];

        if (n_selected > 0) {
            int wanted = 0;
            for (int k = 0; k < n_selected; k++)
                if (strcmp(selected[k], scn->name) == 0) { wanted = 1; break; }
            if (!wanted) continue;
        }

        for (int i = 0; i < iterations; i++)
            samples[i] = scn->fn();

        stats_t st = compute_stats(samples, iterations);
        print_result(scn->name, &st, first);
        first = 0;
    }

    puts("]}");
    free(samples);
    return 0;
}
