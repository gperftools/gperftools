/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef BACKTRACE_COMPARER_H_
#define BACKTRACE_COMPARER_H_

#if defined(__cplusplus)
extern "C" {
#endif

void StartBacktraceComparer();

void StopBacktraceComparer();

#if defined(__cplusplus)
}
#endif

#endif  // BACKTRACE_COMPARER_H_
