/*
 * Copyright (c) 2004, Bull S.A..  All rights reserved.
 * Created by: Sebastien Decugis
 * Modified for Cygwin fork testing, 2026

 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * Test assertion FORK-002:
 * "The child process ID also shall not match any active process group ID"
 *
 * Steps:
 * 1. Create several process groups
 * 2. Fork
 * 3. Verify child PID doesn't match any active PGID
 */

#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "posixtest.h"
#include "testfrmw.h"
#include "testfrmw.c"

#define NUM_PGRPS 5

int main(void)
{
    pid_t child_pid, wpid;
    pid_t pgrps[NUM_PGRPS];
    int status, i;
    int pipe_fd[2];
    char buf[32];

    output_init();

    /* Create a pipe for child to report its PID */
    if (pipe(pipe_fd) != 0)
    {
        UNRESOLVED(errno, "Failed to create pipe");
    }

    /* Create several process groups by forking children that become
       their own process group leaders */
    for (i = 0; i < NUM_PGRPS; i++)
    {
        pid_t pg_child = fork();
        if (pg_child < 0)
        {
            UNRESOLVED(errno, "Failed to fork process group leader");
        }
        if (pg_child == 0)
        {
            /* Child becomes its own process group leader */
            if (setpgid(0, 0) != 0)
            {
                _exit(1);
            }
            /* Sleep to keep process group active */
            sleep(10);
            _exit(0);
        }
        pgrps[i] = pg_child;
        /* Wait a moment for child to set its process group */
        usleep(10000);
    }

    /* Now fork the test child */
    child_pid = fork();

    if (child_pid < 0)
    {
        /* Kill the process group leaders */
        for (i = 0; i < NUM_PGRPS; i++)
            kill(pgrps[i], SIGKILL);
        UNRESOLVED(errno, "Failed to fork test child");
    }

    if (child_pid == 0)
    {
        /* Child process */
        pid_t my_pid = getpid();
        
        close(pipe_fd[0]);
        
        /* Report our PID to parent */
        snprintf(buf, sizeof(buf), "%d", (int)my_pid);
        write(pipe_fd[1], buf, strlen(buf) + 1);
        close(pipe_fd[1]);
        
        _exit(PTS_PASS);
    }

    /* Parent process */
    close(pipe_fd[1]);

    /* Read child's PID */
    if (read(pipe_fd[0], buf, sizeof(buf)) <= 0)
    {
        for (i = 0; i < NUM_PGRPS; i++)
            kill(pgrps[i], SIGKILL);
        UNRESOLVED(errno, "Failed to read child PID from pipe");
    }
    close(pipe_fd[0]);

    pid_t reported_pid = atoi(buf);

    /* Verify child PID doesn't match any active PGID */
    for (i = 0; i < NUM_PGRPS; i++)
    {
        pid_t pgid = getpgid(pgrps[i]);
        if (pgid == reported_pid)
        {
            output("Child PID %d matches active PGID of process %d\n",
                   reported_pid, pgrps[i]);
            for (i = 0; i < NUM_PGRPS; i++)
                kill(pgrps[i], SIGKILL);
            FAILED("Child PID matches an active process group ID");
        }
    }

    /* Also check if child PID matches any of the PGRP leader PIDs
       (which are also their PGIDs) */
    for (i = 0; i < NUM_PGRPS; i++)
    {
        if (pgrps[i] == reported_pid)
        {
            output("Child PID %d matches PGRP leader PID\n", reported_pid);
            for (i = 0; i < NUM_PGRPS; i++)
                kill(pgrps[i], SIGKILL);
            FAILED("Child PID matches a process group leader PID");
        }
    }

    /* Wait for test child */
    wpid = waitpid(child_pid, &status, 0);
    if (wpid != child_pid)
    {
        for (i = 0; i < NUM_PGRPS; i++)
            kill(pgrps[i], SIGKILL);
        UNRESOLVED(errno, "waitpid failed");
    }

    /* Clean up process group leaders */
    for (i = 0; i < NUM_PGRPS; i++)
    {
        kill(pgrps[i], SIGKILL);
        waitpid(pgrps[i], NULL, 0);
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != PTS_PASS)
    {
        FAILED("Child did not exit normally");
    }

    output("Child PID %d does not match any of %d active process group IDs\n",
           reported_pid, NUM_PGRPS);
    PASSED;
}
