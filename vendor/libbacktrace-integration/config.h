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
// This file a hand-crafted config.h that we feed to libbacktrace files
#ifndef LIBBACKTRACE_INTEGRATION_CONFIG_H_
#define LIBBACKTRACE_INTEGRATION_CONFIG_H_

// I.e. glibc needs this for dl_iterate_phdr
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>

#if UINTPTR_MAX == 0xffffffff
#define BACKTRACE_ELF_SIZE 32
#else
#if UINTPTR_MAX != 0xffffffffffffffffULL
#error bad elf word size!?
#endif
#define BACKTRACE_ELF_SIZE 64
#endif

#if defined __ELF__
#define HAVE_DL_ITERATE_PHDR 1
#define HAVE_LINK_H 1

// FreeBSD. We also assume it has modern enough GCC-compatible compiler
#if defined __has_include
#if __has_include(<sys/sysctl.h>)
#if defined KERN_PROC
#define HAVE_KERN_PROC 1
#endif
#if defined KERN_PROC_ARGS
#define HAVE_KERN_PROC_ARGS 1
#endif
#endif
#endif

#elif defined __MACH__
#define HAVE_MACH_O_DYLD_H
#else
#error This code is only prepared to deal with ELF systems or OSX (mach-o)
#endif

#define HAVE_FCNTL 1
#define HAVE_LSTAT 1
#define HAVE_MEMORY_H 1
#define HAVE_READLINK 1

#define backtrace_syminfo_to_full_callback tcmalloc_backtrace_syminfo_to_full_callback
#define backtrace_pcinfo tcmalloc_backtrace_pcinfo
#define backtrace_alloc tcmalloc_backtrace_alloc
#define backtrace_vector_release tcmalloc_backtrace_vector_release
#define backtrace_dwarf_add tcmalloc_backtrace_dwarf_add
#define backtrace_syminfo_to_full_error_callback tcmalloc_backtrace_syminfo_to_full_error_callback
#define backtrace_free tcmalloc_backtrace_free
#define backtrace_open tcmalloc_backtrace_open
#define backtrace_close tcmalloc_backtrace_close
#define backtrace_vector_grow tcmalloc_backtrace_vector_grow
#define backtrace_uncompress_zdebug tcmalloc_backtrace_uncompress_zdebug
#define backtrace_create_state tcmalloc_backtrace_create_state
#define backtrace_get_view tcmalloc_backtrace_get_view
#define backtrace_initialize tcmalloc_backtrace_initialize
#define backtrace_uncompress_zstd tcmalloc_backtrace_uncompress_zstd
#define backtrace_qsort tcmalloc_backtrace_qsort
#define backtrace_uncompress_lzma tcmalloc_backtrace_uncompress_lzma
#define backtrace_release_view tcmalloc_backtrace_release_view
#define backtrace_vector_finish tcmalloc_backtrace_vector_finish
#define backtrace_syminfo tcmalloc_backtrace_syminfo


#endif  // LIBBACKTRACE_INTEGRATION_CONFIG_H_
