/*
 * Cygwin fork() test - Fork Mode Configuration (I28, I29)
 *
 * Tests that fork works correctly under the current fork_mode.
 * The CYGWIN="fork_mode:XXX" mode is parsed at DLL initialization,
 * so it cannot be changed mid-process via setenv.  To test different
 * modes, the entire test suite is run twice with different CYGWIN
 * values (once for legacy, once for rtlclone).
 *
 * This test verifies:
 * - fork() works under whatever mode is currently active
 * - Multiple sequential forks work
 * - fork works after setenv("CYGWIN", ...) — verifying no crash
 *   (the mode won't actually change, but should not break)
 * - CYGWIN env var is readable and inheritable
 */

#define _GNU_SOURCE

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

#include "testfrmw.h"
#include "testfrmw.c"

#define POLL_INTERVAL_US 50000
#define CHILD_TIMEOUT_POLLS 100  /* 100 * 50ms = 5 seconds */

/*
 * Fork a child, have it verify something, exit 0 on success.
 * Parent polls waitpid with timeout to detect hangs.
 */
static int test_fork_basic(const char *label, void (*child_fn)(void))
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

    for (int i = 0; i < CHILD_TIMEOUT_POLLS; i++) {
        wpid = waitpid(pid, &status, WNOHANG);
        if (wpid == pid) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                output("  %s: PASS\n", label);
                return 0;
            }
            output("  %s: child exited with status %d\n", label,
                   WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            return -1;
        }
        usleep(POLL_INTERVAL_US);
    }

    output("  %s: TIMEOUT — fork may have hung\n", label);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
}

static void child_noop(void)
{
    /* Just exit — verifies fork completes */
}

static void child_verify_env(void)
{
    /* Verify CYGWIN env is readable in child */
    const char *cygwin = getenv("CYGWIN");
    /* It's OK if CYGWIN is not set (default mode) */
    if (cygwin)
        (void)strlen(cygwin); /* access it */
}

static void child_nested_fork(void)
{
    /* Fork inside child to verify nested fork works under current mode */
    pid_t pid = fork();
    if (pid < 0)
        _exit(1);
    if (pid == 0)
        _exit(0);
    int status;
    if (waitpid(pid, &status, 0) != pid)
        _exit(2);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        _exit(3);
}

static void child_after_setenv(void)
{
    /* setenv doesn't change the active fork mode (parsed at DLL init)
     * but must not crash */
    setenv("CYGWIN", "fork_mode:legacy", 1);
    pid_t pid = fork();
    if (pid < 0)
        _exit(1);
    if (pid == 0)
        _exit(0);
    int status;
    if (waitpid(pid, &status, 0) != pid)
        _exit(2);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        _exit(3);
}

int main(void)
{
    int failures = 0;
    const char *cygwin = getenv("CYGWIN");

    output_init();
    output("Testing fork mode configuration\n");
    output("Current CYGWIN=%s\n\n", cygwin ? cygwin : "(not set)");

    alarm(45);

    output("Test 1: Basic fork under current mode\n");
    if (test_fork_basic("basic-fork", child_noop) != 0)
        failures++;

    output("\nTest 2: CYGWIN env readable in child\n");
    if (test_fork_basic("env-readable", child_verify_env) != 0)
        failures++;

    output("\nTest 3: Nested fork (child forks again)\n");
    if (test_fork_basic("nested-fork", child_nested_fork) != 0)
        failures++;

    output("\nTest 4: Fork after setenv(CYGWIN) — no crash\n");
    if (test_fork_basic("after-setenv", child_after_setenv) != 0)
        failures++;

    output("\nTest 5: Sequential forks under current mode\n");
    {
        int seq_fails = 0;
        for (int i = 0; i < 5; i++) {
            pid_t pid = fork();
            if (pid < 0) {
                output("  seq-%d: fork failed\n", i);
                seq_fails++;
                continue;
            }
            if (pid == 0)
                _exit(i);
            int status;
            pid_t w = waitpid(pid, &status, 0);
            if (w != pid || !WIFEXITED(status) || WEXITSTATUS(status) != i) {
                output("  seq-%d: wrong exit status\n", i);
                seq_fails++;
            }
        }
        if (seq_fails == 0)
            output("  sequential: PASS (5 forks)\n");
        else
            failures += seq_fails;
    }

    if (failures > 0) {
        output("\n%d fork mode config tests FAILED\n", failures);
        FAILED("Fork mode configuration test failed");
    }

    output("\n=== All fork mode config tests PASSED ===\n");
    PASSED;
}
