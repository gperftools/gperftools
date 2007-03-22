/* Copyright (c) 2005, Google Inc.
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
 * Author: Markus Gutschke
 */

#ifndef _THREAD_LISTER_H
#define _THREAD_LISTER_H

#include <stdarg.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*ListAllProcessThreadsCallBack)(void *parameter,
                                             int num_threads,
                                             pid_t *thread_pids,
                                             va_list ap);

/* This function gets the list of all linux threads of the current process
 * but this one and passes them to the 'callback' along with the 'parameter'
 * pointer; at the call back call time all the threads are paused via
 * PTRACE_ATTACH.
 * 'callback' is supposed to do or arrange for ResumeAllProcessThreads.
 * We return -1 on error and the return value of 'callback' on success.
 */
int ListAllProcessThreads(void *parameter,
                          ListAllProcessThreadsCallBack callback, ...);

/* This function resumes the list of all linux threads that
 * ListAllProcessThreads pauses before giving to its callback.
 */
void ResumeAllProcessThreads(int num_threads, pid_t *thread_pids);

#ifdef __cplusplus
};
#endif

#endif  /* _THREAD_LISTER_H */
