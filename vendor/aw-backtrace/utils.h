// SPDX-License-Identifier: 0BSD
#ifndef UTILS_H_
#define UTILS_H_

#define PREDICT_TRUE(c) __builtin_expect((c), 1)
#define PREDICT_FALSE(c) __builtin_expect((c), 0)

#define ALWAYS_INLINE inline __attribute__((always_inline))
#define NEVER_INLINE __attribute__((noinline))

// FOOTGUN!!! Only use when you know what you're doing!
// #ifdef NDEBUG
// #define AW_ASSUME(x) [[assume((x))]]
// #else
#define AW_ASSUME(x) assert((x))
// #endif

#endif  // UTILS_H_
