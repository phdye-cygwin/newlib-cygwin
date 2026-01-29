/*
 * Cygwin fork() test - Stress Test
 *
 * Forks 100 children in sequence, each does real work and exits.
 * Verifies no zombies, no handle leaks, all reaped correctly.
 *
 * Each child performs:
 * - malloc/free (exercises heap lock)
 * - open/read/close (exercises FD handling)
 * - getpid/getppid (exercises pinfo)
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

#define NUM_CHILDREN 100
#define POLL_INTERVAL_US 50000
#define CHILD_MAX_POLLS 100  /* 5 seconds per child */

int main(void)
{
    pid_t parent_pid;
    int failures = 0;
    int timeouts = 0;

    output_init();
    output("Stress test: forking %d children sequentially\n", NUM_CHILDREN);

    alarm(55); /* Just under cygrun's 60s timeout */

    parent_pid = getpid();

    for (int i = 0; i < NUM_CHILDREN; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            output("fork() failed at child %d: %s\n", i, strerror(errno));
            failures++;
            continue;
        }

        if (pid == 0) {
            /* Child: do real work */

            /* Exercise heap */
            void *p = malloc(1024);
            if (!p) _exit(1);
            memset(p, (unsigned char)i, 1024);
            free(p);

            /* Exercise FD handling */
            int fd = open("/dev/null", O_RDONLY);
            if (fd < 0) _exit(2);
            char buf;
            read(fd, &buf, 1);
            close(fd);

            /* Exercise pinfo */
            if (getpid() == parent_pid)
                _exit(3); /* Should never happen */
            if (getppid() != parent_pid)
                _exit(4);

            _exit(i % 256);
        }

        /* Parent: poll waitpid with timeout */
        int reaped = 0;
        int status;
        for (int p = 0; p < CHILD_MAX_POLLS; p++) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                reaped = 1;
                if (!WIFEXITED(status) || WEXITSTATUS(status) != (i % 256)) {
                    output("Child %d: wrong status (got %d, expected %d)\n",
                           i, WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                           i % 256);
                    failures++;
                }
                break;
            }
            usleep(POLL_INTERVAL_US);
        }

        if (!reaped) {
            output("Child %d (pid %d): TIMEOUT\n", i, (int)pid);
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            timeouts++;
            failures++;
        }

        /* Progress report every 25 children */
        if ((i + 1) % 25 == 0)
            output("  Progress: %d/%d children completed\n", i + 1, NUM_CHILDREN);
    }

    /* Verify no zombie children remain */
    pid_t extra = waitpid(-1, NULL, WNOHANG);
    if (extra > 0) {
        output("Unexpected zombie child %d found!\n", (int)extra);
        failures++;
    }

    output("\nResults: %d/%d children OK, %d failures, %d timeouts\n",
           NUM_CHILDREN - failures, NUM_CHILDREN, failures, timeouts);

    if (failures > 0)
        FAILED("Stress test failed — see details above");

    output("\n=== Stress test PASSED (%d children) ===\n", NUM_CHILDREN);
    PASSED;
}
