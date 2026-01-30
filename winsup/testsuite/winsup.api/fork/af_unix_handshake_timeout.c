/*
 * Cygwin AF_UNIX handshake timeout test
 *
 * Tests the CYGWIN=af_unix_handshake_timeout feature which gates the
 * handshake tolerance mechanism in af_local_accept().
 *
 * Three code paths:
 *   (default, ms=0)     Legacy blocking handshake
 *   (ms=1..N)           Probe with timeout, OS fallback if no handshake
 *   (ms=(DWORD)-1)      Skip probe, always OS credential lookup
 *
 * Sub-tests:
 *   1. Fork-based Cygwin-to-Cygwin handshake (works in legacy + timeout modes)
 *   2. Exec server (timeout probe) + Cygwin client — handshake detected
 *   3. Exec server (timeout probe) + non-handshake client — OS fallback
 *   4. Exec server (skip-probe) + non-handshake client — direct OS lookup
 *   5. Custom timeout (200ms) + Cygwin client — proves custom value parsing
 *   6. Custom timeout (200ms) + non-HS client — proves custom timeout fallback
 *   7. Explicit disable (noaf_unix_handshake_timeout) — proves no-prefix works
 *   8. Zero value (af_unix_handshake_timeout:0) — proves :0 means disabled
 *   9. Multiple sequential connections — proves server reusability
 *  10. Client disconnect during probe — proves robustness
 *
 * The test re-execs itself in server mode with modified CYGWIN env so
 * the DLL re-initializes with the desired af_unix_handshake_timeout_ms.
 */

#define _GNU_SOURCE

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

#include "testfrmw.h"
#include "testfrmw.c"

#define POLL_US         50000   /* 50 ms */
#define POLL_MAX        200     /* 200 * 50ms = 10s */
#define SERVER_ALARM    15      /* seconds */

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

/* Poll-based waitpid with bounded timeout.
   Returns child exit code, or -1 on abnormal exit, -2 on timeout. */
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

static void
make_path (char *buf, size_t len, const char *tag)
{
  const char *tmpdir = getenv ("TMPDIR");
  if (!tmpdir || !*tmpdir)
    tmpdir = getenv ("TMP");
  if (!tmpdir || !*tmpdir)
    tmpdir = ".";
  snprintf (buf, len, "%s/af_hs_%d_%s", tmpdir, getpid (), tag);
}

/* ------------------------------------------------------------------ */
/*  Server role (exec'd child)                                        */
/* ------------------------------------------------------------------ */

/*
 * Usage: <exe> --server <socket_path> <ready_fd>
 *
 * Bind, listen, signal ready, accept one connection, getpeereid, exit.
 * Exit codes:
 *   0 = success (getpeereid returned valid credentials)
 *   1 = setup error (socket/bind/listen)
 *   2 = accept error
 *   3 = getpeereid failed
 *   4 = credential mismatch
 */
static int
run_server (const char *path, int ready_fd)
{
  int s, c, rc;
  struct sockaddr_un addr;
  uid_t peer_uid;
  gid_t peer_gid;

  alarm (SERVER_ALARM);

  s = socket (AF_UNIX, SOCK_STREAM, 0);
  if (s < 0)
    {
      perror ("server: socket");
      return 1;
    }

  memset (&addr, 0, sizeof (addr));
  addr.sun_family = AF_UNIX;
  strncpy (addr.sun_path, path, sizeof (addr.sun_path) - 1);
  unlink (path);

  if (bind (s, (struct sockaddr *) &addr, sizeof (addr)) < 0)
    {
      perror ("server: bind");
      close (s);
      return 1;
    }
  if (listen (s, 1) < 0)
    {
      perror ("server: listen");
      close (s);
      unlink (path);
      return 1;
    }

  /* Signal ready to parent */
  if (write (ready_fd, "R", 1) != 1)
    {
      perror ("server: write ready");
      close (s);
      unlink (path);
      return 1;
    }
  close (ready_fd);

  c = accept (s, NULL, NULL);
  close (s);
  if (c < 0)
    {
      perror ("server: accept");
      unlink (path);
      return 2;
    }

  rc = getpeereid (c, &peer_uid, &peer_gid);

  /* Send done byte so client knows we've finished */
  write (c, "D", 1);
  close (c);
  unlink (path);

  if (rc != 0)
    {
      fprintf (stderr, "server: getpeereid: %s\n", strerror (errno));
      return 3;
    }
  if (peer_uid != geteuid ())
    {
      fprintf (stderr, "server: uid %d != %d\n",
	       (int) peer_uid, (int) geteuid ());
      return 4;
    }
  if (peer_gid != getegid ())
    {
      fprintf (stderr, "server: gid %d != %d\n",
	       (int) peer_gid, (int) getegid ());
      return 4;
    }

  return 0;
}

/* ------------------------------------------------------------------ */
/*  Multi-server role (exec'd child, accepts N connections)           */
/* ------------------------------------------------------------------ */

/*
 * Usage: <exe> --multi-server <socket_path> <ready_fd> <count>
 *
 * Like run_server but accepts <count> connections sequentially,
 * validating getpeereid on each.
 * Exit codes: same as run_server (first failure wins).
 */
static int
run_multi_server (const char *path, int ready_fd, int count)
{
  int s;
  struct sockaddr_un addr;

  alarm (SERVER_ALARM);

  s = socket (AF_UNIX, SOCK_STREAM, 0);
  if (s < 0)
    {
      perror ("multi-server: socket");
      return 1;
    }

  memset (&addr, 0, sizeof (addr));
  addr.sun_family = AF_UNIX;
  strncpy (addr.sun_path, path, sizeof (addr.sun_path) - 1);
  unlink (path);

  if (bind (s, (struct sockaddr *) &addr, sizeof (addr)) < 0)
    {
      perror ("multi-server: bind");
      close (s);
      return 1;
    }
  if (listen (s, 5) < 0)
    {
      perror ("multi-server: listen");
      close (s);
      unlink (path);
      return 1;
    }

  /* Signal ready to parent */
  if (write (ready_fd, "R", 1) != 1)
    {
      perror ("multi-server: write ready");
      close (s);
      unlink (path);
      return 1;
    }
  close (ready_fd);

  for (int i = 0; i < count; i++)
    {
      int c = accept (s, NULL, NULL);
      if (c < 0)
	{
	  fprintf (stderr, "multi-server: accept[%d]: %s\n",
		   i, strerror (errno));
	  close (s);
	  unlink (path);
	  return 2;
	}

      uid_t peer_uid;
      gid_t peer_gid;
      int rc = getpeereid (c, &peer_uid, &peer_gid);

      write (c, "D", 1);
      close (c);

      if (rc != 0)
	{
	  fprintf (stderr, "multi-server: getpeereid[%d]: %s\n",
		   i, strerror (errno));
	  close (s);
	  unlink (path);
	  return 3;
	}
      if (peer_uid != geteuid () || peer_gid != getegid ())
	{
	  fprintf (stderr, "multi-server: cred[%d]: uid=%d(%d) gid=%d(%d)\n",
		   i, (int) peer_uid, (int) geteuid (),
		   (int) peer_gid, (int) getegid ());
	  close (s);
	  unlink (path);
	  return 4;
	}
    }

  close (s);
  unlink (path);
  return 0;
}

/* ------------------------------------------------------------------ */
/*  Test 1: Fork-based Cygwin-to-Cygwin handshake                    */
/* ------------------------------------------------------------------ */

/*
 * Parent = server (bind/listen/accept), child = client (connect).
 * Both share the same DLL state, so both do the Cygwin handshake.
 * Works in legacy mode (blocking handshake) and timeout-probe mode
 * (probe detects handshake data).  Skipped in skip-probe mode because
 * the server never reciprocates and the client blocks on recv.
 */
static int
test_fork_handshake (void)
{
  char path[108];
  make_path (path, sizeof (path), "fork");

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
  unlink (path);

  if (bind (s, (struct sockaddr *) &addr, sizeof (addr)) < 0
      || listen (s, 1) < 0)
    {
      output ("  bind/listen: %s\n", strerror (errno));
      close (s);
      unlink (path);
      return -1;
    }

  pid_t pid = fork ();
  if (pid < 0)
    {
      output ("  fork: %s\n", strerror (errno));
      close (s);
      unlink (path);
      return -1;
    }

  if (pid == 0)
    {
      /* Child = client */
      close (s);
      int cs = socket (AF_UNIX, SOCK_STREAM, 0);
      if (cs < 0)
	_exit (1);
      if (connect (cs, (struct sockaddr *) &addr, sizeof (addr)) < 0)
	_exit (2);
      /* Wait for server to finish (recv returns 0 on close or 'D') */
      char buf;
      recv (cs, &buf, 1, 0);
      close (cs);
      _exit (0);
    }

  /* Parent = server */
  int c = accept (s, NULL, NULL);
  close (s);
  if (c < 0)
    {
      output ("  accept: %s\n", strerror (errno));
      unlink (path);
      kill (pid, SIGKILL);
      waitpid (pid, NULL, 0);
      return -1;
    }

  uid_t peer_uid;
  gid_t peer_gid;
  int rc = getpeereid (c, &peer_uid, &peer_gid);

  /* Signal client we're done */
  write (c, "D", 1);
  close (c);
  unlink (path);

  int child_rc = wait_child (pid);
  if (child_rc != 0)
    {
      output ("  client exit=%d\n", child_rc);
      return -1;
    }
  if (rc != 0)
    {
      output ("  getpeereid: %s\n", strerror (errno));
      return -1;
    }
  if (peer_uid != geteuid () || peer_gid != getegid ())
    {
      output ("  cred mismatch: uid=%d(%d) gid=%d(%d)\n",
	      (int) peer_uid, (int) geteuid (),
	      (int) peer_gid, (int) getegid ());
      return -1;
    }
  return 0;
}

/* ------------------------------------------------------------------ */
/*  Tests 2-4: Exec-based with modified CYGWIN env                    */
/* ------------------------------------------------------------------ */

/*
 * Fork+exec a server child with a specific CYGWIN env value, then
 * connect as client (optionally with SO_PEERCRED=0 to disable the
 * Cygwin handshake on the client side).
 *
 * The exec'd server re-runs this binary in --server mode, getting
 * fresh DLL init with the new CYGWIN value.
 */
static int
test_exec_mode (const char *self, const char *tag,
		const char *cygwin_val, int nohs)
{
  char path[108];
  make_path (path, sizeof (path), tag);

  int pipefd[2];
  if (pipe (pipefd) < 0)
    {
      output ("  pipe: %s\n", strerror (errno));
      return -1;
    }

  pid_t srv = fork ();
  if (srv < 0)
    {
      output ("  fork: %s\n", strerror (errno));
      close (pipefd[0]);
      close (pipefd[1]);
      return -1;
    }

  if (srv == 0)
    {
      /* Child: exec server with modified CYGWIN */
      close (pipefd[0]);
      if (cygwin_val)
	setenv ("CYGWIN", cygwin_val, 1);

      char fd_str[16];
      snprintf (fd_str, sizeof (fd_str), "%d", pipefd[1]);
      execl (self, self, "--server", path, fd_str, (char *) NULL);
      perror ("execl");
      _exit (127);
    }

  close (pipefd[1]);

  /* Wait for server ready with timeout */
  alarm (10);
  char r;
  int n = read (pipefd[0], &r, 1);
  alarm (0);
  close (pipefd[0]);

  if (n != 1 || r != 'R')
    {
      output ("  server not ready (n=%d)\n", n);
      kill (srv, SIGKILL);
      waitpid (srv, NULL, 0);
      unlink (path);
      return -1;
    }

  /* Connect as client */
  int cs = socket (AF_UNIX, SOCK_STREAM, 0);
  if (cs < 0)
    {
      output ("  client socket: %s\n", strerror (errno));
      kill (srv, SIGKILL);
      waitpid (srv, NULL, 0);
      unlink (path);
      return -1;
    }

  if (nohs)
    {
      /* Disable Cygwin handshake on client side so the server must
	 use OS-level credential lookup instead. */
      int zero = 0;
      if (setsockopt (cs, SOL_SOCKET, SO_PEERCRED,
		      &zero, sizeof (zero)) < 0)
	{
	  output ("  SO_PEERCRED=0: %s\n", strerror (errno));
	  close (cs);
	  kill (srv, SIGKILL);
	  waitpid (srv, NULL, 0);
	  unlink (path);
	  return -1;
	}
    }

  struct sockaddr_un addr;
  memset (&addr, 0, sizeof (addr));
  addr.sun_family = AF_UNIX;
  strncpy (addr.sun_path, path, sizeof (addr.sun_path) - 1);

  if (connect (cs, (struct sockaddr *) &addr, sizeof (addr)) < 0)
    {
      output ("  connect: %s\n", strerror (errno));
      close (cs);
      kill (srv, SIGKILL);
      waitpid (srv, NULL, 0);
      unlink (path);
      return -1;
    }

  /* Wait for server's done signal */
  char buf;
  recv (cs, &buf, 1, 0);
  close (cs);

  int srv_rc = wait_child (srv);
  unlink (path);
  return srv_rc;
}

/* ------------------------------------------------------------------ */
/*  Test 9: Multiple sequential connections                           */
/* ------------------------------------------------------------------ */

/*
 * Fork+exec a multi-accept server with timeout probe, then connect
 * 3 times sequentially.  All connections should succeed and get
 * correct credentials.
 */
static int
test_multi_connect (const char *self, int count)
{
  char path[108];
  make_path (path, sizeof (path), "t9_multi");

  int pipefd[2];
  if (pipe (pipefd) < 0)
    {
      output ("  pipe: %s\n", strerror (errno));
      return -1;
    }

  pid_t srv = fork ();
  if (srv < 0)
    {
      output ("  fork: %s\n", strerror (errno));
      close (pipefd[0]);
      close (pipefd[1]);
      return -1;
    }

  if (srv == 0)
    {
      close (pipefd[0]);
      setenv ("CYGWIN", "af_unix_handshake_timeout", 1);

      char fd_str[16], cnt_str[16];
      snprintf (fd_str, sizeof (fd_str), "%d", pipefd[1]);
      snprintf (cnt_str, sizeof (cnt_str), "%d", count);
      execl (self, self, "--multi-server", path, fd_str, cnt_str,
	     (char *) NULL);
      perror ("execl");
      _exit (127);
    }

  close (pipefd[1]);

  /* Wait for server ready */
  alarm (10);
  char r;
  int n = read (pipefd[0], &r, 1);
  alarm (0);
  close (pipefd[0]);

  if (n != 1 || r != 'R')
    {
      output ("  server not ready (n=%d)\n", n);
      kill (srv, SIGKILL);
      waitpid (srv, NULL, 0);
      unlink (path);
      return -1;
    }

  struct sockaddr_un addr;
  memset (&addr, 0, sizeof (addr));
  addr.sun_family = AF_UNIX;
  strncpy (addr.sun_path, path, sizeof (addr.sun_path) - 1);

  for (int i = 0; i < count; i++)
    {
      int cs = socket (AF_UNIX, SOCK_STREAM, 0);
      if (cs < 0)
	{
	  output ("  client socket[%d]: %s\n", i, strerror (errno));
	  kill (srv, SIGKILL);
	  waitpid (srv, NULL, 0);
	  unlink (path);
	  return -1;
	}

      if (connect (cs, (struct sockaddr *) &addr, sizeof (addr)) < 0)
	{
	  output ("  connect[%d]: %s\n", i, strerror (errno));
	  close (cs);
	  kill (srv, SIGKILL);
	  waitpid (srv, NULL, 0);
	  unlink (path);
	  return -1;
	}

      /* Wait for server's done signal */
      char buf;
      recv (cs, &buf, 1, 0);
      close (cs);
    }

  int srv_rc = wait_child (srv);
  unlink (path);
  return srv_rc;
}

/* ------------------------------------------------------------------ */
/*  Test 10: Client disconnect during probe                           */
/* ------------------------------------------------------------------ */

/*
 * Server in timeout-probe mode.  Client connects with SO_PEERCRED=0
 * (no handshake) then closes immediately.  Server's probe should
 * detect EOF/timeout and fall back to OS lookup gracefully.
 * The server must not hang — any exit code 0-4 is acceptable.
 */
static int
test_client_disconnect (const char *self)
{
  char path[108];
  make_path (path, sizeof (path), "t10_disc");

  int pipefd[2];
  if (pipe (pipefd) < 0)
    {
      output ("  pipe: %s\n", strerror (errno));
      return -1;
    }

  pid_t srv = fork ();
  if (srv < 0)
    {
      output ("  fork: %s\n", strerror (errno));
      close (pipefd[0]);
      close (pipefd[1]);
      return -1;
    }

  if (srv == 0)
    {
      close (pipefd[0]);
      setenv ("CYGWIN", "af_unix_handshake_timeout", 1);

      char fd_str[16];
      snprintf (fd_str, sizeof (fd_str), "%d", pipefd[1]);
      execl (self, self, "--server", path, fd_str, (char *) NULL);
      perror ("execl");
      _exit (127);
    }

  close (pipefd[1]);

  /* Wait for server ready */
  alarm (10);
  char r;
  int n = read (pipefd[0], &r, 1);
  alarm (0);
  close (pipefd[0]);

  if (n != 1 || r != 'R')
    {
      output ("  server not ready (n=%d)\n", n);
      kill (srv, SIGKILL);
      waitpid (srv, NULL, 0);
      unlink (path);
      return -1;
    }

  /* Connect with SO_PEERCRED=0 (no handshake), then close immediately */
  int cs = socket (AF_UNIX, SOCK_STREAM, 0);
  if (cs < 0)
    {
      output ("  client socket: %s\n", strerror (errno));
      kill (srv, SIGKILL);
      waitpid (srv, NULL, 0);
      unlink (path);
      return -1;
    }

  int zero = 0;
  setsockopt (cs, SOL_SOCKET, SO_PEERCRED, &zero, sizeof (zero));

  struct sockaddr_un addr;
  memset (&addr, 0, sizeof (addr));
  addr.sun_family = AF_UNIX;
  strncpy (addr.sun_path, path, sizeof (addr.sun_path) - 1);

  if (connect (cs, (struct sockaddr *) &addr, sizeof (addr)) < 0)
    {
      output ("  connect: %s\n", strerror (errno));
      close (cs);
      kill (srv, SIGKILL);
      waitpid (srv, NULL, 0);
      unlink (path);
      return -1;
    }

  /* Close immediately — no recv, no waiting */
  close (cs);

  int srv_rc = wait_child (srv);
  unlink (path);

  /* Server exited without hanging.  Exit codes 0-4 are all acceptable
     (OS lookup may or may not succeed when peer is gone). */
  if (srv_rc == -2)
    {
      output ("  server hung (timeout)\n");
      return -1;
    }
  if (srv_rc < 0 || srv_rc > 4)
    {
      output ("  server unexpected exit=%d\n", srv_rc);
      return -1;
    }
  return 0;
}

/* ------------------------------------------------------------------ */
/*  Main                                                              */
/* ------------------------------------------------------------------ */

int
main (int argc, char *argv[])
{
  /* Server role — entered via exec */
  if (argc == 4 && strcmp (argv[1], "--server") == 0)
    return run_server (argv[2], atoi (argv[3]));

  /* Multi-server role — entered via exec */
  if (argc == 5 && strcmp (argv[1], "--multi-server") == 0)
    return run_multi_server (argv[2], atoi (argv[3]), atoi (argv[4]));

  /* Test runner */
  int failures = 0;
  output_init ();
  output ("AF_UNIX handshake timeout tests\n");

  const char *cygwin = getenv ("CYGWIN");
  output ("CYGWIN=%s\n\n", cygwin ? cygwin : "(not set)");

  alarm (120);

  /* Detect current mode from env.
     We cannot read af_unix_handshake_timeout_ms directly since it is
     internal to the DLL.  Infer from the CYGWIN env var. */
  int cur_mode = 0; /* 0=legacy, 1=timeout, 2=skip */
  if (cygwin)
    {
      const char *p = strstr (cygwin, "af_unix_handshake_timeout");
      if (p)
	{
	  p += strlen ("af_unix_handshake_timeout");
	  if (*p == ':' && atoi (p + 1) == -1)
	    cur_mode = 2;
	  else
	    cur_mode = 1;
	}
    }

  /* ----- Test 1: Fork Cygwin-to-Cygwin handshake ----- */
  /* Works in legacy and timeout-probe modes.
     Skipped in skip-probe mode because the client does a blocking
     handshake that the server never reciprocates. */
  if (cur_mode != 2)
    {
      output ("Test 1: Fork Cygwin-to-Cygwin (%s mode)\n",
	      cur_mode == 0 ? "legacy" : "timeout");
      if (test_fork_handshake () == 0)
	output ("  PASS\n");
      else
	{
	  output ("  FAIL\n");
	  failures++;
	}
    }
  else
    output ("Test 1: Fork Cygwin-to-Cygwin — SKIPPED (skip-probe mode)\n");

  /* ----- Test 2: Exec server (timeout probe) + Cygwin client ----- */
  /* Server has af_unix_handshake_timeout=100.  Client is this process
     (default CYGWIN), so it does the full Cygwin handshake.  Server's
     probe detects the handshake data and runs the full exchange. */
  output ("\nTest 2: Exec server (timeout probe) + Cygwin client\n");
  if (test_exec_mode (argv[0], "t2_cyg",
		      "af_unix_handshake_timeout", 0) == 0)
    output ("  PASS\n");
  else
    {
      output ("  FAIL\n");
      failures++;
    }

  /* ----- Test 3: Exec server (timeout probe) + non-HS client ----- */
  /* Client uses SO_PEERCRED=0 to skip the handshake.  Server probes,
     times out after 100ms, falls back to OS credential lookup. */
  output ("\nTest 3: Exec server (timeout probe) + non-HS client\n");
  if (test_exec_mode (argv[0], "t3_nohs",
		      "af_unix_handshake_timeout", 1) == 0)
    output ("  PASS\n");
  else
    {
      output ("  FAIL\n");
      failures++;
    }

  /* ----- Test 4: Exec server (skip probe) + non-HS client ----- */
  /* Server has af_unix_handshake_timeout:-1 (skip probe entirely).
     Client uses SO_PEERCRED=0.  Server does direct OS lookup. */
  output ("\nTest 4: Exec server (skip probe) + non-HS client\n");
  if (test_exec_mode (argv[0], "t4_skip",
		      "af_unix_handshake_timeout:-1", 1) == 0)
    output ("  PASS\n");
  else
    {
      output ("  FAIL\n");
      failures++;
    }

  /* ----- Test 5: Custom timeout (200ms) + Cygwin client ----- */
  /* Server has af_unix_handshake_timeout:200.  Client does full
     Cygwin handshake.  Proves custom env var value is parsed. */
  output ("\nTest 5: Custom timeout (200ms) + Cygwin client\n");
  if (test_exec_mode (argv[0], "t5_200",
		      "af_unix_handshake_timeout:200", 0) == 0)
    output ("  PASS\n");
  else
    {
      output ("  FAIL\n");
      failures++;
    }

  /* ----- Test 6: Custom timeout (200ms) + non-HS client ----- */
  /* Server has af_unix_handshake_timeout:200.  Client uses SO_PEERCRED=0.
     Server probes for 200ms, times out, falls back to OS lookup. */
  output ("\nTest 6: Custom timeout (200ms) + non-HS client\n");
  if (test_exec_mode (argv[0], "t6_200nohs",
		      "af_unix_handshake_timeout:200", 1) == 0)
    output ("  PASS\n");
  else
    {
      output ("  FAIL\n");
      failures++;
    }

  /* ----- Test 7: Explicit disable (noaf_unix_handshake_timeout) ----- */
  /* Server has noaf_unix_handshake_timeout, which sets ms=0 (legacy).
     Client does full Cygwin handshake.  Proves "no" prefix works. */
  output ("\nTest 7: Explicit disable (noaf_unix_handshake_timeout)\n");
  if (test_exec_mode (argv[0], "t7_nohs",
		      "noaf_unix_handshake_timeout", 0) == 0)
    output ("  PASS\n");
  else
    {
      output ("  FAIL\n");
      failures++;
    }

  /* ----- Test 8: Zero value (af_unix_handshake_timeout:0) ----- */
  /* Server has af_unix_handshake_timeout:0, which sets ms=0 (=disabled,
     same as legacy).  Client does full Cygwin handshake.
     Documents that :0 means "disabled", same as not setting the option. */
  output ("\nTest 8: Zero value (af_unix_handshake_timeout:0)\n");
  if (test_exec_mode (argv[0], "t8_zero",
		      "af_unix_handshake_timeout:0", 0) == 0)
    output ("  PASS\n");
  else
    {
      output ("  FAIL\n");
      failures++;
    }

  /* ----- Test 9: Multiple sequential connections ----- */
  /* Server in timeout probe mode accepts 3 connections sequentially.
     All connections do the full Cygwin handshake.  Proves reusability. */
  output ("\nTest 9: Multiple sequential connections (3x)\n");
  if (test_multi_connect (argv[0], 3) == 0)
    output ("  PASS\n");
  else
    {
      output ("  FAIL\n");
      failures++;
    }

  /* ----- Test 10: Client disconnect during probe ----- */
  /* Server in timeout probe mode.  Client connects with SO_PEERCRED=0
     then closes immediately.  Server must not hang. */
  output ("\nTest 10: Client disconnect during probe\n");
  if (test_client_disconnect (argv[0]) == 0)
    output ("  PASS\n");
  else
    {
      output ("  FAIL\n");
      failures++;
    }

  if (failures > 0)
    {
      output ("\n%d test(s) FAILED\n", failures);
      FAILED ("AF_UNIX handshake timeout test failed");
    }

  output ("\n=== All AF_UNIX handshake timeout tests PASSED ===\n");
  PASSED;
}
