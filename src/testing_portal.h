/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
 * Copyright (c) 2024, gperftools Contributors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef TESTING_PORTAL_H_
#define TESTING_PORTAL_H_
#include "config.h"

#include <stdlib.h>
#include <string.h>

#include <gperftools/malloc_extension.h>

#include "base/function_ref.h"
#include "base/basictypes.h"

namespace tcmalloc {

class ATTRIBUTE_VISIBILITY_HIDDEN TestingPortal {
public:
  static inline constexpr char kMagic[] = "tcmalloc.impl.testing-portal";
  static TestingPortal* Get() {
    static TestingPortal* instance = ([] () {
      struct {
        TestingPortal* ptr = nullptr;
        size_t v = 0;
      } s;
      bool ok = MallocExtension::instance()->GetNumericProperty(kMagic, &s.v);
      if (!ok || s.ptr == nullptr) {
        abort();
      }
      return s.ptr;
    })();

    return instance;
  }
  static TestingPortal** CheckGetPortal(const char* property_name, size_t* value) {
    if (strcmp(property_name, kMagic) != 0) {
      return nullptr;
    }
    return reinterpret_cast<TestingPortal**>(value) - 1;
  }

  virtual bool HaveSystemRelease() = 0;
  virtual bool IsDebuggingMalloc() = 0;
  virtual size_t GetPageSize() = 0;
  virtual size_t GetMinAlign() = 0;
  virtual size_t GetMaxSize() = 0;
  virtual int64_t& GetSampleParameter() = 0;
  virtual double& GetReleaseRate() = 0;
  virtual int32_t& GetMaxFreeQueueSize() = 0;

  virtual bool HasEmergencyMalloc() = 0;
  virtual bool IsEmergencyPtr(void* ptr) = 0;
  virtual void WithEmergencyMallocEnabled(FunctionRef<void()> body) = 0;

protected:
  virtual ~TestingPortal();
};

}  // namespace tcmalloc

#endif  // TESTING_PORTAL_H_
