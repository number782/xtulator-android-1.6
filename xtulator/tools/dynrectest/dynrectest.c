/*
 * dynrectest.c — standalone test skeleton for the dynrec translator.
 *
 * WHY THIS EXISTS
 * ---------------
 * Debugging the dynrec on the IS01 meant reasoning about ARM register dumps
 * captured from a crash inside a 3.4MB emulator, where every hypothesis
 * needed a 10-minute build/install/boot cycle to disprove. This harness
 * removes the device entirely: it pulls the real translator in via #include,
 * feeds it synthetic x86 byte sequences, then disassembles and audits the
 * ARM words it emits. A wrong opcode, a clobbered callee-saved register or
 * a branch into a literal pool shows up in milliseconds.
 *
 * It deliberately does NOT execute the generated code -- there is no ARM
 * runtime available here. It audits structure and invariants instead, which
 * is what the crash actually turned out to be about.
 *
 * BUILD
 *   cc -std=gnu99 -Wall -fno-pie -no-pie -o dynrectest dynrectest.c
 *
 *   -fno-pie is required: dynrec.c casts every helper address through
 *   (uint32_t), which is exact on the 32-bit IS01 but silently truncates
 *   pointers when the host runs as PIE (ASLR loads above 4GB). -no-pie keeps
 *   the stub helpers in the low 4GB so the in-range BL audit is meaningful.
 *
 * USAGE
 *   ./dynrectest              run the built-in suite
 *   ./dynrectest <hexbytes>   translate one ad-hoc sequence, e.g. 5031c0e9
 *
 * The generated ARM is hex-dumped with per-instruction annotation, so the
 * layout can be checked against the emission model by eye.
 */

#define __ARM_ARCH_5TE__ 1   /* compile the real translator on this host */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "armdec.h"

/* CPU_t / I8259_t live in the emulator's own headers; pull them in before
 * the forward declarations below so those can use the real types. */
#include "../../XTulator/cpu/cpu.h"
#include "../../XTulator/chipset/i8259.h"

/* ------------------------------------------------------------------ *
 * Forward declarations of every external the translator TU references.
 * dynrec.c *calls* some of these (cacheflush, cpu_exec, i8259_nextintr)
 * without including a header that declares them, so they must be visible
 * before the #include or C99 emits implicit-declaration errors. The
 * dinstr_* set is declared here purely so the stub definitions below
 * inherit exactly the prototypes dynrec.c uses.
 * ------------------------------------------------------------------ */

void cpu_exec(CPU_t *cpu, uint32_t execloops);
uint8_t i8259_nextintr(I8259_t *pic);
int cacheflush(long start, long end, long flags);
void cpu_intcall(CPU_t *cpu, uint8_t intnum);

int dinstr_nop(CPU_t* cpu);
int dinstr_mov_r_i(CPU_t* cpu, uint8_t regnum, uint16_t imm);
int dinstr_inc_r16(CPU_t* cpu, uint8_t regnum);
int dinstr_dec_r16(CPU_t* cpu, uint8_t regnum);
int dinstr_push_r16(CPU_t* cpu, uint8_t regnum);
int dinstr_pop_r16(CPU_t* cpu, uint8_t regnum);
int dinstr_inc_r8(CPU_t* cpu, uint8_t regnum);
int dinstr_dec_r8(CPU_t* cpu, uint8_t regnum);
int dinstr_hlt(CPU_t* cpu);
int dinstr_int(CPU_t* cpu, uint8_t intnum);
int dinstr_jmp_rel8(CPU_t* cpu, int8_t rel);
int dinstr_jcond_rel8(CPU_t* cpu, uint8_t opcode, int8_t rel);
int dinstr_jmp_rel16(CPU_t* cpu, int16_t rel);
int dinstr_call_rel16(CPU_t* cpu, int16_t rel);
int dinstr_ret_near(CPU_t* cpu);
int dinstr_add_r16_imm(CPU_t* cpu, uint8_t regnum, uint16_t imm);
int dinstr_cmp_r16_imm(CPU_t* cpu, uint8_t regnum, uint16_t imm);
int dinstr_pushf(CPU_t* cpu);
int dinstr_popf(CPU_t* cpu);
int dinstr_inc_r8(CPU_t* cpu, uint8_t regnum);
int dinstr_dec_r8(CPU_t* cpu, uint8_t regnum);
int dinstr_push_es(CPU_t* cpu);
int dinstr_pop_es(CPU_t* cpu);
int dinstr_push_cs(CPU_t* cpu);
int dinstr_pop_cs(CPU_t* cpu);
int dinstr_push_ss(CPU_t* cpu);
int dinstr_pop_ss(CPU_t* cpu);
int dinstr_push_ds(CPU_t* cpu);
int dinstr_pop_ds(CPU_t* cpu);
int dinstr_sahf(CPU_t* cpu);
int dinstr_lahf(CPU_t* cpu);
int dinstr_mov_r8_imm8(CPU_t* cpu, uint8_t regnum, uint8_t imm);
int dinstr_loop_rel8(CPU_t* cpu, uint8_t opcode, int8_t rel);
int dinstr_advance_ip(CPU_t* cpu, uint16_t n);
int dinstr_test_al_imm8(CPU_t* cpu, uint8_t imm);
int dinstr_test_ax_imm16(CPU_t* cpu, uint16_t imm);
int dinstr_test_gb_eb(CPU_t* cpu);
int dinstr_test_gv_ev(CPU_t* cpu);
int dinstr_mov_ew_sw(CPU_t* cpu);
int dinstr_lea_gv_m(CPU_t* cpu);
int dinstr_mov_sw_ew(CPU_t* cpu);
int dinstr_cmc(CPU_t* cpu);
int dinstr_clc(CPU_t* cpu);
int dinstr_stc(CPU_t* cpu);
int dinstr_cli(CPU_t* cpu);
int dinstr_sti(CPU_t* cpu);
int dinstr_cld(CPU_t* cpu);
int dinstr_std(CPU_t* cpu);
int dinstr_or_al_imm8(CPU_t* cpu, uint8_t imm);
int dinstr_or_ax_imm16(CPU_t* cpu, uint16_t imm);
int dinstr_xor_al_imm8(CPU_t* cpu, uint8_t imm);
int dinstr_xor_ax_imm16(CPU_t* cpu, uint16_t imm);
int dinstr_sub_al_imm8(CPU_t* cpu, uint8_t imm);
int dinstr_sub_ax_imm16(CPU_t* cpu, uint16_t imm);
int dinstr_cmp_al_imm8(CPU_t* cpu, uint8_t imm);
int dinstr_cmp_ax_imm16(CPU_t* cpu, uint16_t imm);
int dinstr_adc_al_imm8(CPU_t* cpu, uint8_t imm);
int dinstr_adc_ax_imm16(CPU_t* cpu, uint16_t imm);
int dinstr_sbb_al_imm8(CPU_t* cpu, uint8_t imm);
int dinstr_sbb_ax_imm16(CPU_t* cpu, uint16_t imm);
int dinstr_and_al_imm8(CPU_t* cpu, uint8_t imm);
int dinstr_and_ax_imm16(CPU_t* cpu, uint16_t imm);
int dinstr_exec_one(CPU_t* cpu);

/* Pull in the translator itself so we get its statics: translate_block(),
 * block_cache, code_cache_base, emit helpers. */
#include "../../XTulator/cpu/dynrec.c"

/* ------------------------------------------------------------------ *
 * Stubs for every external the translator TU references. Only their
 * addresses matter (emit_bl takes them); none are ever called here.
 * ------------------------------------------------------------------ */

/* CPU / chipset entry points referenced by dynrec_exec() */
void cpu_exec(CPU_t *cpu, uint32_t execloops) { (void)cpu; (void)execloops; }
uint8_t i8259_nextintr(I8259_t *pic) { (void)pic; return 0; }
void cpu_intcall(CPU_t *cpu, uint8_t intnum) { (void)cpu; (void)intnum; }
int cacheflush(long start, long end, long flags) {
    (void)start; (void)end; (void)flags; return 0;
}

/* Globals declared by debuglog.h */
volatile uint8_t trace_flags = 0;

/* Memory map declared by memory.h -- 1MB of direct RAM, no MMIO callbacks. */
uint8_t *memory_mapRead[MEMORY_RANGE];
uint8_t *memory_mapWrite[MEMORY_RANGE];
uint8_t (*memory_mapReadCallback[MEMORY_RANGE])(void *, uint32_t);
void (*memory_mapWriteCallback[MEMORY_RANGE])(void *, uint32_t, uint8_t);

/* Helper stubs. Prototypes are above (copied verbatim from dynrec.c so the
 * definitions match); bodies are never called -- only their addresses are
 * used, as BL targets. On the real IS01 these live in libxtulator.so's .text
 * section, which is 4-byte-aligned for ARM; align them here too so the host
 * audit exercises the same word-aligned arithmetic emit_bl() relies on. */
#define STUB(name) __attribute__((aligned(4))) int name(CPU_t *cpu) \
    { (void)cpu; return 0; }
STUB(dinstr_nop) STUB(dinstr_hlt) STUB(dinstr_exec_one)
STUB(dinstr_ret_near)
STUB(dinstr_pushf) STUB(dinstr_popf) STUB(dinstr_sahf) STUB(dinstr_lahf)
STUB(dinstr_cmc) STUB(dinstr_clc) STUB(dinstr_stc) STUB(dinstr_cli)
STUB(dinstr_sti) STUB(dinstr_cld) STUB(dinstr_std)
STUB(dinstr_push_es) STUB(dinstr_pop_es) STUB(dinstr_push_cs)
STUB(dinstr_pop_cs) STUB(dinstr_push_ss) STUB(dinstr_pop_ss)
STUB(dinstr_push_ds) STUB(dinstr_pop_ds)
STUB(dinstr_test_gb_eb) STUB(dinstr_test_gv_ev)
STUB(dinstr_mov_ew_sw) STUB(dinstr_lea_gv_m) STUB(dinstr_mov_sw_ew)

/* 2-arg forms: (cpu, uint8_t) */
#define STUB1(name) __attribute__((aligned(4))) int name(CPU_t *cpu, uint8_t a) \
    { (void)cpu; (void)a; return 0; }
/* 3-arg forms: (cpu, uint8_t regnum, uint16_t imm) */
#define STUB3(name) __attribute__((aligned(4))) int name(CPU_t *cpu, uint8_t a, uint16_t b) \
    { (void)cpu; (void)a; (void)b; return 0; }

STUB1(dinstr_int)
STUB1(dinstr_inc_r8) STUB1(dinstr_dec_r8)
STUB1(dinstr_inc_r16) STUB1(dinstr_dec_r16)
STUB1(dinstr_push_r16) STUB1(dinstr_pop_r16)
STUB1(dinstr_or_al_imm8) STUB1(dinstr_xor_al_imm8) STUB1(dinstr_sub_al_imm8)
STUB1(dinstr_cmp_al_imm8) STUB1(dinstr_adc_al_imm8) STUB1(dinstr_sbb_al_imm8)
STUB1(dinstr_and_al_imm8) STUB1(dinstr_add_al_imm8) STUB1(dinstr_test_al_imm8)

STUB3(dinstr_mov_r_i)
STUB3(dinstr_add_r16_imm) STUB3(dinstr_cmp_r16_imm)

/* 3-arg forms where the 2nd parameter is already 16-bit */
__attribute__((aligned(4))) int dinstr_jmp_rel8(CPU_t *cpu, int8_t rel) { (void)cpu; (void)rel; return 0; }
__attribute__((aligned(4))) int dinstr_jcond_rel8(CPU_t *cpu, uint8_t op, int8_t rel) {
    (void)cpu; (void)op; (void)rel; return 0;
}
__attribute__((aligned(4))) int dinstr_jmp_rel16(CPU_t *cpu, int16_t rel) { (void)cpu; (void)rel; return 0; }
__attribute__((aligned(4))) int dinstr_call_rel16(CPU_t *cpu, int16_t rel) { (void)cpu; (void)rel; return 0; }

#define STUB2(name) __attribute__((aligned(4))) int name(CPU_t *cpu, uint16_t a) \
    { (void)cpu; (void)a; return 0; }
STUB2(dinstr_advance_ip)
STUB2(dinstr_or_ax_imm16) STUB2(dinstr_xor_ax_imm16) STUB2(dinstr_sub_ax_imm16)
STUB2(dinstr_cmp_ax_imm16) STUB2(dinstr_adc_ax_imm16) STUB2(dinstr_sbb_ax_imm16)
STUB2(dinstr_and_ax_imm16) STUB2(dinstr_add_ax_imm16) STUB2(dinstr_test_ax_imm16)

/* Custom stubs for signatures not covered by STUB/STUB1/STUB2/STUB3 macros. */
__attribute__((aligned(4))) int dinstr_mov_r8_imm8(CPU_t *cpu, uint8_t a, uint8_t b)
    { (void)cpu; (void)a; (void)b; return 0; }
__attribute__((aligned(4))) int dinstr_loop_rel8(CPU_t *cpu, uint8_t a, int8_t b)
    { (void)cpu; (void)a; (void)b; return 0; }

/* ------------------------------------------------------------------ *
 * Harness
 * ------------------------------------------------------------------ */

/* Where the IS01 actually puts the code cache (mmap'd here, observed). The
 * out-of-range emit_bl() path is only live when the code and the helpers are
 * more than ±32MB apart -- ~355MB on the device -- so the suite relocates the
 * cache here to guarantee that path is exercised rather than silently skipped. */
#define IS01_CACHE_BASE 0x2D5B3000UL

static void relocate_code_cache_to(unsigned long want);

#define TEST_SEG   0xF000
#define TEST_OFF   0x0000
#define TEST_ADDR  ((TEST_SEG << 4) + TEST_OFF)

static uint8_t rom[0x100000];
static CPU_t   cpu;

static int failures;
static int checks;

/* Every helper address the translator is allowed to branch to. Anything a
 * generated branch targets that is NOT in this set and NOT inside the block
 * itself is a bug. */
static const void *known_targets[512];
static int n_known;

static void note_target(const void *p) {
    if (n_known < (int)(sizeof(known_targets) / sizeof(known_targets[0])))
        known_targets[n_known++] = p;
}

static void build_target_table(void) {
    note_target(&dinstr_nop);            note_target(&dinstr_hlt);
    note_target(&dinstr_exec_one);       note_target(&dinstr_advance_ip);
    note_target(&dinstr_ret_near);       note_target(&dinstr_pushf);
    note_target(&dinstr_popf);           note_target(&dinstr_sahf);
    note_target(&dinstr_lahf);           note_target(&dinstr_cmc);
    note_target(&dinstr_clc);            note_target(&dinstr_stc);
    note_target(&dinstr_cli);            note_target(&dinstr_sti);
    note_target(&dinstr_cld);            note_target(&dinstr_std);
    note_target(&dinstr_push_es);        note_target(&dinstr_pop_es);
    note_target(&dinstr_push_cs);        note_target(&dinstr_pop_cs);
    note_target(&dinstr_push_ss);        note_target(&dinstr_pop_ss);
    note_target(&dinstr_push_ds);        note_target(&dinstr_pop_ds);
    note_target(&dinstr_test_gb_eb);     note_target(&dinstr_test_gv_ev);
    note_target(&dinstr_mov_ew_sw);      note_target(&dinstr_lea_gv_m);
    note_target(&dinstr_mov_sw_ew);      note_target(&dinstr_mov_r_i);
    note_target(&dinstr_inc_r16);        note_target(&dinstr_dec_r16);
    note_target(&dinstr_push_r16);       note_target(&dinstr_pop_r16);
    note_target(&dinstr_int);            note_target(&dinstr_inc_r8);
    note_target(&dinstr_dec_r8);         note_target(&dinstr_jmp_rel8);
    note_target(&dinstr_jcond_rel8);     note_target(&dinstr_jmp_rel16);
    note_target(&dinstr_call_rel16);     note_target(&dinstr_add_r16_imm);
    note_target(&dinstr_cmp_r16_imm);    note_target(&dinstr_add_al_imm8);
    note_target(&dinstr_and_al_imm8);    note_target(&dinstr_or_al_imm8);
    note_target(&dinstr_xor_al_imm8);    note_target(&dinstr_sub_al_imm8);
    note_target(&dinstr_cmp_al_imm8);    note_target(&dinstr_adc_al_imm8);
    note_target(&dinstr_sbb_al_imm8);    note_target(&dinstr_test_al_imm8);
    note_target(&dinstr_add_ax_imm16);   note_target(&dinstr_and_ax_imm16);
    note_target(&dinstr_or_ax_imm16);    note_target(&dinstr_xor_ax_imm16);
    note_target(&dinstr_sub_ax_imm16);   note_target(&dinstr_cmp_ax_imm16);
    note_target(&dinstr_adc_ax_imm16);   note_target(&dinstr_sbb_ax_imm16);
    note_target(&dinstr_test_ax_imm16);
}

static void fail(const char *test, const char *fmt, ...) {
    va_list ap;
    failures++;
    printf("  FAIL [%s] ", test);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static void expect(int cond, const char *test, const char *msg) {
    checks++;
    if (!cond) fail(test, "%s", msg);
}

/* expect() with a formatted message -- the emit_bl audit needs the target
 * address baked into every assertion so a failure says which case broke. */
static void expectf(int cond, const char *test, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    expect(cond, test, buf);
}

static int is_known_target(uintptr_t v) {
    int i;
    for (i = 0; i < n_known; i++)
        if ((uintptr_t)known_targets[i] == v) return 1;
    return 0;
}

/*
 * Translate one x86 byte sequence and audit the emitted block.
 * Returns the block entry (NULL if translation failed).
 */
static void dynrec_reset_if_enabled(void);

static block_entry_t *translate(const uint8_t *bytes, int n,
                                int *out_result) {
    int result;
    /* Fill past the test bytes with RET, not 0xFF. 0xFF is an unsupported
     * opcode, so translate_block() bailed out with -1 on every case that is
     * not itself a terminator (the nop chain, push/pop, MOV AX,imm16, ...),
     * and those paths were never audited at all. RET stops the block cleanly
     * instead; dump_block() notes when filler bytes were consumed. */
    memset(rom, 0xC3, sizeof(rom));
    memcpy(&rom[TEST_ADDR], bytes, (size_t)n);

    memset(&cpu, 0, sizeof(cpu));
    cpu.segregs[regcs] = TEST_SEG;
    cpu.ip = TEST_OFF;

    dynrec_reset_if_enabled();
    result = translate_block(&cpu, TEST_ADDR, MAX_INSTRUCTIONS);
    if (out_result) *out_result = result;
    if (result <= 0) return NULL;

    /* translate_block always allocates the first free slot; after a reset
     * that is slot 0. */
    if (!block_cache[0].valid) return NULL;
    return &block_cache[0];
}

/* dynrec_reset() early-returns when disabled, so poke the cache directly. */
static void dynrec_reset_if_enabled(void) {
    flush_all_blocks();
}

/* Value of a data-processing immediate: imm8 ROR (rotate_imm * 2). The
 * rotate is what lets dynrec encode "imm8 << 8" on ARMv5TE, which has no
 * MOVW -- see emit_movw(). */
static uint32_t arm_imm_value(uint32_t word) {
    unsigned rot = ((word >> 8) & 0xF) * 2;
    uint32_t v = word & 0xFF;
    if (rot == 0) return v;
    return (v >> rot) | (v << (32 - rot));
}

/* Human-readable rendering of one decoded word, shared by the block dump and
 * the direct emit_bl audit. `w`/`nwords` are needed to resolve literal-pool
 * loads; pass NULL/0 when the block context is not available. */
static void arm_detail(const uint32_t *w, int nwords, uint32_t i,
                       char *buf, size_t buflen) {
    arm_insn_t in;
    uint32_t word = w[i];
    arm_decode(word, i, &in);
    switch (in.kind) {
    case K_MOV_REG:
        snprintf(buf, buflen, "mov r%d, r%d", in.rd, in.rm); break;
    case K_MOV_IMM:
        snprintf(buf, buflen, "mov r%d, #0x%x",
                 in.rd, (unsigned)arm_imm_value(word)); break;
    case K_ORR_IMM:
        snprintf(buf, buflen, "orr r%d, r%d, #0x%x (imm8 ror #%u)",
                 in.rd, in.rn, (unsigned)arm_imm_value(word),
                 ((word >> 8) & 0xF) * 2); break;
    case K_LDR_LIT:
        snprintf(buf, buflen, "ldr r%d, [pc, #%u]  -> word %u = 0x%08x",
                 in.rd, (unsigned)(word & 0xFFF), in.lit_addr,
                 (unsigned)((w && (int)in.lit_addr < nwords) ? w[in.lit_addr] : 0));
        break;
    case K_PUSH:
        snprintf(buf, buflen, "push {r4,r10,r11,lr} (0x%04x)", in.reglist); break;
    case K_POP:
        snprintf(buf, buflen, "pop  {r4,r10,r11,pc} (0x%04x)", in.reglist); break;
    case K_B:
        snprintf(buf, buflen, "b    word %u", in.branch_target); break;
    case K_BL: {
        int32_t rel = (int32_t)(word & 0x00FFFFFF);
        if (rel & 0x800000) rel |= 0xFF000000;
        snprintf(buf, buflen, "bl   0x%08lx (offset %d words)",
                 (unsigned long)((int64_t)(uint32_t)code_cache_base
                                 + (int32_t)i * 4 + 8 + (int64_t)rel * 4), rel);
        break;
    }
    case K_BLX_REG:
        /* The "[LR = word N]" annotation is the whole point of this decoder:
         * ARM's BLX <Rm> sets LR = PC = address + 8, i.e. two words on. */
        snprintf(buf, buflen, "blx  r%d   [returns to word %u]", in.rm, i + 2);
        break;
    case K_LDR_STR:
        snprintf(buf, buflen, "ldr/str r%d, [r%d]", in.rd, in.rn); break;
    case K_MISC:
        snprintf(buf, buflen, "data-processing 0x%08x", word); break;
    default:
        snprintf(buf, buflen, "undecoded 0x%08x", word); break;
    }
}

/* Maximum block size the audits will walk. dynrec caps a block at
 * MAX_INSTRUCTIONS x86 instructions, so this is comfortably generous. */
#define AUDIT_MAX_WORDS 16384

/* ------------------------------------------------------------------ *
 * Regression check: control flow must never reach a literal-pool word.
 *
 * A "literal-pool word" is any word that a PC-relative LDR loads -- i.e. data
 * embedded among the instructions. The old emit_bl() out-of-range fallback
 * emitted exactly such a word two positions after its BLX, and ARM's
 * `BLX <Rm>` sets LR = PC = address + 8, so the helper's own address was
 * executed as ARM code on every return from every helper call.
 *
 * This is a reachability walk rather than a single pattern match, so it also
 * catches the same class of bug introduced some other way: a pool word reached
 * by fall-through or by a branch.
 * ------------------------------------------------------------------ */
static void audit_control_flow(const char *test, const uint32_t *w, int nwords) {
    static unsigned char pool[AUDIT_MAX_WORDS];
    static unsigned char seen[AUDIT_MAX_WORDS];
    int i, pc;

    if (nwords > AUDIT_MAX_WORDS) nwords = AUDIT_MAX_WORDS;

    memset(pool, 0, sizeof(pool));
    memset(seen, 0, sizeof(seen));

    /* Mark every word that some LDR loads as data. */
    for (i = 0; i < nwords; i++) {
        arm_insn_t in;
        arm_decode(w[i], (uint32_t)i, &in);
        if (in.kind == K_LDR_LIT && (int)in.lit_addr < nwords)
            pool[in.lit_addr] = 1;
    }

    /* Walk forward from the end of the prologue (word 2). */
    for (pc = 2; pc >= 0 && pc < nwords; ) {
        arm_insn_t in;
        arm_decode(w[pc], (uint32_t)pc, &in);

        if (pool[pc])
            fail(test, "control reaches word %d (0x%08x), which is a "
                       "literal-pool entry -- data is being executed as code",
                 pc, (unsigned)w[pc]);

        if (seen[pc] && !in.writes_pc) { pc++; continue; }  /* loop guard */
        seen[pc] = 1;

        if (in.kind == K_POP)
            break;                                        /* left the block */
        if (in.kind == K_B && (int)in.branch_target < nwords)
            pc = (int)in.branch_target;                   /* intra-block jump */
        else
            pc = pc + 1;              /* including BL/BLX, which return here */
    }

/* CORRECT behavior: ARM's BL and BLX set LR = call_address + 4 (i.e. the
     * next word). There is NO B .-4 absorber. The helper returns naturally to
     * the next emitted word (more translated code or the POP epilogue).
     *
     * Verify that calls don't return into a literal pool, and that the return
     * address (call+1) is within the block and not a pool word. */
    for (i = 0; i < nwords; i++) {
        arm_insn_t in;
        int ret = i + 1;          /* LR = call address + 4 bytes = +1 word */
        const char *form;
        arm_decode(w[i], (uint32_t)i, &in);
        if (in.kind == K_BLX_REG)      form = "BLX";
        else if (in.kind == K_BL)      form = "BL";
        else                           continue;

        if (ret >= nwords) {
            fail(test, "%s at word %d returns to word %d, past the end "
                       "of the %d-word block", form, i, ret, nwords);
        } else if (pool[ret]) {
            fail(test, "%s at word %d returns to word %d (0x%08x), which "
                       "is a literal-pool entry -- data is executed as code",
                   form, i, ret, (unsigned)w[ret]);
        }
    }
}

/* ------------------------------------------------------------------ *
 * Direct emit_bl() audit.
 *
 * The block-level checks can only exercise the addresses this harness's own
 * stubs happen to live at, which under -no-pie are all below 0x10000. On the
 * device the helpers are in libxtulator.so at ~0x42000000, so a "fix" that
 * merely truncates the target to 16 bits passes every block test here and
 * still branches to the wrong address on hardware.
 *
 * So call emit_bl() directly with a spread of targets -- full 32-bit ones, and
 * the lo8==0 / hi8==0 degenerate cases emit_movw() branches on -- and verify
 * the emitted words materialise the exact target in R12, and that BOTH paths
 * (indirect BLX and direct BL) satisfy the identical LR-return invariant:
 * the call word, then the `B .-4` absorber, then nothing -- so LR resumes on
 * the caller's next word. The direct path is a separate assertion because the
 * block-level checks cannot reach it reliably (it only fires when mmap lands
 * the code cache within ±32MB of the helpers).
 * ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ *
 * The shared LR-return invariant, applied identically to both emit_bl()
 * paths.
 *
 * ARM's `BL <label>` and `BLX <Rm>` set LR = PC = (address of the call word)
 * + 4 (the next word), with no difference between them. Control resumes at
 * the caller's next word -- which is exactly word call+1. So a conforming
 * sequence is just the call instruction followed immediately by the caller's
 * next word (more translated code or the POP epilogue). No absorber word
 * is needed or wanted.
 */
static void check_lr_returns_to_next_word(const char *t, const uint32_t *w,
                                         int nwords, int call, const char *form) {
    int ret = call + 1;    /* LR = call address + 4 bytes = +1 word */

    /* The call must not be the last word of the sequence -- there must be
     * a caller's next word for LR to return to. */
    expectf(call + 1 < nwords, t,
            "%s at word %d: call is the last word (%d words total), so LR "
            "(word %d) would return past the end -- emit_bl() must not end "
            "with the call", form, call, nwords, ret);

    /* The word after the call is the caller's next word (or POP). It must
     * NOT be a B .-4 absorber (0xEAFFFFFF). */
    expectf(w[call + 1] != 0xEAFFFFFF, t,
            "%s at word %d: the word after the call is 0x%08x (B .-4), "
            "which is the old buggy absorber -- must be the caller's next "
            "word instead", form, call, (unsigned)w[call + 1]);

    /* LR should land exactly on the caller's next word. The total words
     * emitted by emit_bl() should equal call+1 (the call plus the caller's
     * next word that was already there). But since emit_bl() only emits the
     * call itself, nwords == call+1 means emit_bl emitted just the call word.
     * The check here is that emit_bl doesn't emit extra words past the call. */
    expectf(nwords == call + 1, t,
            "%s at word %d: emit_bl() emitted %d words (call at %d), so LR "
            "(word %d) %s the caller's next word",
            form, call, nwords, call, ret,
            nwords == call + 1 ? "exactly" : "does NOT land on");
}

static void test_emit_bl(void) {
    static const uint32_t targets[] = {
        0x00404140UL,   /* 16-bit, like the -no-pie stubs here */
        0x0000FFFFUL,   /* low half all ones */
        0x00000100UL,   /* lo8 == 0  -> emit_movw's single-MOV special case */
        0x00010000UL,   /* lo16 hi byte == 0 -> emit_movw's single-MOV case */
        0x00000123UL,   /* both bytes non-zero -> emit_movw's MOV+ORR case */
        0xFFFF0000UL,   /* high half all ones */
        0xFFFFFFFFUL,
        0x4203330CUL,   /* real IS01 helper address (libxtulator.so) */
        0x00FF00FFUL,   /* both halves' byte pairs distinct */
        0x10000000UL,   /* bits 24-31 only, far out of range */
    };
    const char *t = "emit_bl (direct)";
    size_t k;

    /* Force the out-of-range path regardless of where the suite left the
     * cache: put the cache at the address the IS01 really uses so every
     * target below is far outside BL's ±32MB reach. */
    relocate_code_cache_to(IS01_CACHE_BASE);

    printf("--- %s ---\n", t);

    for (k = 0; k < sizeof(targets) / sizeof(targets[0]); k++) {
        uint32_t tgt = targets[k];
        uint32_t saved = code_cache_pos;
        const uint32_t *w;
        int nwords, i;
        uint32_t r12 = 0;
        int got_blx = 0, blx_reg = -1, blx_index = -1;

        emit_bl(tgt);
        nwords = (int)((code_cache_pos - saved) / 4);
        w = (const uint32_t *)(code_cache_base + saved);

        printf("  target 0x%08x -> %d word(s)\n", (unsigned)tgt, nwords);
        for (i = 0; i < nwords; i++) {
            arm_insn_t in;
            char detail[96];
            arm_decode(w[i], (uint32_t)i, &in);
            arm_detail(w, nwords, (uint32_t)i, detail, sizeof(detail));
            printf("    %-6u %08x   %-10s %s\n", (unsigned)(i * 4), w[i],
                   arm_kind_name(in.kind), detail);
        }

        /* (1) Structure. CORRECT behavior: ARM's `BLX <Rm>` sets LR = address + 4,
         *     i.e. the next word. emit_bl() emits ONLY the call (BLX R12) -- no
         *     absorber. The caller's next word (emitted by the translator, not
         *     by emit_bl) naturally follows. Here we test emit_bl in isolation,
         *     so nwords equals the number of words to materialize target in R12
         *     plus 1 for the BLX itself. The last word MUST be BLX R12. */
        expectf(nwords >= 1 && w[nwords - 1] == 0xE12FFF3C, t,
                "target 0x%08x: last word is 0x%08x, must be BLX R12 (0xE12FFF3C)",
                (unsigned)tgt, (unsigned)w[nwords - 1]);
        expectf(w[nwords - 1] != 0xEAFFFFFF, t,
                "target 0x%08x: last word is B .-4 (0xEAFFFFFF), the old buggy "
                "absorber -- emit_bl() must not emit it", (unsigned)tgt);

        /* (2) No literal pool anywhere: no PC-relative LDR, and no bare word
         *     equal to the target. */
        for (i = 0; i < nwords; i++) {
            arm_insn_t in;
            arm_decode(w[i], (uint32_t)i, &in);
            expectf(in.kind != K_LDR_LIT, t,
                    "target 0x%08x: word %d is a PC-relative literal load -- "
                    "emit_bl() must not need a literal pool",
                    (unsigned)tgt, i);
            expectf(w[i] != tgt, t,
                    "target 0x%08x: word %d (0x%08x) is the raw target address, "
                    "i.e. a literal-pool entry", (unsigned)tgt, i, (unsigned)w[i]);
        }

        /* (3) The words before the BLX must materialise the EXACT target in
         *     R12 -- including the bits above 16, which is what a plain
         *     (uint16_t) truncation of the target would lose. */
        for (i = 0; i < nwords && !got_blx; i++) {
            arm_insn_t in;
            arm_decode(w[i], (uint32_t)i, &in);
            if (in.kind == K_BLX_REG) {
                got_blx = 1; blx_reg = in.rm; blx_index = i;
            } else if (in.kind == K_MOV_IMM && in.rd == 12)
                r12 = arm_imm_value(w[i]);
            else if (in.kind == K_ORR_IMM && in.rd == 12 && in.rn == 12)
                r12 |= arm_imm_value(w[i]);
        }
        expectf(got_blx && blx_reg == 12, t,
                "target 0x%08x: expected a BLX r12 in the sequence", (unsigned)tgt);
        expectf(r12 == tgt, t,
                "target 0x%08x: the emitted words build R12 = 0x%08x",
                (unsigned)tgt, (unsigned)r12);

        /* (4) The R12 materialization is correct. The LR-return invariant is
         *     tested at the block level in dump_block()/audit_control_flow().
         *     Here we only test emit_bl in isolation, which correctly emits just
         *     the call (the caller emits what follows). */

        printf("\n");
    }

    /* The in-range path: a direct `BL` with NO absorber.
     *
     * ARM's `BL` sets LR = address + 4 (the next word), identical to `BLX <Rm>`.
     * emit_bl() emits only the BL instruction. The caller's next word naturally
     * follows. In this isolated test, nwords should be exactly 1. */
    {
        uint32_t saved = code_cache_pos;
        uint32_t near_tgt = (uint32_t)code_cache_base + 0x1000;
        const uint32_t *w;
        int nwords, i, bl_index = -1;
        int32_t rel;
        uintptr_t got_tgt;

        emit_bl(near_tgt);
        nwords = (int)((code_cache_pos - saved) / 4);
        w = (const uint32_t *)(code_cache_base + saved);

        printf("  in-range target 0x%08x -> %d word(s)\n",
               (unsigned)near_tgt, nwords);
        for (i = 0; i < nwords; i++) {
            arm_insn_t in;
            char detail[96];
            arm_decode(w[i], (uint32_t)i, &in);
            arm_detail(w, nwords, (uint32_t)i, detail, sizeof(detail));
            printf("    %-6u %08x   %-10s %s\n", (unsigned)(i * 4), w[i],
                   arm_kind_name(in.kind), detail);
        }

        expectf(nwords == 1, t,
                "in-range target 0x%08x emitted %d words, expected 1 "
                "(BL only, no absorber)", (unsigned)near_tgt, nwords);
        for (i = 0; i < nwords; i++) {
            arm_insn_t in;
            arm_decode(w[i], (uint32_t)i, &in);
            if (in.kind == K_BL) bl_index = i;
        }
        expectf(bl_index == 0, t,
                "in-range target 0x%08x: BL is at word %d, expected word 0 "
                "(nothing may precede a direct BL)", (unsigned)near_tgt, bl_index);

        /* The offset is measured from the address of the BL word itself
         * (+8, the pipeline bias). Recover the absolute target from the
         * encoded 24-bit signed field and compare. */
        if (bl_index == 0) {
            rel = (int32_t)(w[0] & 0x00FFFFFF);
            if (rel & 0x800000) rel |= (int32_t)0xFF000000;
            got_tgt = (uintptr_t)((int64_t)(uint32_t)code_cache_base + saved
                                  + 8 + (int64_t)rel * 4);
            expectf(got_tgt == (uintptr_t)near_tgt, t,
                    "in-range target 0x%08x: the encoded BL offset resolves to "
                    "0x%08lx -- the offset must still be computed from the "
                    "address of the BL word",
                    (unsigned)near_tgt, (unsigned long)got_tgt);
            expectf((w[0] & 0xFF000000) == 0xEB000000, t,
                    "in-range target 0x%08x: call word 0x%08x, expected an "
                    "unconditional BL (0xEB000000 | offset)",
                    (unsigned)near_tgt, (unsigned)w[0]);
        }

        /* LR-return invariant is tested at the block level in dump_block()/
         * audit_control_flow(). Here we only test emit_bl in isolation. */

        printf("\n");
    }
}

static void dump_block(const char *test, const uint8_t *x86, int nx86,
                       const block_entry_t *b) {
    const uint32_t *w = (const uint32_t *)(code_cache_base + b->arm_offset);
    int nwords = (int)(b->code_len / 4);
    int i, j;

    printf("--- %s ---\n", test);
    printf("  x86:");
    for (i = 0; i < nx86; i++) printf(" %02x", x86[i]);
    printf("\n  x86_pc=0x%05X x86_len=%u (bytes) instr_cnt=%u (instrs) "
           "code_len=%u (%d words)\n",
           (unsigned)b->x86_pc, b->x86_len, b->instr_count, b->code_len, nwords);
    printf("  x86_end_ip=0x%04X (expected 0x%04X if x86_len bytes are linear)\n",
           b->x86_end_ip, (unsigned)(TEST_OFF + b->x86_len));
    if ((int)b->x86_len > nx86)
        printf("  NOTE: %u filler byte(s) past the %d test byte(s) were also "
               "consumed (terminator C3)\n", b->x86_len - (unsigned)nx86, nx86);

    /* x86_len is a BYTE count and instr_count an INSTRUCTION count; they were
     * the same field until this suite caught them being printed as one. Every
     * x86 instruction is at least one byte, so instr_count <= x86_len is an
     * invariant, and a block that is nothing but instructions has at least
     * one. A block of 1-byte NOPs is the only shape where the two are equal;
     * print the ratio so a byte/instruction conflation is visible at a glance. */
    expect(b->instr_count >= 1, test,
           "instr_count is 0 for a cached block");
    expectf(b->instr_count <= b->x86_len, test,
            "instr_count=%u exceeds x86_len=%u -- an instruction is at least "
            "one byte, so these are not the same quantity",
            b->instr_count, b->x86_len);
    expectf(b->x86_end_ip == (uint16_t)(TEST_OFF + b->x86_len), test,
            "x86_end_ip=0x%04X does not follow from x86_pc+x86_len (0x%04X)",
            b->x86_end_ip, (unsigned)(TEST_OFF + b->x86_len));
    if (b->instr_count != b->x86_len)
        printf("  note: %u byte(s) over %u instruction(s) -- byte length and "
               "instruction count differ, as they must for any block holding a "
               "multi-byte instruction\n",
               b->x86_len - b->instr_count, b->instr_count);

    printf("  %-6s %-10s %-10s %s\n", "off", "word", "kind", "decoded");
    for (i = 0; i < nwords; i++) {
        arm_insn_t in;
        arm_decode(w[i], (uint32_t)i, &in);
        char detail[96];
        arm_detail(w, nwords, (uint32_t)i, detail, sizeof(detail));
        printf("  %-6u %08x   %-10s %s\n", (unsigned)(i * 4), w[i],
               arm_kind_name(in.kind), detail);
    }

    /* ---- invariants ---- */
    printf("  checks:\n");

    /* 1. Frame integrity. */
    {
        arm_insn_t in;
        arm_decode(w[0], 0, &in);
        expect(in.kind == K_PUSH, test, "block does not start with PUSH");
        arm_decode(w[nwords - 1], (uint32_t)(nwords - 1), &in);
        expect(in.kind == K_POP, test, "block does not end with POP");
    }

    /* 2. R4 discipline. R4 holds the CPU_t*; the prologue is the only
     *    instruction allowed to define it, and the epilogue may restore it.
     *    Anything else means a helper call receives a garbage pointer --
     *    exactly the crash under investigation. */
    for (j = 2; j < nwords - 1; j++) {
        arm_insn_t in;
        uint32_t defs;
        arm_decode(w[j], (uint32_t)j, &in);
        defs = armdefs(&in);
        if (defs & (1u << 4))
            fail(test, "instruction at word %d (%s) redefines R4 -- "
                       "the CPU_t* register is clobbered", j, arm_kind_name(in.kind));
    }
    {
        arm_insn_t in;
        arm_decode(w[1], 1, &in);
        expect(in.kind == K_MOV_REG && in.rd == 4 && in.rm == 0, test,
               "prologue word 1 is not 'mov r4, r0'");
    }

    /* 3. SP must only move via push/pop. */
    for (j = 1; j < nwords - 1; j++) {
        arm_insn_t in;
        arm_decode(w[j], (uint32_t)j, &in);
        if (in.kind == K_LDR_STR && (in.rn == 13 || in.rd == 13))
            fail(test, "instruction at word %d touches SP outside push/pop", j);
        if (in.kind == K_MISC && (in.rn == 13 || in.rd == 13))
            fail(test, "instruction at word %d touches SP outside push/pop", j);
    }

    /* 4. No branch may land on a literal-pool word. */
    for (i = 0; i < nwords; i++) {
        arm_insn_t ld;
        uint32_t pool_off;
        arm_decode(w[i], (uint32_t)i, &ld);
        if (ld.kind != K_LDR_LIT) continue;
        pool_off = ld.lit_addr;
        if (pool_off >= (uint32_t)nwords) {
            fail(test, "literal load at word %d points outside the block "
                       "(word %u, block is %d words)", i, pool_off, nwords);
            continue;
        }
        /* Only the B immediately preceding the pool may target past it. */
        for (j = 0; j < (int)pool_off; j++) {
            arm_insn_t br;
            arm_decode(w[j], (uint32_t)j, &br);
            if (br.kind != K_B) continue;
            if (br.branch_target == pool_off)
                fail(test, "branch at word %d lands on literal pool at word %u",
                     j, pool_off);
        }
    }

    /* 5. Every BL out of the block must target a real helper. The absolute
     *    target is recovered from the encoded 24-bit signed offset; this is
     *    only meaningful because the harness forces both the code cache and
     *    the helper stubs into the low 4GB (see relocate_code_cache). */
    for (j = 0; j < nwords; j++) {
        arm_insn_t in;
        uintptr_t tgt;
        int32_t rel;
        arm_decode(w[j], (uint32_t)j, &in);
        if (in.kind != K_BL) continue;
        /* the 24-bit field is a signed word offset */
        rel = (int32_t)(w[j] & 0x00FFFFFF);
        if (rel & 0x800000) rel |= 0xFF000000;     /* sign-extend */
        tgt = (uintptr_t)((int64_t)(uint32_t)code_cache_base + (j * 4) + 8
              + (int64_t)rel * 4);
        if (!is_known_target(tgt))
            fail(test, "BL at word %d targets 0x%08lx, not a known helper",
                 j, (unsigned long)tgt);
    }

    /* 6. A BL/BLX call must be preceded by a load of R0 from R4 in the same
     *    helper-call group, otherwise the cpu pointer is not what is passed. */
    {
        int i2;
        for (i2 = 1; i2 < nwords - 1; i2++) {
            arm_insn_t in;
            arm_decode(w[i2], (uint32_t)i2, &in);
            if (in.kind != K_BL && in.kind != K_BLX_REG) continue;
            if (in.kind == K_BLX_REG) continue; /* R0 set by the literal pool path */
            /* walk back over any immediate setup to find 'mov r0, r4' */
            {
                int k, ok = 0;
                for (k = i2 - 1; k >= 1 && k >= i2 - 4; k--) {
                    arm_insn_t p;
                    arm_decode(w[k], (uint32_t)k, &p);
                    if (p.kind == K_MOV_REG && p.rd == 0 && p.rm == 4) { ok = 1; break; }
                    if (p.kind == K_LDR_LIT) break; /* indirect path supplies R0 */
                }
                if (!ok)
                    fail(test, "call at word %d has no 'mov r0, r4' setup", i2);
            }
        }
    }

    /* 7. No control path may reach a literal-pool word.
     *
     *    ARM's `BLX <Rm>` sets LR = PC = (address of the BLX) + 8, i.e. word
     *    (blx_index + 2). That is the invariant the old emit_bl() out-of-range
     *    fallback broke: it emitted
     *        LDR R12,[PC,#4] ; BLX R12 ; B 0 ; .word target
     *    so LR pointed *at the .word* and the helper's address was executed
     *    as ARM code before falling through into the POP.
     *
     *    Two assertions, because they catch different mistakes:
     *      (a) a reachability walk, so ANY route to a pool word is caught --
     *          fall-through, a branch, or the call's return address;
     *      (b) the call-word +8 rule stated directly for BOTH `BLX <Rm>` and
     *          `BL <label>` -- the immediate form has the identical +8 bias --
     *          which is the form the bug actually took (the walk alone can
     *          route around it via the following `B`). */
    audit_control_flow(test, w, nwords);

    /* 8. An indirect (BLX) call must branch to a real helper. R12 is
     *    materialised by the instruction stream just before the BLX, so this
     *    replays just R12's writes and checks the resulting address -- it
     *    catches both "calls the wrong address" and "R12 still holds whatever
     *    the previous call left there". */
    {
        uint32_t r12 = 0;
        int i8;
        for (i8 = 0; i8 < nwords; i8++) {
            arm_insn_t in;
            arm_decode(w[i8], (uint32_t)i8, &in);
            if (in.kind == K_MOV_IMM && in.rd == 12)
                r12 = arm_imm_value(w[i8]);
            else if (in.kind == K_ORR_IMM && in.rd == 12 && in.rn == 12)
                r12 |= arm_imm_value(w[i8]);
            else if (in.kind == K_BLX_REG && in.rm == 12)
                expectf(is_known_target((uintptr_t)r12), test,
                        "BLX r12 at word %d branches to 0x%08lx, not a known "
                        "helper", i8, (unsigned long)r12);
        }
    }
    printf("\n");
}

/* ------------------------------------------------------------------ *
 * Direct audit of x86_instr_len() itself.
 *
 * The footprint test can only reach the opcodes the translator actually
 * defers, and a wrong length for anything else would go unnoticed. Every
 * expected value below was produced by assembling the instruction with
 * `nasm -f bin` under BITS 16 and counting the bytes -- NOT by disassembling,
 * because objdump -m i8086 and ndisasm -b 16 both mis-decode 16-bit ModR/M
 * (they read rm=100 as a SIB; in 16-bit addressing it is [SI] and no SIB
 * byte exists). A decoder written from their output is wrong in both
 * directions: `8B 05` is 2 bytes ([DI]), not 4.
 *
 * The contract is "never under-estimate", so a few entries deliberately check
 * an over-estimate, marked OVER.
 * ------------------------------------------------------------------ */
static void test_x86_instr_len(void) {
    static const struct {
        const char *desc;
        uint8_t bytes[10];
        int n;
        unsigned len;
        int over;              /* 1: at least this long is correct */
    } cases[] = {
        /* --- the four cases the byte-length work must cover --- */
        { "8E C0            MOV ES,AL (register ModR/M)", { 0x8E, 0xC0 }, 2, 2, 0 },
        { "83 C2 03         ADD DX,3 (ModR/M + imm8)", { 0x83, 0xC2, 0x03 }, 3, 3, 0 },
        { "0F 84 00 00      JZ +0 (two-byte opcode + rel16)", { 0x0F, 0x84, 0x00, 0x00 }, 4, 4, 0 },
        { "66 8B 07         MOV AX,[BX] (prefix + ModR/M)", { 0x66, 0x8B, 0x07 }, 3, 3, 0 },
        /* --- 16-bit addressing: rm=100 is [SI], there is no SIB byte --- */
        { "8B 05            MOV AX,[DI]", { 0x8B, 0x05 }, 2, 2, 0 },
        { "8B 84 88 00      MOV AX,[SI+88h] (mod=10 -> disp16)", { 0x8B, 0x84, 0x88, 0x00 }, 4, 4, 0 },
        { "8B 46 F8         MOV AX,[BP-8] (mod=01 -> disp8)", { 0x8B, 0x46, 0xF8 }, 3, 3, 0 },
        { "8B 86 34 12      MOV AX,[BP+1234h] (mod=00 rm=110 -> disp16)",
          { 0x8B, 0x86, 0x34, 0x12 }, 4, 4, 0 },
        /* --- 0x67 selects 32-bit addressing, and only then does SIB exist --- */
        { "67 8B 44 24 08   MOV AX,[ESP+8] (0x67 + SIB + disp8)",
          { 0x67, 0x8B, 0x44, 0x24, 0x08 }, 5, 5, 0 },
        { "66 67 8B 85 00 01 00 00 MOV EAX,[EBP+100h] (disp32)",
          { 0x66, 0x67, 0x8B, 0x85, 0x00, 0x01, 0x00, 0x00 }, 8, 8, 0 },
        /* --- 0x66 widens OPERAND size, so immediates widen with it --- */
        { "66 05 34 12 00 00 ADD EAX,1234h", { 0x66, 0x05, 0x34, 0x12, 0x00, 0x00 }, 6, 6, 0 },
        { "05 34 12         ADD AX,1234h", { 0x05, 0x34, 0x12 }, 3, 3, 0 },
        /* --- the ModR/M reg field selects an immediate (F6/F7 TEST) --- */
        { "F6 46 11 22      TEST [BP+11h],22h (memory form: disp8 + imm8)",
          { 0xF6, 0x46, 0x11, 0x22 }, 4, 4, 0 },
        { "F6 05 22         TEST [DI],22h", { 0xF6, 0x05, 0x22 }, 3, 3, 0 },
        { "F6 C8 22         TEST AL,22h", { 0xF6, 0xC8, 0x22 }, 3, 3, 0 },
        { "F7 46 11 22 33 44 TEST [BP+11h],1234h (disp8 + imm16)",
          { 0xF7, 0x46, 0x11, 0x22, 0x33, 0x44 }, 6, 5, 0 },
        { "F6 0E 22         TEST [BP],22h (mod=00 rm=110 -> disp16 + imm8)",
          { 0xF6, 0x0E, 0x00, 0x00, 0x22 }, 5, 5, 0 },
        /* --- opcode groups the translator also defers --- */
        { "F3 A4            REP MOVSB (prefix + string op)", { 0xF3, 0xA4 }, 2, 2, 0 },
        { "A6               CMPSB", { 0xA6 }, 1, 1, 0 },
        { "66 89 06 11 22   MOV [BP+2211h],AX", { 0x66, 0x89, 0x06, 0x11, 0x22 }, 5, 5, 0 },
        { "8F 45 F8         POP [BP-8]", { 0x8F, 0x45, 0xF8 }, 3, 3, 0 },
        { "80 3E 00 00 7F   CMP BYTE [BP+0],7Fh (mod=00 rm=110 -> disp16 + imm8)",
          { 0x80, 0x3E, 0x00, 0x00, 0x7F }, 5, 5, 0 },
        { "81 BE 34 12 78 56 CMP WORD [1234h],5678h",
          { 0x81, 0xBE, 0x34, 0x12, 0x78, 0x56 }, 6, 6, 0 },
        { "0F 1F 00         NOP WORD [BX+SI]", { 0x0F, 0x1F, 0x00 }, 3, 3, 0 },
        { "0F 0E           3DNow! opcode with no operand", { 0x0F, 0x0E }, 2, 2, 0 },
        /* --- 386/486 opcodes the translator does not defer, but a wrong
         *     length here would still shrink some future block's footprint --- */
        { "69 C3 34 12      IMUL AX,BX,1234h", { 0x69, 0xC3, 0x34, 0x12 }, 4, 4, 0 },
        { "6B C3 03         IMUL AX,BX,3", { 0x6B, 0xC3, 0x03 }, 3, 3, 0 },
        { "C4 06 34 12      LES AX,[1234h]", { 0xC4, 0x06, 0x34, 0x12 }, 4, 4, 0 },
        { "9A 34 12 78 56   CALL FAR 1234h:5678h (inline seg:off)", { 0x9A, 0x34, 0x12, 0x78, 0x56 }, 5, 5, 0 },
        { "EA 34 12 78 56   JMP FAR 1234h:5678h (inline seg:off)", { 0xEA, 0x34, 0x12, 0x78, 0x56 }, 5, 5, 0 },
        { "C5 06 34 12      LDS AX,[1234h] (far pointer comes from MEMORY, not inline)",
          { 0xC5, 0x06, 0x34, 0x12 }, 4, 4, 0 },
        { "C2 34 12         RET 1234h", { 0xC2, 0x34, 0x12 }, 3, 3, 0 },
        { "C8 34 12 00      ENTER 1234h,0", { 0xC8, 0x34, 0x12, 0x00 }, 4, 4, 0 },
        { "C6 06 34 12 56   MOV BYTE [1234h],56h", { 0xC6, 0x06, 0x34, 0x12, 0x56 }, 5, 5, 0 },
        { "D4 0A            AAM 0Ah", { 0xD4, 0x0A }, 2, 2, 0 },
        { "E8 34 12         CALL 1234h", { 0xE8, 0x34, 0x12 }, 3, 3, 0 },
        /* --- over-estimates are allowed, and required for far pointers with
         *     0x66 (ptr32:16 = 6 operand bytes, not 4) --- */
        { "66 9A 00 00 34 12 00 00 CALL FAR (over-estimate OK)",
          { 0x66, 0x9A, 0x00, 0x00, 0x34, 0x12, 0x00, 0x00 }, 8, 7, 1 },
        /* --- truncated at the top of the address space: never runs off --- */
    };
    const char *t = "x86_instr_len";
    size_t c;

    printf("--- %s (%d encodings) ---\n", t, (int)(sizeof(cases) / sizeof(cases[0])));
    for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        /* The window handed to the decoder is a 32-byte 0x90 pad, so what is
         * measured is the decoder and not the `avail` clamp. The clamp has its
         * own two cases below. */
        uint8_t buf[32];
        uint32_t got;
        memset(buf, 0x90, sizeof(buf));
        memcpy(buf, cases[c].bytes, (size_t)cases[c].n);
        got = x86_instr_len(buf, (uint32_t)sizeof(buf));
        if (cases[c].over)
            expectf(got >= cases[c].len, t,
                    "%s: got %u, must be at least %u", cases[c].desc, got,
                    cases[c].len);
        else
            expectf(got == cases[c].len, t,
                    "%s: got %u, expected exactly %u  [%02x%02x%02x...]",
                    cases[c].desc, got, cases[c].len, cases[c].bytes[0],
                    cases[c].n > 1 ? cases[c].bytes[1] : 0,
                    cases[c].n > 2 ? cases[c].bytes[2] : 0);
    }

    /*
     * The `avail` clamp, separately: an encoding that runs off the end of the
     * readable window must return the window size, never more -- the
     * translator sizes that window as the distance to the top of the 1MB
     * address space, and reading past it would walk off the mapped page.
     */
    {
        static const uint8_t trunc1[] = { 0x8B };
        static const uint8_t trunc2[] = { 0x0F, 0x84 };
        static const uint8_t trunc3[] = { 0x8B, 0x86, 0x34 };
        expectf(x86_instr_len(trunc1, 1) == 1, t,
                "0xEB with avail=1: got %u, must clamp to 1",
                x86_instr_len(trunc1, 1));
        expectf(x86_instr_len(trunc2, 2) == 2, t,
                "0F 84 with avail=2: got %u, must clamp to 2",
                x86_instr_len(trunc2, 2));
        expectf(x86_instr_len(trunc3, 3) == 3, t,
                "8B 86 34 with avail=3 (disp16 cut off): got %u, must clamp to 3",
                x86_instr_len(trunc3, 3));
    }
    printf("\n");
}

static block_entry_t *run_case(const char *name, const uint8_t *bytes, int n) {
    int result = 0;
    block_entry_t *b = translate(bytes, n, &result);
    if (!b) {
        printf("--- %s ---\n  translate_block returned %d (not cached)\n\n",
               name, result);
        checks++;
        return NULL;
    }
    dump_block(name, bytes, n, b);
    return b;
}

/* ------------------------------------------------------------------ *
 * Regression check: a deferred instruction must be charged its true byte
 * length.
 *
 * The block hands the instruction to dinstr_exec_one() and ends, but
 * block->x86_len -- the block's invalidation footprint -- used to be
 * incremented by 1 no matter how long the instruction really was. The
 * ModR/M, SIB, displacement and immediate bytes then sat OUTSIDE the
 * footprint, so a write to any of them (a code load, a patch, SMC) left the
 * stale block in the cache to be executed again.
 *
 * The assertion is "at least the true length", because over-charging is
 * safe (a spurious retranslation) while under-charging is the bug. The
 * companion test_invalidation_footprint() pins the other side: a write just
 * past the last byte must NOT invalidate, so over-charging cannot quietly
 * turn into a cache that invalidates everything.
 * ------------------------------------------------------------------ */
static void run_case_true_len(const char *name, const uint8_t *bytes, int n,
                              unsigned true_len) {
    block_entry_t *b = run_case(name, bytes, n);
    expectf(b != NULL, name, "block was not cached, so no length to check");
    if (!b) return;
    expectf(b->x86_len >= true_len, name,
            "recorded byte length x86_len=%u is less than the instruction's "
            "true encoding length %u -- the remaining encoding bytes fall "
            "outside the block and a write to them would not invalidate it",
            b->x86_len, true_len);
}

/* ------------------------------------------------------------------ *
 * The same bug, one level up: the invalidation footprint itself.
 *
 * For each deferred encoding, translate it, then simulate cpu_write()'s
 * dynrec_invalidate_range(x, 1) for every byte the instruction occupies and
 * require the block to die each time. A 1-byte charge (the old behaviour)
 * only ever invalidates on a write to the opcode byte itself, so every one
 * of the interior offsets below is a genuine regression detector.
 * ------------------------------------------------------------------ */
static void test_invalidation_footprint(void) {
    static const struct {
        const char *desc;
        uint8_t bytes[8];
        int n;
        unsigned true_len;
    } cases[] = {
        /* Every encoding below was checked against nasm (`BITS 16`), which is
         * the authority here: both objdump -m i8086 and ndisasm -b 16 get
         * 16-bit ModR/M wrong (they read rm=100 as a SIB, which does not exist
         * in 16-bit addressing -- it is [SI]), so a decoder written from
         * their output is wrong in both directions. */
        { "8E C0      MOV ES,AL        (2 bytes, register ModR/M)",
          { 0x8E, 0xC0 }, 2, 2 },
        { "8B 86 34 12 MOV AX,[BP+1234h] (4 bytes, disp16)",
          { 0x8B, 0x86, 0x34, 0x12 }, 4, 4 },
        { "8B 46 F8   MOV AX,[BP-8]    (3 bytes, disp8)",
          { 0x8B, 0x46, 0xF8 }, 3, 3 },
        { "67 8B 44 24 08 MOV AX,[ESP+8] (5 bytes, 0x67 -> 32-bit addr, SIB + disp8)",
          { 0x67, 0x8B, 0x44, 0x24, 0x08 }, 5, 5 },
        { "66 67 8B 85 00 01 00 00 MOV EAX,[EBP+100h] (8 bytes, disp32)",
          { 0x66, 0x67, 0x8B, 0x85, 0x00, 0x01, 0x00, 0x00 }, 8, 8 },
        { "83 C2 03   ADD DX,3         (3 bytes, ModR/M + imm8)",
          { 0x83, 0xC2, 0x03 }, 3, 3 },
        { "0F 84 00 00 JZ +0          (4 bytes, two-byte opcode + rel16)",
          { 0x0F, 0x84, 0x00, 0x00 }, 4, 4 },
        /* `66 89 06` is 5 bytes, not 3: with mod=00 rm=110 in 16-bit
         * addressing [BP] takes a disp16, so the two operand bytes follow. The
         * 3-byte prefixed ModR/M form needs an rm that needs no displacement,
         * hence `66 8B 07` below. */
        { "66 89 06 11 22 MOV [BP+2211h],AX (5 bytes, prefix + ModR/M + disp16)",
          { 0x66, 0x89, 0x06, 0x11, 0x22 }, 5, 5 },
        { "66 8B 07   MOV AX,[BX]      (3 bytes, prefix + ModR/M, no disp)",
          { 0x66, 0x8B, 0x07 }, 3, 3 },
    };
    const char *t = "invalidation footprint";
    size_t c;
    unsigned k;

    printf("--- %s ---\n", t);
    for (c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        block_entry_t *b;
        unsigned len;
        int before = failures;

        /* (a) every byte of the encoding must invalidate */
        for (k = 0; k < cases[c].true_len; k++) {
            dynrec_reset_if_enabled();
            b = translate(cases[c].bytes, cases[c].n, NULL);
            if (!b) {
                expectf(0, t, "%s: block not cached", cases[c].desc);
                continue;
            }
            len = b->x86_len;
            dynrec_invalidate_range(TEST_ADDR + k, 1);
            expectf(!block_cache[0].valid, t,
                    "%s: a 1-byte write to offset +%u of the instruction did "
                    "NOT invalidate the block (x86_len=%u, so the footprint is "
                    "[0x%05X,0x%05X) and that byte is outside it)",
                    cases[c].desc, k, len, TEST_ADDR, TEST_ADDR + len);
        }

        /* (b) and the first byte past it must not, or the "fix" is just an
         *     over-wide footprint that invalidates the world */
        dynrec_reset_if_enabled();
        b = translate(cases[c].bytes, cases[c].n, NULL);
        if (!b) {
            expectf(0, t, "%s: block not cached", cases[c].desc);
            continue;
        }
        len = b->x86_len;
        dynrec_invalidate_range(TEST_ADDR + len, 1);
        expectf(block_cache[0].valid, t,
                "%s: a write to the first byte PAST the block (0x%05X) "
                "invalidated it -- the footprint x86_len=%u is wider than the "
                "%u bytes actually decoded",
                cases[c].desc, TEST_ADDR + len, len, cases[c].true_len);

        printf("  %-4s %-58s x86_len=%u, writes +0..+%u invalidate, +%u does not\n",
               failures == before ? "ok" : "FAIL", cases[c].desc, len,
               cases[c].true_len - 1, cases[c].true_len);
    }
    printf("\n");
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * dynrec.c addresses the code cache and every helper through plain uint32_t
 * (see emit_bl / patch_branch: `(uint32_t)code_cache_base`, `(uint32_t)&dinstr_x`).
 * On the 32-bit IS01 that is exact. On an x86-64 host those casts truncate,
 * which would silently produce meaningless BL offsets and make check #5
 * meaningless. So: build the harness -no-pie (helpers land near 0x00400000)
 * and MAP_FIXED the code cache at HARNESS_CACHE_BASE, both comfortably inside
 * 4GB and within BL's +/-32MB range, so the truncated arithmetic reproduces
 * exactly what the device computes.
 */
#define HARNESS_CACHE_BASE 0x00800000UL
/* Where the IS01 actually puts the code cache (mmap'd here, observed). The
 * out-of-range emit_bl() path is only live when the cache and the helpers are
 * more than ±32MB apart, so the built-in suite re-runs at this address to
 * guarantee the fallback is exercised rather than silently skipped. */
#define IS01_CACHE_BASE   0x2D5B3000UL

static void relocate_code_cache_to(unsigned long want) {
    void *p;
    if (!code_cache_base) {
        printf("dynrec_enable() failed to map a code cache\n");
        return;
    }
    munmap(code_cache_base, CODE_CACHE_SIZE);
    p = mmap((void *)want, CODE_CACHE_SIZE,
             PROT_READ | PROT_WRITE | PROT_EXEC,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) {
        printf("relocate_code_cache: mmap at 0x%lx failed; BL offsets will be "
               "meaningless\n", want);
        return;
    }
    {
        unsigned long h = (unsigned long)(uintptr_t)&dinstr_nop;
        unsigned long d = (h > want) ? h - want : want - h;
        printf("code cache relocated to %p (helpers at ~0x%lx, %lu KiB away; %s "
               "BL range)\n", p, h, d / 1024,
               d < 32UL * 1024 * 1024 ? "within" : "OUTSIDE");
    }
    code_cache_base = (uint8_t *)p;
    code_cache_pos = 0;
    flush_all_blocks();
}

static void relocate_code_cache(void) {
    const char *env = getenv("DYNRECTEST_CACHE_BASE");
    relocate_code_cache_to(env ? strtoul(env, NULL, 0) : HARNESS_CACHE_BASE);
}

int main(int argc, char **argv) {
    /* Map 1MB of direct RAM so translate_block's MMIO check passes. */
    int i;
    for (i = 0; i < MEMORY_RANGE; i++)
        memory_mapRead[i] = memory_mapWrite[i] = &rom[i];

    dynrec_init();
    dynrec_enable();
    relocate_code_cache();
    build_target_table();

    printf("code cache: %p .. %p\n\n",
           (void *)code_cache_base, (void *)(code_cache_base + CODE_CACHE_SIZE));

    if (argc > 1) {
        /* ad-hoc: hex string of x86 bytes */
        static uint8_t buf[64];
        int n = 0;
        const char *s = argv[1];
        while (*s && n < (int)sizeof(buf)) {
            int hi, lo;
            while (*s == ' ' || *s == ',') s++;
            if (!*s) break;
            hi = hexval(*s++);
            if (!*s || (lo = hexval(*s++)) < 0) {
                printf("bad hex: %s\n", argv[1]);
                return 2;
            }
            buf[n++] = (uint8_t)((hi << 4) | lo);
        }
        run_case("adhoc", buf, n);
    } else {
        /*
         * The sequences below cover the opcodes the IS01 crash log actually
         * touched: the deferred-to-interpreter group (which emits a single
         * exec_one call and no explicit IP advance), the pure-native 1-byte
         * group, and the control-flow terminators.
         */
        static const uint8_t nop_chain[]  = { 0x90, 0x90, 0x90, 0x90, 0x90 };
        /* The long block: 10 NOPs come to 55 ARM words = 220 bytes for 11 x86
         * instructions (2 prologue + 10 helper-call sites x2 words + 10
         * advance_ip flushes x3 words + 2 for the RET site + 1 POP = 55). It is
         * the case that shows the three lengths apart: 11 x86 bytes, 11 x86
         * instructions, 220 ARM bytes. instr_cnt is bounded by x86_len, not by
         * the ARM code size -- it used to be printed as x86_len's value, so all
         * three read the same. */
        static const uint8_t nop_chain10[] = { 0x90, 0x90, 0x90, 0x90, 0x90,
                                               0x90, 0x90, 0x90, 0x90, 0x90 };
        static const uint8_t push_pop[]   = { 0x50, 0x58, 0x50, 0x58 };
        static const uint8_t deferred[]   = { 0x8E };      /* MOV Sw,Ev -> exec_one */
        static const uint8_t pop_ev[]     = { 0x8F };      /* POP Ev      -> exec_one */
        static const uint8_t test_gv[]    = { 0x85 };      /* TEST Gv,Ev  -> exec_one */
        static const uint8_t two_byte[]   = { 0x0F, 0x84, 0x00, 0x00 };
        static const uint8_t rep_prefix[] = { 0xF3, 0xA4 };/* REP MOVSB */
        static const uint8_t opsize[]     = { 0x66, 0x89, 0x06 };
        static const uint8_t hlt_seq[]    = { 0x90, 0xF4 };
        static const uint8_t jmp_rel8[]   = { 0xEB, 0x10 };
        static const uint8_t call_rel16[] = { 0xE8, 0x34, 0x12 };
        static const uint8_t ret_near[]   = { 0x90, 0xC3 };
        static const uint8_t int_imm[]    = { 0xCD, 0x21 };
        static const uint8_t mov_ax_imm[] = { 0xB8, 0x40, 0x00, 0x90 };
        static const uint8_t group1[]     = { 0x83, 0xC2, 0x03, 0x90 };
        static const uint8_t test_ax[]    = { 0xA9, 0x34, 0x12, 0x90 };
        static const uint8_t long_run[]   = {
            0x50, 0x1E, 0xB8, 0x40, 0x00, 0x8E, 0xD8, 0x80, 0x3E,
            0x40, 0x00, 0x00, 0x74, 0x13, 0xFE, 0x0E, 0x40, 0x00
        };
        /* Deferred-to-interpreter encodings of 2, 3 and 4 bytes, plus a
         * prefixed one: the minimum true lengths asserted below. Verified
         * against nasm - see test_invalidation_footprint()'s case list. */
        static const uint8_t def_2byte[]   = { 0x8E, 0xC0 };
        static const uint8_t def_3byte[]   = { 0x83, 0xC2, 0x03 };
        static const uint8_t def_4byte[]   = { 0x0F, 0x84, 0x00, 0x00 };
        static const uint8_t def_prefixed[] = { 0x66, 0x8B, 0x07 };

        printf("=== builtin suite ===\n\n");
        run_case("nop chain (5x 90)", nop_chain, sizeof(nop_chain));
        run_case("nop chain (10x 90) -- 220-byte ARM block, 11 x86 instrs",
                 nop_chain10, sizeof(nop_chain10));
        run_case("push/pop r16", push_pop, sizeof(push_pop));
        run_case("deferred: 8E MOV Sw,Ev", deferred, sizeof(deferred));
        run_case("deferred: 8F POP Ev", pop_ev, sizeof(pop_ev));
        run_case("deferred: 85 TEST Gv,Ev", test_gv, sizeof(test_gv));
        run_case("two-byte 0F 84", two_byte, sizeof(two_byte));
        run_case("REP prefix F3 A4", rep_prefix, sizeof(rep_prefix));
        run_case("opsize prefix 66 89 06", opsize, sizeof(opsize));
        run_case("HLT terminator", hlt_seq, sizeof(hlt_seq));
        run_case("JMP rel8", jmp_rel8, sizeof(jmp_rel8));
        run_case("CALL rel16", call_rel16, sizeof(call_rel16));
        run_case("RET near", ret_near, sizeof(ret_near));
        run_case("INT imm8", int_imm, sizeof(int_imm));
        run_case("MOV AX,imm16", mov_ax_imm, sizeof(mov_ax_imm));
        run_case("group1 83 /0", group1, sizeof(group1));
        run_case("TEST AX,imm16", test_ax, sizeof(test_ax));
        run_case("BIOS-like 18-byte run", long_run, sizeof(long_run));

        /*
         * Byte length vs instruction count. Every one of these is handed to
         * the interpreter in a single call, and every one of them is longer
         * than one byte, so each used to record x86_len = 1.
         */
        run_case_true_len("true-len: 8E C0 MOV ES,AL (2 bytes)",
                          def_2byte, sizeof(def_2byte), 2);
        run_case_true_len("true-len: 83 C2 03 ADD DX,3 (3 bytes)",
                          def_3byte, sizeof(def_3byte), 3);
        run_case_true_len("true-len: 0F 84 00 00 JZ (4 bytes)",
                          def_4byte, sizeof(def_4byte), 4);
        run_case_true_len("true-len: 66 8B 07 MOV AX,[BX] (3 bytes, prefixed)",
                          def_prefixed, sizeof(def_prefixed), 3);

        /* The footprint the recorded length defines. */
        test_invalidation_footprint();

        /* The length decoder directly, including opcodes the translator never
         * defers (and so can never reach through translate_block). */
        test_x86_instr_len();

        /*
         * Direct audit of the call sequence emit_bl() itself produces, across
         * the full 32-bit target range. Runs regardless of where the cache sits.
         */
        test_emit_bl();

        /*
         * Forced out-of-range pass. Relocate the cache to the address the IS01
         * really uses so the code and the helpers are ~355MB apart and every
         * helper call is forced through emit_bl()'s out-of-range fallback --
         * the path that was broken. Without this the fallback is never taken
         * in the built-in suite and the regression check below is dead code.
         */
        printf("=== forced out-of-range suite (cache at 0x%lx, as on the IS01) ===\n\n",
               (unsigned long)IS01_CACHE_BASE);
        relocate_code_cache_to(IS01_CACHE_BASE);
        run_case("OOR: deferred: 8F POP Ev", pop_ev, sizeof(pop_ev));
        run_case("OOR: two-byte 0F 84", two_byte, sizeof(two_byte));
        run_case("OOR: MOV AX,imm16", mov_ax_imm, sizeof(mov_ax_imm));
        run_case("OOR: TEST AX,imm16", test_ax, sizeof(test_ax));
        run_case("OOR: BIOS-like 18-byte run", long_run, sizeof(long_run));
        /* Invalidation depends on the recorded byte length, not on how the
         * helper call was emitted, but re-run it here so the footprint is also
         * checked on blocks whose calls took emit_bl()'s out-of-range path. */
        test_invalidation_footprint();
        test_emit_bl();
    }

    printf("=== %d checks, %d failures ===\n", checks, failures);
    return failures ? 1 : 0;
}