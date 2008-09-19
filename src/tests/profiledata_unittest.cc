// Copyright (c) 2007, Google Inc.
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
//
// ---
// Author: Chris Demetriou
//
// This file contains the unit tests for the ProfileData class.

#if defined HAVE_STDINT_H
#include <stdint.h>             // to get uintptr_t
#elif defined HAVE_INTTYPES_H
#include <inttypes.h>           // another place uintptr_t might be defined
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <string.h>
#include <string>

#include "profiledata.h"

#include "base/commandlineflags.h"
#include "base/logging.h"

using std::string;

// Some helpful macros for the test class
#define EXPECT_TRUE(cond)  CHECK(cond)
#define EXPECT_FALSE(cond) CHECK(!(cond))
#define EXPECT_EQ(a, b)    CHECK_EQ(a, b)
#define EXPECT_GT(a, b)    CHECK_GT(a, b)
#define EXPECT_LT(a, b)    CHECK_LT(a, b)
#define EXPECT_GE(a, b)    CHECK_GE(a, b)
#define EXPECT_LE(a, b)    CHECK_LE(a, b)
#define EXPECT_STREQ(a, b) CHECK(strcmp(a, b) == 0)
#define TEST_F(cls, fn)    void cls :: fn()


namespace {

// Re-runs fn until it doesn't cause EINTR.
#define NO_INTR(fn)   do {} while ((fn) < 0 && errno == EINTR)

// Thin wrapper around a file descriptor so that the file descriptor
// gets closed for sure.
struct FileDescriptor {
  const int fd_;
  explicit FileDescriptor(int fd) : fd_(fd) {}
  ~FileDescriptor() {
    if (fd_ >= 0) {
      NO_INTR(close(fd_));
    }
  }
  int get() { return fd_; }
};

// must be the same as with ProfileData::Slot.
typedef uintptr_t ProfileDataSlot;

// Quick and dirty function to make a number into a void* for use in a
// sample.
inline void* V(intptr_t x) { return reinterpret_cast<void*>(x); }

class ProfileDataChecker {
 public:
  ProfileDataChecker() {
    const char* tmpdir = getenv("TMPDIR");
    if (tmpdir == NULL)
      tmpdir = "/tmp";
    mkdir(tmpdir, 0755);     // if necessary
    filename_ = string(tmpdir) + "/profiledata_unittest.tmp";
  }

  string filename() const { return filename_; }

  void Check(const ProfileDataSlot* slots, int num_slots) {
    CheckWithSkips(slots, num_slots, NULL, 0);
  }

  void CheckWithSkips(const ProfileDataSlot* slots, int num_slots,
                      const int* skips, int num_skips) {
    FileDescriptor fd(open(filename_.c_str(), O_RDONLY));
    CHECK_GE(fd.get(), 0);

    ProfileDataSlot* filedata = new ProfileDataSlot[num_slots];
    size_t expected_bytes = num_slots * sizeof filedata[0];
    ssize_t bytes_read = read(fd.get(), filedata, expected_bytes);
    CHECK_EQ(expected_bytes, bytes_read);

    for (int i = 0; i < num_slots; i++) {
      if (num_skips > 0 && *skips == i) {
        num_skips--;
        skips++;
        continue;
      }
      CHECK_EQ(slots[i], filedata[i]);  // "first mismatch at slot " << i;
    }
    delete[] filedata;
  }

 private:
  string filename_;
};

class ProfileDataTest {
 protected:
  void ExpectStopped() {
    EXPECT_FALSE(collector_.enabled());
  }

  void ExpectRunningSamples(int samples) {
    ProfileData::State state;
    collector_.GetCurrentState(&state);
    EXPECT_TRUE(state.enabled);
    EXPECT_EQ(samples, state.samples_gathered);
  }

  void ExpectSameState(const ProfileData::State& before,
                       const ProfileData::State& after) {
    EXPECT_EQ(before.enabled, after.enabled);
    EXPECT_EQ(before.samples_gathered, after.samples_gathered);
    EXPECT_EQ(before.start_time, after.start_time);
    EXPECT_STREQ(before.profile_name, after.profile_name);
  }

  ProfileData        collector_;
  ProfileDataChecker checker_;

 private:
  // The tests to run
  void OpsWhenStopped();
  void StartStopEmpty();
  void StartStopNoOptionsEmpty();
  void StartWhenStarted();
  void StartStopEmpty2();
  void CollectOne();
  void CollectTwoMatching();
  void CollectTwoFlush();

 public:
#define RUN(test)  do {                         \
    printf("Running %s\n", #test);              \
    ProfileDataTest pdt;                        \
    pdt.test();                                 \
} while (0)

  static int RUN_ALL_TESTS() {
    RUN(OpsWhenStopped);
    RUN(StartStopEmpty);
    RUN(StartWhenStarted);
    RUN(StartStopEmpty2);
    RUN(CollectOne);
    RUN(CollectTwoMatching);
    RUN(CollectTwoFlush);
    return 0;
  }
};

// Check that various operations are safe when stopped.
TEST_F(ProfileDataTest, OpsWhenStopped) {
  ExpectStopped();
  EXPECT_FALSE(collector_.enabled());

  // Verify that state is disabled, all-empty/all-0
  ProfileData::State state_before;
  collector_.GetCurrentState(&state_before);
  EXPECT_FALSE(state_before.enabled);
  EXPECT_EQ(0, state_before.samples_gathered);
  EXPECT_EQ(0, state_before.start_time);
  EXPECT_STREQ("", state_before.profile_name);

  // Safe to call stop again.
  collector_.Stop();

  // Safe to call FlushTable.
  collector_.FlushTable();

  // Safe to call Add.
  const void *trace[] = { V(100), V(101), V(102), V(103), V(104) };
  collector_.Add(arraysize(trace), trace);

  ProfileData::State state_after;
  collector_.GetCurrentState(&state_after);

  ExpectSameState(state_before, state_after);
}

// Start and Stop, collecting no samples.  Verify output contents.
TEST_F(ProfileDataTest, StartStopEmpty) {
  const int frequency = 1;
  ProfileDataSlot slots[] = {
    0, 3, 0, 1000000 / frequency, 0,    // binary header
    0, 1, 0                             // binary trailer
  };

  ExpectStopped();
  ProfileData::Options options;
  options.set_frequency(frequency);
  EXPECT_TRUE(collector_.Start(checker_.filename().c_str(), options));
  ExpectRunningSamples(0);
  collector_.Stop();
  ExpectStopped();
  checker_.Check(slots, arraysize(slots));
}

// Start and Stop with no options, collecting no samples.  Verify
// output contents.
TEST_F(ProfileDataTest, StartStopNoOptionsEmpty) {
  // We're not requesting a specific period, implementation can do
  // whatever it likes.
  ProfileDataSlot slots[] = {
    0, 3, 0, 0 /* skipped */, 0,        // binary header
    0, 1, 0                             // binary trailer
  };
  int slots_to_skip[] = { 3 };

  ExpectStopped();
  EXPECT_TRUE(collector_.Start(checker_.filename().c_str(),
                               ProfileData::Options()));
  ExpectRunningSamples(0);
  collector_.Stop();
  ExpectStopped();
  checker_.CheckWithSkips(slots, arraysize(slots),
                          slots_to_skip, arraysize(slots_to_skip));
}

// Start after already started.  Should return false and not impact
// collected data or state.
TEST_F(ProfileDataTest, StartWhenStarted) {
  const int frequency = 1;
  ProfileDataSlot slots[] = {
    0, 3, 0, 1000000 / frequency, 0,    // binary header
    0, 1, 0                             // binary trailer
  };

  ProfileData::Options options;
  options.set_frequency(frequency);
  EXPECT_TRUE(collector_.Start(checker_.filename().c_str(), options));

  ProfileData::State state_before;
  collector_.GetCurrentState(&state_before);

  options.set_frequency(frequency * 2);
  CHECK(!collector_.Start("foobar", options));

  ProfileData::State state_after;
  collector_.GetCurrentState(&state_after);
  ExpectSameState(state_before, state_after);

  collector_.Stop();
  ExpectStopped();
  checker_.Check(slots, arraysize(slots));
}

// Like StartStopEmpty, but uses a different file name and frequency.
TEST_F(ProfileDataTest, StartStopEmpty2) {
  const int frequency = 2;
  ProfileDataSlot slots[] = {
    0, 3, 0, 1000000 / frequency, 0,    // binary header
    0, 1, 0                             // binary trailer
  };

  ExpectStopped();
  ProfileData::Options options;
  options.set_frequency(frequency);
  EXPECT_TRUE(collector_.Start(checker_.filename().c_str(), options));
  ExpectRunningSamples(0);
  collector_.Stop();
  ExpectStopped();
  checker_.Check(slots, arraysize(slots));
}

TEST_F(ProfileDataTest, CollectOne) {
  const int frequency = 2;
  ProfileDataSlot slots[] = {
    0, 3, 0, 1000000 / frequency, 0,    // binary header
    1, 5, 100, 101, 102, 103, 104,      // our sample
    0, 1, 0                             // binary trailer
  };

  ExpectStopped();
  ProfileData::Options options;
  options.set_frequency(frequency);
  EXPECT_TRUE(collector_.Start(checker_.filename().c_str(), options));
  ExpectRunningSamples(0);

  const void *trace[] = { V(100), V(101), V(102), V(103), V(104) };
  collector_.Add(arraysize(trace), trace);
  ExpectRunningSamples(1);

  collector_.Stop();
  ExpectStopped();
  checker_.Check(slots, arraysize(slots));
}

TEST_F(ProfileDataTest, CollectTwoMatching) {
  const int frequency = 2;
  ProfileDataSlot slots[] = {
    0, 3, 0, 1000000 / frequency, 0,    // binary header
    2, 5, 100, 201, 302, 403, 504,      // our two samples
    0, 1, 0                             // binary trailer
  };

  ExpectStopped();
  ProfileData::Options options;
  options.set_frequency(frequency);
  EXPECT_TRUE(collector_.Start(checker_.filename().c_str(), options));
  ExpectRunningSamples(0);

  for (int i = 0; i < 2; ++i) {
    const void *trace[] = { V(100), V(201), V(302), V(403), V(504) };
    collector_.Add(arraysize(trace), trace);
    ExpectRunningSamples(i + 1);
  }

  collector_.Stop();
  ExpectStopped();
  checker_.Check(slots, arraysize(slots));
}

TEST_F(ProfileDataTest, CollectTwoFlush) {
  const int frequency = 2;
  ProfileDataSlot slots[] = {
    0, 3, 0, 1000000 / frequency, 0,    // binary header
    1, 5, 100, 201, 302, 403, 504,      // first sample (flushed)
    1, 5, 100, 201, 302, 403, 504,      // second identical sample
    0, 1, 0                             // binary trailer
  };

  ExpectStopped();
  ProfileData::Options options;
  options.set_frequency(frequency);
  EXPECT_TRUE(collector_.Start(checker_.filename().c_str(), options));
  ExpectRunningSamples(0);

  const void *trace[] = { V(100), V(201), V(302), V(403), V(504) };

  collector_.Add(arraysize(trace), trace);
  ExpectRunningSamples(1);
  collector_.FlushTable();

  collector_.Add(arraysize(trace), trace);
  ExpectRunningSamples(2);

  collector_.Stop();
  ExpectStopped();
  checker_.Check(slots, arraysize(slots));
}

}  // namespace

int main(int argc, char** argv) {
  int rc = ProfileDataTest::RUN_ALL_TESTS();
  printf("%s\n", rc == 0 ? "PASS" : "FAIL");
  return rc;
}
