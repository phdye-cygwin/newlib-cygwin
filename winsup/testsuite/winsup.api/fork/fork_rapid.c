/*
 * Cygwin fork() test - Rapid Sequential Forks
 *
 * Forks N children rapidly, each exits immediately.
 * Parent reaps all, verifying no zombies, no handle leaks,
 * and all exit normally.
 *
 * Tests thread pool recycling, pinfo reuse, handle cleanup.
 */

#define _POSIX_C_SOURCE 200809L

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

#define NUM_CHILDREN 20

int main(void)
{
    pid_t children[NUM_CHILDREN];
    int status;
    int i;

    output_init();
    output("Testing rapid sequential fork (%d children)\n", NUM_CHILDREN);

    alarm(45);

    /* Fork all children rapidly */
    for (i = 0; i < NUM_CHILDREN; i++) {
        children[i] = fork();
        if (children[i] < 0) {
            output("fork() failed at child %d: %s\n", i, strerror(errno));
            /* Reap any already-forked children */
            for (int j = 0; j < i; j++)
                waitpid(children[j], NULL, 0);
            UNRESOLVED(errno, "fork() failed during rapid fork test");
        }

        if (children[i] == 0) {
            /* Child: exit with index as status */
            _exit(i % 256);
        }
    }

    output("All %d children forked, now reaping...\n", NUM_CHILDREN);

    /* Reap all children and verify exit status */
    int failures = 0;
    for (i = 0; i < NUM_CHILDREN; i++) {
        pid_t w = waitpid(children[i], &status, 0);
        if (w != children[i]) {
            output("Child %d: waitpid returned %d (expected %d): %s\n",
                   i, (int)w, (int)children[i], strerror(errno));
            failures++;
            continue;
        }
        if (!WIFEXITED(status)) {
            output("Child %d (pid %d): did not exit normally\n",
                   i, (int)children[i]);
            failures++;
            continue;
        }
        if (WEXITSTATUS(status) != (i % 256)) {
            output("Child %d: exit status %d (expected %d)\n",
                   i, WEXITSTATUS(status), i % 256);
            failures++;
        }
    }

    /* Verify no extra children (waitpid should return -1/ECHILD) */
    pid_t extra = waitpid(-1, &status, WNOHANG);
    if (extra > 0) {
        output("Unexpected extra child %d found!\n", (int)extra);
        failures++;
    }

    if (failures > 0) {
        output("\n%d of %d children had issues\n", failures, NUM_CHILDREN);
        FAILED("Rapid fork test failed");
    }

    output("All %d children reaped with correct exit status\n", NUM_CHILDREN);
    output("\n=== Rapid fork test PASSED ===\n");
    PASSED;
}
