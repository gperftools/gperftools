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
 * Author: Joi Sigurdsson
 *
 * Implementation of PreamblePatcher
 */

#include "preamble_patcher.h"

#include "mini_disassembler.h"

// compatibility shims
#include "base/logging.h"

// Definitions of assembly statements we need
#define ASM_JMP32REL 0xE9
#define ASM_INT3 0xCC

namespace sidestep {

SideStepError PreamblePatcher::RawPatchWithStubAndProtections(
    void* target_function, void *replacement_function,
    unsigned char* preamble_stub, unsigned long stub_size,
    unsigned long* bytes_needed) {
  // We need to be able to write to a process-local copy of the first
  // MAX_PREAMBLE_STUB_SIZE bytes of target_function
  DWORD old_target_function_protect = 0;
  BOOL succeeded = ::VirtualProtect(reinterpret_cast<void*>(target_function), 
                                    MAX_PREAMBLE_STUB_SIZE, PAGE_READWRITE, 
                                    &old_target_function_protect);
  if (!succeeded) {
    ASSERT(false, "Failed to make page containing target function "
                   "copy-on-write.");
    return SIDESTEP_ACCESS_DENIED;
  }

  SideStepError error_code = RawPatchWithStub(target_function, 
                                              replacement_function, 
                                              preamble_stub, 
                                              stub_size,
                                              bytes_needed);
  if (SIDESTEP_SUCCESS != error_code) {
    ASSERT1(false);
    return error_code;
  }

  // Restore the protection of the first MAX_PREAMBLE_STUB_SIZE bytes of
  // pTargetFunction to what they were before we started goofing around.
  succeeded = ::VirtualProtect(reinterpret_cast<void*>(target_function), 
                               MAX_PREAMBLE_STUB_SIZE, 
                               old_target_function_protect, 
                               &old_target_function_protect);
  if (!succeeded) {
    ASSERT(false, "Failed to restore protection to target function.");
    // We must not return an error here because the function has actually
    // been patched, and returning an error would likely cause our client
    // code not to unpatch it.  So we just keep going.
  }

  // Flush the instruction cache to make sure the processor doesn't execute the
  // old version of the instructions (before our patch).
  //
  // FlushInstructionCache is actually a no-op at least on single-processor
  // XP machines.  I'm not sure why this is so, but it is, yet I want to keep the
  // call to the API here for correctness in case there is a difference in
  // some variants of Windows/hardware.
  succeeded = ::FlushInstructionCache(::GetCurrentProcess(), 
                                      target_function, 
                                      MAX_PREAMBLE_STUB_SIZE);
  if (!succeeded) {
    ASSERT(false, "Failed to flush instruction cache.");
    // We must not return an error here because the function has actually
    // been patched, and returning an error would likely cause our client
    // code not to unpatch it.  So we just keep going.
  }

  return SIDESTEP_SUCCESS;
}

SideStepError PreamblePatcher::RawPatch(void* target_function,
                                        void* replacement_function,
                                        void** original_function_stub) {
  if (!target_function || !replacement_function || !original_function_stub ||
      (*original_function_stub) || target_function == replacement_function) {
    ASSERT(false, "Preconditions not met");
    return SIDESTEP_INVALID_PARAMETER;
  }

  // @see MAX_PREAMBLE_STUB_SIZE for an explanation of how we arrives at 
  // this size
  byte* preamble_stub = new unsigned char[MAX_PREAMBLE_STUB_SIZE];
  if (!preamble_stub) {
    ASSERT(false, "Unable to allocate preamble-stub.");
    return SIDESTEP_INSUFFICIENT_BUFFER;
  }

  // Change the protection of the newly allocated preamble stub to
  // PAGE_EXECUTE_READWRITE. This is required to work with DEP (Data
  // Execution Prevention) which will cause an exception if code is executed
  // from a page on which you do not have read access.
  DWORD old_stub_protect = 0;
  BOOL succeeded = VirtualProtect(preamble_stub, MAX_PREAMBLE_STUB_SIZE,
                             PAGE_EXECUTE_READWRITE, &old_stub_protect);
  if (!succeeded) {
    ASSERT(false, "Failed to make page preamble stub read-write-execute.");
    delete[] preamble_stub;
    return SIDESTEP_ACCESS_DENIED;
  }

  SideStepError error_code = RawPatchWithStubAndProtections(target_function, 
                                              replacement_function, 
                                              preamble_stub, 
                                              MAX_PREAMBLE_STUB_SIZE,
                                              NULL);
  if (SIDESTEP_SUCCESS != error_code) {
    ASSERT1(false);
    delete[] preamble_stub;
    return error_code;
  }

  RAW_VLOG(1, "PreamblePatcher::RawPatch successfully patched 0x%x",
           target_function);
                
  *original_function_stub = reinterpret_cast<void*>(preamble_stub);
  return SIDESTEP_SUCCESS;
}

SideStepError PreamblePatcher::Unpatch(void* target_function,
                                       void* replacement_function,
                                       void* original_function_stub) {
  ASSERT1(target_function && original_function_stub);
  if (!target_function || !original_function_stub) {
    return SIDESTEP_INVALID_PARAMETER;
  }

  // We disassemble the preamble of the _stub_ to see how many bytes we
  // originally copied to the stub.
  MiniDisassembler disassembler;
  unsigned int preamble_bytes = 0;
  while (preamble_bytes < 5) {
    InstructionType instruction_type = 
      disassembler.Disassemble(
        reinterpret_cast<byte*>(original_function_stub) + preamble_bytes,
        preamble_bytes);
    if (IT_GENERIC != instruction_type) {
      ASSERT(false, "Should only have generic instructions in stub!!");
      return SIDESTEP_UNSUPPORTED_INSTRUCTION;
    }
  }

  // Before unpatching, target_function should be a JMP to
  // replacement_function.  If it's not, then either it's an error, or
  // we're falling into the case where the original instruction was a
  // JMP, and we patched the jumped_to address rather than the JMP
  // itself.  (For instance, if malloc() is just a JMP to __malloc(),
  // we patched __malloc() and not malloc().)
  unsigned char* target = reinterpret_cast<unsigned char*>(target_function);
  while (1) {    // we stop when target is a JMP to replacement_function
    if (target[0] != ASM_JMP32REL) {
      ASSERT(false, "target_function does not look like it was patched.");
      return SIDESTEP_INVALID_PARAMETER;
    }
    int relative_offset;   // Windows guarantees int is 4 bytes
    ASSERT1(sizeof(relative_offset) == 4);
    memcpy(reinterpret_cast<void*>(&relative_offset),
           reinterpret_cast<void*>(target + 1), 4);
    unsigned char* jump_to = target + 5 + relative_offset;
    if (jump_to == replacement_function)
      break;
    target = jump_to;      // follow the jmp
  }

  // We need to be able to write to a process-local copy of the first
  // MAX_PREAMBLE_STUB_SIZE bytes of target_function
  DWORD old_target_function_protect = 0;
  BOOL succeeded = ::VirtualProtect(reinterpret_cast<void*>(target), 
                                    MAX_PREAMBLE_STUB_SIZE, PAGE_READWRITE, 
                                    &old_target_function_protect);
  if (!succeeded) {
    ASSERT(false, "Failed to make page containing target function "
                   "copy-on-write.");
    return SIDESTEP_ACCESS_DENIED;
  }

  // Replace the first few bytes of the original function with the bytes we
  // previously moved to the preamble stub.
  memcpy(reinterpret_cast<void*>(target),
         original_function_stub, preamble_bytes);

  // Stub is now useless so delete it.
  // [csilvers: Commented out for perftools because it causes big problems
  //  when we're unpatching malloc.  We just let this live on as a leak.]
  //delete original_function_stub;

  // Restore the protection of the first MAX_PREAMBLE_STUB_SIZE bytes of
  // target to what they were before we started goofing around.
  succeeded = ::VirtualProtect(reinterpret_cast<void*>(target), 
                               MAX_PREAMBLE_STUB_SIZE, 
                               old_target_function_protect, 
                               &old_target_function_protect);

  // Flush the instruction cache to make sure the processor doesn't execute the
  // old version of the instructions (before our patch).
  //
  // See comment on FlushInstructionCache elsewhere in this file.
  succeeded = ::FlushInstructionCache(::GetCurrentProcess(), 
                                      target, 
                                      MAX_PREAMBLE_STUB_SIZE);
  if (!succeeded) {
    ASSERT(false, "Failed to flush instruction cache.");
    return SIDESTEP_UNEXPECTED;
  }

  RAW_VLOG(1, "PreamblePatcher::Unpatch successfully unpatched 0x%x",
           target_function);
  return SIDESTEP_SUCCESS;
}

};  // namespace sidestep
