/*
 * Cygwin fork() test - Fork + Exec Sequence
 *
 * Verifies that fork followed by exec works correctly.
 * Uses self-exec pattern (argv[0] --child <exit_code>) to avoid
 * dependency on /bin/* paths which may not be available under cygrun.
 *
 * Tests:
 * - Child exec's self with exit 0
 * - Child exec's self with exit 1
 * - Child exec's self with exit 42
 * - Child exec's self and inherits environment
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

/* When invoked as --child <code>, just exit with that code */
static void child_mode(int argc, char *argv[])
{
    if (argc >= 3 && strcmp(argv[1], "--child") == 0) {
        int code = atoi(argv[2]);
        /* If --child env, check that env var is set */
        if (argc >= 4 && strcmp(argv[3], "env") == 0) {
            const char *val = getenv("FORK_EXEC_TEST");
            if (!val || strcmp(val, "hello") != 0)
                _exit(99);
        }
        _exit(code);
    }
}

static int fork_exec_test(const char *label, char *const argv[],
                          int expected_exit)
{
    pid_t pid, wpid;
    int status;

    pid = fork();
    if (pid < 0) {
        output("  %s: fork failed: %s\n", label, strerror(errno));
        return -1;
    }

    if (pid == 0) {
        execv(argv[0], argv);
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

int main(int argc, char *argv[])
{
    int failures = 0;

    /* Check for child mode first */
    child_mode(argc, argv);

    output_init();
    output("Testing fork + exec sequence (self-exec pattern)\n\n");

    alarm(45);

    /* Test 1: fork + exec self → exit 0 */
    output("Test 1: fork + exec self --child 0\n");
    {
        char *child_argv[] = { argv[0], "--child", "0", NULL };
        if (fork_exec_test("exit-0", child_argv, 0) != 0)
            failures++;
    }

    /* Test 2: fork + exec self → exit 1 */
    output("\nTest 2: fork + exec self --child 1\n");
    {
        char *child_argv[] = { argv[0], "--child", "1", NULL };
        if (fork_exec_test("exit-1", child_argv, 1) != 0)
            failures++;
    }

    /* Test 3: fork + exec self → exit 42 */
    output("\nTest 3: fork + exec self --child 42\n");
    {
        char *child_argv[] = { argv[0], "--child", "42", NULL };
        if (fork_exec_test("exit-42", child_argv, 42) != 0)
            failures++;
    }

    /* Test 4: fork + exec self with environment inheritance */
    output("\nTest 4: fork + exec self with env check\n");
    {
        setenv("FORK_EXEC_TEST", "hello", 1);
        char *child_argv[] = { argv[0], "--child", "0", "env", NULL };
        if (fork_exec_test("env-inherit", child_argv, 0) != 0)
            failures++;
        unsetenv("FORK_EXEC_TEST");
    }

    if (failures > 0) {
        output("\n%d fork+exec tests FAILED\n", failures);
        FAILED("Fork+exec test failed");
    }

    output("\n=== All fork+exec tests PASSED ===\n");
    PASSED;
}
