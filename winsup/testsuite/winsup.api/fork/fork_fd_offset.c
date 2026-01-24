/*
 * Test assertion FORK-012:
 * File descriptor offsets are shared between parent and child
 * (implied by same open file description)
 *
 * "Each of the child's file descriptors shall refer to the same open
 * file description with the corresponding file descriptor of the parent"
 *
 * This means that file offsets are shared - if child seeks, parent sees it.
 *
 * Steps:
 * 1. Create a file and write data to it
 * 2. Open the file and seek to a known position
 * 3. Fork
 * 4. Child seeks to a new position and exits
 * 5. Parent verifies the offset changed
 */

#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "posixtest.h"
#include "testfrmw.h"
#include "testfrmw.c"

#define TEST_FILE "/tmp/fork_fd_offset_test.tmp"
#define FILE_SIZE 1000
#define INITIAL_OFFSET 100
#define CHILD_OFFSET 500

int main(void)
{
    int fd;
    pid_t child_pid, wpid;
    int status;
    off_t offset;
    char buf[FILE_SIZE];

    output_init();

    /* Create test file with known content */
    fd = open(TEST_FILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0)
    {
        UNRESOLVED(errno, "Failed to create test file");
    }

    /* Fill with data */
    memset(buf, 'A', FILE_SIZE);
    if (write(fd, buf, FILE_SIZE) != FILE_SIZE)
    {
        close(fd);
        unlink(TEST_FILE);
        UNRESOLVED(errno, "Failed to write test data");
    }

    /* Seek to initial position */
    offset = lseek(fd, INITIAL_OFFSET, SEEK_SET);
    if (offset != INITIAL_OFFSET)
    {
        close(fd);
        unlink(TEST_FILE);
        UNRESOLVED(errno, "Failed to seek to initial position");
    }

    output("Parent: Initial offset = %ld\n", (long)offset);

    /* Fork */
    child_pid = fork();

    if (child_pid < 0)
    {
        close(fd);
        unlink(TEST_FILE);
        UNRESOLVED(errno, "fork() failed");
    }

    if (child_pid == 0)
    {
        /* Child process */
        off_t child_offset;

        /* Verify we inherited the offset */
        child_offset = lseek(fd, 0, SEEK_CUR);
        if (child_offset != INITIAL_OFFSET)
        {
            output("Child: Expected offset %d, got %ld\n",
                   INITIAL_OFFSET, (long)child_offset);
            _exit(1);
        }

        output("Child: Inherited offset = %ld (correct)\n", (long)child_offset);

        /* Seek to new position */
        child_offset = lseek(fd, CHILD_OFFSET, SEEK_SET);
        if (child_offset != CHILD_OFFSET)
        {
            output("Child: Failed to seek to %d\n", CHILD_OFFSET);
            _exit(2);
        }

        output("Child: Seeked to offset = %ld\n", (long)child_offset);

        /* Exit without closing fd - let parent check offset */
        _exit(PTS_PASS);
    }

    /* Parent process - wait for child to complete */
    wpid = waitpid(child_pid, &status, 0);
    if (wpid != child_pid)
    {
        close(fd);
        unlink(TEST_FILE);
        UNRESOLVED(errno, "waitpid failed");
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != PTS_PASS)
    {
        close(fd);
        unlink(TEST_FILE);
        FAILED("Child exited abnormally");
    }

    /* Check if the offset changed (shared file description) */
    offset = lseek(fd, 0, SEEK_CUR);
    output("Parent: Offset after child seek = %ld\n", (long)offset);

    if (offset != CHILD_OFFSET)
    {
        output("Parent: Expected offset %d after child seek, got %ld\n",
               CHILD_OFFSET, (long)offset);
        close(fd);
        unlink(TEST_FILE);
        FAILED("File offset not shared between parent and child");
    }

    output("Parent: Offset correctly reflects child's seek (shared file description)\n");

    /* Test write affecting shared offset */
    /* Seek back to a known position */
    lseek(fd, 0, SEEK_SET);

    child_pid = fork();
    if (child_pid < 0)
    {
        close(fd);
        unlink(TEST_FILE);
        UNRESOLVED(errno, "Second fork() failed");
    }

    if (child_pid == 0)
    {
        /* Child writes some bytes, advancing offset */
        char write_buf[] = "HELLO";
        ssize_t n = write(fd, write_buf, 5);
        if (n != 5)
        {
            _exit(3);
        }
        output("Child: Wrote 5 bytes\n");
        _exit(PTS_PASS);
    }

    wpid = waitpid(child_pid, &status, 0);
    if (wpid != child_pid || !WIFEXITED(status) || WEXITSTATUS(status) != PTS_PASS)
    {
        close(fd);
        unlink(TEST_FILE);
        FAILED("Second child failed");
    }

    offset = lseek(fd, 0, SEEK_CUR);
    if (offset != 5)
    {
        output("Parent: Expected offset 5 after child write, got %ld\n", (long)offset);
        close(fd);
        unlink(TEST_FILE);
        FAILED("Write offset not shared");
    }

    output("Parent: Offset correctly advanced by child's write\n");

    /* Cleanup */
    close(fd);
    unlink(TEST_FILE);

    PASSED;
}
