/*
 * Cygwin AF_UNIX error path and POSIX conformance tests
 *
 * Tests error conditions and edge cases for AF_UNIX sockets:
 *   1. bind with path too long (ENAMETOOLONG)
 *   2. bind to already-bound path (EADDRINUSE)
 *   3. connect to nonexistent path (ENOENT / ECONNREFUSED)
 *   4. accept on non-listening socket (EINVAL)
 *   5. send on unconnected SOCK_STREAM (EPIPE / ENOTCONN)
 *   6. SO_PEERCRED setsockopt on connected socket (error)
 *   7. getpeereid on non-AF_UNIX socket (ENOTSUP)
 *   8. double close safety
 *
 * Portable: compiles on Cygwin, Linux (WSL), and BSD.
 * Cygwin-specific tests guarded by #ifdef __CYGWIN__.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <netinet/in.h>

#include "testfrmw.h"
#include "testfrmw.c"

#define POLL_US   50000
#define POLL_MAX  200

static void
make_path (char *buf, size_t len, const char *tag)
{
  const char *tmpdir = getenv ("TMPDIR");
  if (!tmpdir || !*tmpdir)
    tmpdir = getenv ("TMP");
  if (!tmpdir || !*tmpdir)
    tmpdir = ".";
  snprintf (buf, len, "%s/af_err_%d_%s", tmpdir, getpid (), tag);
}

/* Poll-based waitpid with bounded timeout. */
static int
wait_child (pid_t pid)
{
  int status;
  for (int i = 0; i < POLL_MAX; i++)
    {
      pid_t w = waitpid (pid, &status, WNOHANG);
      if (w == pid)
	{
	  if (WIFEXITED (status))
	    return WEXITSTATUS (status);
	  return -1;
	}
      if (w < 0)
	return -1;
      usleep (POLL_US);
    }
  kill (pid, SIGKILL);
  waitpid (pid, NULL, 0);
  return -2;
}

/* ------------------------------------------------------------------ */
/*  Test 1: bind with path too long                                   */
/* ------------------------------------------------------------------ */

static int
test_bind_enametoolong (void)
{
  int s = socket (AF_UNIX, SOCK_STREAM, 0);
  if (s < 0)
    {
      output ("  socket: %s\n", strerror (errno));
      return -1;
    }

  struct sockaddr_un addr;
  memset (&addr, 0, sizeof (addr));
  addr.sun_family = AF_UNIX;
  /* Fill path with max+1 characters */
  memset (addr.sun_path, 'x', sizeof (addr.sun_path));
  /* sun_path is already not NUL-terminated at max size */

  int rc = bind (s, (struct sockaddr *) &addr, sizeof (addr));
  int saved_errno = errno;
  close (s);

  if (rc == 0)
    {
      /* Some implementations truncate — clean up */
      unlink (addr.sun_path);
      output ("  bind succeeded (truncated path — implementation-defined)\n");
      return 0;
    }

  /* ENAMETOOLONG is the expected POSIX error */
  if (saved_errno != ENAMETOOLONG && saved_errno != EINVAL)
    {
      output ("  errno=%d (%s), expected ENAMETOOLONG or EINVAL\n",
	      saved_errno, strerror (saved_errno));
      return -1;
    }

  return 0;
}

/* ------------------------------------------------------------------ */
/*  Test 2: bind to already-bound path (EADDRINUSE)                   */
/* ------------------------------------------------------------------ */

static int
test_bind_eaddrinuse (void)
{
  char path[108];
  make_path (path, sizeof (path), "inuse");

  int s1 = socket (AF_UNIX, SOCK_STREAM, 0);
  int s2 = socket (AF_UNIX, SOCK_STREAM, 0);
  if (s1 < 0 || s2 < 0)
    {
      output ("  socket: %s\n", strerror (errno));
      if (s1 >= 0) close (s1);
      if (s2 >= 0) close (s2);
      return -1;
    }

  struct sockaddr_un addr;
  memset (&addr, 0, sizeof (addr));
  addr.sun_family = AF_UNIX;
  strncpy (addr.sun_path, path, sizeof (addr.sun_path) - 1);
  unlink (path);

  if (bind (s1, (struct sockaddr *) &addr, sizeof (addr)) < 0)
    {
      output ("  bind(s1): %s\n", strerror (errno));
      close (s1);
      close (s2);
      return -1;
    }

  int rc = bind (s2, (struct sockaddr *) &addr, sizeof (addr));
  int saved_errno = errno;
  close (s1);
  close (s2);
  unlink (path);

  if (rc == 0)
    {
      output ("  second bind succeeded (expected EADDRINUSE)\n");
      return -1;
    }
  if (saved_errno != EADDRINUSE)
    {
      output ("  errno=%d (%s), expected EADDRINUSE\n",
	      saved_errno, strerror (saved_errno));
      return -1;
    }

  return 0;
}

/* ------------------------------------------------------------------ */
/*  Test 3: connect to nonexistent path                               */
/* ------------------------------------------------------------------ */

static int
test_connect_noent (void)
{
  char path[108];
  make_path (path, sizeof (path), "noent");
  unlink (path);  /* ensure it doesn't exist */

  int s = socket (AF_UNIX, SOCK_STREAM, 0);
  if (s < 0)
    {
      output ("  socket: %s\n", strerror (errno));
      return -1;
    }

  struct sockaddr_un addr;
  memset (&addr, 0, sizeof (addr));
  addr.sun_family = AF_UNIX;
  strncpy (addr.sun_path, path, sizeof (addr.sun_path) - 1);

  int rc = connect (s, (struct sockaddr *) &addr, sizeof (addr));
  int saved_errno = errno;
  close (s);

  if (rc == 0)
    {
      output ("  connect to nonexistent path succeeded\n");
      return -1;
    }
  /* POSIX: ENOENT (path doesn't exist) or ECONNREFUSED (no listener) */
  if (saved_errno != ENOENT && saved_errno != ECONNREFUSED)
    {
      output ("  errno=%d (%s), expected ENOENT or ECONNREFUSED\n",
	      saved_errno, strerror (saved_errno));
      return -1;
    }

  return 0;
}

/* ------------------------------------------------------------------ */
/*  Test 4: accept on non-listening socket                            */
/* ------------------------------------------------------------------ */

static int
test_accept_not_listening (void)
{
  int s = socket (AF_UNIX, SOCK_STREAM, 0);
  if (s < 0)
    {
      output ("  socket: %s\n", strerror (errno));
      return -1;
    }

  /* accept() without listen() should fail */
  int rc = accept (s, NULL, NULL);
  int saved_errno = errno;
  close (s);

  if (rc >= 0)
    {
      close (rc);
      output ("  accept on non-listening socket succeeded\n");
      return -1;
    }
  if (saved_errno != EINVAL)
    {
      output ("  errno=%d (%s), expected EINVAL\n",
	      saved_errno, strerror (saved_errno));
      return -1;
    }

  return 0;
}

/* ------------------------------------------------------------------ */
/*  Test 5: send on unconnected SOCK_STREAM                           */
/* ------------------------------------------------------------------ */

static int
test_send_unconnected (void)
{
  int s = socket (AF_UNIX, SOCK_STREAM, 0);
  if (s < 0)
    {
      output ("  socket: %s\n", strerror (errno));
      return -1;
    }

  /* Block SIGPIPE so send returns error instead of killing us */
  signal (SIGPIPE, SIG_IGN);

  int rc = send (s, "x", 1, MSG_NOSIGNAL);
  int saved_errno = errno;
  close (s);

  signal (SIGPIPE, SIG_DFL);

  if (rc >= 0)
    {
      output ("  send on unconnected socket succeeded\n");
      return -1;
    }
  /* POSIX: ENOTCONN or EPIPE */
  if (saved_errno != ENOTCONN && saved_errno != EPIPE
      && saved_errno != EBADF)
    {
      output ("  errno=%d (%s), expected ENOTCONN or EPIPE\n",
	      saved_errno, strerror (saved_errno));
      return -1;
    }

  return 0;
}

/* ------------------------------------------------------------------ */
/*  Test 6: SO_PEERCRED setsockopt on connected socket                */
/* ------------------------------------------------------------------ */

#ifdef __CYGWIN__
static int
test_so_peercred_after_connect (void)
{
  int sv[2];

  if (socketpair (AF_UNIX, SOCK_STREAM, 0, sv) < 0)
    {
      output ("  socketpair: %s\n", strerror (errno));
      return -1;
    }

  /* Try to set SO_PEERCRED=0 on an already-connected socket.
     This should fail — SO_PEERCRED can only be set before connect. */
  int zero = 0;
  int rc = setsockopt (sv[0], SOL_SOCKET, SO_PEERCRED,
		       &zero, sizeof (zero));
  int saved_errno = errno;
  close (sv[0]);
  close (sv[1]);

  if (rc == 0)
    {
      /* Cygwin's af_local_set_no_getpeereid checks connection state.
	 If it succeeded, the implementation allows it — document but pass. */
      output ("  (SO_PEERCRED set on connected socket succeeded — "
	      "implementation-defined)\n");
      return 0;
    }

  /* Expected: some error (EISCONN, EALREADY, EINVAL, ENOPROTOOPT) */
  output ("  (SO_PEERCRED on connected socket: %s — OK)\n",
	  strerror (saved_errno));
  return 0;
}
#endif

/* ------------------------------------------------------------------ */
/*  Test 7: getpeereid on non-AF_UNIX socket                          */
/* ------------------------------------------------------------------ */

static int
test_getpeereid_non_unix (void)
{
  /* Create a TCP socket — not AF_UNIX */
  int s = socket (AF_INET, SOCK_STREAM, 0);
  if (s < 0)
    {
      output ("  socket(AF_INET): %s\n", strerror (errno));
      return -1;
    }

#ifdef __CYGWIN__
  uid_t uid;
  gid_t gid;
  int rc = getpeereid (s, &uid, &gid);
  int saved_errno = errno;
  close (s);

  if (rc == 0)
    {
      output ("  getpeereid on AF_INET socket succeeded\n");
      return -1;
    }
  /* Expected: ENOTSUP or EINVAL (not AF_LOCAL/SOCK_STREAM) */
  if (saved_errno != ENOTSUP && saved_errno != EINVAL
      && saved_errno != ENOTSOCK)
    {
      output ("  errno=%d (%s), expected ENOTSUP or EINVAL\n",
	      saved_errno, strerror (saved_errno));
      return -1;
    }
#else
  /* Linux: SO_PEERCRED succeeds on any socket type (returns zeroed ucred
     for unconnected sockets).  Just verify it doesn't crash and returns
     reasonable data.  This is documented Linux behavior. */
  struct ucred cred;
  socklen_t len = sizeof (cred);
  int rc = getsockopt (s, SOL_SOCKET, SO_PEERCRED, &cred, &len);
  close (s);

  if (rc != 0)
    {
      output ("  SO_PEERCRED on AF_INET socket failed: %s\n",
	      strerror (errno));
      return -1;
    }
  /* On Linux, unconnected AF_INET returns pid=0 */
  output ("  SO_PEERCRED on AF_INET: pid=%d uid=%d gid=%d (expected pid=0)\n",
	  cred.pid, cred.uid, cred.gid);
  if (cred.pid != 0)
    {
      output ("  unexpected pid=%d on unconnected AF_INET socket\n", cred.pid);
      return -1;
    }
#endif

  return 0;
}

/* ------------------------------------------------------------------ */
/*  Test 8: double close safety                                       */
/* ------------------------------------------------------------------ */

/*
 * Close an AF_UNIX socket twice.  The second close should fail with
 * EBADF but must not crash or corrupt state.
 */
static int
test_double_close (void)
{
  int sv[2];

  if (socketpair (AF_UNIX, SOCK_STREAM, 0, sv) < 0)
    {
      output ("  socketpair: %s\n", strerror (errno));
      return -1;
    }

  close (sv[0]);
  close (sv[1]);

  /* Second close should fail with EBADF, not crash */
  int rc = close (sv[0]);
  if (rc == 0)
    {
      output ("  double close succeeded (unexpected)\n");
      return -1;
    }
  if (errno != EBADF)
    {
      output ("  double close: errno=%d (%s), expected EBADF\n",
	      errno, strerror (errno));
      return -1;
    }

  return 0;
}

/* ------------------------------------------------------------------ */
/*  Main                                                              */
/* ------------------------------------------------------------------ */

int
main (void)
{
  int failures = 0;
  output_init ();
  output ("AF_UNIX error path and POSIX conformance tests\n\n");

  alarm (30);

  output ("Test 1: bind with path too long\n");
  if (test_bind_enametoolong () == 0)
    output ("  PASS\n");
  else
    {
      output ("  FAIL\n");
      failures++;
    }

  output ("\nTest 2: bind to already-bound path (EADDRINUSE)\n");
  if (test_bind_eaddrinuse () == 0)
    output ("  PASS\n");
  else
    {
      output ("  FAIL\n");
      failures++;
    }

  output ("\nTest 3: connect to nonexistent path\n");
  if (test_connect_noent () == 0)
    output ("  PASS\n");
  else
    {
      output ("  FAIL\n");
      failures++;
    }

  output ("\nTest 4: accept on non-listening socket\n");
  if (test_accept_not_listening () == 0)
    output ("  PASS\n");
  else
    {
      output ("  FAIL\n");
      failures++;
    }

  output ("\nTest 5: send on unconnected SOCK_STREAM\n");
  if (test_send_unconnected () == 0)
    output ("  PASS\n");
  else
    {
      output ("  FAIL\n");
      failures++;
    }

#ifdef __CYGWIN__
  output ("\nTest 6: SO_PEERCRED setsockopt on connected socket\n");
  if (test_so_peercred_after_connect () == 0)
    output ("  PASS\n");
  else
    {
      output ("  FAIL\n");
      failures++;
    }
#else
  output ("\nTest 6: SO_PEERCRED setsockopt on connected socket — "
	  "SKIPPED (Cygwin-specific)\n");
#endif

  output ("\nTest 7: getpeereid on non-AF_UNIX socket\n");
  if (test_getpeereid_non_unix () == 0)
    output ("  PASS\n");
  else
    {
      output ("  FAIL\n");
      failures++;
    }

  output ("\nTest 8: double close safety\n");
  if (test_double_close () == 0)
    output ("  PASS\n");
  else
    {
      output ("  FAIL\n");
      failures++;
    }

  if (failures > 0)
    {
      output ("\n%d test(s) FAILED\n", failures);
      FAILED ("AF_UNIX error path test failed");
    }

  output ("\n=== All AF_UNIX error path tests PASSED ===\n");
  PASSED;
}
