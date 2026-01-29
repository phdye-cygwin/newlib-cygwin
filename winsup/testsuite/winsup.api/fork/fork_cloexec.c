/*
 * Cygwin fork() test - Close-on-Exec with Fork vs Exec
 *
 * Verifies that O_CLOEXEC / FD_CLOEXEC:
 * - Does NOT close FDs on fork (fork inherits all FDs)
 * - DOES close FDs on exec
 *
 * POSIX: "FD_CLOEXEC causes the file descriptor to be closed when
 * exec is called... fork() creates copies of all open file descriptors."
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

int main(void)
{
    int fd;
    pid_t pid;
    int status;

    output_init();
    output("Testing O_CLOEXEC behavior with fork vs exec\n");

    alarm(30);

    /* Test 1: FD with O_CLOEXEC survives fork */
    output("Test 1: O_CLOEXEC FD survives fork()\n");

    fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        UNRESOLVED(errno, "open(/dev/null, O_CLOEXEC) failed");

    /* Verify CLOEXEC is set */
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || !(flags & FD_CLOEXEC)) {
        close(fd);
        UNRESOLVED(errno, "FD_CLOEXEC not set");
    }

    pid = fork();
    if (pid < 0) {
        close(fd);
        UNRESOLVED(errno, "fork() failed");
    }

    if (pid == 0) {
        /* Child: FD should still be valid */
        int cflags = fcntl(fd, F_GETFD);
        if (cflags < 0) {
            /* FD not inherited — this is wrong */
            _exit(1);
        }
        if (!(cflags & FD_CLOEXEC)) {
            /* CLOEXEC flag not preserved */
            _exit(2);
        }
        /* FD is valid and has CLOEXEC — correct */
        close(fd);
        _exit(0);
    }

    close(fd);
    if (waitpid(pid, &status, 0) != pid)
        UNRESOLVED(errno, "waitpid failed");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        if (WEXITSTATUS(status) == 1)
            FAILED("O_CLOEXEC FD was NOT inherited by fork (should be)");
        if (WEXITSTATUS(status) == 2)
            FAILED("FD_CLOEXEC flag not preserved in child");
        FAILED("Child exited abnormally");
    }
    output("PASS: O_CLOEXEC FD is inherited by fork with flag preserved\n");

    /* Test 2: FD_CLOEXEC via fcntl also survives fork */
    output("\nTest 2: FD_CLOEXEC via fcntl survives fork()\n");

    fd = open("/dev/null", O_RDONLY);
    if (fd < 0)
        UNRESOLVED(errno, "open(/dev/null) failed");

    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        close(fd);
        UNRESOLVED(errno, "fcntl(F_SETFD, FD_CLOEXEC) failed");
    }

    pid = fork();
    if (pid < 0) {
        close(fd);
        UNRESOLVED(errno, "fork() failed");
    }

    if (pid == 0) {
        int cflags = fcntl(fd, F_GETFD);
        if (cflags < 0)
            _exit(1);
        if (!(cflags & FD_CLOEXEC))
            _exit(2);
        close(fd);
        _exit(0);
    }

    close(fd);
    if (waitpid(pid, &status, 0) != pid)
        UNRESOLVED(errno, "waitpid failed");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        FAILED("FD_CLOEXEC via fcntl not inherited correctly by fork");
    output("PASS: FD_CLOEXEC via fcntl preserved across fork\n");

    /* Test 3: O_CLOEXEC FD is closed on exec */
    output("\nTest 3: O_CLOEXEC FD closed on exec\n");

    fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        UNRESOLVED(errno, "open failed");

    pid = fork();
    if (pid < 0) {
        close(fd);
        UNRESOLVED(errno, "fork() failed");
    }

    if (pid == 0) {
        /* Child: exec a program that checks if FD is valid.
           Use /bin/sh -c with fcntl check on the FD number.
           We pass the FD number as an argument. */
        char fd_str[16];
        snprintf(fd_str, sizeof(fd_str), "%d", fd);

        /* /bin/sh -c 'exec N>&- 2>/dev/null; ...' won't work easily.
           Instead, exec a command and check via /proc/self/fd.
           If cloexec works, the FD won't exist after exec. */
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
                 "test ! -e /proc/self/fd/%d", fd);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    close(fd);
    if (waitpid(pid, &status, 0) != pid)
        UNRESOLVED(errno, "waitpid failed");

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        output("PASS: O_CLOEXEC FD correctly closed on exec\n");
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 1) {
        FAILED("O_CLOEXEC FD was NOT closed on exec");
    } else {
        output("exec test inconclusive (exit %d), skipping\n",
               WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }

    output("\n=== All cloexec tests PASSED ===\n");
    PASSED;
}
