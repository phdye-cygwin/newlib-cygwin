/*
 * POSIX fork() test - File Mode Creation Mask (umask)
 *
 * Verifies that the child inherits the parent's umask, and that
 * changes in the child do not affect the parent.
 *
 * POSIX.1-2017: "The child process shall inherit the file mode
 * creation mask."
 */

#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "testfrmw.h"
#include "testfrmw.c"

int main(void)
{
    mode_t parent_mask, child_mask;
    pid_t pid;
    int pipefd[2];
    int status;

    output_init();
    output("Testing umask inheritance across fork()\n");

    alarm(30);

    /* Set a distinctive umask */
    parent_mask = umask(0037);
    parent_mask = umask(0037); /* Set and read back */
    parent_mask = 0037;
    output("Parent umask: %04o\n", parent_mask);

    if (pipe(pipefd) < 0)
        UNRESOLVED(errno, "pipe() failed");

    pid = fork();
    if (pid < 0)
        UNRESOLVED(errno, "fork() failed");

    if (pid == 0) {
        close(pipefd[0]);
        /* Read current umask (umask returns previous, so set+restore) */
        child_mask = umask(0);
        umask(child_mask);
        if (write(pipefd[1], &child_mask, sizeof(child_mask)) != sizeof(child_mask))
            _exit(2);
        /* Change umask in child to prove isolation */
        umask(0077);
        close(pipefd[1]);
        _exit(0);
    }

    close(pipefd[1]);
    if (read(pipefd[0], &child_mask, sizeof(child_mask)) != sizeof(child_mask)) {
        close(pipefd[0]);
        UNRESOLVED(errno, "Failed to read child umask");
    }
    close(pipefd[0]);

    if (waitpid(pid, &status, 0) != pid)
        UNRESOLVED(errno, "waitpid failed");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        FAILED("Child exited abnormally");

    output("Child umask (before change): %04o\n", child_mask);

    if (child_mask != parent_mask) {
        output("ERROR: child umask %04o != parent umask %04o\n",
               child_mask, parent_mask);
        FAILED("Child umask does not match parent");
    }
    output("PASS: child inherited parent's umask\n");

    /* Verify parent's umask unchanged after child modified it */
    mode_t current = umask(parent_mask);
    if (current != parent_mask) {
        output("ERROR: parent umask changed to %04o after child modified its copy\n",
               current);
        FAILED("Child's umask change affected parent");
    }
    output("PASS: parent umask unchanged after child modification\n");

    /* Restore original umask */
    umask(parent_mask);

    output("\n=== All umask tests PASSED ===\n");
    PASSED;
}
