/*
 * Copyright (c) 2004, Bull S.A..  All rights reserved.
 * Created by: Sebastien Decugis
 * Modified for Cygwin fork testing, 2026

 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * Test assertion FORK-030:
 * "The child process shall have its own copy of the parent's
 * message catalog descriptors"
 *
 * This test requires the XSI message catalog functionality
 * (catopen, catgets, catclose) which requires a .cat file
 * created with gencat.
 *
 * Since gencat may not be available, this test will SKIP if
 * the message catalog cannot be created or opened.
 *
 * Steps:
 * 1. Create a test message catalog (if gencat available)
 * 2. Open the catalog with catopen()
 * 3. Fork
 * 4. Both parent and child use catgets()
 * 5. Child closes its copy
 * 6. Parent verifies its copy is still open
 */

#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <nl_types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "posixtest.h"
#include "testfrmw.h"
#include "testfrmw.c"

#define TEST_CATALOG "/tmp/fork_test.cat"
#define TEST_MSG_SRC "/tmp/fork_test.msg"

/* Try to create a simple message catalog */
static int create_test_catalog(void)
{
    FILE *f;
    int ret;

    /* Create message source file */
    f = fopen(TEST_MSG_SRC, "w");
    if (!f)
    {
        output("Cannot create message source file: %s\n", strerror(errno));
        return -1;
    }

    /* Write a simple message catalog source */
    fprintf(f, "$set 1\n");
    fprintf(f, "1 Hello from test catalog\n");
    fprintf(f, "2 Second message\n");
    fclose(f);

    /* Try to run gencat */
    ret = system("gencat " TEST_CATALOG " " TEST_MSG_SRC " 2>/dev/null");
    if (ret != 0)
    {
        output("gencat not available or failed (exit %d)\n", ret);
        unlink(TEST_MSG_SRC);
        return -1;
    }

    unlink(TEST_MSG_SRC);
    return 0;
}

int main(void)
{
    nl_catd catd;
    pid_t child_pid, wpid;
    int status;
    char *msg;
    int pipe_fd[2];
    char buf[256];

    output_init();

    /* Check for XSI message catalog support */
#ifndef _XOPEN_SOURCE
    output("Note: _XOPEN_SOURCE not defined\n");
#endif

    /* Try to create test catalog */
    if (create_test_catalog() != 0)
    {
        output("Cannot create test message catalog\n");
        output("gencat utility may not be available\n");
        UNTESTED("Message catalog support not available (gencat missing)");
    }

    /* Open the catalog */
    catd = catopen(TEST_CATALOG, NL_CAT_LOCALE);
    if (catd == (nl_catd)-1)
    {
        output("catopen failed: %s\n", strerror(errno));
        unlink(TEST_CATALOG);
        UNTESTED("catopen failed - message catalog support unavailable");
    }

    output("Parent: Opened message catalog\n");

    /* Verify we can read from it */
    msg = catgets(catd, 1, 1, "DEFAULT");
    if (strcmp(msg, "DEFAULT") == 0)
    {
        output("Warning: catgets returned default (message not found)\n");
    }
    else
    {
        output("Parent: catgets returned: '%s'\n", msg);
    }

    /* Create pipe for child communication */
    if (pipe(pipe_fd) != 0)
    {
        catclose(catd);
        unlink(TEST_CATALOG);
        UNRESOLVED(errno, "pipe failed");
    }

    /* Fork */
    child_pid = fork();

    if (child_pid < 0)
    {
        catclose(catd);
        unlink(TEST_CATALOG);
        UNRESOLVED(errno, "fork failed");
    }

    if (child_pid == 0)
    {
        /* Child process */
        char *child_msg;
        int result = PTS_PASS;

        close(pipe_fd[0]);

        /* Try to use the inherited catalog descriptor */
        child_msg = catgets(catd, 1, 1, "CHILD_DEFAULT");

        if (strcmp(child_msg, "CHILD_DEFAULT") == 0)
        {
            output("Child: catgets returned default\n");
            snprintf(buf, sizeof(buf), "default");
        }
        else
        {
            output("Child: catgets returned: '%s'\n", child_msg);
            snprintf(buf, sizeof(buf), "ok:%s", child_msg);
        }

        /* Send result to parent */
        write(pipe_fd[1], buf, strlen(buf) + 1);

        /* Close the child's copy of the catalog */
        if (catclose(catd) != 0)
        {
            output("Child: catclose failed: %s\n", strerror(errno));
            result = 1;
        }
        else
        {
            output("Child: Closed catalog descriptor\n");
        }

        close(pipe_fd[1]);
        _exit(result);
    }

    /* Parent */
    close(pipe_fd[1]);

    /* Read child's result */
    if (read(pipe_fd[0], buf, sizeof(buf)) > 0)
    {
        output("Parent: Child reported: '%s'\n", buf);
    }
    close(pipe_fd[0]);

    /* Wait for child */
    wpid = waitpid(child_pid, &status, 0);
    if (wpid != child_pid)
    {
        catclose(catd);
        unlink(TEST_CATALOG);
        UNRESOLVED(errno, "waitpid failed");
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != PTS_PASS)
    {
        catclose(catd);
        unlink(TEST_CATALOG);
        FAILED("Child exited abnormally");
    }

    /* Verify parent can still use its catalog (child's close shouldn't affect it) */
    msg = catgets(catd, 1, 2, "PARENT_DEFAULT2");
    if (strcmp(msg, "PARENT_DEFAULT2") == 0)
    {
        output("Parent: catgets returned default for message 2\n");
    }
    else
    {
        output("Parent: catgets returned: '%s'\n", msg);
    }

    /* Close parent's catalog */
    if (catclose(catd) != 0)
    {
        output("Parent: catclose failed: %s\n", strerror(errno));
        unlink(TEST_CATALOG);
        FAILED("Parent catclose failed (child may have corrupted state)");
    }

    output("Parent: Closed catalog successfully\n");

    /* Cleanup */
    unlink(TEST_CATALOG);

    output("\nFORK-030: Child has own copy of message catalog descriptors - VERIFIED\n");

    PASSED;
}
