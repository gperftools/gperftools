/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#include "symbolize-backtrace.h"

#include <assert.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "check.h"

using aw_backtrace_internal::FunctionRef;

extern "C" {
extern const unsigned char _binary_sym_helper_bin_start[];
extern const unsigned char _binary_sym_helper_bin_end[];
}

extern "C" {
extern char** environ;  // must be declared by user
}

Symbolizer::~Symbolizer() = default;

namespace {

// Memory allocator using mmap for async safety.
// We use a large fixed size map with MAP_NORESERVE to avoid committing memory.
// We assume 128MB is enough for everything.
struct MmapAllocator {
  uint8_t* ptr;
  size_t size;
  size_t pos;

  explicit MmapAllocator() : size(128 << 20), pos(0) {
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
      abort();
    }
    ptr = static_cast<uint8_t*>(p);
  }

  ~MmapAllocator() {
    munmap(ptr, size);
  }

  void* Alloc(size_t bytes) {
    bytes = (bytes + 7) & ~size_t{7};
    if (pos + bytes > size)
      abort();
    void* result = ptr + pos;
    pos += bytes;
    return result;
  }
};

std::string_view ReadNetString(const char** buf_place) {
  const char* buf = *buf_place;
  const char* p = buf;
  while (*p && *p != ':') {
    p++;
  }
  assert(*p == ':');
  int len = atoi(buf);
  assert(len >= 0);
  p++;
  assert(p[len] == ',');
  *buf_place = p + len + 1;
  return std::string_view(p, (size_t)len);
}

void WriteDiag(int fd, const char* msg) {
  // TODO: EINTR and "feed all loop"
  ssize_t written = write(fd, msg, strlen(msg));
  (void)written;  // ignore any errors
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

// Invoke given body on a alloca()-allocated stack on a child process
// cloned with given flags. Because stack is allocated on parent's
// stack we required CLONE_VFORK in the flags.
__attribute__((noinline)) pid_t CloneWith(size_t stack_size, int flags, FunctionRef<int()> body) {
  CHECK((flags & CLONE_VFORK) != 0);
  stack_size = stack_size & ~size_t{15};
  auto s = (uint8_t*)__builtin_alloca(stack_size);
  s[0] = 0xbf;
  // NOTE: we currently hardcode stack growing towards lower addresses.
  pid_t ret = clone(body.fn, s + stack_size, flags, body.data);
  CHECK(s[0] == 0xbf);  // lightweight stack overflow check
  return ret;
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

// Runs the helper `exec_path` and invokes `body` with a read end of its stdout.
// Returns the helper's exit status, or -1 if it could not be started.
//
// The helper is deliberately a *grand*child, which is what keeps this callable
// from inside an arbitrary host process without disturbing it:
//
//   * execve() resets exit_signal to SIGCHLD unconditionally (de_thread() in
//     fs/exec.c), so any process of ours that execs will signal its parent no
//     matter what we asked clone() for. The intermediate therefore never execs,
//     which is what lets its exit_signal stay 0.
//   * The intermediate outlives the helper and reaps it, so the helper's SIGCHLD
//     lands there rather than escaping to the host. Letting the intermediate
//     exit early instead would orphan the helper onto init, which works until
//     the host is a subreaper -- then it reparents right back onto the host.
//   * exit_signal 0 also hides the intermediate from the host's own reaping: a
//     plain wait(-1) cannot see it, so the host cannot steal our child.
//
// Scoped rather than returning a pid because both cloned stacks live in this
// frame, which is sound only because we do not return until both are gone.
//
// We also run the supplied body _in the intermediate process_. Should
// make no difference in practice, but e.g. crashes will be reported differently.
__attribute__((noinline)) int WithSpawnedChild(const char* exec_path, char* const* args, char* const* envp,
                                               FunctionRef<void(int)> body) {
  // We share the host's address space, so a signal delivered in the
  // cloned child would run the host's own handler against shared
  // memory, from a process the host does not know exists. Block the
  // lot; run_helper undoes it before exec. Also enables us to use
  // small stacks.
  WithBlockedSignals blocked_signals;

  static constexpr size_t kStackSize = 8 << 10;

  pid_t intermediate = CloneWith(kStackSize, CLONE_VM | CLONE_VFORK, [&]() -> int {
    // SIG_IGN or SA_NOCLDWAIT on SIGCHLD makes children reap themselves, which
    // would make the waitpid() below fail and report a bogus success. Blocking
    // does not undo that, only resetting the disposition does -- and since
    // SIG_IGN would otherwise survive execve, this is also what keeps
    // sym-helper's own reaping of addr2line working.
    signal(SIGCHLD, SIG_DFL);

    int pipefd[2];

    if (pipe(pipefd) != 0) {
      _exit(127);
    }

    pid_t helper = CloneWith(4 << 10, CLONE_VM | CLONE_VFORK, [&]() -> int {
      // The signal mask survives execve(), so undo the intermediate's blanket
      // block -- otherwise sym-helper, and the addr2line it spawns, run masked.
      sigset_t empty;
      sigemptyset(&empty);
      sigprocmask(SIG_SETMASK, &empty, nullptr);

      // Point stdout at the pipe and drop everything else. A target is free to
      // close its standard descriptors -- glibc's close_stdout atexit handler
      // does -- and then pipe() above returns numbers inside 0..2. Dropping the
      // read end first frees descriptor 1 for the dup2 when the read end is
      // what landed there; and when the write end *is* already descriptor 1
      // there is nothing to redirect, only a close that would throw away the
      // stdout we came here to install.
      close(pipefd[0]);
      if (pipefd[1] != STDOUT_FILENO) {
        if (dup2(pipefd[1], STDOUT_FILENO) == -1) {
          _exit(127);
        }
        close(pipefd[1]);
      }
      close(STDIN_FILENO);  // already gone if it was the read end above

      execve(exec_path, args, envp);
      _exit(127);  // execve failed
    });

    if (helper < 0) {
      _exit(127);
    }
    close(pipefd[1]);  // close the write end so the child's closing of the same will EOF

    body(pipefd[0]);

    int status = 0;
    while (waitpid(helper, &status, 0) < 0) {
      if (errno != EINTR) {
        _exit(127);
      }
    }
    _exit(WIFEXITED(status) ? WEXITSTATUS(status) : 126);  // relay to our parent
  });

  if (intermediate < 0) {
    return -1;
  }

  // __WALL: the intermediate never exec'd, so its exit_signal is still 0 and a
  // plain wait() would not see it.
  int status = 0;
  while (waitpid(intermediate, &status, __WALL) < 0) {
    if (errno != EINTR) {
      return -1;
    }
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Helper to create an executable memory file.
// Returns a file descriptor on success, or -1 on failure (with errno set).
// Safe to use in async-signal handlers.
int CreateExecutableMemFDOrDie(const char* name, const unsigned char* data, size_t size) {
  // Try to create an executable memory file.
  // Newer kernels (>= 6.3) support MFD_EXEC to explicitly mark the memfd as executable.
  // This is required if vm.memfd_noexec is set to 1.
#ifdef MFD_EXEC
  int fd = memfd_create(name, MFD_CLOEXEC | MFD_EXEC);
#else
  int fd = -1;
#endif

  if (fd < 0 && errno == EINVAL) {
    // Fallback for older kernels that don't understand MFD_EXEC
    fd = memfd_create(name, MFD_CLOEXEC);
  }

  if (fd < 0) {
    perror("memfd_create failed");
    abort();
  }

  if (fd < 3) {
    // in some odd cases memfd file descriptor could be one of
    // standard IO descriptors, so lets avoid them.
    int moved = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (moved < 0) {
      perror("fcntl(F_DUPFD_CLOEXEC)");
      abort();
    }
    close(fd);
    fd = moved;
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

class SymbolizerImpl : public Symbolizer {
 public:
  static constexpr size_t kAddrsCapacity = 1 << 10;
  SymbolizerImpl(SymbolizeCallback outcome_callback, void* outcome_data, MmapAllocator* allocator)
      : outcome_callback_(outcome_callback), outcome_data_(outcome_data), allocator_(allocator), addrs_count_(0) {
  }
  ~SymbolizerImpl() override {
    FlushAddrs();
  }

  void FlushAddrs() {
    if (addrs_count_ == 0) {
      return;
    }
    // create the helper binary in memory
    const size_t sym_helper_len = (size_t)(_binary_sym_helper_bin_end - _binary_sym_helper_bin_start);
    int exec_fd = CreateExecutableMemFDOrDie("sym-helper", _binary_sym_helper_bin_start, sym_helper_len);

    char exec_path[64];
    snprintf(exec_path, sizeof(exec_path), "/proc/self/fd/%d", exec_fd);

    // okay we'll spawn sym-helper program and give it all addresses we have
    char pid_buf[32];
    snprintf(pid_buf, sizeof(pid_buf), "%d", getpid());

    const char* args_prefix[] = {"sym-helper", pid_buf};
    char** args = (char**)allocator_->Alloc(sizeof(char*) * (2 + addrs_count_ + 1));
    memcpy(args, args_prefix, sizeof(args_prefix));
    for (size_t i = 0; i < addrs_count_; ++i) {
      char* arg = (char*)allocator_->Alloc(32);
      snprintf(arg, 32, "%ld", addrs_[i]);
      args[i + 2] = arg;
    }
    args[addrs_count_ + 2] = nullptr;

    char* filtered_environ[2] = {};
    {
      for (char** e = environ; *e != nullptr; e++) {
        if (strncmp("PATH=", *e, 5) == 0) {
          filtered_environ[0] = *e;
          break;
        }
      }
    }

    // now spawn child process and read output
    static constexpr size_t kBufSize = 4 << 20;
    char* output_buffer = (char*)allocator_->Alloc(kBufSize);
    size_t size = 0;

    int status = WithSpawnedChild(exec_path, args, filtered_environ,
                                  [&](int read_fd) { size = ReadAll(read_fd, output_buffer, kBufSize - 1); });
    close(exec_fd);  // Parent can close it now
    if (status < 0) {
      WriteDiag(STDERR_FILENO, "SpawnHelper failed\n");
      abort();
    }
    CHECK(status == 0);
    output_buffer[size] = '\0';

    const char* current = output_buffer;
    while (*current != '\0') {
      // okay so we have 5 netstrings per line exactly. Index (to
      // ignore), function, filename, line, and inline indictor. Then
      // newline
      std::string_view index_str = ReadNetString(&current);
      std::string_view function = ReadNetString(&current);
      std::string_view filename = ReadNetString(&current);
      std::string_view line_str = ReadNetString(&current);
      std::string_view inline_indicator = ReadNetString(&current);
      std::string_view module = ReadNetString(&current);
      std::string_view vaddr_hex = ReadNetString(&current);
      assert(*current == '\n');
      current++;

      int lineno = atoi(line_str.data());
      int addr_index = atoi(index_str.data());
      assert(addr_index >= 0);
      assert((size_t)addr_index < addrs_count_);
      uintptr_t vaddr = vaddr_hex.empty() ? 0 : strtoul(vaddr_hex.data(), nullptr, 16);
      SymbolizeOutcome outcome = {addrs_[addr_index],      function, filename, lineno,
                                  inline_indicator == "1", module,   vaddr};
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
  MmapAllocator* allocator_;
  size_t addrs_count_ = 0;
  uintptr_t addrs_[kAddrsCapacity];
};

}  // namespace

void WithSymbolizer(void (*with_callback)(Symbolizer*, void*), void* with_data, SymbolizeCallback outcome_callback,
                    void* outcome_data) {
  MmapAllocator allocator;
  SymbolizerImpl impl(outcome_callback, outcome_data, &allocator);
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
      WriteDiag(fd, buf);

      // Print first frame
      snprintf(buf, sizeof(buf), "%.*s %.*s (%.*s:%d)\n", (int)line_prefix.size(), line_prefix.data(),
               (int)outcome.function.size(), outcome.function.data(), (int)outcome.filename.size(),
               outcome.filename.data(), outcome.lineno);
      WriteDiag(fd, buf);
    } else {
      // Inlined
      snprintf(buf, sizeof(buf), "%.*s        inlined at %.*s (%.*s:%d)\n", (int)line_prefix.size(), line_prefix.data(),
               (int)outcome.function.size(), outcome.function.data(), (int)outcome.filename.size(),
               outcome.filename.data(), outcome.lineno);
      WriteDiag(fd, buf);
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
