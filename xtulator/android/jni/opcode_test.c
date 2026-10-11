/*
 * Opcode test harness for XTulator dynrec.
 *
 * For each opcode 0x00-0xFF:
 *   1. Generate minimal x86 test code (opcode + operands + NOP padding)
 *   2. Fork a child process to isolate crashes
 *   3. In child: set up CPU+memory, run interpreter (reference), save state,
 *      reset, run dynrec, save state, compare
 *   4. Exit codes: 0=WORKING, 1=CRASH, 2=WRONG, 3=NO_TEST, 4=INTERP_FALLBACK
 *
 * Results written to /sdcard/xtulator_opcode_test.log
 *
 * To use: call nativeTestOpcodes() from JNI. Returns results as a Java string.
 */

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <android/log.h>
#include <jni.h>

#include "../../XTulator/config.h"
#include "../../XTulator/cpu/cpu.h"
#include "../../XTulator/cpu/dynrec.h"
#include "../../XTulator/memory.h"
#include "../../XTulator/chipset/i8259.h"

#define LOG_TAG "XTulator-OpcTest"
#define OT_LOG(...) do { __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__); } while(0)

/* ---- Test RAM: 640KB mapped at 0x00000-0xA0000 ---- */
static uint8_t test_ram[0xA0000];
#define TEST_CODE_ADDR  0x7C00
#define TEST_STACK_ADDR 0x7E00
#define TEST_IVT_SENTINEL 0x7D00  /* address we jump to for INT return */

/* ---- Child crash handler: just exit with signal+100 ---- */
static void child_sig_handler(int sig) {
    _exit(100 + sig);
}

/* ---- Snapshot of observable CPU state ---- */
typedef struct {
    uint16_t ip, sp, bp, si, di, ax, bx, cx, dx;
    uint16_t cs, ds, es, ss;
    uint8_t cf, pf, af, zf, sf, tf, ifl, df, of;
    uint8_t hltstate, trap_toggle;
    uint8_t mode, reg, rm, addrbyte;
    uint32_t ea, tempaddr32;
} cpu_snap_t;

static void snap_save(CPU_t *cpu, cpu_snap_t *snap) {
    snap->ip       = cpu->ip;
    snap->sp       = cpu->regs.wordregs[regsp];
    snap->bp       = cpu->regs.wordregs[regbp];
    snap->si       = cpu->regs.wordregs[regsi];
    snap->di       = cpu->regs.wordregs[regdi];
    snap->ax       = cpu->regs.wordregs[regax];
    snap->bx       = cpu->regs.wordregs[regbx];
    snap->cx       = cpu->regs.wordregs[regcx];
    snap->dx       = cpu->regs.wordregs[regdx];
    snap->cs       = cpu->segregs[regcs];
    snap->ds       = cpu->segregs[regds];
    snap->es       = cpu->segregs[reges];
    snap->ss       = cpu->segregs[regss];
    snap->cf       = cpu->cf;
    snap->pf       = cpu->pf;
    snap->af       = cpu->af;
    snap->zf       = cpu->zf;
    snap->sf       = cpu->sf;
    snap->tf       = cpu->tf;
    snap->ifl      = cpu->ifl;
    snap->df       = cpu->df;
    snap->of       = cpu->of;
    snap->hltstate = cpu->hltstate;
    snap->trap_toggle = cpu->trap_toggle;
    snap->mode     = cpu->mode;
    snap->reg      = cpu->reg;
    snap->rm       = cpu->rm;
    snap->addrbyte = cpu->addrbyte;
    snap->ea       = cpu->ea;
    snap->tempaddr32 = cpu->tempaddr32;
}

/* Compare two snapshots.
 * Skip IP comparison for non-control-flow opcodes (the dynrec block
 * may execute extra NOP padding).  For control-flow opcodes, compare IP.
 * Returns 0 if identical (within rules), 1 if different. */
static int snap_diff(const cpu_snap_t *a, const cpu_snap_t *b, int compare_ip) {
    if (a->ax != b->ax) return 1;
    if (a->bx != b->bx) return 1;
    if (a->cx != b->cx) return 1;
    if (a->dx != b->dx) return 1;
    if (a->sp != b->sp) return 1;
    if (a->bp != b->bp) return 1;
    if (a->si != b->si) return 1;
    if (a->di != b->di) return 1;
    if (a->cs != b->cs) return 1;
    if (a->ds != b->ds) return 1;
    if (a->es != b->es) return 1;
    if (a->ss != b->ss) return 1;
    if (a->cf != b->cf) return 1;
    if (a->pf != b->pf) return 1;
    if (a->af != b->af) return 1;
    if (a->zf != b->zf) return 1;
    if (a->sf != b->sf) return 1;
    if (a->of != b->of) return 1;
    if (a->tf != b->tf) return 1;
    if (a->ifl != b->ifl) return 1;
    if (a->df != b->df) return 1;
    if (a->trap_toggle != b->trap_toggle) return 1;
    if (a->hltstate != b->hltstate) return 1;
    /* mode, reg, rm, addrbyte, ea, tempaddr32 are instruction-decoding
     * scratch fields set by the interpreter's decoder but not by dynrec
     * helpers.  They don't represent persistent emulator state, so skip them. */
    if (compare_ip && a->ip != b->ip) return 1;
    return 0;
}

/* ---- Determine if this opcode's IP should be compared ----
 * Only compare IP for genuine control-flow opcodes (jump/call/ret/loop/int)
 * where the dynrec block is exactly 1 instruction (block terminator).
 * For all other opcodes, the dynrec block includes NOP padding, so IP
 * will differ. We compare registers+flags instead. */
static int is_control_flow(uint8_t opcode) {
    switch (opcode) {
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F:
    case 0xC3: case 0xCD: case 0xE0: case 0xE1:
    case 0xE2: case 0xE3: case 0xE8: case 0xE9:
    case 0xEB: case 0xF4:
        return 1;
    default:
        return 0;
    }
}

/* ---- Check if opcode is in the translate_block switch ---- */
static int is_in_dynrec_switch(uint8_t opcode) {
    switch (opcode) {
    case 0x00: case 0x01: case 0x02: case 0x03:
    case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x0C: case 0x0D: case 0x0E: case 0x0F:
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x1C: case 0x1D: case 0x1E: case 0x1F:
    case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x24: case 0x25: case 0x26:
    case 0x28: case 0x29: case 0x2A: case 0x2B:
    case 0x2C: case 0x2D: case 0x2E:
    case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x34: case 0x35: case 0x36:
    case 0x38: case 0x39: case 0x3A: case 0x3B:
    case 0x3C: case 0x3D: case 0x3E:
    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4A: case 0x4B:
    case 0x4C: case 0x4D: case 0x4E: case 0x4F:
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57:
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F:
    case 0x64: case 0x65: case 0x66: case 0x67:
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F:
    case 0x80: case 0x81: case 0x82: case 0x83:
    case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8A: case 0x8B:
    case 0x8C: case 0x8D: case 0x8E: case 0x8F:
    case 0x90: case 0x9C: case 0x9D: case 0x9E: case 0x9F:
    case 0xA4: case 0xA5: case 0xA6: case 0xA7:
    case 0xA8: case 0xA9: case 0xAA: case 0xAB:
    case 0xAC: case 0xAD: case 0xAE: case 0xAF:
    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7:
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF:
    case 0xC3: case 0xCD:
    case 0xE0: case 0xE1: case 0xE2: case 0xE3:
    case 0xE8: case 0xE9: case 0xEB:
    case 0xF0: case 0xF2: case 0xF3: case 0xF4: case 0xF5:
    case 0xF8: case 0xF9: case 0xFA: case 0xFB:
    case 0xFC: case 0xFD:
        return 1;
    default:
        return 0;
    }
}

/* ---- Setup CPU state for testing ---- */
static void setup_test_cpu(CPU_t *cpu, uint16_t ip) {
    memset(cpu, 0, sizeof(CPU_t));
    cpu->segregs[regcs] = 0x0000;
    cpu->segregs[regds] = 0x0000;
    cpu->segregs[regss] = 0x0000;
    cpu->segregs[reges] = 0x0000;
    cpu->ip = ip;
    cpu->regs.wordregs[regax] = 0x1234;
    cpu->regs.wordregs[regbx] = 0x5678;
    cpu->regs.wordregs[regcx] = 0x9ABC;
    cpu->regs.wordregs[regdx] = 0xDEF0;
    cpu->regs.wordregs[regsp] = TEST_STACK_ADDR;
    cpu->regs.wordregs[regbp] = 0x1000;
    cpu->regs.wordregs[regsi] = 0x0100;
    cpu->regs.wordregs[regdi] = 0x0200;
    cpu->cf = 1; cpu->pf = 0; cpu->af = 0; cpu->zf = 0; cpu->sf = 0;
    cpu->of = 0; cpu->ifl = 0; cpu->tf = 0; cpu->df = 0;
    cpu->oldcf = 1;  /* INC/DEC helpers read cpu->oldcf to preserve CF */
    cpu->useseg = cpu->segregs[regds];
}

/* ---- Setup memory for testing ---- */
static void setup_test_memory(void) {
    /* Fill with NOP (0x90) — safe, no hltstate side effects. HLT (0xF4)
     * was tried but causes an infinite loop: after dinstr_hlt sets
     * cpu->hltstate=1, cpu_exec() skips ALL subsequent instructions
     * (goto skipexecution) without advancing IP. Then dynrec_exec's
     * hltstate handler calls cpu_exec(cpu, 100), which also skips,
     * and total_executed += 100 exceeds max_instr, BUT the IP_DESYNC
     * handler fires because IP didn't advance past the HLT — and if
     * IP_DESYNC keeps re-translating blocks at the same HLT address,
     * the while loop never terminates. With NOP fill, translate_block
     * terminates quickly via is_control_flow() or MAX_INSTRUCTIONS
     * boundary, and each NOP just does nothing. */
    memset(test_ram, 0x90, sizeof(test_ram));
    memory_init();
    memory_mapRegister(0x00000, 0xA0000, test_ram, test_ram);

    /* IVT: INT 0x10 vector at 0x0040-0x0043 */
    /* IP = TEST_IVT_SENTINEL, CS = 0x0000 */
    test_ram[0x0040] = TEST_IVT_SENTINEL & 0xFF;
    test_ram[0x0041] = (TEST_IVT_SENTINEL >> 8) & 0xFF;
    test_ram[0x0042] = 0x00;
    test_ram[0x0043] = 0x00;
    /* NOP at the sentinel address (interrupt return target) */
    test_ram[TEST_IVT_SENTINEL] = 0x90;

    /* Stack: return address for RET instructions — placed AT SP
     * (8086 POP reads from [SP] then increments).  SP=0x7E00 means
     * the return IP is at test_ram[0x7E00..0x7E01] and CS at
     * test_ram[0x7E02..0x7E03] for far RET. */
    test_ram[TEST_STACK_ADDR] = TEST_IVT_SENTINEL & 0xFF;
    test_ram[TEST_STACK_ADDR + 1] = (TEST_IVT_SENTINEL >> 8) & 0xFF;
    test_ram[TEST_STACK_ADDR + 2] = 0x00; /* CS low */
    test_ram[TEST_STACK_ADDR + 3] = 0x00; /* CS high */
}

/* ---- Generate x86 test code for an opcode ----
 * Returns number of bytes written to `code`, or -1 if no valid test. */
static int gen_test_code(uint8_t opcode, uint8_t *code, int maxlen) {
    int pos = 0;
    /* Fill with NOP (0x90) — see setup_test_memory() for rationale.
     * HLT (0xF4) was tried but causes dynrec_exec to infinite-loop. */
    memset(code, 0x90, maxlen);
    code[pos++] = opcode;
    if (pos >= maxlen) return pos;

    switch (opcode) {
    /* ---- imm8 operands ---- */
    case 0x04: case 0x0C: case 0x14: case 0x1C:
    case 0x24: case 0x2C: case 0x34: case 0x3C:
        code[pos++] = 0x55;
        break;
    case 0x6A: /* PUSH imm8 */
        code[pos++] = 0x55;
        break;
    case 0xCD: /* INT imm8 */
        code[pos++] = 0x10; /* INT 0x10 */
        break;

    /* ---- imm16 operands ---- */
    case 0x05: case 0x0D: case 0x15: case 0x1D:
    case 0x25: case 0x2D: case 0x35: case 0x3D:
        code[pos++] = 0x55; code[pos++] = 0x00;
        break;
    case 0x68: /* PUSH Iv */
        code[pos++] = 0x55; code[pos++] = 0x00;
        break;

    /* ---- ModR/M register-direct (mod=3, reg=0=AX/AL, rm=0=AX/AL) ---- */
    case 0x00: case 0x01: case 0x02: case 0x03:
    case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x28: case 0x29: case 0x2A: case 0x2B:
    case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x38: case 0x39: case 0x3A: case 0x3B:
        code[pos++] = 0xC0;
        break;

    /* ---- GRP1 (0x80-0x83), ModR/M + optionally imm ---- */
    case 0x80: /* ADD Eb,Ib — reg=0(ADD) */
        code[pos++] = 0xC0; code[pos++] = 0x55;
        break;
    case 0x81: /* ADD Ev,Iv — reg=0(ADD) */
        code[pos++] = 0xC0; code[pos++] = 0x55; code[pos++] = 0x00;
        break;
    case 0x82: /* ADD Eb,Ib (alias) */
        code[pos++] = 0xC0; code[pos++] = 0x55;
        break;
    case 0x83: /* ADD Ev,Ib (sign-ext) */
        code[pos++] = 0xC0; code[pos++] = 0x55;
        break;

    /* ---- TEST, XCHG, MOV (ModR/M register-direct) ---- */
    case 0x84: code[pos++] = 0xC0; break; /* TEST AL,AL */
    case 0x85: code[pos++] = 0xC0; break; /* TEST AX,AX */
    case 0x86: code[pos++] = 0xC0; break; /* XCHG AL,AL */
    case 0x87: code[pos++] = 0xC0; break; /* XCHG AX,AX */
    case 0x88: code[pos++] = 0xC0; break; /* MOV AL,AL */
    case 0x89: code[pos++] = 0xC0; break; /* MOV AX,AX */
    case 0x8A: code[pos++] = 0xC0; break; /* MOV AL,AL */
    case 0x8B: code[pos++] = 0xC0; break; /* MOV AX,AX */

    /* ---- MOV segment registers ---- */
    case 0x8C: code[pos++] = 0xC0; break; /* MOV AX,ES (reg=0=ES, rm=0=AX) */
    case 0x8E: code[pos++] = 0xC0; break; /* MOV ES,AX (reg=0=ES, rm=0=AX) */

    /* ---- LEA ---- */
    case 0x8D: code[pos++] = 0x00; break; /* LEA AX,[BX+SI] (mod=0,rm=0) */

    /* ---- POP Ev ---- */
    case 0x8F: code[pos++] = 0xC0; break; /* POP AX (reg=0, mod=3, rm=0) */

    /* ---- String ops: no operands, uses SI/DI ---- */
    case 0xA4: case 0xA5: case 0xA6: case 0xA7:
    case 0xAC: case 0xAD: case 0xAE: case 0xAF:
    case 0xAA: case 0xAB:
        break;

    /* ---- TEST AL/AX, Iv ---- */
    case 0xA8: code[pos++] = 0xFF; break;
    case 0xA9: code[pos++] = 0xFF; code[pos++] = 0x00; break;

    /* ---- MOV r8, Ib (0xB0-0xB7) ---- */
    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        code[pos++] = 0x42;
        break;

    /* ---- MOV r16, Iv (0xB8-0xBF) ---- */
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        code[pos++] = 0x34; code[pos++] = 0x12;
        break;

    /* ---- HLT ---- */
    case 0xF4:
        break;

    /* ---- Jcc rel8 (0x70-0x7F) — jump +2 (to NOP) ---- */
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F:
        /* rel8 = 0: if condition is met, jump to next byte (NOP);
           if not met, fall through to next byte (NOP). Either way, next instr. */
        code[pos++] = 0x00;
        break;

    /* ---- CALL rel16 (0xE8) — call next instruction (rel16=0) ---- */
    case 0xE8:
        code[pos++] = 0x03; code[pos++] = 0x00; /* rel16 = +3 (to NOP) */
        break;

    /* ---- JMP rel16 (0xE9) — jump to NOP ---- */
    case 0xE9:
        code[pos++] = 0x03; code[pos++] = 0x00; /* rel16 = +3 (to NOP) */
        break;

    /* ---- JMP rel8 (0xEB) — jump to NOP ---- */
    case 0xEB:
        code[pos++] = 0x02; /* rel8 = +2 (to NOP) */
        break;

    /* ---- RET near (0xC3) — pops return address from stack ---- */
    case 0xC3:
        break; /* Stack was set up with return address */

    /* ---- RETF (0xCB, 0xCA) — pops CS:IP ---- */
    case 0xCB: case 0xCA:
        if (opcode == 0xCA) {
            code[pos++] = 0x00; code[pos++] = 0x00; /* imm16 = 0 */
        }
        break;

    /* ---- INT imm8 (0xCD) — handled above ---- */
    case 0xCE: /* INT3 */
        /* INT3 is 0xCC, 0xCE is also INT3 on some processors */
        break;

    /* ---- LOOPNE/LOOPE/LOOP/JCXZ (0xE0-0xE3) — rel8 ---- */
    case 0xE0: case 0xE1: case 0xE2: case 0xE3:
        code[pos++] = 0x00; /* rel8 = 0 (jump to next instruction) */
        break;

    /* ---- IN/OUT Ib (0xE4-0xE7) ---- */
    case 0xE4: case 0xE6: /* IN AL,Ib / OUT Ib,AL */
        code[pos++] = 0x20;
        break;
    case 0xE5: case 0xE7: /* IN AX,Ib / OUT Ib,AX */
        code[pos++] = 0x20;
        break;

    /* ---- AAM/AAD (0xD4, 0xD5) ---- */
    case 0xD4: code[pos++] = 0x0A; break; /* AAM base=10 */
    case 0xD5: code[pos++] = 0x0A; break; /* AAD base=10 */

    /* ---- SALC/XLAT (0xD6, 0xD7) ---- */
    case 0xD6: case 0xD7:
        break;

    /* ---- x87 (0xD8-0xDF) ---- */
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
    case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        code[pos++] = 0xC0; /* ESC with ModR/M */
        break;

    /* ---- ENTER (0xC8) ---- */
    case 0xC8:
        code[pos++] = 0x00; code[pos++] = 0x00; /* imm16 = 0 */
        code[pos++] = 0x00; /* imm8 = 0 (nesting) */
        break;

    /* ---- GRP3 (0xF6, 0xF7) — TEST ---- */
    case 0xF6: code[pos++] = 0xC0; code[pos++] = 0x55; break; /* TEST AL,0x55 */
    case 0xF7: code[pos++] = 0xC0; code[pos++] = 0x55; code[pos++] = 0x00; break; /* TEST AX,0x0055 */

    /* ---- GRP4/GRP5 (0xFE, 0xFF) — INC ---- */
    case 0xFE: code[pos++] = 0xC0; break; /* INC AL */
    case 0xFF: code[pos++] = 0xC0; break; /* INC AX */

    /* ---- Prefix bytes: need a following instruction ---- */
    case 0x26: case 0x2E: case 0x36: case 0x3E:
    case 0x64: case 0x65: case 0x66: case 0x67:
    case 0xF0: case 0xF2: case 0xF3:
        /* Prefix + INC AX (0x40) — the prefix modifies the following instruction */
        code[pos++] = 0x40;
        break;

    /* ---- 0x0F two-byte escape ---- */
    case 0x0F:
        /* 0F 1F = multi-byte NOP (2 bytes) */
        code[pos++] = 0x1F;
        code[pos++] = 0x00;
        break;

    /* ---- No-operand opcodes handled by helpers ---- */
    case 0x06: case 0x07: case 0x0E: /* PUSH/POP segment registers */
    case 0x16: case 0x17: case 0x1E: case 0x1F: /* PUSH/POP SS/DS */
    case 0x90: /* NOP */
    case 0x9C: case 0x9D: case 0x9E: case 0x9F: /* PUSHF/POPF/SAHF/LAHF */
    case 0xF5: case 0xF8: case 0xF9: case 0xFA:
    case 0xFB: case 0xFC: case 0xFD: /* CMC/CLC/STC/CLI/STI/CLD/STD */
        break;

    default:
        /* Opcode has no operands and is just the opcode byte */
        break;
    }

    return pos;
}

/* ---- Run a single opcode test ----
 * Returns: 0=WORKING, 1=CRASH, 2=WRONG, 3=NO_TEST, 4=INTERP */
static int run_opcode_test(uint8_t opcode, char *detail, int detail_len) {
    OT_LOG("run_opcode_test: starting opcode 0x%02X", opcode);
    pid_t pid = fork();
    if (pid < 0) {
        OT_LOG("run_opcode_test: fork failed for opcode 0x%02X: %s", opcode, strerror(errno));
        snprintf(detail, detail_len, "fork failed: %s", strerror(errno));
        return 1;
    }
    OT_LOG("run_opcode_test: fork ok pid=%d for opcode 0x%02X", pid, opcode);

    if (pid == 0) {
        /* ---- CHILD ---- */
        CPU_t cpu;
        I8259_t i8259;
        uint8_t code[32];

        /* Install crash handler */
        signal(SIGSEGV, child_sig_handler);
        signal(SIGILL, child_sig_handler);
        signal(SIGBUS, child_sig_handler);
        signal(SIGABRT, child_sig_handler);
        signal(SIGFPE, child_sig_handler);

        /* If opcode is not in the dynrec switch, it's INTERP */
        if (!is_in_dynrec_switch(opcode)) {
            _exit(4); /* INTERP */
        }

        /* Set up memory */
        setup_test_memory();

        /* Set up I8259 */
        memset(&i8259, 0, sizeof(i8259));
        i8259.enabled = 1;

        /* Generate test code */
        int code_len = gen_test_code(opcode, code, sizeof(code));
        if (code_len < 0) {
            _exit(3); /* NO_TEST */
        }

        /* Copy code to test RAM */
        memcpy(&test_ram[TEST_CODE_ADDR], code, sizeof(code));

        /* ---- Step 1: Run interpreter (reference) ---- */
        setup_test_cpu(&cpu, TEST_CODE_ADDR);
        cpu.ifl = 0;
        cpu_exec(&cpu, 1);
        cpu_snap_t expected;
        snap_save(&cpu, &expected);

        /* ---- Step 2: Reset, run dynrec ---- */
        /* Restore test_ram to pristine state — the interpreter step may
         * have modified memory (e.g. SBB [mem],AL writes back).  Without
         * this, the dynrec reads stale/modified bytes and diverges. */
        setup_test_memory();
        memcpy(&test_ram[TEST_CODE_ADDR], code, sizeof(code));

        setup_test_cpu(&cpu, TEST_CODE_ADDR);
        cpu.ifl = 0;

        dynrec_init();
        dynrec_enable();

        i8259.irr = 0;
        i8259.imr = 0xFF; /* mask all interrupts during test */

        dynrec_exec(&cpu, &i8259, 1);
        cpu_snap_t actual;
        snap_save(&cpu, &actual);

        /* ---- Step 3: Compare ---- */
        int cf = is_control_flow(opcode);
        int diff = snap_diff(&expected, &actual, cf);

        if (diff) {
            /* Results differ — WRONG */
            char buf[512];
            int blen = snprintf(buf, sizeof(buf),
                "0x%02X interpreter: ax=0x%04x dx=0x%04x cx=0x%04x bx=0x%04x "
                "sp=0x%04x ip=0x%04x cf=%d zf=%d sf=%d hlt=%d | "
                "dynrec: ax=0x%04x dx=0x%04x cx=0x%04x bx=0x%04x "
                "sp=0x%04x ip=0x%04x cf=%d zf=%d sf=%d hlt=%d",
                opcode,
                expected.ax, expected.dx, expected.cx, expected.bx,
                expected.sp, expected.ip, expected.cf, expected.zf,
                expected.sf, expected.hltstate,
                actual.ax, actual.dx, actual.cx, actual.bx,
                actual.sp, actual.ip, actual.cf, actual.zf,
                actual.sf, actual.hltstate);
            int fd = open("/sdcard/xt_opcode_detail.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd >= 0) {
                write(fd, buf, blen);
                write(fd, "\n", 1);
                close(fd);
            }
            _exit(2); /* WRONG */
        }

        _exit(0); /* WORKING */
    }

    /* ---- PARENT ---- */
    int status;
    waitpid(pid, &status, 0);

    if (WIFSIGNALED(status)) {
        snprintf(detail, detail_len, "killed by signal %d", WTERMSIG(status));
        return 1; /* CRASH */
    }

    int code = WEXITSTATUS(status);
    switch (code) {
    case 0:  snprintf(detail, detail_len, "OK"); return 0;  /* WORKING */
    case 1:  snprintf(detail, detail_len, "child exit (crash)"); return 1; /* CRASH */
    case 2:  snprintf(detail, detail_len, "MISMATCH"); return 2; /* WRONG */
    case 3:  snprintf(detail, detail_len, "no test case"); return 3; /* NO_TEST */
    case 4:  snprintf(detail, detail_len, "interp-only"); return 4; /* INTERP */
    default:
        if (code >= 100) {
            snprintf(detail, detail_len, "crashed sig %d", code - 100);
            return 1; /* CRASH */
        }
        snprintf(detail, detail_len, "exit code %d", code);
        return 1;
    }
}

/* ---- JNI entry point: test all opcodes and return results ---- */
JNIEXPORT jstring JNICALL
Java_com_xtulator_android_XTulatorActivity_nativeTestOpcodes(JNIEnv *env, jobject thiz) {
    char result_buf[16384];
    int pos = 0;
    int working = 0, crashed = 0, wrong = 0, interp = 0, notest = 0;

    /* Clear detail log and progress log */
    int dfd = open("/sdcard/xt_opcode_detail.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dfd >= 0) close(dfd);
    
    /* Open progress log */
    int pfd = open("/sdcard/xtulator_test_progress.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    char progress_line[256];
    
    OT_LOG("Starting opcode tests...");
    pos += snprintf(result_buf + pos, sizeof(result_buf) - pos,
        "=== Opcode Test Results ===\n");

    const char *opnames[256];
    /* Fill name table */
    {
        int i;
        for (i = 0; i < 256; i++) opnames[i] = "???";
        opnames[0x00] = "ADD_EbGb"; opnames[0x01] = "ADD_EvGv";
        opnames[0x02] = "ADD_GbEb"; opnames[0x03] = "ADD_GvEv";
        opnames[0x04] = "ADD_ALIb"; opnames[0x05] = "ADD_AXIv";
        opnames[0x06] = "PUSH_ES"; opnames[0x07] = "POP_ES";
        opnames[0x08] = "OR_EbGb"; opnames[0x09] = "OR_EvGv";
        opnames[0x0A] = "OR_GbEb"; opnames[0x0B] = "OR_GvEv";
        opnames[0x0C] = "OR_ALIb"; opnames[0x0D] = "OR_AXIv";
        opnames[0x0E] = "PUSH_CS"; opnames[0x0F] = "2BYTE_Escape";
        opnames[0x10] = "ADC_EbGb"; opnames[0x11] = "ADC_EvGv";
        opnames[0x12] = "ADC_GbEb"; opnames[0x13] = "ADC_GvEv";
        opnames[0x14] = "ADC_ALIb"; opnames[0x15] = "ADC_AXIv";
        opnames[0x16] = "PUSH_SS"; opnames[0x17] = "POP_SS";
        opnames[0x18] = "SBB_EbGb"; opnames[0x19] = "SBB_EvGv";
        opnames[0x1A] = "SBB_GbEb"; opnames[0x1B] = "SBB_GvEv";
        opnames[0x1C] = "SBB_ALIb"; opnames[0x1D] = "SBB_AXIv";
        opnames[0x1E] = "PUSH_DS"; opnames[0x1F] = "POP_DS";
        opnames[0x20] = "AND_EbGb"; opnames[0x21] = "AND_EvGv";
        opnames[0x22] = "AND_GbEb"; opnames[0x23] = "AND_GvEv";
        opnames[0x24] = "AND_ALIb"; opnames[0x25] = "AND_AXIv";
        opnames[0x26] = "ES_PREFIX";
        opnames[0x27] = "DAA";
        opnames[0x28] = "SUB_EbGb"; opnames[0x29] = "SUB_EvGv";
        opnames[0x2A] = "SUB_GbEb"; opnames[0x2B] = "SUB_GvEv";
        opnames[0x2C] = "SUB_ALIb"; opnames[0x2D] = "SUB_AXIv";
        opnames[0x2E] = "CS_PREFIX";
        opnames[0x2F] = "DAS";
        opnames[0x30] = "XOR_EbGb"; opnames[0x31] = "XOR_EvGv";
        opnames[0x32] = "XOR_GbEb"; opnames[0x33] = "XOR_GvEv";
        opnames[0x34] = "XOR_ALIb"; opnames[0x35] = "XOR_AXIv";
        opnames[0x36] = "SS_PREFIX"; opnames[0x37] = "AAA";
        opnames[0x38] = "CMP_EbGb"; opnames[0x39] = "CMP_EvGv";
        opnames[0x3A] = "CMP_GbEb"; opnames[0x3B] = "CMP_GvEv";
        opnames[0x3C] = "CMP_ALIb"; opnames[0x3D] = "CMP_AXIv";
        opnames[0x3E] = "DS_PREFIX"; opnames[0x3F] = "AAS";
        opnames[0x40] = "INC_AX"; opnames[0x41] = "INC_CX";
        opnames[0x42] = "INC_DX"; opnames[0x43] = "INC_BX";
        opnames[0x44] = "INC_SP"; opnames[0x45] = "INC_BP";
        opnames[0x46] = "INC_SI"; opnames[0x47] = "INC_DI";
        opnames[0x48] = "DEC_AX"; opnames[0x49] = "DEC_CX";
        opnames[0x4A] = "DEC_DX"; opnames[0x4B] = "DEC_BX";
        opnames[0x4C] = "DEC_SP"; opnames[0x4D] = "DEC_BP";
        opnames[0x4E] = "DEC_SI"; opnames[0x4F] = "DEC_DI";
        opnames[0x50] = "PUSH_AX"; opnames[0x51] = "PUSH_CX";
        opnames[0x52] = "PUSH_DX"; opnames[0x53] = "PUSH_BX";
        opnames[0x54] = "PUSH_SP"; opnames[0x55] = "PUSH_BP";
        opnames[0x56] = "PUSH_SI"; opnames[0x57] = "PUSH_DI";
        opnames[0x58] = "POP_AX"; opnames[0x59] = "POP_CX";
        opnames[0x5A] = "POP_DX"; opnames[0x5B] = "POP_BX";
        opnames[0x5C] = "POP_SP"; opnames[0x5D] = "POP_BP";
        opnames[0x5E] = "POP_SI"; opnames[0x5F] = "POP_DI";
        opnames[0x60] = "PUSHA"; opnames[0x61] = "POPA";
        opnames[0x62] = "BOUND"; opnames[0x63] = "ARPL";
        opnames[0x64] = "FS_PREFIX"; opnames[0x65] = "GS_PREFIX";
        opnames[0x66] = "OP_SZ_PREFIX"; opnames[0x67] = "ADDR_SZ_PREFIX";
        opnames[0x68] = "PUSH_Iv"; opnames[0x69] = "IMUL_GvEvIv";
        opnames[0x6A] = "PUSH_Ib"; opnames[0x6B] = "IMUL_GvEvIb";
        opnames[0x70] = "JOE_rel8"; opnames[0x71] = "JNO_rel8";
        opnames[0x72] = "JB_rel8"; opnames[0x73] = "JNB_rel8";
        opnames[0x74] = "JZ_rel8"; opnames[0x75] = "JNZ_rel8";
        opnames[0x76] = "JBE_rel8"; opnames[0x77] = "JA_rel8";
        opnames[0x78] = "JS_rel8"; opnames[0x79] = "JNS_rel8";
        opnames[0x7A] = "JP_rel8"; opnames[0x7B] = "JNP_rel8";
        opnames[0x7C] = "JL_rel8"; opnames[0x7D] = "JNL_rel8";
        opnames[0x7E] = "JLE_rel8"; opnames[0x7F] = "JG_rel8";
        opnames[0x80] = "GRP1_EbIb"; opnames[0x81] = "GRP1_EvIv";
        opnames[0x82] = "GRP1_EbIb_A"; opnames[0x83] = "GRP1_EvIb";
        opnames[0x84] = "TEST_GbEb"; opnames[0x85] = "TEST_GvEv";
        opnames[0x86] = "XCHG_EbGb"; opnames[0x87] = "XCHG_EvGv";
        opnames[0x88] = "MOV_EbGb"; opnames[0x89] = "MOV_EvGv";
        opnames[0x8A] = "MOV_GbEb"; opnames[0x8B] = "MOV_GvEv";
        opnames[0x8C] = "MOV_EwSw"; opnames[0x8D] = "LEA_GvM";
        opnames[0x8E] = "MOV_SwEw"; opnames[0x8F] = "POP_Ev";
        opnames[0x90] = "NOP";
        opnames[0x91] = "XCHG_CX_AX"; opnames[0x92] = "XCHG_DX_AX";
        opnames[0x93] = "XCHG_BX_AX"; opnames[0x94] = "XCHG_SP_AX";
        opnames[0x95] = "XCHG_BP_AX"; opnames[0x96] = "XCHG_SI_AX";
        opnames[0x97] = "XCHG_DI_AX"; opnames[0x98] = "CBW";
        opnames[0x99] = "CWD"; opnames[0x9A] = "CALL_FAR";
        opnames[0x9B] = "WAIT"; opnames[0x9C] = "PUSHF";
        opnames[0x9D] = "POPF"; opnames[0x9E] = "SAHF"; opnames[0x9F] = "LAHF";
        opnames[0xA0] = "MOV_AL_moffs"; opnames[0xA1] = "MOV_AX_moffs";
        opnames[0xA2] = "MOV_moffs_AL"; opnames[0xA3] = "MOV_moffs_AX";
        opnames[0xA4] = "MOVSB"; opnames[0xA5] = "MOVSW";
        opnames[0xA6] = "CMPSB"; opnames[0xA7] = "CMPSW";
        opnames[0xA8] = "TEST_AL_Ib"; opnames[0xA9] = "TEST_AX_Iv";
        opnames[0xAA] = "STOSB"; opnames[0xAB] = "STOSW";
        opnames[0xAC] = "LODSB"; opnames[0xAD] = "LODSW";
        opnames[0xAE] = "SCASB"; opnames[0xAF] = "SCASW";
        opnames[0xB0] = "MOV_AL_Ib"; opnames[0xB1] = "MOV_CL_Ib";
        opnames[0xB2] = "MOV_DL_Ib"; opnames[0xB3] = "MOV_BL_Ib";
        opnames[0xB4] = "MOV_AH_Ib"; opnames[0xB5] = "MOV_CH_Ib";
        opnames[0xB6] = "MOV_DH_Ib"; opnames[0xB7] = "MOV_BH_Ib";
        opnames[0xB8] = "MOV_AX_Iv"; opnames[0xB9] = "MOV_CX_Iv";
        opnames[0xBA] = "MOV_DX_Iv"; opnames[0xBB] = "MOV_BX_Iv";
        opnames[0xBC] = "MOV_SP_Iv"; opnames[0xBD] = "MOV_BP_Iv";
        opnames[0xBE] = "MOV_SI_Iv"; opnames[0xBF] = "MOV_DI_Iv";
        opnames[0xC0] = "GRP2_EbIb"; opnames[0xC1] = "GRP2_EvIb";
        opnames[0xC2] = "RET_Iw"; opnames[0xC3] = "RET_near";
        opnames[0xC4] = "LES"; opnames[0xC5] = "LDS";
        opnames[0xC6] = "MOV_EbIb"; opnames[0xC7] = "MOV_EvIv";
        opnames[0xC8] = "ENTER"; opnames[0xC9] = "LEAVE";
        opnames[0xCA] = "RETF_Iw"; opnames[0xCB] = "RETF";
        opnames[0xCC] = "INT3"; opnames[0xCD] = "INT_Ib";
        opnames[0xCE] = "INTo"; opnames[0xCF] = "IRET";
        opnames[0xD0] = "GRP2_Eb1"; opnames[0xD1] = "GRP2_Ev1";
        opnames[0xD2] = "GRP2_EbCL"; opnames[0xD3] = "GRP2_EvCL";
        opnames[0xD4] = "AAM"; opnames[0xD5] = "AAD";
        opnames[0xD6] = "SALC"; opnames[0xD7] = "XLAT";
        opnames[0xD8] = "x87_D8"; opnames[0xD9] = "x87_D9";
        opnames[0xDA] = "x87_DA"; opnames[0xDB] = "x87_DB";
        opnames[0xDC] = "x87_DC"; opnames[0xDD] = "x87_DD";
        opnames[0xDE] = "x87_DE"; opnames[0xDF] = "x87_DF";
        opnames[0xE0] = "LOOPNE"; opnames[0xE1] = "LOOPE";
        opnames[0xE2] = "LOOP"; opnames[0xE3] = "JCXZ";
        opnames[0xE4] = "IN_AL_Ib"; opnames[0xE5] = "IN_AX_Ib";
        opnames[0xE6] = "OUT_Ib_AL"; opnames[0xE7] = "OUT_Ib_AX";
        opnames[0xE8] = "CALL_rel16"; opnames[0xE9] = "JMP_rel16";
        opnames[0xEA] = "JMP_far"; opnames[0xEB] = "JMP_rel8";
        opnames[0xEC] = "IN_AL_DX"; opnames[0xED] = "IN_AX_DX";
        opnames[0xEE] = "OUT_DX_AL"; opnames[0xEF] = "OUT_DX_AX";
        opnames[0xF0] = "LOCK_PREFIX"; opnames[0xF1] = "INT1";
        opnames[0xF2] = "REPNZ_PREFIX"; opnames[0xF3] = "REPE_PREFIX";
        opnames[0xF4] = "HLT"; opnames[0xF5] = "CMC";
        opnames[0xF6] = "GRP3_Eb"; opnames[0xF7] = "GRP3_Ev";
        opnames[0xF8] = "CLC"; opnames[0xF9] = "STC";
        opnames[0xFA] = "CLI"; opnames[0xFB] = "STI";
        opnames[0xFC] = "CLD"; opnames[0xFD] = "STD";
        opnames[0xFE] = "GRP4_Eb"; opnames[0xFF] = "GRP5_Ev";
    }

    for (int i = 0; i < 256; i++) {
        char detail[256] = {0};
        int r = run_opcode_test((uint8_t)i, detail, sizeof(detail));

        const char *status;
        switch (r) {
        case 0: status = "WORKING"; working++; break;
        case 1: status = "CRASH"; crashed++; break;
        case 2: status = "WRONG"; wrong++; break;
        case 3: status = "NO_TEST"; notest++; break;
        case 4: status = "INTERP"; interp++; break;
        default: status = "UNKNOWN"; notest++; break;
        }

        /* Log progress to file */
        int plen = snprintf(progress_line, sizeof(progress_line), 
            "OPCODE TEST %d/256: 0x%02X %-16s %s (%s)\n", 
            i+1, (uint8_t)i, opnames[i], status, detail);
        write(pfd, progress_line, plen);
        
        /* Log to logcat */
        OT_LOG("TEST %d/256: 0x%02X %s", i+1, (uint8_t)i, status);
        
        pos += snprintf(result_buf + pos, sizeof(result_buf) - pos,
            "0x%02X %-16s %s  (%s)\n", i, opnames[i], status, detail);
    }
    
    close(pfd);
    int fd = open("/sdcard/xtulator_opcode_test.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, result_buf, pos);
        close(fd);
    }

    OT_LOG("Opcode tests: %d working, %d crashed, %d wrong, %d interp, %d no_test",
           working, crashed, wrong, interp, notest);

    return (*env)->NewStringUTF(env, result_buf);
}
