/*
 * Cygwin fork() test - Socket Handle Fixup
 *
 * Verifies that socket file descriptors are correctly inherited
 * across fork() and that parent-child communication via sockets works.
 *
 * Tests handle fixup for socket fhandlers (AF_UNIX socketpair).
 */

#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

#include "testfrmw.h"
#include "testfrmw.c"

#define MSG_PARENT "socket-from-parent"
#define MSG_CHILD  "socket-from-child"

int main(void)
{
    int sv[2];
    pid_t pid;
    int status;
    char buf[256];
    ssize_t n;

    output_init();
    output("Testing socket handle inheritance across fork()\n");

    alarm(30);

    /* Test 1: AF_UNIX SOCK_STREAM socketpair */
    output("Test 1: AF_UNIX SOCK_STREAM socketpair\n");

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        UNRESOLVED(errno, "socketpair(AF_UNIX, SOCK_STREAM) failed");

    pid = fork();
    if (pid < 0)
        UNRESOLVED(errno, "fork() failed");

    if (pid == 0) {
        /* Child uses sv[1] */
        close(sv[0]);

        /* Read parent's message */
        memset(buf, 0, sizeof(buf));
        n = recv(sv[1], buf, sizeof(buf) - 1, 0);
        if (n <= 0)
            _exit(1);
        if (strcmp(buf, MSG_PARENT) != 0)
            _exit(2);

        /* Send reply */
        if (send(sv[1], MSG_CHILD, strlen(MSG_CHILD), 0) != (ssize_t)strlen(MSG_CHILD))
            _exit(3);

        close(sv[1]);
        _exit(0);
    }

    /* Parent uses sv[0] */
    close(sv[1]);

    if (send(sv[0], MSG_PARENT, strlen(MSG_PARENT), 0) != (ssize_t)strlen(MSG_PARENT)) {
        close(sv[0]);
        FAILED("Parent send failed");
    }

    memset(buf, 0, sizeof(buf));
    n = recv(sv[0], buf, sizeof(buf) - 1, 0);
    close(sv[0]);

    if (waitpid(pid, &status, 0) != pid)
        UNRESOLVED(errno, "waitpid failed");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        output("Child exit status: %d\n", WEXITSTATUS(status));
        FAILED("Child failed socket communication");
    }

    if (n <= 0 || strcmp(buf, MSG_CHILD) != 0) {
        output("Parent received: '%s' (expected '%s')\n", buf, MSG_CHILD);
        FAILED("Socket data mismatch");
    }
    output("PASS: AF_UNIX SOCK_STREAM socketpair works across fork\n");

    /* Test 2: AF_UNIX SOCK_DGRAM socketpair */
    output("\nTest 2: AF_UNIX SOCK_DGRAM socketpair\n");

    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0) {
        output("socketpair(AF_UNIX, SOCK_DGRAM) failed: %s — skipping\n",
               strerror(errno));
    } else {
        pid = fork();
        if (pid < 0)
            UNRESOLVED(errno, "fork() failed");

        if (pid == 0) {
            close(sv[0]);
            memset(buf, 0, sizeof(buf));
            n = recv(sv[1], buf, sizeof(buf) - 1, 0);
            if (n <= 0 || strcmp(buf, MSG_PARENT) != 0)
                _exit(1);
            if (send(sv[1], MSG_CHILD, strlen(MSG_CHILD), 0) != (ssize_t)strlen(MSG_CHILD))
                _exit(2);
            close(sv[1]);
            _exit(0);
        }

        close(sv[1]);
        if (send(sv[0], MSG_PARENT, strlen(MSG_PARENT), 0) != (ssize_t)strlen(MSG_PARENT)) {
            close(sv[0]);
            FAILED("Parent send (dgram) failed");
        }
        memset(buf, 0, sizeof(buf));
        n = recv(sv[0], buf, sizeof(buf) - 1, 0);
        close(sv[0]);

        if (waitpid(pid, &status, 0) != pid)
            UNRESOLVED(errno, "waitpid failed");
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            output("Child exit status: %d\n", WEXITSTATUS(status));
            FAILED("Child failed DGRAM socket communication");
        }
        if (n <= 0 || strcmp(buf, MSG_CHILD) != 0)
            FAILED("DGRAM socket data mismatch");
        output("PASS: AF_UNIX SOCK_DGRAM socketpair works across fork\n");
    }

    output("\n=== All socket inheritance tests PASSED ===\n");
    PASSED;
}
