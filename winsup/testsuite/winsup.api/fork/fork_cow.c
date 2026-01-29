/*
 * POSIX fork() test - Copy-on-Write / Memory Isolation
 *
 * Verifies that parent and child have independent address spaces
 * after fork().  The child modifies variables in all memory regions
 * (global, stack, heap, mmap'd) and the parent verifies its copies
 * are unchanged.
 *
 * POSIX.1-2017: "The child process shall be created with a single
 * thread.  If the process has more than one thread, ...the entire
 * address space shall be reproduced in the child process."
 *
 * "Physical copy of the parent's address space or copy-on-write
 * (COW) optimization is implementation-defined."
 */

#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "testfrmw.h"
#include "testfrmw.c"

static int global_var = 100;

int main(void)
{
    int stack_var = 200;
    int *heap_var;
    int *mmap_var;
    pid_t pid;
    int status;

    output_init();
    output("Testing Copy-on-Write / memory isolation across fork()\n");

    alarm(30);

    /* Allocate heap memory */
    heap_var = malloc(sizeof(int));
    if (!heap_var)
        UNRESOLVED(errno, "malloc failed");
    *heap_var = 300;

    /* Allocate anonymous mmap region */
    mmap_var = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mmap_var == MAP_FAILED)
        UNRESOLVED(errno, "mmap failed");
    *mmap_var = 400;

    pid = fork();
    if (pid < 0)
        UNRESOLVED(errno, "fork() failed");

    if (pid == 0) {
        /* Child: modify all variables */
        global_var = 999;
        stack_var = 999;
        *heap_var = 999;
        *mmap_var = 999;
        /* Verify child sees its own changes */
        if (global_var != 999 || stack_var != 999 ||
            *heap_var != 999 || *mmap_var != 999) {
            _exit(1);
        }
        _exit(0);
    }

    /* Parent: wait for child to finish modifying */
    if (waitpid(pid, &status, 0) != pid)
        UNRESOLVED(errno, "waitpid failed");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        FAILED("Child failed during COW test");

    /* Verify parent's memory is unchanged */
    if (global_var != 100) {
        output("global_var: expected 100, got %d\n", global_var);
        FAILED("Global variable modified by child (no COW isolation)");
    }
    output("PASS: global variable isolated (parent=%d)\n", global_var);

    if (stack_var != 200) {
        output("stack_var: expected 200, got %d\n", stack_var);
        FAILED("Stack variable modified by child (no COW isolation)");
    }
    output("PASS: stack variable isolated (parent=%d)\n", stack_var);

    if (*heap_var != 300) {
        output("heap_var: expected 300, got %d\n", *heap_var);
        FAILED("Heap variable modified by child (no COW isolation)");
    }
    output("PASS: heap variable isolated (parent=%d)\n", *heap_var);

    if (*mmap_var != 400) {
        output("mmap_var: expected 400, got %d\n", *mmap_var);
        FAILED("mmap variable modified by child (no COW isolation)");
    }
    output("PASS: mmap(MAP_PRIVATE) variable isolated (parent=%d)\n", *mmap_var);

    free(heap_var);
    munmap(mmap_var, 4096);

    output("\n=== All COW/memory isolation tests PASSED ===\n");
    PASSED;
}
