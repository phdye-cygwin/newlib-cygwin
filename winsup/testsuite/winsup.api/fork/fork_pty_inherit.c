/*
 * Cygwin fork() test - PTY Inheritance
 *
 * Verifies that PTY master/slave file descriptors are correctly
 * inherited across fork() and that communication works.
 *
 * Tests handle fixup for PTY fhandlers.
 */

#define _XOPEN_SOURCE 600

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

#define TEST_MSG "pty-test-data"

int main(void)
{
    int master, slave;
    char *slave_name;
    pid_t pid;
    int status;
    char buf[256];
    ssize_t n;

    output_init();
    output("Testing PTY inheritance across fork()\n");

    alarm(30);

    /* Open a PTY pair */
    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0)
        UNRESOLVED(errno, "posix_openpt() failed");

    if (grantpt(master) < 0) {
        close(master);
        UNRESOLVED(errno, "grantpt() failed");
    }

    if (unlockpt(master) < 0) {
        close(master);
        UNRESOLVED(errno, "unlockpt() failed");
    }

    slave_name = ptsname(master);
    if (!slave_name) {
        close(master);
        UNRESOLVED(errno, "ptsname() failed");
    }
    output("PTY slave: %s\n", slave_name);

    slave = open(slave_name, O_RDWR | O_NOCTTY);
    if (slave < 0) {
        close(master);
        UNRESOLVED(errno, "open(slave) failed");
    }

    /* Fork: child writes to slave, parent reads from master */
    pid = fork();
    if (pid < 0) {
        close(master);
        close(slave);
        UNRESOLVED(errno, "fork() failed");
    }

    if (pid == 0) {
        /* Child: write to slave */
        close(master);
        if (write(slave, TEST_MSG, strlen(TEST_MSG)) != (ssize_t)strlen(TEST_MSG)) {
            close(slave);
            _exit(1);
        }
        close(slave);
        _exit(0);
    }

    /* Parent: read from master */
    close(slave);

    memset(buf, 0, sizeof(buf));
    n = read(master, buf, sizeof(buf) - 1);
    close(master);

    if (waitpid(pid, &status, 0) != pid)
        UNRESOLVED(errno, "waitpid failed");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        output("Child exit status: %d\n", WEXITSTATUS(status));
        FAILED("Child failed to write to PTY slave");
    }

    if (n <= 0) {
        output("Parent read returned %zd: %s\n", n, strerror(errno));
        FAILED("Parent failed to read from PTY master");
    }

    /* PTY may do line discipline transformations, just check prefix */
    if (strncmp(buf, TEST_MSG, strlen(TEST_MSG)) != 0) {
        output("Parent read: '%s' (expected prefix '%s')\n", buf, TEST_MSG);
        FAILED("PTY data mismatch");
    }

    output("PASS: PTY master/slave inherited and communication works\n");
    output("\n=== PTY inheritance test PASSED ===\n");
    PASSED;
}
