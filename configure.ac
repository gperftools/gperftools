## Process this file with autoconf to produce configure.
## In general, the safest way to proceed is to run the following:
##    % aclocal -I `pwd`/../autoconf && autoconf && autoheader && automake

# make sure we're interpreted by some minimal autoconf
AC_PREREQ(2.57)

AC_INIT(google-perftools, 0.1, opensource@google.com)
# The argument here is just something that should be in the current directory
# (for sanity checking)
AC_CONFIG_SRCDIR(README)
AM_INIT_AUTOMAKE
AM_CONFIG_HEADER(src/google/perftools/config.h)

# Checks for programs.
AC_PROG_CC
AC_PROG_CPP
AC_PROG_CXX

AC_PROG_LIBTOOL
AC_SUBST(LIBTOOL_DEPS)

AC_C_INLINE
AX_C___ATTRIBUTE__

# Check whether some low-level functions/files are available
AC_HEADER_STDC

# Here are some examples of how to check for the existence of a fn or file
AC_CHECK_TYPES([__int64])       # defined in some windows platforms
AC_CHECK_FUNCS(sbrk)            # for tcmalloc to get memory
AC_CHECK_FUNCS(munmap)
AC_FUNC_MMAP
AC_CHECK_HEADERS(execinfo.h)    # for stacktrace?
AC_CHECK_HEADERS(conflict-signal.h)      # defined on some windows platforms
AC_CHECK_MEMBERS([struct sigcontext.sc_eip,           # for the cpu-profiler
                  struct ucontext.uc_mcontext,
                  struct sigcontext.eip,
                  struct sigcontext.sc_ip,
                  struct siginfo.si_faddr],,,
                 [#include <signal.h>])

# Defines PRIuS
AC_COMPILER_CHARACTERISTICS

# A lot of the code in this directory depends on pthreads
ACX_PTHREAD

# Find out what namespace 'normal' STL code lives in
AC_CXX_STL_NAMESPACE

AC_CXX_MAKE_HASH_SET_H(src/google/perftools/hash_set.h)

# Figure out where libc has program_invocation_name
AC_PROGRAM_INVOCATION_NAME

# Make the install prefix available, to figure out where to look for pprof
AC_INSTALL_PREFIX

# Write generated configuration file
AC_CONFIG_FILES([Makefile])
AC_OUTPUT
