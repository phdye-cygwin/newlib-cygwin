/* lock_reinit.h - Lock reinitialization after RtlCloneUserProcess

   This file is part of Cygwin.

   This software is a copyrighted work licensed under the terms of the
   Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
   details. */

#pragma once

/* After RtlCloneUserProcess, all SRWLOCKs in the child process are in
   an unknown state because they were cloned via copy-on-write from a
   parent that may have had threads holding locks.  These locks MUST be
   reinitialized to SRWLOCK_INIT before ANY code that might try to
   acquire them, including malloc().

   This is CRITICAL: reinitialization must happen before any allocations
   or logging, as those operations may attempt to acquire locks.

   Each subsystem provides a reinit function that resets its locks.  */

/* Master reinit function - calls all subsystem reinit functions.
   Must be called as the FIRST thing in the child after RtlCloneUserProcess
   returns STATUS_PROCESS_CLONED, before any other Cygwin code executes. */
void reinit_all_locks_after_clone ();

/* Individual subsystem reinit functions - C++ */
void malloc_reinit_lock_after_clone ();      /* mm/malloc_wrapper.cc */
void cygheap_reinit_lock_after_clone ();     /* mm/cygheap.cc */
void tls_sentry_reinit_lock_after_clone ();  /* mm/cygheap.cc */
void mmap_reinit_lock_after_clone ();        /* mm/mmap.cc */
void cwdstuff_reinit_lock_after_clone ();    /* path.cc */
void shm_reinit_lock_after_clone ();         /* shm.cc */
void select_reinit_lock_after_clone ();      /* select.cc */
void sec_reinit_locks_after_clone ();        /* sec/helper.cc */
void random_reinit_lock_after_clone ();      /* random.cc */
void clock_reinit_lock_after_clone ();       /* clock.cc */
void debug_reinit_lock_after_clone ();       /* debug.cc */
void sigproc_reinit_lock_after_clone ();     /* sigproc.cc */
void fhandler_reinit_lock_after_clone ();    /* fhandler/base.cc */
void dll_reinit_lock_after_clone ();         /* dll_init.cc */
void posix_timer_reinit_lock_after_clone (); /* posix_timer.cc */
void shared_reinit_after_clone ();           /* mm/shared.cc - handle reset */

/* C function - needs extern "C" linkage */
extern "C" void tzset_reinit_lock_after_clone (); /* tzcode/localtime_wrapper.c */

/* RtlClone handle fixup flag - fork_fixup must DuplicateHandle ALL handles */
extern bool rtlclone_fixup_in_progress;

/* NO_COPY state reinitialization for RtlClone child */
void sigq_clear_after_clone ();              /* sigproc.cc */
void sigproc_set_sendsig_on_pinfo ();        /* sigproc.cc */
void itimer_fixup_after_rtlclone ();         /* posix_timer.cc */
