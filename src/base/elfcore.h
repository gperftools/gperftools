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

#ifndef _ELFCORE_H
#define _ELFCORE_H

/* We currently only support x86-32 and x86-64 on Linux. Porting to
 * other related platforms should not be difficult.
 */
#if (defined(__i386__) || defined(__x86_64__)) && defined(__linux)

#include <stdint.h>
#include <sys/types.h>
#include "config.h"

/* Define the DUMPER symbol to make sure that there is exactly one
 * core dumper built into the library.
 */
#define DUMPER "ELF"

/* By the time that we get a chance to read CPU registers in the
 * calling thread, they are already in a not particularly useful
 * state. Besides, there will be multiple frames on the stack that are
 * just making the core file confusing. To fix this problem, we take a
 * snapshot of the frame pointer, stack pointer, and instruction
 * pointer at an earlier time, and then insert these values into the
 * core file.
 */

typedef struct i386_regs {      /* Normal (non-FPU) CPU registers            */
#ifdef __x86_64__
  #define BP rbp
  #define SP rsp
  #define IP rip
  uint64_t  r15,r14,r13,r12,rbp,rbx,r11,r10;
  uint64_t  r9,r8,rax,rcx,rdx,rsi,rdi,orig_rax;
  uint64_t  rip,cs,eflags;
  uint64_t  rsp,ss;
  uint64_t  fs_base, gs_base;
  uint64_t  ds,es,fs,gs;
#else
  #define BP ebp
  #define SP esp
  #define IP eip
  uint32_t  ebx, ecx, edx, esi, edi, ebp, eax;
  uint16_t  ds, __ds, es, __es;
  uint16_t  fs, __fs, gs, __gs;
  uint32_t  orig_eax, eip;
  uint16_t  cs, __cs;
  uint32_t  eflags, esp;
  uint16_t  ss, __ss;
#endif
} i386_regs;

#if defined(__i386__) && defined(__GNUC__)
  /* On x86 we provide an optimized version of the FRAME() macro, if the
   * compiler supports a GCC-style asm() directive. This results in somewhat
   * more accurate values for CPU registers.
   */
  typedef struct Frame {
    struct i386_regs regs;
    int              errno_;
  } Frame;
  #define FRAME(f) Frame f;                                           \
                   do {                                               \
                     f.errno_ = errno;                                \
                     __asm__ volatile (                               \
                       "push %%eax\n"                                 \
                       "mov  %%ebp,%%eax\n"                           \
                       "push %%ebp\n"                                 \
                       "lea  %0,%%ebp\n"                              \
                       "mov  %%ebx,0(%%ebp)\n"                        \
                       "mov  %%ecx,4(%%ebp)\n"                        \
                       "mov  %%edx,8(%%ebp)\n"                        \
                       "mov  %%esi,12(%%ebp)\n"                       \
                       "mov  %%edi,16(%%ebp)\n"                       \
                       "mov  %%eax,20(%%ebp)\n"                       \
                       "mov  4(%%esp),%%eax\n"                        \
                       "mov  %%eax,24(%%ebp)\n"                       \
                       "mov  %%ds,%%eax\n"                            \
                       "mov  %%eax,28(%%ebp)\n"                       \
                       "mov  %%es,%%eax\n"                            \
                       "mov  %%eax,32(%%ebp)\n"                       \
                       "mov  %%fs,%%eax\n"                            \
                       "mov  %%eax,36(%%ebp)\n"                       \
                       "mov  %%gs,%%eax\n"                            \
                       "mov  %%eax, 40(%%ebp)\n"                      \
                       "lea  0f,%%eax\n"                              \
                       "mov  %%eax,48(%%ebp)\n"                       \
                       "mov  %%cs,%%eax\n"                            \
                       "mov  %%eax,52(%%ebp)\n"                       \
                       "pushf\n"                                      \
                       "pop  %%eax\n"                                 \
                       "mov  %%eax,56(%%ebp)\n"                       \
                       "mov  %%esp,%%eax\n"                           \
                       "add  $8,%%eax\n"                              \
                       "mov  %%eax,60(%%ebp)\n"                       \
                       "mov  %%ss,%%eax\n"                            \
                       "mov  %%eax,64(%%ebp)\n"                       \
                       "pop  %%ebp\n"                                 \
                       "pop  %%eax\n"                                 \
                     "0:"                                             \
                       : : "m" (f) : "memory");                       \
                     } while (0)
  #define SET_FRAME(f,r)                                              \
                     do {                                             \
                       errno = (f).errno_;                            \
                       (r)   = (f).regs;                              \
                     } while (0)
#else
  /* If we do not have a hand-optimized assembly version of the FRAME()
   * macro, we fall back to a generic version, which works across different
   * platforms. This code has been tested on x86_32 and x86_64 with both
   * gcc and icc.
   */
  #ifdef HAVE_BUILTIN_STACK_POINTER
    #define BUILTIN_STACK_POINTER() __builtin_stack_pointer()
  #else
    #define BUILTIN_STACK_POINTER() __builtin_frame_address(0)
  #endif
  typedef struct Frame {
    void *frame_address;
    void *stack_pointer;
    void *instruction_pointer;
    int  errno_;
  } Frame;
  #define FRAME(f) Frame f = { __builtin_frame_address(0),            \
                               BUILTIN_STACK_POINTER(),               \
                               &&label };                             \
                   /* Prevent the compiler from moving the label */   \
                   do {                                               \
                     f.errno_ = errno;                                \
                     label: if (!f.instruction_pointer) goto label;   \
                   } while (!f.stack_pointer)
  #define SET_FRAME(f,r)                                              \
                   do {                                               \
                     errno  = (f).errno_;                             \
                     (r).BP = (unsigned long)(f).frame_address;       \
                     (r).SP = (unsigned long)(f).stack_pointer;       \
                     (r).IP = (unsigned long)(f).instruction_pointer; \
                   } while (0)
#endif


/* Internal function for generating a core file. This API can change without
 * notice and is only supposed to be used internally by the core dumper.
 *
 * This function works for both single- and multi-threaded core
 * dumps. If called as
 *
 *   FRAME(frame);
 *   InternalGetCoreDump(&frame, 0, NULL);
 *
 * it creates a core file that only contains information about the
 * calling thread.
 *
 * Optionally, the caller can provide information about other threads
 * by passing their process ids in "thread_pids". The process id of
 * the caller should not be included in this array. All of the threads
 * must have been attached to with ptrace(), prior to calling this
 * function. They will be detached when "InternalGetCoreDump()" returns.
 *
 * This function either returns a file handle that can be read for obtaining
 * a core dump, or "-1" in case of an error. In the latter case, "errno"
 * will be set appropriately.
 *
 * While "InternalGetCoreDump()" is not technically async signal safe, you
 * might be tempted to invoke it from a signal handler. The code goes to
 * great lengths to make a best effort that this will actually work. But in
 * any case, you must make sure that you preserve the value of "errno"
 * yourself. It is guaranteed to be clobbered otherwise.
 *
 * Also, "InternalGetCoreDump" is not strictly speaking re-entrant. Again,
 * it makes a best effort to behave reasonably when called in a multi-
 * threaded environment, but it is ultimately the caller's responsibility
 * to provide locking.
 */
int InternalGetCoreDump(void *frame, int num_threads, pid_t *thread_pids);

#endif

#endif /* _ELFCORE_H */
