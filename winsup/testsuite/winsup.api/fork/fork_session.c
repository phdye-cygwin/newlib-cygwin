/*
 * POSIX fork() test - Session ID Inheritance
 *
 * Verifies that the child process inherits the parent's session ID.
 *
 * POSIX.1-2017: "The child process shall have the same session ID
 * as the parent process."
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
    pid_t parent_sid, child_sid;
    pid_t pid;
    int pipefd[2];
    int status;

    output_init();
    output("Testing session ID inheritance across fork()\n");

    alarm(30);

    if (pipe(pipefd) < 0)
        UNRESOLVED(errno, "pipe() failed");

    parent_sid = getsid(0);
    output("Parent SID: %d\n", (int)parent_sid);

    pid = fork();
    if (pid < 0)
        UNRESOLVED(errno, "fork() failed");

    if (pid == 0) {
        close(pipefd[0]);
        child_sid = getsid(0);
        if (write(pipefd[1], &child_sid, sizeof(child_sid)) != sizeof(child_sid))
            _exit(2);
        close(pipefd[1]);
        _exit(0);
    }

    close(pipefd[1]);
    if (read(pipefd[0], &child_sid, sizeof(child_sid)) != sizeof(child_sid)) {
        close(pipefd[0]);
        UNRESOLVED(errno, "Failed to read child's SID");
    }
    close(pipefd[0]);

    if (waitpid(pid, &status, 0) != pid)
        UNRESOLVED(errno, "waitpid failed");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        FAILED("Child exited abnormally");

    output("Child SID: %d\n", (int)child_sid);

    if (child_sid != parent_sid) {
        output("ERROR: child SID %d != parent SID %d\n",
               (int)child_sid, (int)parent_sid);
        FAILED("Child SID does not match parent SID");
    }

    output("PASS: child SID matches parent SID\n");
    output("\n=== Session ID test PASSED ===\n");
    PASSED;
}
