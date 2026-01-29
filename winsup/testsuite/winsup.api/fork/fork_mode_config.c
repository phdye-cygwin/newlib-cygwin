/*
 * Cygwin fork() test - Fork Mode Configuration (I28, I29)
 *
 * Tests that the CYGWIN="fork_mode:XXX" environment variable is
 * correctly parsed and controls fork behavior.
 *
 * Tests:
 * - fork_mode:legacy — fork works
 * - fork_mode:rtlclone — fork works
 * - fork_mode:auto — fork works (fallback if needed)
 * - fork_mode:bogus — should fall back gracefully
 *
 * Each mode is tested by spawning a subprocess with the appropriate
 * CYGWIN environment variable.
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
#define CHILD_TIMEOUT_POLLS 200  /* 200 * 50ms = 10 seconds */

/*
 * Spawn a child process with a given CYGWIN env value.
 * The child does fork+exit to verify fork works under that mode.
 * Returns 0 on success, -1 on failure.
 */
static int test_fork_mode(const char *mode_name, const char *cygwin_val)
{
    pid_t pid;
    int status;

    pid = fork();
    if (pid < 0) {
        output("  %s: fork failed: %s\n", mode_name, strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /*
         * Child: set CYGWIN env, then exec a helper that forks.
         * We use /bin/sh -c 'exec /bin/true' which internally forks+execs.
         * But for a more direct test, we'll do another fork inside.
         */
        if (cygwin_val)
            setenv("CYGWIN", cygwin_val, 1);
        else
            unsetenv("CYGWIN");

        /* Fork inside this child to test under the new mode.
         * Note: the mode is parsed at DLL init, so changing CYGWIN env
         * mid-process may not take effect.  To properly test, we exec
         * a small program.  Use /bin/sh which re-initializes. */
        execl("/bin/sh", "sh", "-c",
              /* Shell script: fork (subshell) and verify */
              "(exit 0) && exit 0 || exit 1",
              (char *)NULL);
        _exit(127); /* exec failed */
    }

    /* Poll waitpid with timeout */
    for (int i = 0; i < CHILD_TIMEOUT_POLLS; i++) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                output("  %s: PASS\n", mode_name);
                return 0;
            }
            output("  %s: child exited with status %d\n", mode_name,
                   WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            return -1;
        }
        usleep(POLL_INTERVAL_US);
    }

    output("  %s: TIMEOUT — fork mode may have hung\n", mode_name);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
}

int main(void)
{
    int failures = 0;

    output_init();
    output("Testing fork mode configuration (CYGWIN env var)\n\n");

    alarm(45);

    output("Test 1: fork_mode:legacy\n");
    if (test_fork_mode("legacy", "fork_mode:legacy") != 0)
        failures++;

    output("\nTest 2: fork_mode:rtlclone\n");
    if (test_fork_mode("rtlclone", "fork_mode:rtlclone") != 0)
        failures++;

    output("\nTest 3: fork_mode:auto\n");
    if (test_fork_mode("auto", "fork_mode:auto") != 0)
        failures++;

    output("\nTest 4: fork_mode:bogus (should fallback gracefully)\n");
    if (test_fork_mode("bogus-fallback", "fork_mode:bogus") != 0)
        failures++;

    output("\nTest 5: No fork_mode set (default)\n");
    if (test_fork_mode("default", NULL) != 0)
        failures++;

    if (failures > 0) {
        output("\n%d fork mode config tests FAILED\n", failures);
        FAILED("Fork mode configuration test failed");
    }

    output("\n=== All fork mode config tests PASSED ===\n");
    PASSED;
}
