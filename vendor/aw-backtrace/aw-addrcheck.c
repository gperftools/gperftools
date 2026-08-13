/* -*- Mode: C; c-basic-offset: 8; indent-tabs-mode: t -*- */
// SPDX-License-Identifier: 0BSD
#define _GNU_SOURCE
//
#include "renamings.h"
//
#include "aw-addrcheck.h"
//

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <linux/ioctl.h>

// We copy some of the necessary definitions from linux/fs.h for
// PROCMAP_QUERY to enable compilation against older kernels.
#if !defined(PROCMAP_QUERY)
#define PROCMAP_QUERY _IOWR('f', 17, struct procmap_query)

enum procmap_query_flags {
	PROCMAP_QUERY_VMA_READABLE = 0x01,
	PROCMAP_QUERY_VMA_WRITABLE = 0x02,
	PROCMAP_QUERY_VMA_EXECUTABLE = 0x04,
	PROCMAP_QUERY_VMA_SHARED = 0x08,
};

struct procmap_query {
	/* Query struct size, for backwards/forward compatibility */
	uint64_t size;
	uint64_t query_flags; /* in */
	uint64_t query_addr; /* in */
	uint64_t vma_start; /* out */
	uint64_t vma_end; /* out */
	uint64_t vma_flags; /* out */
	uint64_t vma_page_size; /* out */
	uint64_t vma_offset; /* out */
	uint64_t inode; /* out */
	uint32_t dev_major; /* out */
	uint32_t dev_minor; /* out */
	uint32_t vma_name_size; /* in/out */
	uint32_t build_id_size; /* in/out */
	uint64_t vma_name_addr; /* in */
	uint64_t build_id_addr; /* in */
};
#endif

#define INITIAL_SIZE (1 << 20)
#define MINIMAL_BUF_SIZE (16 << 10)

// session_init is only "alive" during session initialization and only
// during parsing of proc_pid_maps file.
typedef struct session_init session_init;
struct session_init {
	int fd;
	aw_addrcheck_session_t *s;

	size_t entries_num;
	size_t entries_cap;

	// NOTE: parse_buf is pointing inside the s->entries
	// array. We're careful to consume text faster than entries
	// are appended to the entries array, so live entries
	// (i.e. indexes [0, entries_num)) do not overlap with
	// unconsumed text.
	const char *parse_buf;
	size_t parse_len;
	bool parse_eof;
};

struct aw_addrcheck_session {
	int fd;
	bool use_ioctl;
	bool has_cache;
	size_t total_mmap_size;

	union {
		struct aw_addrcheck_entry last_query_cache;
		size_t entries_num;
	};
	struct aw_addrcheck_entry entries[];
};

// fd for /proc/self/maps. Populated by maps_initialize and closed by
// atfork handler. Only set to valid fd when process has detected that
// PROCMAP_QUERY is working.
static int fd_self_maps = -1;
static bool ioctl_disabled;

/* Use initial-exec TLS model to ensure async-signal-safe access
 * (avoids malloc calls in shared libraries). */
static __thread bool in_fork_window __attribute__((tls_model("initial-exec")));

static void *mmap_anon(size_t size)
{
#ifdef SYS_mmap2
	int syscall_nr = SYS_mmap2;
#else
	int syscall_nr = SYS_mmap;
#endif
	void *p = (void *)syscall(syscall_nr, 0, size, PROT_READ | PROT_WRITE,
				  MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
				  -1, 0);
	if (p == MAP_FAILED)
		return NULL;
	return p;
}

static void munmap_anon(void *p, size_t size)
{
	syscall(SYS_munmap, p, size);
}

static void *remap_anon(void *old_p, size_t old_size, size_t new_size)
{
	void *p = (void *)syscall(SYS_mremap, old_p, old_size, new_size,
				  MREMAP_MAYMOVE, 0);
	if (p == MAP_FAILED)
		return NULL;
	return p;
}

static bool reader_fill_slow(session_init *si);

inline __attribute__((always_inline)) static bool reader_fill(session_init *si)
{
	if (si->parse_len)
		return true;
	if (si->parse_eof)
		return false;
	return reader_fill_slow(si);
}

static bool do_grow_session(session_init *si);

static bool reader_fill_slow(session_init *si)
{
	ssize_t n;
	char *buf_start;
	char *buf_end;
	do {
again:
		/* We use the remaining unused capacity of the entries array as our
		 * raw read buffer. We start reading at `entries_num + 2` to leave a
		 * 2-struct gap (e.g., 64 bytes). Because maps lines are
		 * consistently longer than the struct size (typically > 50 bytes),
		 * the text parsing pointer will outrun the struct appending
		 * pointer, ensuring we don't overwrite unparsed text.
		 */
		buf_start = (char *)(si->s->entries + si->entries_num + 2);
		buf_end = (char *)(si->s->entries + si->entries_cap);
		if (buf_end - buf_start < MINIMAL_BUF_SIZE) {
			if (!do_grow_session(si)) {
				return false;
			}
			goto again;
		}
		n = read(si->fd, buf_start, buf_end - buf_start);
	} while (n < 0 && errno == EINTR);

	if (n <= 0) {
		si->parse_eof = true;
		si->parse_buf = NULL;
		si->parse_len = 0;
		return false;
	}
	si->parse_buf = buf_start;
	si->parse_len = (size_t)n;
	return true;
}

static int reader_peek(session_init *si)
{
	if (!reader_fill(si))
		return -1;
	return (unsigned char)*si->parse_buf;
}

static int reader_get(session_init *si)
{
	if (!reader_fill(si))
		return -1;
	si->parse_len--;
	return (unsigned char)*(si->parse_buf++);
}

static void skip_until(session_init *si, char c)
{
	int ch;
	while ((ch = reader_get(si)) != -1) {
		if (ch == c)
			break;
	}
}

static bool consume_char(session_init *si, char c)
{
	if (reader_peek(si) != c)
		return false;

	reader_get(si);
	return true;
}

static void consume_space(session_init *si)
{
	int c;
	while ((c = reader_peek(si)) != -1) {
		if (c == ' ' || c == '\t')
			reader_get(si);
		else
			break;
	}
}

static bool consume_hex(session_init *si, uint64_t *out)
{
	uint64_t res = 0;
	bool found = false;
	int c;
	while ((c = reader_peek(si)) != -1) {
		uint64_t val;
		if (c >= '0' && c <= '9')
			val = (uint64_t)(c - '0');
		else if (c >= 'a' && c <= 'f')
			val = (uint64_t)(10 + (c - 'a'));
		else if (c >= 'A' && c <= 'F')
			val = (uint64_t)(10 + (c - 'A'));
		else
			break;
		res = (res << 4) | val;
		reader_get(si);
		found = true;
	}
	if (found)
		*out = res;
	return found;
}

// Parses linux proc-pid-maps line. See man proc_pid_maps. Example line:
//
// 7fe857669000-7fe85766a000 r--p 00000000 103:02 1338736687   /usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2
static bool parse_line(session_init *si, struct aw_addrcheck_entry *entry)
{
	if (reader_peek(si) == -1)
		return false;

	uint64_t val64;
	if (!consume_hex(si, &val64))
		goto bad;
	entry->start = (uintptr_t)val64;

	if (!consume_char(si, '-'))
		goto bad;

	if (!consume_hex(si, &val64))
		goto bad;
	entry->end = (uintptr_t)val64;

	consume_space(si);

	int c1 = reader_get(si);
	int c2 = reader_get(si);
	int c3 = reader_get(si);
	int c4 = reader_get(si);

	if (c1 == -1 || c2 == -1 || c3 == -1 || c4 == -1)
		return false;

	if (c1 == 'r')
		entry->perm_read = 1;
	else if (c1 != '-')
		goto bad;

	if (c2 == 'w')
		entry->perm_write = 1;
	else if (c2 != '-')
		goto bad;

	if (c3 == 'x')
		entry->perm_exec = 1;
	else if (c3 != '-')
		goto bad;

	// Note/TODO: we could make us ignore "bad" lines, which is
	// good for our purpose (checking if something is readable in
	// the backtracer). But there would be some API implications
	// and so on. So we'll leave it for now.
	if (c4 == 'p')
		entry->perm_priv = 1;
	else if (c4 != 's')
		goto bad;

	int space = reader_peek(si);
	if (space != ' ' && space != '\t')
		goto bad;
	consume_space(si);

	// offset
	if (!consume_hex(si, &val64))
		goto bad;

	skip_until(si, '\n');
	return true;

bad:
	skip_until(si, '\n');
	return false;
}

static bool do_grow_session(session_init *si)
{
	aw_addrcheck_session_t *s = si->s;
	void *new_buf;
	size_t new_total;
	size_t buf_offset = 0;

	if (si->parse_len)
		buf_offset = si->parse_buf - (const char *)s;

	if (!s->total_mmap_size) {
		size_t sz = INITIAL_SIZE;
		size_t cursz = sizeof(aw_addrcheck_session_t) +
			       si->entries_cap * sizeof(struct aw_addrcheck_entry);
		if (cursz * 2 > sz)
			sz = cursz * 2;
		new_buf = mmap_anon(sz);
		if (!new_buf)
			return false;
		new_total = sz;
		memcpy(new_buf, si->s, cursz);
	} else {
		new_total = s->total_mmap_size * 2;
		new_buf = remap_anon(s, s->total_mmap_size, new_total);
		if (!new_buf)
			return false;
	}

	if (si->parse_len)
		si->parse_buf = (const char *)new_buf + buf_offset;

	s = (aw_addrcheck_session_t *)new_buf;
	s->total_mmap_size = new_total;
	si->entries_cap = (new_total - sizeof(aw_addrcheck_session_t)) /
			  sizeof(struct aw_addrcheck_entry);

	si->s = s;
	return true;
}

static bool do_snapshot(session_init *si)
{
	while (reader_peek(si) != -1) {
		if (si->entries_num >= si->entries_cap) {
			if (!do_grow_session(si)) {
				return false;
			}
		}

		struct aw_addrcheck_entry entry = {};

		if (!parse_line(si, &entry)) {
			return si->parse_eof;
		}

		struct aw_addrcheck_entry *place =
			si->s->entries + (si->entries_num++);
		assert(si->parse_buf >= (char *)(place + 1));

		memcpy(place, &entry, sizeof(entry));
	}

	return true;
}

static int try_maps_query(aw_addrcheck_session_t *s, uintptr_t addr)
{
	struct procmap_query q = {
		.size = sizeof(q),
		.query_addr = (uint64_t)addr,
	};

	int ret;
	do {
		ret = ioctl(s->fd, PROCMAP_QUERY, &q);
	} while (ret < 0 && errno == EINTR);

	if (ret != 0) {
		if (errno == ENOENT)
			return 0;
		return -1;
	}

	struct aw_addrcheck_entry *e = &s->last_query_cache;
	e->start = (uintptr_t)q.vma_start;
	e->end = (uintptr_t)q.vma_end;
	e->perm_read = !!(q.vma_flags & PROCMAP_QUERY_VMA_READABLE);
	e->perm_write = !!(q.vma_flags & PROCMAP_QUERY_VMA_WRITABLE);
	e->perm_exec = !!(q.vma_flags & PROCMAP_QUERY_VMA_EXECUTABLE);
	e->perm_priv = !(q.vma_flags & PROCMAP_QUERY_VMA_SHARED);
	s->has_cache = true;
	return 1;
}

static aw_addrcheck_session_t *open_maps_fd(int fd, void *buffer, size_t buffer_size,
					    bool use_ioctl)
{
	aw_addrcheck_session_t *s;
	size_t total_mmap_size = 0;
	const size_t min_size = sizeof(aw_addrcheck_session_t);

	if (!buffer || buffer_size < min_size) {
		buffer_size = INITIAL_SIZE;
		buffer = mmap_anon(buffer_size);
		total_mmap_size = buffer_size;
		if (!buffer)
			return NULL;
	}

	s = (aw_addrcheck_session_t *)buffer;
	memset(s, 0, sizeof(*s));
	s->total_mmap_size = total_mmap_size;
	s->fd = fd;

	/* Check if ioctl works by querying current stack address */
	if (!ioctl_disabled && use_ioctl &&
	    (fd == fd_self_maps || try_maps_query(s, (uintptr_t)&s) == 1)) {
		s->use_ioctl = true;
		return s;
	}

	assert(fd != fd_self_maps);

	session_init si = { .fd = fd, .s = s };
	si.entries_cap = (buffer_size - sizeof(aw_addrcheck_session_t)) /
			 sizeof(struct aw_addrcheck_entry);

	if (!do_snapshot(&si)) {
		total_mmap_size = si.s->total_mmap_size;
		if (total_mmap_size) {
			munmap_anon(si.s, total_mmap_size);
		}
		return NULL;
	}
	si.s->entries_num = si.entries_num;

	return si.s;
}

aw_addrcheck_session_t *aw_addrcheck_open(void *buffer, size_t buffer_size)
{
	int fd = fd_self_maps;
	bool opened = false;
	if (fd < 0 || ioctl_disabled || in_fork_window) {
		fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			return NULL;
		opened = true;
	}
	aw_addrcheck_session_t *s = open_maps_fd(fd, buffer, buffer_size, true);
	if (!s && opened)
		close(fd);
	return s;
}

aw_addrcheck_session_t *aw_addrcheck_open_file(const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	aw_addrcheck_session_t *s = open_maps_fd(fd, NULL, 0, false);
	if (!s)
		close(fd);
	return s;
}

int aw_addrcheck_lookup(aw_addrcheck_session_t *s, uintptr_t addr,
			struct aw_addrcheck_entry *out)
{
	if (!s || !out)
		return 0;

	if (s->use_ioctl) {
		if (s->has_cache && addr >= s->last_query_cache.start &&
		    addr < s->last_query_cache.end) {
			*out = s->last_query_cache;
			return 1;
		}

		s->has_cache = false;
		int res = try_maps_query(s, addr);
		if (res == 1)
			*out = s->last_query_cache;
		return res;
	}

	if (s->entries_num == 0)
		return 0;

	size_t left = 0;
	size_t right = s->entries_num - 1;

	while (left <= right) {
		size_t mid = left + (right - left) / 2;
		const struct aw_addrcheck_entry *e = &s->entries[mid];

		if (addr >= e->start && addr < e->end) {
			*out = *e;
			return 1;
		}

		if (addr < e->start) {
			if (mid == 0)
				break;
			right = mid - 1;
		} else {
			left = mid + 1;
		}
	}

	return 0;
}

void aw_addrcheck_free(aw_addrcheck_session_t *s)
{
	if (!s)
		return;
	if (s->fd >= 0 && s->fd != fd_self_maps)
		close(s->fd);
	if (s->total_mmap_size)
		munmap_anon(s, s->total_mmap_size);
}

static void atfork_prepare(void)
{
	in_fork_window = true;
}

static void atfork_parent(void)
{
	in_fork_window = false;
}

static void atfork_child(void)
{
	/* In the child, we must close the inherited FD and reopen it to
	 * keep the correctness of the ioctl path. We keep the fork window
	 * flag set during this transition to protect signal handlers. */
	if (fd_self_maps >= 0) {
		close(fd_self_maps);
		fd_self_maps = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
		// We assume that if open succeeds (as it usually
		// does) then ioctl option will continue working in
		// the child
	}
	in_fork_window = false;
}

void aw_addrcheck_initialize(void)
{
	fd_self_maps = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
	if (fd_self_maps < 0)
		return;

	aw_addrcheck_session_t tmp_s = { .fd = fd_self_maps };
	if (try_maps_query(&tmp_s, (uintptr_t)&tmp_s) != 1) {
		(void)close(fd_self_maps);
		fd_self_maps = -1;
		return;
	}

	pthread_atfork(atfork_prepare, atfork_parent, atfork_child);
}

void aw_addrcheck_set_disable_ioctl_for_test(bool disable_ioctl)
{
	ioctl_disabled = disable_ioctl;
}
