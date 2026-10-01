/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
//
// This file is a helper *daemon*, spawned once (see EnsureHelperOwner
// / SpawnHelperDaemon in symbolize-backtrace.cc) and kept running for
// the life of the owning process. At spawn time the caller creates an
// AF_UNIX SOCK_SEQPACKET socketpair, keeps one end (the "mailbox")
// for itself, and hands us the other; every symbolization request
// thereafter is one datagram sent to that mailbox.
//
// Each request datagram carries two fds via SCM_RIGHTS, handed over
// atomically in one message: a memfd holding the whole request as
// SymRequest, and the write end of a pipe created fresh for this one
// request. We reuse that *same* memfd for the response (truncate it,
// then write our answer back into it as a small binary struct -- see
// SymResponseHeader/SymResponseEntry below), saving an fd per
// request; the pipe now carries no data of its own, only a close()
// once the response is fully committed, since the caller reads the
// response via mmap() rather than by streaming it, and mmap() has no
// "wait until ready" signal of its own. Concurrent callers (e.g. two
// threads each hitting a mismatch at the same moment) never share any
// mutable state with each other this way -- sendmsg() to the shared
// mailbox is atomic per-message, and each request's response travels
// over its own, private memfd+pipe pair -- so nothing here needs
// locking on the caller's side; we still process one request at a
// time (see main()'s loop), which is what lets every per-binary
// addr2line session below be touched with no locking on this side
// either.
//
// The maps snapshot is deliberately the *caller's own* view: under
// qemu-user this is the guest-address-space view QEMU synthesizes for
// the caller.
//
// Addresses are correlated to an ELF file + vaddr using that maps snapshot,
// batched per backing binary, and resolved via a persistent addr2line (or
// llvm-addr2line) subprocess per binary -- spawned once per binary and reused
// across every request for the life of this daemon, rather than respawned
// (and made to re-parse that binary's debug info from scratch) every time.
//
// Since this is a separate binary (instead of being in async-signal-safe
// context in symbolize-backtrace.cc) it does straightforward things using
// plain normal C and C++ APIs.

#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <deque>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "sym-helper-protocol.h"

struct ProcVMA {
  const uint64_t start;
  const uint64_t end;
  const uint64_t offset;
  const std::string path;

  ProcVMA(uint64_t start, uint64_t end, uint64_t offset, std::string_view path)
      : start{start}, end{end}, offset{offset}, path{std::string{path}} {
  }
};

struct PTLoadSeg {
  uint64_t vaddr;
  uint64_t off;
  uint64_t memsz;
};

struct Frame {
  std::string func_name;
  std::string file_name;
  std::string line_num;
  bool inlined;
};

struct Request {
  int index;
  uint64_t orig_addr;
  bool valid;
  std::string map_path;
  uint64_t elf_vaddr = 0;
  std::vector<Frame> frames;

  Request(int index, unsigned long addr) : index{index}, orig_addr{addr}, valid{true} {
  }
};

using ElfCacheMap = std::unordered_map<std::string, std::vector<PTLoadSeg>>;

static std::vector<ProcVMA> ParseProcMaps(std::string_view full_text) {
  FILE* f = fmemopen(const_cast<char*>(full_text.data()), full_text.size(), "r");
  std::vector<ProcVMA> maps;

  // The proc-maps line looks like this: (man 5 proc_pid_maps)
  // 7ffff7f78000-7ffff7f7a000 rw-p 001e7000 103:02 1338729752                /usr/lib/x86_64-linux-gnu/libc.so.6
  // First "field" is address range, then perms, then file offset.
  for (;;) {
    unsigned long start, end, offset;
    int before_path, after_path = -1;
    long pos = ftell(f);
    int dummy = fscanf(f, "%lx-%lx %*[^ ] %lx %*[^ ] %*[^ ]%n%*[^\n]%n",  //
                       &start, &end, &offset, &before_path, &after_path);
    (void)dummy;
    if (after_path < 0) {
      // eof or garbage. Cannot parse further.
      break;
    }
    std::string_view map_path = full_text.substr(pos + before_path, after_path - before_path);
    while (!map_path.empty() && isspace(map_path[0])) {
      map_path.remove_prefix(1);
    }
    if (!map_path.starts_with('/')) {
      // bad line. skip
      continue;
    }

    maps.emplace_back(start, end, offset, map_path);
  }

  fclose(f);

  return maps;
}

static std::vector<PTLoadSeg> ReadElfLoadSegments(const std::string& path) {
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0)
    return {};

  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(ElfW(Ehdr)))) {
    close(fd);
    return {};
  }

  void* map = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (map == MAP_FAILED)
    return {};

  std::vector<PTLoadSeg> phdrs;
  const auto* ehdr = static_cast<const ElfW(Ehdr)*>(map);

  if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) == 0 &&
      ehdr->e_phoff + ehdr->e_phnum * sizeof(ElfW(Phdr)) <= static_cast<size_t>(st.st_size)) {
    const auto* phdr_table = reinterpret_cast<const ElfW(Phdr)*>(static_cast<const char*>(map) + ehdr->e_phoff);
    for (uint16_t i = 0; i < ehdr->e_phnum; ++i) {
      if (phdr_table[i].p_type == PT_LOAD) {
        phdrs.push_back({phdr_table[i].p_vaddr, phdr_table[i].p_offset, phdr_table[i].p_memsz});
      }
    }
  }

  munmap(map, st.st_size);
  return phdrs;
}

// Unlike the pid-based version this replaces, there is no external process
// to fall back to for a deleted/relocated path: the caller's own maps
// snapshot is the only information we have, and its path is either openable
// directly (the common case -- same architecture, real file still on disk)
// or the address simply cannot be resolved.
static bool GetElfVAddrAndPath(const ProcVMA& m, uint64_t file_offset, ElfCacheMap* elf_cache, uint64_t* out_vaddr,
                               std::string* out_path) {
  auto [it, inserted] = elf_cache->try_emplace(m.path);
  if (inserted) {
    it->second = ReadElfLoadSegments(m.path);
  }
  if (it->second.empty()) {
    return false;
  }

  for (const auto& phdr : it->second) {
    if (file_offset >= phdr.off && file_offset < phdr.off + phdr.memsz) {
      *out_vaddr = phdr.vaddr + (file_offset - phdr.off);
      *out_path = m.path;
      return true;
    }
  }

  return false;
}

static Frame ParseFrame(std::string func_name, std::string_view file_line) {
  if (func_name == "??") {
    func_name.clear();
  }

  size_t disc_pos = file_line.find(" (discriminator");
  if (disc_pos != std::string::npos) {
    file_line = file_line.substr(0, disc_pos);
  }

  size_t colon_pos = file_line.rfind(':');
  std::string file{file_line};
  std::string line;
  if (colon_pos != std::string::npos) {
    file = file_line.substr(0, colon_pos);
    line = file_line.substr(colon_pos + 1);
  }

  if (file == "??" || file == "?") {
    file.clear();
  }
  if (line == "?" || line == "0") {
    line.clear();
  }

  return Frame{std::move(func_name), std::move(file), std::move(line), false};
}

// A persistent addr2line (or llvm-addr2line) subprocess for one backing
// binary, kept alive across every request this daemon ever serves for that
// binary rather than respawned per request -- spawning it and having it
// re-parse that binary's debug info from scratch is the expensive part this
// daemon exists to amortize.
struct Addr2LineSession {
  pid_t pid = -1;
  FILE* stdin_fp = nullptr;
  FILE* stdout_fp = nullptr;

  bool Alive() const {
    return pid > 0;
  }
};

// A bogus address that will never legitimately be requested and always
// resolves to "no symbol": our explicit end-of-batch marker for reading a
// persistent session's stdout, since (unlike the one-shot design this
// replaces) there is no EOF between batches to rely on.
static constexpr uint64_t kSentinelAddr = (uint64_t)~uintptr_t{};

static std::map<std::string, Addr2LineSession> g_sessions;

static Addr2LineSession* GetOrCreateSession(const std::string& path) {
  auto [it, inserted] = g_sessions.try_emplace(path);
  Addr2LineSession& s = it->second;
  if (s.Alive()) {
    return &s;
  }

  struct OwnedFD {
    const int fd;
    bool released = false;
    OwnedFD(int fd) : fd{fd} {
    }

    int Release() {
      released = true;
      return fd;
    }
    OwnedFD(const OwnedFD& other) = delete;
    ~OwnedFD() {
      if (!released)
        close(fd);
    }
  };

  std::deque<OwnedFD> cleanup;

  auto pipe_with_cleanup = [&](OwnedFD* fds[2]) -> bool {
    int pair[2];
    int rv = pipe2(pair, O_CLOEXEC);
    if (rv != 0) {
      perror("pipe2");
      return false;
    }
    cleanup.emplace_back(pair[0]);
    fds[0] = &cleanup.back();
    cleanup.emplace_back(pair[1]);
    fds[1] = &cleanup.back();
    return true;
  };

  OwnedFD* in_pipe[2];
  if (!pipe_with_cleanup(in_pipe)) {
    return nullptr;
  }
  OwnedFD* out_pipe[2];
  if (!pipe_with_cleanup(out_pipe)) {
    return nullptr;
  }

  pid_t pid = fork();
  if (pid < 0) {
    return nullptr;
  }

  if (pid == 0) {
    // child
    //
    // close write side of the child's STDIN and read side of child's STDOUT
    close(in_pipe[1]->fd);
    close(out_pipe[0]->fd);
    // move those read/write ends to their relevant FDs
    dup2(in_pipe[0]->fd, STDIN_FILENO);
    close(in_pipe[0]->fd);
    dup2(out_pipe[1]->fd, STDOUT_FILENO);
    close(out_pipe[1]->fd);

    // No addresses on the command line this time: they are fed one per line
    // on stdin for as long as this process lives.
    std::vector<std::string> arg_storage = {"llvm-addr2line", "-a", "-f", "-i", "-C", "-e", path};
    std::vector<char*> argv_ptrs;
    for (auto& a : arg_storage) argv_ptrs.push_back(a.data());
    argv_ptrs.push_back(nullptr);

    // We try llvm-addr2line and GNU addr2line in order. Both speak the
    // format ParseFrame() expects, and both support reading addresses from
    // stdin indefinitely when none are given on the command line.
    execvp(argv_ptrs[0], argv_ptrs.data());
    argv_ptrs[0] = const_cast<char*>("addr2line");
    execvp(argv_ptrs[0], argv_ptrs.data());
    _exit(127);
  }

  // parent

  s.pid = pid;
  s.stdin_fp = fdopen(in_pipe[1]->Release(), "w");
  s.stdout_fp = fdopen(out_pipe[0]->Release(), "r");
  if (!s.stdout_fp) {
    abort();  // not possible in practice. keep it simple
  }
  return &s;
}

struct LineBuf {
  char* line_buf = nullptr;
  size_t line_len = 0;

  std::optional<std::string> Read(FILE* f) {
    ssize_t nread = getline(&line_buf, &line_len, f);
    if (nread < 0) {
      return std::nullopt;
    }
    // chomp it
    if (nread > 0 && line_buf[nread - 1] == '\n') {
      nread--;
    }
    return std::optional<std::string>(std::in_place, line_buf, (size_t)nread);
  }

  ~LineBuf() {
    free(line_buf);
  }
};

static void RunAddr2LineBatch(const std::string& path, const std::vector<Request*>& batch) {
  Addr2LineSession* s = GetOrCreateSession(path);
  if (!s)
    return;  // leave requests unresolved -- best effort

  std::thread batch_writer([&]() {
    for (const auto* req : batch) {
      fprintf(s->stdin_fp, "0x%lx\n", (unsigned long)req->elf_vaddr);
    }
    fprintf(s->stdin_fp, "0x%lx\n", kSentinelAddr);
    fflush(s->stdin_fp);
  });

  LineBuf lb;

  bool success = false;

  // addr2line outputs: address-line \n (function-name \n file:line \n)+,
  // repeated once per address we fed it, in order, terminated by our own
  // sentinel address instead of EOF (this session outlives this one batch).
  size_t target_idx = 0;
  std::vector<Frame> current_frames;
  for (;;) {
    // address
    unsigned long addr;
    char maybe_nl;
    int r = fscanf(s->stdout_fp, "%lx%c", &addr, &maybe_nl);
    if (r != 2 || maybe_nl != '\n') {
      break;
    }

    if (addr == kSentinelAddr) {
      success = true;
      // Eat sentinel function and file lines.
      lb.Read(s->stdout_fp);
      lb.Read(s->stdout_fp);
      break;
    }

    if (target_idx >= batch.size() || addr != batch[target_idx]->elf_vaddr) {
      break;
    }
    int peek_char;
    do {
      // parse function line followed by file line
      std::optional<std::string> fn_line = lb.Read(s->stdout_fp);
      std::optional<std::string> file_line = lb.Read(s->stdout_fp);
      if (!fn_line || !file_line) {
        break;
      }
      current_frames.push_back(ParseFrame(std::move(fn_line).value(), std::move(file_line).value()));
      // peek if next line looks like address or we have more inlined frames
      peek_char = getc(s->stdout_fp);
      if (peek_char == EOF) {
        break;
      }
      ungetc(peek_char, s->stdout_fp);
    } while (peek_char != '0');
    std::swap(batch[target_idx++]->frames, current_frames);
  }

  if (ferror(s->stdout_fp)) {
    success = false;
  }

  auto kill_s = [s]() {
    if (!s->Alive())
      return;
    kill(s->pid, SIGKILL);
    s->pid = -1;
  };

  if (!success) {
    // make sure the writer isn't stuck trying to write to the bad
    // child
    kill_s();
  }

  batch_writer.join();

  if (success && ferror(s->stdout_fp)) {
    success = false;
  }

  if (!success) {
    fprintf(stderr, "add2line batch failed for %s\n", path.c_str());
    kill_s();
    fclose(s->stdout_fp);
    fclose(s->stdin_fp);
    s->stdin_fp = s->stdout_fp = nullptr;
  }
}

// `mem_fd` carries the whole request as SymRequest. I.e. array of
// addresses and text of caller's /proc/self/maps. It then reuses the
// same memfd to produce SymResponseHeader + many(SymResponseEntry) +
// blobs. Header is written last to let caller detect our premature
// death.
//
// We mmap request whole into memory first, keeping it simple.
static void HandleRequest(int mem_fd) {
  struct stat mem_fd_st;
  int rv = fstat(mem_fd, &mem_fd_st);
  if (rv < 0) {
    perror("fstat");
    return;
  }
  void* mmap_addr = mmap(nullptr, mem_fd_st.st_size, PROT_READ, MAP_SHARED, mem_fd, 0);
  if (mmap_addr == MAP_FAILED) {
    perror("mmap");
    return;
  }

  struct Unmap {
    void* addr;
    size_t sz;
    ~Unmap() {
      munmap(addr, sz);
    }
  };
  Unmap cleanup_mmap{mmap_addr, (size_t)mem_fd_st.st_size};

  std::vector<Request> all_requests;

  SymRequest* req = static_cast<SymRequest*>(cleanup_mmap.addr);
  for (size_t i = 0; i < req->count; i++) {
    all_requests.emplace_back((int)all_requests.size(), req->vaddrs[i]);
  }

  std::string_view full_request{reinterpret_cast<char*>(cleanup_mmap.addr), cleanup_mmap.sz};
  auto proc_maps_offset = offsetof(SymRequest, vaddrs) + req->count * sizeof(req->vaddrs[0]);
  std::vector<ProcVMA> maps = ParseProcMaps(full_request.substr(proc_maps_offset));

  if (maps.size() < 1) {
    fprintf(stderr, "empty proc-maps\n");
    return;
  }
  ElfCacheMap elf_cache;

  // 1. Correlate memory bounds and resolve to an exact ELF VMA
  for (auto& req : all_requests) {
    if (!req.valid)
      continue;

    const ProcVMA* matched_map = nullptr;
    for (const auto& m : maps) {
      if (req.orig_addr >= m.start && req.orig_addr < m.end) {
        matched_map = &m;
        break;
      }
    }
    if (!matched_map) {
      req.valid = false;
      continue;
    }

    uint64_t file_offset = req.orig_addr - matched_map->start + matched_map->offset;
    if (!GetElfVAddrAndPath(*matched_map, file_offset, &elf_cache, &req.elf_vaddr, &req.map_path)) {
      req.valid = false;
    }
  }

  std::unordered_map<std::string_view, std::vector<Request*>> by_path;
  for (auto& req : all_requests) {
    if (req.valid) {
      by_path[req.map_path].push_back(&req);
    }
  }

  // 2. Batch resolve per backing binary, via this binary's persistent
  //    addr2line session (spawned on first use, reused thereafter).
  for (const auto& [path, batch] : by_path) {
    RunAddr2LineBatch(batch[0]->map_path, batch);
  }

  // 3. Build the response as one entry per frame (a request with N inlined
  //    frames produces N entries, exactly as the netstring format used to),
  //    plus a flat blob every string is appended to as it is produced --
  //    each entry records where its strings landed by offset+length rather
  //    than repeating any delimiter-based framing.
  std::vector<SymResponseEntry> entries;
  std::string blob;
  auto append_to_blob = [&](std::string_view s, uint32_t* off, uint32_t* len) {
    *off = (uint32_t)blob.size();
    *len = (uint32_t)s.size();
    blob.append(s);
  };

  for (auto& req : all_requests) {
    if (req.frames.empty()) {
      req.frames.push_back(Frame{"", "", "", false});
    } else {
      for (size_t i = 0; i < req.frames.size() - 1; ++i) {
        req.frames[i].inlined = true;
      }
    }

    for (const auto& f : req.frames) {
      SymResponseEntry e = {};
      e.addr_index = (uint32_t)req.index;
      e.lineno = (uint32_t)atoi(f.line_num.c_str());
      e.inlined = f.inlined ? 1 : 0;
      e.vaddr = req.elf_vaddr;
      append_to_blob(f.func_name, &e.function_off, &e.function_len);
      append_to_blob(f.file_name, &e.filename_off, &e.filename_len);
      append_to_blob(req.map_path, &e.module_off, &e.module_len);
      entries.push_back(e);
    }
  }

  // 4. Commit: write the bulk data (entries +
  //    string blob), and only once that has fully succeeded, write the
  //    header -- with the real magic -- as one final, separate, tiny write.
  //    A crash at any point before that last write leaves offset 0 at its
  //    natural, post-truncate zero value, which a reader can tell apart
  //    from a genuine response; see kSymResponseMagic's own comment.
  lseek(mem_fd, sizeof(SymResponseHeader), SEEK_SET);

  size_t entries_bytes = entries.size() * sizeof(SymResponseEntry);
  if (write(mem_fd, entries.data(), entries_bytes) != (ssize_t)entries_bytes) {
    return;
  }
  if (write(mem_fd, blob.data(), blob.size()) != (ssize_t)blob.size()) {
    return;
  }
  SymResponseHeader header = {kSymResponseMagic, (uint32_t)entries.size()};
  // Nothing further to do if this last write fails.
  auto dummy = pwrite(mem_fd, &header, sizeof(header), 0);
  (void)dummy;  // glibc and gcc insist on using it
}

int main(int argc, char* argv[]) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <mailbox-fd-number>\n", argv[0]);
    return 1;
  }

  // We are a detached daemon (see SpawnHelperDaemon in
  // symbolize-backtrace.cc). Ignoring SIGCHLD makes the kernel reap our
  // addr2line children automatically as they exit, with no explicit wait()
  // loop needed; ignoring SIGPIPE means a caller that vanishes mid-response
  // just fails a write() rather than killing this daemon.
  signal(SIGCHLD, SIG_IGN);
  signal(SIGPIPE, SIG_IGN);
  // When process group gets Ctrl-C lets stay up to help
  // backtrace-comparer dump backtraces.
  signal(SIGINT, SIG_IGN);

  int orig_mailbox_fd = atoi(argv[1]);
  if (orig_mailbox_fd <= 0) {
    fprintf(stderr, "bug: orig_mailbox_fd <= 0\n");
    abort();
  }

  // make sure mailbox fd is not one of 0, 1 or 2.
  int mailbox_fd = fcntl(orig_mailbox_fd, F_DUPFD_CLOEXEC, 16);
  if (mailbox_fd < 0) {
    perror("dup3");
    fprintf(stderr, "orig_mailbox_fd = %d; argv[1] = %s\n", orig_mailbox_fd, argv[1]);
    return 1;
  }
  close(orig_mailbox_fd);

  // We inherited stdin/stdout/stderr from whatever process happened to spawn
  // us. This could be "garbage" FDs. Lets detach.
  for (int i = 0; i < 3; i++) {
    close(i);
    int fd;
#ifdef NDEBUG
    constexpr bool kWantTTY = false;
#else
    constexpr bool kWantTTY = true;
#endif
    if (kWantTTY && i == 2 && (fd = open("/dev/tty", O_WRONLY)) >= 0) {
      // succeeded connecting stderr to controlling terminal. Better
      // than nothing.
    } else {
      fd = open("/dev/null", (i == 0) ? O_RDONLY : O_WRONLY);
    }
    if (fd != i) {
      abort();  // bug
    }
  }

  for (int i = 3; i < mailbox_fd; i++) {
    close(i);
  }
  closefrom(mailbox_fd + 1);

  // Single-threaded request loop, deliberately: each request is handled to
  // completion before the next is even read, which is what lets every
  // per-binary addr2line session above be touched with no locking at all.
  // Symbolization is a diagnostic path, not a hot one, so a second caller
  // waiting for the first to finish is an acceptable, much simpler tradeoff
  // than a threaded/locked server.
  for (;;) {
    char one_byte;
    struct iovec iov = {&one_byte, sizeof(one_byte)};

    union {
      char buf[CMSG_SPACE(sizeof(int) * 2)];
      struct cmsghdr align;
    } cmsg_buf;

    struct msghdr msg = {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf.buf;
    msg.msg_controllen = sizeof(cmsg_buf.buf);

    ssize_t n = recvmsg(mailbox_fd, &msg, MSG_CMSG_CLOEXEC);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (n == 0) {
      break;
    }

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
        cmsg->cmsg_len != CMSG_LEN(sizeof(int) * 2)) {
      abort();  // malformed request (wrong fd count or none at all). Keep it simple.
    }
    int fds[2];
    memcpy(fds, CMSG_DATA(cmsg), sizeof(fds));
    int mem_fd = fds[0];
    int done_fd = fds[1];

    HandleRequest(mem_fd);
    close(mem_fd);
    close(done_fd);  // this is the completion signal the caller is waiting on
  }

  return 0;
}
