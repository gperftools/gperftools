/* -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*- */
// SPDX-License-Identifier: 0BSD
#ifndef DWARF_CONSTANTS_H_
#define DWARF_CONSTANTS_H_

// DWARF encoding constants
// Note: These are LSB encoding constants (Linux Standard Base), effectively
// extending DWARF.
enum {
  DW_EH_PE_absptr = 0x00,
  DW_EH_PE_omitted = 0xff,
  DW_EH_PE_uleb128 = 0x01,
  DW_EH_PE_udata2 = 0x02,
  DW_EH_PE_udata4 = 0x03,
  DW_EH_PE_udata8 = 0x04,
  DW_EH_PE_sleb128 = 0x09,
  DW_EH_PE_sdata2 = 0x0A,
  DW_EH_PE_sdata4 = 0x0B,
  DW_EH_PE_sdata8 = 0x0C,
  DW_EH_PE_pcrel = 0x10,
  DW_EH_PE_datarel = 0x30,
};

// DWARF CFA instructions
enum {
  DW_CFA_advance_loc = 0x40,
  DW_CFA_offset = 0x80,
  DW_CFA_restore = 0xC0,
  DW_CFA_nop = 0,
  DW_CFA_set_loc = 0x01,
  DW_CFA_advance_loc1 = 0x02,
  DW_CFA_advance_loc2 = 0x03,
  DW_CFA_advance_loc4 = 0x04,
  DW_CFA_offset_extended = 0x05,
  DW_CFA_restore_extended = 0x06,
  DW_CFA_undefined = 0x07,
  DW_CFA_same_value = 0x08,
  DW_CFA_register = 0x09,
  DW_CFA_remember_state = 0x0a,
  DW_CFA_restore_state = 0x0b,
  DW_CFA_def_cfa = 0x0c,
  DW_CFA_def_cfa_register = 0x0d,
  DW_CFA_def_cfa_offset = 0x0e,
  DW_CFA_def_cfa_expression = 0x0f,
  DW_CFA_expression = 0x10,
  DW_CFA_offset_extended_sf = 0x11,
  DW_CFA_def_cfa_sf = 0x12,
  DW_CFA_def_cfa_offset_sf = 0x13,
  DW_CFA_val_offset = 0x14,
  DW_CFA_val_offset_sf = 0x15,
  DW_CFA_val_expression = 0x16,
  DW_CFA_AARCH64_negate_ra_state = 0x2d,
  DW_CFA_GNU_window_save = 0x2d,
  DW_CFA_GNU_args_size = 0x2e,
};

// DWARF register numbers (x86_64)
// These mappings are specified by the x86-64 ABI.
#if defined(__x86_64__)
enum {
  DWARF_RAX = 0,
  DWARF_RDX = 1,
  DWARF_RCX = 2,
  DWARF_RBX = 3,
  DWARF_RSI = 4,
  DWARF_RDI = 5,
  DWARF_RBP = 6,
  DWARF_RSP = 7,
  DWARF_R8 = 8,
  DWARF_R9 = 9,
  DWARF_R10 = 10,
  DWARF_R11 = 11,
  DWARF_R12 = 12,
  DWARF_R13 = 13,
  DWARF_R14 = 14,
  DWARF_R15 = 15,
  DWARF_RIP = 16,
};
#elif defined(__aarch64__)
// DWARF register numbers (AArch64)
// These mappings are specified by the AArch64 ABI.
enum {
  DWARF_X0 = 0,
  DWARF_X1 = 1,
  DWARF_X2 = 2,
  DWARF_X3 = 3,
  DWARF_X4 = 4,
  DWARF_X5 = 5,
  DWARF_X6 = 6,
  DWARF_X7 = 7,
  DWARF_X8 = 8,
  DWARF_X29 = 29,
  DWARF_FP = 29,
  DWARF_X30 = 30,
  DWARF_LR = 30,
  DWARF_SP = 31,
  DWARF_PC = 32,
};
#endif

#endif  // DWARF_CONSTANTS_H_
