## Process this file with autoconf to produce configure.
## In general, the safest way to proceed is to run the following:
##    % aclocal -I `pwd`/../autoconf && autoconf && autoheader && automake

# make sure we're interpreted by some minimal autoconf
AC_PREREQ(2.57)

AC_INIT(google-perftools, 0.91, opensource@google.com)
# The argument here is just something that should be in the current directory
# (for sanity checking)
AC_CONFIG_SRCDIR(README)
AM_INIT_AUTOMAKE
AM_CONFIG_HEADER(src/config.h)

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

AC_CHECK_TYPES([__int64])       # defined in some windows platforms
AC_CHECK_TYPES([struct mallinfo],,, [#include <malloc.h>])
AC_CHECK_FUNCS(sbrk)            # for tcmalloc to get memory
AC_CHECK_FUNCS(munmap)
AC_FUNC_MMAP
AC_CHECK_HEADERS(execinfo.h)    # for stacktrace? and heapchecker_unittest
AC_CHECK_HEADERS(libunwind.h)   # for stacktrace
AC_CHECK_HEADERS(unwind.h)      # for stacktrace
AC_CHECK_HEADERS(conflict-signal.h)      # defined on some windows platforms
AC_CHECK_HEADERS(linux/ptrace.h)
AC_CHECK_HEADERS(syscall.h)
AC_CHECK_HEADERS(grp.h)         # for heapchecker_unittest
AC_CHECK_HEADERS(pwd.h)         # for heapchecker_unittest
AC_CHECK_MEMBERS([struct sigcontext.sc_eip,
                  struct ucontext.uc_mcontext,
                  struct sigcontext.eip,
                  struct sigcontext.rip,
                  struct sigcontext.sc_ip,
                  struct siginfo.si_faddr,
		  struct sigcontext.regs->nip],,,
                 [#define _XOPEN_SOURCE 500
                  #include <signal.h>])

# We want to link in libunwind if it exists
AC_CHECK_LIB(unwind, backtrace, UNWIND_LIBS=-lunwind, UNWIND_LIBS=)
AC_SUBST(UNWIND_LIBS)

# On x86_64, instead of libunwind, we can choose to compile with frame-pointers
# (This isn't needed on i386, where -fno-omit-frame-pointer is the default).
AC_ARG_ENABLE(frame_pointer,
              AS_HELP_STRING([--enable-frame-pointer],
                             [On x86_64 systems, compile with -fno-omit-frame-pointer (see INSTALL)]),
	      , enable_frame_pointer=no)
AC_COMPILE_IFELSE([AC_LANG_PROGRAM(, [return __x86_64__ == 1 ? 0 : 1])],
                  [is_x86_64=yes], [is_x86_64=no])
EXTRA_CXXFLAGS=
if test "$is_x86_64" = yes -a "$enable_frame_pointer" = "yes"; then
  EXTRA_CXXFLAGS=-fno-omit-frame-pointer
elif test "$is_x86_64" = yes -a "$enable_frame_pointer" != "yes"; then
  # TODO(csilvers): set -DNO_FRAME_POINTER in the Makefile instead, based
  #                 on the presence of -f[no-]omit-frame-pointer in cxxflags.
  EXTRA_CXXFLAGS=-DNO_FRAME_POINTER
fi
AC_SUBST(EXTRA_CXXFLAGS)

# Defines PRIuS
AC_COMPILER_CHARACTERISTICS

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

AC_CXX_MAKE_HASH_SET_H(src/google/perftools/hash_set.h)

# Figure out where libc has program_invocation_name
AC_PROGRAM_INVOCATION_NAME

# Make the install prefix available, to figure out where to look for pprof
AC_INSTALL_PREFIX

# Write generated configuration file
AC_CONFIG_FILES([Makefile])
AC_OUTPUT
