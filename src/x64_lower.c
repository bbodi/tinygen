// Lowering: tg IR -> x86-64 machine code.
//
// Registers come from tg_alloc_regs (analysis.c). An interval takes rsi, rdi, r8, r9 or
// xmm4-xmm15 when no call happens in it, rbx or r12-r15 otherwise. On Win64 rsi, rdi and
// xmm6-xmm15 are callee saved too, a float interval with a call in it takes one of the latter.
// The rest live in 8 byte stack slots. A constant with one definition is materialized at each
// use, or becomes the instruction's immediate. A compare whose only use is the next branch,
// select or test against zero only sets the flags.
//
// The scratch registers are rax, rcx, rdx, r10 and r11 (r11 for addresses), and xmm0-xmm3.
// Copies and sets of a size known at run time call small helpers that only write scratch registers.
//
// Frame:
//   [rbp+16 ...]  incoming stack arguments
//   [rbp+8]       return address
//   [rbp]         saved rbp
//   [rbp-N ...]   saved callee saved registers, vreg slots, then locals
//   [rsp ...]     outgoing stack arguments
// Win64 pushes its callee saved registers above rbp instead, see x64_win64.c.

// Finds or adds the module symbol `name`, returns its index. Defined by module.c.
TG_INTERNAL i32 tg_symbol(tgModule *m, tgString name);

enum {
	X64_F0 = 0, // float scratch registers
	X64_F1 = 1,
	X64_F2 = 2,
	X64_F3 = 3,
};

// Fixed size copies up to this size are inline loads and stores.
enum { X64_INLINE_COPY = 64 };

#define TG_SWAP(T, a, b) do { T tg__swap_tmp = (a); (a) = (b); (b) = tg__swap_tmp; } while (0)

// xb_lt_align_formula, which lived in x64_abi.cpp: off rounded up to a multiple of a.
TG_INTERNAL i64 x64_align_formula(i64 off, i64 a) {
	return (off + a - 1) / a * a;
}

typedef struct tgLowerFixup { i64 at; i32 block; } tgLowerFixup;
TG_ARRAY(tgLowerFixupArray, tgLowerFixup);

// what the previous instruction stored, still in a register for a load right after it
typedef struct tgLowerStored { bool valid; bool fp; u8 reg; i32 size; tgMem mem; bool fresh; } tgLowerStored;

typedef struct tgLower {
	tgProc *     p;
	tgLowerOut * out;
	tgAsm        a;
	tgI32Array   slot;     // vreg -> rbp offset (negative), when it has no register
	tgI32Array   uses;     // vreg -> number of reads
	tgI8Array    reg;      // vreg -> the register holding it for its whole life, or TG_NOREG
	tgBytes      is_const; // vreg -> defined once by an IConst, materialized at its uses
	tgI64Array   cval;
	tgBytes      in_block; // vreg -> defined once, and only read later in the same block
	tgI8Array    local_reg;
	tgBytes      remat;    // vreg -> defined once as a frame address, computed at each use from rmem
	tgMemArray   rmem;
	tgBytes      zx;       // vreg -> n when its register is known zero above its low n bytes, else 0
	u32          used_x;   // the gprs tg_alloc_regs handed out
	i32          frame_size;
	i32          max_call_stack;
	tgLowerFixupArray fixups;
	i64          proc_start;

	u8           saved[8];      // callee saved gprs the prologue saves, beyond rbp and Win64's rsi and rdi
	i32          saved_count;
	i32          save_offset[8]; // SysV: their rbp offsets
	i32          incoming_base;  // rbp offset of the incoming stack arguments
	u8           saved_v[10];    // Win64: callee saved xmm registers the prologue saves
	i32          saved_v_count;
	i32          win_area;       // Win64: bytes between rbp and the pushes, the pad and the xmm saves

	i32          next_block;  // the block placed right after the current branch, or -1
	bool         fuse;        // the current compare only sets the flags for the next instruction
	u32          flags_vreg;  // the compare result the flags hold
	tgCC         flags_cc;
	tgLowerStored stored;
} tgLower;

// Defined by x64_win64.c, which follows this file in the unity build.
TG_INTERNAL void   tg_win64_prologue(tgLower *L, tgProcDebug *dbg);
TG_INTERNAL void   tg_win64_epilogue(tgLower *L);
TG_INTERNAL tgOpnd tg_win64_tls_opnd(tgLower *L, tgMem m, u8 scratch);
TG_INTERNAL tgOpnd tg_win64_import_opnd(tgLower *L, tgMem m, u8 scratch);

// Defined by x64_asm.c, ported later; see the TODO(port) section at the end of this file.
TG_INTERNAL bool tg_asm_layout(tgAsmItemSlice items, tgU8Slice pool, i32 label_count, i64 start, tgBytes *code);

// The hardware number of a gpr to its DWARF number.
TG_INTERNAL u8 x64_dwarf_reg(u8 r) {
	static u8 const map[16] = {0, 2, 1, 3, 7, 6, 4, 5, 8, 9, 10, 11, 12, 13, 14, 15};
	return map[r & 15];
}

// Whether the lowering of `in` calls something or writes registers that intervals take.
TG_INTERNAL bool x64_clobbers(tgInstr const *in) {
	switch (in->op) {
	case tgOp_Call:
	case tgOp_Ret:
	case tgOp_Syscall:
	case tgOp_TlsAddr:
	case tgOp_Asm:
		return true;
	}
	return false;
}

typedef struct x64RegTarget {
	u32 caller_x, caller_v;
} x64RegTarget;

TG_INTERNAL bool x64_target_clobbers(void *ctx, tgInstr const *in) {
	(void)ctx;
	return x64_clobbers(in);
}

TG_INTERNAL void x64_target_wants(void *ctx, tgProc *p, tgInstr const *in, tgNoteWantProc note, void *note_ctx) {
	x64RegTarget const *t = (x64RegTarget const *)ctx;
	if (in->op != tgOp_Call && in->op != tgOp_Syscall) return;
	tgCall const *c = &p->calls.data[(isize)in->imm];
	for (isize i = 0; i < c->args.count; i++) {
		tgCallArg const *arg = &c->args.data[i];
		if (arg->kind == tgCallArg_Gpr && (t->caller_x & (1u << arg->reg))) note(note_ctx, arg->vreg, arg->reg);
		if (arg->kind == tgCallArg_Xmm && (t->caller_v & (1u << arg->reg)) && tg_type_is_float(arg->type)) note(note_ctx, arg->vreg, TG_FREG + arg->reg);
	}
}

// an asm template's operands move between fixed registers and their slots
TG_INTERNAL void x64_target_pins(void *ctx, tgProc *p, tgInstr const *in, tgPinProc pin, void *pin_ctx) {
	(void)ctx;
	if (in->op != tgOp_Asm) return;
	tgAsmBlock const *blk = &p->asms.data[(isize)in->imm];
	for (isize i = 0; i < blk->inputs.count; i++) pin(pin_ctx, blk->inputs.data[i].vreg);
	for (isize i = 0; i < blk->outputs.count; i++) pin(pin_ctx, blk->outputs.data[i].vreg);
}

TG_INTERNAL void tg_lower_layout(tgLower *L) {
	tgProc *p = L->p;
	bool win = tg_is_win64();
	u32 const rbx_r12_r15 = (1u << RBX) | (1u << R12) | (1u << R13) | (1u << R14) | (1u << R15);
	u32 const si_di = (1u << RSI) | (1u << RDI);
	u32 const r8_r9 = (1u << R8) | (1u << R9);
	tgRegPools pools;
	memset(&pools, 0, sizeof(pools));
	if (win) {
		pools.caller_x = r8_r9;
		pools.callee_x = rbx_r12_r15 | si_di;
		pools.caller_v = 0x0030;
		pools.callee_v = 0xffc0;
		pools.callee_weight = 3;
	} else {
		pools.caller_x = r8_r9 | si_di;
		pools.callee_x = rbx_r12_r15;
		pools.caller_v = 0xfff0;
		pools.callee_v = 0;
		pools.callee_weight = 3;
	}
	if (p->naked) memset(&pools, 0, sizeof(pools));
	tgRegAlloc R;
	memset(&R, 0, sizeof(R));
	x64RegTarget xt;
	xt.caller_x = pools.caller_x;
	xt.caller_v = pools.caller_v;
	tgRegTarget target;
	target.ctx = &xt;
	target.clobbers = x64_target_clobbers;
	target.wants = x64_target_wants;
	target.pins = x64_target_pins;
	target.reads_first = NULL;
	tg_alloc_regs(p, &R, pools, &target);
	L->uses      = R.uses;
	L->reg       = R.reg;
	L->is_const  = R.is_const;
	L->cval      = R.cval;
	L->in_block  = R.in_block;
	L->local_reg = R.local_reg;
	L->remat     = R.remat;
	L->rmem      = R.rmem;
	L->used_x    = R.used_x;
	L->max_call_stack = R.max_call_stack;
	tg_array_free(&R.clean);
	tg_array_free(&R.via);

	// A narrow load fills its register zero extended, and a zext leaves it zero above the source.
	tg_array_make(&L->zx, p->vregs.count);
	for (isize bi = 0; bi < p->order.count; bi++) {
		tgBlock *b = p->order.data[bi];
		for (isize ii = 0; ii < b->instrs.count; ii++) {
			tgInstr const *in = &b->instrs.data[ii];
			if (in->dst == 0 || R.facts.data[in->dst].defs != 1) continue;
			if (in->op == tgOp_Load && tg_type_is_int(in->type) && tg_type_size(in->type) < 8) {
				L->zx.data[in->dst] = (u8)tg_type_size(in->type);
			} else if (in->op == tgOp_Zext && tg_type_size((tgType)in->aux) < 8) {
				L->zx.data[in->dst] = (u8)tg_type_size((tgType)in->aux);
			}
		}
	}

	// the callee saved registers in use, saved in this order
	L->saved_count = 0;
	u8 const order[5] = {RBX, R12, R13, R14, R15};
	for (i32 i = 0; i < 5; i++) {
		u8 r = order[i];
		if (R.used_x & (1u << r)) L->saved[L->saved_count++] = r;
	}
	L->saved_v_count = 0;
	if (win) {
		for (u8 r = 6; r < 16; r++) {
			if (R.used_v & (1u << r)) L->saved_v[L->saved_v_count++] = r;
		}
	}
	i32 cur = 0;
	if (!win) {
		for (i32 i = 0; i < L->saved_count; i++) {
			cur += 8;
			L->save_offset[i] = -cur;
		}
	}

	// vreg slots nearest rbp, where one byte offsets reach them
	tg_alloc_slots(p, &R, &cur);
	L->slot = R.slot;

	for (isize i = 0; i < p->locals.count; i++) {
		tgLocal *l = &p->locals.data[i];
		if (L->local_reg.data[i] >= 0) {
			l->frame_offset = 0; // it lives in its register
			continue;
		}
		if (l->over_align > 16) {
			// the frame is only 16 byte aligned, the prologue picks an aligned spot in the raw area
			cur = (i32)x64_align_formula(cur + l->size + l->over_align - 16, 16);
			l->raw_offset = -cur;
			cur = (i32)x64_align_formula(cur + 8, 8);
			l->frame_offset = -cur;
			continue;
		}
		cur = (i32)x64_align_formula(cur + l->size, l->align);
		l->frame_offset = -cur;
	}

	// the outgoing argument area: with over-aligned stack arguments, its alignment, so the
	// allocas above it, see tgOp_Alloca, stay clear of the frame
	L->frame_size = (i32)x64_align_formula(cur, 16) + (i32)x64_align_formula(L->max_call_stack, tg_max(16, p->call_align));
}

////////////////////////////////////////////////////////////////
// Operands
////////////////////////////////////////////////////////////////

// v's value of `size` bytes extended as `ext` asks
TG_INTERNAL i64 x64_ext_value(i64 v, i32 size, tgExtKind ext) {
	if (size >= 8 || ext == tgExt_None) return v;
	u64 mask = (1ull << (8*size)) - 1;
	u64 u = (u64)v & mask;
	if (ext == tgExt_Sign && (u >> (8*size - 1))) u |= ~mask;
	return (i64)u;
}

TG_INTERNAL bool x64_is_const(tgLower *L, u32 v, i64 *value) {
	if (v == 0 || !L->is_const.data[v]) return false;
	*value = L->cval.data[v];
	return true;
}

// v as the sign extended imm32 of a `size` byte operation
TG_INTERNAL bool x64_imm(tgLower *L, u32 v, i32 size, i32 *imm) {
	i64 k = 0;
	if (!x64_is_const(L, v, &k)) return false;
	if (size >= 8 && (k < -0x80000000ll || k > 0x7fffffffll)) return false;
	*imm = (i32)k;
	return true;
}

TG_INTERNAL tgOpnd x64_slot(tgLower *L, u32 v) {
	TG_ASSERT(v != 0 && L->reg.data[v] == TG_NOREG && !L->is_const.data[v] && !L->remat.data[v]);
	return tg_m(RBP, L->slot.data[v]);
}

// mov dst, src extended from `size` bytes as `ext` asks; tgExt_None leaves the high bytes as they come
TG_INTERNAL void x64_extend(tgAsm *a, tgExtKind ext, i32 size, u8 dst, u8 src) {
	if (size >= 8 || ext == tgExt_None) {
		if (dst != src) tg_mov_r_rm(a, 8, dst, tg_r(src));
		return;
	}
	tg_load_ext(a, size, ext == tgExt_Sign, dst, tg_r(src));
}

TG_INTERNAL void x64_movaps(tgAsm *a, u8 x, u8 y) {
	if (x != y) tg_enc(a, TG_0F, 0x28, x, tg_r(y), 0);
}

// A machine memory operand for an IR memory reference. May clobber `scratch`.
TG_INTERNAL tgOpnd tg_mem_opnd(tgLower *L, tgMem m, u8 scratch);

// A gpr holding v: its own, or `scratch` loaded with it. Narrow values come extended as `ext`
// asks; with tgExt_None only the low `size` bytes mean anything.
TG_INTERNAL u8 x64_src(tgLower *L, u32 v, u8 scratch, i32 size, tgExtKind ext) {
	TG_ASSERT(v != 0);
	tgAsm *a = &L->a;
	bool extend = size < 8 && ext != tgExt_None;
	i64 k = 0;
	if (x64_is_const(L, v, &k)) {
		u64 u = (u64)x64_ext_value(k, size, ext);
		if (!extend && size <= 4) u = (u32)u; // the short form
		tg_mov_r_imm(a, scratch, u);
		return scratch;
	}
	if (L->remat.data[v]) {
		tg_lea(a, scratch, tg_mem_opnd(L, L->rmem.data[v], scratch));
		return scratch;
	}
	i8 r = L->reg.data[v];
	if (r >= TG_FREG) {
		tg_movd_rm_x(a, 8, tg_r(scratch), (u8)(r - TG_FREG));
		return scratch;
	}
	if (r != TG_NOREG) {
		// zero above fewer bytes than `size`, it reads the same sign extended
		u8 zx = L->zx.data[v];
		if (!extend || (zx != 0 && (ext == tgExt_Zero ? zx <= size : zx < size))) return (u8)r;
		x64_extend(a, ext, size, scratch, (u8)r);
		return scratch;
	}
	tg_load_ext(a, tg_min(size, 8), ext == tgExt_Sign, scratch, x64_slot(L, v));
	return scratch;
}

// Like x64_src, into exactly `reg`.
TG_INTERNAL void x64_get(tgLower *L, u8 reg, u32 v, i32 size, tgExtKind ext) {
	u8 r = x64_src(L, v, reg, size, ext);
	if (r != reg) tg_mov_r_rm(&L->a, 8, reg, tg_r(r));
}

// v as an operand of a `size` byte instruction: its register or slot, or `scratch` loaded with it.
TG_INTERNAL tgOpnd x64_opnd(tgLower *L, u32 v, u8 scratch, i32 size) {
	bool computed = v == 0 || L->is_const.data[v] || L->remat.data[v];
	i8 r = !computed ? L->reg.data[v] : TG_NOREG;
	if (!computed && r == TG_NOREG) return x64_slot(L, v);
	if (r != TG_NOREG && r < TG_FREG) return tg_r((u8)r);
	return tg_r(x64_src(L, v, scratch, size, tgExt_None));
}

// An xmm register holding v's `size` byte float: its own, or `scratch` loaded with it.
TG_INTERNAL u8 x64_srcf(tgLower *L, u32 v, u8 scratch, i32 size) {
	TG_ASSERT(v != 0);
	tgAsm *a = &L->a;
	i64 k = 0;
	if (x64_is_const(L, v, &k)) {
		tg_mov_r_imm(a, R10, (u64)k);
		tg_movd_x_rm(a, 8, scratch, tg_r(R10));
		return scratch;
	}
	i8 r = L->reg.data[v];
	if (r >= TG_FREG) return (u8)(r - TG_FREG);
	if (r != TG_NOREG) {
		tg_movd_x_rm(a, 8, scratch, tg_r((u8)r));
		return scratch;
	}
	tg_movs_x_rm(a, size == 4 ? 4 : 8, scratch, x64_slot(L, v));
	return scratch;
}

TG_INTERNAL void x64_getf(tgLower *L, u8 x, u32 v, i32 size) {
	x64_movaps(&L->a, x, x64_srcf(L, v, x, size));
}

// v as the float operand of an sse instruction: its register or slot, or `scratch` loaded with it.
TG_INTERNAL tgOpnd x64_opndf(tgLower *L, u32 v, u8 scratch, i32 size) {
	if (v != 0 && !L->is_const.data[v] && L->reg.data[v] == TG_NOREG) return x64_slot(L, v);
	return tg_r(x64_srcf(L, v, scratch, size));
}

// The gpr to compute v into: its own, or `scratch`. x64_put finishes the definition.
TG_INTERNAL u8 x64_dst(tgLower *L, u32 v, u8 scratch) {
	i8 r = L->reg.data[v];
	return r != TG_NOREG && r < TG_FREG ? (u8)r : scratch;
}

TG_INTERNAL u8 x64_dstf(tgLower *L, u32 v, u8 scratch) {
	i8 r = L->reg.data[v];
	return r >= TG_FREG ? (u8)(r - TG_FREG) : scratch;
}

// Defines v from the gpr `reg`.
TG_INTERNAL void x64_put(tgLower *L, u32 v, u8 reg) {
	TG_ASSERT(v != 0 && !L->is_const.data[v]);
	tgAsm *a = &L->a;
	i8 r = L->reg.data[v];
	if (r >= TG_FREG) {
		tg_movd_x_rm(a, 8, (u8)(r - TG_FREG), tg_r(reg));
	} else if (r != TG_NOREG) {
		if (r != reg) tg_mov_r_rm(a, 8, (u8)r, tg_r(reg));
	} else {
		tg_mov_rm_r(a, 8, x64_slot(L, v), reg);
	}
}

// Defines v from the `size` byte float in the xmm register `x`.
TG_INTERNAL void x64_putf(tgLower *L, u32 v, u8 x, i32 size) {
	TG_ASSERT(v != 0 && !L->is_const.data[v]);
	tgAsm *a = &L->a;
	i8 r = L->reg.data[v];
	if (r >= TG_FREG) {
		x64_movaps(a, (u8)(r - TG_FREG), x);
	} else if (r != TG_NOREG) {
		tg_movd_rm_x(a, 8, tg_r((u8)r), x);
	} else {
		tg_movs_rm_x(a, size == 4 ? 4 : 8, x64_slot(L, v), x);
	}
}

// A machine memory operand for an IR memory reference. May clobber `scratch`.
TG_INTERNAL tgOpnd tg_mem_opnd(tgLower *L, tgMem m, u8 scratch) {
	tgAsm *a = &L->a;
	switch (m.kind) {
	case tgMem_Local: {
		TG_ASSERT(L->local_reg.data[m.base] < 0);
		tgLocal const *l = &L->p->locals.data[m.base];
		if (l->over_align > 16) {
			tg_mov_r_rm(a, 8, scratch, tg_m(RBP, l->frame_offset));
			return tg_m(scratch, (i32)m.offset);
		}
		return tg_m(RBP, l->frame_offset + (i32)m.offset);
	}
	case tgMem_Incoming:
		return tg_m(RBP, L->incoming_base + (i32)m.offset);
	case tgMem_Reg:
		if (L->remat.data[m.base]) {
			// the frame address folds into the access
			tgMem f = L->rmem.data[m.base];
			f.offset += m.offset;
			return tg_mem_opnd(L, f, scratch);
		}
		return tg_m(x64_src(L, m.base, scratch, 8, tgExt_None), (i32)m.offset);
	case tgMem_Sym: {
		u8 flags = tg_lower_sym_flags(L->p->m, m.base);
		if ((flags & tgSymbolFlag_TLS) && tg_is_win64()) {
			return tg_win64_tls_opnd(L, m, scratch);
		}
		if ((flags & tgSymbolFlag_Foreign) && (flags & tgSymbolFlag_Undef) && tg_is_win64()) {
			if (flags & tgSymbolFlag_Func) return tg_m_sym((i32)m.base, (i32)m.offset, tgReloc_PC32);
			return tg_win64_import_opnd(L, m, scratch);
		}
		if (flags & tgSymbolFlag_TLS) {
			TG_ASSERT_MSG(!tg_is_darwin(), "a thread local symbol is reached through tgOp_TlsAddr");
			// initial exec: the thread pointer plus the variable's offset from the GOT
			// mov scratch, fs:[0]
			tg_b(a, 0x64);
			tg_b(a, (u8)(0x48 | ((scratch & 8) ? 4 : 0)));
			tg_b(a, 0x8B);
			tg_b(a, (u8)(0x04 | ((scratch & 7) << 3)));
			tg_b(a, 0x25);
			tg_u32(a, 0);
			// add scratch, [rip + sym@GOTTPOFF]
			tg_enc(a, TG_W, 0x03, scratch, tg_m_sym((i32)m.base, 0, tgReloc_GOTTPOFF), 0);
			return tg_m(scratch, (i32)m.offset);
		}
		// Windows has no GOT: dllexport does not make a symbol preemptible
		bool preemptible = (flags & tgSymbolFlag_Export) && tg_is_pic() && !tg_is_win64();
		if (((flags & tgSymbolFlag_Foreign) && (flags & tgSymbolFlag_Undef)) || preemptible) {
			// mov scratch, [rip + sym@GOTPCREL]
			tg_enc(a, TG_W, 0x8B, scratch, tg_m_sym((i32)m.base, 0, tgReloc_REX_GOTPCRELX), 0);
			return tg_m(scratch, (i32)m.offset);
		}
		return tg_m_sym((i32)m.base, (i32)m.offset, tgReloc_PC32);
	}
	}
	TG_PANIC("bad mem");
	return tg_r(0);
}

// A memory operand that takes no rip relative form: a symbol's address goes in `scratch` first.
TG_INTERNAL tgOpnd x64_mem_base(tgLower *L, tgMem m, u8 scratch) {
	tgOpnd o = tg_mem_opnd(L, m, scratch);
	if (o.reg == TG_RIP) {
		tg_lea(&L->a, scratch, o);
		o = tg_m(scratch, 0);
	}
	return o;
}

TG_INTERNAL void tg_load_xmm(tgLower *L, u8 x, tgOpnd m, i32 size) {
	tgAsm *a = &L->a;
	switch (size) {
	case 2:
		tg_load_ext(a, 2, false, R10, m);
		tg_movd_x_rm(a, 4, x, tg_r(R10));
		break;
	case 4: tg_movs_x_rm(a, 4, x, m); break;
	case 6: {
		// three f16s: movss the low four bytes, then pinsrw x, [m+4], 2
		tg_movs_x_rm(a, 4, x, m);
		tgOpnd m2 = m;
		m2.disp += 4;
		tg_enc(a, TG_P66|TG_0F, 0xC4, x, m2, 1);
		tg_b(a, 2);
		break;
	}
	case 8: tg_movs_x_rm(a, 8, x, m); break;
	case 16: tg_movups_x_m(a, x, m); break;
	case 32: case 64: tg_vmovups_wide(a, size, true, x, m); break;
	default:
		TG_PANIC("bad xmm load size");
	}
}

TG_INTERNAL void tg_store_xmm(tgLower *L, tgOpnd m, u8 x, i32 size) {
	tgAsm *a = &L->a;
	switch (size) {
	case 2:
		tg_movd_rm_x(a, 4, tg_r(R10), x);
		tg_mov_rm_r(a, 2, m, R10);
		break;
	case 4: tg_movs_rm_x(a, 4, m, x); break;
	case 6: {
		tg_movs_rm_x(a, 4, m, x);
		// pextrw r10d, x, 2
		tg_enc(a, TG_P66|TG_0F, 0xC5, R10, tg_r(x), 0);
		tg_b(a, 2);
		tgOpnd m2 = m;
		m2.disp += 4;
		tg_mov_rm_r(a, 2, m2, R10);
		break;
	}
	case 8: tg_movs_rm_x(a, 8, m, x); break;
	case 16: tg_movups_m_x(a, m, x); break;
	case 32: case 64: tg_vmovups_wide(a, size, false, x, m); break;
	default:
		TG_PANIC("bad xmm store size");
	}
}

// Loads `size` (1..8) bytes from memory into a gpr, zero extended.
TG_INTERNAL void tg_load_bytes_gpr(tgLower *L, u8 reg, tgOpnd m, i32 size, tgExtKind ext) {
	tgAsm *a = &L->a;
	switch (size) {
	case 1: case 2: case 4: case 8:
		tg_load_ext(a, size, ext == tgExt_Sign, reg, m);
		return;
	}
	// odd sizes: assemble from pieces, high part first
	TG_ASSERT(m.is_mem && m.reg != TG_RIP);
	TG_ASSERT(reg != R10 && m.reg != R10);
	i32 lo = size > 4 ? 4 : 2;
	tg_load_ext(a, lo, false, reg, m);
	i32 rest = size - lo;
	tgOpnd m2 = m;
	m2.disp += lo;
	if (rest == 1 || rest == 2 || rest == 4) {
		tg_load_ext(a, rest, false, R10, m2);
	} else {
		// rest == 3: two bytes then one
		tg_load_ext(a, 2, false, R10, m2);
		tgOpnd m3 = m;
		m3.disp += lo + 2;
		tg_push(a, reg);
		tg_load_ext(a, 1, false, reg, m3);
		tg_shift_imm(a, 4, 8, tg_r(reg), 16);
		tg_alu_r_rm(a, ALU_OR, 8, R10, tg_r(reg));
		tg_pop(a, reg);
	}
	tg_shift_imm(a, 4, 8, tg_r(R10), (u8)(8*lo));
	tg_alu_r_rm(a, ALU_OR, 8, reg, tg_r(R10));
}

// Stores the low `size` (1..8) bytes of a gpr. Clobbers the register for odd sizes.
TG_INTERNAL void tg_store_bytes_gpr(tgLower *L, tgOpnd m, u8 reg, i32 size) {
	tgAsm *a = &L->a;
	switch (size) {
	case 1: case 2: case 4: case 8:
		tg_mov_rm_r(a, size, m, reg);
		return;
	}
	i32 off = 0;
	while (size > 0) {
		i32 chunk = size >= 4 ? 4 : (size >= 2 ? 2 : 1);
		tgOpnd mm = m;
		mm.disp += off;
		tg_mov_rm_r(a, chunk, mm, reg);
		tg_shift_imm(a, 5, 8, tg_r(reg), (u8)(8*chunk));
		off += chunk;
		size -= chunk;
	}
}

TG_INTERNAL tgCC tg_cc_for(tgCond c) {
	switch (c) {
	case tgCond_EQ:  return CC_E;
	case tgCond_NE:  return CC_NE;
	case tgCond_SLT: return CC_L;
	case tgCond_SLE: return CC_LE;
	case tgCond_SGT: return CC_G;
	case tgCond_SGE: return CC_GE;
	case tgCond_ULT: return CC_B;
	case tgCond_ULE: return CC_BE;
	case tgCond_UGT: return CC_A;
	case tgCond_UGE: return CC_AE;
	}
	TG_PANIC("bad cond");
	return CC_E;
}


typedef struct x64CopyChunk { i32 off; i32 size; } x64CopyChunk;

TG_INTERNAL void x64_copy_add(x64CopyChunk *chunks, i32 *count, i64 off, i32 size) {
	x64CopyChunk c;
	c.off = (i32)off;
	c.size = size;
	chunks[(*count)++] = c;
}

// Copies n bytes from [src] to [dst] with xmm0-xmm3 and rax, rcx, loading everything before
// storing anything, so the ranges may overlap. n <= X64_INLINE_COPY.
TG_INTERNAL void x64_copy_small(tgLower *L, tgOpnd dst, tgOpnd src, i64 n) {
	tgAsm *a = &L->a;
	TG_ASSERT(n <= X64_INLINE_COPY && dst.reg != TG_RIP && src.reg != TG_RIP);
	x64CopyChunk chunks[4];
	memset(chunks, 0, sizeof(chunks));
	i32 count = 0;
	if (n >= 16) {
		// whole xmm chunks, the last one overlapping the one before
		for (i64 off = 0; off + 16 <= n; off += 16) x64_copy_add(chunks, &count, off, 16);
		if (n % 16) x64_copy_add(chunks, &count, n - 16, 16);
	} else if (n >= 8) {
		x64_copy_add(chunks, &count, 0, 8);
		if (n > 8) x64_copy_add(chunks, &count, n - 8, 8);
	} else if (n >= 4) {
		x64_copy_add(chunks, &count, 0, 4);
		if (n > 4) x64_copy_add(chunks, &count, n - 4, 4);
	} else if (n >= 2) {
		x64_copy_add(chunks, &count, 0, 2);
		if (n > 2) x64_copy_add(chunks, &count, n - 2, 2);
	} else if (n == 1) {
		x64_copy_add(chunks, &count, 0, 1);
	}
	u8 const gprs[2] = {RAX, RCX};
	for (i32 i = 0; i < count; i++) {
		tgOpnd s = src;
		s.disp += chunks[i].off;
		if (chunks[i].size == 16) tg_movups_x_m(a, (u8)(X64_F0 + i), s);
		else                      tg_load_ext(a, chunks[i].size, false, gprs[i], s);
	}
	for (i32 i = 0; i < count; i++) {
		tgOpnd d = dst;
		d.disp += chunks[i].off;
		if (chunks[i].size == 16) tg_movups_m_x(a, d, (u8)(X64_F0 + i));
		else                      tg_mov_rm_r(a, chunks[i].size, d, gprs[i]);
	}
}

// Copies n bytes from [r11] to [r10], ranges apart. Moves both pointers and clobbers rcx, xmm0.
TG_INTERNAL void x64_copy_fwd(tgLower *L, i64 n) {
	tgAsm *a = &L->a;
	if (n <= X64_INLINE_COPY) {
		x64_copy_small(L, tg_m(R10, 0), tg_m(R11, 0), n);
		return;
	}
	// 16 bytes a round, then the last 16 bytes, overlapping
	tg_mov_r_imm(a, RCX, (u64)(n / 16));
	i64 loop = tg_pos(a);
	tg_movups_x_m(a, X64_F0, tg_m(R11, 0));
	tg_movups_m_x(a, tg_m(R10, 0), X64_F0);
	tg_alu_rm_imm(a, ALU_ADD, 8, tg_r(R11), 16);
	tg_alu_rm_imm(a, ALU_ADD, 8, tg_r(R10), 16);
	tg_alu_rm_imm(a, ALU_SUB, 4, tg_r(RCX), 1);
	i64 j = tg_jcc32(a, CC_NE);
	tg_patch_rel32(a, j, loop);
	if (n % 16) {
		i32 back = (i32)(n % 16) - 16;
		tg_movups_x_m(a, X64_F0, tg_m(R11, back));
		tg_movups_m_x(a, tg_m(R10, back), X64_F0);
	}
}

// The helpers of x64_helper.
TG_INTERNAL void x64_helper_here(tgAsm *a, i64 j) { tg_patch_rel32(a, j, tg_pos(a)); }
TG_INTERNAL void x64_helper_cmp_n(tgAsm *a, i32 k) { tg_alu_rm_imm(a, ALU_CMP, 8, tg_r(RCX), k); }
// r = base + n, the end of a range
TG_INTERNAL void x64_helper_end_of(tgAsm *a, u8 r, u8 base) {
	tg_mov_r_rm(a, 8, r, tg_r(base));
	tg_alu_r_rm(a, ALU_ADD, 8, r, tg_r(RCX));
}
// memmove: the first and the last `c` bytes, overlapping in the middle, for n in [c, 2c]
TG_INTERNAL void x64_helper_move_pair(tgAsm *a, i32 c, i64 *skip) {
	*skip = tg_jcc32(a, CC_B);
	tg_load_ext(a, c, false, R10, tg_m(RDX, 0));
	x64_helper_end_of(a, R11, RDX);
	tg_load_ext(a, c, false, R11, tg_m(R11, -c));
	x64_helper_end_of(a, RDX, RAX);
	tg_mov_rm_r(a, c, tg_m(RAX, 0), R10);
	tg_mov_rm_r(a, c, tg_m(RDX, -c), R11);
	tg_ret(a);
}
// memset: the first and the last `c` bytes
TG_INTERNAL void x64_helper_set_pair(tgAsm *a, i32 c, i64 *skip) {
	*skip = tg_jcc32(a, CC_B);
	tg_mov_rm_r(a, c, tg_m(RDX, 0), RAX);
	tg_mov_rm_r(a, c, tg_m(R11, -c), RAX);
	tg_ret(a);
}

// The module's memmove(rax, rdx, rcx) and memset(rdx, al, rcx), for sizes known at run time.
// They write only the scratch registers, so the registers intervals hold live through a call.
// Sizes up to 64 take overlapping loads and stores, larger ones rep movsb or rep stosb, with
// rsi and rdi kept in r11 and r10.
TG_INTERNAL i32 x64_helper(tgModule *m, bool set) {
	i32 *at = set ? &m->x64_set_helper : &m->x64_move_helper;
	if (*at != 0) return *at - 1;
	tgAsm as;
	memset(&as, 0, sizeof(as));
	as.m = m;
	as.code = &m->sections[tgSection_Text];
	as.relocs = &m->relocs;
	tgAsm *a = &as;
	while (a->code->count % 16 != 0) tg_b(a, 0xCC);
	i64 start = tg_pos(a);
	i32 sym = tg_symbol(m, set ? tg_str_lit("__tg_memset") : tg_str_lit("__tg_memmove"));
	m->symbols.data[sym].section = tgSection_Text;
	m->symbols.data[sym].offset = start;
	// ELF names it for debuggers and profilers, weak in case two objects bring one
	m->symbols.data[sym].flags = tgSymbolFlag_Func;
	if (!tg_is_win64()) m->symbols.data[sym].flags |= tgSymbolFlag_Global|tgSymbolFlag_Weak|tgSymbolFlag_Hidden;
	*at = sym + 1;

	if (!set) {
		x64_helper_cmp_n(a, 16);
		i64 j_big = tg_jcc32(a, CC_A);
		i64 skip = 0;
		x64_helper_cmp_n(a, 8);  x64_helper_move_pair(a, 8, &skip); x64_helper_here(a, skip);
		x64_helper_cmp_n(a, 4);  x64_helper_move_pair(a, 4, &skip); x64_helper_here(a, skip);
		x64_helper_cmp_n(a, 2);  x64_helper_move_pair(a, 2, &skip); x64_helper_here(a, skip);
		x64_helper_cmp_n(a, 1);
		i64 j_none = tg_jcc32(a, CC_B);
		tg_load_ext(a, 1, false, R10, tg_m(RDX, 0));
		tg_mov_rm_r(a, 1, tg_m(RAX, 0), R10);
		x64_helper_here(a, j_none);
		tg_ret(a);
		x64_helper_here(a, j_big);
		x64_helper_cmp_n(a, 64);
		i64 j_rep = tg_jcc32(a, CC_A);
		// 17..64: the first and the last 16 bytes, and the 16 after and before them when n > 32
		tg_movups_x_m(a, X64_F0, tg_m(RDX, 0));
		x64_helper_end_of(a, R11, RDX);
		tg_movups_x_m(a, X64_F3, tg_m(R11, -16));
		x64_helper_cmp_n(a, 32);
		i64 j_wide = tg_jcc32(a, CC_A);
		x64_helper_end_of(a, R11, RAX);
		tg_movups_m_x(a, tg_m(RAX, 0), X64_F0);
		tg_movups_m_x(a, tg_m(R11, -16), X64_F3);
		tg_ret(a);
		x64_helper_here(a, j_wide);
		tg_movups_x_m(a, X64_F1, tg_m(RDX, 16));
		tg_movups_x_m(a, X64_F2, tg_m(R11, -32));
		x64_helper_end_of(a, R11, RAX);
		tg_movups_m_x(a, tg_m(RAX, 0), X64_F0);
		tg_movups_m_x(a, tg_m(RAX, 16), X64_F1);
		tg_movups_m_x(a, tg_m(R11, -32), X64_F2);
		tg_movups_m_x(a, tg_m(R11, -16), X64_F3);
		tg_ret(a);
		x64_helper_here(a, j_rep);
		tg_mov_r_rm(a, 8, R10, tg_r(RDI));
		tg_mov_r_rm(a, 8, R11, tg_r(RSI));
		tg_mov_r_rm(a, 8, RDI, tg_r(RAX));
		tg_mov_r_rm(a, 8, RSI, tg_r(RDX));
		// backwards when the destination is inside the source range
		tg_alu_r_rm(a, ALU_CMP, 8, RDI, tg_r(RSI));
		i64 j_fwd = tg_jcc32(a, CC_BE);
		x64_helper_end_of(a, RAX, RSI);
		tg_alu_r_rm(a, ALU_CMP, 8, RDI, tg_r(RAX));
		i64 j_fwd2 = tg_jcc32(a, CC_AE);
		tg_lea(a, RSI, tg_m(RSI, -1));
		tg_alu_r_rm(a, ALU_ADD, 8, RSI, tg_r(RCX));
		tg_lea(a, RDI, tg_m(RDI, -1));
		tg_alu_r_rm(a, ALU_ADD, 8, RDI, tg_r(RCX));
		tg_std(a);
		tg_rep_movsb(a);
		tg_cld(a);
		i64 j_done = tg_jmp32(a);
		x64_helper_here(a, j_fwd);
		x64_helper_here(a, j_fwd2);
		tg_rep_movsb(a);
		x64_helper_here(a, j_done);
		tg_mov_r_rm(a, 8, RDI, tg_r(R10));
		tg_mov_r_rm(a, 8, RSI, tg_r(R11));
		tg_ret(a);
	} else {
		// the byte in every byte of rax
		tg_load_ext(a, 1, false, RAX, tg_r(RAX));
		tg_mov_r_imm(a, R10, 0x0101010101010101ull);
		tg_imul_r_rm(a, 8, RAX, tg_r(R10));
		x64_helper_end_of(a, R11, RDX);
		x64_helper_cmp_n(a, 16);
		i64 j_big = tg_jcc32(a, CC_A);
		i64 skip = 0;
		x64_helper_cmp_n(a, 8);  x64_helper_set_pair(a, 8, &skip); x64_helper_here(a, skip);
		x64_helper_cmp_n(a, 4);  x64_helper_set_pair(a, 4, &skip); x64_helper_here(a, skip);
		x64_helper_cmp_n(a, 2);  x64_helper_set_pair(a, 2, &skip); x64_helper_here(a, skip);
		x64_helper_cmp_n(a, 1);
		i64 j_none = tg_jcc32(a, CC_B);
		tg_mov_rm_r(a, 1, tg_m(RDX, 0), RAX);
		x64_helper_here(a, j_none);
		tg_ret(a);
		x64_helper_here(a, j_big);
		x64_helper_cmp_n(a, 64);
		i64 j_rep = tg_jcc32(a, CC_A);
		tg_movd_x_rm(a, 8, X64_F0, tg_r(RAX));
		tg_enc(a, TG_P66|TG_0F, 0x6C, X64_F0, tg_r(X64_F0), 0); // punpcklqdq
		tg_movups_m_x(a, tg_m(RDX, 0), X64_F0);
		tg_movups_m_x(a, tg_m(R11, -16), X64_F0);
		x64_helper_cmp_n(a, 32);
		i64 j_done = tg_jcc32(a, CC_BE);
		tg_movups_m_x(a, tg_m(RDX, 16), X64_F0);
		tg_movups_m_x(a, tg_m(R11, -32), X64_F0);
		x64_helper_here(a, j_done);
		tg_ret(a);
		x64_helper_here(a, j_rep);
		tg_mov_r_rm(a, 8, R10, tg_r(RDI));
		tg_mov_r_rm(a, 8, RDI, tg_r(RDX));
		tg_rep_stosb(a);
		tg_mov_r_rm(a, 8, RDI, tg_r(R10));
		tg_ret(a);
	}

	// no frame, the call's cfa holds throughout; Win64 takes a function without unwind info as a leaf
	if (!tg_is_win64()) {
		// the arrays live as long as the module: on the heap
		tgProcDebug dbg;
		memset(&dbg, 0, sizeof(dbg));
		dbg.name = m->symbols.data[sym].name;
		dbg.link_name = dbg.name;
		dbg.sym = sym;
		dbg.section = tgSection_Text;
		dbg.start = start;
		dbg.end = tg_pos(a);
		dbg.file_id = -1;
		dbg.line_entry_start = (i32)m->lines.count;
		dbg.naked = true;
		tg_array_add(&dbg.scope_parent, -1);
		tgScopeMark mark;
		mark.code_offset = 0;
		mark.scope = 0;
		mark.cold = false;
		tg_array_add(&dbg.scope_marks, mark);
		tg_array_add(&m->proc_debug, dbg);
	}
	m->symbols.data[sym].size = tg_pos(a) - start;
	return sym;
}

// A pending symbol for `name`, which the main thread looks up or adds when it appends the
// code: it changes the symbol table while workers lower. `helper` was a default argument, 0.
TG_INTERNAL i32 tg_lower_symbol(tgModule *m, tgLowerOut *out, tgString name, u32 flags, i8 helper) {
	(void)m;
	for (isize i = 0; i < out->pending.count; i++) {
		if (tg_str_eq(out->pending.data[i].name, name)) return TG_PENDING_SYM + (i32)i;
	}
	tgPendingSym ps;
	memset(&ps, 0, sizeof(ps));
	ps.name = name;
	ps.flags = flags;
	ps.helper = helper;
	tg_array_add(&out->pending, ps);
	return TG_PENDING_SYM + (i32)(out->pending.count-1);
}

// The __imp_ symbol of `sym`, pending.
TG_INTERNAL i32 tg_lower_import_symbol(tgLowerOut *out, i32 sym) {
	for (isize i = 0; i < out->pending.count; i++) {
		if (out->pending.data[i].import_of == sym + 1) return TG_PENDING_SYM + (i32)i;
	}
	tgPendingSym ps;
	memset(&ps, 0, sizeof(ps));
	ps.import_of = sym + 1;
	tg_array_add(&out->pending, ps);
	return TG_PENDING_SYM + (i32)(out->pending.count-1);
}

// The helper is emitted when the procedure's code is appended, right before it.
TG_INTERNAL i32 x64_lower_helper(tgLower *L, bool set) {
	tgModule *m = L->p->m;
	return tg_lower_symbol(m, L->out, set ? tg_str_lit("__tg_memset") : tg_str_lit("__tg_memmove"), 0, set ? 2 : 1);
}

////////////////////////////////////////////////////////////////
// Calls
////////////////////////////////////////////////////////////////

// No vreg lives in an argument register here but the one the argument reads, so arguments
// go straight into their registers.
////////////////////////////////////////////////////////////////
// x87, for long double. The x87 stack is empty between instructions, as the ABI wants it at
// calls. Scratch memory is the red zone below rsp, which SysV keeps from signal handlers.
////////////////////////////////////////////////////////////////

// An x87 instruction with a memory operand: `opcode` /`ext`.
TG_INTERNAL void x64_x87_m(tgAsm *a, u8 opcode, u8 ext, tgOpnd m) {
	tg_enc(a, 0, opcode, ext, m, 0);
}

TG_INTERNAL void x64_x87_2(tgAsm *a, u8 b0, u8 b1) {
	tg_b(a, b0);
	tg_b(a, b1);
}

TG_INTERNAL void x64_fld80(tgAsm *a, tgOpnd m)  { x64_x87_m(a, 0xDB, 5, m); }
TG_INTERNAL void x64_fstp80(tgAsm *a, tgOpnd m) { x64_x87_m(a, 0xDB, 7, m); }
// fld and fstp of a float or a double
TG_INTERNAL void x64_fld_f(tgAsm *a, i32 size, tgOpnd m)  { x64_x87_m(a, size == 4 ? 0xD9 : 0xDD, 0, m); }
TG_INTERNAL void x64_fstp_f(tgAsm *a, i32 size, tgOpnd m) { x64_x87_m(a, size == 4 ? 0xD9 : 0xDD, 3, m); }
// fild and fistp of a 32 or 64-bit int
TG_INTERNAL void x64_fild(tgAsm *a, i32 size, tgOpnd m)  { x64_x87_m(a, size == 4 ? 0xDB : 0xDF, size == 4 ? 0 : 5, m); }
TG_INTERNAL void x64_fistp(tgAsm *a, i32 size, tgOpnd m) { x64_x87_m(a, size == 4 ? 0xDB : 0xDF, size == 4 ? 3 : 7, m); }

// mov dword [rsp + disp], imm
TG_INTERNAL void x64_store_imm32_rsp(tgAsm *a, i32 disp, u32 imm) {
	tg_enc(a, 0, 0xC7, 0, tg_m(RSP, disp), 4);
	tg_u32(a, imm);
}

// fistp of st(0) to [rsp-8], rounding toward zero like C: the control word is saved at
// [rsp-16] and a truncating copy loaded from [rsp-14], then the saved one comes back.
TG_INTERNAL void x64_fistp_trunc(tgAsm *a, i32 size) {
	x64_x87_m(a, 0xD9, 7, tg_m(RSP, -16)); // fnstcw
	x64_x87_m(a, 0xD9, 7, tg_m(RSP, -14));
	tg_enc(a, 0, 0x80, 1, tg_m(RSP, -13), 1); // or byte [rsp-13], 0x0C: round toward zero
	tg_b(a, 0x0C);
	x64_x87_m(a, 0xD9, 5, tg_m(RSP, -14)); // fldcw
	x64_fistp(a, size, tg_m(RSP, -8));
	x64_x87_m(a, 0xD9, 5, tg_m(RSP, -16));
}

TG_INTERNAL void x64_compare_result(tgLower *L, tgInstr const *in, tgCC cc);

TG_INTERNAL void x64_lower_x87(tgLower *L, tgInstr const *in) {
	tgAsm *a = &L->a;
	i32 size = tg_type_size(in->type);
	switch ((tgX87Op)in->aux) {
	case tgX87_Add:
	case tgX87_Sub:
	case tgX87_Mul:
	case tgX87_Div: {
		static u8 const ops[4] = {0xC1, 0xE9, 0xC9, 0xF9}; // faddp, fsubp, fmulp, fdivp st(1), st(0)
		x64_fld80(a, tg_m(x64_src(L, in->b, RAX, 8, tgExt_None), 0));
		x64_fld80(a, tg_m(x64_src(L, in->c, RCX, 8, tgExt_None), 0));
		// st(1) = st(1) op st(0), popped: [b] op [c]
		x64_x87_2(a, 0xDE, ops[in->aux - tgX87_Add]);
		x64_fstp80(a, tg_m(x64_src(L, in->a, RDX, 8, tgExt_None), 0));
		break;
	}
	case tgX87_Neg:
		x64_fld80(a, tg_m(x64_src(L, in->b, RAX, 8, tgExt_None), 0));
		x64_x87_2(a, 0xD9, 0xE0); // fchs
		x64_fstp80(a, tg_m(x64_src(L, in->a, RDX, 8, tgExt_None), 0));
		break;
	case tgX87_Cmp: {
		tgCond c = (tgCond)in->imm;
		u32 x = in->b;
		u32 y = in->c;
		if (c == tgCond_FLT || c == tgCond_FLE) {
			// a < b  <=>  b > a
			x = in->c;
			y = in->b;
		}
		x64_fld80(a, tg_m(x64_src(L, y, RAX, 8, tgExt_None), 0));
		x64_fld80(a, tg_m(x64_src(L, x, RCX, 8, tgExt_None), 0));
		x64_x87_2(a, 0xDF, 0xE9); // fucomip st(0), st(1): the flags of ucomis x, y
		x64_x87_2(a, 0xDD, 0xD8); // fstp st(0)
		switch (c) {
		case tgCond_FGT:
		case tgCond_FLT:
			x64_compare_result(L, in, CC_A);
			break;
		case tgCond_FGE:
		case tgCond_FLE:
			x64_compare_result(L, in, CC_AE);
			break;
		case tgCond_FEQ:
		case tgCond_FNE: {
			bool eq = c == tgCond_FEQ;
			// unordered sets the parity flag
			tg_setcc(a, eq ? CC_E : CC_NE, RAX);
			tg_setcc(a, eq ? CC_NP : CC_P, RCX);
			tg_alu_r_rm(a, eq ? ALU_AND : ALU_OR, 1, RAX, tg_r(RCX));
			tg_load_ext(a, 1, false, RAX, tg_r(RAX));
			x64_put(L, in->dst, RAX);
			break;
		}
		default:
			TG_PANIC("bad float cond");
		}
		break;
	}
	case tgX87_FromSI:
	case tgX87_FromUI: {
		// the destination's memory holds the int for fild
		u8 d = x64_src(L, in->a, RDX, 8, tgExt_None);
		u8 r = x64_src(L, in->c, RAX, size, tgExt_None);
		tg_mov_rm_r(a, size, tg_m(d, 0), r);
		x64_fild(a, size, tg_m(d, 0));
		if (in->aux == tgX87_FromUI) {
			// read as signed, a set top bit is 2^64 too few
			tg_test_rm_r(a, 8, tg_r(r), r);
			i64 j_pos = tg_jcc32(a, CC_NS);
			x64_store_imm32_rsp(a, -4, 0x5F800000u); // 2^64 as a float
			x64_x87_m(a, 0xD8, 0, tg_m(RSP, -4));   // fadd dword
			tg_patch_rel32(a, j_pos, tg_pos(a));
		}
		x64_fstp80(a, tg_m(d, 0));
		break;
	}
	case tgX87_FromF: {
		u8 d = x64_src(L, in->a, RDX, 8, tgExt_None);
		u8 x = x64_srcf(L, in->c, X64_F0, size);
		tg_movs_rm_x(a, size, tg_m(d, 0), x);
		x64_fld_f(a, size, tg_m(d, 0));
		x64_fstp80(a, tg_m(d, 0));
		break;
	}
	case tgX87_ToSI: {
		x64_fld80(a, tg_m(x64_src(L, in->b, RAX, 8, tgExt_None), 0));
		x64_fistp_trunc(a, size);
		u8 d = x64_dst(L, in->dst, RAX);
		tg_mov_r_rm(a, size, d, tg_m(RSP, -8));
		x64_put(L, in->dst, d);
		break;
	}
	case tgX87_ToUI: {
		x64_fld80(a, tg_m(x64_src(L, in->b, RAX, 8, tgExt_None), 0));
		x64_store_imm32_rsp(a, -4, 0x5F000000u); // 2^63 as a float
		x64_x87_m(a, 0xD9, 0, tg_m(RSP, -4));   // fld dword
		x64_x87_2(a, 0xDB, 0xE9);                // fucomi st(0), st(1): 2^63 against the value
		tg_setcc(a, CC_BE, RAX);                 // 2^63 <= value: convert value - 2^63
		i64 j_small = tg_jcc32(a, CC_A);
		x64_x87_2(a, 0xDE, 0xE9);                // fsubp: value - 2^63
		i64 j_conv = tg_jmp32(a);
		tg_patch_rel32(a, j_small, tg_pos(a));
		x64_x87_2(a, 0xDD, 0xD8);                // fstp st(0): drop 2^63
		tg_patch_rel32(a, j_conv, tg_pos(a));
		x64_fistp_trunc(a, 8);
		tg_load_ext(a, 1, false, RAX, tg_r(RAX));
		tg_shift_imm(a, 4, 8, tg_r(RAX), 63);    // shl
		tg_alu_r_rm(a, ALU_XOR, 8, RAX, tg_m(RSP, -8));
		x64_put(L, in->dst, RAX);
		break;
	}
	case tgX87_ToF: {
		x64_fld80(a, tg_m(x64_src(L, in->b, RAX, 8, tgExt_None), 0));
		x64_fstp_f(a, size, tg_m(RSP, -8));
		u8 x = x64_dstf(L, in->dst, X64_F0);
		tg_movs_x_rm(a, size, x, tg_m(RSP, -8));
		x64_putf(L, in->dst, x, size);
		break;
	}
	default:
		TG_PANIC("bad x87 op");
	}
}

TG_INTERNAL void tg_lower_call(tgLower *L, tgCall const *c, bool is_ret) {
	tgAsm *a = &L->a;
	// stack arguments first, the argument registers are free to use as scratch
	for (isize i = 0; i < c->args.count; i++) {
		tgCallArg const *arg = &c->args.data[i];
		switch (arg->kind) {
		case tgCallArg_Stack: {
			tgType t = arg->type;
			if (tg_type_is_float(t)) {
				u8 r = x64_srcf(L, arg->vreg, X64_F0, tg_type_size(t));
				tg_movs_rm_x(a, tg_type_size(t), tg_m(RSP, arg->stack_offset), r);
			} else {
				u8 r = x64_src(L, arg->vreg, RAX, tg_type_size(t), arg->ext == tgExt_Sign ? tgExt_Sign : tgExt_Zero);
				tg_mov_rm_r(a, 8, tg_m(RSP, arg->stack_offset), r);
			}
			break;
		}
		case tgCallArg_StackMem: {
			if (arg->type == tgType_None || arg->size > 8) {
				// a by-value copy
				tgOpnd src = tg_mem_opnd(L, arg->mem, R11);
				tg_lea(a, R11, src);
				tg_lea(a, R10, tg_m(RSP, arg->stack_offset));
				x64_copy_fwd(L, arg->size);
			} else if (arg->type == tgType_V128) {
				tgOpnd src = tg_mem_opnd(L, arg->mem, R11);
				tg_movups_x_m(a, X64_F0, src);
				tg_movups_m_x(a, tg_m(RSP, arg->stack_offset), X64_F0);
			} else {
				tgOpnd src = x64_mem_base(L, arg->mem, R11);
				tg_load_bytes_gpr(L, RAX, src, arg->size, arg->ext);
				tg_mov_rm_r(a, 8, tg_m(RSP, arg->stack_offset), RAX);
			}
			break;
		}
		}
	}
	for (isize i = 0; i < c->args.count; i++) {
		tgCallArg const *arg = &c->args.data[i];
		switch (arg->kind) {
		case tgCallArg_Gpr: {
			i32 size = tg_type_size(arg->type);
			tgExtKind ext = arg->ext;
			if (size < 4 && ext == tgExt_None) ext = tgExt_Zero;
			x64_get(L, arg->reg, arg->vreg, size, ext);
			break;
		}
		case tgCallArg_Xmm:
			if (tg_type_is_float(arg->type)) {
				x64_getf(L, arg->reg, arg->vreg, tg_type_size(arg->type));
			} else {
				tg_load_xmm(L, arg->reg, x64_slot(L, arg->vreg), tg_type_size(arg->type));
			}
			break;
		case tgCallArg_GprMem: {
			tgExtKind ext = arg->ext;
			if (arg->size < 4 && ext == tgExt_None) ext = tgExt_Zero;
			if (arg->mem.kind == tgMem_Local && L->local_reg.data[arg->mem.base] >= 0) {
				// the register is callee saved, the call is in its interval
				x64_extend(a, ext, arg->size, arg->reg, (u8)L->local_reg.data[arg->mem.base]);
				break;
			}
			tgOpnd src = tg_mem_opnd(L, arg->mem, R11);
			if (src.reg == TG_RIP && (arg->size != 1 && arg->size != 2 && arg->size != 4 && arg->size != 8)) {
				tg_lea(a, R11, src);
				src = tg_m(R11, 0);
			}
			tg_load_bytes_gpr(L, arg->reg, src, arg->size, ext);
			break;
		}
		case tgCallArg_XmmMem: {
			tgOpnd src = tg_mem_opnd(L, arg->mem, R11);
			tg_load_xmm(L, arg->reg, src, arg->size);
			break;
		}
		case tgCallArg_X87Mem:
			x64_fld80(a, tg_mem_opnd(L, arg->mem, R11));
			break;
		}
	}
	if (is_ret) {
		return;
	}
	if (c->sse_count >= 0) {
		tg_mov_r_imm(a, RAX, (u64)c->sse_count);
	}
	if (c->target_sym >= 0) {
		tg_call_sym(a, c->target_sym);
	} else {
		tg_call_rm(a, tg_r(x64_src(L, c->target_vreg, R11, 8, tgExt_None)));
	}
	for (isize i = 0; i < c->rets.count; i++) {
		tgCallRet const *r = &c->rets.data[i];
		if (r->loc == tgLoc_X87) {
			x64_fstp80(a, tg_mem_opnd(L, r->dst, R11));
			continue;
		}
		if (r->dst.kind == tgMem_Local && L->local_reg.data[r->dst.base] >= 0) {
			x64_extend(a, tgExt_Zero, r->size, (u8)L->local_reg.data[r->dst.base], r->reg);
			continue;
		}
		if (r->loc == tgLoc_Gpr) {
			tgOpnd dst = x64_mem_base(L, r->dst, R11);
			tg_mov_r_rm(a, 8, R10, tg_r(r->reg));
			tg_store_bytes_gpr(L, dst, R10, r->size);
		} else {
			tgOpnd dst = tg_mem_opnd(L, r->dst, R11);
			tg_store_xmm(L, dst, r->reg, r->size);
		}
	}
	if (c->rets.count == 1 && c->rets.data[0].loc != tgLoc_X87 &&
	    !(c->rets.data[0].dst.kind == tgMem_Local && L->local_reg.data[c->rets.data[0].dst.base] >= 0)) {
		tgCallRet const *r = &c->rets.data[0];
		tgLowerStored st;
		memset(&st, 0, sizeof(st));
		st.valid = true;
		st.fp = r->loc != tgLoc_Gpr;
		st.reg = r->reg;
		st.size = r->size;
		st.mem = r->dst;
		st.fresh = true;
		L->stored = st;
	}
}

// vmovups ymm/zmm <-> [rax]
TG_INTERNAL void tg_vmovups_wide_rax(tgAsm *a, i32 size, bool load, u8 x) {
	tg_vmovups_wide(a, size, load, x, tg_m(RAX, 0));
}

// The operands are pinned to their slots, so loading one register never overwrites another's input.
TG_INTERNAL void tg_lower_asm(tgLower *L, tgAsmBlock const *blk) {
	tgAsm *a = &L->a;
	i32 save_base = blk->save_local >= 0 ? L->p->locals.data[blk->save_local].frame_offset : 0;
	i32 k = 0;
	for (u8 r = 0; r < 16; r++) {
		if (blk->save_regs & (1u << r)) tg_mov_rm_r(a, 8, tg_m(RBP, save_base + 8*k++), r);
	}
	i32 xmm_base = blk->xmm_save_local >= 0 ? L->p->locals.data[blk->xmm_save_local].frame_offset : 0;
	k = 0;
	for (u8 r = 0; r < 16; r++) {
		if (blk->save_xmms & (1u << r)) tg_movups_m_x(a, tg_m(RBP, xmm_base + 16*k++), r);
	}
	if (blk->rbp_local >= 0) tg_mov_rm_r(a, 8, tg_m(RBP, L->p->locals.data[blk->rbp_local].frame_offset), RBP);
	// vector inputs first, they go through rax
	for (isize i = 0; i < blk->inputs.count; i++) {
		tgAsmIo const *io = &blk->inputs.data[i];
		if (io->kind != tgAsmIo_XmmMem) continue;
		x64_get(L, RAX, io->vreg, 8, tgExt_None);
		if (io->size >= 32) tg_vmovups_wide_rax(a, io->size, true, io->reg);
		else tg_load_xmm(L, io->reg, tg_m(RAX, 0), io->size);
	}
	for (isize i = 0; i < blk->inputs.count; i++) {
		tgAsmIo const *io = &blk->inputs.data[i];
		if (io->kind == tgAsmIo_Xmm) tg_load_xmm(L, io->reg, x64_slot(L, io->vreg), io->size);
	}
	for (isize i = 0; i < blk->inputs.count; i++) {
		tgAsmIo const *io = &blk->inputs.data[i];
		if (io->kind == tgAsmIo_Gpr) x64_get(L, io->reg, io->vreg, io->size, io->sign ? tgExt_Sign : tgExt_Zero);
	}
	// mov ah, [rbp + slot]: no REX, so the reg field names the high byte
	for (isize i = 0; i < blk->inputs.count; i++) {
		tgAsmIo const *io = &blk->inputs.data[i];
		if (io->kind != tgAsmIo_Gpr8H) continue;
		i64 kv = 0;
		if (x64_is_const(L, io->vreg, &kv)) {
			tg_b(a, (u8)(0xB0 + io->reg)); // mov ah, imm8
			tg_b(a, (u8)kv);
		} else {
			tg_enc(a, 0, 0x8A, io->reg, x64_slot(L, io->vreg), 0);
		}
	}
	tgBytes code;
	memset(&code, 0, sizeof(code));
	code.arena = tg_arena();
	tg_array_reserve(&code, blk->pool.count + 16);
	bool fits = tg_asm_layout(blk->items, blk->pool, blk->label_count, tg_pos(a), &code);
	TG_ASSERT(fits);
	tg_bytes(a, code.data, code.count);
	tg_array_free(&code);
	if (blk->rbp_local >= 0) {
		// rbp is not the frame yet, rsp is back where it was
		tg_mov_r_rm(a, 8, RBP, tg_m(RSP, L->frame_size + L->p->locals.data[blk->rbp_local].frame_offset));
	}
	// gpr outputs first, then the flags, before anything changes them, then vectors through rax
	for (isize i = 0; i < blk->outputs.count; i++) {
		tgAsmIo const *io = &blk->outputs.data[i];
		if (io->kind == tgAsmIo_Gpr) tg_mov_rm_r(a, io->size, x64_slot(L, io->vreg), io->reg);
		if (io->kind == tgAsmIo_Gpr8H) tg_enc(a, 0, 0x88, io->reg, x64_slot(L, io->vreg), 0);
		if (io->kind == tgAsmIo_Xmm) tg_store_xmm(L, x64_slot(L, io->vreg), io->reg, io->size);
	}
	for (isize i = 0; i < blk->outputs.count; i++) {
		tgAsmIo const *io = &blk->outputs.data[i];
		if (io->kind != tgAsmIo_Flag) continue;
		tg_setcc(a, (tgCC)io->reg, RAX);
		tg_load_ext(a, 1, false, RAX, tg_r(RAX));
		tg_mov_rm_r(a, io->size, x64_slot(L, io->vreg), RAX);
	}
	for (isize i = 0; i < blk->outputs.count; i++) {
		tgAsmIo const *io = &blk->outputs.data[i];
		if (io->kind != tgAsmIo_XmmMem) continue;
		x64_get(L, RAX, io->vreg, 8, tgExt_None);
		if (io->size >= 32) tg_vmovups_wide_rax(a, io->size, false, io->reg);
		else tg_store_xmm(L, tg_m(RAX, 0), io->reg, io->size);
	}
	k = 0;
	for (u8 r = 0; r < 16; r++) {
		if (blk->save_regs & (1u << r)) tg_mov_r_rm(a, 8, r, tg_m(RBP, save_base + 8*k++));
	}
	k = 0;
	for (u8 r = 0; r < 16; r++) {
		if (blk->save_xmms & (1u << r)) tg_movups_x_m(a, r, tg_m(RBP, xmm_base + 16*k++));
	}
}

TG_INTERNAL void x64_epilogue(tgLower *L) {
	tgAsm *a = &L->a;
	if (L->p->naked) {
		tg_ret(a);
		return;
	}
	if (tg_is_win64()) {
		tg_win64_epilogue(L);
		return;
	}
	for (i32 i = 0; i < L->saved_count; i++) {
		tg_mov_r_rm(a, 8, L->saved[i], tg_m(RBP, L->save_offset[i]));
	}
	tg_leave(a);
	tg_ret(a);
}

// Jumps to `block`, patched once every block is placed.
TG_INTERNAL void x64_jump(tgLower *L, i32 block) {
	tgLowerFixup f;
	f.at = tg_jmp32(&L->a);
	f.block = block;
	tg_array_add(&L->fixups, f);
}

// Goes to `t` when `cc` holds, else to `f`.
TG_INTERNAL void x64_branch(tgLower *L, tgCC cc, i32 t, i32 f) {
	// x86 condition codes come in pairs that differ in the low bit
	if (t == L->next_block) {
		TG_SWAP(i32, t, f);
		cc = (tgCC)(cc ^ 1);
	}
	tgLowerFixup f1;
	f1.at = tg_jcc32(&L->a, cc);
	f1.block = t;
	tg_array_add(&L->fixups, f1);
	if (f != L->next_block) x64_jump(L, f);
}

// Sets the flags so that CC_NE says whether the bool v is true.
TG_INTERNAL void x64_test_bool(tgLower *L, u32 v) {
	tgAsm *a = &L->a;
	i8 r = L->reg.data[v];
	if (!L->is_const.data[v] && r == TG_NOREG) {
		tg_alu_rm_imm(a, ALU_CMP, 1, x64_slot(L, v), 0);
		return;
	}
	u8 x = x64_src(L, v, RAX, 1, tgExt_None);
	tg_test_rm_r(a, 1, tg_r(x), x);
}

// Ends a compare: the flags hold `cc`, kept for the next instruction or put in dst.
TG_INTERNAL void x64_compare_result(tgLower *L, tgInstr const *in, tgCC cc) {
	if (L->fuse) {
		L->flags_vreg = in->dst;
		L->flags_cc = cc;
		return;
	}
	u8 d = x64_dst(L, in->dst, RAX);
	tg_setcc(&L->a, cc, d);
	tg_load_ext(&L->a, 1, false, d, tg_r(d));
	x64_put(L, in->dst, d);
}

////////////////////////////////////////////////////////////////
// Instructions
////////////////////////////////////////////////////////////////

TG_INTERNAL void tg_lower_instr(tgLower *L, tgInstr const *in) {
	tgAsm *a = &L->a;
	tgProc *p = L->p;
	i32 size = tg_type_size(in->type);
	i64 k = 0;
	i32 imm = 0;
	switch (in->op) {
	case tgOp_Nop:
		if (in->imm == 1) tg_b(a, 0x90);
		break;
	case tgOp_Scope:
		break;
	case tgOp_Loc: {
		tgLineEntry e;
		memset(&e, 0, sizeof(e));
		e.code_offset = (i32)(tg_pos(a) - L->proc_start);
		e.file_id = (i32)in->a;
		e.line = (i32)in->imm;
		e.column = (i32)in->b;
		tg_array_add(&L->out->lines, e);
		break;
	}
	case tgOp_IConst:
	case tgOp_FConst: {
		i8 r = L->reg.data[in->dst];
		if (r == TG_NOREG && in->imm >= -0x80000000ll && in->imm <= 0x7fffffffll) {
			// mov qword [slot], simm32
			tg_enc(a, TG_W, 0xC7, 0, x64_slot(L, in->dst), 4);
			tg_u32(a, (u32)(i32)in->imm);
			break;
		}
		u8 d = x64_dst(L, in->dst, RAX);
		tg_mov_r_imm(a, d, (u64)in->imm);
		x64_put(L, in->dst, d);
		break;
	}
	case tgOp_Lea: {
		if (L->remat.data[in->dst]) break;
		u8 d = x64_dst(L, in->dst, RAX);
		tgOpnd m = tg_mem_opnd(L, in->mem, R11);
		if (m.is_mem && m.reg != TG_RIP && m.disp == 0) {
			if (m.reg != d) tg_mov_r_rm(a, 8, d, tg_r(m.reg));
		} else {
			tg_lea(a, d, m);
		}
		x64_put(L, in->dst, d);
		break;
	}
	case tgOp_Load:
	case tgOp_AtomicLoad: {
		bool fp = tg_type_is_float(in->type);
		if (in->mem.kind == tgMem_Local && L->local_reg.data[in->mem.base] >= 0) {
			i8 lr = L->local_reg.data[in->mem.base];
			if (lr >= TG_FREG) x64_putf(L, in->dst, (u8)(lr - TG_FREG), size);
			else               x64_put(L, in->dst, (u8)lr);
			break;
		}
		tgLowerStored const *st = &L->stored;
		if (in->op == tgOp_Load && st->valid && st->mem.kind == in->mem.kind && st->mem.base == in->mem.base && st->mem.offset == in->mem.offset &&
		    st->size == size && st->fp == fp && !(in->flags & tgInstrFlag_Volatile)) {
			if (fp) {
				x64_putf(L, in->dst, st->reg, size);
			} else {
				u8 d = x64_dst(L, in->dst, RAX);
				x64_extend(a, tgExt_Zero, size, d, st->reg);
				x64_put(L, in->dst, d);
			}
			break;
		}
		tgOpnd m = tg_mem_opnd(L, in->mem, R11);
		if (fp) {
			u8 d = x64_dstf(L, in->dst, X64_F0);
			tg_movs_x_rm(a, size, d, m);
			x64_putf(L, in->dst, d, size);
		} else {
			u8 d = x64_dst(L, in->dst, RAX);
			tg_load_ext(a, size, false, d, m);
			x64_put(L, in->dst, d);
		}
		break;
	}
	case tgOp_Store: {
		if (in->mem.kind == tgMem_Local && L->local_reg.data[in->mem.base] >= 0) {
			i8 lr = L->local_reg.data[in->mem.base];
			// zero extended, as a load from memory would give it back
			if (lr >= TG_FREG) x64_getf(L, (u8)(lr - TG_FREG), in->a, size);
			else               x64_get(L, (u8)lr, in->a, size, tgExt_Zero);
			break;
		}
		tgLowerStored st;
		memset(&st, 0, sizeof(st));
		st.valid = true;
		st.fp = tg_type_is_float(in->type);
		st.reg = 0;
		st.size = size;
		st.mem = in->mem;
		st.fresh = true;
		if (st.fp) {
			st.reg = x64_srcf(L, in->a, X64_F0, size);
			tgOpnd m = tg_mem_opnd(L, in->mem, R11);
			tg_movs_rm_x(a, size, m, st.reg);
		} else if (x64_imm(L, in->a, size, &imm)) {
			tgOpnd m = tg_mem_opnd(L, in->mem, R11);
			// mov rm, imm, sign extended for qwords
			tg_enc(a, tg_size_flags(size), size == 1 ? 0xC6 : 0xC7, 0, m, tg_min(size, 4));
			switch (size) {
			case 1: tg_b(a, (u8)imm); break;
			case 2: tg_u16(a, (u16)imm); break;
			default: tg_u32(a, (u32)imm); break;
			}
			st.valid = false;
		} else {
			// the value first, the address may need r11
			st.reg = x64_src(L, in->a, RAX, size, tgExt_None);
			tgOpnd m = tg_mem_opnd(L, in->mem, R11);
			tg_mov_rm_r(a, size, m, st.reg);
		}
		if (!(in->flags & tgInstrFlag_Volatile) && st.valid) L->stored = st;
		break;
	}
	case tgOp_AtomicStore: {
		x64_get(L, RAX, in->a, size, tgExt_None);
		tgOpnd m = tg_mem_opnd(L, in->mem, R11);
		// xchg is a full barrier
		tg_enc(a, tg_size_flags(size), size == 1 ? 0x86 : 0x87, RAX, m, 0);
		break;
	}
	case tgOp_Copy:
	case tgOp_Trunc:
	case tgOp_Bitcast: {
		bool df = tg_type_is_float(p->vregs.data[in->dst]);
		bool sf = tg_type_is_float(p->vregs.data[in->a]);
		if (df && sf) {
			i32 fs = tg_type_size(p->vregs.data[in->dst]);
			x64_putf(L, in->dst, x64_srcf(L, in->a, x64_dstf(L, in->dst, X64_F0), fs), fs);
			break;
		}
		u8 d = x64_dst(L, in->dst, RAX);
		x64_put(L, in->dst, x64_src(L, in->a, d, 8, tgExt_None));
		break;
	}

	case tgOp_Add:
	case tgOp_Sub:
	case tgOp_And:
	case tgOp_Or:
	case tgOp_Xor: {
		tgAluOp op = ALU_ADD;
		switch (in->op) {
		case tgOp_Add: op = ALU_ADD; break;
		case tgOp_Sub: op = ALU_SUB; break;
		case tgOp_And: op = ALU_AND; break;
		case tgOp_Or:  op = ALU_OR;  break;
		case tgOp_Xor: op = ALU_XOR; break;
		}
		// narrow values compute in 32 bits, only their low bytes mean anything
		i32 s = tg_max(size, 4);
		bool commutes = in->op != tgOp_Sub;
		u32 x = in->a, y = in->b;
		if (commutes && L->is_const.data[x] && !L->is_const.data[y]) TG_SWAP(u32, x, y);
		u8 d = x64_dst(L, in->dst, RAX);
		if (!L->is_const.data[y] && L->reg.data[y] == (i8)d) {
			if (commutes) TG_SWAP(u32, x, y);
			else          d = RAX;
		}
		i8 rx = L->is_const.data[x] ? TG_NOREG : L->reg.data[x];
		if ((in->op == tgOp_Add || in->op == tgOp_Sub) && rx != TG_NOREG && rx < TG_FREG && rx != (i8)d &&
		    x64_imm(L, y, 8, &imm) && imm != INT32_MIN) {
			// a three operand add
			tg_lea(a, d, tg_m((u8)rx, in->op == tgOp_Add ? imm : -imm));
			x64_put(L, in->dst, d);
			break;
		}
		x64_get(L, d, x, s, tgExt_None);
		if (x64_imm(L, y, s, &imm)) {
			tg_alu_rm_imm(a, op, s, tg_r(d), imm);
		} else {
			tg_alu_r_rm(a, op, s, d, x64_opnd(L, y, RCX, s));
		}
		x64_put(L, in->dst, d);
		break;
	}
	case tgOp_Mul: {
		i32 s = tg_max(size, 4);
		u32 x = in->a, y = in->b;
		if (L->is_const.data[x] && !L->is_const.data[y]) TG_SWAP(u32, x, y);
		u8 d = x64_dst(L, in->dst, RAX);
		if (!L->is_const.data[y] && L->reg.data[y] == (i8)d) TG_SWAP(u32, x, y);
		if (x64_imm(L, y, s, &imm)) {
			// imul d, x, imm
			bool imm8 = imm >= -128 && imm <= 127;
			tg_enc(a, tg_size_flags(s), imm8 ? 0x6B : 0x69, d, x64_opnd(L, x, d, s), imm8 ? 1 : 4);
			if (imm8) tg_b(a, (u8)(i8)imm);
			else       tg_u32(a, (u32)imm);
		} else {
			x64_get(L, d, x, s, tgExt_None);
			tg_imul_r_rm(a, s, d, x64_opnd(L, y, RCX, s));
		}
		x64_put(L, in->dst, d);
		break;
	}
	case tgOp_SDiv:
	case tgOp_SRem:
	case tgOp_UDiv:
	case tgOp_URem: {
		bool sgn = in->op == tgOp_SDiv || in->op == tgOp_SRem;
		tgExtKind ext = sgn ? tgExt_Sign : tgExt_Zero;
		i32 s = tg_max(size, 4);
		x64_get(L, RAX, in->a, size, ext);
		u8 rb = x64_src(L, in->b, RCX, size, ext);
		if (sgn) {
			tg_sign_extend_rdx(a, s);
			tg_grp3(a, 7, s, tg_r(rb));
		} else {
			tg_alu_r_rm(a, ALU_XOR, 4, RDX, tg_r(RDX));
			tg_grp3(a, 6, s, tg_r(rb));
		}
		bool rem = in->op == tgOp_SRem || in->op == tgOp_URem;
		x64_put(L, in->dst, rem ? RDX : RAX);
		break;
	}
	case tgOp_Shl:
	case tgOp_LShr:
	case tgOp_AShr: {
		i32 s = tg_max(size, 4);
		u8 n = in->op == tgOp_Shl ? 4 : (in->op == tgOp_LShr ? 5 : 7);
		tgExtKind ext = in->op == tgOp_AShr ? tgExt_Sign : in->op == tgOp_LShr ? tgExt_Zero : tgExt_None;
		u8 d = x64_dst(L, in->dst, RAX);
		if (x64_is_const(L, in->b, &k)) {
			x64_get(L, d, in->a, size, ext);
			// the cpu masks an immediate count like it masks cl
			tg_shift_imm(a, n, s, tg_r(d), (u8)k);
		} else {
			x64_get(L, RCX, in->b, 8, tgExt_None);
			x64_get(L, d, in->a, size, ext);
			tg_shift_cl(a, n, s, tg_r(d));
		}
		x64_put(L, in->dst, d);
		break;
	}
	case tgOp_FAdd:
	case tgOp_FSub:
	case tgOp_FMul:
	case tgOp_FDiv: {
		tgSseOp op = SSE_ADD;
		switch (in->op) {
		case tgOp_FAdd: op = SSE_ADD; break;
		case tgOp_FSub: op = SSE_SUB; break;
		case tgOp_FMul: op = SSE_MUL; break;
		case tgOp_FDiv: op = SSE_DIV; break;
		}
		bool commutes = in->op == tgOp_FAdd || in->op == tgOp_FMul;
		u32 x = in->a, y = in->b;
		u8 d = x64_dstf(L, in->dst, X64_F0);
		if (!L->is_const.data[y] && L->reg.data[y] == (i8)(TG_FREG + d)) {
			if (commutes) TG_SWAP(u32, x, y);
			else          d = X64_F0;
		}
		x64_getf(L, d, x, size);
		tg_sse_scalar(a, op, size, d, x64_opndf(L, y, X64_F1, size));
		x64_putf(L, in->dst, d, size);
		break;
	}
	case tgOp_Sqrt: {
		u8 d = x64_dstf(L, in->dst, X64_F0);
		tg_sse_scalar(a, SSE_SQRT, size, d, x64_opndf(L, in->a, X64_F1, size));
		x64_putf(L, in->dst, d, size);
		break;
	}
	case tgOp_Fma:
		x64_getf(L, X64_F0, in->a, size);
		x64_getf(L, X64_F1, in->b, size);
		tg_vfmadd213_s(a, size, X64_F0, X64_F1, x64_opndf(L, in->c, X64_F2, size));
		x64_putf(L, in->dst, X64_F0, size);
		break;
	case tgOp_Neg:
	case tgOp_Not: {
		u8 d = x64_dst(L, in->dst, RAX);
		x64_get(L, d, in->a, size, tgExt_None);
		tg_grp3(a, in->op == tgOp_Neg ? 3 : 2, tg_max(size, 4), tg_r(d));
		x64_put(L, in->dst, d);
		break;
	}
	case tgOp_FNeg: {
		u8 d = x64_dstf(L, in->dst, X64_F0);
		x64_getf(L, d, in->a, size);
		tg_mov_r_imm(a, RAX, size == 4 ? 0x80000000ull : 0x8000000000000000ull);
		tg_movd_x_rm(a, 8, X64_F1, tg_r(RAX));
		tg_xorps(a, d, X64_F1);
		x64_putf(L, in->dst, d, size);
		break;
	}
	case tgOp_ICmp: {
		tgCond c = (tgCond)in->aux;
		if (in->a == L->flags_vreg) {
			// a test against zero of the compare that set the flags
			TG_ASSERT(c == tgCond_EQ || c == tgCond_NE);
			tgCC cc = c == tgCond_NE ? L->flags_cc : (tgCC)(L->flags_cc ^ 1);
			L->flags_vreg = 0;
			x64_compare_result(L, in, cc);
			break;
		}
		if (x64_imm(L, in->b, size, &imm)) {
			tg_alu_rm_imm(a, ALU_CMP, size, x64_opnd(L, in->a, RAX, size), imm);
		} else {
			u8 ra = x64_src(L, in->a, RAX, size, tgExt_None);
			tg_alu_r_rm(a, ALU_CMP, size, ra, x64_opnd(L, in->b, RCX, size));
		}
		x64_compare_result(L, in, tg_cc_for(c));
		break;
	}
	case tgOp_FCmp: {
		tgCond c = (tgCond)in->aux;
		u32 x = in->a;
		u32 y = in->b;
		if (c == tgCond_FLT || c == tgCond_FLE) {
			// a < b  <=>  b > a
			x = in->b;
			y = in->a;
		}
		u8 rx = x64_srcf(L, x, X64_F0, size);
		tg_ucomis(a, size, rx, x64_opndf(L, y, X64_F1, size));
		switch (c) {
		case tgCond_FGT:
		case tgCond_FLT:
			x64_compare_result(L, in, CC_A);
			break;
		case tgCond_FGE:
		case tgCond_FLE:
			x64_compare_result(L, in, CC_AE);
			break;
		case tgCond_FEQ:
		case tgCond_FNE: {
			TG_ASSERT(!L->fuse);
			bool eq = c == tgCond_FEQ;
			// unordered sets the parity flag
			tg_setcc(a, eq ? CC_E : CC_NE, RAX);
			tg_setcc(a, eq ? CC_NP : CC_P, RCX);
			tg_alu_r_rm(a, eq ? ALU_AND : ALU_OR, 1, RAX, tg_r(RCX));
			tg_load_ext(a, 1, false, RAX, tg_r(RAX));
			x64_put(L, in->dst, RAX);
			break;
		}
		default:
			TG_PANIC("bad float cond");
		}
		break;
	}
	case tgOp_Zext:
	case tgOp_Sext: {
		tgType src = (tgType)in->aux;
		u8 d = x64_dst(L, in->dst, RAX);
		x64_put(L, in->dst, x64_src(L, in->a, d, tg_type_size(src), in->op == tgOp_Sext ? tgExt_Sign : tgExt_Zero));
		break;
	}
	case tgOp_SIToF: {
		tgType src = (tgType)in->aux;
		u8 r = x64_src(L, in->a, RAX, tg_type_size(src), tgExt_Sign);
		u8 d = x64_dstf(L, in->dst, X64_F0);
		tg_cvtsi2f(a, size, 8, d, tg_r(r));
		x64_putf(L, in->dst, d, size);
		break;
	}
	case tgOp_UIToF: {
		tgType src = (tgType)in->aux;
		i32 ss = tg_type_size(src);
		u8 d = x64_dstf(L, in->dst, X64_F0);
		x64_get(L, RAX, in->a, ss, tgExt_Zero);
		if (ss < 8) {
			tg_cvtsi2f(a, size, 8, d, tg_r(RAX));
		} else {
			tg_test_rm_r(a, 8, tg_r(RAX), RAX);
			i64 j_neg = tg_jcc32(a, CC_S);
			tg_cvtsi2f(a, size, 8, d, tg_r(RAX));
			i64 j_done = tg_jmp32(a);
			tg_patch_rel32(a, j_neg, tg_pos(a));
			// halve it keeping the low bit for the rounding, convert, double
			tg_mov_r_rm(a, 8, RCX, tg_r(RAX));
			tg_shift_imm(a, 5, 8, tg_r(RCX), 1);
			tg_alu_rm_imm(a, ALU_AND, 4, tg_r(RAX), 1);
			tg_alu_r_rm(a, ALU_OR, 8, RCX, tg_r(RAX));
			tg_cvtsi2f(a, size, 8, d, tg_r(RCX));
			tg_sse_scalar(a, SSE_ADD, size, d, tg_r(d));
			tg_patch_rel32(a, j_done, tg_pos(a));
		}
		x64_putf(L, in->dst, d, size);
		break;
	}
	case tgOp_FToSI: {
		i32 fs = tg_type_size((tgType)in->aux);
		u8 d = x64_dst(L, in->dst, RAX);
		tg_cvttf2si(a, fs, size >= 8 ? 8 : 4, d, x64_opndf(L, in->a, X64_F0, fs));
		x64_put(L, in->dst, d);
		break;
	}
	case tgOp_FToUI: {
		i32 fs = tg_type_size((tgType)in->aux);
		x64_getf(L, X64_F0, in->a, fs);
		// 2^63 as a float of the source size
		if (fs == 4) {
			tg_mov_r_imm(a, RAX, 0x5f000000ull);
			tg_movd_x_rm(a, 4, X64_F1, tg_r(RAX));
		} else {
			tg_mov_r_imm(a, RAX, 0x43e0000000000000ull);
			tg_movd_x_rm(a, 8, X64_F1, tg_r(RAX));
		}
		tg_ucomis(a, fs, X64_F0, tg_r(X64_F1));
		i64 j_big = tg_jcc32(a, CC_AE);
		tg_cvttf2si(a, fs, 8, RAX, tg_r(X64_F0));
		i64 j_done = tg_jmp32(a);
		tg_patch_rel32(a, j_big, tg_pos(a));
		tg_sse_scalar(a, SSE_SUB, fs, X64_F0, tg_r(X64_F1));
		tg_cvttf2si(a, fs, 8, RAX, tg_r(X64_F0));
		tg_mov_r_imm(a, RCX, 0x8000000000000000ull);
		tg_alu_r_rm(a, ALU_XOR, 8, RAX, tg_r(RCX));
		tg_patch_rel32(a, j_done, tg_pos(a));
		x64_put(L, in->dst, RAX);
		break;
	}
	case tgOp_FExt:
	case tgOp_FTrunc: {
		bool ext = in->op == tgOp_FExt;
		u8 d = x64_dstf(L, in->dst, X64_F0);
		// cvtss2sd or cvtsd2ss
		tg_sse_scalar(a, SSE_CVT, ext ? 4 : 8, d, x64_opndf(L, in->a, X64_F1, ext ? 4 : 8));
		x64_putf(L, in->dst, d, ext ? 8 : 4);
		break;
	}
	case tgOp_Select: {
		tgCC cc = CC_NE;
		if (in->a == L->flags_vreg) {
			cc = L->flags_cc;
			L->flags_vreg = 0;
		} else {
			x64_test_bool(L, in->a);
		}
		// nothing below changes the flags: plain movs and cmov
		bool fp = tg_type_is_float(in->type);
		u8 d = fp ? RAX : x64_dst(L, in->dst, RAX);
		if (!L->is_const.data[in->b] && L->reg.data[in->b] == (i8)d) d = RAX;
		tgOpnd rb = x64_opnd(L, in->b, RCX, 8);
		x64_get(L, d, in->c, 8, tgExt_None);
		tg_cmov(a, cc, 8, d, rb);
		x64_put(L, in->dst, d);
		break;
	}

	case tgOp_MemCopy:
	case tgOp_MemMove: {
		i64 n = in->imm;
		if (n <= X64_INLINE_COPY) {
			u8 dst = x64_src(L, in->a, R10, 8, tgExt_None);
			u8 src = x64_src(L, in->b, R11, 8, tgExt_None);
			x64_copy_small(L, tg_m(dst, 0), tg_m(src, 0), n);
		} else {
			x64_get(L, RDX, in->b, 8, tgExt_None);
			x64_get(L, RAX, in->a, 8, tgExt_None);
			tg_mov_r_imm(a, RCX, (u64)n);
			tg_call_sym(a, x64_lower_helper(L, false));
		}
		break;
	}
	case tgOp_MemZero: {
		i64 n = in->imm;
		if (n <= 64) {
			tgOpnd m = x64_mem_base(L, in->mem, R11);
			tg_alu_r_rm(a, ALU_XOR, 4, RAX, tg_r(RAX));
			i64 off = 0;
			while (off < n) {
				i64 rem = n - off;
				i32 c = rem >= 8 ? 8 : rem >= 4 ? 4 : rem >= 2 ? 2 : 1;
				tgOpnd mm = m;
				mm.disp += (i32)off;
				tg_mov_rm_r(a, c, mm, RAX);
				off += c;
			}
		} else {
			tg_lea(a, RDX, tg_mem_opnd(L, in->mem, R11));
			tg_alu_r_rm(a, ALU_XOR, 4, RAX, tg_r(RAX));
			tg_mov_r_imm(a, RCX, (u64)n);
			tg_call_sym(a, x64_lower_helper(L, true));
		}
		break;
	}
	case tgOp_MemCopyDyn:
	case tgOp_MemMoveDyn:
		x64_get(L, RCX, in->c, 8, tgExt_None);
		x64_get(L, RDX, in->b, 8, tgExt_None);
		x64_get(L, RAX, in->a, 8, tgExt_None);
		tg_call_sym(a, x64_lower_helper(L, false));
		break;
	case tgOp_MemSetDyn:
		x64_get(L, RCX, in->c, 8, tgExt_None);
		x64_get(L, RDX, in->a, 8, tgExt_None);
		x64_get(L, RAX, in->b, 1, tgExt_None);
		tg_call_sym(a, x64_lower_helper(L, true));
		break;
	case tgOp_Call:
		tg_lower_call(L, &p->calls.data[(isize)in->imm], false);
		break;
	case tgOp_Jump:
		x64_jump(L, (i32)in->imm);
		break;
	case tgOp_Branch: {
		i32 t = (i32)in->imm;
		i32 f = (i32)in->c;
		if (in->a == L->flags_vreg) {
			L->flags_vreg = 0;
			x64_branch(L, L->flags_cc, t, f);
		} else if (x64_is_const(L, in->a, &k)) {
			i32 to = (k & 0xff) ? t : f;
			if (to != L->next_block) x64_jump(L, to);
		} else {
			x64_test_bool(L, in->a);
			x64_branch(L, CC_NE, t, f);
		}
		break;
	}
	case tgOp_Ret:
		tg_lower_call(L, &p->calls.data[(isize)in->imm], true);
		x64_epilogue(L);
		break;
	case tgOp_Unreachable:
	case tgOp_Trap:
		tg_ud2(a);
		break;
	case tgOp_DebugTrap:
		tg_int3(a);
		break;
	case tgOp_AtomicFence:
		tg_mfence(a);
		break;
	case tgOp_CpuRelax:
		tg_pause(a);
		break;
	case tgOp_ReadCycleCounter:
		tg_rdtsc(a);
		tg_shift_imm(a, 4, 8, tg_r(RDX), 32);
		tg_alu_r_rm(a, ALU_OR, 8, RAX, tg_r(RDX));
		x64_put(L, in->dst, RAX);
		break;
	case tgOp_StackPointer:
		x64_put(L, in->dst, RSP);
		break;
	case tgOp_StackRestore: {
		// the value is an earlier rsp: the allocas since then are freed, and the
		// outgoing argument area stays below rsp as the prologue left it
		u8 r = x64_src(L, in->a, RAX, 8, tgExt_None);
		tg_mov_r_rm(a, 8, RSP, tg_r(r));
		break;
	}
	case tgOp_FrameAddress:
		x64_put(L, in->dst, RBP);
		break;
	case tgOp_ReturnAddress: {
		u8 d = x64_dst(L, in->dst, RAX);
		if (in->imm) {
			tg_lea(a, d, tg_m(RBP, L->incoming_base - 8));
		} else {
			tg_mov_r_rm(a, 8, d, tg_m(RBP, L->incoming_base - 8));
		}
		x64_put(L, in->dst, d);
		break;
	}
	case tgOp_AtomicRmw: {
		tgRmwOp op = (tgRmwOp)in->aux;
		x64_get(L, RCX, in->a, size, tgExt_None);
		tgOpnd m = x64_mem_base(L, in->mem, R11);
		switch (op) {
		case tgRmw_Xchg:
			tg_enc(a, tg_size_flags(size), size == 1 ? 0x86 : 0x87, RCX, m, 0);
			tg_mov_r_rm(a, 8, RAX, tg_r(RCX));
			break;
		case tgRmw_Add:
		case tgRmw_Sub:
			if (op == tgRmw_Sub) tg_grp3(a, 3, size, tg_r(RCX));
			tg_enc(a, tg_size_flags(size)|TG_LOCK|TG_0F, size == 1 ? 0xC0 : 0xC1, RCX, m, 0);
			tg_mov_r_rm(a, 8, RAX, tg_r(RCX));
			break;
		default: {
			// cmpxchg loop
			tg_mov_r_rm(a, size, RAX, m);
			i64 loop = tg_pos(a);
			tg_mov_r_rm(a, 8, RDX, tg_r(RAX));
			switch (op) {
			case tgRmw_And:  tg_alu_r_rm(a, ALU_AND, 8, RDX, tg_r(RCX)); break;
			case tgRmw_Or:   tg_alu_r_rm(a, ALU_OR,  8, RDX, tg_r(RCX)); break;
			case tgRmw_Xor:  tg_alu_r_rm(a, ALU_XOR, 8, RDX, tg_r(RCX)); break;
			case tgRmw_Nand: tg_alu_r_rm(a, ALU_AND, 8, RDX, tg_r(RCX)); tg_grp3(a, 2, 8, tg_r(RDX)); break;
			}
			tg_enc(a, tg_size_flags(size)|TG_LOCK|TG_0F, size == 1 ? 0xB0 : 0xB1, RDX, m, 0);
			i64 j = tg_jcc32(a, CC_NE);
			tg_patch_rel32(a, j, loop);
			break;
		}
		}
		x64_put(L, in->dst, RAX);
		break;
	}
	case tgOp_AtomicCas: {
		x64_get(L, RAX, in->a, size, tgExt_None);
		x64_get(L, RCX, in->b, size, tgExt_None);
		tgOpnd m = tg_mem_opnd(L, in->mem, R11);
		tg_enc(a, tg_size_flags(size)|TG_LOCK|TG_0F, size == 1 ? 0xB0 : 0xB1, RCX, m, 0);
		tg_setcc(a, CC_E, RDX);
		tg_load_ext(a, 1, false, RDX, tg_r(RDX));
		x64_put(L, in->dst, RAX);
		x64_put(L, in->c, RDX);
		break;
	}
	case tgOp_Cpuid:
	case tgOp_Xgetbv: {
		bool cpuid = in->op == tgOp_Cpuid;
		// the address before cpuid writes rbx, which may hold its base
		tg_lea(a, R11, tg_mem_opnd(L, in->mem, R11));
		x64_get(L, cpuid ? RAX : RCX, in->a, 4, tgExt_None);
		if (cpuid) {
			x64_get(L, RCX, in->b, 4, tgExt_None);
			tg_mov_r_rm(a, 8, R10, tg_r(RBX)); // rbx is callee saved
			tg_b(a, 0x0F); tg_b(a, 0xA2);
		} else {
			tg_b(a, 0x0F); tg_b(a, 0x01); tg_b(a, 0xD0);
		}
		u8 const regs[4] = {RAX, (u8)(cpuid ? RBX : RDX), RCX, RDX};
		for (i32 i = 0; i < (cpuid ? 4 : 2); i++) {
			tg_mov_rm_r(a, 4, tg_m(R11, 4*i), regs[i]);
		}
		if (cpuid) {
			tg_mov_r_rm(a, 8, RBX, tg_r(R10));
		}
		break;
	}
	case tgOp_Vec128: {
		tgVecIntrinsic const *e = &tg_vec_intrinsics[in->aux];
		if (e->form == tgVecForm_Rdtscp) {
			tg_rdtscp(a);
			tg_shift_imm(a, 4, 8, tg_r(RDX), 32);
			tg_alu_r_rm(a, ALU_OR, 8, RAX, tg_r(RDX));
			tg_mov_rm_r(a, 8, tg_mem_opnd(L, in->mem, R11), RAX);
			tg_mov_rm_r(a, 4, tg_mem_opnd(L, tg_mem_offset(in->mem, 8), R11), RCX);
			break;
		}
		u8 const xmm1 = X64_F1, xmm2 = X64_F2;
		x64_get(L, RAX, in->a, 8, tgExt_None);
		tg_movups_x_m(a, xmm1, tg_m(RAX, 0));
		if (e->form == tgVecForm_ShiftImm) {
			tg_enc(a, e->flags, e->opcode, e->ext, tg_r(xmm1), 1);
		} else if (e->form == tgVecForm_ToGpr) {
			tg_enc(a, e->flags, e->opcode, RAX, tg_r(xmm1), 0);
			tg_mov_rm_r(a, (e->flags & TG_W) ? 8 : 4, tg_mem_opnd(L, in->mem, R11), RAX);
			break;
		} else if (e->form == tgVecForm_Flags) {
			x64_get(L, RAX, in->b, 8, tgExt_None);
			tg_movups_x_m(a, xmm2, tg_m(RAX, 0));
			bool swap = (e->ext & tgVecCond_Swap) != 0;
			tg_enc(a, e->flags, e->opcode, swap ? xmm2 : xmm1, tg_r(swap ? xmm1 : xmm2), 0);
			tg_setcc(a, (tgCC)(e->ext & 0xF), RAX);
			if (e->ext & (tgVecCond_AndNP|tgVecCond_OrP)) {
				tg_setcc(a, (e->ext & tgVecCond_AndNP) ? CC_NP : CC_P, RCX);
				tg_alu_r_rm(a, (e->ext & tgVecCond_AndNP) ? ALU_AND : ALU_OR, 1, RAX, tg_r(RCX));
			}
			tg_load_ext(a, 1, false, RAX, tg_r(RAX));
			tg_mov_rm_r(a, 4, tg_mem_opnd(L, in->mem, R11), RAX);
			break;
		} else {
			x64_get(L, RAX, in->b, 8, tgExt_None);
			tg_movups_x_m(a, xmm2, tg_m(RAX, 0));
			if (e->form == tgVecForm_Xmm0) {
				x64_get(L, RAX, in->c, 8, tgExt_None);
				tg_movups_x_m(a, 0, tg_m(RAX, 0));
			}
			tg_enc(a, e->flags, e->opcode, xmm1, tg_r(xmm2), e->imm ? 1 : 0);
		}
		if (e->imm) tg_b(a, (u8)in->imm);
		tg_movups_m_x(a, tg_mem_opnd(L, in->mem, R11), xmm1);
		break;
	}
	case tgOp_Asm:
		tg_lower_asm(L, &p->asms.data[(isize)in->imm]);
		break;
	case tgOp_X87:
		x64_lower_x87(L, in);
		break;
	case tgOp_TlsAddr: {
		if (tg_is_darwin()) {
			// mov rdi, [rip + sym@TLVP]; call [rdi], the descriptor's getter returns the address in rax
			tg_enc(a, TG_W, 0x8B, RDI, tg_m_sym((i32)in->imm, 0, tgReloc_TLV), 0);
			tg_call_rm(a, tg_m(RDI, 0));
			x64_put(L, in->dst, RAX);
			break;
		}
		// the exact general dynamic sequence, which the linker may rewrite into a cheaper model
		tg_b(a, 0x66);
		tg_enc(a, TG_W, 0x8D, RDI, tg_m_sym((i32)in->imm, 0, tgReloc_TLSGD), 0);
		tg_b(a, 0x66);
		tg_b(a, 0x66);
		tg_b(a, 0x48);
		tg_call_sym(a, tg_lower_symbol(L->p->m, L->out, tg_str_lit("__tls_get_addr"), 0, 0));
		x64_put(L, in->dst, RAX);
		break;
	}
	case tgOp_Valgrind:
		x64_get(L, RDX, in->a, 8, tgExt_None);
		x64_get(L, RAX, in->b, 8, tgExt_None);
		// the magic preamble rotates rdi by 128 bits in total, then xchg rbx, rbx
		tg_shift_imm(a, 0, 8, tg_r(RDI), 3);
		tg_shift_imm(a, 0, 8, tg_r(RDI), 13);
		tg_shift_imm(a, 0, 8, tg_r(RDI), 61);
		tg_shift_imm(a, 0, 8, tg_r(RDI), 51);
		tg_enc(a, TG_W, 0x87, RBX, tg_r(RBX), 0);
		x64_put(L, in->dst, RDX);
		break;
	case tgOp_Alloca: {
		// the block goes above the outgoing argument area, which moves down with rsp
		// with over-aligned stack arguments rsp keeps their alignment, see the prologue
		i32 sp_align = tg_max(16, L->p->call_align);
		i32 args_area = (i32)x64_align_formula(L->max_call_stack, sp_align);
		tgOpnd n = x64_opnd(L, in->a, RAX, 8);
		tg_lea(a, RCX, tg_m(RSP, args_area));
		tg_alu_r_rm(a, ALU_SUB, 8, RCX, n);
		tg_alu_rm_imm(a, ALU_AND, 8, tg_r(RCX), -(i32)tg_max(in->imm, (i64)sp_align));
		if (tg_is_windows()) {
			// Win64 commits the stack a guard page at a time: touch every page down to the new rsp, like __chkstk
			// lea r10, [rcx-args_area]; mov r11, rsp; 1: sub r11, 4096; cmp r11, r10; jb 2f; test [r11], r11; jmp 1b; 2:
			tg_lea(a, R10, tg_m(RCX, -args_area));
			tg_mov_r_rm(a, 8, R11, tg_r(RSP));
			i64 loop = tg_pos(a);
			tg_alu_rm_imm(a, ALU_SUB, 8, tg_r(R11), 4096);
			tg_alu_r_rm(a, ALU_CMP, 8, R11, tg_r(R10));
			i64 done = tg_jcc32(a, CC_B);
			tg_test_rm_r(a, 8, tg_m(R11, 0), R11);
			tg_patch_rel32(a, tg_jmp32(a), loop);
			tg_patch_rel32(a, done, tg_pos(a));
		}
		tg_lea(a, RSP, tg_m(RCX, -args_area));
		x64_put(L, in->dst, RCX);
		break;
	}
	case tgOp_MulHiU:
		x64_get(L, RAX, in->a, 8, tgExt_None);
		tg_grp3(a, 4, 8, x64_opnd(L, in->b, RCX, 8)); // mul: rdx:rax
		x64_put(L, in->dst, RDX);
		break;
	case tgOp_MulOvf: {
		bool sgn = in->aux != 0;
		TG_ASSERT(size >= 4);
		x64_get(L, RAX, in->a, size, tgExt_None);
		tgOpnd rb = x64_opnd(L, in->b, RCX, size);
		if (sgn) {
			tg_imul_r_rm(a, size, RAX, rb);
			tg_setcc(a, CC_O, RCX);
		} else {
			tg_grp3(a, 4, size, rb); // mul: rdx:rax
			tg_setcc(a, CC_B, RCX);
		}
		tg_load_ext(a, 1, false, RCX, tg_r(RCX));
		x64_put(L, in->dst, RAX);
		x64_put(L, in->c, RCX);
		break;
	}
	case tgOp_Syscall: {
		tgCall const *c = &p->calls.data[(isize)in->imm];
		tg_lower_call(L, c, true);
		tg_syscall(a);
		x64_put(L, c->result_vreg, RAX);
		break;
	}
	case tgOp_Bswap: {
		u8 d = x64_dst(L, in->dst, RAX);
		x64_get(L, d, in->a, size, tgExt_None);
		if (size == 2) {
			tg_shift_imm(a, 0, 2, tg_r(d), 8); // rol ax, 8
		} else if (size > 2) {
			tg_bswap(a, size, d);
		}
		x64_put(L, in->dst, d);
		break;
	}
	case tgOp_Popcount:
		// SWAR popcount, no POPCNT needed
		x64_get(L, RAX, in->a, size, tgExt_Zero);
		tg_mov_r_rm(a, 8, RCX, tg_r(RAX));
		tg_shift_imm(a, 5, 8, tg_r(RCX), 1);
		tg_mov_r_imm(a, RDX, 0x5555555555555555ull);
		tg_alu_r_rm(a, ALU_AND, 8, RCX, tg_r(RDX));
		tg_alu_r_rm(a, ALU_SUB, 8, RAX, tg_r(RCX));
		tg_mov_r_imm(a, RDX, 0x3333333333333333ull);
		tg_mov_r_rm(a, 8, RCX, tg_r(RAX));
		tg_alu_r_rm(a, ALU_AND, 8, RCX, tg_r(RDX));
		tg_shift_imm(a, 5, 8, tg_r(RAX), 2);
		tg_alu_r_rm(a, ALU_AND, 8, RAX, tg_r(RDX));
		tg_alu_r_rm(a, ALU_ADD, 8, RAX, tg_r(RCX));
		tg_mov_r_rm(a, 8, RCX, tg_r(RAX));
		tg_shift_imm(a, 5, 8, tg_r(RCX), 4);
		tg_alu_r_rm(a, ALU_ADD, 8, RAX, tg_r(RCX));
		tg_mov_r_imm(a, RDX, 0x0F0F0F0F0F0F0F0Full);
		tg_alu_r_rm(a, ALU_AND, 8, RAX, tg_r(RDX));
		tg_mov_r_imm(a, RDX, 0x0101010101010101ull);
		tg_imul_r_rm(a, 8, RAX, tg_r(RDX));
		tg_shift_imm(a, 5, 8, tg_r(RAX), 56);
		x64_put(L, in->dst, RAX);
		break;
	case tgOp_Ctz:
	case tgOp_Clz: {
		// bsf/bsr leave the destination undefined for zero, so handle zero first
		x64_get(L, RCX, in->a, size, tgExt_Zero);
		tg_mov_r_imm(a, RAX, (u64)(8*size));
		tg_test_rm_r(a, 8, tg_r(RCX), RCX);
		i64 j_zero = tg_jcc32(a, CC_E);
		if (in->op == tgOp_Ctz) {
			tg_enc(a, TG_W|TG_0F, 0xBC, RAX, tg_r(RCX), 0); // bsf
		} else {
			tg_enc(a, TG_W|TG_0F, 0xBD, RAX, tg_r(RCX), 0); // bsr
			// clz = (size*8-1) - bsr
			tg_mov_r_imm(a, RDX, (u64)(8*size-1));
			tg_alu_r_rm(a, ALU_SUB, 8, RDX, tg_r(RAX));
			tg_mov_r_rm(a, 8, RAX, tg_r(RDX));
		}
		tg_patch_rel32(a, j_zero, tg_pos(a));
		x64_put(L, in->dst, RAX);
		break;
	}
	default:
		TG_PANIC("tg: cannot lower op");
	}
}

////////////////////////////////////////////////////////////////
// Procedures
////////////////////////////////////////////////////////////////

// Whether `in` is a compare whose result only feeds the next instruction `n` through the flags.
TG_INTERNAL bool x64_can_fuse(tgLower *L, tgInstr const *in, tgInstr const *n) {
	if (in->op == tgOp_FCmp) {
		// equality needs the parity flag too
		tgCond c = (tgCond)in->aux;
		if (c == tgCond_FEQ || c == tgCond_FNE) return false;
	} else if (in->op != tgOp_ICmp) {
		return false;
	}
	u32 v = in->dst;
	if (v == 0 || L->uses.data[v] != 1 || !L->in_block.data[v]) return false;
	switch (n->op) {
	case tgOp_Branch:
	case tgOp_Select:
		return n->a == v;
	case tgOp_ICmp: {
		i64 k = 0;
		tgCond c = (tgCond)n->aux;
		return n->a == v && (c == tgCond_EQ || c == tgCond_NE) &&
		       x64_is_const(L, n->b, &k) && x64_ext_value(k, tg_type_size(n->type), tgExt_Zero) == 0;
	}
	}
	return false;
}

// constants materialized at their uses and unread pure values emit nothing
TG_INTERNAL bool x64_skipped(tgLower *L, tgInstr const *n) {
	if (n->op == tgOp_IConst && n->dst != 0 && L->is_const.data[n->dst]) return true;
	return n->dst != 0 && L->uses.data[n->dst] == 0 && tg_op_is_pure(n->op);
}

// the next instruction that emits code
TG_INTERNAL isize x64_next_code(tgLower *L, tgBlock *b, isize i) {
	for (isize j = i+1; j < b->instrs.count; j++) {
		tgInstr const *n = &b->instrs.data[j];
		if (n->op == tgOp_Loc || n->op == tgOp_Scope || n->op == tgOp_Nop || x64_skipped(L, n)) continue;
		return j;
	}
	return -1;
}

TG_INTERNAL void x64_lower_proc(tgProc *p, tgLowerOut *out) {
	tgModule *m = p->m;
	tgLower L;
	memset(&L, 0, sizeof(L));
	L.p = p;
	L.out = out;
	L.a.m = m;
	L.a.code = &out->text;
	L.a.relocs = &out->relocs;
	tg_array_make_cap(&L.fixups, 0, 64);

	tg_lower_layout(&L);

	tgAsm *a = &L.a;
	// 16 byte aligned procedure starts, more when an asm template aligns its code
	out->align = 16;
	for (isize i = 0; i < p->asms.count; i++) out->align = tg_max(out->align, (i64)p->asms.data[i].align);
	L.proc_start = tg_pos(a);

	// the arrays of dbg live as long as the module, in its arena
	tgProcDebug dbg;
	tg_proc_debug_init(&dbg, &p->m->arena);
	dbg.name = p->debug_name.len > 0 ? p->debug_name : p->name;
	dbg.link_name = p->name;
	dbg.sym = p->sym;
	dbg.start = L.proc_start;
	dbg.file_id = p->decl_file;
	dbg.line = p->decl_line;
	dbg.line_entry_start = (i32)out->lines.count;
	dbg.type = p->debug_type;
	dbg.naked = p->naked;

	// prologue
	L.incoming_base = 16;
	tg_array_reserve(&dbg.saved_regs, L.saved_count);
	tg_array_reserve(&dbg.scope_marks, 16);
	if (p->naked) {
		TG_ASSERT(L.saved_count == 0 && p->locals.count == 0 && p->params_in.count == 0);
	} else if (tg_is_win64()) {
		tg_win64_prologue(&L, &dbg);
	} else {
		tg_push(a, RBP);
		tg_mov_rm_r(a, 8, tg_r(RBP), RSP);
		if (L.frame_size > 0) {
			tg_enc(a, TG_W, 0x81, 5, tg_r(RSP), 4); // sub rsp, imm32
			tg_u32(a, (u32)L.frame_size);
		}
		if (p->call_align > 16) {
			// stack arguments aligned past 16 bytes: the outgoing area at rsp is aligned for
			// them, the frame below rbp stays where it is
			tg_alu_rm_imm(a, ALU_AND, 8, tg_r(RSP), -p->call_align);
		}
		for (i32 i = 0; i < L.saved_count; i++) {
			u8 r = L.saved[i];
			tg_mov_rm_r(a, 8, tg_m(RBP, L.save_offset[i]), r);
			tgSavedReg s;
			s.dwarf_reg = x64_dwarf_reg(r);
			s.frame_offset = L.save_offset[i];
			tg_array_add(&dbg.saved_regs, s);
		}
	}
	dbg.saved_at = (i32)(tg_pos(a) - L.proc_start);
	for (isize i = 0; i < p->locals.count; i++) {
		tgLocal const *l = &p->locals.data[i];
		if (l->over_align <= 16) continue;
		// lea r11, [rbp + raw + align-1]; and r11, -align; mov [rbp + slot], r11
		tg_lea(a, R11, tg_m(RBP, l->raw_offset + (i32)l->over_align - 1));
		tg_alu_rm_imm(a, ALU_AND, 8, tg_r(R11), -(i32)l->over_align);
		tg_mov_rm_r(a, 8, tg_m(RBP, l->frame_offset), R11);
	}
	for (isize i = 0; i < p->params_in.count; i++) {
		tgParamIn const *in = &p->params_in.data[i];
		if (in->dst.kind == tgMem_Local && L.local_reg.data[in->dst.base] >= 0) continue;
		tgOpnd dst = tg_mem_opnd(&L, in->dst, R11);
		switch (in->loc) {
		case tgLoc_Gpr:
			tg_store_bytes_gpr(&L, dst, in->reg, in->size);
			break;
		case tgLoc_Xmm:
			tg_store_xmm(&L, dst, in->reg, in->size);
			break;
		case tgLoc_Stack:
			if (in->type == tgType_V128) {
				tg_movups_x_m(a, 15, tg_m(RBP, L.incoming_base + in->stack_offset));
				tg_movups_m_x(a, dst, 15);
			} else {
				tg_mov_r_rm(a, 8, RAX, tg_m(RBP, L.incoming_base + in->stack_offset));
				tg_store_bytes_gpr(&L, dst, RAX, in->size);
			}
			break;
		}
	}
	// the promoted ones stay in the register they arrive in or move to a callee saved one
	for (isize i = 0; i < p->params_in.count; i++) {
		tgParamIn const *in = &p->params_in.data[i];
		if (in->dst.kind != tgMem_Local || L.local_reg.data[in->dst.base] < 0) continue;
		i8 lr = L.local_reg.data[in->dst.base];
		tgOpnd src = tg_m(RBP, L.incoming_base + in->stack_offset);
		if (lr >= TG_FREG) {
			u8 x = (u8)(lr - TG_FREG);
			if (in->loc == tgLoc_Xmm) {
				x64_movaps(a, x, in->reg);
			} else {
				TG_ASSERT(in->loc == tgLoc_Stack);
				tg_movs_x_rm(a, in->size, x, src);
			}
		} else if (in->loc == tgLoc_Gpr) {
			x64_extend(a, tgExt_Zero, in->size, (u8)lr, in->reg);
		} else {
			TG_ASSERT(in->loc == tgLoc_Stack);
			tg_load_ext(a, in->size, false, (u8)lr, src);
		}
	}
	// like LLVM's prologue_end: a breakpoint on the procedure stops here, its parameters in place. That is
	// after the prologue's moves, or with tyg_debug_param after the front end's stores, at its marker.
	bool prologue_mark = tg_debug_info() && dbg.line > 0 && !p->naked;
	if (prologue_mark && p->params_ready == 0) {
		dbg.prologue_end = (i32)(tg_pos(a) - L.proc_start);
		tg_b(a, 0x90);
	}

	// cold blocks go last, so the hot path falls through
	tgBlockPtrArray order;
	memset(&order, 0, sizeof(order));
	order.arena = tg_arena();
	tg_array_reserve(&order, p->order.count);
	for (isize i = 0; i < p->order.count; i++) if (!p->order.data[i]->cold) tg_array_add(&order, p->order.data[i]);
	for (isize i = 0; i < p->order.count; i++) if (p->order.data[i]->cold)  tg_array_add(&order, p->order.data[i]);

	for (isize bi = 0; bi < order.count; bi++) {
		tgBlock *b = order.data[bi];
		i32 next_block = bi+1 < order.count ? order.data[bi+1]->index : -1;
		b->code_offset = (i32)tg_pos(a);
		tgScopeMark mark;
		mark.code_offset = (i32)(tg_pos(a) - L.proc_start);
		mark.scope = b->debug_scope;
		mark.cold = b->cold;
		tg_array_add(&dbg.scope_marks, mark);
		for (isize i = 0; i < b->instrs.count; i++) {
			tgInstr const *in = &b->instrs.data[i];
			if (in->op == tgOp_Scope) {
				tgScopeMark smark;
				smark.code_offset = (i32)(tg_pos(a) - L.proc_start);
				smark.scope = (i32)in->imm;
				smark.cold = b->cold;
				tg_array_add(&dbg.scope_marks, smark);
				continue;
			}
			// a jump to the next block is a fallthrough, but like LLVM's a block with no other code keeps it for
			// its line, except the first, which LLVM fills with the parameters' stores
			if (in->op == tgOp_Jump && i+1 == b->instrs.count && (i32)in->imm == next_block &&
			    !(tg_debug_info() && bi > 0 && tg_pos(a) == b->code_offset)) {
				continue;
			}
			if (x64_skipped(&L, in)) continue;
			if (in->op == tgOp_Nop && in->imm == 2) {
				if (prologue_mark) {
					dbg.prologue_end = (i32)(tg_pos(a) - L.proc_start);
					tg_b(a, 0x90);
				}
				continue;
			}
			if (in->op == tgOp_Loc || in->op == tgOp_Nop) {
				tg_lower_instr(&L, in);
				continue;
			}
			L.next_block = i+1 == b->instrs.count ? next_block : -1;
			L.fuse = false;
			if (in->op == tgOp_ICmp || in->op == tgOp_FCmp) {
				isize j = x64_next_code(&L, b, i);
				L.fuse = j >= 0 && x64_can_fuse(&L, in, &b->instrs.data[j]);
			}
			u32 flags = L.flags_vreg;
			// only the instruction right after a store sees it
			L.stored.valid = L.stored.fresh;
			L.stored.fresh = false;
			tg_lower_instr(&L, in);
			TG_ASSERT_MSG(flags == 0 || L.flags_vreg != flags, "tg: an op did not read the flags");
		}
		memset(&L.stored, 0, sizeof(L.stored));
		TG_ASSERT(L.flags_vreg == 0);
	}
	for (isize i = 0; i < L.fixups.count; i++) {
		tgLowerFixup const *f = &L.fixups.data[i];
		tgBlock *target = p->blocks.data[f->block];
		TG_ASSERT_MSG(target->placed, "jump to an unplaced block");
		tg_patch_rel32(a, f->at, target->code_offset);
	}

	dbg.end = tg_pos(a);
	dbg.line_entry_count = (i32)(out->lines.count - dbg.line_entry_start);

	// debug variables, now that frame offsets are known
	tg_array_reserve(&dbg.vars, p->debug_vars.count);
	tg_array_reserve(&dbg.scope_parent, p->debug_scope_parent.count);
	tg_array_add_elems(&dbg.scope_parent, p->debug_scope_parent.data, p->debug_scope_parent.count);
	tg_array_reserve(&dbg.inline_sites, p->inline_sites.count);
	tg_array_add_elems(&dbg.inline_sites, p->inline_sites.data, p->inline_sites.count);
	for (isize i = 0; i < p->debug_vars.count; i++) {
		tgDebugVar v = p->debug_vars.data[i];
		if (v.local >= 0) {
			tgLocal const *l = &p->locals.data[v.local];
			if (l->over_align > 16) {
				// the slot holds the variable's address
				if (v.by_ref || v.frame_offset_fixup != 0) continue;
				v.by_ref = true;
			}
			v.frame_offset_fixup += l->frame_offset;
			if (L.local_reg.data[v.local] >= 0) {
				v.in_reg = true;
				i8 lr = L.local_reg.data[v.local];
				v.dwarf_reg = lr >= TG_FREG ? (u8)(17 + lr - TG_FREG) : x64_dwarf_reg((u8)lr); // xmm0 is 17
			}
		}
		tg_array_add(&dbg.vars, v);
	}
	out->dbg = dbg;

	tg_array_free(&order);
	tg_array_free(&L.fixups);
	tg_array_free(&L.slot);
	tg_array_free(&L.uses);
	tg_array_free(&L.reg);
	tg_array_free(&L.is_const);
	tg_array_free(&L.cval);
	tg_array_free(&L.in_block);
	tg_array_free(&L.local_reg);
	tg_array_free(&L.remat);
	tg_array_free(&L.rmem);
	tg_array_free(&L.zx);
}

////////////////////////////////////////////////////////////////
// TODO(port): defined by x64_asm.c later
//
// Lays out an asm template's items at `start` into `code`. Delete this when the asm template
// machinery is ported, if ever; nothing in the public API makes a tgOp_Asm yet.
////////////////////////////////////////////////////////////////

TG_INTERNAL bool tg_asm_layout(tgAsmItemSlice items, tgU8Slice pool, i32 label_count, i64 start, tgBytes *code) {
	(void)items; (void)pool; (void)label_count; (void)start; (void)code;
	TG_PANIC("tg_asm_layout: asm templates are not ported yet");
}
