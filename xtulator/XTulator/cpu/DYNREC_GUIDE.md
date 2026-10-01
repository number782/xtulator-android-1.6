# Dynarec Opcode Implementation Guide

## Architecture Overview

The dynrec translates x86 instructions to ARM code at runtime:
1. `dynrec.c:translate_block()` reads x86 opcodes and emits ARM instructions
2. Helper functions in `dynrec_helpers.c` execute the actual x86 instruction logic
3. Generated ARM code calls helpers via `BL` instructions

## Key Files
- `xtulator/XTulator/cpu/dynrec.c` - ARM code emission + translate_block switch
- `xtulator/XTulator/cpu/dynrec_helpers.c` - C helper function implementations  
- `xtulator/XTulator/cpu/dynrec.h` - API declarations
- `xtulator/XTulator/cpu/cpu.c` - Original interpreter (reference logic)
- `xtulator/XTulator/cpu/cpu.h` - CPU_t struct, macros, byteregtable

## Patterns

### Adding a new opcode:
1. In `dynrec_helpers.c`: Add a C function that mirrors the cpu.c logic
   ```c
   int dinstr_opname(CPU_t* cpu, ...) {
       /* logic from cpu.c */
       return 0;  /* or 1 to terminate block */
   }
   ```
2. In `dynrec.c`:
   - Add `extern int dinstr_opname(CPU_t* cpu, ...);` to the extern block (around line 231)
   - Add case in `translate_block()` switch:
   ```c
   case 0xNN:  /* OPCODE */
       emit_bl((uint32_t)&dinstr_opname);
       offset++;
       break;
   ```

### ARM Code Emission Functions (in dynrec.c):
- `emit32(uint32_t instr)` - emit raw ARM instruction
- `emit_mov_imm(int Rd, uint8_t imm)` - MOV Rd, #imm8
- `emit_movw(int Rd, uint16_t imm)` - MOVW Rd, #imm16 (multiple instructions)
- `emit_bl(uint32_t target_addr)` - BL to target
- `emit_subs_imm(uint8_t imm)` - SUBS r10, r10, #imm

### Block Control:
- `goto block_done` - terminate current block (for control-flow changes: JMP, CALL, RET, INT)
- `return -1` - can't translate this opcode, fall back to interpreter
- `break` - continue to next instruction in the block (fall through to counter decrement)

## Key CPU_t Fields:
- `cpu->opcode` - current opcode byte
- `cpu->ip` - instruction pointer
- `cpu->regs.wordregs[8]` - 16-bit registers (AX,CX,DX,BX,SP,BP,SI,DI)
- `cpu->regs.byteregs[8]` - 8-bit registers (see cpu.h for regal/regah/etc.)
- `cpu->segregs[4]` - segment registers (ES,CS,SS,DS)
- `cpu->cf, pf, af, zf, sf, tf, ifl, df, of` - flags
- `cpu->mode, reg, rm` - ModR/M fields (set by modregrm() macro)
- `cpu->ea` - effective address
- `cpu->disp16, disp8` - displacement values

## ModR/M Decoding (for opcodes 0x00-0xFF that use ModR/M):
The `modregrm(cpu)` macro from cpu.h sets cpu->mode, cpu->reg, cpu->rm.
For dynrec, the helpers need to handle ModR/M internally.
The existing dynrec_helpers.c has `dynrec_getea()`, `dynrec_readrm8()`, etc.

## Current Opcode Coverage:
Implemented in helpers: 0x06,0x07,0x0E,0x16,0x17,0x1E,0x1F,0x40-0x4F (INC/DEC),
0x50-0x5F (PUSH/POP), 0x70-0x7F (Jcond), 0x84,0x85,0x8C,0x8D,0x8E, 0x90,0x9C,0x9D,
0x9E,0x9F, 0xB8-0xBF, 0xC3,0xCD,0xE8,0xE9,0xEB,0xF4, 0xF0,0xF5,0xF8-0xFD

TODO - High Priority (0x00-0x3F):
0x00-0x05 (ADD), 0x08-0x0D (OR), 0x10-0x15 (ADC), 0x18-0x1D (SBB),
0x20-0x25 (AND), 0x28-0x2D (SUB), 0x30-0x35 (XOR), 0x38-0x3D (CMP),
0x27 (DAA), 0x2F (DAS), 0x37 (AAA), 0x3F (AAS)

TODO - Medium Priority:
0x61-0x6F (string/IO ops - complex, likely fall back to interpreter)
0xA0-0xA3 (MOV moffs), 0xA4-AF (string ops), 0xC0-C1 (shift groups)
0xC2 (RET Iw), 0xC4-C5 (LES/LDS), 0xC6-C7 (MOV ib/id), 0xCA-CB (RETF)
0xCC (INT3), 0xD0-D3 (shift groups), 0xD4-D5 (AAM/AAD), 0xD7 (XLAT)
0xE0-E3 (LOOP/JCXZ), 0xE4-E7 (IN/OUT Ib), 0xEC-EF (IN/OUT DX), 0xEA (JMP Ap)
0xF1 (ICEBP/INT1), 0xF6-F7 (test/neg/not/mul/div groups), 0xFE-FF (INC/DEC/JMP/CALL groups)

## Notes from Agents:
- The `parity` table from cpu.c is NOT accessible — use `calc_parity()` helper
- FUNC_INLINE functions from cpu.c are inlined, not linkable — implement logic directly
- `byteregtable` IS accessible — declared as extern in dynrec_helpers.c
- ModR/M helpers: dynrec_getea, dynrec_readrm8/16, dynrec_writerm8/16 are available
- The `modregrm(cpu)` macro IS accessible via cpu.h — it reads the ModR/M byte and sets fields
- Helper argument passing: R0=cpu (first), R1/R2/... for additional args
- For conditional jumps, return 0 (don't terminate block) since execution may continue sequentially
