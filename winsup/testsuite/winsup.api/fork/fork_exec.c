/*
 * Cygwin fork() test - Fork + Exec Sequence
 *
 * Verifies that fork followed by exec works correctly:
 * - Child exec's /bin/true (exit 0)
 * - Child exec's /bin/false (exit 1)
 * - Child exec's /bin/sh with a command
 * - Parent waitpid gets correct status
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
#define MAX_POLLS 200  /* 200 * 50ms = 10 seconds */

static int fork_exec_test(const char *label, const char *prog,
                          char *const argv[], int expected_exit)
{
    pid_t pid, wpid;
    int status;

    pid = fork();
    if (pid < 0) {
        output("  %s: fork failed: %s\n", label, strerror(errno));
        return -1;
    }

    if (pid == 0) {
        execvp(prog, argv);
        /* exec failed */
        _exit(127);
    }

    /* Poll waitpid with timeout */
    for (int i = 0; i < MAX_POLLS; i++) {
        wpid = waitpid(pid, &status, WNOHANG);
        if (wpid == pid) {
            if (!WIFEXITED(status)) {
                output("  %s: child did not exit normally\n", label);
                return -1;
            }
            if (WEXITSTATUS(status) != expected_exit) {
                output("  %s: exit %d (expected %d)\n", label,
                       WEXITSTATUS(status), expected_exit);
                return -1;
            }
            output("  %s: PASS (exit %d)\n", label, WEXITSTATUS(status));
            return 0;
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
    output("Testing fork + exec sequence\n\n");

    alarm(45);

    /* Test 1: exec /bin/true → exit 0 */
    output("Test 1: fork + exec /bin/true\n");
    {
        char *argv[] = { "true", NULL };
        if (fork_exec_test("true", "/bin/true", argv, 0) != 0)
            failures++;
    }

    /* Test 2: exec /bin/false → exit 1 */
    output("\nTest 2: fork + exec /bin/false\n");
    {
        char *argv[] = { "false", NULL };
        if (fork_exec_test("false", "/bin/false", argv, 1) != 0)
            failures++;
    }

    /* Test 3: exec /bin/sh -c 'exit 42' */
    output("\nTest 3: fork + exec /bin/sh -c 'exit 42'\n");
    {
        char *argv[] = { "sh", "-c", "exit 42", NULL };
        if (fork_exec_test("sh-exit42", "/bin/sh", argv, 42) != 0)
            failures++;
    }

    /* Test 4: exec /bin/sh -c with actual command */
    output("\nTest 4: fork + exec /bin/sh -c 'echo hello >/dev/null'\n");
    {
        char *argv[] = { "sh", "-c", "echo hello >/dev/null", NULL };
        if (fork_exec_test("sh-echo", "/bin/sh", argv, 0) != 0)
            failures++;
    }

    if (failures > 0) {
        output("\n%d fork+exec tests FAILED\n", failures);
        FAILED("Fork+exec test failed");
    }

    output("\n=== All fork+exec tests PASSED ===\n");
    PASSED;
}
