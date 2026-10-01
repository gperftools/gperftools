/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#include "symbolize-backtrace.h"

#include <assert.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>

#include "check.h"
#include "sym-helper-protocol.h"

// MFD_EXEC is relatively newer feature. The value is well-known and
// part of the ABI. But we might be building on older system. We
// handle lack of MFD_EXEC support at runtime.
#ifndef MFD_EXEC
#define MFD_EXEC 0x0010U
#endif

extern "C" {
extern const unsigned char _binary_sym_helper_bin_start[];
extern const unsigned char _binary_sym_helper_bin_end[];
}

extern "C" {
extern char** environ;  // must be declared by user
}

Symbolizer::~Symbolizer() = default;

namespace {

bool WriteAll(int fd, const char* buf, size_t size) {
  size_t written = 0;
  while (written < size) {
    ssize_t n = write(fd, buf + written, size - written);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    written += (size_t)n;
  }
  return true;
}

void WriteDiag(const char* msg) {
  WriteAll(STDERR_FILENO, msg, strlen(msg));
}

size_t ReadAll(int fd, char* buf, size_t size) {
  size_t total_read = 0;
  while (total_read < size - 1) {
    ssize_t n = read(fd, buf + total_read, size - 1 - total_read);
    if (n > 0) {
      total_read += (size_t)n;
    } else if (n == 0) {
      break;  // EOF
    } else if (errno != EINTR) {
      break;  // Error
    }
  }
  return total_read;
}

// Async-signal-safe: open/read/close only, into a caller-supplied buffer.
size_t ReadSelfMapsInto(char* buf, size_t size) {
  int fd = open("/proc/self/maps", O_RDONLY);
  if (fd < 0) {
    return 0;
  }
  size_t n = ReadAll(fd, buf, size);
  close(fd);
  return n;
}

struct WithBlockedSignals {
  WithBlockedSignals() {
    sigset_t new_mask;
    sigfillset(&new_mask);
    CHECK(sigprocmask(SIG_SETMASK, &new_mask, &old_mask) == 0);
  }
  void Restore() {
    CHECK(sigprocmask(SIG_SETMASK, &old_mask, nullptr) == 0);
  }
  ~WithBlockedSignals() {
    Restore();
  }

  sigset_t old_mask;
};

// A raw CAS spinlock, nothing smarter. We are not chasing throughput here --
// (re)spawning the helper daemon is a rare, at-most-once-per-process-image
// event -- only correctness under signals: never call this while holding the
// lock's word from within a context that could itself be interrupted and
// re-enter (callers pair it with WithBlockedSignals for exactly that reason).
struct SpinLock {
  std::atomic<int>* word;

  void Lock() {
    for (;;) {
      int expected = 0;
      if (word->compare_exchange_weak(expected, 1, std::memory_order_acquire)) {
        return;
      }
      sched_yield();
    }
  }
  void Unlock() {
    word->store(0, std::memory_order_release);
  }
};

// Helper to create an executable memory file.
// Returns a file descriptor on success, or -1 on failure (with errno set).
// Safe to use in async-signal handlers.
int CreateExecutableMemFDOrDie(const char* name, const unsigned char* data, size_t size) {
  // Try to create an executable memory file.
  // Newer kernels (>= 6.3) support MFD_EXEC to explicitly mark the memfd as executable.
  // This is required if vm.memfd_noexec is set to 1.
  int fd = memfd_create(name, MFD_CLOEXEC | MFD_EXEC);

  if (fd < 0 && errno == EINVAL) {
    // Fallback for older kernels that don't understand MFD_EXEC
    fd = memfd_create(name, MFD_CLOEXEC);
  }

  if (fd < 0) {
    perror("memfd_create failed");
    abort();
  }

  size_t total_written = 0;
  while (total_written < size) {
    ssize_t n = write(fd, data + total_written, size - total_written);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      perror("failed to write sym-helper data");
      abort();
    }
    total_written += (size_t)n;
  }
  return fd;
}

// ---------------------------------------------------------------------------
// Persistent sym-helper daemon: spawn-once, message-many.
//
// At spawn time we create an AF_UNIX SOCK_SEQPACKET socketpair, keep
// one end (the "mailbox") for this process and hand the other to the
// daemon. Every symbolization request thereafter is one "package"
// sent to the mailbox.
//
// Each request datagram carries two FDs via SCM_RIGHTS, handed to the
// daemon atomically in one message (see RunHelperRequest): a memfd
// holding the whole request, and the write end of a pipe created
// fresh for this one request. The daemon writes its response into
// that pipe and closes it; we read it until EOF. Concurrent callers
// (e.g. two threads each hitting a mismatch at once) share only the
// mailbox fd, and sendmsg() to it is atomic per-message, so nothing
// here needs locking -- each request's response travels over its own,
// private pipe, with no shared mutable state between concurrent
// callers at all.
//
// The one place serialization is unavoidable is the lazy, at-most-once spawn
// of the daemon itself. That is guarded by a raw CAS spinlock with signals
// blocked for its duration (SpinLock above).
// ---------------------------------------------------------------------------

constinit std::atomic<int> g_helper_lock{0};

// kHelperUnresolved until the first EnsureHelperOwner() call spawns
// (or tries to spawn) the daemon; thereafter either a valid mailbox
// fd or -1 (SpawnHelperDaemon's own failure value) forever. A child
// sharing the inherited daemon this way is no different from two
// threads of the same process sharing it, which this code already has
// to support.
constexpr int kHelperUnresolved = -2;
constinit std::atomic<int> g_helper_mailbox_fd{kHelperUnresolved};

// Spawns the persistent sym-helper daemon and returns the "mailbox" fd this
// process should use for every future symbolization request, or -1 on
// failure.
//
// A single plain vfork() is enough. The daemon is expected to run
// forever. We accept the tradeoff here: if the daemon (or the exec
// itself) ever does die -- a crash, a bad exec path -- this process
// gets one ordinary SIGCHLD for it, same as for any other child. In
// practice the case most likely to actually fire this is an immediate
// execve() failure, not some far-future crash.
int SpawnHelperDaemon(const char* exec_path, char* const* envp) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0) {
    WriteDiag("sym-helper: socketpair failed\n");
    return -1;
  }

  char fd_arg[16];
  snprintf(fd_arg, sizeof(fd_arg), "%d", sv[1]);
  char* args[] = {const_cast<char*>("sym-helper"), fd_arg, nullptr};

  WithBlockedSignals blocked_signals;

  pid_t child = vfork();
  if (child == 0) {
    // vfork() gave us every signal blocked (WithBlockedSignals, still in
    // effect on this shared mask); the daemon needs its own signals back
    // before it execs into a program that expects normal signal delivery.
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, nullptr);
    // reset CLOEXEC from mailbox fd
    fcntl(sv[1], F_SETFD, 0);
    close(sv[0]);  // the daemon only ever needs its own end of the socketpair
    execve(exec_path, args, envp);
    _exit(127);  // execve failed
  }

  // Our own copy of sv[1] is redundant either way: the daemon (if it
  // started) has its own via vfork()+exec() inheritance, and if spawning
  // failed outright there is nothing left to talk to.
  close(sv[1]);

  if (child < 0) {
    WriteDiag("sym-helper: failed to spawn daemon\n");
    close(sv[0]);
    return -1;
  }
  return sv[0];
}

// Ensures a sym-helper daemon is running, and returns the mailbox fd to
// send requests to (or -1 if spawning ever failed -- cached permanently,
// not retried). Safe to call concurrently, including from async-signal
// context: the common case (already resolved) takes no lock at all; the
// rare case (first call ever, in this process or an ancestor it forked
// from without an intervening exec) is serialized by SpinLock with signals
// blocked for its duration.
int EnsureHelperOwner() {
  int mailbox = g_helper_mailbox_fd.load(std::memory_order_acquire);
  if (mailbox == kHelperUnresolved) {
    WithBlockedSignals blocked;
    SpinLock lock{&g_helper_lock};
    lock.Lock();

    mailbox = g_helper_mailbox_fd.load(std::memory_order_relaxed);
    if (mailbox == kHelperUnresolved) {
      const size_t sym_helper_len = (size_t)(_binary_sym_helper_bin_end - _binary_sym_helper_bin_start);
      int exec_fd = CreateExecutableMemFDOrDie("sym-helper", _binary_sym_helper_bin_start, sym_helper_len);

      char exec_path[64];
      snprintf(exec_path, sizeof(exec_path), "/proc/self/fd/%d", exec_fd);

      char* filtered_environ[2] = {};
      for (char** e = environ; *e != nullptr; e++) {
        if (strncmp("PATH=", *e, 5) == 0) {
          filtered_environ[0] = *e;
          break;
        }
      }

      mailbox = SpawnHelperDaemon(exec_path, filtered_environ);
      close(exec_fd);
      g_helper_mailbox_fd.store(mailbox, std::memory_order_release);
    }

    lock.Unlock();
  }

  return mailbox;
}

// A successful RunHelperRequest's result: the response lives in this
// mapping until the caller munmap()s it. Deliberately oversized and
// MAP_NORESERVE'd (see RunHelperRequest) rather than sized to the exact
// response via an fstat() first -- address space is cheap on 64-bit hosts,
// and this way nothing needs to ask the daemon "how big is your answer"
// before reading it.
struct Mapping {
  char* const base;
  size_t reserved_size;

  bool Valid() const {
    return base != nullptr;
  }

  Mapping() : base{}, reserved_size{} {
  }

  Mapping(char* base, size_t reserved_size) : base{base}, reserved_size{reserved_size} {
  }

  Mapping(Mapping&& other) : base(other.base), reserved_size{other.reserved_size} {
    other.reserved_size = 0;
  }

  ~Mapping() {
    if (reserved_size) {
      munmap(base, reserved_size);
    }
  }
};

// Sends one symbolization request to the (already-ensured-running) helper
// daemon and reads its response. Builds the whole request as a memfd (one
// address per line, a "0" sentinel line, then our own /proc/self/maps
// snapshot verbatim) and a fresh, private completion pipe, then hands both
// to the daemon atomically via SCM_RIGHTS on the shared mailbox.
//
// The same memfd carries the response back (the daemon truncates and
// rewrites it in place -- see sym-helper.cc), which saves an fd per
// request compared to a separate response memfd. The completion pipe
// carries no data at all, only a close(). Returns an invalid mapping
// (Valid() == false) on any failure.
Mapping RunHelperRequest(int mailbox_fd, const uintptr_t* addrs, size_t addrs_count) {
  if (mailbox_fd < 0) {
    return {};
  }

  int mem_fd = memfd_create("sym-request", MFD_CLOEXEC);
  if (mem_fd < 0) {
    return {};
  }

  struct CloseFd {
    const int fd;
    ~CloseFd() {
      close(fd);
    }
  } close_mem_fd{mem_fd};

  // Deliberately oversized and MAP_NORESERVE'd: address space is cheap on a
  // 64-bit host.
  static constexpr size_t kMmapReserve = 128 << 20;
  int rv = ftruncate(mem_fd, kMmapReserve);
  if (rv != 0) {
    return {};
  }

  char* mapped = (char*)mmap(nullptr, kMmapReserve, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NORESERVE, mem_fd, 0);
  if (mapped == MAP_FAILED) {
    return {};
  }
  Mapping ret{mapped, kMmapReserve};

  {
    SymRequest* req = reinterpret_cast<SymRequest*>(mapped);
    req->count = addrs_count;
    for (size_t i = 0; i < addrs_count; i++) {
      req->vaddrs[i] = addrs[i];
    }

    char* p = reinterpret_cast<char*>(&req->vaddrs[addrs_count]);
    p += ReadSelfMapsInto(p, mapped + ret.reserved_size - p);

    rv = ftruncate(mem_fd, (p - mapped));
    if (rv != 0) {
      return {};
    }
  }

  int done_pipe[2];
  if (pipe2(done_pipe, O_CLOEXEC) != 0) {
    return {};
  }

  char one_byte = 0;
  struct iovec iov = {&one_byte, sizeof(one_byte)};

  int fds[2] = {mem_fd, done_pipe[1]};
  union {
    char buf[CMSG_SPACE(sizeof(fds))];
    struct cmsghdr align;
  } cmsg_buf;

  struct msghdr msg = {};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsg_buf.buf;
  msg.msg_controllen = sizeof(cmsg_buf.buf);

  struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(fds));
  memcpy(CMSG_DATA(cmsg), fds, sizeof(fds));

  bool sent = false;
  for (;;) {
    // MSG_NOSIGNAL: a dead daemon must fail this request, not kill the host
    // process with SIGPIPE.
    ssize_t n = sendmsg(mailbox_fd, &msg, MSG_NOSIGNAL);
    if (n >= 0) {
      sent = true;
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    break;
  }

  // Our local copy of the write end must go regardless of outcome, or the
  // daemon closing its own copy would never be enough to signal EOF to us.
  // mem_fd, by contrast, we deliberately keep open past this point: we
  // still need it for the mmap() below.
  close(done_pipe[1]);

  if (!sent) {
    close(done_pipe[0]);
    return {};
  }

  // Wait for the daemon to finish: it never writes any actual bytes down
  // this pipe, only closes its end once the response is fully committed to
  // mem_fd (or it dies trying to, which closes its end too -- the magic
  // check below is what tells those two cases apart).
  for (;;) {
    char dummy;
    ssize_t n = read(done_pipe[0], &dummy, sizeof(dummy));
    if (n < 0 && errno == EINTR) {
      continue;
    }
    break;  // EOF, an unexpected byte, or a real error: either way, done waiting
  }
  close(done_pipe[0]);

  const auto* header = (const SymResponseHeader*)mapped;
  if (header->magic != kSymResponseMagic) {
    // Either the daemon crashed before it could commit a response, or
    // something else went wrong -- either way, there is nothing valid here.
    return {};
  }

  return ret;
}

// Stands in for the daemon's answer when there is none: one outcome per pc,
// carrying only the module the pc falls in and its ELF vaddr, from the dynamic
// loader's own records. No function or line, but (module, vaddr) is exactly
// what addr2line needs by hand, and a dump never comes back with its frames
// missing.
//
// Async-signal-safe. The vaddr is the pc minus the module's load
// bias. The main executable's link_map has an empty l_name, so it is
// reported as the literal "/proc/self/exe" rather than a real path.
void ReportUnsymbolized(const uintptr_t* addrs, size_t count, SymbolizeCallback callback, void* data) {
  for (size_t i = 0; i < count; i++) {
    SymbolizeOutcome outcome = {addrs[i], "", "", 0, false, "", 0};

    struct dl_find_object dlfo;
    if (_dl_find_object(reinterpret_cast<void*>(addrs[i]), &dlfo) == 0 && dlfo.dlfo_link_map != nullptr) {
      const struct link_map* map = dlfo.dlfo_link_map;
      const char* l_name = map->l_name;
      if (!l_name || l_name[0] == 0) {
        l_name = "/proc/self/exe";
      }
      outcome.module = std::string_view{l_name};
      outcome.vaddr = addrs[i] - map->l_addr;
    }

    callback(outcome, data);
  }
}

class SymbolizerImpl : public Symbolizer {
 public:
  static constexpr size_t kAddrsCapacity = 1 << 10;

  SymbolizerImpl(SymbolizeCallback outcome_callback, void* outcome_data)
      : outcome_callback_(outcome_callback), outcome_data_(outcome_data), addrs_count_(0) {
  }
  ~SymbolizerImpl() override {
    FlushAddrs();
  }

  void FlushAddrs() {
    if (addrs_count_ == 0) {
      return;
    }

    int mailbox_fd = EnsureHelperOwner();

    Mapping mapping = RunHelperRequest(mailbox_fd, addrs_, addrs_count_);
    if (!mapping.Valid()) {
      WriteDiag("sym-helper request failed; reporting unsymbolized addresses\n");
      ReportUnsymbolized(addrs_, addrs_count_, outcome_callback_, outcome_data_);
      addrs_count_ = 0;
      return;
    }

    // Trivial to read by construction: a fixed header, then a fixed-size
    // entry array, then a flat blob every string points into by
    // offset+length. No parsing beyond pointer arithmetic -- string_view is
    // safe to construct directly over these raw, mmap()'d bytes (unlike,
    // say, casting them straight into a std::string, whose own internal
    // representation is not something arbitrary bytes can safely become).
    const auto* header = (const SymResponseHeader*)mapping.base;
    const auto* entries = (const SymResponseEntry*)(header + 1);
    const char* blob = (const char*)(entries + header->entry_count);

    for (uint32_t i = 0; i < header->entry_count; ++i) {
      const SymResponseEntry& e = entries[i];
      std::string_view function(blob + e.function_off, e.function_len);
      std::string_view filename(blob + e.filename_off, e.filename_len);
      std::string_view module(blob + e.module_off, e.module_len);
      assert(e.addr_index < addrs_count_);
      SymbolizeOutcome outcome = {addrs_[e.addr_index], function, filename, (int)e.lineno,
                                  e.inlined != 0,       module,   e.vaddr};
      outcome_callback_(outcome, outcome_data_);
    }

    addrs_count_ = 0;
  }

  void Add(uintptr_t addr) override {
    if (addrs_count_ >= kAddrsCapacity) {
      FlushAddrs();
    }
    addrs_[addrs_count_++] = addr;
  }

 private:
  SymbolizeCallback outcome_callback_;
  void* outcome_data_;
  size_t addrs_count_ = 0;
  uintptr_t addrs_[kAddrsCapacity];
};

}  // namespace

extern "C" {
void test_only_symbolize_backtrace_kill_daemon(void);
}

// Shuts the mailbox down rather than closing it: the descriptor number stays
// valid, so every later request still goes through a real, failing sendmsg()
// (EPIPE), and the daemon reads EOF and exits.
void test_only_symbolize_backtrace_kill_daemon(void) {
  int fd = g_helper_mailbox_fd.load();
  if (fd >= 0) {
    shutdown(fd, SHUT_RDWR);
  }
}

void WithSymbolizer(void (*with_callback)(Symbolizer*, void*), void* with_data, SymbolizeCallback outcome_callback,
                    void* outcome_data) {
  SymbolizerImpl impl(outcome_callback, outcome_data);
  with_callback(&impl, with_data);
  // impl destuctor will flush all the addresses
}

void DumpStackTraceToFD(int fd, void* const* stack, int stack_depth, bool want_symbolize,
                        std::string_view line_prefix) {
  bool fresh_frame = true;
  auto dump_outcome = [&](const SymbolizeOutcome& outcome) {
    char buf[512];

    if (fresh_frame) {
      // Print address line
      if (!outcome.module.empty()) {
        snprintf(buf, sizeof(buf), "%.*s@ 0x%lx (%.*s+0x%lx)\n", (int)line_prefix.size(), line_prefix.data(),
                 outcome.pc, (int)outcome.module.size(), outcome.module.data(), outcome.vaddr);
      } else {
        snprintf(buf, sizeof(buf), "%.*s@ 0x%lx\n", (int)line_prefix.size(), line_prefix.data(), outcome.pc);
      }
      WriteAll(fd, buf, strlen(buf));

      // Print first frame
      snprintf(buf, sizeof(buf), "%.*s %.*s (%.*s:%d)\n", (int)line_prefix.size(), line_prefix.data(),
               (int)outcome.function.size(), outcome.function.data(), (int)outcome.filename.size(),
               outcome.filename.data(), outcome.lineno);
      WriteAll(fd, buf, strlen(buf));
    } else {
      // Inlined
      snprintf(buf, sizeof(buf), "%.*s        inlined at %.*s (%.*s:%d)\n", (int)line_prefix.size(), line_prefix.data(),
               (int)outcome.function.size(), outcome.function.data(), (int)outcome.filename.size(),
               outcome.filename.data(), outcome.lineno);
      WriteAll(fd, buf, strlen(buf));
    }
    fresh_frame = !outcome.inlined;
  };

  if (!want_symbolize) {
    SymbolizeOutcome outcome;
    outcome.function = "";
    outcome.filename = "";
    outcome.lineno = 0;
    outcome.inlined = false;
    for (int i = 0; i < stack_depth; ++i) {
      outcome.pc = (uintptr_t)stack[i];
      dump_outcome(outcome);
    }
    return;
  }

  WithSymbolizerFnRef(
      [&](Symbolizer* sym) {
        for (int i = 0; i < stack_depth; ++i) {
          sym->Add((uintptr_t)stack[i]);
        }
      },
      dump_outcome);
}
