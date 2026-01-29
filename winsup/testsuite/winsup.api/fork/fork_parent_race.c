/*
 * Cygwin fork() test - Parent-Child Handle Race
 *
 * Stress-tests the fork handle fixup synchronization by rapidly
 * forking many children, each with open pipe/file FDs.  Each child
 * reads/writes FDs to verify they were correctly fixed up.
 *
 * This tests the fixup_done_evt synchronization under load,
 * which is critical for both legacy and RtlClone fork paths.
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
#include <sys/wait.h>

#include "testfrmw.h"
#include "testfrmw.c"

#define NUM_CHILDREN 20
#define POLL_INTERVAL_US 50000
#define CHILD_TIMEOUT_POLLS 100  /* 100 * 50ms = 5 seconds */

int main(void)
{
    pid_t children[NUM_CHILDREN];
    int pipes[NUM_CHILDREN][2];
    int fd_file;
    int status;
    int i;

    output_init();
    output("Testing parent-child handle race with %d children\n", NUM_CHILDREN);

    alarm(45);

    /* Open a file FD that all children will inherit */
    fd_file = open("/dev/null", O_RDONLY);
    if (fd_file < 0)
        UNRESOLVED(errno, "open(/dev/null) failed");

    /* Fork many children rapidly, each with a unique pipe */
    for (i = 0; i < NUM_CHILDREN; i++) {
        if (pipe(pipes[i]) < 0)
            UNRESOLVED(errno, "pipe() failed");

        children[i] = fork();
        if (children[i] < 0)
            UNRESOLVED(errno, "fork() failed");

        if (children[i] == 0) {
            /* Child: verify FDs work, then signal via pipe */
            close(pipes[i][0]);

            /* Test that the inherited file FD works */
            if (fcntl(fd_file, F_GETFD) < 0)
                _exit(1);

            /* Write our PID to prove pipe works */
            pid_t my_pid = getpid();
            if (write(pipes[i][1], &my_pid, sizeof(my_pid)) != sizeof(my_pid))
                _exit(2);

            close(pipes[i][1]);
            _exit(0);
        }

        /* Parent: close write end */
        close(pipes[i][1]);
    }

    /* Collect results from all children */
    int failures = 0;
    for (i = 0; i < NUM_CHILDREN; i++) {
        pid_t reported_pid;
        ssize_t n = read(pipes[i][0], &reported_pid, sizeof(reported_pid));
        close(pipes[i][0]);

        /* Poll waitpid with timeout */
        int reaped = 0;
        for (int p = 0; p < CHILD_TIMEOUT_POLLS; p++) {
            pid_t w = waitpid(children[i], &status, WNOHANG);
            if (w == children[i]) {
                reaped = 1;
                break;
            }
            usleep(POLL_INTERVAL_US);
        }

        if (!reaped) {
            output("Child %d (pid %d): TIMEOUT — handle fixup may have deadlocked\n",
                   i, (int)children[i]);
            kill(children[i], SIGKILL);
            waitpid(children[i], NULL, 0);
            failures++;
            continue;
        }

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            output("Child %d: exited with status %d\n",
                   i, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            failures++;
            continue;
        }

        if (n != sizeof(reported_pid) || reported_pid != children[i]) {
            output("Child %d: pipe data mismatch (got pid %d, expected %d)\n",
                   i, (int)reported_pid, (int)children[i]);
            failures++;
            continue;
        }
    }

    close(fd_file);

    if (failures > 0) {
        output("\n%d of %d children FAILED\n", failures, NUM_CHILDREN);
        FAILED("Handle race test failed");
    }

    output("All %d children completed successfully\n", NUM_CHILDREN);
    output("\n=== Handle race test PASSED ===\n");
    PASSED;
}
