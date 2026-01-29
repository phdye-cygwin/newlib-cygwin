/*
 * Test: SIGKILL delivery to fork() children
 *
 * Verifies that SIGKILL is actually delivered to forked children,
 * not just accepted by kill().  The child must be terminated by the
 * signal (WIFSIGNALED, WTERMSIG==SIGKILL), not exit normally.
 *
 * This test exposes a bug where the first child forked after a prior
 * fork+waitpid cycle is immune to SIGKILL: kill() returns 0 but the
 * child is never killed.
 *
 * Test structure:
 *   1. Fork+waitpid a "warmup" child (exits immediately)
 *   2. Fork a long-sleeping child
 *   3. Wait for child to initialize (1 second)
 *   4. Send SIGKILL
 *   5. Poll waitpid with timeout — child must die within 5 seconds
 *   6. Verify child was killed by signal 9, not normal exit
 */

#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "posixtest.h"
#include "testfrmw.h"
#include "testfrmw.c"

#define KILL_TIMEOUT_POLLS 50   /* 50 * 100ms = 5 seconds max */
#define POLL_INTERVAL_US  100000 /* 100ms */

/* Fork a child, verify SIGKILL actually terminates it.
   Returns 0 on success, -1 on failure.  Never blocks indefinitely. */
static int test_sigkill_delivery(const char *label)
{
    pid_t pid, wpid;
    int status, kr, i;

    pid = fork();
    if (pid < 0)
    {
        output("%s: fork failed: %s\n", label, strerror(errno));
        return -1;
    }

    if (pid == 0)
    {
        /* Child: sleep long enough to prove SIGKILL killed us.
           If SIGKILL works, we never reach _exit.
           30s is short enough for cygrun's 60s timeout but long
           enough that natural exit won't be confused with SIGKILL
           (the poll timeout is only 5s). */
        sleep(30);
        _exit(0);
    }

    /* Parent: let child fully initialize */
    sleep(1);

    /* Send SIGKILL */
    errno = 0;
    kr = kill(pid, SIGKILL);
    if (kr != 0)
    {
        output("%s: kill(%d, SIGKILL) failed: %s\n",
               label, pid, strerror(errno));
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return -1;
    }
    output("%s: kill(%d, SIGKILL) returned 0\n", label, pid);

    /* Poll waitpid with WNOHANG — never block indefinitely */
    for (i = 0; i < KILL_TIMEOUT_POLLS; i++)
    {
        wpid = waitpid(pid, &status, WNOHANG);
        if (wpid == pid)
            goto reaped;
        if (wpid < 0)
        {
            output("%s: waitpid error: %s\n", label, strerror(errno));
            return -1;
        }
        usleep(POLL_INTERVAL_US);
    }

    /* Timed out — child was NOT killed by SIGKILL */
    output("%s: SIGKILL had no effect after %d ms — child %d still alive\n",
           label, (KILL_TIMEOUT_POLLS * POLL_INTERVAL_US) / 1000, pid);
    /* Kill child again and brief wait — cygrun will reap if needed */
    kill(pid, SIGKILL);
    for (i = 0; i < 30; i++)  /* wait up to 3s */
    {
        if (waitpid(pid, NULL, WNOHANG) == pid)
            break;
        usleep(100000);
    }
    return -1;

reaped:
    /* Verify child was killed by signal, not normal exit */
    if (!WIFSIGNALED(status))
    {
        if (WIFEXITED(status))
            output("%s: child exited normally with status %d "
                   "(SIGKILL was NOT delivered)\n",
                   label, WEXITSTATUS(status));
        else
            output("%s: child terminated with unexpected status 0x%x\n",
                   label, status);
        return -1;
    }

    if (WTERMSIG(status) != SIGKILL)
    {
        output("%s: child killed by signal %d, expected %d (SIGKILL)\n",
               label, WTERMSIG(status), SIGKILL);
        return -1;
    }

    output("%s: PASS (child %d killed by SIGKILL in %d ms)\n",
           label, pid, i * (POLL_INTERVAL_US / 1000));
    return 0;
}

int main(void)
{
    pid_t pid;
    int status;

    output_init();
    output("=== Testing SIGKILL delivery to fork children ===\n\n");

    /* Step 1: Warmup fork+waitpid cycle.
       This is critical to trigger the bug — the FIRST child forked
       AFTER a completed fork+waitpid is the one affected. */
    output("Step 1: Warmup fork+waitpid\n");
    pid = fork();
    if (pid < 0)
        FAILED("warmup fork failed");
    if (pid == 0)
        _exit(42);
    if (waitpid(pid, &status, 0) != pid)
        FAILED("warmup waitpid failed");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 42)
        FAILED("warmup child wrong exit status");
    output("Step 1: warmup complete\n\n");

    /* Step 2: Test SIGKILL on first child after warmup */
    output("Step 2: SIGKILL first child after warmup\n");
    if (test_sigkill_delivery("first-child") != 0)
        FAILED("SIGKILL not delivered to first child after warmup fork");

    /* Step 3: Test SIGKILL on second child (for comparison) */
    output("\nStep 3: SIGKILL second child\n");
    if (test_sigkill_delivery("second-child") != 0)
        FAILED("SIGKILL not delivered to second child");

    output("\n=== SIGKILL delivery verified ===\n");
    PASSED;
}
