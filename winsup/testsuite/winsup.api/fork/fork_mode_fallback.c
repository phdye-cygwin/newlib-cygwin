/*
 * Cygwin fork() test - Fork Mode Auto Fallback (I30)
 *
 * Tests that fork works robustly under the current mode, including
 * scenarios that might stress fallback paths:
 * - Rapid sequential forks (tests cleanup/reinit paths)
 * - Fork with heavy child workload (tests full initialization)
 * - Fork after failed fork attempt (if possible to trigger)
 *
 * Note: The auto fallback from rtlclone to legacy cannot be directly
 * triggered in tests (requires API unavailability).  Instead, we
 * verify that fork is resilient under load in the current mode.
 * To test different modes, run the suite with different CYGWIN values.
 */

#define _GNU_SOURCE

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>

#include "testfrmw.h"
#include "testfrmw.c"

#define POLL_INTERVAL_US 50000
#define MAX_POLLS 100  /* 5 seconds */

static int fork_and_wait(const char *label, void (*child_fn)(void))
{
    pid_t pid, wpid;
    int status;

    pid = fork();
    if (pid < 0) {
        output("  %s: fork failed: %s\n", label, strerror(errno));
        return -1;
    }

    if (pid == 0) {
        child_fn();
        _exit(0);
    }

    for (int i = 0; i < MAX_POLLS; i++) {
        wpid = waitpid(pid, &status, WNOHANG);
        if (wpid == pid) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                output("  %s: PASS\n", label);
                return 0;
            }
            output("  %s: child exited %d\n", label,
                   WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            return -1;
        }
        usleep(POLL_INTERVAL_US);
    }

    output("  %s: TIMEOUT\n", label);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
}

static void child_heavy_work(void)
{
    /* Exercise multiple subsystems to verify full initialization */
    void *p = malloc(4096);
    if (!p) _exit(1);
    memset(p, 0xAA, 4096);
    free(p);

    int fd = open("/dev/null", O_RDONLY);
    if (fd < 0) _exit(2);
    close(fd);

    char buf[256];
    if (!getcwd(buf, sizeof(buf))) _exit(3);

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
}

static void child_nested_fork(void)
{
    /* Fork inside fork — tests reinit after reinit */
    pid_t pid = fork();
    if (pid < 0) _exit(1);
    if (pid == 0) {
        /* Grandchild does work */
        void *p = malloc(1024);
        if (!p) _exit(1);
        free(p);
        _exit(0);
    }
    int status;
    if (waitpid(pid, &status, 0) != pid) _exit(2);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) _exit(3);
}

int main(void)
{
    int failures = 0;
    const char *cygwin = getenv("CYGWIN");

    output_init();
    output("Testing fork mode fallback behavior\n");
    output("Current CYGWIN=%s\n\n", cygwin ? cygwin : "(not set)");

    alarm(45);

    /* Test 1: Fork with heavy child workload */
    output("Test 1: Fork with heavy child workload\n");
    if (fork_and_wait("heavy-work", child_heavy_work) != 0)
        failures++;

    /* Test 2: Nested fork (child forks grandchild) */
    output("\nTest 2: Nested fork in current mode\n");
    if (fork_and_wait("nested", child_nested_fork) != 0)
        failures++;

    /* Test 3: Rapid sequential forks stress-test reinit */
    output("\nTest 3: Rapid sequential forks (10 iterations)\n");
    {
        int rapid_fails = 0;
        for (int i = 0; i < 10; i++) {
            pid_t pid = fork();
            if (pid < 0) {
                output("  rapid-%d: fork failed: %s\n", i, strerror(errno));
                rapid_fails++;
                continue;
            }
            if (pid == 0)
                _exit(0);
            int status;
            if (waitpid(pid, &status, 0) != pid ||
                !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                rapid_fails++;
            }
        }
        if (rapid_fails == 0)
            output("  rapid-10: PASS\n");
        else {
            output("  rapid: %d failures\n", rapid_fails);
            failures += rapid_fails;
        }
    }

    /* Test 4: Fork after CYGWIN env manipulation — must not crash */
    output("\nTest 4: Fork after CYGWIN env manipulation\n");
    {
        /* Save and restore */
        const char *orig = cygwin ? strdup(cygwin) : NULL;
        setenv("CYGWIN", "fork_mode:auto", 1);

        pid_t pid = fork();
        if (pid < 0) {
            output("  env-manip: fork failed\n");
            failures++;
        } else if (pid == 0) {
            _exit(0);
        } else {
            int status;
            waitpid(pid, &status, 0);
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
                output("  env-manip: PASS\n");
            else {
                output("  env-manip: child failed\n");
                failures++;
            }
        }

        /* Restore original */
        if (orig) {
            setenv("CYGWIN", orig, 1);
            free((void *)orig);
        } else {
            unsetenv("CYGWIN");
        }
    }

    if (failures > 0) {
        output("\n%d fork mode fallback tests FAILED\n", failures);
        FAILED("Fork mode fallback test failed");
    }

    output("\n=== All fork mode fallback tests PASSED ===\n");
    PASSED;
}
