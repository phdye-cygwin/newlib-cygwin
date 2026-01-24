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
void posix_timer_reinit_lock_after_clone (); /* posix_timer.cc */
void shared_reinit_after_clone ();           /* mm/shared.cc - handle reset */

/* C function - needs extern "C" linkage */
extern "C" void tzset_reinit_lock_after_clone (); /* tzcode/localtime_wrapper.c */

/* Flag indicating that the child process is performing RtlClone fixup.
   When true, fork_fixup should duplicate ALL handles from the parent,
   not just those with close_on_exec set.  This is needed because
   RTL_CLONE_PROCESS_FLAGS_INHERIT_HANDLES may not work reliably for
   all handle types.  Set by dofork_rtlclone before fixup_after_fork,
   cleared after fixup is complete. */
extern bool rtlclone_fixup_in_progress;
