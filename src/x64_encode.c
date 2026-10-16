// x86-64 machine code encoder for the forms the lowering uses.

typedef u8 tgReg;
enum {
	RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
	R8,  R9,  R10, R11, R12, R13, R14, R15,
	TG_RIP = 0xff,
};

typedef u8 tgCC;
enum {
	CC_O  = 0x0, CC_NO = 0x1, CC_B  = 0x2, CC_AE = 0x3,
	CC_E  = 0x4, CC_NE = 0x5, CC_BE = 0x6, CC_A  = 0x7,
	CC_S  = 0x8, CC_NS = 0x9, CC_P  = 0xA, CC_NP = 0xB,
	CC_L  = 0xC, CC_GE = 0xD, CC_LE = 0xE, CC_G  = 0xF,

	// combined with a condition in the ext of tgVecForm_Flags, so in the same enum
	tgVecCond_AndNP = 0x10, // and not unordered
	tgVecCond_OrP   = 0x20, // or unordered
	tgVecCond_Swap  = 0x40, // compares b with a
};

// A machine operand: a register or a memory reference.
typedef struct tgOpnd {
	bool is_mem;
	u8   reg;    // register, or memory base (TG_RIP for rip-relative)
	i32  disp;
	i32  sym;    // rip-relative symbol, -1 if none
	tgRelocKind reloc;
} tgOpnd;

TG_INTERNAL tgOpnd tg_r(u8 reg) {
	tgOpnd o;
	memset(&o, 0, sizeof(o));
	o.reg = reg;
	o.sym = -1;
	return o;
}

TG_INTERNAL tgOpnd tg_m(u8 base, i32 disp) {
	tgOpnd o;
	memset(&o, 0, sizeof(o));
	o.is_mem = true;
	o.reg = base;
	o.disp = disp;
	o.sym = -1;
	return o;
}

// reloc was a default argument of tgReloc_PC32
TG_INTERNAL tgOpnd tg_m_sym(i32 sym, i32 addend, tgRelocKind reloc) {
	tgOpnd o;
	memset(&o, 0, sizeof(o));
	o.is_mem = true;
	o.reg = TG_RIP;
	o.disp = addend;
	o.sym = sym;
	o.reloc = reloc;
	return o;
}

enum {
	TG_W      = 1<<0, // REX.W
	TG_BYTE   = 1<<1, // 8-bit operation on a gpr (needs REX for sil/dil/spl/bpl)
	TG_P66    = 1<<2,
	TG_PF2    = 1<<3,
	TG_PF3    = 1<<4,
	TG_LOCK   = 1<<5,
	TG_0F     = 1<<6,
	TG_0F38   = 1<<7,
	TG_0F3A   = 1<<8,
};

// modrm [sib] [disp]. EVEX scales an 8-bit displacement by `disp8_scale` (1 unless EVEX).
TG_INTERNAL void tg_enc_modrm(tgAsm *a, u8 reg, tgOpnd rm, i32 imm_size, i32 disp8_scale) {
	u8 r = reg & 7;
	if (!rm.is_mem) {
		tg_b(a, (u8)(0xC0 | (r<<3) | (rm.reg & 7)));
		return;
	}
	if (rm.reg == TG_RIP) {
		tg_b(a, (u8)(0x00 | (r<<3) | 5));
		i64 at = tg_pos(a);
		tg_u32(a, 0);
		TG_ASSERT(rm.sym >= 0);
		// rip points past the immediate when there is one
		tg_asm_reloc(a, rm.reloc, at, rm.sym, (i64)rm.disp - 4 - imm_size);
		a->relocs->data[a->relocs->count-1].tail = (u8)imm_size;
		return;
	}
	u8 base = rm.reg & 7;
	i32 disp = rm.disp;
	u8 mod = 0;
	if (disp == 0 && base != 5) {
		mod = 0;
	} else if (disp % disp8_scale == 0 && disp/disp8_scale >= -128 && disp/disp8_scale <= 127) {
		mod = 1;
	} else {
		mod = 2;
	}
	if (base == 4) {
		tg_b(a, (u8)((mod<<6) | (r<<3) | 4));
		tg_b(a, 0x24);
	} else {
		tg_b(a, (u8)((mod<<6) | (r<<3) | base));
	}
	if (mod == 1) {
		tg_b(a, (u8)(i8)(disp/disp8_scale));
	} else if (mod == 2) {
		tg_u32(a, (u32)disp);
	}
}

// Encodes [prefixes] [REX] opcode modrm [sib] [disp]. `imm_size` is the number of
// immediate bytes the caller writes afterwards, needed for rip-relative addends.
TG_INTERNAL void tg_enc(tgAsm *a, u32 flags, u8 opcode, u8 reg, tgOpnd rm, i32 imm_size) {
	if (flags & TG_LOCK) tg_b(a, 0xF0);
	if (flags & TG_P66)  tg_b(a, 0x66);
	if (flags & TG_PF2)  tg_b(a, 0xF2);
	if (flags & TG_PF3)  tg_b(a, 0xF3);

	u8 rex = 0x40;
	if (flags & TG_W) rex |= 0x08;
	if (reg & 8)      rex |= 0x04;
	if (rm.is_mem) {
		if (rm.reg != TG_RIP && (rm.reg & 8)) rex |= 0x01;
	} else {
		if (rm.reg & 8) rex |= 0x01;
	}
	bool need_rex = rex != 0x40;
	if (flags & TG_BYTE) {
		// spl, bpl, sil, dil are only reachable with a REX prefix
		if ((reg >= 4 && reg < 8) || (!rm.is_mem && rm.reg >= 4 && rm.reg < 8)) {
			need_rex = true;
		}
	}
	if (need_rex) tg_b(a, rex);

	if (flags & TG_0F)   tg_b(a, 0x0F);
	if (flags & TG_0F38) { tg_b(a, 0x0F); tg_b(a, 0x38); }
	if (flags & TG_0F3A) { tg_b(a, 0x0F); tg_b(a, 0x3A); }
	tg_b(a, opcode);
	tg_enc_modrm(a, reg, rm, imm_size, 1);
}

// vmovups between ymm (VEX.256) or zmm (EVEX.512) and memory
TG_INTERNAL void tg_vmovups_wide(tgAsm *a, i32 size, bool load, u8 x, tgOpnd m) {
	TG_ASSERT(m.is_mem && x < 16);
	u8 not_r = (x & 8) ? 0 : 0x80;
	u8 not_b = (m.reg != TG_RIP && (m.reg & 8)) ? 0 : 0x20;
	if (size == 32) {
		tg_b(a, 0xC4);
		tg_b(a, (u8)(not_r | 0x40 | not_b | 0x01)); // ~R ~X ~B, map 0F
		tg_b(a, 0x7C);                               // W0, no vvvv, L=256, no prefix
		tg_b(a, load ? 0x10 : 0x11);
		tg_enc_modrm(a, x, m, 0, 1);
	} else {
		TG_ASSERT(size == 64);
		tg_b(a, 0x62);
		tg_b(a, (u8)(not_r | 0x40 | not_b | 0x10 | 0x01)); // ~R ~X ~B ~R', map 0F
		tg_b(a, 0x7C);                                      // W0, no vvvv, no prefix
		tg_b(a, 0x48);                                      // L'L=512, ~V'
		tg_b(a, load ? 0x10 : 0x11);
		tg_enc_modrm(a, x, m, 0, 64);
	}
}

TG_INTERNAL u32 tg_size_flags(i32 size) {
	switch (size) {
	case 1: return TG_BYTE;
	case 2: return TG_P66;
	case 4: return 0;
	case 8: return TG_W;
	}
	TG_PANIC("invalid operand size");
	return 0;
}

// mov reg, rm
TG_INTERNAL void tg_mov_r_rm(tgAsm *a, i32 size, u8 reg, tgOpnd rm) {
	tg_enc(a, tg_size_flags(size), size == 1 ? 0x8A : 0x8B, reg, rm, 0);
}
// mov rm, reg
TG_INTERNAL void tg_mov_rm_r(tgAsm *a, i32 size, tgOpnd rm, u8 reg) {
	tg_enc(a, tg_size_flags(size), size == 1 ? 0x88 : 0x89, reg, rm, 0);
}

TG_INTERNAL void tg_mov_r_imm(tgAsm *a, u8 reg, u64 imm) {
	if (imm <= 0xffffffffull) {
		// mov r32, imm32 zero extends
		if (reg & 8) tg_b(a, 0x41);
		tg_b(a, (u8)(0xB8 + (reg & 7)));
		tg_u32(a, (u32)imm);
	} else if ((i64)imm >= -0x80000000ll && (i64)imm < 0) {
		// mov r/m64, simm32
		tg_enc(a, TG_W, 0xC7, 0, tg_r(reg), 0);
		tg_u32(a, (u32)imm);
	} else {
		tg_b(a, (u8)(0x48 | ((reg & 8) ? 1 : 0)));
		tg_b(a, (u8)(0xB8 + (reg & 7)));
		tg_u64(a, imm);
	}
}

// Loads `size` bytes from rm into a 64-bit register, zero or sign extending.
TG_INTERNAL void tg_load_ext(tgAsm *a, i32 size, bool is_signed, u8 reg, tgOpnd rm) {
	switch (size) {
	case 1:
		// TG_BYTE: a REX prefix makes sil and dil reachable as the source
		if (is_signed) tg_enc(a, TG_BYTE|TG_W|TG_0F, 0xBE, reg, rm, 0);
		else           tg_enc(a, TG_BYTE|TG_0F,      0xB6, reg, rm, 0);
		break;
	case 2:
		if (is_signed) tg_enc(a, TG_W|TG_0F, 0xBF, reg, rm, 0);
		else           tg_enc(a, TG_0F,      0xB7, reg, rm, 0);
		break;
	case 4:
		if (is_signed) tg_enc(a, TG_W, 0x63, reg, rm, 0);
		else           tg_enc(a, 0,    0x8B, reg, rm, 0);
		break;
	case 8:
		tg_enc(a, TG_W, 0x8B, reg, rm, 0);
		break;
	default:
		TG_PANIC("invalid load size");
	}
}

TG_INTERNAL void tg_lea(tgAsm *a, u8 reg, tgOpnd m) {
	TG_ASSERT(m.is_mem);
	tg_enc(a, TG_W, 0x8D, reg, m, 0);
}

typedef u8 tgAluOp;
enum {
	ALU_ADD = 0, ALU_OR = 1, ALU_ADC = 2, ALU_SBB = 3, ALU_AND = 4, ALU_SUB = 5, ALU_XOR = 6, ALU_CMP = 7,
};

// alu reg, rm
TG_INTERNAL void tg_alu_r_rm(tgAsm *a, tgAluOp op, i32 size, u8 reg, tgOpnd rm) {
	u8 opc = (u8)((op<<3) | (size == 1 ? 0x02 : 0x03));
	tg_enc(a, tg_size_flags(size), opc, reg, rm, 0);
}

// alu rm, imm32 (sign extended)
TG_INTERNAL void tg_alu_rm_imm(tgAsm *a, tgAluOp op, i32 size, tgOpnd rm, i32 imm) {
	if (size == 1) {
		tg_enc(a, TG_BYTE, 0x80, op, rm, 1);
		tg_b(a, (u8)imm);
	} else if (imm >= -128 && imm <= 127) {
		tg_enc(a, tg_size_flags(size), 0x83, op, rm, 1);
		tg_b(a, (u8)(i8)imm);
	} else if (size == 2) {
		tg_enc(a, tg_size_flags(size), 0x81, op, rm, 2);
		tg_u16(a, (u16)imm);
	} else {
		tg_enc(a, tg_size_flags(size), 0x81, op, rm, 4);
		tg_u32(a, (u32)imm);
	}
}

TG_INTERNAL void tg_test_rm_r(tgAsm *a, i32 size, tgOpnd rm, u8 reg) {
	tg_enc(a, tg_size_flags(size), size == 1 ? 0x84 : 0x85, reg, rm, 0);
}

TG_INTERNAL void tg_imul_r_rm(tgAsm *a, i32 size, u8 reg, tgOpnd rm) {
	TG_ASSERT(size >= 2);
	tg_enc(a, tg_size_flags(size)|TG_0F, 0xAF, reg, rm, 0);
}

// group 3: F7 /n -- 2 not, 3 neg, 4 mul, 5 imul, 6 div, 7 idiv
TG_INTERNAL void tg_grp3(tgAsm *a, u8 n, i32 size, tgOpnd rm) {
	tg_enc(a, tg_size_flags(size), size == 1 ? 0xF6 : 0xF7, n, rm, 0);
}

// shifts by cl: D3 /n -- 4 shl, 5 shr, 7 sar, 0 rol, 1 ror
TG_INTERNAL void tg_shift_cl(tgAsm *a, u8 n, i32 size, tgOpnd rm) {
	tg_enc(a, tg_size_flags(size), size == 1 ? 0xD2 : 0xD3, n, rm, 0);
}

TG_INTERNAL void tg_shift_imm(tgAsm *a, u8 n, i32 size, tgOpnd rm, u8 imm) {
	tg_enc(a, tg_size_flags(size), size == 1 ? 0xC0 : 0xC1, n, rm, 1);
	tg_b(a, imm);
}

TG_INTERNAL void tg_setcc(tgAsm *a, tgCC cc, u8 reg) {
	tg_enc(a, TG_BYTE|TG_0F, (u8)(0x90 + cc), 0, tg_r(reg), 0);
}

TG_INTERNAL void tg_cmov(tgAsm *a, tgCC cc, i32 size, u8 reg, tgOpnd rm) {
	if (size < 4) size = 4;
	tg_enc(a, tg_size_flags(size)|TG_0F, (u8)(0x40 + cc), reg, rm, 0);
}

// sign-extends rax into rdx for division
TG_INTERNAL void tg_sign_extend_rdx(tgAsm *a, i32 size) {
	switch (size) {
	case 2: tg_b(a, 0x66); tg_b(a, 0x99); break;
	case 4: tg_b(a, 0x99); break;
	case 8: tg_b(a, 0x48); tg_b(a, 0x99); break;
	default: TG_PANIC("bad size");
	}
}

TG_INTERNAL i64 tg_jmp32(tgAsm *a) {
	tg_b(a, 0xE9);
	i64 at = tg_pos(a);
	tg_u32(a, 0);
	return at;
}

TG_INTERNAL i64 tg_jcc32(tgAsm *a, tgCC cc) {
	tg_b(a, 0x0F);
	tg_b(a, (u8)(0x80 + cc));
	i64 at = tg_pos(a);
	tg_u32(a, 0);
	return at;
}

TG_INTERNAL void tg_patch_rel32(tgAsm *a, i64 at, i64 target) {
	i64 rel = target - (at + 4);
	tg_patch_u32(a, at, (u32)(i32)rel);
}

TG_INTERNAL void tg_call_sym(tgAsm *a, i32 sym) {
	tg_b(a, 0xE8);
	i64 at = tg_pos(a);
	tg_u32(a, 0);
	tg_asm_reloc(a, tgReloc_PLT32, at, sym, -4);
}

TG_INTERNAL void tg_call_rm(tgAsm *a, tgOpnd rm) {
	tg_enc(a, 0, 0xFF, 2, rm, 0);
}

TG_INTERNAL void tg_push(tgAsm *a, u8 reg) {
	if (reg & 8) tg_b(a, 0x41);
	tg_b(a, (u8)(0x50 + (reg & 7)));
}

TG_INTERNAL void tg_pop(tgAsm *a, u8 reg) {
	if (reg & 8) tg_b(a, 0x41);
	tg_b(a, (u8)(0x58 + (reg & 7)));
}

TG_INTERNAL void tg_ret(tgAsm *a)  { tg_b(a, 0xC3); }
TG_INTERNAL void tg_leave(tgAsm *a) { tg_b(a, 0xC9); }
TG_INTERNAL void tg_ud2(tgAsm *a)  { tg_b(a, 0x0F); tg_b(a, 0x0B); }
TG_INTERNAL void tg_int3(tgAsm *a) { tg_b(a, 0xCC); }
TG_INTERNAL void tg_rep_movsb(tgAsm *a) { tg_b(a, 0xF3); tg_b(a, 0xA4); }
TG_INTERNAL void tg_rep_stosb(tgAsm *a) { tg_b(a, 0xF3); tg_b(a, 0xAA); }
TG_INTERNAL void tg_mfence(tgAsm *a) { tg_b(a, 0x0F); tg_b(a, 0xAE); tg_b(a, 0xF0); }
TG_INTERNAL void tg_pause(tgAsm *a) { tg_b(a, 0xF3); tg_b(a, 0x90); }
TG_INTERNAL void tg_rdtsc(tgAsm *a) { tg_b(a, 0x0F); tg_b(a, 0x31); }
TG_INTERNAL void tg_rdtscp(tgAsm *a) { tg_b(a, 0x0F); tg_b(a, 0x01); tg_b(a, 0xF9); }
TG_INTERNAL void tg_syscall(tgAsm *a) { tg_b(a, 0x0F); tg_b(a, 0x05); }
TG_INTERNAL void tg_std(tgAsm *a) { tg_b(a, 0xFD); }
TG_INTERNAL void tg_cld(tgAsm *a) { tg_b(a, 0xFC); }

// SSE

// movss/movsd xmm, m  (or xmm, xmm)
TG_INTERNAL void tg_movs_x_rm(tgAsm *a, i32 size, u8 xmm, tgOpnd rm) {
	tg_enc(a, (size == 4 ? TG_PF3 : TG_PF2)|TG_0F, 0x10, xmm, rm, 0);
}
// movss/movsd m, xmm
TG_INTERNAL void tg_movs_rm_x(tgAsm *a, i32 size, tgOpnd rm, u8 xmm) {
	tg_enc(a, (size == 4 ? TG_PF3 : TG_PF2)|TG_0F, 0x11, xmm, rm, 0);
}
TG_INTERNAL void tg_movups_x_m(tgAsm *a, u8 xmm, tgOpnd m) {
	tg_enc(a, TG_0F, 0x10, xmm, m, 0);
}
TG_INTERNAL void tg_movups_m_x(tgAsm *a, tgOpnd m, u8 xmm) {
	tg_enc(a, TG_0F, 0x11, xmm, m, 0);
}
// vfmadd213ss/sd xmm, xmm_v, xmm/m: xmm = xmm_v*xmm + rm
TG_INTERNAL void tg_vfmadd213_s(tgAsm *a, i32 size, u8 xmm, u8 xmm_v, tgOpnd rm) {
	u8 not_r = (xmm & 8) ? 0 : 0x80;
	u8 not_b = (rm.reg != TG_RIP && (rm.reg & 8)) ? 0 : 0x20;
	tg_b(a, 0xC4);
	tg_b(a, (u8)(not_r | 0x40 | not_b | 0x02));                       // ~R ~X ~B, map 0F38
	tg_b(a, (u8)((size == 8 ? 0x80 : 0) | ((~xmm_v & 15) << 3) | 0x01)); // W, ~vvvv, LIG, 66
	tg_b(a, 0xA9);
	tg_enc_modrm(a, xmm, rm, 0, 1);
}
// movd/movq xmm, r/m32/64
TG_INTERNAL void tg_movd_x_rm(tgAsm *a, i32 size, u8 xmm, tgOpnd rm) {
	tg_enc(a, TG_P66|TG_0F|(size == 8 ? TG_W : 0), 0x6E, xmm, rm, 0);
}
// movd/movq r/m32/64, xmm
TG_INTERNAL void tg_movd_rm_x(tgAsm *a, i32 size, tgOpnd rm, u8 xmm) {
	tg_enc(a, TG_P66|TG_0F|(size == 8 ? TG_W : 0), 0x7E, xmm, rm, 0);
}

typedef u8 tgSseOp;
enum {
	SSE_SQRT = 0x51,
	SSE_AND  = 0x54,
	SSE_XOR  = 0x57,
	SSE_ADD  = 0x58,
	SSE_MUL  = 0x59,
	SSE_CVT  = 0x5A, // ss<->sd
	SSE_SUB  = 0x5C,
	SSE_MIN  = 0x5D,
	SSE_DIV  = 0x5E,
	SSE_MAX  = 0x5F,
};

// scalar op: addss/addsd xmm, xmm/m
TG_INTERNAL void tg_sse_scalar(tgAsm *a, tgSseOp op, i32 size, u8 xmm, tgOpnd rm) {
	tg_enc(a, (size == 4 ? TG_PF3 : TG_PF2)|TG_0F, op, xmm, rm, 0);
}

TG_INTERNAL void tg_xorps(tgAsm *a, u8 x, u8 y) {
	tg_enc(a, TG_0F, 0x57, x, tg_r(y), 0);
}

TG_INTERNAL void tg_ucomis(tgAsm *a, i32 size, u8 xmm, tgOpnd rm) {
	tg_enc(a, (size == 8 ? TG_P66 : 0)|TG_0F, 0x2E, xmm, rm, 0);
}

// cvtsi2ss/sd xmm, r/m32/64
TG_INTERNAL void tg_cvtsi2f(tgAsm *a, i32 fsize, i32 isz, u8 xmm, tgOpnd rm) {
	tg_enc(a, (fsize == 4 ? TG_PF3 : TG_PF2)|TG_0F|(isz == 8 ? TG_W : 0), 0x2A, xmm, rm, 0);
}

// cvttss/sd2si r32/64, xmm/m
TG_INTERNAL void tg_cvttf2si(tgAsm *a, i32 fsize, i32 isz, u8 reg, tgOpnd rm) {
	tg_enc(a, (fsize == 4 ? TG_PF3 : TG_PF2)|TG_0F|(isz == 8 ? TG_W : 0), 0x2C, reg, rm, 0);
}

TG_INTERNAL void tg_bswap(tgAsm *a, i32 size, u8 reg) {
	u8 rex = 0x40;
	if (size == 8) rex |= 0x08;
	if (reg & 8)   rex |= 0x01;
	if (rex != 0x40) tg_b(a, rex);
	tg_b(a, 0x0F);
	tg_b(a, (u8)(0xC8 + (reg & 7)));
}

// How an x86 vector intrinsic maps onto its 128-bit SSE instruction
typedef u8 tgVecForm;
enum {
	tgVecForm_Binary,   // op xmm1(a), xmm2(b)
	tgVecForm_Unary,    // op xmm1(a), xmm2(a): the scalar forms keep the upper lanes of a
	tgVecForm_Xmm0,     // op xmm1(a), xmm2(b), with c implicitly in xmm0
	tgVecForm_ShiftImm, // op /ext xmm1(a), imm8
	tgVecForm_ToGpr,    // op eax, xmm1(a): the result is a scalar
	tgVecForm_Flags,    // op xmm1(a), xmm2(b): the result is the condition in ext
	tgVecForm_Rdtscp,   // rdtscp: the result is the (u64, u32) tuple of the counter and IA32_TSC_AUX
};

// ext of tgVecForm_Flags: a tgCC, possibly combined with a tgVecCond_ flag (declared with the tgCC values)

typedef struct tgVecIntrinsic {
	char const *name;
	u32 flags;
	u8  opcode;
	tgVecForm form;
	u8  ext;  // the modrm reg field of tgVecForm_ShiftImm, the condition of tgVecForm_Flags
	bool imm; // the last argument is a constant imm8
} tgVecIntrinsic;

static tgVecIntrinsic const tg_vec_intrinsics[] = {
	{"llvm.x86.ssse3.pshuf.b.128",   TG_P66|TG_0F38, 0x00, tgVecForm_Binary, 0, false},
	{"llvm.x86.sha1msg1",            TG_0F38,        0xC9, tgVecForm_Binary, 0, false},
	{"llvm.x86.sha1msg2",            TG_0F38,        0xCA, tgVecForm_Binary, 0, false},
	{"llvm.x86.sha1nexte",           TG_0F38,        0xC8, tgVecForm_Binary, 0, false},
	{"llvm.x86.sha1rnds4",           TG_0F3A,        0xCC, tgVecForm_Binary, 0, true},
	{"llvm.x86.sha256msg1",          TG_0F38,        0xCC, tgVecForm_Binary, 0, false},
	{"llvm.x86.sha256msg2",          TG_0F38,        0xCD, tgVecForm_Binary, 0, false},
	{"llvm.x86.sha256rnds2",         TG_0F38,        0xCB, tgVecForm_Xmm0, 0, false},

	{"llvm.x86.aesni.aesenc",          TG_P66|TG_0F38, 0xDC, tgVecForm_Binary, 0, false},
	{"llvm.x86.aesni.aesenclast",      TG_P66|TG_0F38, 0xDD, tgVecForm_Binary, 0, false},
	{"llvm.x86.aesni.aesdec",          TG_P66|TG_0F38, 0xDE, tgVecForm_Binary, 0, false},
	{"llvm.x86.aesni.aesdeclast",      TG_P66|TG_0F38, 0xDF, tgVecForm_Binary, 0, false},
	{"llvm.x86.aesni.aesimc",          TG_P66|TG_0F38, 0xDB, tgVecForm_Unary, 0, false},
	{"llvm.x86.aesni.aeskeygenassist", TG_P66|TG_0F3A, 0xDF, tgVecForm_Unary, 0, true},
	{"llvm.x86.pclmulqdq",             TG_P66|TG_0F3A, 0x44, tgVecForm_Binary, 0, true},

	{"llvm.x86.sse.max.ss",   TG_PF3|TG_0F, 0x5F, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse.max.ps",   TG_0F,        0x5F, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse.min.ss",   TG_PF3|TG_0F, 0x5D, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse.min.ps",   TG_0F,        0x5D, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse.rcp.ss",   TG_PF3|TG_0F, 0x53, tgVecForm_Unary, 0, false},
	{"llvm.x86.sse.rcp.ps",   TG_0F,        0x53, tgVecForm_Unary, 0, false},
	{"llvm.x86.sse.rsqrt.ss", TG_PF3|TG_0F, 0x52, tgVecForm_Unary, 0, false},
	{"llvm.x86.sse.rsqrt.ps", TG_0F,        0x52, tgVecForm_Unary, 0, false},
	{"llvm.x86.sse.cmp.ss",   TG_PF3|TG_0F, 0xC2, tgVecForm_Binary, 0, true},
	{"llvm.x86.sse.cmp.ps",   TG_0F,        0xC2, tgVecForm_Binary, 0, true},

	{"llvm.x86.sse2.pavg.b",        TG_P66|TG_0F, 0xE0, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse2.pavg.w",        TG_P66|TG_0F, 0xE3, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse2.pmadd.wd",      TG_P66|TG_0F, 0xF5, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse2.pmulh.w",       TG_P66|TG_0F, 0xE5, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse2.pmulhu.w",      TG_P66|TG_0F, 0xE4, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse2.psad.bw",       TG_P66|TG_0F, 0xF6, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse2.psll.w",        TG_P66|TG_0F, 0xF1, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse2.psll.d",        TG_P66|TG_0F, 0xF2, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse2.psll.q",        TG_P66|TG_0F, 0xF3, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse2.psrl.w",        TG_P66|TG_0F, 0xD1, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse2.psrl.d",        TG_P66|TG_0F, 0xD2, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse2.psrl.q",        TG_P66|TG_0F, 0xD3, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse2.psra.w",        TG_P66|TG_0F, 0xE1, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse2.psra.d",        TG_P66|TG_0F, 0xE2, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse2.pslli.w",       TG_P66|TG_0F, 0x71, tgVecForm_ShiftImm, 6, true},
	{"llvm.x86.sse2.pslli.d",       TG_P66|TG_0F, 0x72, tgVecForm_ShiftImm, 6, true},
	{"llvm.x86.sse2.pslli.q",       TG_P66|TG_0F, 0x73, tgVecForm_ShiftImm, 6, true},
	{"llvm.x86.sse2.psrli.w",       TG_P66|TG_0F, 0x71, tgVecForm_ShiftImm, 2, true},
	{"llvm.x86.sse2.psrli.d",       TG_P66|TG_0F, 0x72, tgVecForm_ShiftImm, 2, true},
	{"llvm.x86.sse2.psrli.q",       TG_P66|TG_0F, 0x73, tgVecForm_ShiftImm, 2, true},
	{"llvm.x86.sse2.psrai.w",       TG_P66|TG_0F, 0x71, tgVecForm_ShiftImm, 4, true},
	{"llvm.x86.sse2.psrai.d",       TG_P66|TG_0F, 0x72, tgVecForm_ShiftImm, 4, true},
	{"llvm.x86.sse2.cvtps2dq",      TG_P66|TG_0F, 0x5B, tgVecForm_Unary, 0, false},
	{"llvm.x86.sse2.cvttps2dq",     TG_PF3|TG_0F, 0x5B, tgVecForm_Unary, 0, false},
	{"llvm.x86.sse2.cvtpd2ps",      TG_P66|TG_0F, 0x5A, tgVecForm_Unary, 0, false},
	{"llvm.x86.sse2.cvtsd2ss",      TG_PF2|TG_0F, 0x5A, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse2.cvtpd2dq",      TG_PF2|TG_0F, 0xE6, tgVecForm_Unary, 0, false},
	{"llvm.x86.sse2.cvttpd2dq",     TG_P66|TG_0F, 0xE6, tgVecForm_Unary, 0, false},
	{"llvm.x86.sse2.packsswb.128",  TG_P66|TG_0F, 0x63, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse2.packssdw.128",  TG_P66|TG_0F, 0x6B, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse2.packuswb.128",  TG_P66|TG_0F, 0x67, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse2.max.sd",        TG_PF2|TG_0F, 0x5F, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse2.max.pd",        TG_P66|TG_0F, 0x5F, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse2.min.sd",        TG_PF2|TG_0F, 0x5D, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse2.min.pd",        TG_P66|TG_0F, 0x5D, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse2.cmp.sd",        TG_PF2|TG_0F, 0xC2, tgVecForm_Binary, 0, true},
	{"llvm.x86.sse2.cmp.pd",        TG_P66|TG_0F, 0xC2, tgVecForm_Binary, 0, true},

	{"llvm.x86.sse3.addsub.ps", TG_PF2|TG_0F, 0xD0, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse3.addsub.pd", TG_P66|TG_0F, 0xD0, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse3.hadd.ps",   TG_PF2|TG_0F, 0x7C, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse3.hadd.pd",   TG_P66|TG_0F, 0x7C, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse3.hsub.ps",   TG_PF2|TG_0F, 0x7D, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse3.hsub.pd",   TG_P66|TG_0F, 0x7D, tgVecForm_Binary, 0, false},

	{"llvm.x86.ssse3.phadd.w.128",     TG_P66|TG_0F38, 0x01, tgVecForm_Binary, 0, false},
	{"llvm.x86.ssse3.phadd.d.128",     TG_P66|TG_0F38, 0x02, tgVecForm_Binary, 0, false},
	{"llvm.x86.ssse3.phadd.sw.128",    TG_P66|TG_0F38, 0x03, tgVecForm_Binary, 0, false},
	{"llvm.x86.ssse3.pmadd.ub.sw.128", TG_P66|TG_0F38, 0x04, tgVecForm_Binary, 0, false},
	{"llvm.x86.ssse3.phsub.w.128",     TG_P66|TG_0F38, 0x05, tgVecForm_Binary, 0, false},
	{"llvm.x86.ssse3.phsub.d.128",     TG_P66|TG_0F38, 0x06, tgVecForm_Binary, 0, false},
	{"llvm.x86.ssse3.phsub.sw.128",    TG_P66|TG_0F38, 0x07, tgVecForm_Binary, 0, false},
	{"llvm.x86.ssse3.psign.b.128",     TG_P66|TG_0F38, 0x08, tgVecForm_Binary, 0, false},
	{"llvm.x86.ssse3.psign.w.128",     TG_P66|TG_0F38, 0x09, tgVecForm_Binary, 0, false},
	{"llvm.x86.ssse3.psign.d.128",     TG_P66|TG_0F38, 0x0A, tgVecForm_Binary, 0, false},
	{"llvm.x86.ssse3.pmul.hr.sw.128",  TG_P66|TG_0F38, 0x0B, tgVecForm_Binary, 0, false},

	{"llvm.x86.sse41.pblendvb",   TG_P66|TG_0F38, 0x10, tgVecForm_Xmm0, 0, false},
	{"llvm.x86.sse41.blendvps",   TG_P66|TG_0F38, 0x14, tgVecForm_Xmm0, 0, false},
	{"llvm.x86.sse41.blendvpd",   TG_P66|TG_0F38, 0x15, tgVecForm_Xmm0, 0, false},
	{"llvm.x86.sse41.packusdw",   TG_P66|TG_0F38, 0x2B, tgVecForm_Binary, 0, false},
	{"llvm.x86.sse41.phminposuw", TG_P66|TG_0F38, 0x41, tgVecForm_Unary, 0, false},
	{"llvm.x86.sse41.round.ps",   TG_P66|TG_0F3A, 0x08, tgVecForm_Unary,  0, true},
	{"llvm.x86.sse41.round.pd",   TG_P66|TG_0F3A, 0x09, tgVecForm_Unary,  0, true},
	{"llvm.x86.sse41.round.ss",   TG_P66|TG_0F3A, 0x0A, tgVecForm_Binary, 0, true},
	{"llvm.x86.sse41.round.sd",   TG_P66|TG_0F3A, 0x0B, tgVecForm_Binary, 0, true},
	{"llvm.x86.sse41.insertps",   TG_P66|TG_0F3A, 0x21, tgVecForm_Binary, 0, true},
	{"llvm.x86.sse41.dpps",       TG_P66|TG_0F3A, 0x40, tgVecForm_Binary, 0, true},
	{"llvm.x86.sse41.dppd",       TG_P66|TG_0F3A, 0x41, tgVecForm_Binary, 0, true},
	{"llvm.x86.sse41.mpsadbw",    TG_P66|TG_0F3A, 0x42, tgVecForm_Binary, 0, true},

	{"llvm.x86.sse.movmsk.ps",       TG_0F,              0x50, tgVecForm_ToGpr, 0, false},
	{"llvm.x86.sse2.movmsk.pd",      TG_P66|TG_0F,       0x50, tgVecForm_ToGpr, 0, false},
	{"llvm.x86.sse2.pmovmskb.128",   TG_P66|TG_0F,       0xD7, tgVecForm_ToGpr, 0, false},
	{"llvm.x86.sse.cvtss2si",        TG_PF3|TG_0F,       0x2D, tgVecForm_ToGpr, 0, false},
	{"llvm.x86.sse.cvttss2si",       TG_PF3|TG_0F,       0x2C, tgVecForm_ToGpr, 0, false},
	{"llvm.x86.sse.cvtss2si64",      TG_PF3|TG_0F|TG_W,  0x2D, tgVecForm_ToGpr, 0, false},
	{"llvm.x86.sse.cvttss2si64",     TG_PF3|TG_0F|TG_W,  0x2C, tgVecForm_ToGpr, 0, false},
	{"llvm.x86.sse2.cvtsd2si",       TG_PF2|TG_0F,       0x2D, tgVecForm_ToGpr, 0, false},
	{"llvm.x86.sse2.cvttsd2si",      TG_PF2|TG_0F,       0x2C, tgVecForm_ToGpr, 0, false},
	{"llvm.x86.sse2.cvtsd2si64",     TG_PF2|TG_0F|TG_W,  0x2D, tgVecForm_ToGpr, 0, false},
	{"llvm.x86.sse2.cvttsd2si64",    TG_PF2|TG_0F|TG_W,  0x2C, tgVecForm_ToGpr, 0, false},

	// the conditions LLVM's X86 lowering picks for each predicate
	{"llvm.x86.sse.comieq.ss",    TG_0F,        0x2F, tgVecForm_Flags, CC_E|tgVecCond_AndNP, false},
	{"llvm.x86.sse.comineq.ss",   TG_0F,        0x2F, tgVecForm_Flags, CC_NE|tgVecCond_OrP, false},
	{"llvm.x86.sse.comigt.ss",    TG_0F,        0x2F, tgVecForm_Flags, CC_A, false},
	{"llvm.x86.sse.comige.ss",    TG_0F,        0x2F, tgVecForm_Flags, CC_AE, false},
	{"llvm.x86.sse.comilt.ss",    TG_0F,        0x2F, tgVecForm_Flags, CC_A|tgVecCond_Swap, false},
	{"llvm.x86.sse.comile.ss",    TG_0F,        0x2F, tgVecForm_Flags, CC_AE|tgVecCond_Swap, false},
	{"llvm.x86.sse.ucomieq.ss",   TG_0F,        0x2E, tgVecForm_Flags, CC_E|tgVecCond_AndNP, false},
	{"llvm.x86.sse.ucomineq.ss",  TG_0F,        0x2E, tgVecForm_Flags, CC_NE|tgVecCond_OrP, false},
	{"llvm.x86.sse.ucomigt.ss",   TG_0F,        0x2E, tgVecForm_Flags, CC_A, false},
	{"llvm.x86.sse.ucomige.ss",   TG_0F,        0x2E, tgVecForm_Flags, CC_AE, false},
	{"llvm.x86.sse.ucomilt.ss",   TG_0F,        0x2E, tgVecForm_Flags, CC_A|tgVecCond_Swap, false},
	{"llvm.x86.sse.ucomile.ss",   TG_0F,        0x2E, tgVecForm_Flags, CC_AE|tgVecCond_Swap, false},
	{"llvm.x86.sse2.comieq.sd",   TG_P66|TG_0F, 0x2F, tgVecForm_Flags, CC_E|tgVecCond_AndNP, false},
	{"llvm.x86.sse2.comineq.sd",  TG_P66|TG_0F, 0x2F, tgVecForm_Flags, CC_NE|tgVecCond_OrP, false},
	{"llvm.x86.sse2.comigt.sd",   TG_P66|TG_0F, 0x2F, tgVecForm_Flags, CC_A, false},
	{"llvm.x86.sse2.comige.sd",   TG_P66|TG_0F, 0x2F, tgVecForm_Flags, CC_AE, false},
	{"llvm.x86.sse2.comilt.sd",   TG_P66|TG_0F, 0x2F, tgVecForm_Flags, CC_A|tgVecCond_Swap, false},
	{"llvm.x86.sse2.comile.sd",   TG_P66|TG_0F, 0x2F, tgVecForm_Flags, CC_AE|tgVecCond_Swap, false},
	{"llvm.x86.sse2.ucomieq.sd",  TG_P66|TG_0F, 0x2E, tgVecForm_Flags, CC_E|tgVecCond_AndNP, false},
	{"llvm.x86.sse2.ucomineq.sd", TG_P66|TG_0F, 0x2E, tgVecForm_Flags, CC_NE|tgVecCond_OrP, false},
	{"llvm.x86.sse2.ucomigt.sd",  TG_P66|TG_0F, 0x2E, tgVecForm_Flags, CC_A, false},
	{"llvm.x86.sse2.ucomige.sd",  TG_P66|TG_0F, 0x2E, tgVecForm_Flags, CC_AE, false},
	{"llvm.x86.sse2.ucomilt.sd",  TG_P66|TG_0F, 0x2E, tgVecForm_Flags, CC_A|tgVecCond_Swap, false},
	{"llvm.x86.sse2.ucomile.sd",  TG_P66|TG_0F, 0x2E, tgVecForm_Flags, CC_AE|tgVecCond_Swap, false},
	{"llvm.x86.sse41.ptestz",     TG_P66|TG_0F38, 0x17, tgVecForm_Flags, CC_E, false},
	{"llvm.x86.sse41.ptestc",     TG_P66|TG_0F38, 0x17, tgVecForm_Flags, CC_B, false},
	{"llvm.x86.sse41.ptestnzc",   TG_P66|TG_0F38, 0x17, tgVecForm_Flags, CC_A, false},

	{"llvm.x86.rdtscp", 0, 0, tgVecForm_Rdtscp, 0, false},
};

// tgOp_Vec128 keeps the index in its u8 aux
typedef char tg_vec_intrinsics_fit_in_u8[tg_count_of(tg_vec_intrinsics) <= 256 ? 1 : -1];

TG_INTERNAL i32 tg_vec_intrinsic_index(tgString name) {
	for (isize i = 0; i < tg_count_of(tg_vec_intrinsics); i++) {
		if (tg_str_eq(name, tg_str(tg_vec_intrinsics[i].name))) {
			return (i32)i;
		}
	}
	return -1;
}
