/*
 * Copyright (c) 2004, Bull S.A..  All rights reserved.
 * Created by: Sebastien Decugis
 * Modified for Cygwin fork testing, 2026

 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * Test assertion FORK-400 [XSI]:
 * "All semadj values shall be cleared"
 *
 * The semadj value tracks adjustments to be made to semaphore values
 * when a process terminates. After fork, the child should have
 * cleared semadj values (not inherit parent's pending adjustments).
 *
 * Steps:
 * 1. Create a System V semaphore
 * 2. Perform operation with SEM_UNDO (creates semadj)
 * 3. Fork
 * 4. Child exits - should NOT undo parent's operation
 * 5. Parent verifies semaphore value unchanged
 */

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/wait.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "posixtest.h"
#include "testfrmw.h"
#include "testfrmw.c"

/* Union for semctl */
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

int main(void)
{
    int semid;
    key_t key;
    union semun arg;
    struct sembuf sop;
    pid_t child_pid, wpid;
    int status;
    int semval;

    output_init();

    /* Create a unique key */
    key = ftok("/tmp", 'F');
    if (key == (key_t)-1)
    {
        /* Use a fixed key if ftok fails */
        key = 0x464F524B;  /* "FORK" */
    }

    /* Create semaphore set with 1 semaphore */
    semid = semget(key, 1, IPC_CREAT | IPC_EXCL | 0600);
    if (semid < 0)
    {
        if (errno == ENOSYS)
        {
            /* SysV semaphores not available (Cygwin requires cygserver) */
            output("ERROR: SysV semaphores not available (ENOSYS)\n");
            output("\n");
            output("On Cygwin, SysV IPC requires the cygserver daemon.\n");
            output("To enable SysV IPC:\n");
            output("  1. Run as Administrator: cygserver-config\n");
            output("  2. Start service: cygrunsrv -S cygserver\n");
            output("  OR: Run 'cygserver &' in a terminal\n");
            output("\n");
            FAILED("cygserver not running - SysV semaphores unavailable");
        }
        if (errno == EEXIST)
        {
            /* Remove existing and retry */
            semid = semget(key, 1, 0600);
            if (semid >= 0)
                semctl(semid, 0, IPC_RMID);
            semid = semget(key, 1, IPC_CREAT | IPC_EXCL | 0600);
        }
        if (semid < 0)
        {
            if (errno == ENOSYS)
            {
                output("ERROR: SysV semaphores not available (ENOSYS)\n");
                output("See instructions above to enable cygserver.\n");
                FAILED("cygserver not running - SysV semaphores unavailable");
            }
            UNRESOLVED(errno, "semget failed");
        }
    }

    /* Initialize semaphore to 10 */
    arg.val = 10;
    if (semctl(semid, 0, SETVAL, arg) < 0)
    {
        semctl(semid, 0, IPC_RMID);
        UNRESOLVED(errno, "semctl SETVAL failed");
    }

    output("Semaphore created, initial value = 10\n");

    /* Decrement by 3 with SEM_UNDO - creates semadj of +3 */
    sop.sem_num = 0;
    sop.sem_op = -3;
    sop.sem_flg = SEM_UNDO;

    if (semop(semid, &sop, 1) < 0)
    {
        semctl(semid, 0, IPC_RMID);
        UNRESOLVED(errno, "semop failed");
    }

    /* Verify semaphore value is now 7 */
    semval = semctl(semid, 0, GETVAL);
    if (semval != 7)
    {
        output("Expected semval=7 after decrement, got %d\n", semval);
        semctl(semid, 0, IPC_RMID);
        UNRESOLVED(0, "Unexpected semaphore value");
    }

    output("Parent decremented semaphore by 3 with SEM_UNDO, value now = %d\n", semval);
    output("Parent has semadj = +3 (will add 3 on exit)\n");

    /* Fork */
    child_pid = fork();

    if (child_pid < 0)
    {
        semctl(semid, 0, IPC_RMID);
        UNRESOLVED(errno, "fork failed");
    }

    if (child_pid == 0)
    {
        /* Child process */

        /* Verify semaphore value is still 7 */
        int child_semval = semctl(semid, 0, GETVAL);
        output("Child: semaphore value = %d\n", child_semval);

        if (child_semval != 7)
        {
            output("Child: Expected 7, got %d\n", child_semval);
            _exit(1);
        }

        /* If child inherited semadj, when child exits, it would add 3
         * to the semaphore. We exit and parent will check. */
        output("Child: Exiting (semadj should be cleared, no undo)\n");
        _exit(PTS_PASS);
    }

    /* Parent waits for child to exit */
    wpid = waitpid(child_pid, &status, 0);
    if (wpid != child_pid)
    {
        semctl(semid, 0, IPC_RMID);
        UNRESOLVED(errno, "waitpid failed");
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != PTS_PASS)
    {
        semctl(semid, 0, IPC_RMID);
        FAILED("Child exited abnormally");
    }

    /* Check semaphore value - should still be 7 */
    /* If child had inherited semadj, it would be 10 now */
    semval = semctl(semid, 0, GETVAL);
    output("Parent: After child exit, semaphore value = %d\n", semval);

    if (semval == 10)
    {
        output("ERROR: semadj was inherited by child!\n");
        output("Child's exit triggered SEM_UNDO which should not have happened\n");
        semctl(semid, 0, IPC_RMID);
        FAILED("Child inherited parent's semadj value");
    }

    if (semval != 7)
    {
        output("ERROR: Unexpected semaphore value %d (expected 7)\n", semval);
        semctl(semid, 0, IPC_RMID);
        FAILED("Semaphore value corrupted");
    }

    output("SUCCESS: Child's semadj was cleared (fork did not inherit semadj)\n");

    /* Cleanup - remove semaphore */
    /* Note: When parent exits, semadj will restore value to 10 */
    semctl(semid, 0, IPC_RMID);

    PASSED;
}
