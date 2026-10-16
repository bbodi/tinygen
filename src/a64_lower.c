// Lowering: tg IR -> arm64 machine code.
//
// Registers come from tg_alloc_regs (analysis.c). An interval takes x0-x8, v0-v7 or v20-v31
// when no call happens in it, x19-x28 or d8-d15 otherwise. The rest live in 8 byte stack slots.
// A constant with one definition is materialized at each use, or becomes the instruction's
// immediate; a frame address is computed at each use, or becomes the access's offset. A compare
// whose only use is the next branch, select or test against zero only sets the flags.
//
// The scratch registers are x9-x15, x16 for addresses and x17 for large offsets, and
// v16-v19 for floats.
//
// Frame (macOS requires the frame pointer):
//   [x29+16 ...]  incoming stack arguments
//   [x29+8]       saved x30
//   [x29]         saved x29
//   [x29-N ...]   callee saved pairs, where compact unwind expects them: x19 at x29-8
//   [...]         vreg slots, nearest the frame pointer where offsets are short, then locals
//   [sp ...]      outgoing stack arguments

enum {
	A64_T0 = X9,
	A64_T1 = X10,
	A64_T2 = X11,
	A64_T3 = X12,
	A64_T4 = X13,
	A64_T5 = X14,
	A64_TB = X15, // assembles odd sized loads and stores
	A64_TA = X16, // memory addresses and indirect call targets

	A64_F0 = 16,
	A64_F1 = 17,
	A64_F2 = 18,
};

// Copies and zeroing up to these sizes are inline, larger ones call the C library.
enum {
	A64_INLINE_COPY = 64,
	A64_INLINE_ZERO = 256,
};

// Loop heads start on this many bytes, and so do procedures (a64_loop_heads).
enum { A64_LOOP_ALIGN = 32 };

typedef struct a64Fixup { i64 at; i32 block; bool imm19; } a64Fixup;
TG_ARRAY(a64FixupArray, a64Fixup);

typedef struct a64Lower {
	tgProc *     p;
	tgLowerOut * out;
	tgAsm        a;
	tgI32Array   slot;     // vreg -> x29 offset (negative)
	tgI32Array   uses;     // vreg -> number of reads
	tgI8Array    reg;      // vreg -> the register holding it for its whole life, or TG_NOREG
	tgBytes      is_const; // vreg -> defined once by an IConst, materialized at its uses
	tgI64Array   cval;
	tgBytes      in_block; // vreg -> defined once, and only read later in the same block
	tgBytes      clean;    // vreg -> its register holds 0 or 1
	tgI8Array    local_reg;
	tgI32Array   via;      // vreg -> the local whose register it shares, or -1
	tgBytes      remat;    // vreg -> defined once as a frame address, computed at each use from rmem
	tgMemArray   rmem;
	tgBytes      zx;       // vreg -> n when its register is known zero above its low n bytes, else 0
	u32          pairs;    // the saved callee saved pairs, as compact unwind flags
	i32          frame_size;
	i32          max_call_stack;
	bool         sp_moves;  // an alloca moves sp, so only x29 reaches the frame
	a64FixupArray fixups;
	i64          proc_start;
	i32          next_block;
	bool         far_branches; // conditional branches skip over a `b`, the procedure is too long for theirs
	bool         fuse;     // the current compare only sets the flags for the next instruction
	bool         fold_addr; // the current add only forms the address of the next load or store
	u32          addr_vreg; // that address, its two terms in addr_x and addr_y
	u32          addr_x, addr_y;
	u32          shift_vreg; // a shl left to the next add or sub: its result, source and count
	u32          shift_src, shift_amt;
	u32          flags_vreg;
	a64Cond      flags_cond;
	// what the previous instruction stored, still in a register for a load right after it
	tgLowerStored stored;
} a64Lower;

typedef struct a64Addr {
	u8  base;
	i64 off;
} a64Addr;

// Whether a thread local's address comes from a call: macOS calls the variable's TLV
// descriptor, and on Linux anything but an executable may be loaded with dlopen, so it calls
// the TLS descriptor's resolver (general dynamic) like LLVM. An executable adds the offset in
// the variable's GOT entry to the thread pointer (initial exec). tinygen does not know the
// build mode, so it takes position independent code as the sign of a shared library, as
// tg_tls_mem in ir.c does.
TG_INTERNAL bool a64_tls_calls(void) {
	return tg_is_darwin() || tg_is_pic();
}

// Whether the lowering of `in` calls something or writes x0-x8, which a vreg in those
// registers or in v0-v7/v20-v31 cannot live through.
TG_INTERNAL bool a64_clobbers(tgInstr const *in) {
	switch (in->op) {
	case tgOp_TlsAddr:
		return a64_tls_calls();
	case tgOp_Call:
	case tgOp_Ret:
	case tgOp_Syscall:
	case tgOp_MemCopyDyn:
	case tgOp_MemMoveDyn:
	case tgOp_MemSetDyn:
		return true;
	case tgOp_MemCopy:
	case tgOp_MemMove:
		return in->imm > A64_INLINE_COPY;
	case tgOp_MemZero:
		return in->imm > A64_INLINE_ZERO;
	}
	return false;
}

// One saved pair: lo at x29+off+8, hi at x29+off.
typedef struct a64Pair { u8 lo, hi; bool fp; i32 off; } a64Pair;

TG_INTERNAL i32 a64_list_pairs(u32 pairs, a64Pair out[9]) {
	i32 n = 0;
	i32 off = 0;
	for (u32 k = 0; k < 5; k++) {
		if (pairs & (1u << k)) {
			off -= 16;
			a64Pair pr;
			pr.lo = (u8)(X19 + 2*k);
			pr.hi = (u8)(X20 + 2*k);
			pr.fp = false;
			pr.off = off;
			out[n++] = pr;
		}
	}
	for (u32 k = 0; k < 4; k++) {
		if (pairs & (0x100u << k)) {
			off -= 16;
			a64Pair pr;
			pr.lo = (u8)(8 + 2*k);
			pr.hi = (u8)(9 + 2*k);
			pr.fp = true;
			pr.off = off;
			out[n++] = pr;
		}
	}
	return n;
}

// A return writes x0-x8 only on its way out, after which nothing runs, so the vregs and locals
// it does not read may live in them through it. One register value moves out safely from any
// register; with more, a value in another's return register could be overwritten first.
TG_INTERNAL bool a64_target_clobbers(void *ctx, tgInstr const *in) {
	tgProc *p = (tgProc *)ctx;
	if (in->op == tgOp_Ret) {
		tgCall const *c = &p->calls.data[(isize)in->imm];
		i32 regs = 0;
		for (isize i = 0; i < c->args.count; i++) {
			tgCallArgKind k = c->args.data[i].kind;
			if (k == tgCallArg_Gpr || k == tgCallArg_Xmm || k == tgCallArg_GprMem || k == tgCallArg_XmmMem) regs++;
		}
		return regs > 1;
	}
	return a64_clobbers(in);
}

TG_INTERNAL void a64_target_wants(void *ctx, tgProc *p, tgInstr const *in, tgNoteWantProc note, void *note_ctx) {
	(void)ctx;
	switch (in->op) {
	case tgOp_Call:
	case tgOp_Ret:
	case tgOp_Syscall: {
		tgCall const *c = &p->calls.data[(isize)in->imm];
		for (isize i = 0; i < c->args.count; i++) {
			tgCallArg const *arg = &c->args.data[i];
			if (arg->kind == tgCallArg_Gpr && arg->reg <= X8) note(note_ctx, arg->vreg, arg->reg);
			if (arg->kind == tgCallArg_Xmm && arg->reg < 8)   note(note_ctx, arg->vreg, TG_FREG + arg->reg);
		}
		break;
	}
	case tgOp_MemCopyDyn:
	case tgOp_MemMoveDyn:
	case tgOp_MemSetDyn:
		note(note_ctx, in->a, X0);
		note(note_ctx, in->b, X1);
		note(note_ctx, in->c, X2);
		break;
	}
}

// The ops whose lowering reads every operand before its one write of the result.
TG_INTERNAL bool a64_target_reads_first(void *ctx, tgInstr const *in) {
	(void)ctx;
	switch (in->op) {
	case tgOp_Copy:
	case tgOp_Trunc:
	case tgOp_Zext:
	case tgOp_Sext:
	case tgOp_Add:
	case tgOp_Sub:
	case tgOp_And:
	case tgOp_Or:
	case tgOp_Xor:
	case tgOp_Mul:
	case tgOp_Shl:
	case tgOp_LShr:
	case tgOp_AShr:
	case tgOp_Neg:
	case tgOp_Not:
	case tgOp_FAdd:
	case tgOp_FSub:
	case tgOp_FMul:
	case tgOp_FDiv:
	case tgOp_FNeg:
		return true;
	}
	return false;
}

TG_INTERNAL void a64_lower_layout(a64Lower *L) {
	tgProc *p = L->p;

	// x0-x8 and v0-v7, v20-v31 are caller saved, x19-x28 and d8-d15 callee saved
	tgRegPools pools;
	memset(&pools, 0, sizeof(pools));
	pools.caller_x = 0x1ff;
	pools.callee_x = 0x1ff80000;
	pools.caller_v = 0xfff000ff;
	pools.callee_v = 0x0000ff00;
	pools.callee_weight = 0;
	tgRegAlloc R;
	memset(&R, 0, sizeof(R));
	tgRegTarget target;
	target.ctx = p;
	target.clobbers = a64_target_clobbers;
	target.wants = a64_target_wants;
	target.reads_first = a64_target_reads_first;
	tg_alloc_regs(p, &R, pools, &target);
	L->uses      = R.uses;
	L->reg       = R.reg;
	L->is_const  = R.is_const;
	L->cval      = R.cval;
	L->in_block  = R.in_block;
	L->clean     = R.clean;
	L->local_reg = R.local_reg;
	L->via       = R.via;
	L->remat     = R.remat;
	L->rmem      = R.rmem;
	L->max_call_stack = R.max_call_stack;
	L->sp_moves  = R.sp_moves;

	// A narrow load fills its register zero extended, a zext leaves it zero above the source, and a
	// copy of either is too. A copy comes after its source here, as tg_reuse_loads makes them.
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
			} else if ((in->op == tgOp_Copy || in->op == tgOp_Trunc) && tg_type_is_int(in->type)) {
				L->zx.data[in->dst] = L->zx.data[in->a]; // a copy, or a truncation, moves the whole register
			} else if (in->op == tgOp_LShr && tg_type_is_int(in->type) && in->b != 0 && R.is_const.data[in->b]) {
				// it shifts its operand zero extended from the type's size
				i64 k = R.cval.data[in->b];
				i32 size = tg_type_size(in->type);
				if (k > 0 && k % 8 == 0 && k < 8*size) L->zx.data[in->dst] = (u8)(size - k/8);
			}
		}
	}

	L->pairs = 0;
	for (u32 k = 0; k < 5; k++) {
		if (R.used_x & (3u << (X19 + 2*k))) L->pairs |= 1u << k;
	}
	for (u32 k = 0; k < 4; k++) {
		if (R.used_v & (3u << (8 + 2*k))) L->pairs |= 0x100u << k;
	}
	i32 cur = 0;
	a64Pair prs[9];
	i32 npairs = a64_list_pairs(L->pairs, prs);
	if (npairs > 0) cur = -prs[npairs-1].off;

	// the other vregs used only in the block that defines them share slots
	tg_alloc_slots(p, &R, &cur);
	L->slot = R.slot;

	for (isize i = 0; i < p->locals.count; i++) {
		tgLocal *l = &p->locals.data[i];
		if (l->over_align > 16) {
			// the frame is only 16 byte aligned, the prologue picks an aligned spot in the raw area
			cur = (i32)tg_round_up(cur + l->size + l->over_align - 16, 16);
			l->raw_offset = -cur;
			cur = (i32)tg_round_up(cur + 8, 8);
			l->frame_offset = -cur;
			continue;
		}
		cur = (i32)tg_round_up(cur + l->size, l->align);
		l->frame_offset = -cur;
	}

	L->frame_size = (i32)tg_round_up(cur, 16) + (i32)tg_round_up(L->max_call_stack, 16);
}

////////////////////////////////////////////////////////////////
// Operands
////////////////////////////////////////////////////////////////

// The frame at x29+off for an access of `size` bytes, 0 when not known. Far below x29,
// sp reaches it with a short scaled offset instead.
TG_INTERNAL a64Addr a64_frame(a64Lower *L, i64 off, i32 size) {
	a64Addr r;
	r.base = A64_FP;
	r.off = off;
	if (off >= -256 || L->sp_moves) return r;
	i64 s = L->frame_size + off;
	if (s >= 0 && (size == 0 || (s % size == 0 && s / size < 4096))) {
		r.base = A64_SP;
		r.off = s;
	}
	return r;
}

// v's value of `size` bytes extended as `ext` asks
TG_INTERNAL i64 a64_ext_value(i64 v, i32 size, tgExtKind ext) {
	if (size >= 8 || ext == tgExt_None) return v;
	u64 mask = (1ull << (8*size)) - 1;
	u64 u = (u64)v & mask;
	if (ext == tgExt_Sign && (u >> (8*size - 1))) u |= ~mask;
	return (i64)u;
}

TG_INTERNAL bool a64_is_const(a64Lower *L, u32 v, i64 *value) {
	if (v == 0 || !L->is_const.data[v]) return false;
	*value = L->cval.data[v];
	return true;
}

// `scratch` was a default argument of A64_TA.
TG_INTERNAL a64Addr a64_mem(a64Lower *L, tgMem m, u8 scratch);

// An x register holding v: its own, or `scratch` loaded with it. Narrow values come
// extended as `ext` asks; with tgExt_None only the low `size` bytes mean anything.
TG_INTERNAL u8 a64_src(a64Lower *L, u32 v, u8 scratch, i32 size, tgExtKind ext) {
	TG_ASSERT(v != 0);
	tgAsm *a = &L->a;
	bool extend = size < 8 && ext != tgExt_None;
	if (L->is_const.data[v]) {
		a64_mov_imm(a, scratch, (u64)a64_ext_value(L->cval.data[v], size, ext));
		return scratch;
	}
	if (L->remat.data[v]) {
		a64Addr m = a64_mem(L, L->rmem.data[v], scratch);
		a64_add_imm(a, scratch, m.base, m.off);
		return scratch;
	}
	i8 r = L->reg.data[v];
	if (r >= TG_FREG) {
		a64_fmov_from_fp(a, scratch, (u8)(r - TG_FREG));
		if (extend) a64_extend(a, ext == tgExt_Sign, size, scratch, scratch);
		return scratch;
	}
	if (r != TG_NOREG) {
		// zero above fewer bytes than `size`, it reads the same sign extended
		u8 zx = L->zx.data[v];
		if (!extend || (zx != 0 && (ext == tgExt_Zero ? zx <= size : zx < size))) return (u8)r;
		a64_extend(a, ext == tgExt_Sign, size, scratch, (u8)r);
		return scratch;
	}
	a64Addr f = a64_frame(L, L->slot.data[v], size);
	a64_ldr(a, size, ext == tgExt_Sign, scratch, f.base, f.off);
	return scratch;
}

// Like a64_src, into exactly `reg`.
TG_INTERNAL void a64_get(a64Lower *L, u8 reg, u32 v, i32 size, tgExtKind ext) {
	u8 r = a64_src(L, v, reg, size, ext);
	if (r != reg) a64_mov(&L->a, reg, r);
}

// A v register holding v's `size` byte float: its own, or `scratch` loaded with it.
TG_INTERNAL u8 a64_srcf(a64Lower *L, u32 v, u8 scratch, i32 size) {
	TG_ASSERT(v != 0);
	tgAsm *a = &L->a;
	if (L->is_const.data[v]) {
		a64_mov_imm(a, X17, (u64)L->cval.data[v]);
		a64_fmov_to_fp(a, scratch, X17);
		return scratch;
	}
	i8 r = L->reg.data[v];
	if (r >= TG_FREG) return (u8)(r - TG_FREG);
	if (r != TG_NOREG) {
		a64_fmov_to_fp(a, scratch, (u8)r);
		return scratch;
	}
	a64Addr f = a64_frame(L, L->slot.data[v], size);
	a64_ldr_fp(a, size, scratch, f.base, f.off);
	return scratch;
}

TG_INTERNAL void a64_getf(a64Lower *L, u8 vr, u32 v, i32 size) {
	u8 r = a64_srcf(L, v, vr, size);
	if (r != vr) a64_fmov_reg(&L->a, vr, r);
}

// The x register to compute v into: its own, or `scratch`. a64_put finishes the definition.
TG_INTERNAL u8 a64_dst(a64Lower *L, u32 v, u8 scratch) {
	i8 r = L->reg.data[v];
	return r != TG_NOREG && r < TG_FREG ? (u8)r : scratch;
}

TG_INTERNAL u8 a64_dstf(a64Lower *L, u32 v, u8 scratch) {
	i8 r = L->reg.data[v];
	return r >= TG_FREG ? (u8)(r - TG_FREG) : scratch;
}

// Defines v from the x register `reg`.
TG_INTERNAL void a64_put(a64Lower *L, u32 v, u8 reg) {
	TG_ASSERT(v != 0 && !L->is_const.data[v]);
	tgAsm *a = &L->a;
	i8 r = L->reg.data[v];
	if (r >= TG_FREG) {
		a64_fmov_to_fp(a, (u8)(r - TG_FREG), reg);
	} else if (r != TG_NOREG) {
		if (r != reg) a64_mov(a, (u8)r, reg);
	} else {
		a64Addr f = a64_frame(L, L->slot.data[v], 8);
		a64_str(a, 8, reg, f.base, f.off);
	}
}

// Defines v from the `size` byte float in the v register `vr`.
TG_INTERNAL void a64_putf(a64Lower *L, u32 v, u8 vr, i32 size) {
	TG_ASSERT(v != 0 && !L->is_const.data[v]);
	tgAsm *a = &L->a;
	i8 r = L->reg.data[v];
	if (r >= TG_FREG) {
		if (r - TG_FREG != vr) a64_fmov_reg(a, (u8)(r - TG_FREG), vr);
	} else if (r != TG_NOREG) {
		a64_fmov_from_fp(a, (u8)r, vr);
	} else {
		a64Addr f = a64_frame(L, L->slot.data[v], size);
		a64_str_fp(a, size, vr, f.base, f.off);
	}
}

// A base register and offset for an IR memory reference. May clobber `scratch` and x17.
TG_INTERNAL a64Addr a64_mem(a64Lower *L, tgMem m, u8 scratch) {
	tgAsm *a = &L->a;
	a64Addr r;
	memset(&r, 0, sizeof(r));
	switch (m.kind) {
	case tgMem_Local: {
		TG_ASSERT(L->local_reg.data[m.base] < 0);
		tgLocal const *l = &L->p->locals.data[m.base];
		if (l->over_align > 16) {
			a64Addr f = a64_frame(L, l->frame_offset, 8);
			a64_ldr(a, 8, false, scratch, f.base, f.off);
			r.base = scratch;
			r.off = m.offset;
			return r;
		}
		return a64_frame(L, l->frame_offset + m.offset, 0);
	}
	case tgMem_Incoming:
		r.base = A64_FP;
		r.off = 16 + m.offset;
		return r;
	case tgMem_Reg:
		if (L->remat.data[m.base]) {
			// the frame address folds into the access
			tgMem f = L->rmem.data[m.base];
			f.offset += m.offset;
			return a64_mem(L, f, scratch);
		}
		r.base = a64_src(L, m.base, scratch, 8, tgExt_None);
		r.off = m.offset;
		return r;
	case tgMem_Sym: {
		i32 sym = (i32)m.base;
		u8 flags = tg_lower_sym_flags(L->p->m, sym);
		TG_ASSERT_MSG((flags & tgSymbolFlag_TLS) == 0, "a64: a thread local symbol is reached through tgOp_TlsAddr");
		// like x64_lower.c: an exported symbol of a shared library may be preempted, its address is in the GOT
		bool preemptible = (flags & tgSymbolFlag_Export) && tg_is_pic();
		if (((flags & tgSymbolFlag_Foreign) && (flags & tgSymbolFlag_Undef)) || preemptible) {
			// it may live in a dylib
			a64_adrp(a, scratch, sym, tgReloc_A64_GotPage21);
			a64_ldr_pageoff(a, scratch, scratch, sym, tgReloc_A64_GotPageOff12);
		} else {
			a64_adrp(a, scratch, sym, tgReloc_A64_Page21);
			a64_add_pageoff(a, scratch, scratch, sym);
		}
		r.base = scratch;
		r.off = m.offset;
		return r;
	}
	}
	TG_PANIC("a64: bad mem");
	return r;
}

// Loads `size` (1..8) bytes into xt, zero extended unless `ext` asks for a sign extension.
TG_INTERNAL void a64_load_bytes(a64Lower *L, u8 rt, a64Addr m, i32 size, tgExtKind ext) {
	tgAsm *a = &L->a;
	switch (size) {
	case 1: case 2: case 4: case 8:
		a64_ldr(a, size, ext == tgExt_Sign, rt, m.base, m.off);
		return;
	}
	TG_ASSERT(rt != A64_TB && m.base != A64_TB);
	i32 lo = size > 4 ? 4 : 2;
	a64_ldr(a, lo, false, rt, m.base, m.off);
	i32 at = lo;
	while (at < size) {
		i32 c = size - at >= 2 ? 2 : 1;
		a64_ldr(a, c, false, A64_TB, m.base, m.off + at);
		a64_alu(a, A64_ORR, rt, rt, A64_TB, A64_LSL, (u32)(8*at));
		at += c;
	}
}

// Stores the low `size` (1..8) bytes of xt; xt is left as it was.
TG_INTERNAL void a64_store_bytes(a64Lower *L, a64Addr m, u8 rt, i32 size) {
	tgAsm *a = &L->a;
	switch (size) {
	case 1: case 2: case 4: case 8:
		a64_str(a, size, rt, m.base, m.off);
		return;
	}
	TG_ASSERT(m.base != A64_TB);
	a64_mov(a, A64_TB, rt);
	i32 at = 0;
	while (at < size) {
		i32 rem = size - at;
		i32 c = rem >= 4 ? 4 : rem >= 2 ? 2 : 1;
		a64_str(a, c, A64_TB, m.base, m.off + at);
		a64_lsr_imm(a, A64_TB, A64_TB, (u32)(8*c));
		at += c;
	}
}

TG_INTERNAL a64Cond a64_cond_for(tgCond c) {
	switch (c) {
	case tgCond_EQ:  return A64_EQ;
	case tgCond_NE:  return A64_NE;
	case tgCond_SLT: return A64_LT;
	case tgCond_SLE: return A64_LE;
	case tgCond_SGT: return A64_GT;
	case tgCond_SGE: return A64_GE;
	case tgCond_ULT: return A64_LO;
	case tgCond_ULE: return A64_LS;
	case tgCond_UGT: return A64_HI;
	case tgCond_UGE: return A64_HS;
	// after fcmp an unordered result sets C and V, which every one of these rejects but NE
	case tgCond_FEQ: return A64_EQ;
	case tgCond_FNE: return A64_NE;
	case tgCond_FLT: return A64_MI;
	case tgCond_FLE: return A64_LS;
	case tgCond_FGT: return A64_GT;
	case tgCond_FGE: return A64_GE;
	}
	TG_PANIC("a64: bad cond");
	return A64_EQ;
}

// The exact negation, also for an unordered float compare.
TG_INTERNAL a64Cond a64_invert(a64Cond c) {
	return (a64Cond)(c ^ 1);
}

TG_INTERNAL bool a64_cond_is_signed(tgCond c) {
	return c == tgCond_SLT || c == tgCond_SLE || c == tgCond_SGT || c == tgCond_SGE;
}

////////////////////////////////////////////////////////////////
// Calls
////////////////////////////////////////////////////////////////

// No vreg lives in x0-x8 or v0-v7 here, so arguments go straight into their registers.
TG_INTERNAL void a64_lower_call(a64Lower *L, tgCall const *c, bool is_ret) {
	tgAsm *a = &L->a;
	// stack arguments first, the argument registers are not touched yet
	for (isize i = 0; i < c->args.count; i++) {
		tgCallArg const *arg = &c->args.data[i];
		switch (arg->kind) {
		case tgCallArg_Stack: {
			tgType t = arg->type;
			if (tg_type_is_float(t)) {
				u8 r = a64_srcf(L, arg->vreg, A64_F0, tg_type_size(t));
				a64_str_fp(a, tg_type_size(t), r, A64_SP, arg->stack_offset);
			} else {
				u8 r = a64_src(L, arg->vreg, A64_T0, tg_type_size(t), arg->ext == tgExt_Sign ? tgExt_Sign : tgExt_Zero);
				a64_str(a, a64_slot_size(arg->size), r, A64_SP, arg->stack_offset);
			}
			break;
		}
		case tgCallArg_StackMem: {
			a64Addr src = a64_mem(L, arg->mem, A64_TA);
			if (arg->size == 16) {
				a64_ldr_fp(a, 16, A64_F0, src.base, src.off);
				a64_str_fp(a, 16, A64_F0, A64_SP, arg->stack_offset);
			} else {
				TG_ASSERT_MSG(arg->size <= 8, "a64: a stack argument of more than 8 bytes");
				a64_load_bytes(L, A64_T0, src, arg->size, arg->ext);
				a64_str(a, a64_slot_size(arg->size), A64_T0, A64_SP, arg->stack_offset);
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
			a64_get(L, arg->reg, arg->vreg, size, ext);
			break;
		}
		case tgCallArg_Xmm:
			a64_getf(L, arg->reg, arg->vreg, tg_type_size(arg->type));
			break;
		case tgCallArg_GprMem: {
			tgExtKind ext = arg->ext;
			if (arg->size < 4 && ext == tgExt_None) ext = tgExt_Zero;
			if (arg->mem.kind == tgMem_Local && L->local_reg.data[arg->mem.base] >= 0) {
				// the register is callee saved, the call is in its interval
				a64_extend(a, ext == tgExt_Sign, arg->size, arg->reg, (u8)L->local_reg.data[arg->mem.base]);
				break;
			}
			a64Addr src = a64_mem(L, arg->mem, A64_TA);
			a64_load_bytes(L, arg->reg, src, arg->size, ext);
			break;
		}
		case tgCallArg_XmmMem: {
			if (arg->mem.kind == tgMem_Local && L->local_reg.data[arg->mem.base] >= 0) {
				// a scalar local in a register
				i8 lr = L->local_reg.data[arg->mem.base];
				if (lr >= TG_FREG) a64_fmov_reg(a, arg->reg, (u8)(lr - TG_FREG));
				else               a64_fmov_to_fp(a, arg->reg, (u8)lr);
				break;
			}
			a64Addr src = a64_mem(L, arg->mem, A64_TA);
			a64_ldr_fp(a, arg->size, arg->reg, src.base, src.off);
			break;
		}
		}
	}
	if (is_ret) {
		return;
	}
	if (c->target_sym >= 0) {
		a64_bl_sym(a, c->target_sym);
	} else {
		a64_blr(a, a64_src(L, c->target_vreg, A64_TA, 8, tgExt_None));
	}
	for (isize i = 0; i < c->rets.count; i++) {
		tgCallRet const *r = &c->rets.data[i];
		if (r->dst.kind == tgMem_Local && L->local_reg.data[r->dst.base] >= 0) {
			i8 lr = L->local_reg.data[r->dst.base];
			if (lr >= TG_FREG) {
				// a float local lives in a v register: the value moves there from an x or v register
				u8 vr = (u8)(lr - TG_FREG);
				if (r->loc == tgLoc_Gpr) a64_fmov_to_fp(a, vr, r->reg);
				else if (vr != r->reg)   a64_fmov_reg(a, vr, r->reg);
			} else if (r->loc == tgLoc_Gpr) {
				a64_extend(a, false, r->size, (u8)lr, r->reg);
			} else {
				a64_fmov_from_fp(a, (u8)lr, r->reg);
			}
			continue;
		}
		a64Addr dst = a64_mem(L, r->dst, A64_TA);
		if (r->loc == tgLoc_Gpr) {
			a64_store_bytes(L, dst, r->reg, r->size);
		} else {
			a64_str_fp(a, r->size, r->reg, dst.base, dst.off);
		}
	}
	if (c->rets.count == 1 && !(c->rets.data[0].dst.kind == tgMem_Local && L->local_reg.data[c->rets.data[0].dst.base] >= 0)) {
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

// memmove(x0, x1, x2) or memset(x0, w1, x2), from the C library like LLVM's lowering
TG_INTERNAL void a64_call_libc(a64Lower *L, char const *name) {
	a64_bl_sym(&L->a, tg_lower_symbol(L->p->m, L->out, tg_str(name), tgSymbolFlag_Func | tgSymbolFlag_Foreign, 0));
}

TG_INTERNAL void a64_epilogue(a64Lower *L) {
	tgAsm *a = &L->a;
	a64Pair prs[9];
	i32 n = a64_list_pairs(L->pairs, prs);
	for (i32 i = 0; i < n; i++) {
		a64_pair(a, prs[i].fp, true, prs[i].hi, prs[i].lo, A64_FP, prs[i].off);
	}
	a64_mov_sp(a, A64_SP, A64_FP);
	a64_emit(a, 0xA8C17BFD); // ldp x29, x30, [sp], #16
	a64_ret(a);
}

// Jumps to `block`, patched once every block is placed.
TG_INTERNAL void a64_jump(a64Lower *L, i32 block) {
	a64Fixup f;
	f.at = a64_b(&L->a);
	f.block = block;
	f.imm19 = false;
	tg_array_add(&L->fixups, f);
}

// The branch to `block` when the condition is `when`.
TG_INTERNAL void a64_jump_if(a64Lower *L, a64Cond c, i32 test, bool when, i32 block) {
	tgAsm *a = &L->a;
	a64Fixup fx;
	fx.at = test >= 0 ? a64_cb(a, when, (u8)test) : a64_bcond(a, when ? c : a64_invert(c));
	fx.block = block;
	fx.imm19 = true;
	tg_array_add(&L->fixups, fx);
}

// Skips the next instruction when the condition is `when`.
TG_INTERNAL void a64_skip_if(a64Lower *L, a64Cond c, i32 test, bool when) {
	tgAsm *a = &L->a;
	if (test >= 0) a64_cb_skip(a, when, (u8)test);
	else           a64_bcond_skip(a, when ? c : a64_invert(c));
}

// Goes to `t` when the condition holds, else to `f`. The condition is `c` on the flags,
// or with `test` >= 0, whether that register is not zero.
TG_INTERNAL void a64_branch(a64Lower *L, a64Cond c, i32 test, i32 t, i32 f) {
	if (L->far_branches) {
		// cbz/cbnz and b.cond only skip one instruction, so no procedure is too long for them
		if (t == L->next_block) {
			a64_skip_if(L, c, test, true);
			a64_jump(L, f);
		} else {
			a64_skip_if(L, c, test, false);
			a64_jump(L, t);
			if (f != L->next_block) a64_jump(L, f);
		}
		return;
	}
	if (f == L->next_block) {
		a64_jump_if(L, c, test, true, t);
	} else if (t == L->next_block) {
		a64_jump_if(L, c, test, false, f);
	} else {
		a64_jump_if(L, c, test, true, t);
		a64_jump(L, f);
	}
}

// Sets the flags or picks a register so that a64_branch can test the bool v.
TG_INTERNAL i32 a64_test_bool(a64Lower *L, u32 v) {
	i8 r = L->reg.data[v];
	if (r != TG_NOREG && r < TG_FREG) {
		if (L->clean.data[v]) return r;
		a64_tst_byte(&L->a, (u8)r);
		return -1;
	}
	return a64_src(L, v, A64_T0, 1, tgExt_Zero);
}

////////////////////////////////////////////////////////////////
// Instructions
////////////////////////////////////////////////////////////////

// Ends a compare: the flags hold `c`, kept for the next instruction or put in dst.
TG_INTERNAL void a64_compare_result(a64Lower *L, tgInstr const *in, a64Cond c) {
	if (L->fuse) {
		L->flags_vreg = in->dst;
		L->flags_cond = c;
		return;
	}
	u8 d = a64_dst(L, in->dst, A64_T0);
	a64_cset(&L->a, d, c);
	a64_put(L, in->dst, d);
}

// A logical immediate for k as a `size` byte operand, whose higher bytes mean nothing: k
// itself, zero extended, or repeated across the register.
TG_INTERNAL i32 a64_logical_imm_any(u64 k, i32 size) {
	i32 enc = a64_logical_imm(k);
	if (enc >= 0 || size >= 8) return enc;
	u64 low = k & ((1ull << (8*size)) - 1);
	enc = a64_logical_imm(low);
	if (enc >= 0) return enc;
	u64 rep = low;
	for (i32 w = 8*size; w < 64; w *= 2) rep |= rep << w;
	return a64_logical_imm(rep);
}

// One piece of an inline copy.
typedef struct a64CopyChunk { i32 size; i64 off; u8 reg; bool fp; } a64CopyChunk;

// Whether the load or store `in` reads its address from the add folded into it.
TG_INTERNAL bool a64_folded_addr(a64Lower *L, tgInstr const *in) {
	if (L->addr_vreg == 0) return false;
	TG_ASSERT(in->mem.kind == tgMem_Reg && in->mem.base == L->addr_vreg && in->mem.offset == 0);
	L->addr_vreg = 0;
	return true;
}

// A register's value that a folded operation may read as its source: not a constant or frame address
// computed into a scratch register on the way.
TG_INTERNAL bool a64_in_reg_or_slot(a64Lower *L, u32 v) {
	return v != 0 && !L->is_const.data[v] && !L->remat.data[v] && L->reg.data[v] < TG_FREG;
}

// Whether the i64 add `in` only forms the address of the load or store `n` after it: [x + y].
TG_INTERNAL bool a64_can_fold_addr(a64Lower *L, tgInstr const *in, tgInstr const *n) {
	u32 v = in->dst;
	if (in->op != tgOp_Add || in->type != tgType_I64 || v == 0 || L->uses.data[v] != 1 || !L->in_block.data[v]) return false;
	if (!a64_in_reg_or_slot(L, in->a) || !a64_in_reg_or_slot(L, in->b) || in->a == L->shift_vreg || in->b == L->shift_vreg) return false;
	if (n->op != tgOp_Load && n->op != tgOp_Store) return false;
	if (n->mem.kind != tgMem_Reg || n->mem.base != v || n->mem.offset != 0) return false;
	if (n->type == tgType_V128 || (n->op == tgOp_Store && n->a == v)) return false;
	return true;
}

// Whether the shl `in` by a constant only feeds the add or sub `n` after it, as its shifted operand.
TG_INTERNAL bool a64_can_fold_shift(a64Lower *L, tgInstr const *in, tgInstr const *n, u32 *amount) {
	u32 v = in->dst;
	i64 k = 0;
	if (in->op != tgOp_Shl || v == 0 || L->uses.data[v] != 1 || !L->in_block.data[v]) return false;
	if (!a64_is_const(L, in->b, &k) || k < 0 || k >= (tg_type_size(in->type) == 8 ? 64 : 32)) return false;
	if (!a64_in_reg_or_slot(L, in->a)) return false;
	if ((n->op != tgOp_Add && n->op != tgOp_Sub) || n->type != in->type) return false;
	u32 other = n->b == v ? n->a : n->op == tgOp_Add && n->a == v ? n->b : 0;
	if (other == 0 || other == v || L->is_const.data[other]) return false;
	*amount = (u32)k;
	return true;
}

// Whether a load or store of `size` bytes at [xn + off] encodes without x17.
TG_INTERNAL bool a64_ldst_direct(i64 off, i32 size) {
	return (off >= 0 && off % size == 0 && off / size < 4096) || (off >= -256 && off < 256);
}

// Where an inline copy's bytes at the pointer v start. A frame address folds into the copy's
// loads and stores, as it does into a load's, when each of them encodes without x17, which may
// carry a piece of the copy; otherwise `scratch` holds the address.
TG_INTERNAL a64Addr a64_copy_base(a64Lower *L, u32 v, u8 scratch, a64CopyChunk const *chunks, i32 count) {
	a64Addr r;
	if (L->remat.data[v] && (L->rmem.data[v].kind == tgMem_Local || L->rmem.data[v].kind == tgMem_Incoming)) {
		r = a64_mem(L, L->rmem.data[v], scratch);
		bool direct = r.base == A64_FP || r.base == A64_SP;
		for (i32 i = 0; i < count && direct; i++) direct = a64_ldst_direct(r.off + chunks[i].off, chunks[i].size);
		if (direct) return r;
	}
	r.base = a64_src(L, v, scratch, 8, tgExt_None);
	r.off = 0;
	return r;
}

TG_INTERNAL void a64_lower_instr(a64Lower *L, tgInstr const *in) {
	tgAsm *a = &L->a;
	tgProc *p = L->p;
	i32 size = tg_type_size(in->type);
	i64 k = 0;
	switch (in->op) {
	case tgOp_Nop:
		if (in->imm == 1) a64_emit(a, 0xD503201F);
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
		u8 d = a64_dst(L, in->dst, A64_T0);
		a64_mov_imm(a, d, (u64)in->imm);
		a64_put(L, in->dst, d);
		break;
	}
	case tgOp_Lea: {
		if (L->remat.data[in->dst]) break;
		u8 d = a64_dst(L, in->dst, A64_T0);
		// a symbol's address goes right into d
		a64Addr m = a64_mem(L, in->mem, d);
		if (m.base != d || m.off != 0) a64_add_imm(a, d, m.base, m.off);
		a64_put(L, in->dst, d);
		break;
	}
	case tgOp_Load: {
		if (in->mem.kind == tgMem_Local && L->local_reg.data[in->mem.base] >= 0) {
			i8 lr = L->local_reg.data[in->mem.base];
			if (lr >= TG_FREG) a64_putf(L, in->dst, (u8)(lr - TG_FREG), size);
			else               a64_put(L, in->dst, (u8)lr);
			break;
		}
		tgLowerStored const *st = &L->stored;
		if (st->valid && st->mem.kind == in->mem.kind && st->mem.base == in->mem.base && st->mem.offset == in->mem.offset &&
		    st->size == size && st->fp == tg_type_is_float(in->type) && !(in->flags & tgInstrFlag_Volatile)) {
			if (st->fp) {
				a64_putf(L, in->dst, st->reg, size);
			} else {
				u8 d = a64_dst(L, in->dst, A64_T0);
				a64_extend(a, false, size, d, st->reg);
				a64_put(L, in->dst, d);
			}
			break;
		}
		if (a64_folded_addr(L, in)) {
			u8 rn = a64_src(L, L->addr_x, A64_TA, 8, tgExt_None);
			u8 rm = a64_src(L, L->addr_y, A64_T1, 8, tgExt_None);
			bool fp = tg_type_is_float(in->type);
			u8 d = fp ? a64_dstf(L, in->dst, A64_F0) : a64_dst(L, in->dst, A64_T0);
			a64_ldst_reg(a, fp, A64_LDR, size, d, rn, rm);
			if (fp) a64_putf(L, in->dst, d, size);
			else    a64_put(L, in->dst, d);
			break;
		}
		a64Addr m = a64_mem(L, in->mem, A64_TA);
		if (tg_type_is_float(in->type)) {
			u8 d = a64_dstf(L, in->dst, A64_F0);
			a64_ldr_fp(a, size, d, m.base, m.off);
			a64_putf(L, in->dst, d, size);
		} else {
			u8 d = a64_dst(L, in->dst, A64_T0);
			a64_ldr(a, size, false, d, m.base, m.off);
			a64_put(L, in->dst, d);
		}
		break;
	}
	case tgOp_Store: {
		if (in->mem.kind == tgMem_Local && L->local_reg.data[in->mem.base] >= 0) {
			i8 lr = L->local_reg.data[in->mem.base];
			if (lr >= TG_FREG) {
				a64_getf(L, (u8)(lr - TG_FREG), in->a, size);
				break;
			}
			// zero extended, as a load from memory would give it back
			u8 r = a64_src(L, in->a, (u8)lr, size, tgExt_Zero);
			if (r != (u8)lr) a64_mov(a, (u8)lr, r);
			break;
		}
		// the value first, the address may need x16 and x17
		tgLowerStored st;
		memset(&st, 0, sizeof(st));
		st.valid = true;
		st.fp = tg_type_is_float(in->type);
		st.size = size;
		st.mem = in->mem;
		st.fresh = true;
		if (st.fp) {
			st.reg = a64_srcf(L, in->a, A64_F0, size);
		} else {
			st.reg = XZR;
			if (!a64_is_const(L, in->a, &k) || a64_ext_value(k, size, tgExt_Zero) != 0) {
				st.reg = a64_src(L, in->a, A64_T0, size, tgExt_None);
			}
		}
		if (a64_folded_addr(L, in)) {
			u8 rn = a64_src(L, L->addr_x, A64_TA, 8, tgExt_None);
			u8 rm = a64_src(L, L->addr_y, A64_T1, 8, tgExt_None);
			a64_ldst_reg(a, st.fp, A64_STR, size, st.reg, rn, rm);
		} else {
			a64Addr m = a64_mem(L, in->mem, A64_TA);
			if (st.fp) a64_str_fp(a, size, st.reg, m.base, m.off);
			else       a64_str(a, size, st.reg, m.base, m.off);
		}
		if (!(in->flags & tgInstrFlag_Volatile)) L->stored = st;
		break;
	}
	case tgOp_Copy:
	case tgOp_Trunc:
	case tgOp_Bitcast: {
		i8 rs = L->reg.data[in->a];
		i8 rd = L->reg.data[in->dst];
		if (!L->is_const.data[in->a] && rs >= TG_FREG && rd >= TG_FREG) {
			if (rs != rd) a64_fmov_reg(a, (u8)(rd - TG_FREG), (u8)(rs - TG_FREG));
			break;
		}
		u8 d = a64_dst(L, in->dst, A64_T0);
		a64_put(L, in->dst, a64_src(L, in->a, d, 8, tgExt_None));
		break;
	}

	case tgOp_Add:
	case tgOp_Sub:
	case tgOp_And:
	case tgOp_Or:
	case tgOp_Xor: {
		a64AluOp op = A64_ADD;
		switch (in->op) {
		case tgOp_Add: op = A64_ADD; break;
		case tgOp_Sub: op = A64_SUB; break;
		case tgOp_And: op = A64_AND; break;
		case tgOp_Or:  op = A64_ORR; break;
		case tgOp_Xor: op = A64_EOR; break;
		}
		u32 x = in->a, y = in->b;
		if (L->fold_addr) {
			// the next load or store adds the terms itself
			L->addr_vreg = in->dst;
			L->addr_x = x;
			L->addr_y = y;
			break;
		}
		if (in->op != tgOp_Sub && L->is_const.data[x] && !L->is_const.data[y]) {
			u32 t = x; x = y; y = t;
		}
		if (in->op != tgOp_Sub && L->shift_vreg != 0 && x == L->shift_vreg) {
			u32 t = x; x = y; y = t;
		}
		u8 d = a64_dst(L, in->dst, A64_T0);
		u8 ra = a64_src(L, x, A64_T0, size, tgExt_None);
		i32 enc = -1;
		if (L->shift_vreg != 0 && y == L->shift_vreg) {
			a64_alu(a, op, d, ra, a64_src(L, L->shift_src, A64_T1, size, tgExt_None), A64_LSL, L->shift_amt);
			L->shift_vreg = 0;
		} else if ((in->op == tgOp_Add || in->op == tgOp_Sub) && a64_is_const(L, y, &k) && a64_addsub_imm_ok(k)) {
			a64_addsub_imm(a, in->op == tgOp_Sub, false, d, ra, k);
		} else if (in->op != tgOp_Add && in->op != tgOp_Sub && a64_is_const(L, y, &k) && (enc = a64_logical_imm_any((u64)k, size)) >= 0) {
			a64_logical_imm_op(a, op, d, ra, enc);
		} else {
			a64_alu(a, op, d, ra, a64_src(L, y, A64_T1, size, tgExt_None), A64_LSL, 0);
		}
		a64_put(L, in->dst, d);
		break;
	}
	case tgOp_Mul: {
		u8 ra = a64_src(L, in->a, A64_T0, size, tgExt_None);
		u8 rb = a64_src(L, in->b, A64_T1, size, tgExt_None);
		u8 d = a64_dst(L, in->dst, A64_T0);
		a64_mul(a, d, ra, rb);
		a64_put(L, in->dst, d);
		break;
	}
	case tgOp_SDiv:
	case tgOp_SRem:
	case tgOp_UDiv:
	case tgOp_URem: {
		bool sgn = in->op == tgOp_SDiv || in->op == tgOp_SRem;
		tgExtKind ext = sgn ? tgExt_Sign : tgExt_Zero;
		u8 ra = a64_src(L, in->a, A64_T0, size, ext);
		u8 rb = a64_src(L, in->b, A64_T1, size, ext);
		u8 d = a64_dst(L, in->dst, A64_T2);
		if (in->op == tgOp_SRem || in->op == tgOp_URem) {
			a64_div(a, sgn, A64_T2, ra, rb);
			a64_msub(a, d, A64_T2, rb, ra);
		} else {
			a64_div(a, sgn, d, ra, rb);
		}
		a64_put(L, in->dst, d);
		break;
	}
	case tgOp_Shl:
	case tgOp_LShr:
	case tgOp_AShr: {
		if (L->shift_vreg == in->dst) break; // the next add or sub shifts its operand
		a64Shift kind = in->op == tgOp_Shl ? A64_LSL : in->op == tgOp_LShr ? A64_LSR : A64_ASR;
		// the low bytes of a left shift depend only on the low bytes of its operand
		tgExtKind ext = in->op == tgOp_AShr ? tgExt_Sign : in->op == tgOp_LShr ? tgExt_Zero : tgExt_None;
		u8 ra = a64_src(L, in->a, A64_T0, size, ext);
		u8 d = a64_dst(L, in->dst, A64_T0);
		// like x86: narrow values shift as 32 bits, and the count is masked
		if (a64_is_const(L, in->b, &k)) {
			a64_shift_imm(a, kind, size == 8 ? 8 : 4, d, ra, (u32)k);
		} else {
			a64_shiftv(a, kind, size == 8 ? 8 : 4, d, ra, a64_src(L, in->b, A64_T1, 8, tgExt_None));
		}
		a64_put(L, in->dst, d);
		break;
	}
	case tgOp_FAdd:
	case tgOp_FSub:
	case tgOp_FMul:
	case tgOp_FDiv: {
		a64FOp op = A64_FADD;
		switch (in->op) {
		case tgOp_FAdd: op = A64_FADD; break;
		case tgOp_FSub: op = A64_FSUB; break;
		case tgOp_FMul: op = A64_FMUL; break;
		case tgOp_FDiv: op = A64_FDIV; break;
		}
		u8 ra = a64_srcf(L, in->a, A64_F0, size);
		u8 rb = a64_srcf(L, in->b, A64_F1, size);
		u8 d = a64_dstf(L, in->dst, A64_F0);
		a64_fop(a, op, size, d, ra, rb);
		a64_putf(L, in->dst, d, size);
		break;
	}
	case tgOp_Fma: {
		u8 ra = a64_srcf(L, in->a, A64_F0, size);
		u8 rb = a64_srcf(L, in->b, A64_F1, size);
		u8 rc = a64_srcf(L, in->c, A64_F2, size);
		u8 d = a64_dstf(L, in->dst, A64_F0);
		a64_fmadd(a, size, d, ra, rb, rc);
		a64_putf(L, in->dst, d, size);
		break;
	}
	case tgOp_Sqrt:
	case tgOp_FNeg: {
		u8 r = a64_srcf(L, in->a, A64_F0, size);
		u8 d = a64_dstf(L, in->dst, A64_F0);
		if (in->op == tgOp_Sqrt) a64_fsqrt(a, size, d, r);
		else                     a64_fneg(a, size, d, r);
		a64_putf(L, in->dst, d, size);
		break;
	}
	case tgOp_Neg:
	case tgOp_Not: {
		u8 r = a64_src(L, in->a, A64_T0, size, tgExt_None);
		u8 d = a64_dst(L, in->dst, A64_T0);
		a64_alu(a, in->op == tgOp_Neg ? A64_SUB : A64_ORN, d, XZR, r, A64_LSL, 0);
		a64_put(L, in->dst, d);
		break;
	}
	case tgOp_ICmp: {
		tgCond c = (tgCond)in->aux;
		if (in->a == L->flags_vreg) {
			// a test against zero of the compare that set the flags
			TG_ASSERT(c == tgCond_EQ || c == tgCond_NE);
			a64Cond cc = c == tgCond_NE ? L->flags_cond : a64_invert(L->flags_cond);
			L->flags_vreg = 0;
			a64_compare_result(L, in, cc);
			break;
		}
		tgExtKind ext = a64_cond_is_signed(c) ? tgExt_Sign : tgExt_Zero;
		u8 ra = a64_src(L, in->a, A64_T0, size, ext);
		if (a64_is_const(L, in->b, &k) && a64_addsub_imm_ok(a64_ext_value(k, size, ext))) {
			a64_addsub_imm(a, true, true, XZR, ra, a64_ext_value(k, size, ext));
		} else {
			a64_cmp(a, ra, a64_src(L, in->b, A64_T1, size, ext));
		}
		a64_compare_result(L, in, a64_cond_for(c));
		break;
	}
	case tgOp_FCmp: {
		u8 ra = a64_srcf(L, in->a, A64_F0, size);
		u8 rb = a64_srcf(L, in->b, A64_F1, size);
		a64_fcmp(a, size, ra, rb);
		a64_compare_result(L, in, a64_cond_for((tgCond)in->aux));
		break;
	}
	case tgOp_Zext:
	case tgOp_Sext: {
		tgType src = (tgType)in->aux;
		u8 d = a64_dst(L, in->dst, A64_T0);
		a64_put(L, in->dst, a64_src(L, in->a, d, tg_type_size(src), in->op == tgOp_Sext ? tgExt_Sign : tgExt_Zero));
		break;
	}
	case tgOp_SIToF:
	case tgOp_UIToF: {
		bool sgn = in->op == tgOp_SIToF;
		tgType src = (tgType)in->aux;
		u8 r = a64_src(L, in->a, A64_T0, tg_type_size(src), sgn ? tgExt_Sign : tgExt_Zero);
		u8 d = a64_dstf(L, in->dst, A64_F0);
		a64_cvtf(a, sgn, size, d, r);
		a64_putf(L, in->dst, d, size);
		break;
	}
	case tgOp_FToSI:
	case tgOp_FToUI: {
		i32 fs = tg_type_size((tgType)in->aux);
		u8 r = a64_srcf(L, in->a, A64_F0, fs);
		u8 d = a64_dst(L, in->dst, A64_T0);
		a64_fcvtz(a, in->op == tgOp_FToSI, fs, d, r);
		a64_put(L, in->dst, d);
		break;
	}
	case tgOp_FExt:
	case tgOp_FTrunc:
	case tgOp_FToHalf:
	case tgOp_HalfToF: {
		i32 from = 0, to = 0;
		switch (in->op) {
		case tgOp_FExt:    from = 4; to = 8; break;
		case tgOp_FTrunc:  from = 8; to = 4; break;
		case tgOp_FToHalf: from = tg_type_size((tgType)in->aux); to = 2; break;
		case tgOp_HalfToF: from = 2; to = 4; break;
		}
		u8 r = a64_srcf(L, in->a, A64_F0, from);
		u8 d = a64_dstf(L, in->dst, A64_F0);
		a64_fcvt(a, to, from, d, r);
		a64_putf(L, in->dst, d, to);
		break;
	}
	case tgOp_Select: {
		a64Cond cc = A64_NE;
		if (in->a == L->flags_vreg) {
			cc = L->flags_cond;
			L->flags_vreg = 0;
		} else {
			i32 test = a64_test_bool(L, in->a);
			if (test >= 0) a64_cmp_imm32(a, (u8)test, 0);
		}
		// nothing below changes the flags
		if (tg_type_is_float(in->type)) {
			u8 rb = a64_srcf(L, in->b, A64_F0, size);
			u8 rc = a64_srcf(L, in->c, A64_F1, size);
			u8 d = a64_dstf(L, in->dst, A64_F0);
			a64_fcsel(a, d, rb, rc, cc);
			a64_putf(L, in->dst, d, size);
		} else {
			u8 rb = a64_src(L, in->b, A64_T1, 8, tgExt_None);
			u8 rc = a64_src(L, in->c, A64_T2, 8, tgExt_None);
			u8 d = a64_dst(L, in->dst, A64_T1);
			a64_csel(a, d, rb, rc, cc);
			a64_put(L, in->dst, d);
		}
		break;
	}

	case tgOp_MemCopy:
	case tgOp_MemMove: {
		i64 n = in->imm;
		if (n <= A64_INLINE_COPY) {
			// load everything, then store everything: overlap safe
			// Up to 32 bytes in x registers, 8 at a time: a value just stored in 8 byte halves, as a
			// slice header or a parameter is, then loads at full speed, where a q register load (or
			// an ldp) of both halves waits for the stores to retire.
			a64CopyChunk chunks[8];
			u8 const gprs[7] = {A64_T0, A64_T1, A64_T2, A64_T5, A64_TB, A64_TA, X17};
			i32 count = 0, nq = 0, ng = 0;
			memset(chunks, 0, sizeof(chunks));
			for (i64 off = 0; off < n;) {
				i64 rem = n - off;
				i32 c = rem >= 16 && n > 32 ? 16 : rem >= 8 ? 8 : rem >= 4 ? 4 : rem >= 2 ? 2 : 1;
				a64CopyChunk ch;
				ch.size = c;
				ch.off = off;
				ch.fp = c == 16;
				ch.reg = ch.fp ? (u8)(A64_F0 + nq++) : gprs[ng++];
				chunks[count++] = ch;
				off += c;
			}
			a64Addr dst = a64_copy_base(L, in->a, A64_T3, chunks, count);
			a64Addr src = a64_copy_base(L, in->b, A64_T4, chunks, count);
			for (i32 i = 0; i < count; i++) {
				if (chunks[i].fp) a64_ldr_fp(a, 16, chunks[i].reg, src.base, src.off + chunks[i].off);
				else              a64_ldr(a, chunks[i].size, false, chunks[i].reg, src.base, src.off + chunks[i].off);
			}
			for (i32 i = 0; i < count; i++) {
				if (chunks[i].fp) a64_str_fp(a, 16, chunks[i].reg, dst.base, dst.off + chunks[i].off);
				else              a64_str(a, chunks[i].size, chunks[i].reg, dst.base, dst.off + chunks[i].off);
			}
		} else {
			u8 dst = a64_src(L, in->a, A64_T3, 8, tgExt_None);
			u8 src = a64_src(L, in->b, A64_T4, 8, tgExt_None);
			a64_mov(a, X0, dst);
			a64_mov(a, X1, src);
			a64_mov_imm(a, X2, (u64)n);
			a64_call_libc(L, "memmove");
		}
		break;
	}
	case tgOp_MemZero: {
		i64 n = in->imm;
		a64Addr m = a64_mem(L, in->mem, A64_TA);
		if (n <= A64_INLINE_ZERO) {
			for (i64 off = 0; off < n;) {
				i64 rem = n - off;
				i32 c = rem >= 8 ? 8 : rem >= 4 ? 4 : rem >= 2 ? 2 : 1;
				a64_str(a, c, XZR, m.base, m.off + off);
				off += c;
			}
		} else {
			a64_add_imm(a, X0, m.base, m.off);
			a64_mov_imm(a, X1, 0);
			a64_mov_imm(a, X2, (u64)n);
			a64_call_libc(L, "memset");
		}
		break;
	}
	case tgOp_MemCopyDyn:
	case tgOp_MemMoveDyn:
		a64_get(L, X0, in->a, 8, tgExt_None);
		a64_get(L, X1, in->b, 8, tgExt_None);
		a64_get(L, X2, in->c, 8, tgExt_None);
		a64_call_libc(L, "memmove");
		break;
	case tgOp_MemSetDyn:
		a64_get(L, X0, in->a, 8, tgExt_None);
		a64_get(L, X1, in->b, 1, tgExt_Zero);
		a64_get(L, X2, in->c, 8, tgExt_None);
		a64_call_libc(L, "memset");
		break;

	case tgOp_Call:
		a64_lower_call(L, &p->calls.data[(isize)in->imm], false);
		break;
	case tgOp_Jump:
		a64_jump(L, (i32)in->imm);
		break;
	case tgOp_Branch: {
		i32 t = (i32)in->imm;
		i32 f = (i32)in->c;
		if (in->a == L->flags_vreg) {
			L->flags_vreg = 0;
			a64_branch(L, L->flags_cond, -1, t, f);
		} else if (a64_is_const(L, in->a, &k)) {
			a64_jump(L, (k & 0xff) ? t : f);
		} else {
			a64_branch(L, A64_NE, a64_test_bool(L, in->a), t, f);
		}
		break;
	}
	case tgOp_Ret:
		a64_lower_call(L, &p->calls.data[(isize)in->imm], true);
		a64_epilogue(L);
		break;
	case tgOp_Unreachable:
	case tgOp_Trap:
		a64_brk(a, 1);
		break;
	case tgOp_DebugTrap:
		a64_brk(a, 0xf000);
		break;
	case tgOp_CpuRelax:
		a64_emit(a, 0xD503203F); // yield
		break;
	case tgOp_Prefetch:
		break;
	case tgOp_ReadCycleCounter: {
		u8 d = a64_dst(L, in->dst, A64_T0);
		if (in->imm) {
			a64_emit(a, 0xD53BE000 | d); // mrs xd, cntfrq_el0
		} else {
			a64_emit(a, 0xD53BE040 | d); // mrs xd, cntvct_el0
		}
		a64_put(L, in->dst, d);
		break;
	}
	case tgOp_Vec128: {
		a64_ldr_fp(a, 16, 16, a64_src(L, in->a, A64_TA, 8, tgExt_None), 0);
		a64_ldr_fp(a, 16, 17, a64_src(L, in->b, A64_TA, 8, tgExt_None), 0);
		if (in->c) a64_ldr_fp(a, 16, 18, a64_src(L, in->c, A64_TA, 8, tgExt_None), 0);
		a64_vec_intrinsic(a, in->aux);
		a64Addr m = a64_mem(L, in->mem, A64_TA);
		a64_str_fp(a, 16, 16, m.base, m.off);
		break;
	}
	case tgOp_StackPointer: {
		u8 d = a64_dst(L, in->dst, A64_T0);
		a64_mov_sp(a, d, A64_SP);
		a64_put(L, in->dst, d);
		break;
	}
	case tgOp_StackRestore: {
		// the value is an earlier sp: the allocas since then are freed
		u8 r = a64_src(L, in->a, A64_T0, 8, tgExt_None);
		a64_mov_sp(a, A64_SP, r);
		break;
	}
	case tgOp_FrameAddress:
		a64_put(L, in->dst, A64_FP);
		break;
	case tgOp_ReturnAddress: {
		u8 d = a64_dst(L, in->dst, A64_T0);
		if (in->imm) {
			a64_add_imm(a, d, A64_FP, 8);
		} else {
			a64_ldr(a, 8, false, d, A64_FP, 8);
		}
		a64_put(L, in->dst, d);
		break;
	}
	case tgOp_TlsAddr: {
		i32 sym = (i32)in->imm;
		u32 const mrs_tpidr_el0 = 0xD53BD040;
		if (tg_is_darwin()) {
			// the variable's descriptor holds a function that returns its address in x0
			a64_adrp(a, X0, sym, tgReloc_A64_TlvPage21);
			a64_ldr_pageoff(a, X0, X0, sym, tgReloc_A64_TlvPageOff12);
			a64_ldr(a, 8, false, A64_TA, X0, 0);
			a64_blr(a, A64_TA);
			a64_put(L, in->dst, X0);
		} else if (a64_tls_calls()) {
			// the exact TLS descriptor sequence, which the linker may rewrite into a cheaper
			// model; the resolver returns the offset from the thread pointer in x0
			a64_adrp(a, X0, sym, tgReloc_A64_TlsDescPage21);
			a64_ldr_pageoff(a, X1, X0, sym, tgReloc_A64_TlsDescLd);
			tg_asm_reloc(a, tgReloc_A64_TlsDescAdd, tg_pos(a), sym, 0);
			a64_emit(a, 0x91000000 | ((u32)X0 << 5) | X0); // add x0, x0, #0
			tg_asm_reloc(a, tgReloc_A64_TlsDescCall, tg_pos(a), sym, 0);
			a64_blr(a, X1);
			a64_emit(a, mrs_tpidr_el0 | X1);
			a64_alu(a, A64_ADD, X0, X1, X0, A64_LSL, 0);
			a64_put(L, in->dst, X0);
		} else {
			u8 d = a64_dst(L, in->dst, A64_T0);
			a64_adrp(a, d, sym, tgReloc_A64_TlsIePage21);
			a64_ldr_pageoff(a, d, d, sym, tgReloc_A64_TlsIeLo12);
			a64_emit(a, mrs_tpidr_el0 | A64_TA);
			a64_alu(a, A64_ADD, d, A64_TA, d, A64_LSL, 0);
			a64_put(L, in->dst, d);
		}
		break;
	}
	case tgOp_Alloca: {
		// the block goes above the outgoing argument area, which moves down with sp
		i32 args_area = (i32)tg_round_up(L->max_call_stack, 16);
		u32 shift = 0;
		while (((i64)1 << shift) < in->imm) shift++;
		u8 n = a64_src(L, in->a, A64_T1, 8, tgExt_None);
		a64_add_imm(a, A64_T0, A64_SP, args_area);
		a64_alu(a, A64_SUB, A64_T0, A64_T0, n, A64_LSL, 0);
		a64_lsr_imm(a, A64_T0, A64_T0, shift);
		a64_lsl_imm(a, A64_T0, A64_T0, shift);
		a64_add_imm(a, A64_SP, A64_T0, -args_area);
		a64_put(L, in->dst, A64_T0);
		break;
	}
	case tgOp_AtomicFence:
		a64_dmb_ish(a);
		break;
	case tgOp_AtomicLoad: {
		a64Addr m = a64_mem(L, in->mem, A64_TA);
		a64_add_imm(a, A64_TA, m.base, m.off);
		u8 d = a64_dst(L, in->dst, A64_T0);
		a64_ldar(a, size, d, A64_TA);
		a64_put(L, in->dst, d);
		break;
	}
	case tgOp_AtomicStore: {
		u8 r = a64_src(L, in->a, A64_T0, size, tgExt_None);
		a64Addr m = a64_mem(L, in->mem, A64_TA);
		a64_add_imm(a, A64_TA, m.base, m.off);
		a64_stlr(a, size, r, A64_TA);
		break;
	}
	case tgOp_AtomicRmw: {
		tgRmwOp op = (tgRmwOp)in->aux;
		u8 rv = a64_src(L, in->a, A64_T1, size, tgExt_Zero);
		a64Addr m = a64_mem(L, in->mem, A64_TA);
		a64_add_imm(a, A64_TA, m.base, m.off);
		switch (op) {
		case tgRmw_Xchg: a64_lse(a, A64_SWP, size, rv, A64_T0, A64_TA); break;
		case tgRmw_Add:  a64_lse(a, A64_LDADD, size, rv, A64_T0, A64_TA); break;
		case tgRmw_Or:   a64_lse(a, A64_LDSET, size, rv, A64_T0, A64_TA); break;
		case tgRmw_Xor:  a64_lse(a, A64_LDEOR, size, rv, A64_T0, A64_TA); break;
		case tgRmw_Sub:
			a64_alu(a, A64_SUB, A64_T1, XZR, rv, A64_LSL, 0);
			a64_lse(a, A64_LDADD, size, A64_T1, A64_T0, A64_TA);
			break;
		case tgRmw_And:
			a64_alu(a, A64_ORN, A64_T1, XZR, rv, A64_LSL, 0);
			a64_lse(a, A64_LDCLR, size, A64_T1, A64_T0, A64_TA);
			break;
		case tgRmw_Nand: {
			// no instruction for it: a compare and swap loop
			a64_ldr(a, size, false, A64_T0, A64_TA, 0);
			i64 loop = tg_pos(a);
			a64_mov(a, A64_T2, A64_T0);
			a64_alu(a, A64_AND, A64_T3, A64_T0, rv, A64_LSL, 0);
			a64_alu(a, A64_ORN, A64_T3, XZR, A64_T3, A64_LSL, 0);
			a64_casal(a, size, A64_T2, A64_T3, A64_TA);
			a64_cmp(a, A64_T2, A64_T0);
			a64_mov(a, A64_T0, A64_T2);
			a64_bcond_to(a, A64_NE, loop);
			break;
		}
		}
		a64_put(L, in->dst, A64_T0);
		break;
	}
	case tgOp_AtomicCas: {
		u8 expected = a64_src(L, in->a, A64_T0, size, tgExt_Zero);
		u8 desired = a64_src(L, in->b, A64_T1, size, tgExt_Zero);
		a64Addr m = a64_mem(L, in->mem, A64_TA);
		a64_add_imm(a, A64_TA, m.base, m.off);
		a64_mov(a, A64_T2, expected);
		a64_casal(a, size, A64_T2, desired, A64_TA);
		a64_cmp(a, A64_T2, expected);
		a64_cset(a, A64_T3, A64_EQ);
		a64_put(L, in->dst, A64_T2);
		a64_put(L, in->c, A64_T3);
		break;
	}
	case tgOp_Syscall: {
		// the number in x16 on Darwin, in x8 on Linux, the arguments in x0-x5, the result in x0
		tgCall const *c = &p->calls.data[(isize)in->imm];
		a64_lower_call(L, c, true);
		a64_emit(a, tg_is_darwin() ? 0xD4001001 : 0xD4000001); // svc #0x80 or svc #0
		a64_put(L, c->result_vreg, X0);
		break;
	}
	case tgOp_MulHiU: {
		u8 ra = a64_src(L, in->a, A64_T0, 8, tgExt_None);
		u8 rb = a64_src(L, in->b, A64_T1, 8, tgExt_None);
		u8 d = a64_dst(L, in->dst, A64_T0);
		a64_umulh(a, d, ra, rb);
		a64_put(L, in->dst, d);
		break;
	}
	case tgOp_MulOvf: {
		bool sgn = in->aux != 0;
		TG_ASSERT(size == 4 || size == 8);
		tgExtKind ext = sgn ? tgExt_Sign : tgExt_Zero;
		u8 ra = a64_src(L, in->a, A64_T0, size, ext);
		u8 rb = a64_src(L, in->b, A64_T1, size, ext);
		a64_mul(a, A64_T2, ra, rb);
		if (size == 8) {
			if (sgn) {
				a64_smulh(a, A64_T3, ra, rb);
				a64_alu(a, A64_SUBS, XZR, A64_T3, A64_T2, A64_ASR, 63);
			} else {
				a64_umulh(a, A64_T3, ra, rb);
				a64_cmp(a, A64_T3, XZR);
			}
		} else {
			// the 64 bit product of the extended values is exact
			if (sgn) {
				a64_extend(a, true, 4, A64_T3, A64_T2);
				a64_cmp(a, A64_T2, A64_T3);
			} else {
				a64_lsr_imm(a, A64_T3, A64_T2, 32);
				a64_cmp(a, A64_T3, XZR);
			}
		}
		a64_cset(a, A64_T3, A64_NE);
		a64_put(L, in->dst, A64_T2);
		a64_put(L, in->c, A64_T3);
		break;
	}
	case tgOp_Bswap: {
		u8 r = a64_src(L, in->a, A64_T0, size, tgExt_Zero);
		u8 d = a64_dst(L, in->dst, A64_T0);
		if (size > 1) a64_rev(a, size, d, r);
		else if (d != r) a64_mov(a, d, r);
		a64_put(L, in->dst, d);
		break;
	}
	case tgOp_Popcount: {
		u8 r = a64_src(L, in->a, A64_T0, size, tgExt_Zero);
		a64_fmov_to_fp(a, A64_F0, r);
		a64_cnt8b(a, A64_F0, A64_F0);
		a64_addv8b(a, A64_F0, A64_F0);
		u8 d = a64_dst(L, in->dst, A64_T0);
		a64_fmov_from_fp(a, d, A64_F0);
		a64_put(L, in->dst, d);
		break;
	}
	case tgOp_Ctz: {
		u8 r = a64_src(L, in->a, A64_T0, size, tgExt_Zero);
		if (size < 8) {
			// a bit just above the value makes zero count as the width
			a64_mov_imm(a, A64_T1, 1ull << (8*size));
			a64_alu(a, A64_ORR, A64_T0, r, A64_T1, A64_LSL, 0);
			r = A64_T0;
		}
		a64_rbit(a, A64_T0, r);
		u8 d = a64_dst(L, in->dst, A64_T0);
		a64_clz(a, d, A64_T0);
		a64_put(L, in->dst, d);
		break;
	}
	case tgOp_Clz: {
		u8 r = a64_src(L, in->a, A64_T0, size, tgExt_Zero);
		u8 d = a64_dst(L, in->dst, A64_T0);
		a64_clz(a, d, r);
		if (size < 8) a64_add_imm(a, d, d, -(64 - 8*size));
		a64_put(L, in->dst, d);
		break;
	}
	default:
		TG_PANIC("a64: cannot lower op");
	}
}

////////////////////////////////////////////////////////////////
// Procedures
////////////////////////////////////////////////////////////////

// Whether `in` is a compare whose result only feeds the next instruction `n` through the flags.
TG_INTERNAL bool a64_can_fuse(a64Lower *L, tgInstr const *in, tgInstr const *n) {
	if (in->op != tgOp_ICmp && in->op != tgOp_FCmp) return false;
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
		       a64_is_const(L, n->b, &k) && a64_ext_value(k, tg_type_size(n->type), tgExt_Zero) == 0;
	}
	}
	return false;
}

// constants materialized at their uses and unread pure values emit nothing
TG_INTERNAL bool a64_skipped(a64Lower *L, tgInstr const *n) {
	if (n->op == tgOp_IConst && n->dst != 0 && L->is_const.data[n->dst]) return true;
	return n->dst != 0 && L->uses.data[n->dst] == 0 && tg_op_is_pure(n->op);
}

// the next instruction that emits code
TG_INTERNAL isize a64_next_code(a64Lower *L, tgBlock *b, isize i) {
	for (isize j = i+1; j < b->instrs.count; j++) {
		tgInstr const *n = &b->instrs.data[j];
		if (n->op == tgOp_Loc || n->op == tgOp_Scope || n->op == tgOp_Nop || a64_skipped(L, n)) continue;
		return j;
	}
	return -1;
}

TG_INTERNAL void a64_lower_free(a64Lower *L) {
	tg_array_free(&L->fixups);
	tg_array_free(&L->slot);
	tg_array_free(&L->uses);
	tg_array_free(&L->reg);
	tg_array_free(&L->is_const);
	tg_array_free(&L->cval);
	tg_array_free(&L->in_block);
	tg_array_free(&L->clean);
	tg_array_free(&L->local_reg);
	tg_array_free(&L->via);
	tg_array_free(&L->remat);
	tg_array_free(&L->rmem);
	tg_array_free(&L->zx);
}

// Lowers p, or returns false when a conditional branch cannot reach its target unless `far_branches`.
// A block that only one jump reaches goes right after the block jumping to it, so the jump becomes
// a fallthrough: a front end's entry block that jumps to a setup block made last, which jumps back
// to the body, no longer costs every call two branches. Only the order of the code changes.
TG_INTERNAL void a64_chain_blocks(tgProc *p, tgBlockPtrArray *order) {
	isize n = order->count;
	tgI32Array preds;
	tg_array_make(&preds, p->blocks.count);
	tgBoolArray done;
	tg_array_make(&done, p->blocks.count);
	for (isize i = 0; i < n; i++) {
		tgBlock *b = order->data[i];
		for (isize ii = 0; ii < b->instrs.count; ii++) {
			tgInstr const *in = &b->instrs.data[ii];
			if (in->op == tgOp_Jump) preds.data[in->imm]++;
			if (in->op == tgOp_Branch) { preds.data[in->imm]++; preds.data[in->c]++; }
		}
	}
	tgBlockPtrArray out;
	memset(&out, 0, sizeof(out));
	out.arena = tg_arena();
	tg_array_reserve(&out, n);
	for (isize i = 0; i < n; i++) {
		tgBlock *b = order->data[i];
		if (done.data[b->index]) continue;
		for (;;) {
			done.data[b->index] = true;
			tg_array_add(&out, b);
			if (b->instrs.count == 0) break;
			tgInstr const *last = &b->instrs.data[b->instrs.count-1];
			if (last->op != tgOp_Jump) break;
			tgBlock *t = p->blocks.data[last->imm];
			if (!t->placed || done.data[t->index] || preds.data[t->index] != 1 || t->cold != b->cold) break;
			b = t;
		}
	}
	TG_ASSERT(out.count == n);
	memcpy(order->data, out.data, (size_t)n * sizeof(order->data[0]));
	tg_array_free(&out);
	tg_array_free(&preds);
	tg_array_free(&done);
}

// The blocks a later block jumps back to: loop heads. Each starts on A64_LOOP_ALIGN bytes. Apple's
// cores fetch a loop much faster when its head does not sit late in a 32 byte chunk: graph's BFS
// loop ran anywhere from 445 to 700 ms with the code above it, and 432 ms aligned.
TG_INTERNAL void a64_loop_heads(tgProc *p, tgBlockPtrArray const *order, tgBoolArray *head) {
	tg_array_make(head, p->blocks.count);
	tgI32Array pos;
	tg_array_make(&pos, p->blocks.count);
	for (isize i = 0; i < order->count; i++) pos.data[order->data[i]->index] = (i32)i;
	for (isize i = 0; i < order->count; i++) {
		tgBlock *b = order->data[i];
		for (isize ii = 0; ii < b->instrs.count; ii++) {
			tgInstr const *in = &b->instrs.data[ii];
			if (in->op == tgOp_Jump && pos.data[in->imm] <= i) head->data[in->imm] = true;
			if (in->op == tgOp_Branch) {
				if (pos.data[in->imm] <= i) head->data[in->imm] = true;
				if (pos.data[in->c] <= i) head->data[in->c] = true;
			}
		}
	}
	tg_array_free(&pos);
}

// Lowers instruction i of block b, whose code falls through to next_block.
TG_INTERNAL void a64_lower_at(a64Lower *L, tgBlock *b, isize i, i32 next_block) {
	tgInstr const *in = &b->instrs.data[i];
	if (a64_skipped(L, in)) return;
	if (in->op == tgOp_Loc || in->op == tgOp_Nop) {
		a64_lower_instr(L, in);
		return;
	}
	L->next_block = i+1 == b->instrs.count ? next_block : -1;
	L->fuse = false;
	L->fold_addr = false;
	if (in->op == tgOp_ICmp || in->op == tgOp_FCmp) {
		isize j = a64_next_code(L, b, i);
		L->fuse = j >= 0 && a64_can_fuse(L, in, &b->instrs.data[j]);
	} else if (in->op == tgOp_Shl) {
		isize j = a64_next_code(L, b, i);
		u32 amount = 0;
		if (j >= 0 && a64_can_fold_shift(L, in, &b->instrs.data[j], &amount)) {
			L->shift_vreg = in->dst;
			L->shift_src = in->a;
			L->shift_amt = amount;
		}
	} else if (in->op == tgOp_Add) {
		isize j = a64_next_code(L, b, i);
		L->fold_addr = j >= 0 && a64_can_fold_addr(L, in, &b->instrs.data[j]);
	}
	u32 flags = L->flags_vreg;
	// only the instruction right after a store sees it
	L->stored.valid = L->stored.fresh;
	L->stored.fresh = false;
	a64_lower_instr(L, in);
	TG_ASSERT_MSG(flags == 0 || L->flags_vreg != flags, "a64: an op did not read the flags");
}

// The loop heads whose code a jump back to them repeats instead: a few instructions that only
// compute and test the loop's condition. The end of the loop then branches back into the body
// itself or falls out of the loop, as a C compiler's rotated loop does, where it took a jump to
// the head and a branch there. Running the head's code at the jump is running it after the jump:
// the registers are the same, and the head's line marks the repeat for the debugger. `pos` is
// each block's place in the order.
TG_INTERNAL void a64_dup_heads(a64Lower *L, tgBlockPtrArray const *order, tgBoolArray const *head, tgBoolArray *dup, tgI32Array *pos) {
	tgProc *p = L->p;
	tg_array_make(dup, p->blocks.count);
	tg_array_make(pos, p->blocks.count);
	for (isize i = 0; i < order->count; i++) pos->data[order->data[i]->index] = (i32)i;
	for (isize i = 1; i < order->count; i++) {
		tgBlock *b = order->data[i];
		if (!head->data[b->index] || b->cold || b->instrs.count == 0) continue;
		if (b->instrs.data[b->instrs.count-1].op != tgOp_Branch) continue;
		i32 code = 0;
		bool ok = true;
		for (isize k = 0; k + 1 < b->instrs.count && ok; k++) {
			tgInstr const *in = &b->instrs.data[k];
			switch (in->op) {
			case tgOp_Loc:
			case tgOp_Scope:
				break;
			case tgOp_Nop:
				ok = in->imm != 2;
				break;
			case tgOp_Load:
				ok = !(in->flags & tgInstrFlag_Volatile);
				code++;
				break;
			case tgOp_IConst:
			case tgOp_ICmp:
			case tgOp_FCmp:
			case tgOp_Add:
			case tgOp_Sub:
			case tgOp_And:
			case tgOp_Or:
			case tgOp_Xor:
			case tgOp_Shl:
			case tgOp_LShr:
			case tgOp_AShr:
			case tgOp_Zext:
			case tgOp_Sext:
			case tgOp_Trunc:
			case tgOp_Copy:
			case tgOp_Lea:
				if (!a64_skipped(L, in)) code++;
				break;
			default:
				ok = false;
				break;
			}
		}
		dup->data[b->index] = ok && code <= 4;
	}
}

TG_INTERNAL bool a64_lower_proc_with(tgProc *p, tgLowerOut *out, bool far_branches) {
	tgModule *m = p->m;
	a64Lower L;
	memset(&L, 0, sizeof(L));
	L.p = p;
	L.out = out;
	L.far_branches = far_branches;
	L.a.m = m;
	L.a.code = &out->text;
	L.a.relocs = &out->relocs;
	tg_array_make_cap(&L.fixups, 0, 64);

	a64_lower_layout(&L);

	tgAsm *a = &L.a;
	// 32 byte aligned procedure starts, for the loop heads in them (A64_LOOP_ALIGN)
	out->align = A64_LOOP_ALIGN;
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
	tg_array_reserve(&dbg.scope_marks, 16);

	// prologue
	TG_ASSERT_MSG(!p->naked, "a64: naked procedures are not supported");
	a64_emit(a, 0xA9BF7BFD); // stp x29, x30, [sp, #-16]!
	a64_mov_sp(a, A64_FP, A64_SP);
	if (L.frame_size > 0) {
		a64_add_imm(a, A64_SP, A64_SP, -L.frame_size);
	}
	a64Pair prs[9];
	i32 npairs = a64_list_pairs(L.pairs, prs);
	for (i32 i = 0; i < npairs; i++) {
		a64_pair(a, prs[i].fp, false, prs[i].hi, prs[i].lo, A64_FP, prs[i].off);
	}
	dbg.saved_at = (i32)(tg_pos(a) - L.proc_start);
	for (isize i = 0; i < p->locals.count; i++) {
		tgLocal const *l = &p->locals.data[i];
		if (l->over_align <= 16) continue;
		u32 shift = 0;
		while (((i64)1 << shift) < l->over_align) shift++;
		a64_add_imm(a, A64_TA, A64_FP, l->raw_offset + l->over_align - 1);
		a64_lsr_imm(a, A64_TA, A64_TA, shift);
		a64_lsl_imm(a, A64_TA, A64_TA, shift);
		a64_str(a, 8, A64_TA, A64_FP, l->frame_offset);
	}
	for (isize i = 0; i < p->params_in.count; i++) {
		tgParamIn const *in = &p->params_in.data[i];
		if (in->dst.kind == tgMem_Local && L.local_reg.data[in->dst.base] >= 0) continue;
		a64Addr dst = a64_mem(&L, in->dst, A64_TA);
		switch (in->loc) {
		case tgLoc_Gpr:
			a64_store_bytes(&L, dst, in->reg, in->size);
			break;
		case tgLoc_Xmm:
			a64_str_fp(a, in->size, in->reg, dst.base, dst.off);
			break;
		case tgLoc_Stack: {
			a64Addr src;
			src.base = A64_FP;
			src.off = 16 + in->stack_offset;
			if (in->size == 16) {
				a64_ldr_fp(a, 16, A64_F0, src.base, src.off);
				a64_str_fp(a, 16, A64_F0, dst.base, dst.off);
			} else {
				a64_load_bytes(&L, A64_T0, src, in->size, tgExt_None);
				a64_store_bytes(&L, dst, A64_T0, in->size);
			}
			break;
		}
		}
	}
	// the promoted ones stay in the register they arrive in or move to a callee saved one
	for (isize i = 0; i < p->params_in.count; i++) {
		tgParamIn const *in = &p->params_in.data[i];
		if (in->dst.kind != tgMem_Local || L.local_reg.data[in->dst.base] < 0) continue;
		i8 lr = L.local_reg.data[in->dst.base];
		if (lr >= TG_FREG) {
			u8 vr = (u8)(lr - TG_FREG);
			if (in->loc == tgLoc_Xmm) {
				if (vr != in->reg) a64_fmov_reg(a, vr, in->reg);
			} else {
				TG_ASSERT(in->loc == tgLoc_Stack);
				a64_ldr_fp(a, in->size, vr, A64_FP, 16 + in->stack_offset);
			}
		} else if (in->loc == tgLoc_Gpr) {
			a64_extend(a, false, in->size, (u8)lr, in->reg);
		} else {
			TG_ASSERT(in->loc == tgLoc_Stack);
			a64_ldr(a, in->size, false, (u8)lr, A64_FP, 16 + in->stack_offset);
		}
	}
	// like LLVM's prologue_end: a breakpoint on the procedure stops here, its parameters in place. That is
	// after the prologue's moves, or with tyg_debug_param after the front end's stores, at its marker.
	bool prologue_mark = tg_debug_info() && dbg.line > 0 && !p->naked;
	if (prologue_mark && p->params_ready == 0) {
		dbg.prologue_end = (i32)(tg_pos(a) - L.proc_start);
		a64_emit(a, 0xD503201F); // nop
	}

	// cold blocks go last, so the hot path falls through
	tgBlockPtrArray order;
	memset(&order, 0, sizeof(order));
	order.arena = tg_arena();
	tg_array_reserve(&order, p->order.count);
	for (isize i = 0; i < p->order.count; i++) if (!p->order.data[i]->cold) tg_array_add(&order, p->order.data[i]);
	for (isize i = 0; i < p->order.count; i++) if (p->order.data[i]->cold)  tg_array_add(&order, p->order.data[i]);
	a64_chain_blocks(p, &order);
	tgBoolArray head;
	a64_loop_heads(p, &order, &head);
	tgBoolArray dup;
	tgI32Array pos;
	a64_dup_heads(&L, &order, &head, &dup, &pos);

	for (isize bi = 0; bi < order.count; bi++) {
		tgBlock *b = order.data[bi];
		i32 next_block = bi+1 < order.count ? order.data[bi+1]->index : -1;
		if (head.data[b->index]) {
			while ((tg_pos(a) - L.proc_start) % A64_LOOP_ALIGN != 0) a64_emit(a, 0xD503201F); // nop
		}
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
			if (in->op == tgOp_Nop && in->imm == 2) {
				if (prologue_mark) {
					dbg.prologue_end = (i32)(tg_pos(a) - L.proc_start);
					a64_emit(a, 0xD503201F); // nop
				}
				continue;
			}
			if (in->op == tgOp_Jump && i+1 == b->instrs.count && dup.data[in->imm] && pos.data[in->imm] <= bi) {
				// the loop's condition again, instead of the jump back to it
				tgBlock *h = p->blocks.data[in->imm];
				memset(&L.stored, 0, sizeof(L.stored));
				tgScopeMark hmark;
				hmark.code_offset = (i32)(tg_pos(a) - L.proc_start);
				hmark.scope = h->debug_scope;
				hmark.cold = b->cold;
				tg_array_add(&dbg.scope_marks, hmark);
				for (isize k = 0; k < h->instrs.count; k++) {
					tgInstr const *hin = &h->instrs.data[k];
					if (hin->op == tgOp_Scope) {
						tgScopeMark smark;
						smark.code_offset = (i32)(tg_pos(a) - L.proc_start);
						smark.scope = (i32)hin->imm;
						smark.cold = b->cold;
						tg_array_add(&dbg.scope_marks, smark);
						continue;
					}
					a64_lower_at(&L, h, k, next_block);
				}
				continue;
			}
			a64_lower_at(&L, b, i, next_block);
		}
		memset(&L.stored, 0, sizeof(L.stored));
		TG_ASSERT(L.flags_vreg == 0 && L.shift_vreg == 0 && L.addr_vreg == 0);
	}
	for (isize i = 0; i < L.fixups.count; i++) {
		a64Fixup const *f = &L.fixups.data[i];
		tgBlock *target = p->blocks.data[f->block];
		TG_ASSERT_MSG(target->placed, "jump to an unplaced block");
		if (!f->imm19) {
			a64_patch_b(a, f->at, target->code_offset);
		} else if (!a64_patch_imm19(a, f->at, target->code_offset)) {
			tg_array_free(&dbg.scope_marks);
			tg_array_free(&order);
			tg_array_free(&head);
			tg_array_free(&dup);
			tg_array_free(&pos);
			a64_lower_free(&L);
			return false;
		}
	}

	dbg.end = tg_pos(a);
	dbg.line_entry_count = (i32)(out->lines.count - dbg.line_entry_start);

	// dwarf numbers x19-x28 as themselves and d8-d15 as 72-79
	tg_array_reserve(&dbg.saved_regs, 2*npairs);
	for (i32 i = 0; i < npairs; i++) {
		tgSavedReg r_lo, r_hi;
		r_lo.dwarf_reg = (prs[i].fp ? 64 : 0) + prs[i].lo;
		r_lo.frame_offset = prs[i].off + 8;
		r_hi.dwarf_reg = (prs[i].fp ? 64 : 0) + prs[i].hi;
		r_hi.frame_offset = prs[i].off;
		tg_array_add(&dbg.saved_regs, r_lo);
		tg_array_add(&dbg.saved_regs, r_hi);
	}

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
				v.dwarf_reg = lr >= TG_FREG ? (u8)(64 + lr - TG_FREG) : (u8)lr; // v0 is 64
			}
		}
		tg_array_add(&dbg.vars, v);
	}
	out->dbg = dbg;

	tg_array_free(&order);
	tg_array_free(&head);
	tg_array_free(&dup);
	tg_array_free(&pos);
	a64_lower_free(&L);
	return true;
}

TG_INTERNAL void a64_lower_proc(tgProc *p, tgLowerOut *out) {
	if (a64_lower_proc_with(p, out, false)) {
		return;
	}
	// a conditional branch is out of range: start over with ones that reach anywhere
	out->text.count = 0;
	out->relocs.count = 0;
	out->lines.count = 0;
	out->pending.count = 0;
	bool ok = a64_lower_proc_with(p, out, true);
	TG_ASSERT(ok);
	(void)ok;
}
