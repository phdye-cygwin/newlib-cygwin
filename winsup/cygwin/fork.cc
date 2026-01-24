/* fork.cc

This file is part of Cygwin.

This software is a copyrighted work licensed under the terms of the
Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
details. */

#include "winsup.h"
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include "cygerrno.h"
#include "sigproc.h"
#include "pinfo.h"
#include "path.h"
#include "fhandler.h"
#include "dtable.h"
#include "cygheap.h"
#include "child_info.h"
#include "cygtls.h"
#include "tls_pbuf.h"
#include "shared_info.h"
#include "dll_init.h"
#include "cygmalloc.h"
#include "ntdll.h"
#include "lock_reinit.h"
#include "wincap.h"

#define NPIDS_HELD 4

/* Timeout to wait for child to start, parent to init child, etc.  */
/* FIXME: Once things stabilize, bump up to a few minutes.  */
#define FORK_WAIT_TIMEOUT (300 * 1000)     /* 300 seconds */

static int dofork (void **proc, bool *with_forkables);
static int dofork_rtlclone (void **proc);
static inline bool use_rtlclone_fork ();
class frok
{
  frok (bool *forkables)
    : with_forkables (forkables)
  {}
  bool *with_forkables;
  bool load_dlls;
  child_info_fork ch;
  const char *errmsg;
  int child_pid;
  int this_errno;
  HANDLE hchild;
  int parent (volatile char * volatile here);
  int child (volatile char * volatile here);
  bool error (const char *fmt, ...);
  friend int dofork (void **proc, bool *with_forkables);
};

static void
resume_child (HANDLE forker_finished)
{
  SetEvent (forker_finished);
  debug_printf ("signalled child");
  return;
}

/* Notify parent that it is time for the next step. */
static void
sync_with_parent (const char *s, bool hang_self)
{
  debug_printf ("signalling parent: %s", s);
  fork_info->ready (false);
  if (hang_self)
    {
      HANDLE h = fork_info->forker_finished;
      /* Wait for the parent to fill in our stack and heap.
	 Don't wait forever here.  If our parent dies we don't want to clog
	 the system.  If the wait fails, we really can't continue so exit.  */
      DWORD psync_rc = WaitForSingleObject (h, FORK_WAIT_TIMEOUT);
      debug_printf ("awake");
      switch (psync_rc)
	{
	case WAIT_TIMEOUT:
	  api_fatal ("WFSO timed out %s", s);
	  break;
	case WAIT_FAILED:
	  if (GetLastError () == ERROR_INVALID_HANDLE &&
	      WaitForSingleObject (fork_info->forker_finished, 1) != WAIT_FAILED)
	    break;
	  api_fatal ("WFSO failed %s, fork_finished %p, %E", s,
		     fork_info->forker_finished);
	  break;
	default:
	  debug_printf ("no problems");
	  break;
	}
    }
}

bool
frok::error (const char *fmt, ...)
{
  DWORD exit_code = ch.exit_code;
  if (!exit_code && hchild)
    {
      exit_code = ch.proc_retry (hchild);
      if (!exit_code)
	return false;
    }
  if (exit_code != EXITCODE_FORK_FAILED)
    {
      va_list ap;
      static char buf[NT_MAX_PATH + 256];
      va_start (ap, fmt);
      __small_vsprintf (buf, fmt, ap);
      errmsg = buf;
    }
  return true;
}

/* Set up a pipe which will track the life of a "pid" through
   even after we've exec'ed.  */
void
child_info::prefork (bool detached)
{
  if (!detached)
    {
      if (!CreatePipe (&rd_proc_pipe, &wr_proc_pipe, &sec_none_nih, 16))
	api_fatal ("prefork: couldn't create pipe process tracker, %E");

      if (!SetHandleInformation (wr_proc_pipe, HANDLE_FLAG_INHERIT,
				 HANDLE_FLAG_INHERIT))
	api_fatal ("prefork: couldn't set process pipe(%p) inherit state, %E",
		   wr_proc_pipe);
      ProtectHandle1 (rd_proc_pipe, rd_proc_pipe);
      ProtectHandle1 (wr_proc_pipe, wr_proc_pipe);
    }
}

int
frok::child (volatile char * volatile here)
{
  HANDLE& hParent = ch.parent;

  sync_with_parent ("after longjmp", true);
  debug_printf ("child is running.  pid %d, ppid %d, stack here %p",
		myself->pid, myself->ppid, __builtin_frame_address (0));
  sigproc_printf ("hParent %p, load_dlls %d", hParent, load_dlls);

  /* Make sure threadinfo information is properly set up. */
  if (&_my_tls != _main_tls)
    {
      _main_tls = &_my_tls;
      _main_tls->init_thread (NULL, NULL);
    }

  set_cygwin_privileges (hProcToken);
  clear_procimptoken ();
  cygheap->user.reimpersonate ();

#ifdef DEBUGGING
  if (GetEnvironmentVariableA ("FORKDEBUG", NULL, 0))
    try_to_debug ();
  char buf[80];
  /* This is useful for debugging fork problems.  Use gdb to attach to
     the pid reported here. */
  if (GetEnvironmentVariableA ("CYGWIN_FORK_SLEEP", buf, sizeof (buf)))
    {
      small_printf ("Sleeping %d after fork, pid %u\n", atoi (buf), GetCurrentProcessId ());
      Sleep (atoi (buf));
    }
#endif

  /* Incredible but true:  If we use sockets and SYSV IPC shared memory,
     there's a good chance that a duplicated socket in the child occupies
     memory which is needed to duplicate shared memory from the parent
     process, if the shared memory hasn't been duplicated already.
     The same goes very likely for "normal" mmap shared memory, too, but
     with SYSV IPC it was the first time observed.  So, *never* fixup
     fdtab before fixing up shared memory. */
  if (fixup_shms_after_fork ())
    api_fatal ("recreate_shm areas after fork failed");

  /* load dynamic dlls, if any, re-track main-executable and cygwin1.dll */
  dlls.load_after_fork (hParent);

  cygheap->fdtab.fixup_after_fork (hParent);

  /* Signal that we have successfully initialized, so the parent can
     - transfer data/bss for dynamically loaded dlls (if any), or
     - terminate the current fork call even if the child is initialized. */
  sync_with_parent ("performed fork fixups and dynamic dll loading", true);

  ForceCloseHandle1 (fork_info->forker_finished, forker_finished);

  cygbench ("fork-child");
  ld_preload ();
  fixup_hooks_after_fork ();
  _my_tls.fixup_after_fork ();
  /* Clear this or the destructor will close them.  In the case of
     rd_proc_pipe that would be an invalid handle.  In the case of
     wr_proc_pipe it would be == my_wr_proc_pipe.  Both would be bad. */
  ch.rd_proc_pipe = ch.wr_proc_pipe = NULL;
  CloseHandle (hParent);
  hParent = NULL;
  cygwin_finished_initializing = true;
  pthread::atforkchild ();
  return 0;
}

int
frok::parent (volatile char * volatile stack_here)
{
  HANDLE forker_finished;
  DWORD rc;
  child_pid = -1;
  this_errno = 0;
  bool fix_impersonation = false;
  pinfo child;

  /* Inherit scheduling parameters by default. */
  int child_nice = myself->nice;
  int child_sched_policy = myself->sched_policy;
  int c_flags = 0;

  /* Handle SCHED_RESET_ON_FORK flag. */
  if (myself->sched_reset_on_fork)
    {
      bool batch = (myself->sched_policy == SCHED_BATCH);
      bool idle = (myself->sched_policy == SCHED_IDLE);
      bool set_prio = false;
      /* Reset negative nice values to zero. */
      if (myself->nice < 0)
	{
	  child_nice = 0;
	  set_prio = !idle;
	}
      /* Reset realtime policies to SCHED_OTHER. */
      if (!(myself->sched_policy == SCHED_OTHER || batch || idle))
	{
	  child_sched_policy = SCHED_OTHER;
	  set_prio = true;
	}
      if (set_prio)
	c_flags = nice_to_winprio (child_nice, batch);
    }

  /* Always request a priority because otherwise anything above
     NORMAL_PRIORITY_CLASS would not be inherited. */
  if (!c_flags)
    c_flags = GetPriorityClass (GetCurrentProcess ());
  debug_printf ("priority class %d", c_flags);
  /* Per MSDN, this must be specified even if lpEnvironment is set to NULL,
     otherwise UNICODE characters in the parent environment are not copied
     correctly to the child.  Omitting it may scramble %PATH% on non-English
     systems. */
  c_flags |= CREATE_UNICODE_ENVIRONMENT;

  errmsg = NULL;
  hchild = NULL;

  /* If we don't have a console, then don't create a console for the
     child either.  */
  HANDLE console_handle = CreateFile ("CONOUT$", GENERIC_WRITE,
				      FILE_SHARE_READ | FILE_SHARE_WRITE,
				      &sec_none_nih, OPEN_EXISTING,
				      FILE_ATTRIBUTE_NORMAL, NULL);

  if (console_handle != INVALID_HANDLE_VALUE)
    CloseHandle (console_handle);
  else
    c_flags |= DETACHED_PROCESS;

  /* Some file types (currently only sockets) need extra effort in the
     parent after CreateProcess and before copying the datastructures
     to the child. So we have to start the child in suspend state,
     unfortunately, to avoid a race condition. */
  if (cygheap->fdtab.need_fixup_before ())
    c_flags |= CREATE_SUSPENDED;

  /* Remember if we need to load dynamically linked dlls.
     We do this here so that this information will be available
     in the parent and, when the stack is copied, in the child. */
  load_dlls = dlls.reload_on_fork && dlls.loaded_dlls;

  forker_finished = CreateEvent (&sec_all, FALSE, FALSE, NULL);
  if (forker_finished == NULL)
    {
      this_errno = geterrno_from_win_error ();
      error ("unable to allocate forker_finished event");
      return -1;
    }

  ProtectHandleINH (forker_finished);

  ch.forker_finished = forker_finished;

  ch.stackbase = NtCurrentTeb ()->Tib.StackBase;
  ch.stackaddr = NtCurrentTeb ()->DeallocationStack;
  if (!ch.stackaddr)
    {
      /* If DeallocationStack is NULL, we're running on an application-provided
	 stack.  If so, the entire stack is committed anyway and StackLimit
	 points to the allocation address of the stack.  Mark in guardsize that
	 we must not set up guard pages. */
      ch.stackaddr = ch.stacklimit = NtCurrentTeb ()->Tib.StackLimit;
      ch.guardsize = (size_t) -1;
    }
  else
    {
      /* Otherwise we're running on a system-allocated stack.  Since stack_here
	 is the address of the stack pointer we start the child with anyway, we
	 can set ch.stacklimit to this value rounded down to page size.  The
	 child will not need the rest of the stack anyway.  Guardsize depends
	 on whether we're running on a pthread or not.  If pthread, we fetch
	 the guardpage size from the pthread attribs, otherwise we use the
	 system default. */
      ch.stacklimit = (void *) ((uintptr_t) stack_here & ~(wincap.page_size () - 1));
      ch.guardsize = (&_my_tls != _main_tls && _my_tls.tid)
		     ? _my_tls.tid->attr.guardsize
		     : wincap.def_guard_page_size ();
    }
  debug_printf ("stack - bottom %p, top %p, addr %p, guardsize %ly",
		ch.stackbase, ch.stacklimit, ch.stackaddr, ch.guardsize);

  PROCESS_INFORMATION pi;
  STARTUPINFOW si;

  memset (&si, 0, sizeof (si));
  si.cb = sizeof si;

  si.lpReserved2 = (LPBYTE) &ch;
  si.cbReserved2 = sizeof (ch);

  /* NEVER, EVER, call a function which in turn calls malloc&friends while this
     malloc lock is active! */
  __malloc_lock ();
  bool locked = true;

  /* Remove impersonation */
  cygheap->user.deimpersonate ();
  fix_impersonation = true;
  ch.refresh_cygheap ();
  ch.prefork ();	/* set up process tracking pipes. */

  *with_forkables = dlls.setup_forkables (*with_forkables);

  ch.silentfail (!*with_forkables); /* fail silently without forkables */

  PSECURITY_ATTRIBUTES sa = (PSECURITY_ATTRIBUTES) alloca (1024);
  if (!sec_user_nih (sa, cygheap->user.saved_sid (),
		     well_known_authenticated_users_sid,
		     PROCESS_QUERY_LIMITED_INFORMATION))
    sa = &sec_none_nih;

  while (1)
    {
      PCWCHAR forking_progname = NULL;
      if (dlls.main_executable)
        forking_progname = dll_list::buffered_shortname
			   (dlls.main_executable->forkedntname ());
      if (!forking_progname || !*forking_progname)
	forking_progname = myself->progname;

      syscall_printf ("CreateProcessW (%W, %W, 0, 0, 1, %y, 0, 0, %p, %p)",
		      forking_progname, myself->progname, c_flags, &si, &pi);

      hchild = NULL;
      /* cygwin1.dll may reuse the forking_progname buffer, even
	 in case of failure: don't reuse forking_progname later */
      rc = CreateProcessW (forking_progname,	/* image to run */
			   GetCommandLineW (),	/* Take same space for command
						   line as in parent to make
						   sure child stack is allocated
						   in the same memory location
						   as in parent. */
			   sa,
			   sa,
			   TRUE,		/* inherit handles */
			   c_flags,
			   NULL,		/* environ filled in later */
			   0,			/* use cwd */
			   &si,
			   &pi);

      if (rc)
	debug_printf ("forked pid %u", pi.dwProcessId);
      else
	{
	  this_errno = geterrno_from_win_error ();
	  error ("CreateProcessW failed for '%W'", myself->progname);
	  dlls.release_forkables ();
	  memset (&pi, 0, sizeof (pi));
	  goto cleanup;
	}

      if (cygheap->fdtab.need_fixup_before ())
	{
	  cygheap->fdtab.fixup_before_fork (pi.dwProcessId);
	  ResumeThread (pi.hThread);
	}

      CloseHandle (pi.hThread);
      hchild = pi.hProcess;

      dlls.release_forkables ();

      /* Protect the handle but name it similarly to the way it will
	 be called in subproc handling. */
      ProtectHandle1 (hchild, childhProc);

      strace.write_childpid (pi.dwProcessId);

      /* Wait for subproc to initialize itself. */
      if (!ch.sync (pi.dwProcessId, hchild, FORK_WAIT_TIMEOUT))
	{
	  if (!error ("forked process %u died unexpectedly, retry %d, exit code %y",
		      pi.dwProcessId, ch.retry, ch.exit_code))
	    continue;
	  this_errno = EAGAIN;
	  goto cleanup;
	}
      break;
    }

  /* Restore impersonation */
  cygheap->user.reimpersonate ();
  fix_impersonation = false;

  child_pid = cygwin_pid (pi.dwProcessId);
  child.init (child_pid, PID_IN_USE | PID_NEW, NULL);

  if (!child)
    {
      this_errno = get_errno () == ENOMEM ? ENOMEM : EAGAIN;
      syscall_printf ("pinfo failed");
      goto cleanup;
    }

  child->nice = child_nice;
  child->sched_policy = child_sched_policy;
  child->sched_reset_on_fork = false;

  /* Initialize things that are done later in dll_crt0_1 that aren't done
     for the forkee.  */
  wcscpy (child->progname, myself->progname);

  /* Fill in fields in the child's process table entry.  */
  child->dwProcessId = pi.dwProcessId;
  child.hProcess = hchild;
  ch.postfork (child);

  /* Hopefully, this will succeed.  The alternative to doing things this
     way is to reserve space prior to calling CreateProcess and then fill
     it in afterwards.  This requires more bookkeeping than I like, though,
     so we'll just do it the easy way.  So, terminate any child process if
     we can't actually record the pid in the internal table. */
  if (!child.remember ())
    {
      this_errno = EAGAIN;
#ifdef DEBUGGING0
      error ("child remember failed");
#endif
      goto cleanup;
    }

  /* CHILD IS STOPPED */
  debug_printf ("child is alive (but stopped)");


  /* Initialize, in order: stack, dll data, dll bss.
     data, bss, heap were done earlier (in dcrt0.cc)
     Note: variables marked as NO_COPY will not be copied since they are
     placed in a protected segment.  */

  const void *impure_beg;
  const void *impure_end;
  const char *impure;
  if (&_my_tls == _main_tls)
    impure_beg = impure_end = impure = NULL;
  else
    {
      impure = "impure";
      impure_beg = _impure_ptr;
      impure_end = _impure_ptr + 1;
    }
  rc = child_copy (hchild, true, !*with_forkables,
		   "stack", stack_here, ch.stackbase,
		   impure, impure_beg, impure_end,
		   NULL);

  __malloc_unlock ();
  locked = false;
  if (!rc)
    {
      this_errno = get_errno ();
      error ("pid %u, exitval %p", pi.dwProcessId, ch.exit_code);
      goto cleanup;
    }

  /* Now fill data/bss of any DLLs that were linked into the program. */
  for (dll *d = dlls.istart (DLL_LINK); d; d = dlls.inext ())
    {
      debug_printf ("copying data/bss of a linked dll");
      if (!child_copy (hchild, true, !*with_forkables,
		       "linked dll data", d->p.data_start, d->p.data_end,
		       "linked dll bss", d->p.bss_start, d->p.bss_end,
		       NULL))
	{
	  this_errno = get_errno ();
	  error ("couldn't copy linked dll data/bss");
	  goto cleanup;
	}
    }

  /* Start the child up, and then wait for it to
     perform fork fixups and dynamic dll loading (if any). */
  resume_child (forker_finished);
  if (!ch.sync (child->pid, hchild, FORK_WAIT_TIMEOUT))
    {
      this_errno = EAGAIN;
      error ("died waiting for dll loading");
      goto cleanup;
    }

  /* If DLLs were loaded in the parent, then the child has reloaded all
     of them and is now waiting to have all of the individual data and
     bss sections filled in. */
  if (load_dlls)
    {
      /* CHILD IS STOPPED */
      /* write memory of reloaded dlls */
      for (dll *d = dlls.istart (DLL_LOAD); d; d = dlls.inext ())
	{
	  debug_printf ("copying data/bss for a loaded dll");
	  if (!child_copy (hchild, true, !*with_forkables,
			   "loaded dll data", d->p.data_start, d->p.data_end,
			   "loaded dll bss", d->p.bss_start, d->p.bss_end,
			   NULL))
	    {
	      this_errno = get_errno ();
#ifdef DEBUGGING
	      error ("copying data/bss for a loaded dll");
#endif
	      goto cleanup;
	    }
	}
    }

  /* Do not attach to the child before it has successfully initialized.
     Otherwise we may wait forever, or deliver an orphan SIGCHILD. */
  if (!child.attach ())
    {
      this_errno = EAGAIN;
#ifdef DEBUGGING0
      error ("child attach failed");
#endif
      goto cleanup;
    }

  /* Finally start the child up. */
  resume_child (forker_finished);

  ForceCloseHandle (forker_finished);
  forker_finished = NULL;

  return child_pid;

/* Common cleanup code for failure cases */
cleanup:
  /* release procinfo before hProcess in destructor */
  child.allow_remove ();

  if (fix_impersonation)
    cygheap->user.reimpersonate ();
  if (locked)
    __malloc_unlock ();

  /* Remember to de-allocate the fd table. */
  if (hchild)
    {
      TerminateProcess (hchild, 1);
      if (!child.hProcess) /* no child.procinfo */
	ForceCloseHandle1 (hchild, childhProc);
    }
  if (forker_finished)
    ForceCloseHandle (forker_finished);
  debug_printf ("returning -1");
  return -1;
}

extern "C" int
fork ()
{
  /* Try RtlCloneUserProcess if enabled and available */
  if (use_rtlclone_fork ())
    {
      /* Call pthread fork-prepare handlers to save state (e.g., semaphore
	 values) before the clone.  This mirrors what hold_everything does
	 for legacy fork via lock_pthread. */
      pthread::atforkprepare ();

      int res = dofork_rtlclone (NULL);

      if (res > 0)
	{
	  /* Parent: call fork-parent handlers */
	  pthread::atforkparent ();
	  return res;
	}
      else if (res == 0)
	{
	  /* Child: atforkchild() is called in dofork_rtlclone */
	  return 0;
	}
      else
	{
	  /* Error: still need to call parent handlers to release locks */
	  pthread::atforkparent ();
	  if (fork_mode == FORK_rtlclone)
	    return res;
	  /* In auto mode, fallback to legacy on failure */
	}
    }

  /* Legacy CreateProcess-based fork */
  bool with_forkables = false; /* do not force hardlinks on first try */
  int res = dofork (NULL, &with_forkables);
  if (res >= 0)
    return res;
  if (with_forkables)
    return res; /* no need for second try when already enabled */
  with_forkables = true; /* enable hardlinks for second try */
  return dofork (NULL, &with_forkables);
}


/* __posix_spawn_fork is called from newlib's posix_spawn implementation.
   The original code in newlib has been taken from FreeBSD, and the core
   code relies on specific, non-portable behaviour of vfork(2).  Our
   replacement implementation needs the forked child's HANDLE for
   synchronization, so __posix_spawn_fork returns it in proc. */
extern "C" int
__posix_spawn_fork (void **proc)
{
  /* Try RtlCloneUserProcess if enabled and available */
  if (use_rtlclone_fork ())
    {
      /* Call pthread fork-prepare handlers to save state */
      pthread::atforkprepare ();

      int res = dofork_rtlclone (proc);

      if (res > 0)
	{
	  /* Parent: call fork-parent handlers */
	  pthread::atforkparent ();
	  return res;
	}
      else if (res == 0)
	{
	  /* Child: atforkchild() is called in dofork_rtlclone */
	  return 0;
	}
      else
	{
	  /* Error: still need to call parent handlers to release locks */
	  pthread::atforkparent ();
	  if (fork_mode == FORK_rtlclone)
	    return res;
	  /* In auto mode, fallback to legacy on failure */
	  debug_printf ("RtlCloneUserProcess failed, falling back to legacy fork");
	}
    }

  /* Legacy CreateProcess-based fork */
  bool with_forkables = false; /* do not force hardlinks on first try */
  int res = dofork (proc, &with_forkables);
  if (res >= 0)
    return res;
  if (with_forkables)
    return res; /* no need for second try when already enabled */
  with_forkables = true; /* enable hardlinks for second try */
  return dofork (proc, &with_forkables);
}

static int
dofork (void **proc, bool *with_forkables)
{
  frok grouped (with_forkables);

  debug_printf ("entering");
  grouped.load_dlls = 0;

  int res;
  bool ischild = false;

  myself->set_has_pgid_children ();

  if (grouped.ch.parent == NULL)
    return -1;
  if (grouped.ch.subproc_ready == NULL)
    {
      system_printf ("unable to allocate subproc_ready event, %E");
      return -1;
    }

  {
    hold_everything held_everything (ischild);
    /* This tmp_pathbuf constructor is required here because the below setjmp
       magic will otherwise not restore the original buffer count values in
       the thread-local storage.  A process forking too deeply will run into
       the problem to be out of temporary TLS path buffers. */
    tmp_pathbuf tp;

    if (!held_everything)
      {
	if (exit_state)
	  Sleep (INFINITE);
	set_errno (EAGAIN);
	return -1;
      }

    /* Put the dll list in topological dependency ordering, in
       hopes that the child will have a better shot at loading dlls
       properly if it only has to deal with one at a time.  */
    dlls.topsort ();

    ischild = !!setjmp (grouped.ch.jmp);

    volatile char * volatile stackp;
#if defined(__x86_64__)
    __asm__ volatile ("movq %%rsp,%0": "=r" (stackp));
#elif defined(__aarch64__)
    __asm__ volatile ("mov %0, sp" : "=r" (stackp));
#else
#error unimplemented for this target
#endif

    if (!ischild)
      res = grouped.parent (stackp);
    else
      {
	res = grouped.child (stackp);
	__in_forkee = FORKED;
	ischild = true;	/* might have been reset by fork mem copy */
      }
  }

  if (ischild)
    {
      InterlockedOr ((LONG *) &myself->process_state, PID_ACTIVE);
      InterlockedAnd ((LONG *) &myself->process_state,
		      ~(PID_INITIALIZING | PID_EXITED | PID_REAPED));
    }
  else if (res < 0)
    {
      if (!grouped.errmsg)
	syscall_printf ("fork failed - child pid %d, errno %d", grouped.child_pid, grouped.this_errno);
      else if (grouped.ch.silentfail ())
	debug_printf ("child %d - %s, errno %d", grouped.child_pid,
		       grouped.errmsg, grouped.this_errno);
      else
	system_printf ("child %d - %s, errno %d", grouped.child_pid,
		       grouped.errmsg, grouped.this_errno);

      set_errno (grouped.this_errno);
    }
  else if (proc)
    {
      /* Return child process handle to posix_fork. */
      *proc = grouped.hchild;
    }
  syscall_printf ("%R = fork()", res);
  return res;
}
#ifdef DEBUGGING
void
fork_init ()
{
}
#endif /*DEBUGGING*/

/*
 * RtlCloneUserProcess-based fork implementation.
 *
 * This uses the undocumented NT API RtlCloneUserProcess which creates a
 * copy-on-write clone of the current process, similar to Unix fork().
 *
 * Advantages over legacy CreateProcess + WriteProcessMemory approach:
 * - True copy-on-write semantics (faster, more efficient)
 * - Memory is already duplicated via COW, no need to copy explicitly
 * - Handles are inherited automatically
 *
 * Critical requirements:
 * - Must reinitialize ALL locks before any allocations in child
 * - Must reinitialize signal infrastructure in child (sigproc_init)
 * - Must set up process tracking pipe for parent-child communication
 * - WoW64 (32-bit on 64-bit) is not supported, must fallback to legacy
 *
 * Returns: child pid in parent, 0 in child, -1 on error
 */

/* Global to pass wr_proc_pipe to child via COW.  This is set by the parent
   BEFORE calling RtlCloneUserProcess, so the child inherits both the value
   (via COW memory) and the handle (via RTL_CLONE_PROCESS_FLAGS_INHERIT_HANDLES).
   The child then copies this to my_wr_proc_pipe.  This must not be NO_COPY
   since we rely on COW to pass the value. */
static HANDLE rtlclone_wr_proc_pipe;

/* Global to pass parent process handle to child via COW.  The child needs
   this to duplicate handles that weren't marked inheritable.  Created by
   duplicating GetCurrentProcess() with DUPLICATE_SAME_ACCESS and making
   it inheritable. */
static HANDLE rtlclone_parent_handle;

/* Flag indicating RtlClone child is performing handle fixup.
   When true, fork_fixup should duplicate ALL handles from parent. */
bool rtlclone_fixup_in_progress;


static int
dofork_rtlclone (void **proc)
{
  RTL_USER_PROCESS_INFORMATION process_info;
  NTSTATUS status;
  HANDLE rd_proc_pipe = NULL;
  HANDLE wr_proc_pipe = NULL;

  debug_printf ("attempting RtlCloneUserProcess fork");

  /* WoW64 (32-bit process on 64-bit Windows) is not supported.
     RtlCloneUserProcess has known issues on WoW64. */
  if (wincap.host_machine () != wincap.cygwin_machine ())
    {
      debug_printf ("WoW64 detected, RtlCloneUserProcess not supported");
      set_errno (ENOSYS);
      return -1;
    }

  myself->set_has_pgid_children ();

  /* Create process tracking pipe BEFORE cloning so child inherits wr_proc_pipe.
     This is similar to what child_info::prefork() does for legacy fork.
     The pipe is used by the parent to track child state (via proc_waiter thread)
     and by the child to notify parent of state changes (via alert_parent). */
  if (!CreatePipe (&rd_proc_pipe, &wr_proc_pipe, &sec_none_nih, 16))
    {
      __seterrno ();
      debug_printf ("CreatePipe for proc tracking failed, %E");
      return -1;
    }

  /* Make wr_proc_pipe inheritable so child gets it via RtlClone's handle
     inheritance.  rd_proc_pipe stays non-inheritable (parent only). */
  if (!SetHandleInformation (wr_proc_pipe, HANDLE_FLAG_INHERIT,
			     HANDLE_FLAG_INHERIT))
    {
      __seterrno ();
      debug_printf ("SetHandleInformation for wr_proc_pipe failed, %E");
      CloseHandle (rd_proc_pipe);
      CloseHandle (wr_proc_pipe);
      return -1;
    }

  /* Store wr_proc_pipe in global so child can retrieve it via COW */
  rtlclone_wr_proc_pipe = wr_proc_pipe;

  /* Create an inheritable handle to the current (parent) process.  The child
     needs this to duplicate handles that weren't marked inheritable via
     fork_fixup -> DuplicateHandle.  Without this, handles with close_on_exec
     set would fail to be duplicated in the child. */
  HANDLE parent_handle = NULL;
  if (!DuplicateHandle (GetCurrentProcess (), GetCurrentProcess (),
			GetCurrentProcess (), &parent_handle,
			0, TRUE /* inheritable */, DUPLICATE_SAME_ACCESS))
    {
      __seterrno ();
      debug_printf ("DuplicateHandle for parent_handle failed, %E");
      CloseHandle (rd_proc_pipe);
      CloseHandle (wr_proc_pipe);
      return -1;
    }
  rtlclone_parent_handle = parent_handle;

  /* Initialize process_info structure */
  memset (&process_info, 0, sizeof (process_info));
  process_info.Length = sizeof (process_info);

  /* Perform the clone.  Create child suspended so parent can set up pinfo
     and winpid symlink before child runs and tries to find itself. */
  status = RtlCloneUserProcess (RTL_CLONE_PROCESS_FLAGS_CREATE_SUSPENDED
				| RTL_CLONE_PROCESS_FLAGS_INHERIT_HANDLES,
				NULL, NULL, NULL, &process_info);
  {
    char buf[128];
    __small_sprintf (buf, "RtlCloneUserProcess returned status=0x%x", status);
  }

  /* Check if function is unavailable (autoload stub returned error).
     This happens on older Windows versions that don't have this function.
     The autoload mechanism sets GetLastError to ERROR_PROC_NOT_FOUND. */
  if (GetLastError () == ERROR_PROC_NOT_FOUND)
    {
      debug_printf ("RtlCloneUserProcess not available (ERROR_PROC_NOT_FOUND)");
      CloseHandle (rd_proc_pipe);
      CloseHandle (wr_proc_pipe);
      CloseHandle (parent_handle);
      set_errno (ENOSYS);
      return -1;
    }

  if (status == STATUS_PROCESS_CLONED)
    {
      /* === CHILD PROCESS === */

      /* rd_proc_pipe is parent's handle - not inherited (wasn't marked
	 inheritable).  The variable value is copied via COW but the handle
	 doesn't exist in child's handle table.  Just clear the variable. */
      rd_proc_pipe = NULL;

      /* CRITICAL: Reinitialize ALL locks FIRST, before any allocations
	 or operations that might acquire a lock.  This is necessary because
	 the parent process may have had threads holding locks when we cloned,
	 and those locks are now in an undefined state in the child. */
      reinit_all_locks_after_clone ();

      /* Set up process tracking pipe.  Close any leftover pipe from a previous
	 fork (shouldn't happen but be safe), then set my_wr_proc_pipe from
	 the value we stored in rtlclone_wr_proc_pipe before the clone. */
      if (my_wr_proc_pipe)
	ForceCloseHandle1 (my_wr_proc_pipe, wr_proc_pipe);
      my_wr_proc_pipe = rtlclone_wr_proc_pipe;
      rtlclone_wr_proc_pipe = NULL;

      /* Initialize signal infrastructure.  Unlike legacy fork where the child
	 goes through dll_crt0_1 and inherits signal setup via child_info,
	 RtlClone child has COW copies of parent's signal pipes which are
	 invalid in the child's handle table.  We need fresh signal pipes
	 and a new wait_sig thread. */
      sigproc_init ();

      /* Find our own pinfo.  The parent already created it for us using our
	 Windows PID.  We need to update 'myself' to point to our own pinfo
	 instead of the parent's (which we inherited via copy-on-write). */
      DWORD wpid = GetCurrentProcessId ();
      {
	char buf[128];
	__small_sprintf (buf, "child: wpid=%lu, calling cygwin_pid", (unsigned long)wpid);
      }
      pid_t child_pid = cygwin_pid (wpid);
      {
	char buf[128];
	__small_sprintf (buf, "child: cygwin_pid returned %d", child_pid);
      }
      if (child_pid)
	{
	  cygheap->pid = child_pid;
	  myself.init (child_pid, PID_IN_USE, NULL);
	}
      else
	{
	}

      /* Now we can safely do minimal child initialization */
      debug_printf ("child: RtlCloneUserProcess returned STATUS_PROCESS_CLONED, pid %d", child_pid);

      /* Make sure threadinfo is properly set up */
      if (&_my_tls != _main_tls)
	{
	  _main_tls = &_my_tls;
	  _main_tls->init_thread (NULL, NULL);
	}

      /* Set up privileges */
      set_cygwin_privileges (hProcToken);
      clear_procimptoken ();
      cygheap->user.reimpersonate ();

      /* Fix up shared memory areas */
      if (fixup_shms_after_fork ())
	api_fatal ("fixup_shms_after_fork failed in RtlCloneUserProcess child");

      /* DLLs are already loaded via COW, just need to fixup bookkeeping.
	 Unlike legacy fork, we don't need to reload DLLs - they're already
	 mapped at the same addresses due to copy-on-write. */
      /* Note: dlls.load_after_fork is not needed here since DLLs are
	 already in memory. However, we may need to reinitialize DLL-specific
	 state if any DLLs have fork callbacks. */

      /* Fix up file descriptor table.  Use parent_handle (inherited via COW)
	 to duplicate ALL handles from parent.  Set rtlclone_fixup_in_progress
	 flag so fork_fixup knows to duplicate regardless of close_on_exec. */
      rtlclone_fixup_in_progress = true;
      cygheap->fdtab.fixup_after_fork (rtlclone_parent_handle);
      rtlclone_fixup_in_progress = false;

      /* Close the parent handle - we're done duplicating handles */
      if (rtlclone_parent_handle)
	{
	  CloseHandle (rtlclone_parent_handle);
	  rtlclone_parent_handle = NULL;
	}

      /* Additional fixups */
      fixup_hooks_after_fork ();
      _my_tls.fixup_after_fork ();
      ld_preload ();
      pthread::atforkchild ();

      /* Mark as initialized */
      cygwin_finished_initializing = true;
      __in_forkee = FORKED;

      /* Mark process as active */
      InterlockedOr ((LONG *) &myself->process_state, PID_ACTIVE);
      InterlockedAnd ((LONG *) &myself->process_state,
		      ~(PID_INITIALIZING | PID_EXITED | PID_REAPED));

      syscall_printf ("0 = fork() [child via RtlCloneUserProcess]");
      return 0;
    }
  else if (NT_SUCCESS (status))
    {
      /* === PARENT PROCESS === */
      pid_t child_pid;
      pinfo child;
      DWORD child_wpid = (DWORD) (uintptr_t) process_info.ClientId.UniqueProcess;

      {
	char buf[128];
	__small_sprintf (buf, "parent: child wpid=%lu", (unsigned long)child_wpid);
      }
      debug_printf ("parent: RtlCloneUserProcess returned SUCCESS, child wpid %lu",
		    (unsigned long) child_wpid);

      /* Close wr_proc_pipe - parent doesn't need it (child has it).
	 This is similar to what child_info::postfork() does. */
      ForceCloseHandle (wr_proc_pipe);
      wr_proc_pipe = NULL;
      rtlclone_wr_proc_pipe = NULL;

      /* Close parent_handle - parent doesn't need it anymore.
	 Child inherited it and will use it for handle duplication. */
      if (parent_handle)
	{
	  CloseHandle (parent_handle);
	  parent_handle = NULL;
	}
      rtlclone_parent_handle = NULL;

      /* Create a new Cygwin pid for the child.  Unlike legacy fork where
	 the child creates its own pid during DLL initialization, with
	 RtlCloneUserProcess the child is a copy of the parent and doesn't
	 go through normal DLL init.  So the parent must create the pid. */
      child_pid = create_cygwin_pid ();
      child.init (child_pid, PID_IN_USE | PID_NEW, NULL);

      if (!child)
	{
	  system_printf ("pinfo init failed for RtlCloneUserProcess child");
	  CloseHandle (rd_proc_pipe);
	  TerminateProcess (process_info.Process, 1);
	  NtClose (process_info.Process);
	  NtClose (process_info.Thread);
	  set_errno (EAGAIN);
	  return -1;
	}

      /* Set up child pinfo - must mirror what PROC_ADD_CHILD does in sigproc.cc.
	 Set dwProcessId BEFORE create_winpid_symlink since symlink uses it. */
      child->dwProcessId = child_wpid;
      child.hProcess = process_info.Process;
      child.create_winpid_symlink ();  /* Create symlink so child can find itself */
      wcscpy (child->progname, myself->progname);
      child->nice = myself->nice;
      child->sched_policy = myself->sched_policy;
      child->sched_reset_on_fork = false;
      child->uid = myself->uid;
      child->gid = myself->gid;
      child->pgid = myself->pgid;
      child->sid = myself->sid;
      child->ctty = myself->ctty;
      child->cygstarted = true;
      InterlockedOr ((LONG *) &child->process_state, PID_INITIALIZING);
      child->ppid = myself->pid;  /* always set last */

      /* Set up rd_proc_pipe so proc_waiter can track the child.
	 This is similar to what child_info::postfork() does. */
      child.set_rd_proc_pipe (rd_proc_pipe);
      rd_proc_pipe = NULL;  /* Ownership transferred to child pinfo */

      /* Register child with remember() then start tracking with attach().
	 remember() does PROC_ADD_CHILD which sets up child metadata.
	 attach() does PROC_ATTACH_CHILD which adds to chld_procs array
	 and starts the proc_waiter thread to track child via rd_proc_pipe. */
      if (!child.remember ())
	{
	  system_printf ("child remember failed for RtlCloneUserProcess child");
	  TerminateProcess (process_info.Process, 1);
	  NtClose (process_info.Process);
	  NtClose (process_info.Thread);
	  set_errno (EAGAIN);
	  return -1;
	}

      if (!child.attach ())
	{
	  system_printf ("child attach failed for RtlCloneUserProcess child");
	  /* Don't terminate - remember() succeeded so child is partially set up.
	     The child may still run, we just can't track it properly. */
	}

      /* Resume the child's main thread (it was created suspended by RtlClone) */
      ResumeThread (process_info.Thread);
      NtClose (process_info.Thread);

      /* Return process handle if requested (for posix_spawn) */
      if (proc)
	*proc = process_info.Process;

      syscall_printf ("%d = fork() [parent via RtlCloneUserProcess]", child_pid);
      return child_pid;
    }
  else
    {
      /* Clone failed */
      debug_printf ("RtlCloneUserProcess failed with status 0x%08lx",
		    (unsigned long) status);
      CloseHandle (rd_proc_pipe);
      CloseHandle (wr_proc_pipe);
      CloseHandle (parent_handle);
      __seterrno_from_nt_status (status);
      return -1;
    }
}

/* Check if RtlCloneUserProcess should be used based on fork_mode setting.
   Note: The actual availability check happens when RtlCloneUserProcess is
   called - the autoload mechanism will return an error if unavailable. */
static inline bool
use_rtlclone_fork ()
{
  /* Never use on WoW64 */
  if (wincap.host_machine () != wincap.cygwin_machine ())
    return false;

  /* Check fork_mode setting */
  switch (fork_mode)
    {
    case FORK_rtlclone:
    case FORK_auto:
      return true;
    case FORK_legacy:
    default:
      return false;
    }
}

extern "C" int
vfork ()
{
  debug_printf ("stub called");
  return fork ();
}

/* Copy memory from one process to another. */

bool
child_copy (HANDLE hp, bool write, bool silentfail, ...)
{
  va_list args;
  va_start (args, silentfail);
  static const char *huh[] = {"read", "write"};

  char *what;
  while ((what = va_arg (args, char *)))
    {
      char *low = va_arg (args, char *);
      char *high = va_arg (args, char *);
      SIZE_T todo = high - low;
      char *here;

      for (here = low; here < high; here += todo)
	{
	  SIZE_T done = 0;
	  if (here + todo > high)
	    todo = high - here;
	  BOOL res;
	  if (write)
	    res = WriteProcessMemory (hp, here, here, todo, &done);
	  else
	    res = ReadProcessMemory (hp, here, here, todo, &done);
	  debug_printf ("%s - hp %p low %p, high %p, res %d", what, hp, low, high, res);
	  if (!res || todo != done)
	    {
	      if (!res)
		__seterrno ();
	      if (silentfail)
		debug_printf ("%s %s copy failed, %p..%p, done %lu, windows pid %u, %E",
			     what, huh[write], low, high, done, myself->dwProcessId);
	      else
		/* If this happens then there is a bug in our fork
		   implementation somewhere. */
		system_printf ("%s %s copy failed, %p..%p, done %lu, windows pid %u, %E",
			      what, huh[write], low, high, done, myself->dwProcessId);
	      goto err;
	    }
	}
    }

  va_end (args);
  debug_printf ("done");
  return true;

 err:
  va_end (args);
  TerminateProcess (hp, 1);
  set_errno (EAGAIN);
  return false;
}
