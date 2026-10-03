/*
 * armdec.h — minimal ARM decoder for the instruction subset dynrec.c emits.
 *
 * This is NOT a general disassembler. It decodes exactly the encodings the
 * translator produces, enough to answer the questions the dynrec crash
 * investigation needs:
 *
 *   - which instructions write R4 (the block's CPU_t* register)?
 *   - does control flow ever land on a literal-pool word?
 *   - is the push/mov r4,r0 ... pop frame intact?
 *
 * Anything it does not recognise is reported as K_OTHER with the raw word
 * intact, so an unexpected encoding shows up as a decode miss rather than
 * being silently skipped.
 */
#ifndef ARMDEC_H
#define ARMDEC_H

#include <stdint.h>

enum arm_kind {
    K_OTHER = 0,
    K_PUSH,     /* STMDB sp!, {..}   */
    K_POP,      /* LDMIA sp!, {..pc}  */
    K_MOV_REG,  /* MOV Rd, Rm         */
    K_MOV_IMM,  /* MOV Rd, #imm       */
    K_ORR_IMM,  /* ORR Rd, Rn, #imm   */
    K_LDR_LIT,  /* LDR Rd, [PC, #imm] */
    K_LDR_STR,  /* LDR/STR [Rn, ...]  */
    K_B,        /* B  target           */
    K_BL,       /* BL target           */
    K_BLX_REG,  /* BLX Rm              */
    K_MISC      /* flag/arith DP       */
};

typedef struct {
    uint32_t word;
    int      kind;
    int      rd, rn, rm;      /* register operands, -1 if n/a  */
    int      writes_pc;       /* branches, or loads PC */
    int      writes_rd;       /* Rd is a destination */
    int      updates_flags;   /* S bit */
    int      loads_regs;      /* LDM-style: reglist lands in registers */
    uint32_t reglist;         /* STM/LDM register mask */
    uint32_t branch_target;   /* absolute word offset for B/BL */
    uint32_t lit_addr;        /* for K_LDR_LIT: absolute word offset loaded */
    uint32_t pool_value;      /* for literal-pool words: the embedded value */
    int      is_pool_word;    /* this word IS a literal pool entry */
} arm_insn_t;

/* Which registers does this instruction define? */
static inline uint32_t armdefs(const arm_insn_t *in) {
    switch (in->kind) {
    case K_MOV_REG:
    case K_MOV_IMM:
    case K_ORR_IMM:
    case K_LDR_LIT:
    case K_LDR_STR:
        return (in->rd >= 0 && in->writes_rd) ? (1u << in->rd) : 0u;
    case K_POP:
        return in->reglist;
    case K_PUSH:
        return 0u; /* stores, does not define */
    default:
        return 0u;
    }
}

static const char *arm_kind_name(int k) {
    switch (k) {
    case K_PUSH:    return "PUSH";
    case K_POP:     return "POP";
    case K_MOV_REG: return "MOV_REG";
    case K_MOV_IMM: return "MOV_IMM";
    case K_ORR_IMM: return "ORR_IMM";
    case K_LDR_LIT: return "LDR_LIT";
    case K_LDR_STR: return "LDR_STR";
    case K_B:       return "B";
    case K_BL:      return "BL";
    case K_BLX_REG: return "BLX_REG";
    case K_MISC:    return "MISC";
    default:        return "OTHER";
    }
}

static void arm_decode(uint32_t word, uint32_t pc_off, arm_insn_t *out) {
    out->word = word;
    out->rd = out->rn = out->rm = -1;
    out->writes_pc = 0;
    out->writes_rd = 0;
    out->updates_flags = 0;
    out->loads_regs = 0;
    out->reglist = 0;
    out->branch_target = 0;
    out->lit_addr = 0;
    out->pool_value = 0;
    out->is_pool_word = 0;
    out->kind = K_OTHER;

    /*
     * pc_off is a WORD INDEX within the block, and every computed target is
     * a word index too. ARM's PC reads as instr_addr + 8, i.e. +2 words, so
     * the pipeline bias is a constant +2 here.
     */

    /* Block data transfer (LDM/STM): cond 100 P U 0 W L rn reglist.
     * NOTE: there is no bit-4 test here -- bit 4 of the encoding is simply
     * r4 in the register list, and the very prologue dynrec emits
     * (0xE92D4C10 = PUSH {r4,r10,r11,lr}) has it set. */
    if ((word & 0x0E000000) == 0x08000000) {
        int L = (word >> 20) & 1;
        out->kind = L ? K_POP : K_PUSH;
        out->rn = (word >> 16) & 0xF;
        out->reglist = word & 0xFFFF;
        out->loads_regs = L;
        out->writes_rd = L;
        out->rd = 15; /* LDM writes a list; rd unused */
        if (out->reglist & (1u << 15)) out->writes_pc = 1;
        return;
    }

    /* Branch / branch-with-link: cond 101 offset24 */
    if ((word & 0x0E000000) == 0x0A000000) {
        int32_t off = (int32_t)(word & 0x00FFFFFF); /* 24-bit signed */
        if (off & 0x800000) off |= 0xFF000000;     /* sign-extend to 32 bits */
        out->kind = (word & 0x01000000) ? K_BL : K_B;
        out->branch_target = (uint32_t)((int32_t)pc_off + 2 + off);
        out->writes_pc = 1;
        return;
    }

    /* BLX (immediate) does not exist on ARMv5; BLX register is
     * 0xE12FFF3C == cond 0001 001 1 1111 1111 1111 0011 1100, so the
     * masked value is 0x012FFF30 (not 0x012FFF10 -- bits 7:4 are the
     * register number, and dynrec's BLX R12 has 0b1100 there). */
    if ((word & 0x0FFFFFF0) == 0x012FFF30) {
        out->kind = K_BLX_REG;
        out->rm = word & 0xF;
        out->writes_pc = 1;
        return;
    }

    /* Single data transfer: cond 010 P U 1(S) W L rn rd offset12 */
    if ((word & 0x0C000000) == 0x04000000) {
        int L = (word >> 20) & 1;
        int imm = !(word & 0x02000000);   /* bit 25 = "immediate" form */
        out->kind = K_LDR_STR;
        out->rn = (word >> 16) & 0xF;
        out->rd = (word >> 12) & 0xF;
        out->writes_rd = 1;
        if (imm) {
            uint32_t off = word & 0xFFF;
            uint32_t U = (word >> 23) & 1;
            int32_t byte_off = U ? (int32_t)off : -(int32_t)off;
            if (L && out->rn == 15) {
                /* LDR Rd, [PC, #off] — a literal pool load. */
                out->kind = K_LDR_LIT;
                out->lit_addr = (uint32_t)((int32_t)pc_off + 2 + byte_off / 4);
            }
        }
        if (out->rd == 15) out->writes_pc = 1;
        return;
    }

    /* Data processing: cond 001 opcode S rn rd operand2 */
    if ((word & 0x0C000000) == 0x00000000) {
        int op = (word >> 21) & 0xF;
        int S = (word >> 20) & 1;
        int I = (word >> 25) & 1;
        int rot = ((word >> 8) & 0xF) * 2;
        out->rn = (word >> 16) & 0xF;
        out->rd = (word >> 12) & 0xF;
        out->updates_flags = S;
        if (op == 0x0D) {           /* MOV */
            out->writes_rd = 1;
            if (!I && rot == 0) {
                /* MOV Rd, Rm -- the form the prologue and every helper
                 * call site use. It shares the MOV-immediate opcode with
                 * I=0/rotate=0, so it must be split out here or every
                 * "mov r0, r4" gets mislabelled as an immediate. */
                out->kind = K_MOV_REG;
                out->rm = word & 0xF;
                out->rn = -1;
            } else {
                out->kind = K_MOV_IMM;
            }
        } else if (op == 0x0C) {    /* ORR */
            out->kind = K_ORR_IMM;
            out->writes_rd = 1;
        } else {
            out->kind = K_MISC;
            out->writes_rd = (S || op != 0x0A); /* MVN is read-modify-write */
        }
        return;
    }
}

#endif /* ARMDEC_H */