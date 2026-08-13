// SPDX-License-Identifier: 0BSD
#ifndef SIMPLE_FP_BACKTRACE_H_
#define SIMPLE_FP_BACKTRACE_H_

#if defined(__cplusplus)
extern "C" {
#endif

int simple_backtrace(const void* _uc, void** result, int max_frames);

#if defined(__cplusplus)
}  // extern "C"
#endif

#endif  // SIMPLE_FP_BACKTRACE_H_
