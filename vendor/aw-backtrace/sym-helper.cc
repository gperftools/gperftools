/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
//
// This file contains a helper program that is spawned by
// symbolize-backtrace.cc. It reads /proc/$pid/map, then it does the
// necessary "data massaging" (addr2lines need vaddrs, we got offsets
// from mmap-ed data) to invoke {llvm-,}addr2line for actual
// symbolization and then communicates the results back over stdout
// via simple netstrings-based format.
//
// Since it is a separate binary (instead of being in
// async-signal-safe context in symbolize-backtrace) it does
// straightforward things using plain normal C and C++ APIs.

#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct MemoryMap {
  uint64_t start;
  uint64_t end;
  uint64_t offset;
  std::string path;
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
  uint64_t elf_vaddr;
  std::vector<Frame> frames;
};

struct ProgHeaderInfo {
  uint64_t vaddr;
  uint64_t off;
  uint64_t memsz;
};

using ElfCacheMap = std::map<std::string, std::vector<ProgHeaderInfo>>;

template <typename Fn>
static void ForEachLine(FILE* f, Fn&& body) {
  char* line_buf = nullptr;
  size_t line_len = 0;
  ssize_t nread;
  while ((nread = getline(&line_buf, &line_len, f)) != -1) {
    while (nread > 0 && line_buf[nread - 1] == '\n') {
      line_buf[--nread] = '\0';
    }
    if (!body(std::string_view(line_buf, nread))) {
      break;
    }
  }
  free(line_buf);
}

static std::vector<MemoryMap> ParseMaps(const std::string& pid) {
  std::vector<MemoryMap> maps;
  std::string maps_path = "/proc/" + pid + "/maps";
  FILE* f = fopen(maps_path.c_str(), "r");
  if (!f)
    return maps;

  // The proc-maps line looks like this: (man 5 proc_pid_maps)
  // 7ffff7f78000-7ffff7f7a000 rw-p 001e7000 103:02 1338729752                /usr/lib/x86_64-linux-gnu/libc.so.6
  // First "field" is address range, then perms, then file offset.
  ForEachLine(f, [&](std::string_view line) {
    size_t idx = 0;
    for (int col = 0; col < 5; ++col) {
      while (idx < line.size() && line[idx] != ' ') idx++;
      while (idx < line.size() && line[idx] == ' ') idx++;
    }
    if (idx >= line.size())
      return true;

    std::string_view map_path = line.substr(idx);
    while (!map_path.empty() && (map_path.back() == ' ' || map_path.back() == '\r' || map_path.back() == '\n')) {
      map_path.remove_suffix(1);
    }
    static constexpr std::string_view kDeletedSuffix = " (deleted)";
    if (map_path.ends_with(kDeletedSuffix)) {
      map_path.remove_suffix(kDeletedSuffix.size());
    }

    if (!map_path.starts_with('/'))
      return true;

    char field0[64] = {0};
    char perms[16] = {0};
    char field2[64] = {0};
    std::string line_str(line);
    if (sscanf(line_str.c_str(), "%63s %15s %63s", field0, perms, field2) == 3) {
      char* dash = strchr(field0, '-');
      if (dash) {
        *dash = '\0';
        uint64_t start = strtoull(field0, nullptr, 16);
        uint64_t end = strtoull(dash + 1, nullptr, 16);
        uint64_t offset = strtoull(field2, nullptr, 16);
        maps.push_back({start, end, offset, std::string(map_path)});
      }
    }
    return true;
  });

  fclose(f);
  return maps;
}

static std::vector<ProgHeaderInfo> ReadElfLoadSegments(const std::string& path) {
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

  std::vector<ProgHeaderInfo> phdrs;
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

static bool GetElfVAddrAndPath(const std::string& pid, const MemoryMap& m, uint64_t file_offset, ElfCacheMap& elf_cache,
                               uint64_t* out_vaddr, std::string* out_path) {
  const std::vector<ProgHeaderInfo>* phdrs = nullptr;
  std::string actual_path;

  auto try_path = [&](const std::string& path) -> bool {
    auto [it, inserted] = elf_cache.try_emplace(path);
    if (inserted) {
      it->second = ReadElfLoadSegments(path);
      if (it->second.empty()) {
        elf_cache.erase(it);
        return false;
      }
    }

    phdrs = &it->second;
    actual_path = path;
    return true;
  };

  if (!try_path(m.path)) {
    struct stat st;
    if (stat(m.path.c_str(), &st) != 0) {
      char buf[128];
      snprintf(buf, sizeof(buf), "/proc/%s/map_files/%lx-%lx", pid.c_str(), m.start, m.end);
      if (!try_path(buf)) {
        try_path("/proc/" + pid + "/root" + m.path);
      }
    }
  }

  if (!phdrs)
    return false;

  for (const auto& phdr : *phdrs) {
    if (file_offset >= phdr.off && file_offset < phdr.off + phdr.memsz) {
      *out_vaddr = phdr.vaddr + (file_offset - phdr.off);
      *out_path = actual_path;
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

static void WriteNetstring(FILE* out, const std::string& s) {
  fprintf(out, "%zu:%s,", s.size(), s.c_str());
}

static void BatchAddr2Line(const std::string& path, const std::vector<Request*>& batch) {
  int pipefd[2];
  if (pipe(pipefd) != 0)
    return;

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return;
  }

  if (pid == 0) {
    // Unlike the equivalent dance in symbolize-backtrace.cc's WithSpawnedChild,
    // this one needs no guard against pipefd landing on STDOUT_FILENO: our own
    // stdout is the netstring pipe our parent installed, so it is always open
    // and pipe() above can never have handed us descriptor 1. Keep it that way.
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);

    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }

    std::vector<std::string> arg_storage = {"llvm-addr2line", "-a", "-f", "-i", "-C", "-e", path};

    for (const auto* req : batch) {
      char addr_buf[32];
      snprintf(addr_buf, sizeof(addr_buf), "0x%lx", req->elf_vaddr);
      arg_storage.push_back(addr_buf);
    }

    std::vector<char*> argv_ptrs;
    for (auto& s : arg_storage) {
      argv_ptrs.push_back(s.data());
    }
    argv_ptrs.push_back(nullptr);

    // We try llvm-addr2line and GNU addr2line in order. Both speak
    // the format ParseFrame() expects (llvm's leaves the address line
    // unpadded, which is_address() does not care about).
    execvp(argv_ptrs[0], argv_ptrs.data());
    argv_ptrs[0] = const_cast<char*>("addr2line");
    execvp(argv_ptrs[0], argv_ptrs.data());
    _exit(127);
  }

  close(pipefd[1]);

  FILE* fp = fdopen(pipefd[0], "r");
  if (!fp) {
    close(pipefd[0]);
    waitpid(pid, nullptr, 0);
    return;
  }

  size_t target_idx = 0;
  std::vector<Frame> current_frames;
  char* line_buf = nullptr;
  size_t line_len = 0;

  auto next_line = [&]() -> std::optional<std::string> {
    ssize_t nread = getline(&line_buf, &line_len, fp);
    if (nread < 0) {
      return std::nullopt;
    }
    if (nread > 0 && line_buf[nread - 1] == '\n') {
      nread--;
    }
    return std::make_optional<std::string>(line_buf, static_cast<size_t>(nread));
  };

  // addr2line outputs: address-line \n (function-name \n file:line \n)+
  //
  // I.e. sequence of function-name file:line pairs given inlinings then next address etc.
  for (;;) {
    std::optional<std::string> function_line = next_line();
    if (!function_line) {
      break;
    }

    auto is_address = [](const std::string& line, uint64_t expected) -> bool {
      if (!line.starts_with("0x"))
        return false;
      const char* buf = line.c_str();
      char* endptr = nullptr;
      uint64_t val = strtoull(buf, &endptr, 16);
      return endptr != buf && *endptr == '\0' && val == expected;
    };

    // Line could be another function name or address.
    if (target_idx < batch.size() && is_address(function_line.value(), batch[target_idx]->elf_vaddr)) {
      // if it is address we append current set of frames to previous request and start new request
      if (target_idx > 0) {
        std::swap(batch[target_idx - 1]->frames, current_frames);
      }
      target_idx++;
      continue;
    }

    std::optional<std::string> file_line = next_line();
    if (!file_line) {
      break;
    }

    current_frames.push_back(ParseFrame(std::move(*function_line), *file_line));
  }

  if (target_idx > 0) {
    batch[target_idx - 1]->frames = std::move(current_frames);
  }

  free(line_buf);
  fclose(fp);
  waitpid(pid, nullptr, 0);
}

int main(int argc, char* argv[]) {
  if (argc < 3) {
    fprintf(stderr, "Usage: %s <pid> <addr1> [<addr2> ...]\n", argv[0]);
    return 1;
  }

  std::string pid = argv[1];
  std::vector<Request> all_requests;

  // 1. Parse requested arguments
  for (int i = 2; i < argc; ++i) {
    Request req;
    req.index = i - 2;
    req.valid = false;
    char* endptr = nullptr;
    uint64_t addr = strtoull(argv[i], &endptr, 0);
    if (endptr != argv[i]) {
      req.orig_addr = addr;
      req.valid = true;
    }
    all_requests.push_back(std::move(req));
  }

  std::vector<MemoryMap> maps = ParseMaps(pid);
  ElfCacheMap elf_cache;

  // 2. Correlate memory bounds and resolve to an exact ELF VMA
  for (auto& req : all_requests) {
    if (!req.valid)
      continue;

    const MemoryMap* matched_map = nullptr;
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
    uint64_t vaddr = 0;
    std::string actual_path;
    if (!GetElfVAddrAndPath(pid, *matched_map, file_offset, elf_cache, &vaddr, &actual_path)) {
      req.valid = false;
      continue;
    }

    req.map_path = actual_path;
    req.elf_vaddr = vaddr;
  }

  std::map<std::string, std::vector<Request*>> by_path;
  for (auto& req : all_requests) {
    if (req.valid) {
      by_path[req.map_path].push_back(&req);
    }
  }

  // 3. Batch invoke addr2line asynchronously per backing binary
  for (const auto& [path, batch] : by_path) {
    BatchAddr2Line(path, batch);
  }

  // 4. Safely flush out DJB-Netstrings sequence back to stdout memory file descriptor
  for (auto& req : all_requests) {
    if (req.frames.empty()) {
      req.frames.push_back(Frame{"", "", "", false});
    } else {
      for (size_t i = 0; i < req.frames.size() - 1; ++i) {
        req.frames[i].inlined = true;
      }
    }

    std::string idx_str = std::to_string(req.index);
    std::string module_path;
    std::string vaddr_hex;
    if (req.valid && !req.map_path.empty()) {
      module_path = req.map_path;
      char buf[32];
      snprintf(buf, sizeof(buf), "0x%lx", req.elf_vaddr);
      vaddr_hex = buf;
    }
    for (const auto& f : req.frames) {
      WriteNetstring(stdout, idx_str);
      WriteNetstring(stdout, f.func_name);
      WriteNetstring(stdout, f.file_name);
      WriteNetstring(stdout, f.line_num);
      WriteNetstring(stdout, f.inlined ? "1" : "0");
      WriteNetstring(stdout, module_path);
      WriteNetstring(stdout, vaddr_hex);
      fputc('\n', stdout);
    }
  }

  return 0;
}
