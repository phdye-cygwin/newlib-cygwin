/*
 * Copyright (c) 2026 Cygwin Authors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/*
 * NAME
 *	pipe12.c
 *
 * DESCRIPTION
 *	Test that reading from a pipe opened via /proc/<pid>/fd/N returns
 *	EOF after the original process exits.
 *
 *	This is a regression test for a Cygwin bug where such reads would
 *	hang forever instead of returning EOF.
 *
 * ALGORITHM
 *	1. Create a pipe
 *	2. Fork a "holder" child that uses the pipe read end as stdin
 *	3. Parent opens holder's stdin via /proc/<pid>/fd/0
 *	4. Parent closes the write end of the pipe
 *	5. Holder child exits
 *	6. Fork a "reader" child to do the risky read (may hang)
 *	7. Parent waits for reader with timeout, kills if hung
 *	8. Reader should get EOF; hang indicates bug
 *
 * EXPECTED RESULT
 *	read() should return 0 (EOF) within a reasonable time after the
 *	holder child exits.
 *
 * HISTORY
 *	2026-01-28 - Created as regression test for pipe read hang bug
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include "test.h"
#include "usctest.h"

const char *TCID = "pipe12";
int TST_TOTAL = 1;
extern int Tst_count;

/* Exit codes from reader child */
#define EXIT_GOT_EOF    0
#define EXIT_GOT_ERROR  1
#define EXIT_GOT_DATA   2
#define EXIT_SETUP_FAIL 3

void setup(void);
void cleanup(void) __attribute__((noreturn));

int
main(int ac, char **av)
{
	int lc;
	const char *msg;
	int pipefd[2];
	pid_t holder, reader;
	char proc_path[64];
	int fd;
	int status;
	int i;

	if ((msg = parse_opts(ac, av, (option_t *)NULL, NULL)) != (char *)NULL) {
		tst_brkm(TBROK, tst_exit, "OPTION PARSING ERROR - %s", msg);
	}

	setup();

	for (lc = 0; TEST_LOOPING(lc); lc++) {
		Tst_count = 0;

		TEST(pipe(pipefd));
		if (TEST_RETURN != 0) {
			tst_resm(TFAIL, "pipe() failed: %s", strerror(errno));
			continue;
		}

		/*
		 * Fork "holder" child - holds the pipe as stdin then exits
		 */
		holder = fork();
		if (holder == -1) {
			close(pipefd[0]);
			close(pipefd[1]);
			tst_brkm(TBROK, cleanup, "fork(holder) failed: %s",
				 strerror(errno));
		}

		if (holder == 0) {
			/* Holder child: use pipe read end as stdin */
			close(pipefd[1]);
			if (dup2(pipefd[0], STDIN_FILENO) == -1) {
				_exit(EXIT_SETUP_FAIL);
			}
			close(pipefd[0]);

			/* Give parent time to open /proc/.../fd/0 */
			usleep(500000);
			_exit(0);
		}

		/* Parent */
		close(pipefd[0]); /* Don't need read end directly */

		/* Wait for holder to set up stdin */
		usleep(200000);

		/* Open holder's stdin via /proc */
		snprintf(proc_path, sizeof(proc_path), "/proc/%d/fd/0", holder);
		fd = open(proc_path, O_RDONLY);
		if (fd == -1) {
			close(pipefd[1]);
			waitpid(holder, NULL, 0);
			tst_resm(TFAIL, "open(%s) failed: %s",
				 proc_path, strerror(errno));
			continue;
		}

		/* Close write end - no more writers */
		close(pipefd[1]);

		/* Wait for holder to exit */
		waitpid(holder, NULL, 0);

		/*
		 * Fork "reader" child to do the risky read.
		 * This isolates the potentially hanging read() so we can
		 * kill it if needed and still have the test exit cleanly.
		 */
		reader = fork();
		if (reader == -1) {
			close(fd);
			tst_brkm(TBROK, cleanup, "fork(reader) failed: %s",
				 strerror(errno));
		}

		if (reader == 0) {
			/* Reader child: attempt the read that may hang */
			char buf[64];
			ssize_t n;

			n = read(fd, buf, sizeof(buf));
			close(fd);

			if (n == 0) {
				_exit(EXIT_GOT_EOF);
			} else if (n == -1) {
				_exit(EXIT_GOT_ERROR);
			} else {
				_exit(EXIT_GOT_DATA);
			}
		}

		/* Parent: wait for reader with timeout */
		close(fd); /* Parent doesn't need it */

		/*
		 * Poll for reader completion with 5 second timeout.
		 * We use polling instead of alarm() because the reader
		 * is in a separate process we can kill.
		 */
		for (i = 0; i < 50; i++) {  /* 50 * 100ms = 5 seconds */
			pid_t ret = waitpid(reader, &status, WNOHANG);
			if (ret == reader) {
				/* Reader finished */
				if (WIFEXITED(status)) {
					switch (WEXITSTATUS(status)) {
					case EXIT_GOT_EOF:
						tst_resm(TPASS, "read() returned "
							 "EOF as expected");
						break;
					case EXIT_GOT_ERROR:
						tst_resm(TPASS, "read() returned "
							 "error - acceptable");
						break;
					case EXIT_GOT_DATA:
						tst_resm(TFAIL, "read() returned "
							 "data - unexpected");
						break;
					default:
						tst_resm(TBROK, "reader exited "
							 "with code %d",
							 WEXITSTATUS(status));
					}
				} else if (WIFSIGNALED(status)) {
					tst_resm(TBROK, "reader killed by "
						 "signal %d", WTERMSIG(status));
				}
				goto next_iteration;
			} else if (ret == -1 && errno != EINTR) {
				tst_resm(TBROK, "waitpid failed: %s",
					 strerror(errno));
				goto next_iteration;
			}
			usleep(100000); /* 100ms */
		}

		/*
		 * Timeout - reader is hung. Kill it and report failure.
		 */
		kill(reader, SIGKILL);
		waitpid(reader, NULL, 0);
		tst_resm(TFAIL, "read() blocked for 5 seconds - "
			 "pipe read hang bug triggered");

next_iteration:
		;
	}

	cleanup();
}

void
setup(void)
{
	tst_sig(FORK, DEF_HANDLER, cleanup);
	TEST_PAUSE;
}

void
cleanup(void)
{
	TEST_CLEANUP;
	tst_exit();
}
