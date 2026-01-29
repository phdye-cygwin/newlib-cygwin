/*
 * Cygwin fork() test - Pending Signal Clearing (I20)
 *
 * Verifies that pending signals in the parent are NOT inherited
 * by the child.  The child's pending signal set must be empty.
 *
 * POSIX.1-2017: "Signals pending to the parent process shall not
 * be pending to the child process."
 *
 * Also tests RtlClone implementation item I20 (sigq_clear_after_clone).
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

static volatile sig_atomic_t sig_received = 0;

static void handler(int sig)
{
    (void)sig;
    sig_received = 1;
}

int main(void)
{
    sigset_t block_set, pending;
    pid_t pid;
    int pipefd[2];
    int status;

    output_init();
    output("Testing pending signal clearing across fork()\n");

    alarm(30);

    /* Block SIGUSR1 */
    sigemptyset(&block_set);
    sigaddset(&block_set, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &block_set, NULL) < 0)
        UNRESOLVED(errno, "sigprocmask(SIG_BLOCK) failed");

    /* Install a handler so the signal is catchable (not default/ignore) */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL) < 0)
        UNRESOLVED(errno, "sigaction failed");

    /* Send SIGUSR1 to self — it becomes pending (blocked) */
    if (kill(getpid(), SIGUSR1) < 0)
        UNRESOLVED(errno, "kill(self, SIGUSR1) failed");

    /* Verify it's pending in parent */
    if (sigpending(&pending) < 0)
        UNRESOLVED(errno, "sigpending failed");
    if (!sigismember(&pending, SIGUSR1)) {
        output("SIGUSR1 not pending after kill(self) — test cannot proceed\n");
        UNRESOLVED(0, "Cannot make SIGUSR1 pending");
    }
    output("Parent: SIGUSR1 is pending (as expected)\n");

    if (pipe(pipefd) < 0)
        UNRESOLVED(errno, "pipe() failed");

    pid = fork();
    if (pid < 0)
        UNRESOLVED(errno, "fork() failed");

    if (pid == 0) {
        close(pipefd[0]);

        /* Child: check pending signals */
        sigset_t child_pending;
        if (sigpending(&child_pending) < 0)
            _exit(2);

        int pending_in_child = sigismember(&child_pending, SIGUSR1);

        /* Also unblock SIGUSR1 and check if handler fires */
        sig_received = 0;
        sigprocmask(SIG_UNBLOCK, &block_set, NULL);
        /* Brief pause to let any signal be delivered */
        usleep(10000);

        /* Report results: byte 0 = pending flag, byte 1 = received flag */
        char report[2];
        report[0] = (char)pending_in_child;
        report[1] = (char)sig_received;
        if (write(pipefd[1], report, 2) != 2)
            _exit(3);
        close(pipefd[1]);
        _exit(0);
    }

    close(pipefd[1]);

    char report[2];
    if (read(pipefd[0], report, 2) != 2) {
        close(pipefd[0]);
        UNRESOLVED(errno, "Failed to read child report");
    }
    close(pipefd[0]);

    if (waitpid(pid, &status, 0) != pid)
        UNRESOLVED(errno, "waitpid failed");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        output("Child exit status: %d\n", WEXITSTATUS(status));
        FAILED("Child exited abnormally");
    }

    int child_had_pending = report[0];
    int child_got_signal = report[1];

    output("Child: SIGUSR1 pending=%d, received_after_unblock=%d\n",
           child_had_pending, child_got_signal);

    if (child_had_pending) {
        FAILED("Child inherited parent's pending SIGUSR1 (POSIX violation)");
    }
    output("PASS: child's pending signal set is empty\n");

    if (child_got_signal) {
        FAILED("Child received stale SIGUSR1 after unblocking");
    }
    output("PASS: no stale signal delivered to child after unblocking\n");

    /* Clean up: unblock SIGUSR1 in parent (will deliver the pending signal) */
    sigprocmask(SIG_UNBLOCK, &block_set, NULL);

    output("\n=== Pending signal clearing test PASSED ===\n");
    PASSED;
}
