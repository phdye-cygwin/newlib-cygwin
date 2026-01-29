/*
 * Cygwin fork() test - Pipe Handle Fixup
 *
 * Verifies that pipe file descriptors are correctly inherited
 * across fork() and that parent-child communication via pipes works.
 * Also tests pipe2(O_CLOEXEC) behavior: cloexec FDs are inherited
 * by fork but would be closed on exec.
 *
 * Tests handle fixup for named pipe (npfs) fhandlers.
 */

#define _GNU_SOURCE

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

#define MSG_PARENT "message-from-parent"
#define MSG_CHILD  "message-from-child"

int main(void)
{
    int p2c[2], c2p[2];
    pid_t pid;
    int status;
    char buf[256];
    ssize_t n;

    output_init();
    output("Testing pipe handle inheritance across fork()\n");

    alarm(30);

    /* Test 1: Bidirectional pipe communication */
    output("Test 1: Bidirectional pipe communication\n");

    if (pipe(p2c) < 0)
        UNRESOLVED(errno, "pipe(p2c) failed");
    if (pipe(c2p) < 0)
        UNRESOLVED(errno, "pipe(c2p) failed");

    pid = fork();
    if (pid < 0)
        UNRESOLVED(errno, "fork() failed");

    if (pid == 0) {
        /* Child: read from p2c, write to c2p */
        close(p2c[1]);
        close(c2p[0]);

        memset(buf, 0, sizeof(buf));
        n = read(p2c[0], buf, sizeof(buf) - 1);
        close(p2c[0]);
        if (n <= 0)
            _exit(1);
        if (strcmp(buf, MSG_PARENT) != 0)
            _exit(2);

        if (write(c2p[1], MSG_CHILD, strlen(MSG_CHILD)) != (ssize_t)strlen(MSG_CHILD)) {
            close(c2p[1]);
            _exit(3);
        }
        close(c2p[1]);
        _exit(0);
    }

    /* Parent: write to p2c, read from c2p */
    close(p2c[0]);
    close(c2p[1]);

    if (write(p2c[1], MSG_PARENT, strlen(MSG_PARENT)) != (ssize_t)strlen(MSG_PARENT)) {
        close(p2c[1]);
        close(c2p[0]);
        FAILED("Parent write to pipe failed");
    }
    close(p2c[1]);

    memset(buf, 0, sizeof(buf));
    n = read(c2p[0], buf, sizeof(buf) - 1);
    close(c2p[0]);

    if (waitpid(pid, &status, 0) != pid)
        UNRESOLVED(errno, "waitpid failed");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        output("Child exit status: %d\n", WEXITSTATUS(status));
        FAILED("Child failed pipe communication test");
    }

    if (n <= 0 || strcmp(buf, MSG_CHILD) != 0) {
        output("Parent received: '%s' (expected '%s')\n", buf, MSG_CHILD);
        FAILED("Pipe data mismatch");
    }
    output("PASS: bidirectional pipe communication works\n");

    /* Test 2: pipe2(O_CLOEXEC) — FDs inherited by fork */
    output("\nTest 2: pipe2(O_CLOEXEC) inherited by fork\n");

    int cloexec_pipe[2];
    if (pipe2(cloexec_pipe, O_CLOEXEC) < 0) {
        output("pipe2(O_CLOEXEC) not supported, skipping\n");
    } else {
        pid = fork();
        if (pid < 0)
            UNRESOLVED(errno, "fork() failed");

        if (pid == 0) {
            /* Child: cloexec FD should still be valid after fork */
            close(cloexec_pipe[0]);
            int flags = fcntl(cloexec_pipe[1], F_GETFD);
            if (flags < 0)
                _exit(1); /* FD not inherited — FAIL */
            if (!(flags & FD_CLOEXEC))
                _exit(2); /* CLOEXEC flag not preserved */
            /* Write to prove it works */
            if (write(cloexec_pipe[1], "ok", 2) != 2)
                _exit(3);
            close(cloexec_pipe[1]);
            _exit(0);
        }

        close(cloexec_pipe[1]);
        memset(buf, 0, sizeof(buf));
        n = read(cloexec_pipe[0], buf, sizeof(buf));
        close(cloexec_pipe[0]);

        if (waitpid(pid, &status, 0) != pid)
            UNRESOLVED(errno, "waitpid failed");
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            output("Child exit status: %d\n", WEXITSTATUS(status));
            FAILED("pipe2(O_CLOEXEC) FDs not inherited by fork");
        }
        if (n != 2 || memcmp(buf, "ok", 2) != 0)
            FAILED("pipe2(O_CLOEXEC) pipe data mismatch");
        output("PASS: pipe2(O_CLOEXEC) FDs inherited by fork with flag preserved\n");
    }

    output("\n=== All pipe inheritance tests PASSED ===\n");
    PASSED;
}
