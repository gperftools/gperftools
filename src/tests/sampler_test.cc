// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// Copyright (c) 2008, Google Inc.
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

// ---
// All Rights Reserved.
//
// Author: Daniel Ford
//
// Checks basic properties of the sampler

#include "config_for_unittests.h"

#include "sampler.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#include <algorithm>
#include <iostream>
#include <math.h>
#include <string>
#include <vector>

#include "base/commandlineflags.h"

#include "gtest/gtest.h"

#undef LOG   // defined in base/logging.h
// Ideally, we'd put the newline at the end, but this hack puts the
// newline at the end of the previous log message, which is good enough :-)
#define LOG(level)  std::cerr << "\n"

static std::string StringPrintf(const char* format, ...) {
  char buf[256];   // should be big enough for all logging
  va_list ap;
  va_start(ap, format);
  vsnprintf(buf, sizeof(buf), format, ap);
  va_end(ap);
  return buf;
}

// Note that these tests are stochastic.
// This mean that the chance of correct code passing the test is,
// in the case of 5 standard deviations:
// kSigmas=5:    ~99.99994267%
// in the case of 4 standard deviations:
// kSigmas=4:    ~99.993666%
static const double kSigmas = 4;
static const size_t kSamplingInterval = 512*1024;

DECLARE_int64(tcmalloc_sample_parameter);

class SamplerTest : public ::testing::Test {
public:
  void SetUp() {
    // Make sure Sampler's TrivialOnce logic runs before we're messing
    // up with sample parameter.
    tcmalloc::Sampler{}.Init(1);

    old_parameter_ = 512 << 10;
    std::swap(old_parameter_, FLAGS_tcmalloc_sample_parameter);
  }
  void TearDown() {
    std::swap(old_parameter_, FLAGS_tcmalloc_sample_parameter);
  }
private:
  int64_t old_parameter_;
};

// Tests that GetSamplePeriod returns the expected value
// which is 1<<19
TEST_F(SamplerTest, TestGetSamplePeriod) {
  tcmalloc::Sampler sampler;
  sampler.Init(1);
  uint64_t sample_period;
  sample_period = sampler.GetSamplePeriod();
  ASSERT_GT(sample_period, 0);
}

// Tests of the quality of the random numbers generated
// This uses the Anderson Darling test for uniformity.
// See "Evaluating the Anderson-Darling Distribution" by Marsaglia
// for details.

// Short cut version of ADinf(z), z>0 (from Marsaglia)
// This returns the p-value for Anderson Darling statistic in
// the limit as n-> infinity. For finite n, apply the error fix below.
double AndersonDarlingInf(double z) {
  if (z < 2) {
    return exp(-1.2337141 / z) / sqrt(z) * (2.00012 + (0.247105 -
                (0.0649821 - (0.0347962 - (0.011672 - 0.00168691
                * z) * z) * z) * z) * z);
  }
  return exp( - exp(1.0776 - (2.30695 - (0.43424 - (0.082433 -
                    (0.008056 - 0.0003146 * z) * z) * z) * z) * z));
}

// Corrects the approximation error in AndersonDarlingInf for small values of n
// Add this to AndersonDarlingInf to get a better approximation
// (from Marsaglia)
double AndersonDarlingErrFix(int n, double x) {
  if (x > 0.8) {
    return (-130.2137 + (745.2337 - (1705.091 - (1950.646 -
            (1116.360 - 255.7844 * x) * x) * x) * x) * x) / n;
  }
  double cutoff = 0.01265 + 0.1757 / n;
  double t;
  if (x < cutoff) {
    t = x / cutoff;
    t = sqrt(t) * (1 - t) * (49 * t - 102);
    return t * (0.0037 / (n * n) + 0.00078 / n + 0.00006) / n;
  } else {
    t = (x - cutoff) / (0.8 - cutoff);
    t = -0.00022633 + (6.54034 - (14.6538 - (14.458 - (8.259 - 1.91864
          * t) * t) * t) * t) * t;
    return t * (0.04213 + 0.01365 / n) / n;
  }
}

// Returns the AndersonDarling p-value given n and the value of the statistic
double AndersonDarlingPValue(int n, double z) {
  double ad = AndersonDarlingInf(z);
  double errfix = AndersonDarlingErrFix(n, ad);
  return ad + errfix;
}

double AndersonDarlingStatistic(int n, double* random_sample) {
  double ad_sum = 0;
  for (int i = 0; i < n; i++) {
    ad_sum += (2*i + 1) * log(random_sample[i] * (1 - random_sample[n-1-i]));
  }
  double ad_statistic = - n - 1/static_cast<double>(n) * ad_sum;
  return ad_statistic;
}

// Tests if the array of doubles is uniformly distributed.
// Returns the p-value of the Anderson Darling Statistic
// for the given set of sorted random doubles
// See "Evaluating the Anderson-Darling Distribution" by
// Marsaglia and Marsaglia for details.
double AndersonDarlingTest(int n, double* random_sample) {
  double ad_statistic = AndersonDarlingStatistic(n, random_sample);
  LOG(INFO) << StringPrintf("AD stat = %f, n=%d\n", ad_statistic, n);
  double p = AndersonDarlingPValue(n, ad_statistic);
  return p;
}

// Test the AD Test. The value of the statistic should go to zero as n->infty
// Not run as part of regular tests
void ADTestTest(int n) {
  std::unique_ptr<double[]> random_sample(new double[n]);
  for (int i = 0; i < n; i++) {
    random_sample[i] = (i+0.01)/n;
  }
  std::sort(random_sample.get(), random_sample.get() + n);
  double ad_stat = AndersonDarlingStatistic(n, random_sample.get());
  LOG(INFO) << StringPrintf("Testing the AD test. n=%d, ad_stat = %f",
                            n, ad_stat);
}

// Print the CDF of the distribution of the Anderson-Darling Statistic
// Used for checking the Anderson-Darling Test
// Not run as part of regular tests
void ADCDF() {
  for (int i = 1; i < 40; i++) {
    double x = i/10.0;
    LOG(INFO) << "x= " << x << "  adpv= "
              << AndersonDarlingPValue(100, x) << ", "
              << AndersonDarlingPValue(1000, x);
  }
}

// Testing that NextRandom generates uniform
// random numbers.
// Applies the Anderson-Darling test for uniformity
void TestNextRandom(int n) {
  tcmalloc::Sampler sampler;
  sampler.Init(1);
  uint64_t x = 1;
  // This assumes that the prng returns 48 bit numbers
  uint64_t max_prng_value = static_cast<uint64_t>(1)<<48;
  // Initialize
  for (int i = 1; i <= 20; i++) {  // 20 mimics sampler.Init()
    x = sampler.NextRandom(x);
  }
  std::unique_ptr<uint64_t[]> int_random_sample(new uint64_t[n]);
  // Collect samples
  for (int i = 0; i < n; i++) {
    int_random_sample[i] = x;
    x = sampler.NextRandom(x);
  }
  // First sort them...
  std::sort(int_random_sample.get(), int_random_sample.get() + n);
  std::unique_ptr<double[]> random_sample(new double[n]);
  // Convert them to uniform randoms (in the range [0,1])
  for (int i = 0; i < n; i++) {
    random_sample[i] = static_cast<double>(int_random_sample[i])/max_prng_value;
  }
  // Now compute the Anderson-Darling statistic
  double ad_pvalue = AndersonDarlingTest(n, random_sample.get());
  LOG(INFO) << StringPrintf("pvalue for AndersonDarlingTest "
                            "with n= %d is p= %f\n", n, ad_pvalue);
  ASSERT_GT(std::min(ad_pvalue, 1 - ad_pvalue), 0.0001)
            << StringPrintf("prng is not uniform, %d\n", n);
}


TEST_F(SamplerTest, TestNextRandom_MultipleValues) {
  ASSERT_NO_FATAL_FAILURE(TestNextRandom(10));  // Check short-range correlation
  ASSERT_NO_FATAL_FAILURE(TestNextRandom(100));
  ASSERT_NO_FATAL_FAILURE(TestNextRandom(1000));
  ASSERT_NO_FATAL_FAILURE(TestNextRandom(10000));  // Make sure there's no systematic error
}

// Tests that PickNextSamplePeriod generates
// geometrically distributed random numbers.
// First converts to uniforms then applied the
// Anderson-Darling test for uniformity.
void TestPickNextSample(int n) {
  tcmalloc::Sampler sampler;
  sampler.Init(1);
  std::unique_ptr<uint64_t[]> int_random_sample(new uint64_t[n]);
  int sample_period = sampler.GetSamplePeriod();
  int ones_count = 0;
  for (int i = 0; i < n; i++) {
    int_random_sample[i] = sampler.PickNextSamplingPoint();
    ASSERT_GE(int_random_sample[i], 1);
    if (int_random_sample[i] == 1) {
      ones_count += 1;
    }
    ASSERT_LT(ones_count, 4) << " out of " << i << " samples.";
  }
  // First sort them...
  std::sort(int_random_sample.get(), int_random_sample.get() + n);
  std::unique_ptr<double[]> random_sample(new double[n]);
  // Convert them to uniform random numbers
  // by applying the geometric CDF
  for (int i = 0; i < n; i++) {
    random_sample[i] = 1 - exp(-static_cast<double>(int_random_sample[i])
                           / sample_period);
  }
  // Now compute the Anderson-Darling statistic
  double geom_ad_pvalue = AndersonDarlingTest(n, random_sample.get());
  LOG(INFO) << StringPrintf("pvalue for geometric AndersonDarlingTest "
                             "with n= %d is p= %f\n", n, geom_ad_pvalue);
  ASSERT_GT(std::min(geom_ad_pvalue, 1 - geom_ad_pvalue), 0.0001)
               << "PickNextSamplingPoint does not produce good "
                  "geometric/exponential random numbers\n";
}

TEST_F(SamplerTest, TestPickNextSample_MultipleValues) {
  ASSERT_NO_FATAL_FAILURE(TestPickNextSample(10));  // Make sure the first few are good (enough)
  ASSERT_NO_FATAL_FAILURE(TestPickNextSample(100));
  ASSERT_NO_FATAL_FAILURE(TestPickNextSample(1000));
  ASSERT_NO_FATAL_FAILURE(TestPickNextSample(10000));  // Make sure there's no systematic erro)r
}


// Futher tests

bool CheckMean(size_t mean, int num_samples) {
  tcmalloc::Sampler sampler;
  sampler.Init(1);
  size_t total = 0;
  for (int i = 0; i < num_samples; i++) {
    total += sampler.PickNextSamplingPoint();
  }
  double empirical_mean = total / static_cast<double>(num_samples);
  double expected_sd = mean / pow(num_samples * 1.0, 0.5);
  return(fabs(mean-empirical_mean) < expected_sd * kSigmas);
}

// Prints a sequence so you can look at the distribution
void OutputSequence(int sequence_length) {
  tcmalloc::Sampler sampler;
  sampler.Init(1);
  size_t next_step;
  for (int i = 0; i< sequence_length; i++) {
    next_step = sampler.PickNextSamplingPoint();
    LOG(INFO) << next_step;
  }
}


double StandardDeviationsErrorInSample(
              int total_samples, int picked_samples,
              int alloc_size, int sampling_interval) {
  double p = 1 - exp(-(static_cast<double>(alloc_size) / sampling_interval));
  double expected_samples = total_samples * p;
  double sd = pow(p*(1-p)*total_samples, 0.5);
  return((picked_samples - expected_samples) / sd);
}

TEST_F(SamplerTest, LargeAndSmallAllocs_CombinedTest) {
  tcmalloc::Sampler sampler;
  sampler.Init(1);
  int counter_big = 0;
  int counter_small = 0;
  int size_big = 129*8*1024+1;
  int size_small = 1024*8;
  int num_iters = 128*4*8;
  // Allocate in mixed chunks
  for (int i = 0; i < num_iters; i++) {
    if (!sampler.RecordAllocation(size_big)) {
      counter_big += 1;
    }
    for (int i = 0; i < 129; i++) {
      if (!sampler.RecordAllocation(size_small)) {
        counter_small += 1;
      }
    }
  }
  // Now test that there are the right number of each
  double large_allocs_sds =
     StandardDeviationsErrorInSample(num_iters, counter_big,
                                     size_big, kSamplingInterval);
  double small_allocs_sds =
     StandardDeviationsErrorInSample(num_iters*129, counter_small,
                                     size_small, kSamplingInterval);
  LOG(INFO) << StringPrintf("large_allocs_sds = %f\n", large_allocs_sds);
  LOG(INFO) << StringPrintf("small_allocs_sds = %f\n", small_allocs_sds);
  ASSERT_LE(fabs(large_allocs_sds), kSigmas);
  ASSERT_LE(fabs(small_allocs_sds), kSigmas);
}

// Tests whether the mean is about right over 1000 samples
TEST_F(SamplerTest, IsMeanRight) {
  ASSERT_TRUE(CheckMean(kSamplingInterval, 1000));
}

// This checks that the stated maximum value for the
// tcmalloc_sample_parameter flag never overflows bytes_until_sample_
TEST_F(SamplerTest, bytes_until_sample_Overflow_Underflow) {
  tcmalloc::Sampler sampler;
  sampler.Init(1);
  uint64_t one = 1;
  // sample_parameter = 0;  // To test the edge case
  uint64_t sample_parameter_array[4] = {0, 1, one<<19, one<<58};
  for (int i = 0; i < 4; i++) {
    uint64_t sample_parameter = sample_parameter_array[i];
    LOG(INFO) << "sample_parameter = " << sample_parameter;
    double sample_scaling = - log(2.0) * sample_parameter;
    // Take the top 26 bits as the random number
    // (This plus the 1<<26 sampling bound give a max step possible of
    // 1209424308 bytes.)
    const uint64_t prng_mod_power = 48;  // Number of bits in prng

    // First, check the largest_prng value
    uint64_t largest_prng_value = (static_cast<uint64_t>(1)<<48) - 1;
    double q = (largest_prng_value >> (prng_mod_power - 26)) + 1.0;
    LOG(INFO) << StringPrintf("q = %f\n", q);
    LOG(INFO) << StringPrintf("log2(q) = %f\n", log(q)/log(2.0));
    uint64_t smallest_sample_step
      = static_cast<uint64_t>(std::min(log2(q) - 26, 0.0)
                              * sample_scaling + 1);
    LOG(INFO) << "Smallest sample step is " << smallest_sample_step;
    uint64_t cutoff = static_cast<uint64_t>(10)
                      * (sample_parameter/(one<<24) + 1);
    LOG(INFO) << "Acceptable value is < " << cutoff;
    // This checks that the answer is "small" and positive
    ASSERT_LE(smallest_sample_step, cutoff);

    // Next, check with the smallest prng value
    uint64_t smallest_prng_value = 0;
    q = (smallest_prng_value >> (prng_mod_power - 26)) + 1.0;
    LOG(INFO) << StringPrintf("q = %f\n", q);
    uint64_t largest_sample_step
      = static_cast<uint64_t>(std::min(log2(q) - 26, 0.0)
                              * sample_scaling + 1);
    LOG(INFO) << "Largest sample step is " << largest_sample_step;
    ASSERT_LE(largest_sample_step, one<<63);
    ASSERT_GE(largest_sample_step, smallest_sample_step);
  }
}


// Test that NextRand is in the right range.  Unfortunately, this is a
// stochastic test which could miss problems.
TEST_F(SamplerTest, NextRand_range) {
  tcmalloc::Sampler sampler;
  sampler.Init(1);
  uint64_t one = 1;
  // The next number should be (one << 48) - 1
  uint64_t max_value = (one << 48) - 1;
  uint64_t x = (one << 55);
  int n = 22;  // 27;
  LOG(INFO) << "Running sampler.NextRandom 1<<" << n << " times";
  for (int i = 1; i <= (1<<n); i++) {  // 20 mimics sampler.Init()
    x = sampler.NextRandom(x);
    ASSERT_LE(x, max_value);
  }
}

// Tests certain arithmetic operations to make sure they compute what we
// expect them too (for testing across different platforms)
TEST_F(SamplerTest, arithmetic_1) {
  tcmalloc::Sampler sampler;
  sampler.Init(1);
  uint64_t rnd;  // our 48 bit random number, which we don't trust
  const uint64_t prng_mod_power = 48;
  uint64_t one = 1;
  rnd = one;
  uint64_t max_value = (one << 48) - 1;
  for (int i = 1; i <= (1>>27); i++) {  // 20 mimics sampler.Init()
    rnd = sampler.NextRandom(rnd);
    ASSERT_LE(rnd, max_value);
    double q = (rnd >> (prng_mod_power - 26)) + 1.0;
    ASSERT_GE(q, 0) << rnd << "  " << prng_mod_power;
  }
  // Test some potentially out of bounds value for rnd
  for (int i = 1; i <= 63; i++) {
    rnd = one << i;
    double q = (rnd >> (prng_mod_power - 26)) + 1.0;
    LOG(INFO) << "rnd = " << rnd << " i=" << i << " q=" << q;
    ASSERT_GE(q, 0)
      << " rnd=" << rnd << "  i=" << i << " prng_mod_power" << prng_mod_power;
  }
}

void test_arithmetic(uint64_t rnd) {
  const uint64_t prng_mod_power = 48;  // Number of bits in prng
  uint64_t shifted_rnd = rnd >> (prng_mod_power - 26);
  ASSERT_GE(shifted_rnd, 0);
  ASSERT_LT(shifted_rnd, (1<<26));
  LOG(INFO) << shifted_rnd;
  LOG(INFO) << static_cast<double>(shifted_rnd);
  ASSERT_GE(static_cast<double>(static_cast<uint32_t>(shifted_rnd)), 0)
    << " rnd=" << rnd << "  srnd=" << shifted_rnd;
  ASSERT_GE(static_cast<double>(shifted_rnd), 0)
    << " rnd=" << rnd << "  srnd=" << shifted_rnd;
  double q = static_cast<double>(shifted_rnd) + 1.0;
  ASSERT_GT(q, 0);
}

// Tests certain arithmetic operations to make sure they compute what we
// expect them too (for testing across different platforms)
// know bad values under with -c dbg --cpu piii for _some_ binaries:
// rnd=227453640600554
// shifted_rnd=54229173
// (hard to reproduce)
TEST_F(SamplerTest, arithmetic_2) {
  uint64_t rnd = 227453640600554LL;
  test_arithmetic(rnd);
}


// It's not really a test, but it's good to know
TEST_F(SamplerTest, size_of_class) {
  tcmalloc::Sampler sampler;
  sampler.Init(1);
  LOG(INFO) << "Size of Sampler class is: " << sizeof(tcmalloc::Sampler);
  LOG(INFO) << "Size of Sampler object is: " << sizeof(sampler);
}
