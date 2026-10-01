// SPDX-License-Identifier: 0BSD
/*
 * pstepper: shared ABI between guest programs and the pstepper QEMU plugin.
 *
 * This header defines the "magic syscall" interface that stands in for the
 * imagined kernel facility: a per-thread single-step trap that delivers a
 * UXI (User eXecution Interrupt) to a registered handler at every
 * instruction boundary. UXI is pstepper's own term, deliberately distinct
 * from a POSIX "signal" -- it's synchronous and tied to instruction
 * boundaries rather than async and tied to arbitrary points, and the two
 * should never be conflated when reading this code or talking about it.
 * Delivering a UXI makes an upcall into the registered handler; that upcall
 * runs on a dedicated "upcall stack" (registered at PSTEPPER_CMD_ENABLE
 * time) -- pstepper's analogue of a POSIX sigaltstack, again named
 * distinctly on purpose. Guest code (and only guest code -- the consumer,
 * e.g. a future backtrace-comparer port, is meant to be written against
 * this header and have zero awareness that a QEMU plugin implements it)
 * issues these via a raw `syscall` instruction with rax =
 * PSTEPPER_SYSCALL_NR.
 *
 * Design (v2): the plugin itself does almost nothing. On a trap it writes
 * two words -- the trapped instruction's original RIP and RSP -- onto the
 * guest's registered upcall stack, points RSP there, and jumps to a
 * guest-side trampoline (pstepper_trampoline, in pstepper_trampoline_amd64.S).
 * From that point on it's ordinary guest code, running at full speed and
 * using real hardware instructions (push/mov/xsave/pushfq) to save and
 * restore its own state -- the plugin never marshals a register file or
 * touches eflags itself. Resuming is a custom, otherwise-invalid
 * instruction (ud2) that the plugin recognizes by *address* (it only ever
 * legitimately appears at one place: inside our own trampoline) and
 * interprets as "pop two words for RIP/RSP off the current stack and
 * jump" -- our own minimal analogue of Intel UINTR's `uiret`.
 *
 * The plugin tracks one flag per vCPU, "trap parked", spanning a genuine
 * trap through to the trapped instruction retiring after resume. While it is
 * set -- trampoline, handler, and any guest signal handler QEMU interposes
 * in that window -- nothing is re-intercepted. No RSP-range bookkeeping is
 * involved; the plugin is told only the aligned top of the upcall stack.
 *
 * This header is the guest-facing ABI only: types, constants, and the
 * entry points a guest program actually calls. Its arch-specific slice (the
 * register-snapshot type) is split out into a per-arch header picked by the
 * #ifdef block below -- pstepper_amd64.h today. The trampoline itself lives
 * in pstepper_trampoline_amd64.S (hand-written asm; see pstepper_frame_amd64.h
 * for the stack frame layout it and pstepper_amd64.c both agree on); the
 * arch-specific plumbing (xsave policy, syscall-opcode recognition, the
 * clone/exit intercepts) lives in pstepper_amd64.c, and the portable
 * higher-level plumbing (arming/disarming, upcall-stack management) in
 * pstepper.c.
 */
#ifndef PSTEPPER_H_
#define PSTEPPER_H_

#include <stddef.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#if defined(__cplusplus)
extern "C" {
#endif


/*
 * The registered upcall handler. Deliberately shaped like a POSIX
 * sa_sigaction handler so the consumer above (part 3 -- a future
 * backtrace-comparer port) can be written as if it were an ordinary signal
 * handler and stay unaware that a QEMU plugin, rather than a kernel,
 * delivers the context.
 *
 *   uxi_kind  -- PSTEPPER_UXI_* (not a signal number).
 *   info      -- always NULL for now (analogue of siginfo_t*).
 *   uc        -- a "fake"-ish ucontext_t. Only the integer register file is
 *                real: uc->uc_mcontext.gregs[REG_R8..REG_RIP] hold the
 *                trapped instruction's registers (RIP is the address of the
 *                instruction *about to* execute). Everything else is fake --
 *                gregs[REG_EFL] and the other scalar slots are zero,
 *                uc_mcontext.fpregs is NULL (no FP/SIMD state is exposed,
 *                though it is preserved transparently around the upcall),
 *                uc_stack / uc_sigmask / uc_flags / uc_link are zeroed. The
 *                conversion is one-way: edits the handler makes to uc are
 *                NOT reflected back when the trapped instruction resumes.
 */
typedef void (*pstepper_handler_fn)(int uxi_kind, void *info, ucontext_t *uc);

/*
 * Defined in pstepper.c. pstepper_enable() mmaps its own upcall stack
 * of upcall_stack_size bytes (remembered process-wide for the
 * thread-creation hook to reuse when bootstrapping later threads) and
 * frees it again in pstepper_disable(). Returns errno. So 0 on
 * success, and e.g. ENOSYS on failure.
 */
int pstepper_enable(pstepper_handler_fn handler, uint64_t upcall_stack_size, int enable_acceleration);
void pstepper_enable_or_die(pstepper_handler_fn handler, uint64_t upcall_stack_size, int enable_acceleration);
void pstepper_disable(void);

#if defined(__cplusplus)
}  // extern "C"
#endif

#endif /* PSTEPPER_H_ */
