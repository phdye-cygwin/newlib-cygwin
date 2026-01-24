/*
 * POSIX fork() test - Assertion #23: Return values
 *
 * This test verifies that fork() returns:
 * - 0 to the child process
 * - The child's PID to the parent process
 *
 * POSIX.1-2017: "Upon successful completion, fork() shall return 0 to the
 * child process and shall return the process ID of the child process to
 * the parent process."
 */

#define _POSIX_C_SOURCE 200112L

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
    pid_t fork_ret, my_pid, parent_pid, child_pid;
    int pipefd[2];
    int status;
    ssize_t n;
    pid_t child_reported_pid;
    pid_t child_reported_ppid;

    output_init();
    output("Testing fork() return values\n");

    /* Create pipe for child to report its PID back */
    if (pipe(pipefd) < 0) {
        UNRESOLVED(errno, "pipe() failed");
    }

    parent_pid = getpid();
    output("Parent PID: %d\n", (int)parent_pid);

    fork_ret = fork();
    if (fork_ret < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        UNRESOLVED(errno, "fork() failed");
    }

    if (fork_ret == 0) {
        /* Child process */
        close(pipefd[0]); /* Close read end */
        
        my_pid = getpid();
        pid_t my_ppid = getppid();
        
        output("Child: fork() returned 0 (correct)\n");
        output("Child: getpid() = %d\n", (int)my_pid);
        output("Child: getppid() = %d\n", (int)my_ppid);
        
        /* Check that fork() returned 0 */
        if (fork_ret != 0) {
            output("Child: ERROR - fork() did not return 0!\n");
            _exit(1);
        }

        /* Send our PID and PPID to parent */
        pid_t report[2] = { my_pid, my_ppid };
        if (write(pipefd[1], report, sizeof(report)) != sizeof(report)) {
            output("Child: failed to write to pipe\n");
            _exit(2);
        }
        
        close(pipefd[1]);
        _exit(0);
    }

    /* Parent process */
    close(pipefd[1]); /* Close write end */
    
    output("Parent: fork() returned %d\n", (int)fork_ret);
    
    /* Check that fork() returned a positive value (child PID) */
    if (fork_ret <= 0) {
        close(pipefd[0]);
        FAILED("fork() did not return positive value to parent");
    }

    /* Read child's reported PID and PPID */
    pid_t report[2];
    n = read(pipefd[0], report, sizeof(report));
    close(pipefd[0]);
    
    if (n != sizeof(report)) {
        UNRESOLVED(errno, "Failed to read child's report");
    }
    
    child_reported_pid = report[0];
    child_reported_ppid = report[1];
    
    output("Child reported: PID=%d, PPID=%d\n", 
           (int)child_reported_pid, (int)child_reported_ppid);

    /* Wait for child */
    child_pid = waitpid(fork_ret, &status, 0);
    
    if (child_pid != fork_ret) {
        output("waitpid returned %d, expected %d\n", (int)child_pid, (int)fork_ret);
        FAILED("waitpid returned wrong PID");
    }
    
    if (!WIFEXITED(status)) {
        FAILED("Child did not exit normally");
    }
    
    if (WEXITSTATUS(status) != 0) {
        FAILED("Child reported an error");
    }

    /* Verify that fork()'s return value matches child's actual PID */
    if (fork_ret != child_reported_pid) {
        output("ERROR: fork() returned %d but child's PID is %d\n",
               (int)fork_ret, (int)child_reported_pid);
        FAILED("fork() return value does not match child's PID");
    }
    output("VERIFIED: fork() return value (%d) matches child's getpid() (%d)\n",
           (int)fork_ret, (int)child_reported_pid);

    /* Verify child's parent PID matches our PID */
    if (child_reported_ppid != parent_pid) {
        output("ERROR: child's getppid() = %d but parent's getpid() = %d\n",
               (int)child_reported_ppid, (int)parent_pid);
        FAILED("Child's PPID does not match parent's PID");
    }
    output("VERIFIED: child's getppid() (%d) matches parent's getpid() (%d)\n",
           (int)child_reported_ppid, (int)parent_pid);

    output("\n=== All return value tests PASSED ===\n");
    PASSED
    return 0;
}
