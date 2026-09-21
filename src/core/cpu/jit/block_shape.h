// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// Where a block ends. Shared by every backend on purpose: block boundaries
// decide where the budget is tested, so two backends that cut blocks
// identically produce identical slice interleaving -- which makes the AArch64
// backend's frame hashes an oracle for the A32 one (docs/arm32-jit-scoping.md).
#pragma once
#include "core/cpu/arm_decode.h"
#include "core/types.h"

namespace ds::jit::shape {

// CP15 writes the core accepts and ignores (cp15.cpp): every c7 cache /
// write-buffer operation except wait-for-interrupt (c7,c0,4 and c7,c8,2).
inline bool mcr_is_nop(u32 instr, bool a9) {
  if (!a9 || ((instr >> 8) & 0xF) != 15) return false;
  const u32 crn = (instr >> 16) & 0xF, crm = instr & 0xF, opc2 = (instr >> 5) & 7;
  return crn == 7 && !((crm == 0 && opc2 == 4) || (crm == 8 && opc2 == 2));
}
// MSR forms translated inline: CPSR writes from a register or immediate. The
// mode must not change (tested at run time; otherwise the interpreter runs it).
// SUBS/ADDS/MOVS pc, rn, #imm with S set: the exception return. Inlined --
// it is one mode switch (restore_cpsr) and an interworking branch, which is
// 120 k fallbacks a run in Golden Sun's IRQ exit. The register-operand forms
// and the other opcodes keep the interpreter.
inline bool dp_exc_return(u32 instr) {
  if ((instr >> 28) != 0xE) return false;                       // unconditional only
  if (arm::decode_arm(instr) != arm::AOp::DpImm) return false;
  if (!(instr & (1u << 20))) return false;                      // S
  if (((instr >> 12) & 0xF) != 15) return false;                // rd == pc
  const u32 opc = (instr >> 21) & 0xF;
  return opc == 0x2 || opc == 0x4 || opc == 0xD;                // SUB / ADD / MOV
}

inline bool msr_inline(u32 instr) {
  // SPSR writes inline too: they bank nothing and change no mode -- the
  // current mode's SPSR is one word -- and user/system mode, which have no
  // SPSR, is tested at run time. Golden Sun's IRQ handler runs one per
  // interrupt, 120 k a run through the interpreter before this.
  return !(arm::decode_arm(instr) == arm::AOp::MsrReg && (instr & 0xF) == 15);
}

// `LDM ^` / `STM ^` (the S bit without r15 in a load list): a user-bank
// transfer, and 120 k fallbacks a run in Golden Sun's IRQ exit. Inlined for
// the shapes where the architecture -- not the interpreter -- says the user
// bank is reachable without a mode switch. `bank_r8_r12` covers USR and FIQ
// only and `bank_r13`/`bank_r14` are per mode (cpu.h), so in IRQ/SVC/ABT/UND
// the live r0-r12 ARE the user's and only r13/r14 belong elsewhere. Those two
// modes-with-banked-r8 (FIQ) and the banks-are-current modes (USR/SYS) are
// tested at run time and keep the interpreter.
//
// Writeback stays on the interpreter: architecturally UNPREDICTABLE with S,
// and the interpreter writes the base back while still switched to user mode,
// so a base of r13/r14 would land in a different bank than an inline path
// would choose. No cost to excluding it -- the shape does not occur.
inline bool ldm_user_inline(u32 instr) {
  using arm::AOp;
  const AOp op = arm::decode_arm(instr);
  if (op != AOp::Ldm && op != AOp::Stm) return false;
  if (!(instr & (1u << 22))) return false;                      // S
  if (instr & (1u << 21)) return false;                         // writeback
  const u32 list = instr & 0xFFFF;
  if (list == 0 || ((instr >> 16) & 0xF) == 15) return false;
  const bool load = instr & (1u << 20);
  return !(load && (list & 0x8000));                            // else: CPSR restore, not a user-bank transfer
}

inline bool arm_ends_block(u32 instr, bool a9) {
  using arm::AOp;
  const u32 cond = instr >> 28;
  if (cond == 0xF) return true;
  switch (arm::decode_arm(instr)) {
  case AOp::B: case AOp::Bl: case AOp::Bx: case AOp::BlxReg: case AOp::Swi: case AOp::Bkpt: case AOp::Undefined:
  case AOp::Cdp: case AOp::Ldc: case AOp::Stc: case AOp::Mrc:
    return true;
  case AOp::Mcr: return !mcr_is_nop(instr, a9);
  case AOp::MsrReg: case AOp::MsrImm: return !msr_inline(instr);
  case AOp::DpImm: case AOp::DpImmShift: case AOp::DpRegShift: {
    const u32 opcode = (instr >> 21) & 0xF;
    const bool test = opcode >= 8 && opcode <= 0xB;
    return !test && ((instr >> 12) & 0xF) == 15;
  }
  case AOp::LdrStrImm: case AOp::LdrStrReg: case AOp::LdrStrHImm: case AOp::LdrStrHReg:
    return (instr & (1u << 20)) && ((instr >> 12) & 0xF) == 15;
  case AOp::Ldm: return (instr & 0x8000) != 0;
  default: return false;
  }
}

inline bool thumb_ends_block(u16 instr) {
  using arm::TOp;
  switch (arm::decode_thumb(instr)) {
  case TOp::HiRegOp: {
    const u32 rd = (instr & 7) | ((instr >> 4) & 8), op = (instr >> 8) & 3;
    return rd == 15 && (op == 0 || op == 2);
  }
  case TOp::BxBlx: case TOp::BCond: case TOp::Swi: case TOp::B: case TOp::BlSuffix: case TOp::BlxSuffix:
  case TOp::Bkpt: case TOp::Undefined:
    return true;
  case TOp::PushPop: return (instr & (1 << 11)) && (instr & (1 << 8));
  case TOp::StmLdm: return (instr & 0xFF) == 0;
  default: return false;
  }
}

} // namespace ds::jit::shape
