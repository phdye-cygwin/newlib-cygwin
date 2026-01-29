/*
 * POSIX fork() test - Working Directory Inheritance
 *
 * Verifies that the child inherits the parent's current working
 * directory, and that changes in the child do not affect the parent.
 *
 * POSIX.1-2017: "The child process shall inherit the current
 * working directory."
 */

#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "testfrmw.h"
#include "testfrmw.c"

#define TMPDIR_TEMPLATE "/tmp/fork_cwd_XXXXXX"

int main(void)
{
    char tmpdir[PATH_MAX];
    char parent_cwd[PATH_MAX];
    char child_cwd[PATH_MAX];
    pid_t pid;
    int pipefd[2];
    int status;
    ssize_t n;

    output_init();
    output("Testing working directory inheritance across fork()\n");

    alarm(30);

    /* Create a temporary directory and chdir to it */
    strncpy(tmpdir, TMPDIR_TEMPLATE, sizeof(tmpdir));
    if (!mkdtemp(tmpdir))
        UNRESOLVED(errno, "mkdtemp failed");

    if (chdir(tmpdir) < 0) {
        rmdir(tmpdir);
        UNRESOLVED(errno, "chdir failed");
    }

    if (!getcwd(parent_cwd, sizeof(parent_cwd))) {
        rmdir(tmpdir);
        UNRESOLVED(errno, "getcwd failed");
    }
    output("Parent CWD: %s\n", parent_cwd);

    if (pipe(pipefd) < 0)
        UNRESOLVED(errno, "pipe() failed");

    pid = fork();
    if (pid < 0)
        UNRESOLVED(errno, "fork() failed");

    if (pid == 0) {
        close(pipefd[0]);
        if (!getcwd(child_cwd, sizeof(child_cwd)))
            _exit(2);
        size_t len = strlen(child_cwd) + 1;
        if (write(pipefd[1], child_cwd, len) != (ssize_t)len)
            _exit(3);
        /* Change directory in child to prove isolation */
        chdir("/tmp");
        close(pipefd[1]);
        _exit(0);
    }

    close(pipefd[1]);
    memset(child_cwd, 0, sizeof(child_cwd));
    n = read(pipefd[0], child_cwd, sizeof(child_cwd) - 1);
    close(pipefd[0]);

    if (n <= 0)
        UNRESOLVED(errno, "Failed to read child CWD");

    if (waitpid(pid, &status, 0) != pid)
        UNRESOLVED(errno, "waitpid failed");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        FAILED("Child exited abnormally");

    output("Child CWD: %s\n", child_cwd);

    if (strcmp(child_cwd, parent_cwd) != 0) {
        output("ERROR: child CWD '%s' != parent CWD '%s'\n",
               child_cwd, parent_cwd);
        FAILED("Child CWD does not match parent");
    }
    output("PASS: child inherited parent's CWD\n");

    /* Verify parent CWD unchanged */
    char verify_cwd[PATH_MAX];
    if (!getcwd(verify_cwd, sizeof(verify_cwd)))
        UNRESOLVED(errno, "getcwd failed after child exit");

    if (strcmp(verify_cwd, parent_cwd) != 0) {
        output("ERROR: parent CWD changed to '%s'\n", verify_cwd);
        FAILED("Parent CWD changed after child modified its copy");
    }
    output("PASS: parent CWD unchanged after child chdir\n");

    /* Cleanup */
    chdir("/tmp");
    rmdir(tmpdir);

    output("\n=== All CWD tests PASSED ===\n");
    PASSED;
}
