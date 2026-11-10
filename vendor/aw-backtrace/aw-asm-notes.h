/* SPDX-License-Identifier: 0BSD */
/*
 * The ELF notes every assembly object of this library must carry. Each .S
 * #includes this at its very end, for whatever architecture it is being
 * compiled for. That includes the "wrong-arch" case: every aw-asm-*.S is
 * compiled on every arch (so an embedder can just compile them all), and the
 * near-empty object that results still has to carry these notes.
 *
 * Why both matter:
 *
 *  - .note.GNU-stack: an object without it is taken (by binutils older than
 *    2.43, and with only a warning) to want an executable stack, and the
 *    whole output gets PT_GNU_STACK RWE.
 *
 *  - .note.gnu.property: the linker ANDs the feature bits (aarch64
 *    BTI/PAC/GCS, x86-64 IBT/SHSTK) over every input object, empty ones
 *    included. One object without the note silently drops the marking for
 *    the whole .so or executable. The compiler emits it for C/C++ sources;
 *    for .S nobody does, so it is done here.
 *
 * The feature bits are claimed unconditionally, whatever the build flags.
 * Since the linker ANDs them, claiming them can never mark an output the rest
 * of the build doesn't already mark.
 *
 * That makes it a requirement on every .S that includes this file: its code
 * must actually be compatible with all of these features, regardless of build
 * flags. Concretely:
 *  - every function starts with a landing pad, bti c / endbr64,
 *    unconditionally (both are NOPs where unused);
 *  - every function returns with a plain ret to where it was called from (no
 *    return-address tricks), so shadow stacks / GCS are happy;
 *  - x30 is never spilled, or is signed with PAC if it is.
 *
 * This assumes a linker that understands GNU property notes (GNU ld since
 * about 2.29 on x86-64 and 2.33 on aarch64, and lld). An older one copies the
 * section through unmerged, marking the output as protected when nothing else
 * is. Toolchains new enough for glibc 2.35 are well past that.
 *
 * Only meaningful to the assembler; it expands to nothing anywhere else (it
 * sits in :headers' glob, so tools may well parse it as C++).
 */
#ifndef AW_ASM_NOTES_H_
#define AW_ASM_NOTES_H_

#if defined(__ASSEMBLER__) && defined(__linux__) && defined(__ELF__)

	.pushsection .note.GNU-stack, "", %progbits
	.popsection

#if defined(__aarch64__)
/* GNU_PROPERTY_AARCH64_FEATURE_1_AND: BTI = 1, PAC = 2, GCS = 4 */
#define AW_GNU_PROPERTY_TYPE 0xc0000000
#define AW_GNU_PROPERTY_BITS 7
#elif defined(__x86_64__)
/* GNU_PROPERTY_X86_FEATURE_1_AND: IBT = 1, SHSTK = 2 */
#define AW_GNU_PROPERTY_TYPE 0xc0000002
#define AW_GNU_PROPERTY_BITS 3
#endif

#if defined(AW_GNU_PROPERTY_TYPE)
	.pushsection .note.gnu.property, "a"
	.p2align 3
	.long	4			/* n_namesz */
	.long	16			/* n_descsz */
	.long	5			/* n_type: NT_GNU_PROPERTY_TYPE_0 */
	.asciz	"GNU"
	.long	AW_GNU_PROPERTY_TYPE	/* pr_type */
	.long	4			/* pr_datasz */
	.long	AW_GNU_PROPERTY_BITS
	.long	0			/* pad to 8 */
	.popsection
#endif

#endif  /* __ASSEMBLER__ && __linux__ && __ELF__ */

#endif  /* AW_ASM_NOTES_H_ */
