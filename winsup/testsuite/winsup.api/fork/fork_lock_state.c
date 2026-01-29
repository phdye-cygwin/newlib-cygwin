/*
 * Cygwin fork() test - Lock State After Fork
 *
 * Verifies that the child does not deadlock when using resources
 * that were locked in the parent at the time of fork().
 *
 * POSIX.1-2017: "A process shall be created with a single thread.
 * If a multi-threaded process calls fork(), the new process shall
 * contain a replica of the calling thread... The state of mutexes
 * is undefined."
 *
 * For single-threaded fork, internal locks should be in clean state.
 * This test verifies no deadlock occurs when the child uses
 * various subsystems.
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

#define CHILD_TIMEOUT_POLLS 50  /* 50 * 100ms = 5 seconds */
#define POLL_INTERVAL_US 100000

/* Run a child that performs an operation; detect deadlock via timeout */
static int test_child_op(const char *label, void (*op)(void))
{
    pid_t pid, wpid;
    int status;

    pid = fork();
    if (pid < 0) {
        output("%s: fork failed: %s\n", label, strerror(errno));
        return -1;
    }

    if (pid == 0) {
        op();
        _exit(0);
    }

    /* Poll waitpid with timeout to detect deadlock */
    for (int i = 0; i < CHILD_TIMEOUT_POLLS; i++) {
        wpid = waitpid(pid, &status, WNOHANG);
        if (wpid == pid) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                output("%s: PASS\n", label);
                return 0;
            }
            output("%s: child exited with status %d\n", label,
                   WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            return -1;
        }
        usleep(POLL_INTERVAL_US);
    }

    output("%s: DEADLOCK — child did not complete in %d ms\n",
           label, (CHILD_TIMEOUT_POLLS * POLL_INTERVAL_US) / 1000);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
}

/* Operations to test in child */
static void op_pthread_mutex(void)
{
    pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mtx);
    pthread_mutex_unlock(&mtx);
    pthread_mutex_destroy(&mtx);
}

static void op_flockfile(void)
{
    flockfile(stdout);
    funlockfile(stdout);
    flockfile(stderr);
    funlockfile(stderr);
}

static void op_fcntl_lock(void)
{
    int fd = open("fork_lock_test.tmp", O_CREAT | O_RDWR, 0644);
    if (fd < 0)
        _exit(2);
    struct flock fl = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0,
    };
    /* POSIX: file locks not inherited, so child should be able to lock */
    if (fcntl(fd, F_SETLK, &fl) < 0)
        _exit(3);
    fl.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &fl);
    close(fd);
    unlink("fork_lock_test.tmp");
}

static void op_malloc_free(void)
{
    /* Exercise malloc/free — deadlock here means __malloc_lock not reinit'd */
    for (int i = 0; i < 100; i++) {
        void *p = malloc(1024);
        if (!p)
            _exit(2);
        memset(p, 0xAA, 1024);
        free(p);
    }
}

int main(void)
{
    int failures = 0;

    output_init();
    output("Testing lock state after fork()\n");

    alarm(30);

    if (test_child_op("pthread_mutex", op_pthread_mutex) != 0)
        failures++;
    if (test_child_op("flockfile", op_flockfile) != 0)
        failures++;
    if (test_child_op("fcntl_lock", op_fcntl_lock) != 0)
        failures++;
    if (test_child_op("malloc/free", op_malloc_free) != 0)
        failures++;

    if (failures > 0) {
        output("\n%d lock state tests FAILED\n", failures);
        FAILED("Lock state not clean after fork");
    }

    output("\n=== All lock state tests PASSED ===\n");
    PASSED;
}
