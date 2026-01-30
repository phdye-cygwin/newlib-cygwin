/*
 * Cygwin AF_UNIX stress and concurrency tests
 *
 * Tests AF_UNIX socket behavior under concurrent load:
 *   1. Full lifecycle: bind→listen→connect→handshake→accept→send/recv→close
 *   2. Concurrent clients: 8 simultaneous fork'd clients to one server
 *   3. Rapid connect/disconnect cycling (16 iterations)
 *   4. Data integrity: large transfer through connected AF_UNIX pair
 *
 * All tests use fork (not threads) to avoid DLL threading complications.
 * All blocking operations are bounded by alarm() or poll-based waitpid.
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
  snprintf (buf, len, "%s/af_st_%d_%s", tmpdir, getpid (), tag);
}

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
/*  Test 1: Full lifecycle                                            */
/* ------------------------------------------------------------------ */

/*
 * Complete lifecycle in one test:
 *   bind → listen → fork(client) → connect → accept →
 *   bidirectional send/recv → getpeereid → close
 */
static int
test_full_lifecycle (void)
{
  char path[108];
  make_path (path, sizeof (path), "lifecycle");

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

      /* Send a message */
      const char *msg = "client-hello";
      if (send (cs, msg, strlen (msg), 0) != (ssize_t) strlen (msg))
	_exit (3);

      /* Receive reply */
      char buf[32];
      int n = recv (cs, buf, sizeof (buf) - 1, 0);
      if (n <= 0)
	_exit (4);
      buf[n] = '\0';
      if (strcmp (buf, "server-reply") != 0)
	_exit (5);

      /* Wait for done signal */
      char done;
      recv (cs, &done, 1, 0);
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

  /* Receive client's message */
  char buf[32];
  int n = recv (c, buf, sizeof (buf) - 1, 0);
  if (n <= 0)
    {
      output ("  recv: %s\n", n < 0 ? strerror (errno) : "EOF");
      close (c);
      unlink (path);
      kill (pid, SIGKILL);
      waitpid (pid, NULL, 0);
      return -1;
    }
  buf[n] = '\0';
  if (strcmp (buf, "client-hello") != 0)
    {
      output ("  received '%s' (expected 'client-hello')\n", buf);
      close (c);
      unlink (path);
      kill (pid, SIGKILL);
      waitpid (pid, NULL, 0);
      return -1;
    }

  /* Send reply */
  const char *reply = "server-reply";
  send (c, reply, strlen (reply), 0);

  /* Verify credentials */
  uid_t peer_uid;
  gid_t peer_gid;
  int rc = getpeereid (c, &peer_uid, &peer_gid);
  if (rc != 0)
    {
      output ("  getpeereid: %s\n", strerror (errno));
      write (c, "D", 1);
      close (c);
      unlink (path);
      kill (pid, SIGKILL);
      waitpid (pid, NULL, 0);
      return -1;
    }
  if (peer_uid != geteuid () || peer_gid != getegid ())
    {
      output ("  cred mismatch: uid=%d(%d) gid=%d(%d)\n",
	      (int) peer_uid, (int) geteuid (),
	      (int) peer_gid, (int) getegid ());
      write (c, "D", 1);
      close (c);
      unlink (path);
      kill (pid, SIGKILL);
      waitpid (pid, NULL, 0);
      return -1;
    }

  /* Signal done and clean up */
  write (c, "D", 1);
  close (c);
  unlink (path);

  int child_rc = wait_child (pid);
  if (child_rc != 0)
    {
      output ("  client exit=%d\n", child_rc);
      return -1;
    }

  return 0;
}

/* ------------------------------------------------------------------ */
/*  Test 2: Concurrent clients                                        */
/* ------------------------------------------------------------------ */

#define CONCURRENT_CLIENTS  8

/*
 * Server binds/listens, then forks N clients that all connect
 * simultaneously.  Server accepts each, validates credentials.
 */
static int
test_concurrent_clients (void)
{
  char path[108];
  make_path (path, sizeof (path), "conc");

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
      || listen (s, CONCURRENT_CLIENTS) < 0)
    {
      output ("  bind/listen: %s\n", strerror (errno));
      close (s);
      unlink (path);
      return -1;
    }

  /* Fork N client children */
  pid_t children[CONCURRENT_CLIENTS];
  for (int i = 0; i < CONCURRENT_CLIENTS; i++)
    {
      children[i] = fork ();
      if (children[i] < 0)
	{
	  output ("  fork[%d]: %s\n", i, strerror (errno));
	  /* Kill already-forked children */
	  for (int j = 0; j < i; j++)
	    {
	      kill (children[j], SIGKILL);
	      waitpid (children[j], NULL, 0);
	    }
	  close (s);
	  unlink (path);
	  return -1;
	}

      if (children[i] == 0)
	{
	  /* Child = client */
	  close (s);
	  int cs = socket (AF_UNIX, SOCK_STREAM, 0);
	  if (cs < 0)
	    _exit (1);
	  if (connect (cs, (struct sockaddr *) &addr, sizeof (addr)) < 0)
	    _exit (2);

	  /* Wait for done signal from server */
	  char buf;
	  recv (cs, &buf, 1, 0);
	  close (cs);
	  _exit (0);
	}
    }

  /* Parent = server: accept all N clients */
  int fail = 0;
  for (int i = 0; i < CONCURRENT_CLIENTS; i++)
    {
      int c = accept (s, NULL, NULL);
      if (c < 0)
	{
	  output ("  accept[%d]: %s\n", i, strerror (errno));
	  fail++;
	  continue;
	}

      uid_t peer_uid;
      gid_t peer_gid;
      int rc = getpeereid (c, &peer_uid, &peer_gid);
      if (rc != 0)
	{
	  output ("  getpeereid[%d]: %s\n", i, strerror (errno));
	  fail++;
	}
      else if (peer_uid != geteuid () || peer_gid != getegid ())
	{
	  output ("  cred[%d] mismatch: uid=%d gid=%d\n",
		  i, (int) peer_uid, (int) peer_gid);
	  fail++;
	}

      write (c, "D", 1);
      close (c);
    }

  close (s);
  unlink (path);

  /* Wait for all children */
  for (int i = 0; i < CONCURRENT_CLIENTS; i++)
    {
      int child_rc = wait_child (children[i]);
      if (child_rc != 0)
	{
	  output ("  child[%d] exit=%d\n", i, child_rc);
	  fail++;
	}
    }

  return fail == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/*  Test 3: Rapid connect/disconnect cycling                          */
/* ------------------------------------------------------------------ */

#define RAPID_CYCLES  16

/*
 * Server in one fork, parent rapidly creates+connects+closes sockets.
 * Tests that the server doesn't leak resources or corrupt state.
 */
static int
test_rapid_cycle (void)
{
  char path[108];
  make_path (path, sizeof (path), "rapid");

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
      || listen (s, RAPID_CYCLES) < 0)
    {
      output ("  bind/listen: %s\n", strerror (errno));
      close (s);
      unlink (path);
      return -1;
    }

  /* Fork server child that accepts RAPID_CYCLES connections */
  pid_t srv = fork ();
  if (srv < 0)
    {
      output ("  fork: %s\n", strerror (errno));
      close (s);
      unlink (path);
      return -1;
    }

  if (srv == 0)
    {
      /* Child = server: accept and immediately close */
      alarm (30);
      for (int i = 0; i < RAPID_CYCLES; i++)
	{
	  int c = accept (s, NULL, NULL);
	  if (c < 0)
	    {
	      /* Some connections may fail if client already closed */
	      continue;
	    }
	  write (c, "D", 1);
	  close (c);
	}
      close (s);
      _exit (0);
    }

  close (s);  /* Parent doesn't need listener */

  /* Parent = rapid client */
  int fail = 0;
  for (int i = 0; i < RAPID_CYCLES; i++)
    {
      int cs = socket (AF_UNIX, SOCK_STREAM, 0);
      if (cs < 0)
	{
	  output ("  cycle[%d] socket: %s\n", i, strerror (errno));
	  fail++;
	  continue;
	}

      if (connect (cs, (struct sockaddr *) &addr, sizeof (addr)) < 0)
	{
	  output ("  cycle[%d] connect: %s\n", i, strerror (errno));
	  close (cs);
	  fail++;
	  continue;
	}

      /* Wait for done signal, then close */
      char buf;
      recv (cs, &buf, 1, 0);
      close (cs);
    }

  int srv_rc = wait_child (srv);
  unlink (path);

  if (srv_rc != 0)
    {
      output ("  server exit=%d\n", srv_rc);
      return -1;
    }
  if (fail > 0)
    {
      output ("  %d/%d cycles failed\n", fail, RAPID_CYCLES);
      return -1;
    }

  return 0;
}

/* ------------------------------------------------------------------ */
/*  Test 4: Large data transfer integrity                             */
/* ------------------------------------------------------------------ */

#define TRANSFER_SIZE  (64 * 1024)  /* 64 KB */

/*
 * Send a large buffer through a socketpair and verify byte-for-byte
 * integrity.  Tests that the AF_UNIX transport doesn't corrupt data.
 */
static int
test_large_transfer (void)
{
  int sv[2];

  if (socketpair (AF_UNIX, SOCK_STREAM, 0, sv) < 0)
    {
      output ("  socketpair: %s\n", strerror (errno));
      return -1;
    }

  /* Generate a deterministic test pattern */
  unsigned char *sendbuf = malloc (TRANSFER_SIZE);
  unsigned char *recvbuf = malloc (TRANSFER_SIZE);
  if (!sendbuf || !recvbuf)
    {
      output ("  malloc failed\n");
      free (sendbuf);
      free (recvbuf);
      close (sv[0]);
      close (sv[1]);
      return -1;
    }

  for (int i = 0; i < TRANSFER_SIZE; i++)
    sendbuf[i] = (unsigned char) (i * 37 + 13);

  pid_t pid = fork ();
  if (pid < 0)
    {
      output ("  fork: %s\n", strerror (errno));
      free (sendbuf);
      free (recvbuf);
      close (sv[0]);
      close (sv[1]);
      return -1;
    }

  if (pid == 0)
    {
      /* Child: send all data on sv[0] */
      close (sv[1]);
      alarm (10);
      int total = 0;
      while (total < TRANSFER_SIZE)
	{
	  int n = send (sv[0], sendbuf + total, TRANSFER_SIZE - total, 0);
	  if (n <= 0)
	    _exit (1);
	  total += n;
	}
      close (sv[0]);
      _exit (0);
    }

  /* Parent: receive all data on sv[1] */
  close (sv[0]);
  int total = 0;
  while (total < TRANSFER_SIZE)
    {
      int n = recv (sv[1], recvbuf + total, TRANSFER_SIZE - total, 0);
      if (n <= 0)
	{
	  output ("  recv at offset %d: %s\n",
		  total, n < 0 ? strerror (errno) : "EOF");
	  close (sv[1]);
	  free (sendbuf);
	  free (recvbuf);
	  kill (pid, SIGKILL);
	  waitpid (pid, NULL, 0);
	  return -1;
	}
      total += n;
    }
  close (sv[1]);

  int child_rc = wait_child (pid);
  if (child_rc != 0)
    {
      output ("  sender exit=%d\n", child_rc);
      free (sendbuf);
      free (recvbuf);
      return -1;
    }

  /* Verify byte-for-byte */
  if (memcmp (sendbuf, recvbuf, TRANSFER_SIZE) != 0)
    {
      int first_diff = -1;
      for (int i = 0; i < TRANSFER_SIZE; i++)
	{
	  if (sendbuf[i] != recvbuf[i])
	    {
	      first_diff = i;
	      break;
	    }
	}
      output ("  data mismatch at offset %d: sent=0x%02x recv=0x%02x\n",
	      first_diff, sendbuf[first_diff], recvbuf[first_diff]);
      free (sendbuf);
      free (recvbuf);
      return -1;
    }

  free (sendbuf);
  free (recvbuf);
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
  output ("AF_UNIX stress and concurrency tests\n\n");

  alarm (60);

  output ("Test 1: Full lifecycle (bind→accept→send/recv→getpeereid)\n");
  if (test_full_lifecycle () == 0)
    output ("  PASS\n");
  else
    {
      output ("  FAIL\n");
      failures++;
    }

  output ("\nTest 2: Concurrent clients (%d simultaneous)\n",
	  CONCURRENT_CLIENTS);
  if (test_concurrent_clients () == 0)
    output ("  PASS\n");
  else
    {
      output ("  FAIL\n");
      failures++;
    }

  output ("\nTest 3: Rapid connect/disconnect (%d cycles)\n", RAPID_CYCLES);
  if (test_rapid_cycle () == 0)
    output ("  PASS\n");
  else
    {
      output ("  FAIL\n");
      failures++;
    }

  output ("\nTest 4: Large data transfer integrity (%d KB)\n",
	  TRANSFER_SIZE / 1024);
  if (test_large_transfer () == 0)
    output ("  PASS\n");
  else
    {
      output ("  FAIL\n");
      failures++;
    }

  if (failures > 0)
    {
      output ("\n%d test(s) FAILED\n", failures);
      FAILED ("AF_UNIX stress test failed");
    }

  output ("\n=== All AF_UNIX stress tests PASSED ===\n");
  PASSED;
}
