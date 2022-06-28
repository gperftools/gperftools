// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
/* Copyright (c) 2009, Google Inc.
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
 *
 * ---
 * This file is a Win32-specific part of spinlock_internal.cc
 */


#include <windows.h>

namespace base {
namespace internal {
    void SpinLockDelay(volatile Atomic32 *w, int32 value, int loop) {
        if (loop != 0) {
            auto wait_ns = static_cast<uint64_t>(base::internal::SuggestedDelayNS(loop)) * 16ull;
            auto wait_ms = wait_ns / 1000000ull;

            //Waits for the value at the specified address to change.
            WaitOnAddress(w, &value, 4u, static_cast<DWORD>(wait_ms));
        }
    }

    void SpinLockWake(volatile Atomic32 *w, bool all) {
        if (all) WakeByAddressAll((void*)w);///< Wakes all threads that are waiting for the value of an address to change.
        else WakeByAddressSingle((void*)w);
    }

//void SpinLockWake(volatile Atomic32 *w, bool all) {
//}

} // namespace internal
} // namespace base
