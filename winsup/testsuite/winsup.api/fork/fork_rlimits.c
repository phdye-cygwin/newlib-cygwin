/*
 * POSIX fork() test - Resource Limit Inheritance
 *
 * Verifies that the child inherits the parent's resource limits.
 *
 * POSIX.1-2017: "The child process shall inherit the resource
 * limits."
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

struct rlim_report {
    struct rlimit nofile;
    struct rlimit as;
    struct rlimit stack;
};

int main(void)
{
    struct rlim_report parent_rl, child_rl;
    struct rlimit new_nofile;
    pid_t pid;
    int pipefd[2];
    int status;

    output_init();
    output("Testing resource limit inheritance across fork()\n");

    alarm(30);

    /* Set a distinctive RLIMIT_NOFILE value */
    if (getrlimit(RLIMIT_NOFILE, &new_nofile) < 0)
        UNRESOLVED(errno, "getrlimit(NOFILE) failed");
    /* Lower soft limit to something distinctive */
    if (new_nofile.rlim_cur > 128)
        new_nofile.rlim_cur = 128;
    if (setrlimit(RLIMIT_NOFILE, &new_nofile) < 0)
        UNRESOLVED(errno, "setrlimit(NOFILE) failed");

    /* Gather parent limits */
    if (getrlimit(RLIMIT_NOFILE, &parent_rl.nofile) < 0)
        UNRESOLVED(errno, "getrlimit(NOFILE) failed");
    if (getrlimit(RLIMIT_AS, &parent_rl.as) < 0)
        UNRESOLVED(errno, "getrlimit(AS) failed");
    if (getrlimit(RLIMIT_STACK, &parent_rl.stack) < 0)
        UNRESOLVED(errno, "getrlimit(STACK) failed");

    output("Parent RLIMIT_NOFILE: cur=%lu max=%lu\n",
           (unsigned long)parent_rl.nofile.rlim_cur,
           (unsigned long)parent_rl.nofile.rlim_max);

    if (pipe(pipefd) < 0)
        UNRESOLVED(errno, "pipe() failed");

    pid = fork();
    if (pid < 0)
        UNRESOLVED(errno, "fork() failed");

    if (pid == 0) {
        close(pipefd[0]);
        if (getrlimit(RLIMIT_NOFILE, &child_rl.nofile) < 0)
            _exit(2);
        if (getrlimit(RLIMIT_AS, &child_rl.as) < 0)
            _exit(3);
        if (getrlimit(RLIMIT_STACK, &child_rl.stack) < 0)
            _exit(4);
        if (write(pipefd[1], &child_rl, sizeof(child_rl)) != sizeof(child_rl))
            _exit(5);
        close(pipefd[1]);
        _exit(0);
    }

    close(pipefd[1]);
    if (read(pipefd[0], &child_rl, sizeof(child_rl)) != sizeof(child_rl)) {
        close(pipefd[0]);
        UNRESOLVED(errno, "Failed to read child rlimits");
    }
    close(pipefd[0]);

    if (waitpid(pid, &status, 0) != pid)
        UNRESOLVED(errno, "waitpid failed");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        output("Child exit status: %d\n", WEXITSTATUS(status));
        FAILED("Child exited abnormally");
    }

    output("Child RLIMIT_NOFILE: cur=%lu max=%lu\n",
           (unsigned long)child_rl.nofile.rlim_cur,
           (unsigned long)child_rl.nofile.rlim_max);

    /* Verify RLIMIT_NOFILE */
    if (child_rl.nofile.rlim_cur != parent_rl.nofile.rlim_cur ||
        child_rl.nofile.rlim_max != parent_rl.nofile.rlim_max)
        FAILED("RLIMIT_NOFILE mismatch");

    /* Verify RLIMIT_AS */
    if (child_rl.as.rlim_cur != parent_rl.as.rlim_cur ||
        child_rl.as.rlim_max != parent_rl.as.rlim_max)
        FAILED("RLIMIT_AS mismatch");

    /* Verify RLIMIT_STACK */
    if (child_rl.stack.rlim_cur != parent_rl.stack.rlim_cur ||
        child_rl.stack.rlim_max != parent_rl.stack.rlim_max)
        FAILED("RLIMIT_STACK mismatch");

    output("PASS: all resource limits inherited correctly\n");
    output("\n=== Resource limit tests PASSED ===\n");
    PASSED;
}
