// SPDX-License-Identifier: 0BSD
#ifndef AW_ADDRCHECK_H_
#define AW_ADDRCHECK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * Address Check Lookup Session
 *
 * This module provides an async-signal-safe way to parse and search
 * /proc/self/maps. It is intended for use in backtracers and as a
 * fallback for dynamic linker object lookups.
 *
 * It also takes advantage of newer PROCMAP_QUERY Linux kernel API
 * when available. Performance is only great in that case.
 */

typedef struct aw_addrcheck_session aw_addrcheck_session_t;

struct aw_addrcheck_entry {
	uintptr_t start;
	uintptr_t end;
	unsigned char perm_read : 1;
	unsigned char perm_write : 1;
	unsigned char perm_exec : 1;
	unsigned char perm_priv : 1;
};

// aw_addrcheck_initialize needs to be called once by the process
// preferably early in its lifetime. The rest of API can still be
// used prior to calling this.
void aw_addrcheck_initialize(void);

/**
 * aw_addrcheck_open, "opens" aw_addrcheck_session_t. Unless PROCMAP_QUERY is
 * available it snapshots /proc/self/maps into a searchable session.
 *
 * If 'buffer' is non-NULL, we're encouraged to use it to save mmap()
 * syscalls. We might still invoke mmap() if the buffer isn't big
 * enough. Note, we use direct syscalls to mmap to bypass any possible
 * mmap interceptors (e.g. asan, heap checker etc).
 *
 * This function is async-signal-safe. Returns NULL on failure. But do
 * note, that a single maps sessions should only be used by a single
 * thread. Also due to snapshotting and caching effect, its lifetime
 * should typically be short. Also maps sessions are not to be kept
 * across process forks.
 */
aw_addrcheck_session_t *aw_addrcheck_open(void *buffer, size_t buffer_size);

/**
 * aw_addrcheck_open_file: Same as aw_addrcheck_open but takes an explicit path.
 * Primarily for testing with synthetic maps files.
 */
aw_addrcheck_session_t *aw_addrcheck_open_file(const char *path);

/**
 * aw_addrcheck_lookup: O(log N) binary search for 'addr'.  Returns 1
 * on success (populating 'out'), 0 if not found. Passing NULL for s
 * is safe (automatically "fails" returning 0).
 *
 * This function is async-signal-safe.
 */
int aw_addrcheck_lookup(aw_addrcheck_session_t *s, uintptr_t addr,
			struct aw_addrcheck_entry *out);

/**
 * aw_addrcheck_free: Closes the session and releases memory. Passing
 * NULL is safe.
 */
void aw_addrcheck_free(aw_addrcheck_session_t *s);

// test-only
void aw_addrcheck_set_disable_ioctl_for_test(bool disable_ioctl);

#if defined(__cplusplus)
}  // extern "C"
#endif

#endif /* AW_ADDRCHECK_H_ */
