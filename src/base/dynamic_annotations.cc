/* Copyright (c) 2008, Google Inc.
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
 * Author: Kostya Serebryany
 */

#include "base/dynamic_annotations.h"

// Each function is empty and called (via a macro) only in debug mode.
// The arguments are captured by dynamic tools at runtime.

extern "C" void AnnotateRWLockCreate(const char *file, int line, void *lock){}
extern "C" void AnnotateRWLockDestroy(const char *file, int line, void *lock){}
extern "C" void AnnotateRWLockAcquired(const char *file, int line, 
                                       void *lock, long is_w){}
extern "C" void AnnotateRWLockReleased(const char *file, int line, 
                                       void *lock, long is_w){}
extern "C" void AnnotateCondVarWait(const char *file, int line, void *cv, 
                                    void *lock){}
extern "C" void AnnotateCondVarSignal(const char *file, int line, void *cv){}
extern "C" void AnnotateCondVarSignalAll(const char *file, int line, void *cv){}
extern "C" void AnnotatePCQCreate(const char *file, int line, void *pcq){}
extern "C" void AnnotatePCQDestroy(const char *file, int line, void *pcq){}
extern "C" void AnnotatePCQPut(const char *file, int line, void *pcq){}
extern "C" void AnnotatePCQGet(const char *file, int line, void *pcq){}
extern "C" void AnnotateNewMemory(const char *file, int line, void *mem, 
                                  long size){}
extern "C" void AnnotateExpectRace(const char *file, int line, void *mem, 
                                   const char *description){}
extern "C" void AnnotateBenignRace(const char *file, int line, void *mem, 
                                   const char *description){}
extern "C" void AnnotateMutexIsUsedAsCondVar(const char *file, int line, 
                                            void *mu){}
extern "C" void AnnotateTraceMemory(const char *file, int line, 
                                    const void *arg){}
extern "C" void AnnotateNoOp(const char *file, int line, const void *arg){}
