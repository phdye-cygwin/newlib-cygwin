/* lock_reinit.cc - Lock reinitialization after RtlCloneUserProcess

   This file is part of Cygwin.

   This software is a copyrighted work licensed under the terms of the
   Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
   details. */

#include "winsup.h"
#include "lock_reinit.h"
#include "sync.h"
#include <sys/reent.h>
#include <sys/lock.h>

/* Master reinit function - calls all subsystem reinit functions.
   Must be called as the FIRST thing in the child after RtlCloneUserProcess
   returns STATUS_PROCESS_CLONED, before any other Cygwin code executes.

   The order matters: malloc must be first since other reinit functions
   might allocate memory (though they shouldn't, to be safe).  */

void
reinit_all_locks_after_clone ()
{
  /* CRITICAL: malloc lock must be reinitialized FIRST because any
     subsequent operations might need to allocate memory.  */
  malloc_reinit_lock_after_clone ();

  /* Reset and reinitialize lock_process muto - used by many subsystems
     including file descriptor operations (dtable::lock via cygheap_fdget).
     Must be early since it guards many critical sections.
     CRITICAL: reset_after_clone() only clears name/bruteforce, but does NOT
     reinitialize the muto.  Must call init() after reset to create a new
     bruteforce event and set name, otherwise acquire() will fail (bruteforce
     is NULL → WaitForSingleObject(NULL) → WAIT_FAILED). */
  lock_process::reset_after_clone ();
  lock_process::init ();

  /* Reset shared directory handles - these are inherited from parent via
     COW but the handle values are invalid in the child's handle table.
     Must be reset early so get_shared_parent_dir() works correctly. */
  shared_reinit_after_clone ();

  /* Cygheap protection lock - used by many subsystems */
  cygheap_reinit_lock_after_clone ();

  /* tls_sentry muto - guards thread list access.  May have been held
     by parent's wait_sig thread at clone time, causing deadlock. */
  tls_sentry_reinit_lock_after_clone ();

  /* Memory mapping lock */
  mmap_reinit_lock_after_clone ();

  /* Current working directory lock */
  cwdstuff_reinit_lock_after_clone ();

  /* SYSV shared memory lock */
  shm_reinit_lock_after_clone ();

  /* Select/poll pty peek lock */
  select_reinit_lock_after_clone ();

  /* Security helper locks (authz, user_ctx, slist) */
  sec_reinit_locks_after_clone ();

  /* Random number generator lock */
  random_reinit_lock_after_clone ();

  /* Clock/TAI leap second lock */
  clock_reinit_lock_after_clone ();

  /* Debug lock */
  debug_reinit_lock_after_clone ();

  /* Timezone lock */
  tzset_reinit_lock_after_clone ();

  /* Signal processing lock */
  sigproc_reinit_lock_after_clone ();

  /* File handler NPFS lock */
  fhandler_reinit_lock_after_clone ();

  /* POSIX timer lock */
  posix_timer_reinit_lock_after_clone ();

  /* DLL list protect muto - not NO_COPY, but inherited via COW.
     Must reset before any DLL operations in child. */
  dll_reinit_lock_after_clone ();

  /* Reset stdio FILE locks (stdin, stdout, stderr).
     These use pthread_mutex internally via __cygwin_lock_*.
     After RtlClone, the child has COW copies of these locks which
     may be in a held state (if parent was doing I/O) or reference
     thread IDs that don't exist in the child.  Reset to initial state. */
  extern __FILE __sf[3];
  for (int i = 0; i < 3; i++)
    __sf[i]._lock = _LOCK_T_INITIALIZER;

  /* Also reset the per-thread reent's stdin/stdout/stderr locks.
     _REENT points to the current thread's reent structure. */
  if (_REENT && _REENT != _GLOBAL_REENT)
    {
      /* Thread has its own reent - also need to reset those FILE locks */
      /* For now, this is handled by the above __sf reset since
         _REENT->_stdin etc typically point to __sf[0..2] */
    }
}
