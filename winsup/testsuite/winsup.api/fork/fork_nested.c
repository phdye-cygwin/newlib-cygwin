/*
 * Cygwin fork() test - Nested Fork
 *
 * Verifies that a child can fork a grandchild, and that all
 * process relationships and wait operations work correctly
 * across three generations.
 *
 * Parent → Child → Grandchild
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

struct pid_report {
    pid_t grandchild_pid;
    pid_t grandchild_ppid;
    pid_t grandchild_exit;
};

int main(void)
{
    pid_t parent_pid, child_pid;
    int p2c[2]; /* parent reads child's report */
    int status;

    output_init();
    output("Testing nested fork (parent → child → grandchild)\n");

    alarm(30);

    parent_pid = getpid();
    output("Parent PID: %d\n", (int)parent_pid);

    if (pipe(p2c) < 0)
        UNRESOLVED(errno, "pipe() failed");

    child_pid = fork();
    if (child_pid < 0)
        UNRESOLVED(errno, "fork() failed");

    if (child_pid == 0) {
        /* Child process */
        close(p2c[0]);

        pid_t my_pid = getpid();
        struct pid_report report;

        /* Fork grandchild */
        pid_t gc_pid = fork();
        if (gc_pid < 0) {
            report.grandchild_pid = -1;
            write(p2c[1], &report, sizeof(report));
            close(p2c[1]);
            _exit(1);
        }

        if (gc_pid == 0) {
            /* Grandchild */
            pid_t gc_ppid = getppid();
            /* Verify grandchild's parent is the child, not the original parent */
            if (gc_ppid != my_pid)
                _exit(10);
            _exit(77); /* distinctive exit code */
        }

        /* Child: wait for grandchild */
        int gc_status;
        pid_t w = waitpid(gc_pid, &gc_status, 0);
        if (w != gc_pid) {
            report.grandchild_pid = -1;
            write(p2c[1], &report, sizeof(report));
            close(p2c[1]);
            _exit(2);
        }

        report.grandchild_pid = gc_pid;
        report.grandchild_ppid = my_pid; /* child's PID = grandchild's expected PPID */
        report.grandchild_exit = WIFEXITED(gc_status) ? WEXITSTATUS(gc_status) : -1;

        if (write(p2c[1], &report, sizeof(report)) != sizeof(report))
            _exit(3);
        close(p2c[1]);
        _exit(0);
    }

    /* Parent */
    close(p2c[1]);

    struct pid_report report;
    ssize_t n = read(p2c[0], &report, sizeof(report));
    close(p2c[0]);

    if (n != sizeof(report))
        UNRESOLVED(errno, "Failed to read child's report");

    if (waitpid(child_pid, &status, 0) != child_pid)
        UNRESOLVED(errno, "waitpid(child) failed");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        output("Child exit status: %d\n", WEXITSTATUS(status));
        FAILED("Child exited abnormally");
    }

    output("Child PID: %d\n", (int)child_pid);
    output("Grandchild PID: %d\n", (int)report.grandchild_pid);
    output("Grandchild exit code: %d\n", report.grandchild_exit);

    /* Verify all PIDs are distinct */
    if (report.grandchild_pid <= 0)
        FAILED("Grandchild fork failed");
    if (parent_pid == child_pid || parent_pid == report.grandchild_pid ||
        child_pid == report.grandchild_pid)
        FAILED("PIDs are not all distinct");
    output("PASS: all three PIDs are distinct\n");

    /* Verify grandchild exited with expected code */
    if (report.grandchild_exit != 77) {
        if (report.grandchild_exit == 10)
            FAILED("Grandchild's PPID was wrong (not child's PID)");
        output("Grandchild exit code: %d (expected 77)\n", report.grandchild_exit);
        FAILED("Grandchild exited with wrong code");
    }
    output("PASS: grandchild exited correctly, PPID was child's PID\n");

    output("\n=== Nested fork test PASSED ===\n");
    PASSED;
}
