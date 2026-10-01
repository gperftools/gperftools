// SPDX-License-Identifier: 0BSD
/*
 * pstepper: cross-TU internals shared between the portable plumbing
 * (pstepper.c) and the arch-specific plumbing (pstepper_amd64.c). Not part of
 * the guest-facing ABI -- pstepper.h stays limited to what a guest program
 * actually calls.
 *
 * These were file-static in pstepper.c before the arch split; the clone/exit
 * intercepts that moved to pstepper_amd64.c still need them, so they are
 * declared here and defined in pstepper.c.
 */
#ifndef PSTEPPER_INTERNAL_H_
#define PSTEPPER_INTERNAL_H_

#include <stdint.h>

#include "pstepper.h"

/* The syscall/command wire protocol -- shared verbatim with the plugin,
 * which includes only that header (it needs none of the guest-side,
 * target-gated machinery below). */
#include "pstepper_abi.h"

/*
 * Arch-specific slice of the ABI: pstepper_gpregs_t, the register snapshot
 * handed to the handler. Picked here so the rest of this header (the handler
 * signature, pstepper_invoke_handler) can be written against the type
 * regardless of target. A new port adds its own pstepper_<arch>.h and another
 * arm below.
 */
#if defined(__x86_64__)
#include "pstepper_amd64.h"
#elif defined(__aarch64__)
#include "pstepper_arm64.h"
#else
#error "pstepper: no register-snapshot definition for this architecture yet"
#endif

// Makes PSTEPPER_SYSCALL_NR syscall
int pstepper_syscall(uint64_t cmd, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5);

/* Defined in pstepper_trampoline_amd64.S. Portable-named on purpose -- these
 * are the symbol contract any arch's trampoline satisfies, so the portable
 * arming code in pstepper.c can reference them directly. */
extern void pstepper_trampoline(void);
extern void pstepper_resume_marker(void);

/*
 * Defined in pstepper_amd64.c; called only from pstepper_trampoline_amd64.S
 * once it has built the gpregs snapshot. Always either returns normally (an
 * ordinary step -- the trampoline falls through to its usual
 * restore-and-resume) or diverts internally and never returns at all (e.g.
 * when the trap turns out to be a CLONE_THREAD syscall about to happen,
 * handled entirely on this side before the trampoline ever gets control
 * back).
 */
void pstepper_invoke_handler(pstepper_gpregs_t *regs);

/* The handler registered by pstepper_enable(); consulted by
 * pstepper_invoke_handler() (in the arch TU). NULL until armed. */
extern pstepper_handler_fn pstepper_user_handler;

/* fprintf(stderr, "pstepper: %s\n", msg) then abort(). */
__attribute__((noreturn)) void pstepper_die(const char *msg);

/*
 * The size every thread's upcall stack gets mmapped at -- set once by
 * whichever thread first calls pstepper_enable() (normally the main thread),
 * and reused for every thread the clone hook bootstraps later.
 */
extern uint64_t pstepper_upcall_stack_size;

/*
 * This thread's own upcall stack, remembered so pstepper_disable() (and the
 * exit intercept) can free it. Per-thread: each thread mmaps and owns its
 * own.
 *
 * initial-exec, not the default global-dynamic: pstepper_child_bootstrap()
 * writes this on a clone-bootstrapped thread *before* glibc's start_thread
 * runs, and a global-dynamic access there compiles to __tls_get_addr, which
 * can call malloc on a half-initialized thread -- on the upcall stack.
 * initial-exec resolves to a fixed offset from the thread pointer with no
 * call. The declaration must carry the same tls_model as the definition.
 */
extern __thread void *pstepper_own_upcall_stack __attribute__((tls_model("initial-exec")));

/* mmap a fresh PROT_READ|PROT_WRITE MAP_STACK region of `size` bytes for use
 * as an upcall stack; aborts on failure. */
void *pstepper_alloc_upcall_stack(uint64_t size);

/* The high end of an upcall stack that starts at `base`, rounded down to a
 * 64-byte boundary -- the only stack geometry the plugin is told
 * (PSTEPPER_CMD_ENABLE arg2). The 64-byte alignment is what the amd64
 * trampoline's xsave area derives from (see pstepper_frame_amd64.h). */
uint64_t pstepper_upcall_top(uint64_t base);

/* Arch half of pstepper_enable(), run before stepping is armed: set up
 * whatever per-target state the trampoline needs (on amd64, the xsave mask
 * and its host-capability checks). Defined in pstepper_<arch>.c. */
void pstepper_arch_prepare(void);

#endif /* PSTEPPER_INTERNAL_H_ */
