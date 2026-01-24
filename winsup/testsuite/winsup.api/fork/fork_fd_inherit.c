/*
 * POSIX fork() test - Assertion #5: File descriptor inheritance
 *
 * This test verifies that open file descriptors are inherited by the child
 * process after fork(), and that the child can read/write to them.
 *
 * POSIX.1-2017: "The child process shall have its own copy of the parent's
 * file descriptors. Each of the child's file descriptors shall refer to the
 * same open file description with the corresponding file descriptor of the
 * parent."
 */

#define _POSIX_C_SOURCE 200112L

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "testfrmw.h"
#include "testfrmw.c"

/* O_CLOEXEC may not be available on all platforms */
#ifndef O_CLOEXEC
#define O_CLOEXEC 0x80000  /* Cygwin value, will be skipped if not supported */
#define NO_O_CLOEXEC_SUPPORT 1
#endif

#define TEST_FILE "/tmp/fork_fd_test.tmp"
#define TEST_DATA "Hello from parent"
#define CHILD_DATA "Hello from child"

int main(void)
{
    int fd_read, fd_write;
    int pipefd[2];
    pid_t pid, child_pid;
    char buffer[256];
    int status;
    ssize_t n;

    output_init();
    output("Testing file descriptor inheritance across fork()\n");

    /* Test 1: Regular file descriptor inheritance */
    output("Test 1: Regular file FD inheritance\n");
    
    fd_write = open(TEST_FILE, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd_write < 0) {
        UNRESOLVED(errno, "Failed to create test file");
    }

    /* Write data before fork */
    if (write(fd_write, TEST_DATA, strlen(TEST_DATA)) != (ssize_t)strlen(TEST_DATA)) {
        close(fd_write);
        unlink(TEST_FILE);
        UNRESOLVED(errno, "Failed to write test data");
    }

    /* Reset file position */
    if (lseek(fd_write, 0, SEEK_SET) < 0) {
        close(fd_write);
        unlink(TEST_FILE);
        UNRESOLVED(errno, "Failed to seek");
    }

    pid = fork();
    if (pid < 0) {
        close(fd_write);
        unlink(TEST_FILE);
        UNRESOLVED(errno, "fork() failed");
    }

    if (pid == 0) {
        /* Child process - read from inherited FD */
        memset(buffer, 0, sizeof(buffer));
        n = read(fd_write, buffer, sizeof(buffer) - 1);
        if (n < 0) {
            output("Child: read failed: %s\n", strerror(errno));
            _exit(1);
        }
        if (strcmp(buffer, TEST_DATA) != 0) {
            output("Child: data mismatch: got '%s', expected '%s'\n", buffer, TEST_DATA);
            _exit(2);
        }
        output("Child: successfully read '%s' from inherited FD\n", buffer);
        _exit(0);
    }

    /* Parent waits for child */
    child_pid = waitpid(pid, &status, 0);
    close(fd_write);
    unlink(TEST_FILE);

    if (child_pid != pid) {
        UNRESOLVED(errno, "waitpid returned wrong PID");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        FAILED("Child failed to read from inherited file descriptor");
    }
    output("Test 1 PASSED: File FD inherited correctly\n");

    /* Test 2: Pipe file descriptor inheritance */
    output("\nTest 2: Pipe FD inheritance\n");
    
    if (pipe(pipefd) < 0) {
        UNRESOLVED(errno, "pipe() failed");
    }

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        UNRESOLVED(errno, "fork() failed");
    }

    if (pid == 0) {
        /* Child - close read end, write to pipe */
        close(pipefd[0]);
        if (write(pipefd[1], CHILD_DATA, strlen(CHILD_DATA)) != (ssize_t)strlen(CHILD_DATA)) {
            output("Child: write to pipe failed: %s\n", strerror(errno));
            _exit(1);
        }
        close(pipefd[1]);
        output("Child: wrote '%s' to inherited pipe\n", CHILD_DATA);
        _exit(0);
    }

    /* Parent - close write end, read from pipe */
    close(pipefd[1]);
    memset(buffer, 0, sizeof(buffer));
    n = read(pipefd[0], buffer, sizeof(buffer) - 1);
    close(pipefd[0]);

    child_pid = waitpid(pid, &status, 0);
    
    if (child_pid != pid) {
        UNRESOLVED(errno, "waitpid returned wrong PID");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        FAILED("Child failed to write to inherited pipe");
    }
    if (n < 0) {
        FAILED("Parent failed to read from pipe");
    }
    if (strcmp(buffer, CHILD_DATA) != 0) {
        output("Parent: data mismatch: got '%s', expected '%s'\n", buffer, CHILD_DATA);
        FAILED("Pipe data mismatch");
    }
    output("Test 2 PASSED: Pipe FDs inherited correctly\n");
    output("Parent: received '%s' from child via inherited pipe\n", buffer);

    /* Test 3: FD_CLOEXEC should NOT affect fork (only exec) */
    output("\nTest 3: FD with O_CLOEXEC still inherited by fork\n");
    
    fd_read = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd_read < 0) {
        /* O_CLOEXEC might not be supported, skip this test */
        output("O_CLOEXEC not supported, skipping test 3\n");
    } else {
        pid = fork();
        if (pid < 0) {
            close(fd_read);
            UNRESOLVED(errno, "fork() failed");
        }
        
        if (pid == 0) {
            /* Child - FD should still be accessible (CLOEXEC only affects exec) */
            if (fcntl(fd_read, F_GETFD) < 0) {
                output("Child: FD with CLOEXEC not inherited\n");
                _exit(1);
            }
            output("Child: FD with CLOEXEC correctly inherited (will close on exec)\n");
            _exit(0);
        }
        
        close(fd_read);
        child_pid = waitpid(pid, &status, 0);
        
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            FAILED("FD with O_CLOEXEC not inherited by fork");
        }
        output("Test 3 PASSED: O_CLOEXEC FD inherited by fork\n");
    }

    output("\n=== All FD inheritance tests PASSED ===\n");
    PASSED
    return 0;
}
