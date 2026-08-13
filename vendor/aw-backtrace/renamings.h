#ifndef RENAMINGS_H_
#define RENAMINGS_H_

// This declares possibly prefixed name for all symbols in the
// aw-backtrace library. In case something (e.g. gperftools) wants to
// link it statically without risk of conflicts with other users.

#ifndef AW_RENAME_PREFIX
#define AW_RENAME(n) n
#else
#define AW_RENAME_APPLY2(a,b) a##b
#define AW_RENAME_APPLY(a,b) AW_RENAME_APPLY2(a,b)
#define AW_RENAME(n) AW_RENAME_APPLY(AW_RENAME_PREFIX, n)
#endif

#define aw_addrcheck_free AW_RENAME(aw_addrcheck_free)
#define aw_addrcheck_initialize AW_RENAME(aw_addrcheck_initialize)
#define aw_addrcheck_lookup AW_RENAME(aw_addrcheck_lookup)
#define aw_addrcheck_open AW_RENAME(aw_addrcheck_open)
#define aw_addrcheck_open_file AW_RENAME(aw_addrcheck_open_file)
#define aw_addrcheck_set_disable_ioctl_for_test AW_RENAME(aw_addrcheck_set_disable_ioctl_for_test)

#define aw_backtrace_internal AW_RENAME(aw_backtrace_internal)
#define aw_backtrace_ext AW_RENAME(aw_backtrace_ext)

#define aw_backtrace_full AW_RENAME(aw_backtrace_full)
#define aw_backtrace AW_RENAME(aw_backtrace)

#endif  // RENAMINGS_H_
