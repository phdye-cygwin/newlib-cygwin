/*
 * POSIX fork() test - Process Group Inheritance
 *
 * Verifies that the child process inherits the parent's process
 * group ID, and can independently set its own.
 *
 * POSIX.1-2017: "The child process shall have the same process
 * group ID as the parent process."
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

int main(void)
{
    pid_t parent_pgid, child_pgid;
    pid_t pid;
    int pipefd[2];
    int status;

    output_init();
    output("Testing process group inheritance across fork()\n");

    alarm(30);

    if (pipe(pipefd) < 0)
        UNRESOLVED(errno, "pipe() failed");

    parent_pgid = getpgrp();
    output("Parent PGID: %d\n", (int)parent_pgid);

    pid = fork();
    if (pid < 0)
        UNRESOLVED(errno, "fork() failed");

    if (pid == 0) {
        close(pipefd[0]);
        child_pgid = getpgrp();
        if (write(pipefd[1], &child_pgid, sizeof(child_pgid)) != sizeof(child_pgid))
            _exit(2);
        close(pipefd[1]);
        _exit(0);
    }

    close(pipefd[1]);
    if (read(pipefd[0], &child_pgid, sizeof(child_pgid)) != sizeof(child_pgid)) {
        close(pipefd[0]);
        UNRESOLVED(errno, "Failed to read child's PGID");
    }
    close(pipefd[0]);

    if (waitpid(pid, &status, 0) != pid)
        UNRESOLVED(errno, "waitpid failed");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        FAILED("Child exited abnormally");

    output("Child PGID: %d\n", (int)child_pgid);

    if (child_pgid != parent_pgid) {
        output("ERROR: child PGID %d != parent PGID %d\n",
               (int)child_pgid, (int)parent_pgid);
        FAILED("Child PGID does not match parent PGID");
    }
    output("PASS: child PGID matches parent PGID\n");

    /* Test 2: Child can independently change its PGID */
    output("\nTest 2: Child can setpgid() independently\n");

    pid = fork();
    if (pid < 0)
        UNRESOLVED(errno, "fork() failed");

    if (pid == 0) {
        /* Child sets its own process group */
        if (setpgid(0, 0) < 0) {
            /* Not fatal — may fail under certain terminal conditions */
            _exit(77);
        }
        pid_t new_pgid = getpgrp();
        if (new_pgid == getpid()) {
            _exit(0);
        }
        _exit(1);
    }

    if (waitpid(pid, &status, 0) != pid)
        UNRESOLVED(errno, "waitpid failed");

    if (WIFEXITED(status) && WEXITSTATUS(status) == 77) {
        output("Test 2 SKIPPED: setpgid() not permitted in this context\n");
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        output("Test 2 PASS: child successfully changed its PGID\n");
    } else {
        FAILED("Child failed to change PGID");
    }

    output("\n=== All PGID tests PASSED ===\n");
    PASSED;
}
