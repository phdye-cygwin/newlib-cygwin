/*
 * Cygwin fork() test - Fork Mode Auto Fallback (I30)
 *
 * Tests that fork_mode:auto gracefully falls back when rtlclone
 * is unavailable or fails.  Also tests that fork works regardless
 * of what fork_mode is configured.
 *
 * This test spawns child processes with different CYGWIN env values
 * and verifies fork works in each.  The auto fallback path is hard
 * to trigger directly (requires API unavailability), so we primarily
 * test that fork succeeds under all configured modes.
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
#define MAX_POLLS 200  /* 10 seconds */

/*
 * Spawn /bin/sh with a given CYGWIN value.  The shell forks internally
 * (subshell), so if fork fails the command fails.
 */
static int test_mode_via_exec(const char *label, const char *cygwin_val)
{
    pid_t pid, wpid;
    int status;

    pid = fork();
    if (pid < 0) {
        output("  %s: outer fork failed: %s\n", label, strerror(errno));
        return -1;
    }

    if (pid == 0) {
        if (cygwin_val)
            setenv("CYGWIN", cygwin_val, 1);
        else
            unsetenv("CYGWIN");

        /* Execute sh which must fork internally for subshell */
        execl("/bin/sh", "sh", "-c",
              /* Fork a subshell, run a command, verify exit */
              "v=$(echo ok); test \"$v\" = ok",
              (char *)NULL);
        _exit(127);
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

int main(void)
{
    int failures = 0;

    output_init();
    output("Testing fork mode fallback behavior\n\n");

    alarm(45);

    output("Test 1: fork_mode:auto (should work via rtlclone or legacy)\n");
    if (test_mode_via_exec("auto", "fork_mode:auto") != 0)
        failures++;

    output("\nTest 2: fork_mode:legacy (explicit legacy path)\n");
    if (test_mode_via_exec("legacy", "fork_mode:legacy") != 0)
        failures++;

    output("\nTest 3: fork_mode:rtlclone (explicit rtlclone path)\n");
    if (test_mode_via_exec("rtlclone", "fork_mode:rtlclone") != 0)
        failures++;

    output("\nTest 4: empty CYGWIN (default behavior)\n");
    if (test_mode_via_exec("default", "") != 0)
        failures++;

    if (failures > 0) {
        output("\n%d fork mode fallback tests FAILED\n", failures);
        FAILED("Fork mode fallback test failed");
    }

    output("\n=== All fork mode fallback tests PASSED ===\n");
    PASSED;
}
