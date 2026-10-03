# AGENT KNOWLEDGE — XTulator dynrec crash (read before working)

## Current state (verified)
- Interpreter path works. dynrec crashes IMMEDIATELY when toggled on (regression: a prior build booted to FreeDOS and ran `DIR`).
- Crash: SIGSEGV @ PC = 0x2d5f3000 = code_cache_base (0x2d5b3000) + CODE_CACHE_SIZE (0x40000), CPSR=0x60000010 (Thumb bit set). fault PC is one past the end of the 256KB cache (mapped cache is [0x2d5b3000..0x2d5f3000)).
- Active crash block = x86_pc 0x0F454, arm_off=10116, code_len=40, func=0x2d5b5784. entry_cpu == fault_r0 == 0x4394F7C8 => CPU pointer intact; crash is INSIDE the helper tree (dinstr_exec_one->cpu_exec), NOT a bad dispatcher dispatch.
- Active block ARM dump is CORRECT: push; mov r4,r0; mov r0,r4; mov r12,#0x50; ORR r12,#0x8A00; ORR r12,#0x00020000; ORR r12,#0x42000000; BLX r12 (target 0x42028A50 = dinstr_exec_one) [correct]; B .-4; POP. Emission verified by harness (585 checks pass) + .so disasm + bounds-checked dispatch BLX.
- No static .so instruction computes/branches to cache_base+0x40000 (confirmed by subagent objdump: only dynrec_exec BLX r7 = cache_base+write_ptr, and write_ptr is bounds-checked <= 0x3C000). So the 0x2d5f3000 target is a RUNTIME-computed/corrupted pointer reached via an indirect call inside cpu_exec.
- Dispatch trace tail (fresh, /sdcard/xt_dispatch_trace.log) shows clean ok=1 2-byte blocks 0x0F434..0x0F452 then crash at 0x0F454. Guest bytes at 0x0F454 = 00 00 00 00 (executed as `ADD [bx+si],al`). Region F0000:0x434..0x454 reads as 0x000000.
- /sdcard/xtulator_dynrec.log is MISSING; /sdcard/xt_memcb_dump.log is MISSING => cpu_write/cpu_read stale-callback guards did NOT fire => not a stale memory callback.
- crashlog() gated on TRACE_FLAG_DIAG (off) => per-block Exec/Exit trace not captured; signal handler DOES dump active block.

## Root-cause hypothesis (to confirm, not assumed)
Crash = Thumb BLX to 0x2d5f3000 reached via an UNGUARDED indirect call in cpu_exec: cpu_intcall calls cpu->int_callback[intnum] with NO address-range guard (cpu.c:1107-1108), unlike cpu_read/cpu_write (memory.c:58, guarded to 0x40000000..0x50000000). A corrupted int_callback slot == 0x2d5f3001 would BLX to cache_end in Thumb -> exact crash signature. The corruption cascade begins at the zeroed-BIOS region F000:0454.

## Regression scope
- Regression is in uncommitted xtulator/ source (worktree git has only setup commits). Need git diff vs last known-good to isolate. Candidate recent edits: AL/AX immediate opcodes added to dynrec.c; cpu_write/cpu_read stale-callback guards (memory.c); emit_bl absorber; ORR (0x3800000=ORR) encoding.

## Helper-address PIC fix (applied)
- dynrec.c used `emit_bl((uint32_t)&helper)` which captures compile-time link-time symbol addresses — wrong under -fPIC when the .so is relocated at load. Fix: `struct dynrec_helpers_t` table + `dynrec_resolve_helpers()` resolved at runtime via `(uint32_t)(uintptr_t)&helper`, called from `dynrec_enable()` BEFORE `flush_all_blocks()`.
- All 69 `emit_bl` calls now use `g_helpers.<field>`. Zero `emit_bl((uint32_t)&)` patterns remain.
- Build fix: 29 missing `extern` declarations were placed AFTER `dynrec_resolve_helpers()` — moved all externs before the resolve function. Removed duplicate `dinstr_jcond_rel8` assignment and duplicate `dinstr_add_r16_imm`/`dinstr_cmp_r16_imm` externs.
  - libxtulator.so builds clean in docker xtulator-android16 (NDK r10e, ELF32 ARM, -fPIC). dynrectest: 655 checks pass, 0 failures.

## Progress (2026-10-03)
- PIC helper-address fix verified on IS01: code cache overflow crash (fault_addr=0x2d5f3000) is FIXED.
- dynrec counter reached 1000 (was stuck at 1), process runs ~4s before hitting a DIFFERENT crash.
- New crash: signal=11, fault_addr=0x42, at x86_pc=0x00042 (IVT data interpreted as code).
- Root cause of new crash: LOOP (0xE0-0xE3) opcodes missing from translate_block switch => falls to default => translation fails => interpreter fallback => interrupt fires during fallback => IVT entry (CS=0x0000, uninitialized) is jumped to => dynarec executes IVT data as x86 instructions => R4 (cpu ptr) corrupted by helper calls on IVT garbage bytes => null deref.
- Additional missing opcodes in translate_block: 0xB0-0xB7 (MOV r8,imm8), 0x60-0x6F, 0xC0-0xEF, 0xFE-0xFF. All present in x86_instr_len but NOT in the translate_block switch.
- Next fix: add cases for 0xE0-0xE3 (LOOP/LOOPE/LOOPNE/JCXZ) and 0xB0-0xB7 (MOV r8,imm8) to translate_block, then rebuild and retest.
