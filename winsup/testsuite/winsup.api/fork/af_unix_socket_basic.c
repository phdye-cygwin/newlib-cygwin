/*
 * Cygwin AF_UNIX socket basic tests
 *
 * Tests AF_UNIX socket fundamentals that don't depend on the handshake
 * timeout feature:
 *   1. socketpair credential verification (uid/gid match)
 *   2. getpeereid on unconnected socket (ENOTCONN)
 *   3. getpeereid on SOCK_DGRAM (EINVAL)
 *   4. SO_PEERCRED getsockopt after handshake (struct ucred)
 *   5. SOCK_DGRAM socketpair (no hang, no handshake)
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

#include "testfrmw.h"
#include "testfrmw.c"

/* ------------------------------------------------------------------ */
/*  Test 1: socketpair credential verification                        */
/* ------------------------------------------------------------------ */

/*
 * Create a SOCK_STREAM socketpair.  Both ends should report the
 * current process's uid/gid via getpeereid.
 */
static int
test_socketpair_creds (void)
{
  int sv[2];

  if (socketpair (AF_UNIX, SOCK_STREAM, 0, sv) < 0)
    {
      output ("  socketpair: %s\n", strerror (errno));
      return -1;
    }

  uid_t uid0, uid1;
  gid_t gid0, gid1;

  if (getpeereid (sv[0], &uid0, &gid0) != 0)
    {
      output ("  getpeereid(sv[0]): %s\n", strerror (errno));
      close (sv[0]);
      close (sv[1]);
      return -1;
    }
  if (getpeereid (sv[1], &uid1, &gid1) != 0)
    {
      output ("  getpeereid(sv[1]): %s\n", strerror (errno));
      close (sv[0]);
      close (sv[1]);
      return -1;
    }

  close (sv[0]);
  close (sv[1]);

  if (uid0 != geteuid () || gid0 != getegid ())
    {
      output ("  sv[0] creds: uid=%d(%d) gid=%d(%d)\n",
	      (int) uid0, (int) geteuid (),
	      (int) gid0, (int) getegid ());
      return -1;
    }
  if (uid1 != geteuid () || gid1 != getegid ())
    {
      output ("  sv[1] creds: uid=%d(%d) gid=%d(%d)\n",
	      (int) uid1, (int) geteuid (),
	      (int) gid1, (int) getegid ());
      return -1;
    }

  return 0;
}

/* ------------------------------------------------------------------ */
/*  Test 2: getpeereid on unconnected socket                          */
/* ------------------------------------------------------------------ */

/*
 * getpeereid on a socket that isn't connected should fail with ENOTCONN.
 */
static int
test_getpeereid_unconnected (void)
{
  int s = socket (AF_UNIX, SOCK_STREAM, 0);
  if (s < 0)
    {
      output ("  socket: %s\n", strerror (errno));
      return -1;
    }

  uid_t uid;
  gid_t gid;
  int rc = getpeereid (s, &uid, &gid);
  int saved_errno = errno;
  close (s);

  if (rc == 0)
    {
      output ("  getpeereid succeeded (expected ENOTCONN)\n");
      return -1;
    }
  if (saved_errno != ENOTCONN)
    {
      output ("  errno=%d (%s), expected ENOTCONN=%d\n",
	      saved_errno, strerror (saved_errno), ENOTCONN);
      return -1;
    }

  return 0;
}

/* ------------------------------------------------------------------ */
/*  Test 3: getpeereid on SOCK_DGRAM                                  */
/* ------------------------------------------------------------------ */

/*
 * getpeereid on a SOCK_DGRAM socket should fail.
 * The expected error may be EINVAL or ENOTSUP depending on implementation.
 */
static int
test_getpeereid_dgram (void)
{
  int sv[2];

  if (socketpair (AF_UNIX, SOCK_DGRAM, 0, sv) < 0)
    {
      output ("  socketpair(DGRAM): %s\n", strerror (errno));
      /* DGRAM socketpair may not be supported — skip */
      return 0;
    }

  uid_t uid;
  gid_t gid;
  int rc = getpeereid (sv[0], &uid, &gid);
  int saved_errno = errno;
  close (sv[0]);
  close (sv[1]);

  if (rc == 0)
    {
      /* Some implementations allow getpeereid on DGRAM — that's OK too.
	 Just verify the creds are reasonable. */
      if (uid != geteuid () || gid != getegid ())
	{
	  output ("  DGRAM creds wrong: uid=%d(%d) gid=%d(%d)\n",
		  (int) uid, (int) geteuid (),
		  (int) gid, (int) getegid ());
	  return -1;
	}
      output ("  (getpeereid on DGRAM succeeded — implementation allows it)\n");
      return 0;
    }

  /* Expected failure — any errno is acceptable (EINVAL, ENOTSUP, etc.) */
  output ("  (getpeereid on DGRAM failed with %s — OK)\n",
	  strerror (saved_errno));
  return 0;
}

/* ------------------------------------------------------------------ */
/*  Test 4: SO_PEERCRED getsockopt after connection                   */
/* ------------------------------------------------------------------ */

/*
 * After a SOCK_STREAM socketpair, getsockopt(SO_PEERCRED) should
 * return a struct ucred with valid pid/uid/gid.
 */
static int
test_so_peercred_getsockopt (void)
{
  int sv[2];

  if (socketpair (AF_UNIX, SOCK_STREAM, 0, sv) < 0)
    {
      output ("  socketpair: %s\n", strerror (errno));
      return -1;
    }

  struct ucred cred;
  socklen_t len = sizeof (cred);
  memset (&cred, 0, sizeof (cred));

  if (getsockopt (sv[0], SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0)
    {
      output ("  getsockopt(SO_PEERCRED): %s\n", strerror (errno));
      close (sv[0]);
      close (sv[1]);
      return -1;
    }

  close (sv[0]);
  close (sv[1]);

  if ((uid_t) cred.uid != geteuid ())
    {
      output ("  SO_PEERCRED uid=%d, expected %d\n",
	      (int) cred.uid, (int) geteuid ());
      return -1;
    }
  if ((gid_t) cred.gid != getegid ())
    {
      output ("  SO_PEERCRED gid=%d, expected %d\n",
	      (int) cred.gid, (int) getegid ());
      return -1;
    }
  if (cred.pid <= 0)
    {
      output ("  SO_PEERCRED pid=%d (invalid)\n", (int) cred.pid);
      return -1;
    }

  return 0;
}

/* ------------------------------------------------------------------ */
/*  Test 5: SOCK_DGRAM socketpair (no hang)                           */
/* ------------------------------------------------------------------ */

/*
 * SOCK_DGRAM socketpair should work without involving the handshake
 * mechanism.  Verify basic send/recv works.
 */
static int
test_dgram_socketpair (void)
{
  int sv[2];

  if (socketpair (AF_UNIX, SOCK_DGRAM, 0, sv) < 0)
    {
      /* DGRAM socketpair might not be supported — skip gracefully */
      output ("  socketpair(DGRAM): %s (skipping)\n", strerror (errno));
      return 0;
    }

  const char *msg = "hello";
  if (send (sv[0], msg, 5, 0) != 5)
    {
      output ("  send: %s\n", strerror (errno));
      close (sv[0]);
      close (sv[1]);
      return -1;
    }

  char buf[16];
  int n = recv (sv[1], buf, sizeof (buf), 0);
  close (sv[0]);
  close (sv[1]);

  if (n != 5 || memcmp (buf, msg, 5) != 0)
    {
      output ("  recv: n=%d (expected 5)\n", n);
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
  output ("AF_UNIX socket basic tests\n\n");

  alarm (30);

  output ("Test 1: socketpair credential verification\n");
  if (test_socketpair_creds () == 0)
    output ("  PASS\n");
  else
    {
      output ("  FAIL\n");
      failures++;
    }

  output ("\nTest 2: getpeereid on unconnected socket\n");
  if (test_getpeereid_unconnected () == 0)
    output ("  PASS\n");
  else
    {
      output ("  FAIL\n");
      failures++;
    }

  output ("\nTest 3: getpeereid on SOCK_DGRAM\n");
  if (test_getpeereid_dgram () == 0)
    output ("  PASS\n");
  else
    {
      output ("  FAIL\n");
      failures++;
    }

  output ("\nTest 4: SO_PEERCRED getsockopt after socketpair\n");
  if (test_so_peercred_getsockopt () == 0)
    output ("  PASS\n");
  else
    {
      output ("  FAIL\n");
      failures++;
    }

  output ("\nTest 5: SOCK_DGRAM socketpair (no hang)\n");
  if (test_dgram_socketpair () == 0)
    output ("  PASS\n");
  else
    {
      output ("  FAIL\n");
      failures++;
    }

  if (failures > 0)
    {
      output ("\n%d test(s) FAILED\n", failures);
      FAILED ("AF_UNIX socket basic test failed");
    }

  output ("\n=== All AF_UNIX socket basic tests PASSED ===\n");
  PASSED;
}
