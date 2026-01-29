/*
 * POSIX fork() test - Nice Value Inheritance
 *
 * Verifies that the child inherits the parent's nice value.
 *
 * POSIX.1-2017: "The child process shall inherit the nice value."
 */

#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/resource.h>
#include <sys/wait.h>

#include "testfrmw.h"
#include "testfrmw.c"

int main(void)
{
    int parent_nice, child_nice;
    pid_t pid;
    int pipefd[2];
    int status;

    output_init();
    output("Testing nice value inheritance across fork()\n");

    alarm(30);

    /* Get current nice value */
    errno = 0;
    parent_nice = getpriority(PRIO_PROCESS, 0);
    if (parent_nice == -1 && errno != 0)
        UNRESOLVED(errno, "getpriority failed");

    output("Parent nice value: %d\n", parent_nice);

    if (pipe(pipefd) < 0)
        UNRESOLVED(errno, "pipe() failed");

    pid = fork();
    if (pid < 0)
        UNRESOLVED(errno, "fork() failed");

    if (pid == 0) {
        close(pipefd[0]);
        errno = 0;
        child_nice = getpriority(PRIO_PROCESS, 0);
        if (child_nice == -1 && errno != 0)
            _exit(2);
        if (write(pipefd[1], &child_nice, sizeof(child_nice)) != sizeof(child_nice))
            _exit(3);
        close(pipefd[1]);
        _exit(0);
    }

    close(pipefd[1]);
    if (read(pipefd[0], &child_nice, sizeof(child_nice)) != sizeof(child_nice)) {
        close(pipefd[0]);
        UNRESOLVED(errno, "Failed to read child nice value");
    }
    close(pipefd[0]);

    if (waitpid(pid, &status, 0) != pid)
        UNRESOLVED(errno, "waitpid failed");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        FAILED("Child exited abnormally");

    output("Child nice value: %d\n", child_nice);

    if (child_nice != parent_nice) {
        output("ERROR: child nice %d != parent nice %d\n",
               child_nice, parent_nice);
        FAILED("Nice value not inherited");
    }

    output("PASS: nice value inherited correctly\n");
    output("\n=== Nice value test PASSED ===\n");
    PASSED;
}
