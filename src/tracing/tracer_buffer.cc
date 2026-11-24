// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2026, gperftools Contributors
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#include "config.h"

#include "tracing/tracer_buffer.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#if USE_LZ4
#include <lz4frame.h>
#endif

#include <new>

#include "base/spinlock.h"
#include "base/sysinfo.h"
#include "internal_logging.h"
#include "tracing/malloc_tracer.h"

namespace tcmalloc {

namespace {

constexpr int kBufsCount = 8;
constexpr int kOneBufSize = (64 << 20) / kBufsCount;
// We write trace with O_DIRECT (when possible), which needs write
// sizes (and thus file offsets) to be multiples of filesystem's
// direct IO granularity. That is commonly 512 bytes or 4k, but can
// be up to 64k (e.g. 64k block size filesystems). So we always write
// in multiples of 64k, except for the very last write (see
// WriteLast). Note, buffer address alignment requirements of
// O_DIRECT are much weaker, and page alignment is plenty for them.
constexpr int kDirectIOGranularity = 64 << 10;

// Buffer lifecycle is as follows:
//
//  a) space_sem = 0, ready_sem = 0: buffer is actively being written
//  to by TracerBuffer. write_buf has index of buffer
//
//  b) space_sem = 0, ready_sem = 1: buffer is ready to be saved
//  to_save_pos represents amount of data in buffer. Which is multiple
//  of page size (except for very last data).
//
//  c) space_sem = 0, ready_sem = 0: once saver acquires ready_sem
//  it is saving that buffer.
//
//  d) space_sem = 1, ready_sem = 0: once saver is done, it releases
//  space_sem, which represents that the buffer is ready to be filled
//  again.
//
// Note, we mmap those buffers only when tracing is active (see
// TracerBuffer::Create), since every tcmalloc variant has tracing
// support compiled in.
using OneBuf = char[kOneBufSize];
OneBuf* fd_buf;
int write_buf;
int to_save_pos[kBufsCount];
// Note, we use our own CountingSemaphore rather than sem_t, since
// arbitrary threads may wait for buffer space from deep inside malloc
// (and sem_wait is a cancellation point and can fail with EINTR).
CountingSemaphore space_sem[kBufsCount];
CountingSemaphore ready_sem[kBufsCount];

uint64_t total_saved;

class Writer {
 public:
  virtual ~Writer() = default;

  virtual void Write(const char* data, size_t amount) = 0;
  virtual void WriteLast(const char* data, size_t amount) = 0;
};

class DirectWriter : public Writer {
 public:
  explicit DirectWriter(int fd) : fd_(fd) {}

  void Write(const char* buf, size_t bytes) override {
    do {
      ssize_t rv = write(fd_, buf, bytes);
      if (rv < 0) {
        if (errno != EINTR) {
          perror("write");
          abort();
        }
        rv = 0;
      }
      bytes -= rv;
      buf += rv;
    } while (bytes > 0);
  }
  void WriteLast(const char* data, size_t amount) override {
    {
      // We might be doing direct io. And this last write is likely to
      // be less than page worth of data. So lets turn of direct io
      // just for that last block.
      int flags = fcntl(fd_, F_GETFL);
      if (flags < 0) {
        perror("fcntl(F_GETFL)");
        abort();
      }
      if ((flags & O_DIRECT) != 0) {
        flags &= ~O_DIRECT;
        fcntl(fd_, F_SETFL, flags);
      }
    }
    Write(data, amount);
    close(fd_);
  }

 private:
  int fd_;
};

#if USE_LZ4

constexpr int kPageSize = 4 << 10;

class LZ4Compressor : public Writer {
 public:
  explicit LZ4Compressor(Writer* slave);
  ~LZ4Compressor() override;

  void Write(const char* data, size_t amount) override;
  void WriteLast(const char* data, size_t amount) override;

 private:
  static constexpr int kMinAmountToSave = 3 << 20;
  static constexpr int kBlockSize = 16 << 10;
  int buf_tail_;
  LZ4F_cctx* lzctx_;
  Writer* const slave_;
  alignas(kPageSize) char buffer_[4 << 20];
};

LZ4Compressor::LZ4Compressor(Writer* slave) : buf_tail_(0), slave_(slave) {
  LZ4F_errorCode_t err = LZ4F_createCompressionContext(&lzctx_, LZ4F_VERSION);
  if (err != 0) {
    abort();
  }

  LZ4F_preferences_t prefs{};
  prefs.frameInfo.blockMode = LZ4F_blockIndependent;

  buf_tail_ = LZ4F_compressBegin(lzctx_, buffer_, sizeof(buffer_), &prefs);
}

LZ4Compressor::~LZ4Compressor() { LZ4F_freeCompressionContext(lzctx_); }

void LZ4Compressor::Write(const char* data, size_t amount) {
  while (amount > 0) {
    size_t this_write = kBlockSize;
    if (this_write > amount) {
      this_write = amount;
    }
    size_t written =
        LZ4F_compressUpdate(lzctx_, buffer_ + buf_tail_, sizeof(buffer_) - buf_tail_, data, this_write, nullptr);
    if (LZ4F_isError(written)) {
      abort();
    }

    buf_tail_ += written;
    data += this_write;
    amount -= this_write;

    if (buf_tail_ >= kMinAmountToSave) {
      int to_save = buf_tail_ & -kDirectIOGranularity;
      slave_->Write(buffer_, to_save);
      buf_tail_ -= to_save;
      memcpy(buffer_, buffer_ + to_save, buf_tail_);
    }
  }
}

void LZ4Compressor::WriteLast(const char* data, size_t amount) {
  Write(data, amount);

  size_t written = LZ4F_compressEnd(lzctx_, buffer_ + buf_tail_, sizeof(buffer_) - buf_tail_, nullptr);
  if (LZ4F_isError(written)) {
    abort();
  }
  buf_tail_ += written;

  slave_->WriteLast(buffer_, buf_tail_);
}

#endif  // USE_LZ4

CountingSemaphore saver_thread_started;

void* SaverThread(void* arg) {
  // Saver must never trace anything. See MarkCurrentThreadUntraced.
  MallocTracer::MarkCurrentThreadUntraced();
  saver_thread_started.Release();

  Writer* writer = static_cast<Writer*>(arg);

  int bufno = 0;
  bool wrote_last = false;
  while (true) {
    ready_sem[bufno].Acquire();
    int to_save = to_save_pos[bufno];
    if (to_save == 0) {
      break;
    }

    char* save_buf = fd_buf[bufno];

    // All buffers except the last one (see Finalize) are multiples
    // of kDirectIOGranularity, which O_DIRECT needs. The unaligned
    // tail of the last one goes via WriteLast (which handles
    // O_DIRECT and finishes compression).
    int aligned = to_save & -kDirectIOGranularity;
    if (writer) {
      if (aligned) {
        writer->Write(save_buf, aligned);
      }
      if (aligned != to_save) {
        writer->WriteLast(save_buf + aligned, to_save - aligned);
        wrote_last = true;
      }
    }
    total_saved += to_save;
    space_sem[bufno].Release();
    bufno = (bufno + 1) % kBufsCount;
  }

  if (writer && !wrote_last) {
    writer->WriteLast(nullptr, 0);
  }

  {
    char buf[4096];
    char* p = buf;
    p += snprintf(p, buf + sizeof(buf) - p, "total_saved = %" PRIu64 "\n", total_saved);
    MallocTracer::SPrintStats(p, buf + sizeof(buf));
    fprintf(stderr, "%s", buf);
  }

  // we've hit to_save = 0 case above and exited loop without
  // signalling buffer emptying. We do it here, signalling that saver
  // is fully done.
  space_sem[bufno].Release();

  return nullptr;
}

Writer* OpenTraceOutput() {
  int fd = -1;

  // Like for heap and cpu profiles, this arranges for exec-ed children
  // (which inherit TCMALLOC_TRACE_OUTPUT) to trace into separate files
  // with their pids appended (and handles HPC environments' suffixes
  // and explicit TCMALLOC_TRACE_OUTPUT_USE_PID).
  char filename[PATH_MAX];
  if (!GetUniquePathFromEnv("TCMALLOC_TRACE_OUTPUT", filename)) {
    return nullptr;
  }

  // O_NONBLOCK is for named pipes. O_CLOEXEC is so that exec-ed
  // children don't inherit trace fd (e.g. keeping pipe reader from
  // seeing EOF).
  fd = open(filename, O_WRONLY | O_CREAT | O_NONBLOCK | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    perror("open");
    fprintf(stderr, "will not save trace anywhere\n");
    return nullptr;
  }

  int flags = fcntl(fd, F_GETFL);
  if (flags < 0) {
    perror("fcntl(F_GETFL)");
    abort();
  }
  flags &= ~O_NONBLOCK;
  fcntl(fd, F_SETFL, flags);
  // O_DIRECT is good idea for regular files on disk
  fcntl(fd, F_SETFL, flags | O_DIRECT);

  // if output is pipe, we need larger buffer
  fcntl(fd, F_SETPIPE_SZ, 1 << 20);

  Writer* direct_writer = new DirectWriter(fd);

#if USE_LZ4
  if (getenv("TCMALLOC_TRACE_UNCOMPRESSED")) {
    return direct_writer;
  }
  return new LZ4Compressor(direct_writer);

#else
  return direct_writer;
#endif
}

}  // namespace

TracerBuffer::TracerBuffer() {
  space_sem[0].Acquire();
  SetBuffer(fd_buf[0], 0, kOneBufSize);
}

void TracerBuffer::RefreshInternal(int to_write) {
  CHECK_CONDITION(fd_buf[write_buf] == start_);

  int tail = current_ - start_ - to_write;

  int next_buf = (write_buf + 1) % kBufsCount;
  space_sem[next_buf].Acquire();

  memcpy(fd_buf[next_buf], fd_buf[write_buf] + to_write, tail);

  to_save_pos[write_buf] = to_write;
  ready_sem[write_buf].Release();

  write_buf = next_buf;
  SetBuffer(fd_buf[write_buf], tail, kOneBufSize);
}

void TracerBuffer::Refresh() { RefreshInternal((current_ - start_) & -kDirectIOGranularity); }

void TracerBuffer::Finalize() {
  if (!saver_started_) {
    // InitLate never ran, so there is nobody to save the trace.
    return;
  }

  RefreshInternal(current_ - start_);
  // signal saver thread that we're done
  RefreshInternal(0);

  // and wait until it is done. We're just taking "ownership" of all
  // buffers. Once we're done, we can be sure that saver cannot be
  // running and that our last 2 buffers we passed to saver got
  // through.
  for (int i = 1; i < kBufsCount; i++) {
    space_sem[(write_buf + i) % kBufsCount].Acquire();
  }
}

void TracerBuffer::InitLate() {
  Writer* writer = OpenTraceOutput();

  CreateServiceThread(SaverThread, writer);
  saver_thread_started.Acquire();

  saver_started_ = true;
}

void CreateServiceThread(void* (*body)(void*), void* arg) {
  // We block all signals except synchronous ones (i.e. generated by
  // executing faulting instruction). Blocking those is undefined
  // behavior (see sigprocmask(2), which lists SIGBUS, SIGFPE, SIGILL
  // and SIGSEGV). And on Linux, kernel then unblocks the signal and
  // resets its handler to default, so the program's crash handlers
  // (if any) would not run if our thread crashes. SIGTRAP and SIGSYS
  // (breakpoints and seccomp traps) are forced by kernel the same way.
  sigset_t mask, saved;
  sigfillset(&mask);
  for (int sig : {SIGBUS, SIGFPE, SIGILL, SIGSEGV, SIGTRAP, SIGSYS}) {
    sigdelset(&mask, sig);
  }
  // New thread inherits our signal mask.
  pthread_sigmask(SIG_SETMASK, &mask, &saved);

  pthread_t thread;
  int rv = pthread_create(&thread, nullptr, body, arg);

  pthread_sigmask(SIG_SETMASK, &saved, nullptr);

  if (rv != 0) {
    errno = rv;
    perror("pthread_create");
    abort();
  }
}

TracerBuffer* TracerBuffer::Create() {
  static StaticStorage<TracerBuffer> storage;

  void* bufs = mmap(nullptr, sizeof(OneBuf) * kBufsCount, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (bufs == MAP_FAILED) {
    perror("mmap");
    abort();
  }
  fd_buf = static_cast<OneBuf*>(bufs);

  for (int i = 0; i < kBufsCount; i++) {
    space_sem[i].Release();
  }

  return storage.Construct();
}

}  // namespace tcmalloc
