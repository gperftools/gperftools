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
 * --
 * Author: Craig Silverstein
 *
 * C shims for the C++ malloc_extension.h.  See malloc_extension.h for
 * details.  Note these C shims always work on
 * MallocExtension::instance(); it is not possible to have more than
 * one MallocExtension object in C applications.
 */

#ifndef _MALLOC_EXTENSION_C_H_
#define _MALLOC_EXTENSION_C_H_

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

bool MallocExtension_VerifyAllMemory();
bool MallocExtension_VerifyAllMemory();
bool MallocExtension_VerifyNewMemory(void* p);
bool MallocExtension_VerifyArrayNewMemory(void* p);
bool MallocExtension_VerifyMallocMemory(void* p);
bool MallocExtension_MallocMemoryStats(int* blocks, size_t* total,
                                       int histogram[kMallocHistogramSize]);

void MallocExtension_GetStats(char* buffer, int buffer_length);

/* NOTE: commented out because they require a c++ string.
 * TODO(csilvers): write a C version of these routines, that perhaps
 * take a fixed-size buffer.
 */
/* void MallocExtension_GetHeapSample(string* result); */
/* void MallocExtension_GetHeapGrowthStacks(string* result); */

bool MallocExtension_GetNumericProperty(const char* property, size_t* value);
bool MallocExtension_SetNumericProperty(const char* property, size_t value);
void MallocExtension_MarkThreadIdle();
void MallocExtension_ReleaseFreeMemory();

#ifdef __cplusplus
}   // extern "C"
#endif

#endif /* _MALLOC_EXTENSION_C_H_ */
