/*
 * POSIX fork() test - Credential Inheritance
 *
 * Verifies that the child inherits real/effective UID, GID,
 * and supplementary groups from the parent.
 *
 * POSIX.1-2017: "The child process shall inherit the real user ID,
 * real group ID, effective user ID, effective group ID."
 */

#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <grp.h>
#include <sys/wait.h>

#include "testfrmw.h"
#include "testfrmw.c"

struct cred_info {
    uid_t ruid, euid;
    gid_t rgid, egid;
    int ngroups;
    gid_t groups[64];
};

int main(void)
{
    struct cred_info parent_cred, child_cred;
    pid_t pid;
    int pipefd[2];
    int status;

    output_init();
    output("Testing credential inheritance across fork()\n");

    alarm(30);

    /* Gather parent credentials */
    parent_cred.ruid = getuid();
    parent_cred.euid = geteuid();
    parent_cred.rgid = getgid();
    parent_cred.egid = getegid();
    parent_cred.ngroups = getgroups(64, parent_cred.groups);
    if (parent_cred.ngroups < 0)
        parent_cred.ngroups = 0;

    output("Parent: ruid=%d euid=%d rgid=%d egid=%d ngroups=%d\n",
           parent_cred.ruid, parent_cred.euid,
           parent_cred.rgid, parent_cred.egid,
           parent_cred.ngroups);

    if (pipe(pipefd) < 0)
        UNRESOLVED(errno, "pipe() failed");

    pid = fork();
    if (pid < 0)
        UNRESOLVED(errno, "fork() failed");

    if (pid == 0) {
        close(pipefd[0]);
        child_cred.ruid = getuid();
        child_cred.euid = geteuid();
        child_cred.rgid = getgid();
        child_cred.egid = getegid();
        child_cred.ngroups = getgroups(64, child_cred.groups);
        if (child_cred.ngroups < 0)
            child_cred.ngroups = 0;
        if (write(pipefd[1], &child_cred, sizeof(child_cred)) != sizeof(child_cred))
            _exit(2);
        close(pipefd[1]);
        _exit(0);
    }

    close(pipefd[1]);
    if (read(pipefd[0], &child_cred, sizeof(child_cred)) != sizeof(child_cred)) {
        close(pipefd[0]);
        UNRESOLVED(errno, "Failed to read child credentials");
    }
    close(pipefd[0]);

    if (waitpid(pid, &status, 0) != pid)
        UNRESOLVED(errno, "waitpid failed");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        FAILED("Child exited abnormally");

    output("Child:  ruid=%d euid=%d rgid=%d egid=%d ngroups=%d\n",
           child_cred.ruid, child_cred.euid,
           child_cred.rgid, child_cred.egid,
           child_cred.ngroups);

    /* Verify all credentials match */
    if (child_cred.ruid != parent_cred.ruid)
        FAILED("Real UID mismatch");
    if (child_cred.euid != parent_cred.euid)
        FAILED("Effective UID mismatch");
    if (child_cred.rgid != parent_cred.rgid)
        FAILED("Real GID mismatch");
    if (child_cred.egid != parent_cred.egid)
        FAILED("Effective GID mismatch");

    if (child_cred.ngroups != parent_cred.ngroups)
        FAILED("Supplementary group count mismatch");
    for (int i = 0; i < parent_cred.ngroups; i++) {
        if (child_cred.groups[i] != parent_cred.groups[i]) {
            output("Group[%d]: parent=%d child=%d\n",
                   i, parent_cred.groups[i], child_cred.groups[i]);
            FAILED("Supplementary group mismatch");
        }
    }

    output("\n=== All credential inheritance tests PASSED ===\n");
    PASSED;
}
