/* Copyright (c) 2007, Google Inc.
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
 * Author: Craig Silverstein
 *
 * A simple mutex wrapper.  Right now, it's implemented in terms of
 * pthreads, but is meant to be easy to extend to other threads impls.
 */

#include "config.h"
#include "mutex.h"

#if defined(NO_THREADS)

Mutex::Mutex() {}
Mutex::~Mutex() {}
void Mutex::Lock() {}
void Mutex::Unlock() {}
void Mutex::ReaderLock() {}
void Mutex::ReaderUnlock() {}

#elif defined(HAVE_PTHREAD) && defined(HAVE_RWLOCK)

#include <stdlib.h>      // for abort()
#include <pthread.h>
#define SAFE_PTHREAD(fncall)  do { if ((fncall) != 0) abort(); } while (0)

Mutex::Mutex()             { SAFE_PTHREAD(pthread_rwlock_init(&mutex_, NULL)); }
Mutex::~Mutex()            { SAFE_PTHREAD(pthread_rwlock_destroy(&mutex_)); }
void Mutex::Lock()         { SAFE_PTHREAD(pthread_rwlock_wrlock(&mutex_)); }
void Mutex::Unlock()       { SAFE_PTHREAD(pthread_rwlock_unlock(&mutex_)); }
void Mutex::ReaderLock()   { SAFE_PTHREAD(pthread_rwlock_rdlock(&mutex_)); }
void Mutex::ReaderUnlock() { SAFE_PTHREAD(pthread_rwlock_unlock(&mutex_)); }

#elif defined(HAVE_PTHREAD)

#include <stdlib.h>      // for abort()
#include <pthread.h>
#define SAFE_PTHREAD(fncall)  do { if ((fncall) != 0) abort(); } while (0)

Mutex::Mutex()             { SAFE_PTHREAD(pthread_mutex_init(&mutex_, NULL)); }
Mutex::~Mutex()            { SAFE_PTHREAD(pthread_mutex_destroy(&mutex_)); }
void Mutex::Lock()         { SAFE_PTHREAD(pthread_mutex_lock(&mutex_)); }
void Mutex::Unlock()       { SAFE_PTHREAD(pthread_mutex_unlock(&mutex_)); }
void Mutex::ReaderLock()   { Lock(); }      // we don't have read-write locks
void Mutex::ReaderUnlock() { Unlock(); }

#else

#error Need to implement mutex.h/cc for your architecture, or #define NO_THREADS

#endif
