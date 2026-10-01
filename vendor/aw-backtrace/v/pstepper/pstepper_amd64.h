// SPDX-License-Identifier: 0BSD
/*
 * pstepper: amd64-specific slice of the guest-facing ABI.
 *
 * Picked by the arch #ifdef block in pstepper.h; also included directly by
 * pstepper_frame_amd64.h, which needs only pstepper_gpregs_t. Self-contained
 * (pulls its own <stddef.h>/<stdint.h>/<ucontext.h>). Everything here is
 * x86-64 detail that a future arm64 port would replace with its own
 * pstepper_arm64.h: the portable parts of the ABI (the syscall interface, the
 * handler signature, the entry points) stay in pstepper.h.
 *
 * The one type this defines, pstepper_gpregs_t, is what pstepper.h's
 * pstepper_handler_fn / pstepper_invoke_handler are expressed in terms of, so
 * the include has to precede those declarations.
 */
#ifndef PSTEPPER_AMD64_H_
#define PSTEPPER_AMD64_H_

#include <stddef.h>
#include <stdint.h>
#include <ucontext.h>

/*
 * General-purpose register snapshot handed to the pstepper handler. This is
 * pstepper's own minimal, portable context -- only the integer registers,
 * since that is all a backtracer ever needs. FP + full vector state (via
 * xsave/xrstor) and eflags are transparently preserved by the trampoline
 * around the handler call (so a real program's floating-point/SIMD state is
 * never disturbed), but never exposed here. There is no promise that edits
 * made to this struct are reflected back on resume.
 *
 * The field order deliberately mirrors x86-64 glibc's gregset_t -- the
 * REG_* indices in <sys/ucontext.h> (REG_R8=0 .. REG_R15, REG_RDI, REG_RSI,
 * REG_RBP, REG_RBX, REG_RDX, REG_RAX, REG_RCX, REG_RSP, REG_RIP), and the
 * _Static_assert block below build-checks that against the real header. The
 * struct still isn't a ucontext_t (no signal mask, no FP pointer, no
 * segment/error words); the point of the correspondence is only that
 * synthesizing a "fake"-ish ucontext_t from this snapshot is a straight
 * index mapping rather than a field-by-field shuffle.
 */
typedef struct pstepper_gpregs {
	uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
	uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx;
	uint64_t rsp;
	uint64_t rip;
} pstepper_gpregs_t;

#ifndef __cplusplus
/*
 * gregset_t is greg_t[__NGREG]; each pstepper_gpregs_t field must sit at the
 * byte offset its REG_* index would land at in that array, so a fake
 * ucontext_t is `memcpy(uc->uc_mcontext.gregs, regs, sizeof *regs)` on the
 * covered prefix. greg_t is `long long`, i.e. 8 bytes == our uint64_t.
 */
_Static_assert(sizeof(uint64_t) == sizeof(greg_t), "greg_t is not 64-bit");
#define PSTEPPER_ASSERT_GREG(field, reg)                                                                               \
	_Static_assert(offsetof(pstepper_gpregs_t, field) == (reg) * sizeof(greg_t),                                   \
		       "pstepper_gpregs_t." #field " does not match gregset_t[" #reg "]")
PSTEPPER_ASSERT_GREG(r8, REG_R8);
PSTEPPER_ASSERT_GREG(r9, REG_R9);
PSTEPPER_ASSERT_GREG(r10, REG_R10);
PSTEPPER_ASSERT_GREG(r11, REG_R11);
PSTEPPER_ASSERT_GREG(r12, REG_R12);
PSTEPPER_ASSERT_GREG(r13, REG_R13);
PSTEPPER_ASSERT_GREG(r14, REG_R14);
PSTEPPER_ASSERT_GREG(r15, REG_R15);
PSTEPPER_ASSERT_GREG(rdi, REG_RDI);
PSTEPPER_ASSERT_GREG(rsi, REG_RSI);
PSTEPPER_ASSERT_GREG(rbp, REG_RBP);
PSTEPPER_ASSERT_GREG(rbx, REG_RBX);
PSTEPPER_ASSERT_GREG(rdx, REG_RDX);
PSTEPPER_ASSERT_GREG(rax, REG_RAX);
PSTEPPER_ASSERT_GREG(rcx, REG_RCX);
PSTEPPER_ASSERT_GREG(rsp, REG_RSP);
PSTEPPER_ASSERT_GREG(rip, REG_RIP);
#undef PSTEPPER_ASSERT_GREG
/* rip is the last field, and every field from REG_R8=0 up to it is covered
 * with no gaps -- so the struct is exactly a prefix of gregset_t. */
_Static_assert(sizeof(pstepper_gpregs_t) == (REG_RIP + 1) * sizeof(greg_t),
	       "pstepper_gpregs_t is not a gap-free prefix of gregset_t");
#endif // !__cplusplus

#endif /* PSTEPPER_AMD64_H_ */
