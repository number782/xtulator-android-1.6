# Query Knowledge — dynrec startup crash diagnosis (DO NOT WORK ON THESE)

## Confirmed findings (agents: do NOT re-investigate these)

1. **emit_bl absorber removed in commit 35950ec**. Old code (ff54899) emitted `0xEAFFFFFF` (B .-4) after both BL and BLX R12. New code (35950ec, current working tree) does NOT emit it. On ARM, BL/BLX set LR = call_addr + 8, not +4 as the new comments claim. Without absorber, every helper return skips the next instruction. THIS IS THE PRIMARY CRASH CAUSE.

2. **Wrong shift encoding in emit_movw and emit_bl**. Code uses `(4 << 7)` and `(8 << 7)` for shift_imm. shift_imm is at ARM bits [11:8], so correct is `(4 << 8)` and `(8 << 8)`. Using `<<7` halves the shift (LSL #2 instead of LSL #4). Affects all 16-bit immediate loads.

3. **R12 building in emit_bl indirect path is broken**. Old code (ff54899) used ARM ROR immediate encoding (correct). New code uses LSL shifts (broken due to #2). dynrectest confirms: R12 = 0xc0000003 instead of 0x4203330c. 41 failures in dynrectest.

4. **dynrectest rewritten to match the bugs**. Assumes LR = call+4 (wrong). `check_lr_returns_to_next_word` function is defined but NEVER CALLED (confirmed by compiler warning). Test does NOT execute generated code (line 14: "deliberately does NOT execute"). So it can't catch the runtime crash from #1.

5. **The PIC helper table fix in 35950ec is CORRECT** (g_helpers table + dynrec_resolve_helpers()). This part is fine. Only the absorber removal and R12 rewriting are broken.

6. **0x8C/0x8D merge (working tree, uncommitted)** uses `emit_bl(g_helpers.dinstr_mov_ew_sw)` and `emit_bl(g_helpers.dinstr_lea_gv_m)` which go through the broken emit_bl. The helpers themselves (dinstr_mov_ew_sw, dinstr_lea_gv_m) are correct.

## What agents SHOULD verify

- Verify ARM BL/BLX LR semantics definitively (LR = call+8 for ARM state)
- Verify the old ff54899 emit_bl code that had the absorber was correct
- Check if there are any OTHER emit_bl issues beyond what we found
- Look at what the git diff between ff54899 and 35950ec changed that we might have missed

## VERIFIED by agents (confirmed)

### Agent 1 (git diff ff54899 vs 35950ec) — CONFIRMED ALL FINDINGS
- `0xEAFFFFFF` absorber present in ff54899, absent in 35950ec: CONFIRMED
- Old ROR-based R12 building (`(8 << 8)` ROR #16, `(4 << 8)` ROR #8): CONFIRMED
- New LSL-based R12 building (`(4 << 7)` LSL #4 x2 = LSL #8, `(8 << 7)` LSL #8 x2 = LSL #16): CONFIRMED
- emit_ldr_imm32 also changed from ROR-based to LSL-based (same <<7 bug carries over): CONFIRMED
- Additional diff changes: emit32 guard page overflow check, mmap allocates CODE_CACHE_SIZE+4096 with PROT_NONE guard page, dynrec_exec has guard-page safety check for arm_offset >= CODE_CACHE_SIZE: CONFIRMED (these are fine — the guard page infrastructure is correct, only the code generation bugs remain)

### Agent 2 (working tree dynrec.c) — CONFIRMED ALL FINDINGS
- emit32() at line 159: bounds guard added (fine)
- emit_movw() at lines 229-246: uses `(4 << 7)` for LSL #4 (broken, should be `<< 8`)
- emit_movw ORR uses `0x3800000` = TST not ORR (broken, should be `0x3B00000`)
- emit_bl() at lines 299-374: NO absorber; comment falsely claims "BL/BLX set LR = call + 4" (should be +8); R12 built with broken LSL; BLX R12 at line 367 is last instruction (should have B .-4 after it)
- emit22 does NOT EXIST (confirmed)

### Agent 3 (dynrectest.c) — CONFIRMED ALL FINDINGS
- `check_lr_returns_to_next_word` defined at line 519, NEVER CALLED (confirmed by grep + nm): CONFIRMED
- "deliberately does NOT execute" at line 14-16: CONFIRMED
- LR = call+4 assumption at lines 464, 472, 513, 521, 688-690: CONFIRMED (wrong for ARM — should be +8)
- 41 failures in emit_bl tests (all out-of-range R12 targets, e.g. R12 = 0x0c000040 instead of 0x4203330c): CONFIRMED
- In-range BL test (single 0xEB000000 word) PASSES — because it has the same missing-absorber bug but the test doesn't know that

### Agent 4 (other emit_bl issues) — CONFIRMED + NEW FINDING
- No other functions emit BL/BLX directly — only emit_bl() at lines 367/369: CONFIRMED
- 0x8C calls `emit_bl(g_helpers.dinstr_mov_ew_sw)` — helper chain (decl line 624, struct field line 722, resolve line 803) all correct: CONFIRMED
- 0x8D calls `emit_bl(g_helpers.dinstr_lea_gv_m)` — helper chain (decl line 625, struct field line 723, resolve line 804) all correct: CONFIRMED

### NEW FINDING (not a crash cause): 0x8E dead helper
- 0x8E at dynrec.c:1844 uses `emit_bl(g_helpers.dinstr_exec_one)` (line 1846) instead of the dedicated helper `dinstr_mov_sw_ew`
- `dinstr_mov_sw_ew` is declared (line 626), in the struct (line 724), and resolved (line 803) but NEVER CALLED → dead code
- This is a performance issue (falls back to interpreter instead of native) but NOT a crash cause
