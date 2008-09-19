## Process this file with autoconf to produce configure.
## In general, the safest way to proceed is to run ./autogen.sh

# make sure we're interpreted by some minimal autoconf
AC_PREREQ(2.57)

AC_INIT(google-perftools, 0.99, opensource@google.com)
# The argument here is just something that should be in the current directory
# (for sanity checking)
AC_CONFIG_SRCDIR(README)
AC_CANONICAL_TARGET
AM_INIT_AUTOMAKE([dist-zip])
AM_CONFIG_HEADER(src/config.h)

# Checks for programs.
AC_PROG_CC
AC_PROG_CPP
AC_PROG_CXX
AM_CONDITIONAL(GCC, test "$GCC" = yes)   # let the Makefile know if we're gcc

AC_PROG_LIBTOOL
AC_SUBST(LIBTOOL_DEPS)

AC_C_INLINE
AX_C___ATTRIBUTE__

# Check whether some low-level functions/files are available
AC_HEADER_STDC

AC_CHECK_TYPES([__int64])       # defined in some windows platforms
AC_CHECK_TYPES([struct mallinfo],,, [#include <malloc.h>])
AC_CHECK_FUNCS(sbrk)            # for tcmalloc to get memory
AC_CHECK_FUNCS(geteuid)         # for turning off services when run as root
AC_CHECK_HEADERS(malloc.h)      # some systems define stuff there, others not
AC_CHECK_HEADERS(glob.h)        # for heap-profile-table (cleaning up profiles)
AC_CHECK_HEADERS(execinfo.h)    # for stacktrace? and heapchecker_unittest
AC_CHECK_HEADERS(libunwind.h)   # for stacktrace
AC_CHECK_HEADERS(unwind.h)      # for stacktrace
AC_CHECK_HEADERS(ucontext.h)    # for profiler.cc (cpu profiler)
AC_CHECK_HEADERS(conflict-signal.h)      # defined on some windows platforms
AC_CHECK_HEADERS(sys/prctl.h)   # for thread_lister (needed by leak-checker)
AC_CHECK_HEADERS(linux/ptrace.h)# also needed by leak-checker
AC_CHECK_HEADERS(sys/syscall.h)
AC_CHECK_HEADERS(fcntl.h)       # for tcmalloc_unittest
AC_CHECK_HEADERS(grp.h)         # for heapchecker_unittest
AC_CHECK_HEADERS(pwd.h)         # for heapchecker_unittest
AC_CHECK_HEADERS(sys/resource.h)         # for memalign_unittest.cc
# We override a lot of memory allocation routines, not all of which are
# standard.  For those the system doesn't declare, we'll declare ourselves.
AC_CHECK_DECLS([cfree,
                posix_memalign,
                memalign,
                valloc,
                pvalloc],,,
               [#define _XOPEN_SOURCE 600
                #include <stdlib.h>
                #include <malloc.h>])

# We need to check for mmap.  cygwin supports mmap, but the autoconf
# test doesn't work on cygwin:
#    http://www.cygwin.com/ml/cygwin/2002-04/msg00412.html
# This workaround comes from
#    http://cygwin.com/ml/cygwin/2004-11/msg00138.html
case "$host" in
  *-*-cygwin*) ac_cv_func_mmap_fixed_mapped=yes
               AC_DEFINE(HAVE_MMAP, 1,
                         [Define to 1 if you have a working `mmap' system call.])
               ;;
            *) AC_FUNC_MMAP
               ;;
esac

# If AtomicWord != Atomic32, we need to define two versions of all the
# atomicops functions.  If they're the same, we want to define only one.
AC_MSG_CHECKING([if int32_t is the same type as intptr_t])
AC_TRY_COMPILE([#include <stdint.h>],
	       [int32_t v1 = 0; intptr_t v2 = 0; return (&v1 - &v2)],
               [AC_DEFINE(INT32_EQUALS_INTPTR, 1,
                          Define to 1 if int32_t is equivalent to intptr_t)
                AC_MSG_RESULT([yes])],
               [AC_MSG_RESULT([no])])

# We want to access the "PC" (Program Counter) register from a struct
# ucontext.  Every system has its own way of doing that.  We try all the
# possibilities we know about.  Note REG_PC should come first (REG_RIP
# is also defined on solaris, but does the wrong thing).
AC_MSG_CHECKING([how to access the program counter from a struct ucontext])
pc_fields="           uc_mcontext.gregs[[REG_PC]]"  # Solaris x86 (32 + 64 bit)
pc_fields="$pc_fields uc_mcontext.gregs[[REG_EIP]]" # Linux (i386)
pc_fields="$pc_fields uc_mcontext.gregs[[REG_RIP]]" # Linux (x86_64)
pc_fields="$pc_fields uc_mcontext.sc_ip"            # Linux (ia64)
pc_fields="$pc_fields uc_mcontext.uc_regs->gregs[[PT_NIP]]" # Linux (ppc)
pc_fields="$pc_fields uc_mcontext.mc_eip"           # FreeBSD (i386)
pc_fields="$pc_fields uc_mcontext.mc_rip"           # FreeBSD (x86_64 [untested])
pc_fields="$pc_fields uc_mcontext->ss.eip"          # OS X (i386, <=10.4)
pc_fields="$pc_fields uc_mcontext->__ss.__eip"      # OS X (i386, >=10.5)
pc_fields="$pc_fields uc_mcontext->ss.rip"          # OS X (x86_64)
pc_fields="$pc_fields uc_mcontext->__ss.__rip"      # OS X (>=10.5 [untested])
pc_fields="$pc_fields uc_mcontext->ss.srr0"         # OS X (ppc, ppc64 [untested])
pc_fields="$pc_fields uc_mcontext->__ss.__srr0"     # OS X (>=10.5 [untested])
pc_field_found=false
for pc_field in $pc_fields; do
  if ! $pc_field_found; then
    AC_TRY_COMPILE([#define _GNU_SOURCE 1
                    #include <ucontext.h>],
                   [ucontext_t u; return u.$pc_field == 0;],
                   AC_DEFINE_UNQUOTED(PC_FROM_UCONTEXT, $pc_field,
                                      How to access the PC from a struct ucontext)
                   AC_MSG_RESULT([$pc_field])
  		 pc_field_found=true)
  fi
done
if ! $pc_field_found; then
  AC_MSG_WARN(Could not find the PC.  Will not try to compile libprofiler...)
fi
AM_CONDITIONAL(HAS_PC, test "$pc_field_found" = true)

# We want to link in libunwind if it exists
AC_CHECK_LIB(unwind, backtrace, UNWIND_LIBS=-lunwind, UNWIND_LIBS=)
AC_SUBST(UNWIND_LIBS)

# On x86_64, instead of libunwind, we can choose to compile with frame-pointers
# (This isn't needed on i386, where -fno-omit-frame-pointer is the default).
AC_ARG_ENABLE(frame_pointers,
              AS_HELP_STRING([--enable-frame-pointers],
                             [On x86_64 systems, compile with -fno-omit-frame-pointer (see INSTALL)]),
	      , enable_frame_pointers=no)
AC_COMPILE_IFELSE([AC_LANG_PROGRAM(, [return __x86_64__ == 1 ? 0 : 1])],
                  [is_x86_64=yes], [is_x86_64=no])
AM_CONDITIONAL(ENABLE_FRAME_POINTERS, test "$enable_frame_pointers" = yes)
AM_CONDITIONAL(X86_64, test "$is_x86_64" = yes)

# Defines PRIuS
AC_COMPILER_CHARACTERISTICS

# Also make sure we get standard PRI... definitions, even with glibc.
# We have to use AH_VERBATIM because we need the #ifdef guard (gcc buglet)
AH_VERBATIM([__STDC_FORMAT_MACROS],
            [/* C99 says: define this to get the PRI... macros from stdint.h */
#ifndef __STDC_FORMAT_MACROS
# define __STDC_FORMAT_MACROS 1
#endif])

# Check if __builtin_stack_pointer() is available (for elfcore.h)
AC_MSG_CHECKING([for __builtin_stack_pointer()])
AC_LINK_IFELSE([AC_LANG_PROGRAM(, [void *sp = __builtin_stack_pointer()])],
               [AC_DEFINE(HAVE_BUILTIN_STACK_POINTER, 1,
                      Define to 1 if compiler supports __builtin_stack_pointer)
                AC_MSG_RESULT([yes])],
               [AC_MSG_RESULT([no])])

# If we support __thread, that can speed up tcmalloc a bit.
AC_MSG_CHECKING([for __thread])
AC_LINK_IFELSE([AC_LANG_PROGRAM(, [static __thread int p = 0])],
               [AC_DEFINE(HAVE_TLS, 1,
                      Define to 1 if compiler supports __thread)
                AC_MSG_RESULT([yes])],
               [AC_MSG_RESULT([no])])

# We also need to check if the kernel supports __thread, which requires uname()
AC_CHECK_DECLS(uname,,, [#include <sys/utsname.h>])

# In fact, a lot of the code in this directory depends on pthreads
ACX_PTHREAD

# Find out what namespace 'normal' STL code lives in
AC_CXX_STL_NAMESPACE

# Figure out where hash_set lives, and what namespace hash code is in.
AC_CXX_STL_HASH

# Figure out where libc has program_invocation_name
AC_PROGRAM_INVOCATION_NAME

# Make the install prefix available, to figure out where to look for pprof
AC_INSTALL_PREFIX

# For windows, this has a non-trivial value (__declspec(export)), but any
# system that uses configure wants this to be the empty string.
AC_DEFINE(PERFTOOLS_DLL_DECL,,
          [Always the empty-string on non-windows systems.
           On windows, should be "__declspec(dllexport)".
	   This way, when we compile the dll, we export our functions/classes.
	   It's safe to define this here because config.h is only used
	   internally, to compile the DLL, and every DLL source file
	   #includes "config.h" before anything else.])

# MinGW uses autoconf, but also needs the windows shim routines
# (since it doesn't have its own support for, say, pthreads).
# This requires us to #include a special header file, and also to
# link in some windows versions of .o's instead of the unix versions.
AH_BOTTOM([
#ifdef __MINGW32__
#include "windows/mingw.h"
#endif
])
AM_CONDITIONAL(MINGW, expr $target : '.*-mingw' >/dev/null 2>&1)

# Write generated configuration file
AC_CONFIG_FILES([Makefile])
AC_OUTPUT
