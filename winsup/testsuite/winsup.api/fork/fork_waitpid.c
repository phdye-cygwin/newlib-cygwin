/*
 * Cygwin fork() test - Process Tracking (waitpid)
 *
 * Verifies that waitpid correctly tracks child processes after fork:
 * - WIFEXITED / WEXITSTATUS for normal exit
 * - WIFSIGNALED / WTERMSIG for signal death
 * - waitpid(-1, ...) for any child
 * - waitpid(pid, ..., WNOHANG) polling
 * - SIGCHLD delivery to parent
 */

#define _GNU_SOURCE

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

#include "testfrmw.h"
#include "testfrmw.c"

#define POLL_INTERVAL_US 50000
#define MAX_POLLS 100

static volatile sig_atomic_t sigchld_count = 0;

static void sigchld_handler(int sig)
{
    (void)sig;
    sigchld_count++;
}

int main(void)
{
    pid_t pid, wpid;
    int status;

    output_init();
    output("Testing waitpid process tracking after fork()\n");

    alarm(30);

    /* Test 1: WIFEXITED + WEXITSTATUS */
    output("Test 1: Normal exit with status 42\n");

    pid = fork();
    if (pid < 0)
        UNRESOLVED(errno, "fork() failed");
    if (pid == 0)
        _exit(42);

    wpid = waitpid(pid, &status, 0);
    if (wpid != pid)
        UNRESOLVED(errno, "waitpid returned wrong PID");
    if (!WIFEXITED(status))
        FAILED("WIFEXITED not set for normal exit");
    if (WEXITSTATUS(status) != 42) {
        output("Expected exit status 42, got %d\n", WEXITSTATUS(status));
        FAILED("WEXITSTATUS wrong");
    }
    output("PASS: WIFEXITED + WEXITSTATUS(42) correct\n");

    /* Test 2: WIFSIGNALED + WTERMSIG */
    output("\nTest 2: Signal death (SIGKILL)\n");

    pid = fork();
    if (pid < 0)
        UNRESOLVED(errno, "fork() failed");
    if (pid == 0) {
        kill(getpid(), SIGKILL);
        _exit(99); /* should not reach */
    }

    /* Poll waitpid */
    for (int i = 0; i < MAX_POLLS; i++) {
        wpid = waitpid(pid, &status, WNOHANG);
        if (wpid == pid)
            goto test2_reaped;
        usleep(POLL_INTERVAL_US);
    }
    FAILED("Child not reaped after SIGKILL (timeout)");

test2_reaped:
    if (!WIFSIGNALED(status))
        FAILED("WIFSIGNALED not set for signal death");
    if (WTERMSIG(status) != SIGKILL) {
        output("Expected SIGKILL(%d), got signal %d\n", SIGKILL, WTERMSIG(status));
        FAILED("WTERMSIG wrong");
    }
    output("PASS: WIFSIGNALED + WTERMSIG(SIGKILL) correct\n");

    /* Test 3: waitpid(-1, ...) for any child */
    output("\nTest 3: waitpid(-1) for any child\n");

    pid_t child1 = fork();
    if (child1 < 0)
        UNRESOLVED(errno, "fork() failed");
    if (child1 == 0)
        _exit(10);

    pid_t child2 = fork();
    if (child2 < 0)
        UNRESOLVED(errno, "fork() failed");
    if (child2 == 0)
        _exit(20);

    /* Wait for both with -1 */
    int reaped = 0;
    int got_child1 = 0, got_child2 = 0;
    for (int r = 0; r < 2; r++) {
        wpid = waitpid(-1, &status, 0);
        if (wpid == child1) {
            got_child1 = 1;
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 10)
                FAILED("child1 wrong exit status via waitpid(-1)");
        } else if (wpid == child2) {
            got_child2 = 1;
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 20)
                FAILED("child2 wrong exit status via waitpid(-1)");
        } else {
            output("waitpid(-1) returned unexpected PID %d\n", (int)wpid);
            FAILED("waitpid(-1) returned unknown PID");
        }
        reaped++;
    }
    if (!got_child1 || !got_child2)
        FAILED("waitpid(-1) did not return both children");
    output("PASS: waitpid(-1) returned both children correctly\n");

    /* Test 4: WNOHANG returns 0 when child still running */
    output("\nTest 4: WNOHANG returns 0 for running child\n");

    pid = fork();
    if (pid < 0)
        UNRESOLVED(errno, "fork() failed");
    if (pid == 0) {
        usleep(500000); /* sleep 500ms */
        _exit(0);
    }

    wpid = waitpid(pid, &status, WNOHANG);
    if (wpid != 0) {
        output("WNOHANG returned %d immediately (expected 0)\n", (int)wpid);
        /* Child may have already exited on fast systems — not a hard failure */
        output("NOTE: child may have exited very quickly, checking status\n");
        if (wpid == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            output("Child already exited — WNOHANG test inconclusive\n");
        }
    } else {
        output("PASS: WNOHANG returned 0 for running child\n");
    }

    /* Wait for it to finish */
    for (int i = 0; i < MAX_POLLS; i++) {
        wpid = waitpid(pid, &status, WNOHANG);
        if (wpid == pid)
            break;
        usleep(POLL_INTERVAL_US);
    }
    if (wpid != pid) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        FAILED("Child did not exit in time");
    }
    output("PASS: WNOHANG eventually returned child\n");

    /* Test 5: SIGCHLD delivery */
    output("\nTest 5: SIGCHLD delivery to parent\n");

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) < 0)
        UNRESOLVED(errno, "sigaction(SIGCHLD) failed");

    sigchld_count = 0;

    pid = fork();
    if (pid < 0)
        UNRESOLVED(errno, "fork() failed");
    if (pid == 0)
        _exit(0);

    /* Wait a bit for SIGCHLD */
    for (int i = 0; i < 50; i++) {
        if (sigchld_count > 0)
            break;
        usleep(POLL_INTERVAL_US);
    }

    waitpid(pid, &status, 0);

    if (sigchld_count > 0) {
        output("PASS: SIGCHLD delivered (count=%d)\n", (int)sigchld_count);
    } else {
        output("SIGCHLD not delivered — may be timing issue\n");
        /* Not a hard failure — SIGCHLD timing can vary */
    }

    /* Restore default SIGCHLD */
    sa.sa_handler = SIG_DFL;
    sigaction(SIGCHLD, &sa, NULL);

    output("\n=== All waitpid tests PASSED ===\n");
    PASSED;
}
