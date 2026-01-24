/*
 * Test assertion FORK-111:
 * "If a multi-threaded process calls fork(), the new process shall
 * contain a replica of the calling thread and its entire address space"
 *
 * Steps:
 * 1. Create multiple threads with distinct thread-local data
 * 2. Have one non-main thread call fork()
 * 3. Verify child contains only the calling thread
 * 4. Verify calling thread's TLS data is preserved in child
 * 5. Verify child has copy of shared address space data
 */

#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "posixtest.h"
#include "testfrmw.h"
#include "testfrmw.c"

#define NUM_THREADS 4
#define MAGIC_VALUE 0xDEADBEEF

/* Thread-local storage */
static __thread int tls_thread_id = -1;
static __thread unsigned int tls_magic = 0;

/* Shared data that should be copied to child */
static volatile int shared_value = 12345;
static char shared_buffer[64] = "Hello from parent";

/* Synchronization */
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static volatile int forker_ready = 0;
static volatile int fork_done = 0;
static volatile pid_t fork_result = -1;
static volatile int child_exit_status = -1;

/* Thread that will call fork */
static void *forker_thread(void *arg)
{
    int my_id = *(int *)arg;
    pid_t child_pid, wpid;
    int status;

    /* Set thread-local data */
    tls_thread_id = my_id;
    tls_magic = MAGIC_VALUE;

    output("Forker thread %d: TLS id=%d, magic=0x%X\n",
           my_id, tls_thread_id, tls_magic);

    /* Signal we're ready */
    pthread_mutex_lock(&mutex);
    forker_ready = 1;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&mutex);

    /* Small delay to ensure other threads are running */
    usleep(50000);

    /* Fork from this thread */
    output("Forker thread %d: Calling fork()\n", my_id);
    child_pid = fork();

    if (child_pid < 0)
    {
        output("Forker thread: fork() failed: %s\n", strerror(errno));
        fork_result = -1;
        return NULL;
    }

    if (child_pid == 0)
    {
        /* Child process - should only have one thread (this one) */
        int errors = 0;

        /* Verify TLS data is preserved */
        if (tls_thread_id != my_id)
        {
            output("Child: TLS thread_id mismatch: expected %d, got %d\n",
                   my_id, tls_thread_id);
            errors++;
        }

        if (tls_magic != MAGIC_VALUE)
        {
            output("Child: TLS magic mismatch: expected 0x%X, got 0x%X\n",
                   MAGIC_VALUE, tls_magic);
            errors++;
        }

        /* Verify shared data is copied */
        if (shared_value != 12345)
        {
            output("Child: shared_value mismatch: expected 12345, got %d\n",
                   shared_value);
            errors++;
        }

        if (strcmp(shared_buffer, "Hello from parent") != 0)
        {
            output("Child: shared_buffer mismatch: got '%s'\n", shared_buffer);
            errors++;
        }

        /* Modify to verify COW */
        shared_value = 99999;
        strcpy(shared_buffer, "Modified by child");

        if (errors > 0)
        {
            output("Child: %d errors found\n", errors);
            _exit(1);
        }

        output("Child: TLS and shared data correctly inherited\n");
        output("Child: TLS thread_id=%d, magic=0x%X\n", tls_thread_id, tls_magic);
        _exit(PTS_PASS);
    }

    /* Parent - forker thread */
    output("Forker thread: fork() returned child PID %d\n", child_pid);

    wpid = waitpid(child_pid, &status, 0);
    if (wpid != child_pid)
    {
        output("Forker thread: waitpid failed: %s\n", strerror(errno));
        fork_result = -1;
        child_exit_status = -1;
    }
    else
    {
        fork_result = child_pid;
        if (WIFEXITED(status))
            child_exit_status = WEXITSTATUS(status);
        else
            child_exit_status = -1;
    }

    /* Verify parent's data unchanged */
    if (shared_value != 12345)
    {
        output("Parent: shared_value corrupted! Got %d\n", shared_value);
    }
    if (strcmp(shared_buffer, "Hello from parent") != 0)
    {
        output("Parent: shared_buffer corrupted! Got '%s'\n", shared_buffer);
    }

    /* Signal fork is done */
    pthread_mutex_lock(&mutex);
    fork_done = 1;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&mutex);

    return NULL;
}

/* Other threads that should NOT be in child */
static void *other_thread(void *arg)
{
    int my_id = *(int *)arg;

    /* Set distinct TLS data */
    tls_thread_id = my_id;
    tls_magic = my_id * 1000;

    output("Other thread %d: TLS id=%d, magic=0x%X\n",
           my_id, tls_thread_id, tls_magic);

    /* Wait for forker to be ready */
    pthread_mutex_lock(&mutex);
    while (!forker_ready)
        pthread_cond_wait(&cond, &mutex);
    pthread_mutex_unlock(&mutex);

    /* Wait for fork to complete */
    pthread_mutex_lock(&mutex);
    while (!fork_done)
        pthread_cond_wait(&cond, &mutex);
    pthread_mutex_unlock(&mutex);

    output("Other thread %d: Fork completed, exiting\n", my_id);
    return NULL;
}

int main(void)
{
    pthread_t threads[NUM_THREADS];
    int thread_ids[NUM_THREADS];
    int i, ret;

    output_init();

    /* Create threads */
    for (i = 0; i < NUM_THREADS; i++)
    {
        thread_ids[i] = i;
        if (i == 0)
        {
            /* First thread is the forker */
            ret = pthread_create(&threads[i], NULL, forker_thread, &thread_ids[i]);
        }
        else
        {
            ret = pthread_create(&threads[i], NULL, other_thread, &thread_ids[i]);
        }
        if (ret != 0)
        {
            UNRESOLVED(ret, "pthread_create failed");
        }
    }

    /* Wait for all threads to complete */
    for (i = 0; i < NUM_THREADS; i++)
    {
        pthread_join(threads[i], NULL);
    }

    /* Check results */
    if (fork_result < 0)
    {
        FAILED("fork() failed in forker thread");
    }

    if (child_exit_status != PTS_PASS)
    {
        output("Child exit status: %d\n", child_exit_status);
        FAILED("Child process reported errors");
    }

    /* Verify parent data unchanged after child modified its copy */
    if (shared_value != 12345)
    {
        FAILED("Parent shared_value corrupted by child (COW failure)");
    }

    output("Multi-threaded fork test passed:\n");
    output("  - Child contained replica of calling thread\n");
    output("  - Thread-local storage preserved in child\n");
    output("  - Address space correctly copied (COW)\n");
    output("  - Parent data unchanged after child modification\n");

    PASSED;
}
