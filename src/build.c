// The public procedure builder: tyg_proc_begin ... tyg_proc_end and everything that
// builds a procedure's IR in between, including call lowering, returns, parameters,
// C varargs and the per-procedure debug info.
//
// A failure cannot leave the builder with a longjmp, since the caller's code is not inside
// a setjmp. So a failure records p->fail_reason (the first one wins), every
// builder call after it is a no-op that returns a fresh dummy value, and tyg_proc_end
// discards the procedure and reports the reason.

////////////////////////////////////////////////////////////////
// Failure
////////////////////////////////////////////////////////////////

TG_INTERNAL void tg_fail(tgProc *p, char const *reason) {
	if (p->fail_reason == NULL) p->fail_reason = reason;
}

////////////////////////////////////////////////////////////////
// IR building
////////////////////////////////////////////////////////////////

TG_INTERNAL u32 tg_new_vreg(tgProc *p, tgType t) {
	TG_ASSERT(t != tgType_None);
	u32 r = (u32)p->vregs.count;
	tg_array_add(&p->vregs, t);
	// a rolled back inline body leaves stale entries
	if (p->vinfo.count > (isize)r) p->vinfo.count = r;
	while (p->vinfo.count < (isize)r) {
		tgVregInfo none = {0, ~0ull, false, 0, 0, 0, 0};
		tg_array_add(&p->vinfo, none);
	}
	{
		tgVregInfo info = {0, ~0ull, false, 0, 0, 0, 0};
		tg_array_add(&p->vinfo, info);
	}
	return r;
}

////////////////////////////////////////////////////////////////
// Folding. Every vreg has one definition that comes before its uses, so what is known of
// it holds at every use.
////////////////////////////////////////////////////////////////

TG_INTERNAL u64 tg_width_mask(tgType t) {
	i32 size = tg_type_size(t);
	return size >= 8 ? ~0ull : (1ull << (8*size)) - 1;
}

// v as the `t` value it is, sign extended
TG_INTERNAL i64 tg_fold_norm(tgType t, i64 v) {
	i32 size = tg_type_size(t);
	if (size >= 8) return v;
	u32 sh = (u32)(64 - 8*size);
	return (i64)((u64)v << sh) >> sh;
}

TG_INTERNAL bool tg_known(tgProc *p, u32 v, i64 *out) {
	if (v == 0 || (isize)v >= p->vinfo.count || !p->vinfo.data[v].is_k) return false;
	*out = p->vinfo.data[v].k;
	return true;
}

// the largest value v can hold, unsigned
TG_INTERNAL u64 tg_umax(tgProc *p, u32 v) {
	if (v == 0 || (isize)v >= p->vinfo.count) return ~0ull;
	tgType t = p->vregs.data[v];
	if (!tg_type_is_int(t)) return ~0ull;
	{
		tgVregInfo const *i = &p->vinfo.data[v];
		if (i->is_k) return (u64)i->k & tg_width_mask(t);
		return tg_min(i->umax, tg_width_mask(t));
	}
}

TG_INTERNAL void tg_note_umax(tgProc *p, u32 v, u64 umax) {
	if (v != 0 && (isize)v < p->vinfo.count) p->vinfo.data[v].umax = umax;
}

TG_INTERNAL bool tg_is_pow2_u64(u64 v, u32 *log2) {
	u32 n = 0;
	if (v == 0 || (v & (v - 1)) != 0) return false;
	while ((1ull << n) != v) n++;
	*log2 = n;
	return true;
}

TG_INTERNAL tgBlock *tg_new_block(tgProc *p) {
	tgBlock *b = tg_alloc_item(tgBlock);
	b->index = (i32)p->blocks.count;
	b->instrs.arena = tg_arena();
	tg_array_add(&p->blocks, b);
	return b;
}

TG_INTERNAL void tg_start_block(tgProc *p, tgBlock *b, bool line_row) {
	TG_ASSERT(!b->placed);
	b->placed = true;
	b->debug_scope = p->debug_scope;
	tg_array_add(&p->order, b);
	p->curr = b;
	// a block starts its own line row, so a debugger's step stops where a jump lands, as at a loop's head;
	// without one it goes on in the row before, like LLVM loop heads that start with a reload of no line
	if (line_row && p->last_line > 0) {
		tgInstr i = tg_instr(tgOp_Loc);
		i.imm = p->last_line;
		i.a = (u32)p->file_id;
		i.b = (u32)p->last_column;
		tg_array_add(&b->instrs, i);
	}
}

TG_INTERNAL bool tg_is_terminator(tgOp op) {
	switch (op) {
	case tgOp_Jump:
	case tgOp_Branch:
	case tgOp_Ret:
	case tgOp_Unreachable:
		return true;
	}
	return false;
}

TG_INTERNAL bool tg_block_terminated(tgBlock *b) {
	if (b == NULL) return true;
	if (b->instrs.count == 0) return false;
	return tg_is_terminator(b->instrs.data[b->instrs.count-1].op);
}

TG_INTERNAL bool tg_curr_terminated(tgProc *p) {
	return tg_block_terminated(p->curr);
}

TG_INTERNAL tgInstr *tg_emit(tgProc *p, tgInstr instr) {
	if (tg_curr_terminated(p)) {
		// dead code after a terminator, put it in a block that is never placed
		p->curr = tg_new_block(p);
	}
	tg_array_add(&p->curr->instrs, instr);
	return &p->curr->instrs.data[p->curr->instrs.count-1];
}

TG_INTERNAL tgInstr tg_instr_t(tgOp op, tgType type) {
	tgInstr i = tg_instr(op);
	i.type = type;
	return i;
}

TG_INTERNAL u32 tg_iconst(tgProc *p, tgType t, i64 v) {
	tgInstr i = tg_instr_t(tgOp_IConst, t);
	i.dst = tg_new_vreg(p, t);
	i.imm = v;
	tg_emit(p, i);
	if (tg_type_is_int(t)) {
		p->vinfo.data[i.dst].is_k = true;
		p->vinfo.data[i.dst].k = tg_fold_norm(t, v);
	}
	return i.dst;
}

TG_INTERNAL u32 tg_fconst(tgProc *p, tgType t, f64 v) {
	tgInstr i = tg_instr_t(tgOp_FConst, t);
	i.dst = tg_new_vreg(p, t);
	if (t == tgType_F32) {
		f32 f = (f32)v;
		u32 bits = 0;
		memmove(&bits, &f, 4);
		i.imm = bits;
	} else {
		u64 bits = 0;
		memmove(&bits, &v, 8);
		i.imm = (i64)bits;
	}
	tg_emit(p, i);
	return i.dst;
}

TG_INTERNAL u32 tg_lea_mem(tgProc *p, tgMem mem) {
	if (mem.kind == tgMem_Reg && mem.offset == 0) {
		return mem.base;
	}
	{
		tgInstr i = tg_instr_t(tgOp_Lea, tgType_I64);
		i.dst = tg_new_vreg(p, tgType_I64);
		i.mem = mem;
		tg_emit(p, i);
		return i.dst;
	}
}

// The vreg stored to the frame local `m` earlier in the current block, if nothing since may
// have written it, or 0.
TG_INTERNAL u32 tg_stored_value(tgProc *p, tgType t, tgMem m) {
	isize stop, i;
	if (p->curr == NULL || m.kind != tgMem_Local || tg_curr_terminated(p)) return 0;
	// a short look back keeps long blocks linear
	stop = tg_max(p->curr->instrs.count - 64, 0);
	for (i = p->curr->instrs.count-1; i >= stop; i--) {
		tgInstr const *in = &p->curr->instrs.data[i];
		switch (in->op) {
		case tgOp_Nop:
		case tgOp_Loc:
		case tgOp_Scope:
		case tgOp_Load:
			continue;
		case tgOp_Store:
			if (in->flags & tgInstrFlag_Volatile) return 0;
			if (in->mem.kind == tgMem_Reg) return 0; // may point into the local
			if (in->mem.kind != tgMem_Local || in->mem.base != m.base) continue;
			if (in->mem.offset == m.offset && in->type == t) return in->a;
			if (in->mem.offset + tg_type_size(in->type) <= m.offset || m.offset + tg_type_size(t) <= in->mem.offset) continue;
			return 0;
		}
		if (!tg_op_is_pure(in->op)) return 0;
	}
	return 0;
}

TG_INTERNAL u32 tg_load(tgProc *p, tgType t, tgMem mem) {
	u32 v = tg_stored_value(p, t, mem);
	if (v != 0) return v;
	{
		tgInstr i = tg_instr_t(tgOp_Load, t);
		i.dst = tg_new_vreg(p, t);
		i.mem = mem;
		tg_emit(p, i);
		return i.dst;
	}
}

TG_INTERNAL void tg_store(tgProc *p, tgType t, tgMem mem, u32 v) {
	tgInstr i = tg_instr_t(tgOp_Store, t);
	i.a = v;
	i.mem = mem;
	tg_emit(p, i);
}

// The x86 shifts, which the lowerings copy: narrow values shift as 32 bits, the count masked.
TG_INTERNAL i64 tg_fold_shift(tgOp op, tgType t, i64 x, i64 c) {
	i32 size = tg_type_size(t);
	u32 n = (u32)(c & (size == 8 ? 63 : 31));
	u64 ux = (u64)x & tg_width_mask(t);
	switch (op) {
	case tgOp_Shl:  return (i64)(ux << n);
	case tgOp_LShr: return (i64)(ux >> n);
	case tgOp_AShr: return tg_fold_norm(t, x) >> n;
	}
	return 0;
}

TG_INTERNAL u32 tg_binop(tgProc *p, tgOp op, tgType t, u32 a, u32 b);

// base + k at t's width: base itself for 0, a sub for a negative k
TG_INTERNAL u32 tg_add_const(tgProc *p, tgType t, u32 base, i64 k) {
	k = tg_fold_norm(t, k);
	if (k == 0) return base;
	i64 neg = tg_fold_norm(t, (i64)(0ull - (u64)k));
	if (k < 0 && neg > 0) return tg_binop(p, tgOp_Sub, t, base, tg_iconst(p, t, neg));
	return tg_binop(p, tgOp_Add, t, base, tg_iconst(p, t, k));
}

TG_INTERNAL u32 tg_binop(tgProc *p, tgOp op, tgType t, u32 a, u32 b) {
	if (tg_type_is_int(t)) {
		i64 x = 0, y = 0;
		bool kx = tg_known(p, a, &x);
		bool ky = tg_known(p, b, &y);
		u64 mask = tg_width_mask(t);
		u64 ux = (u64)x & mask, uy = (u64)y & mask;
		if (kx && ky) {
			bool ok = true;
			i64 r = 0;
			i64 min_val = tg_fold_norm(t, (i64)(1ull << (8*tg_type_size(t)-1)));
			switch (op) {
			case tgOp_Add: r = (i64)((u64)x + (u64)y); break;
			case tgOp_Sub: r = (i64)((u64)x - (u64)y); break;
			case tgOp_Mul: r = (i64)((u64)x * (u64)y); break;
			case tgOp_And: r = x & y; break;
			case tgOp_Or:  r = x | y; break;
			case tgOp_Xor: r = x ^ y; break;
			case tgOp_Shl:
			case tgOp_LShr:
			case tgOp_AShr: r = tg_fold_shift(op, t, x, y); break;
			case tgOp_UDiv: ok = uy != 0; if (ok) r = (i64)(ux / uy); break;
			case tgOp_URem: ok = uy != 0; if (ok) r = (i64)(ux % uy); break;
			case tgOp_SDiv: ok = y != 0 && !(y == -1 && x == min_val); if (ok) r = x / y; break;
			case tgOp_SRem: ok = y != 0 && !(y == -1 && x == min_val); if (ok) r = x % y; break;
			default: ok = false; break;
			}
			if (ok) return tg_iconst(p, t, tg_fold_norm(t, r));
		}
		// x op identity
		if (ky) {
			u32 n = 0;
			switch (op) {
			case tgOp_Add: case tgOp_Sub: case tgOp_Or: case tgOp_Xor:
			case tgOp_Shl: case tgOp_LShr: case tgOp_AShr:
				if (y == 0) return a;
				break;
			case tgOp_Mul:
			case tgOp_UDiv:
			case tgOp_SDiv:
				if (y == 1) return a;
				break;
			case tgOp_And:
				if (uy == mask) return a;
				if (y == 0) return b;
				break;
			}
			if (tg_is_pow2_u64(uy, &n)) {
				if (op == tgOp_Mul)  return tg_binop(p, tgOp_Shl, t, a, tg_iconst(p, t, n));
				if (op == tgOp_UDiv) return tg_binop(p, tgOp_LShr, t, a, tg_iconst(p, t, n));
				if (op == tgOp_URem) return tg_binop(p, tgOp_And, t, a, tg_iconst(p, t, tg_fold_norm(t, (i64)(uy - 1))));
			}
		}
		if (kx) {
			u32 n = 0;
			switch (op) {
			case tgOp_Add: case tgOp_Or: case tgOp_Xor:
				if (x == 0) return b;
				break;
			case tgOp_Mul:
				if (x == 1) return b;
				break;
			case tgOp_And:
				if (ux == mask) return b;
				if (x == 0) return a;
				break;
			}
			if (op == tgOp_Mul && tg_is_pow2_u64(ux, &n)) return tg_binop(p, tgOp_Shl, t, b, tg_iconst(p, t, n));
		}
		// (x + c1) + c2 and the like: x + (c1 + c2), one add however long the chain
		if (ky && (op == tgOp_Add || op == tgOp_Sub) && p->vinfo.data[a].kbase != 0) {
			u64 k1 = (u64)p->vinfo.data[a].kadd;
			return tg_add_const(p, t, p->vinfo.data[a].kbase, (i64)(op == tgOp_Add ? k1 + (u64)y : k1 - (u64)y));
		}
		if (kx && op == tgOp_Add && p->vinfo.data[b].kbase != 0) {
			return tg_add_const(p, t, p->vinfo.data[b].kbase, (i64)((u64)p->vinfo.data[b].kadd + (u64)x));
		}
	}
	{
		tgInstr i = tg_instr_t(op, t);
		i.dst = tg_new_vreg(p, t);
		i.a = a;
		i.b = b;
		tg_emit(p, i);
		if (tg_type_is_int(t)) {
			i64 k = 0;
			tgVregInfo *vi = &p->vinfo.data[i.dst];
			if (op == tgOp_Add && tg_known(p, b, &k))      { vi->kbase = a; vi->kadd = k; }
			else if (op == tgOp_Sub && tg_known(p, b, &k)) { vi->kbase = a; vi->kadd = tg_fold_norm(t, (i64)(0ull - (u64)k)); }
			else if (op == tgOp_Add && tg_known(p, a, &k)) { vi->kbase = b; vi->kadd = k; }
			else if (op == tgOp_Add && t == tgType_I64)    { vi->suma = a; vi->sumb = b; }
		}
		if (tg_type_is_int(t)) {
			i64 y = 0;
			switch (op) {
			case tgOp_And:
				tg_note_umax(p, i.dst, tg_min(tg_umax(p, a), tg_umax(p, b)));
				break;
			case tgOp_LShr:
				if (tg_known(p, b, &y) && y >= 0 && y < 8*tg_type_size(t)) tg_note_umax(p, i.dst, tg_umax(p, a) >> y);
				break;
			case tgOp_URem:
				if (tg_known(p, b, &y) && ((u64)y & tg_width_mask(t)) != 0) tg_note_umax(p, i.dst, ((u64)y & tg_width_mask(t)) - 1);
				break;
			}
		}
		return i.dst;
	}
}

TG_INTERNAL u32 tg_unop(tgProc *p, tgOp op, tgType t, u32 a) {
	i64 x = 0;
	if (tg_type_is_int(t) && tg_known(p, a, &x)) {
		if (op == tgOp_Neg) return tg_iconst(p, t, tg_fold_norm(t, (i64)(0ull - (u64)x)));
		if (op == tgOp_Not) return tg_iconst(p, t, tg_fold_norm(t, ~x));
	}
	{
		tgInstr i = tg_instr_t(op, t);
		i.dst = tg_new_vreg(p, t);
		i.a = a;
		tg_emit(p, i);
		return i.dst;
	}
}

// a <cond> b when it is known, -1 otherwise
TG_INTERNAL i32 tg_fold_cmp(tgProc *p, tgCond cond, tgType t, u32 a, u32 b) {
	i64 x = 0, y = 0;
	bool kx, ky;
	u64 mask;
	if (!tg_type_is_int(t)) return -1;
	kx = tg_known(p, a, &x);
	ky = tg_known(p, b, &y);
	mask = tg_width_mask(t);
	if (kx && ky) {
		u64 ux = (u64)x & mask, uy = (u64)y & mask;
		switch (cond) {
		case tgCond_EQ:  return x == y;
		case tgCond_NE:  return x != y;
		case tgCond_SLT: return x < y;
		case tgCond_SLE: return x <= y;
		case tgCond_SGT: return x > y;
		case tgCond_SGE: return x >= y;
		case tgCond_ULT: return ux < uy;
		case tgCond_ULE: return ux <= uy;
		case tgCond_UGT: return ux > uy;
		case tgCond_UGE: return ux >= uy;
		}
		return -1;
	}
	// a value below a constant bound, like a masked shift count
	if (ky) {
		u64 uy = (u64)y & mask;
		u64 hi = tg_umax(p, a);
		if (cond == tgCond_ULT && hi < uy)  return 1;
		if (cond == tgCond_UGE && hi < uy)  return 0;
		if (cond == tgCond_ULE && hi <= uy) return 1;
		if (cond == tgCond_UGT && hi <= uy) return 0;
	}
	return -1;
}

TG_INTERNAL u32 tg_cmp(tgProc *p, tgCond cond, tgType t, u32 a, u32 b) {
	i32 known = tg_fold_cmp(p, cond, t, a, b);
	i64 y = 0;
	if (known >= 0) return tg_iconst(p, tgType_I8, known);
	// a bool tested against zero is itself
	if (t == tgType_I8 && cond == tgCond_NE && tg_known(p, b, &y) && y == 0 && tg_umax(p, a) <= 1) return a;
	{
		tgInstr i = tg_instr_t(tg_type_is_float(t) ? tgOp_FCmp : tgOp_ICmp, t);
		i.aux = cond;
		i.dst = tg_new_vreg(p, tgType_I8);
		i.a = a;
		i.b = b;
		tg_emit(p, i);
		tg_note_umax(p, i.dst, 1);
		return i.dst;
	}
}

TG_INTERNAL u32 tg_convop(tgProc *p, tgOp op, tgType dst, tgType src, u32 a) {
	i64 x = 0;
	if (tg_type_is_int(dst) && tg_type_is_int(src) && tg_known(p, a, &x)) {
		switch (op) {
		case tgOp_Zext:  return tg_iconst(p, dst, tg_fold_norm(dst, (i64)((u64)x & tg_width_mask(src))));
		case tgOp_Sext:
		case tgOp_Trunc: return tg_iconst(p, dst, tg_fold_norm(dst, x));
		}
	}
	{
		tgInstr i = tg_instr_t(op, dst);
		i.aux = src;
		i.dst = tg_new_vreg(p, dst);
		i.a = a;
		tg_emit(p, i);
		if (tg_type_is_int(dst) && tg_type_is_int(src)) {
			if (op == tgOp_Zext)  tg_note_umax(p, i.dst, tg_umax(p, a));
			if (op == tgOp_Trunc) tg_note_umax(p, i.dst, tg_min(tg_umax(p, a), tg_width_mask(dst)));
		}
		return i.dst;
	}
}

TG_INTERNAL u32 tg_select(tgProc *p, tgType t, u32 cond, u32 a, u32 b) {
	i64 c = 0;
	if (tg_known(p, cond, &c)) return (c & 0xff) != 0 ? a : b;
	if (a == b) return a;
	{
		tgInstr i = tg_instr_t(tgOp_Select, t);
		i.dst = tg_new_vreg(p, t);
		i.a = cond;
		i.b = a;
		i.c = b;
		tg_emit(p, i);
		if (tg_type_is_int(t)) tg_note_umax(p, i.dst, tg_max(tg_umax(p, a), tg_umax(p, b)));
		return i.dst;
	}
}

TG_INTERNAL void tg_memcopy(tgProc *p, tgMem dst, tgMem src, i64 size) {
	tgInstr i;
	if (size <= 0) return;
	// both memory operands are needed, so both go through pointer vregs
	i = tg_instr(tgOp_MemCopy);
	i.a = tg_lea_mem(p, dst);
	i.b = tg_lea_mem(p, src);
	i.imm = size;
	tg_emit(p, i);
}

// like tg_memcopy, the ranges may overlap
TG_INTERNAL void tg_memmove(tgProc *p, tgMem dst, tgMem src, i64 size) {
	tgInstr i;
	if (size <= 0) return;
	i = tg_instr(tgOp_MemMove);
	i.a = tg_lea_mem(p, dst);
	i.b = tg_lea_mem(p, src);
	i.imm = size;
	tg_emit(p, i);
}

TG_INTERNAL void tg_memzero(tgProc *p, tgMem dst, i64 size) {
	tgInstr i;
	if (size <= 0) return;
	i = tg_instr(tgOp_MemZero);
	i.mem = dst;
	i.imm = size;
	tg_emit(p, i);
}

TG_INTERNAL void tg_jump(tgProc *p, tgBlock *target) {
	tgInstr i;
	if (tg_curr_terminated(p)) return;
	i = tg_instr(tgOp_Jump);
	i.imm = target->index;
	tg_emit(p, i);
}

TG_INTERNAL void tg_branch(tgProc *p, u32 cond, tgBlock *then_, tgBlock *else_) {
	i64 c = 0;
	tgInstr i;
	if (tg_curr_terminated(p)) return;
	if (tg_known(p, cond, &c)) {
		tg_jump(p, (c & 0xff) != 0 ? then_ : else_);
		return;
	}
	i = tg_instr(tgOp_Branch);
	i.a = cond;
	i.imm = then_->index;
	i.c = (u32)else_->index;
	tg_emit(p, i);
}

TG_INTERNAL void tg_unreachable(tgProc *p) {
	if (tg_curr_terminated(p)) return;
	tg_emit(p, tg_instr(tgOp_Unreachable));
}

TG_INTERNAL i32 tg_add_local_raw(tgProc *p, i64 size, i64 align) {
	tgLocal l;
	if (align > 4096) {
		tg_fail(p, "over-aligned local");
		align = 16;
	}
	memset(&l, 0, sizeof(l));
	l.size = tg_max(size, (i64)1);
	l.align = tg_max(align, (i64)1);
	if (align > 16) {
		l.over_align = align;
		l.align = 16;
	}
	tg_array_add(&p->locals, l);
	return (i32)(p->locals.count-1);
}

// pointer arithmetic helpers
TG_INTERNAL u32 tg_ptr_add_const(tgProc *p, u32 ptr, i64 offset) {
	if (offset == 0) return ptr;
	return tg_lea_mem(p, tg_mem(tgMem_Reg, ptr, (i32)offset));
}

// A thread local's storage. On x86-64 Linux an executable reaches it from the thread pointer
// (initial exec), anything else may be loaded with dlopen, so it asks __tls_get_addr (general
// dynamic) like LLVM. tinygen does not know the build mode, so position independent code
// takes the general dynamic way, which also works in an executable.
TG_INTERNAL tgMem tg_tls_mem(tgProc *p, i32 sym) {
	tgInstr in;
	if (tg_is_darwin() || tg_is_arm64()) {
		u32 addr;
		i64 align;
		// macOS calls the variable's TLV descriptor in every build mode, arm64 Linux has no
		// thread pointer relative addressing mode, see a64_tls_calls
		in = tg_instr_t(tgOp_TlsAddr, tgType_I64);
		in.imm = sym;
		in.dst = tg_new_vreg(p, tgType_I64);
		tg_emit(p, in);
		addr = in.dst;
		align = p->m->symbols.data[sym].realign;
		if (align != 0) {
			// the variable starts at the next multiple of its alignment
			u32 pad = tg_binop(p, tgOp_And, tgType_I64, tg_unop(p, tgOp_Neg, tgType_I64, addr), tg_iconst(p, tgType_I64, align-1));
			addr = tg_binop(p, tgOp_Add, tgType_I64, addr, pad);
		}
		return tg_mem(tgMem_Reg, addr, 0);
	}
	if (!tg_is_pic() || tg_is_win64()) {
		// Windows goes through _tls_index in every build mode, see tg_win64_tls_opnd
		return tg_mem(tgMem_Sym, (u32)sym, 0);
	}
	tyg_symbol(p->m, "__tls_get_addr", TYG_SYM_FUNC | TYG_SYM_GLOBAL | TYG_SYM_FOREIGN);
	in = tg_instr_t(tgOp_TlsAddr, tgType_I64);
	in.imm = sym;
	in.dst = tg_new_vreg(p, tgType_I64);
	tg_emit(p, in);
	return tg_mem(tgMem_Reg, in.dst, 0);
}

////////////////////////////////////////////////////////////////
// Debug info
////////////////////////////////////////////////////////////////

TG_INTERNAL void tg_set_debug_loc(tgProc *p, i32 file_id, i32 line, i32 column) {
	tgInstr i;
	if (line <= 0 || file_id <= 0) return;
	if (line == p->last_line && file_id == p->file_id) return;
	p->last_line = line;
	p->last_column = column;
	p->file_id = file_id;
	i = tg_instr(tgOp_Loc);
	i.imm = line;
	i.a = (u32)file_id;
	i.b = (u32)column;
	tg_emit(p, i);
}

TG_INTERNAL void tg_set_debug_scope(tgProc *p, i32 scope) {
	tgInstr i;
	p->debug_scope = scope;
	i = tg_instr(tgOp_Scope);
	i.imm = scope;
	tg_emit(p, i);
}

TG_INTERNAL void tg_open_scope(tgProc *p) {
	tg_array_add(&p->debug_scope_parent, p->debug_scope);
	tg_set_debug_scope(p, (i32)p->debug_scope_parent.count-1);
}

TG_INTERNAL void tg_close_scope(tgProc *p) {
	tg_set_debug_scope(p, p->debug_scope_parent.data[p->debug_scope]);
}

////////////////////////////////////////////////////////////////
// Calls
////////////////////////////////////////////////////////////////

// The SysV integer argument registers
static u8 const tg_build_sysv_int_regs[6] = {RDI, RSI, RDX, RCX, R8, R9};

// A scalar held in memory, for an ABI piece that takes part of it.
TG_INTERNAL tgMem tg_value_to_mem(tgProc *p, tgType t, u32 v) {
	i32 size = tg_type_size(t);
	tgMem m = tg_mem(tgMem_Local, (u32)tg_add_local_raw(p, size, size), 0);
	tg_store(p, t, m, v);
	return m;
}

// An argument (or a return value): the scalar `val`, or when val is 0, the value in memory at `mem`.
TG_INTERNAL void tg_add_piece_arg(tgProc *p, tgCallArgArray *out, tgAbiPiece const *piece, u32 val, tgMem mem, tyg_type const *type) {
	tgCallArg a;
	tgType st = tg_t_scalar(type);
	bool whole_scalar = val != 0 && piece->src_offset == 0 && st != tgType_None &&
	                    tg_type_size(st) == piece->size && tg_type_is_float(st) == (piece->loc == tgLoc_Xmm || (piece->loc == tgLoc_Stack && tg_type_is_float(piece->type)));
	memset(&a, 0, sizeof(a));
	a.type = piece->type;
	a.size = piece->size;
	a.ext = piece->ext;
	if (whole_scalar) {
		a.vreg = val;
		a.type = st;
		if (tg_type_is_int(st) && tg_type_size(st) < 4) {
			a.ext = type->is_signed ? tgExt_Sign : tgExt_Zero;
		}
		switch (piece->loc) {
		case tgLoc_Gpr:   a.kind = tgCallArg_Gpr; a.reg = piece->reg; break;
		case tgLoc_Xmm:   a.kind = tgCallArg_Xmm; a.reg = piece->reg; break;
		case tgLoc_Stack: a.kind = tgCallArg_Stack; a.stack_offset = piece->stack_offset; break;
		}
	} else {
		tgMem m = val != 0 ? tg_value_to_mem(p, p->vregs.data[val], val) : mem;
		a.mem = tg_mem_offset(m, piece->src_offset);
		switch (piece->loc) {
		case tgLoc_Gpr:   a.kind = tgCallArg_GprMem; a.reg = piece->reg; break;
		case tgLoc_Xmm:   a.kind = tgCallArg_XmmMem; a.reg = piece->reg; break;
		case tgLoc_Stack: a.kind = tgCallArg_StackMem; a.stack_offset = piece->stack_offset; break;
		}
	}
	tg_array_add(out, a);
}

TG_INTERNAL void tg_add_ptr_arg(tgProc *p, tgCallArgArray *out, tgAbiFunc *abi, tgAbiArg const *arg, u32 ptr, i32 part) {
	tgAbiPiece const *piece;
	tgCallArg a;
	(void)p;
	TG_ASSERT(part < arg->piece_count);
	piece = &abi->pieces.data[arg->piece_index+part];
	memset(&a, 0, sizeof(a));
	a.type = tgType_I64;
	a.size = 8;
	a.vreg = ptr;
	if (piece->loc == tgLoc_Gpr) {
		a.kind = tgCallArg_Gpr;
		a.reg = piece->reg;
	} else {
		a.kind = tgCallArg_Stack;
		a.stack_offset = piece->stack_offset;
	}
	tg_array_add(out, a);
}

// The memory of an argument: its tyg_addr, or a scalar stored to a fresh local.
TG_INTERNAL tgMem tg_arg_mem(tgProc *p, u32 val, tgMem mem) {
	if (val != 0) return tg_value_to_mem(p, p->vregs.data[val], val);
	return mem;
}

// The variadic arguments of a C call on arm64: on macOS each one
// goes onto the stack after the fixed arguments, never into a register, a scalar in an 8
// byte slot and an aggregate as its bytes rounded up to 8 (aligned to 16 when its type is),
// or by pointer to a copy when it is larger than 16 bytes and not a float aggregate, like
// clang. On Linux each takes the next x or v register like a fixed argument, then an 8 byte
// stack slot; tyg_call passes aggregate varargs there as fixed parameters, so only scalars
// come here. Returns the stack size of the call.
TG_INTERNAL i32 tg_a64_varargs(tgProc *p, tgAbiFunc *abi, tgCallArgArray *call_args, tgArgValue const *args, isize arg_count, isize arg_index) {
	i32 stack = (i32)tg_align_up(abi->stack_size, 8);
	i32 ngrn = a64_is_apple() ? 8 : abi->gpr_count;
	i32 nsrn = a64_is_apple() ? 8 : abi->xmm_count;
	for (; arg_index < arg_count; arg_index++) {
		tgArgValue const *v = &args[arg_index];
		tgType st = tg_t_scalar(v->type);
		tgCallArg a;
		i32 *next;
		memset(&a, 0, sizeof(a));
		if (st == tgType_None || v->val == 0) {
			tyg_type const *t = v->type;
			tgType base = tgType_None;
			i64 members = 0;
			tgMem m;
			i64 off;
			if (!a64_is_apple()) {
				tg_fail(p, "aggregate c vararg");
				break;
			}
			if (t->size > 16 && !a64_hfa(t, &base, &members)) {
				// a pointer to a copy, which the callee may write
				tgMem copy = tg_mem(tgMem_Local, (u32)tg_add_local_raw(p, t->size, t->align), 0);
				tg_memcopy(p, copy, tg_arg_mem(p, v->val, v->mem), t->size);
				a.kind = tgCallArg_Stack;
				a.type = tgType_I64;
				a.size = 8;
				a.vreg = tg_lea_mem(p, copy);
				a.stack_offset = stack;
				stack += 8;
				tg_array_add(call_args, a);
				continue;
			}
			if (t->align > 8) stack = (i32)tg_align_up(stack, 16);
			m = tg_arg_mem(p, v->val, v->mem);
			for (off = 0; off < t->size; off += 8) {
				a.kind = tgCallArg_StackMem;
				a.type = tgType_None;
				a.size = (i32)tg_min((i64)8, t->size - off);
				a.mem = tg_mem_offset(m, off);
				a.stack_offset = stack + (i32)off;
				tg_array_add(call_args, a);
			}
			stack += (i32)tg_align_up(t->size, 8);
			continue;
		}
		a.kind = tgCallArg_Stack;
		a.type = st;
		a.size = 8;
		a.vreg = v->val;
		a.stack_offset = stack;
		if (tg_type_is_int(st)) {
			a.ext = v->type->is_signed ? tgExt_Sign : tgExt_Zero;
		}
		if (st == tgType_F32) {
			a.vreg = tg_convop(p, tgOp_FExt, tgType_F64, tgType_F32, a.vreg);
			a.type = tgType_F64;
		}
		next = tg_type_is_float(a.type) ? &nsrn : &ngrn;
		if (*next < 8) {
			a.kind = tg_type_is_float(a.type) ? tgCallArg_Xmm : tgCallArg_Gpr;
			a.reg = (u8)(*next)++;
			a.stack_offset = 0;
		} else {
			stack += 8;
		}
		tg_array_add(call_args, a);
	}
	return stack;
}

// Calls with arguments already checked against the parameter types. The first
// abi->params.count arguments are the fixed parameters, C varargs follow. `result` is
// where an aggregate result goes; a scalar result is returned.
TG_INTERNAL u32 tg_emit_call_internal(tgProc *p, tgAbiFunc *abi, tyg_type const *ret_type, u32 flags,
                                      i32 direct_sym, u32 target_vreg,
                                      tgArgValue const *args, isize arg_count, tgMem const *result) {
	tgCallArgArray call_args;
	tgCallRetArray call_rets;
	tgMem result_mem;
	tgType result_scalar = tgType_None;
	bool has_result = false;
	isize arg_index = 0;
	isize param_index;
	i32 stack_size;
	i32 sse_count;
	tgCall call;
	tgInstr ci;

	memset(&call_args, 0, sizeof(call_args));
	memset(&call_rets, 0, sizeof(call_rets));
	memset(&result_mem, 0, sizeof(result_mem));
	call_args.arena = tg_arena();
	call_rets.arena = tg_arena();
	tg_array_reserve(&call_args, arg_count+4);

	if (ret_type != NULL && ret_type->kind != tgT_Void) {
		has_result = true;
		result_scalar = tg_t_scalar(ret_type);
		if (result_scalar != tgType_None || result == NULL) {
			// a scalar result is stored to a local by the call and loaded after it
			result_mem = tg_mem(tgMem_Local, (u32)tg_add_local_raw(p, ret_type->size, ret_type->align), 0);
		} else {
			result_mem = *result;
		}
	}

	if (abi->has_sret) {
		tg_add_ptr_arg(p, &call_args, abi, &abi->sret, tg_lea_mem(p, result_mem), 0);
	}

	for (param_index = 0; param_index < abi->params.count; param_index++) {
		tgArgValue const *v = &args[arg_index++];
		tgAbiArg const *arg = &abi->params.data[param_index];
		i32 i;
		switch (arg->kind) {
		case tgArg_Ignore:
			break;
		case tgArg_Direct:
			for (i = 0; i < arg->piece_count; i++) {
				tg_add_piece_arg(p, &call_args, &abi->pieces.data[arg->piece_index+i], v->val, v->mem, v->type);
			}
			break;
		case tgArg_Indirect: {
			if (arg->copy_part > 0) {
				i32 l = tg_add_local_raw(p, arg->copy_part*arg->piece_count, arg->copy_part);
				tgMem m = tg_mem(tgMem_Local, (u32)l, 0);
				if (v->val != 0) tg_store(p, p->vregs.data[v->val], m, v->val);
				else tg_memcopy(p, m, v->mem, v->type->size);
				for (i = 0; i < arg->piece_count; i++) {
					tg_add_ptr_arg(p, &call_args, abi, arg, tg_lea_mem(p, tg_mem_offset(m, i*arg->copy_part)), i);
				}
				break;
			}
			{
				// C: the callee may write its parameter, so it gets a copy
				tgMem m = tg_mem(tgMem_Local, (u32)tg_add_local_raw(p, v->type->size, v->type->align), 0);
				if (v->val != 0) tg_store(p, p->vregs.data[v->val], m, v->val);
				else tg_memcopy(p, m, v->mem, v->type->size);
				tg_add_ptr_arg(p, &call_args, abi, arg, tg_lea_mem(p, m), 0);
			}
			break;
		}
		case tgArg_ByVal: {
			tgCallArg a;
			// an over-aligned argument needs rsp aligned for it at the call, see the prologue
			if (arg->byval_align > 16) p->call_align = tg_max(p->call_align, arg->byval_align);
			memset(&a, 0, sizeof(a));
			a.kind = tgCallArg_StackMem;
			a.mem = tg_arg_mem(p, v->val, v->mem);
			a.size = arg->byval_size;
			a.stack_offset = arg->stack_offset;
			tg_array_add(&call_args, a);
			break;
		}
		}
	}

	stack_size = abi->stack_size;
	sse_count = -1;
	if (abi->c_vararg && tg_is_win64()) {
		stack_size = tg_win64_varargs(p, abi, &call_args, args, arg_index, arg_count);
	} else if (abi->c_vararg && tg_is_arm64()) {
		stack_size = tg_a64_varargs(p, abi, &call_args, args, arg_count, arg_index);
	} else if (abi->c_vararg) {
		i32 gpr = abi->gpr_count;
		i32 xmm = abi->xmm_count;
		i32 stack = abi->stack_size;
		for (; arg_index < arg_count; arg_index++) {
			tgArgValue const *v = &args[arg_index];
			tgType st = tg_t_scalar(v->type);
			tgCallArg a;
			if (st == tgType_None || v->val == 0) {
				tg_fail(p, "aggregate c vararg");
				break;
			}
			memset(&a, 0, sizeof(a));
			a.type = st;
			a.size = tg_type_size(st);
			a.vreg = v->val;
			if (tg_type_is_int(st) && tg_type_size(st) < 4) {
				a.ext = v->type->is_signed ? tgExt_Sign : tgExt_Zero;
			}
			if (tg_type_is_float(st)) {
				if (st == tgType_F32) {
					a.vreg = tg_convop(p, tgOp_FExt, tgType_F64, tgType_F32, a.vreg);
					a.type = tgType_F64;
					a.size = 8;
				}
				if (xmm < 8) {
					a.kind = tgCallArg_Xmm;
					a.reg = (u8)xmm++;
				} else {
					a.kind = tgCallArg_Stack;
					a.stack_offset = stack;
					stack += 8;
				}
			} else {
				if (gpr < 6) {
					a.kind = tgCallArg_Gpr;
					a.reg = tg_build_sysv_int_regs[gpr++];
				} else {
					a.kind = tgCallArg_Stack;
					a.stack_offset = stack;
					stack += 8;
				}
			}
			tg_array_add(&call_args, a);
		}
		stack_size = stack;
		sse_count = xmm;
	}

	if (abi->ret.kind == tgArg_Direct) {
		i32 i;
		for (i = 0; i < abi->ret.piece_count; i++) {
			tgAbiPiece const *piece = &abi->pieces.data[abi->ret.piece_index+i];
			tgCallRet r;
			memset(&r, 0, sizeof(r));
			r.loc = piece->loc;
			r.reg = piece->reg;
			r.type = piece->type;
			r.size = piece->size;
			r.dst = tg_mem_offset(result_mem, piece->src_offset);
			tg_array_add(&call_rets, r);
		}
	}

	memset(&call, 0, sizeof(call));
	call.target_sym = direct_sym;
	call.target_vreg = direct_sym < 0 ? target_vreg : 0;
	tg_slice_from_array(tgCallArgSlice, &call_args, &call.args);
	tg_slice_from_array(tgCallRetSlice, &call_rets, &call.rets);
	call.stack_size = stack_size;
	call.sse_count = sse_count;
	call.returns_twice = (flags & TYG_SIG_RETURNS_TWICE) != 0;
	tg_array_add(&p->calls, call);

	ci = tg_instr(tgOp_Call);
	ci.imm = p->calls.count-1;
	tg_emit(p, ci);

	if (flags & TYG_SIG_NORETURN) {
		tg_unreachable(p);
	}

	if (!has_result || result_scalar == tgType_None) return 0;
	return tg_load(p, result_scalar, result_mem);
}

////////////////////////////////////////////////////////////////
// Returns and parameters
////////////////////////////////////////////////////////////////

// Returns the value `val`, or with val 0 the value in memory at `direct_result`, or nothing
// when has_value is false.
TG_INTERNAL void tg_emit_ret(tgProc *p, bool has_value, u32 val, tgMem direct_result) {
	tgAbiFunc *abi = p->abi;
	tgCallArgArray args;
	tgCall c;
	tgInstr i;
	memset(&args, 0, sizeof(args));
	args.arena = tg_arena();
	if (abi->has_sret && !tg_is_arm64()) {
		// x86-64 hands the sret pointer back in rax
		tgCallArg a;
		memset(&a, 0, sizeof(a));
		a.kind = tgCallArg_Gpr;
		a.type = tgType_I64;
		a.size = 8;
		a.reg = RAX;
		a.vreg = tg_load(p, tgType_I64, tg_mem(tgMem_Local, (u32)p->sret_local, 0));
		tg_array_add(&args, a);
	} else if (abi->ret.kind == tgArg_Direct && has_value) {
		i32 k;
		for (k = 0; k < abi->ret.piece_count; k++) {
			tgAbiPiece const *piece = &abi->pieces.data[abi->ret.piece_index+k];
			if (val != 0) {
				// the value goes straight into its register when it fills the piece
				tg_add_piece_arg(p, &args, piece, val, direct_result, p->sig->ret);
				continue;
			}
			{
				tgCallArg a;
				memset(&a, 0, sizeof(a));
				a.kind = piece->loc == tgLoc_Gpr ? tgCallArg_GprMem : piece->loc == tgLoc_X87 ? tgCallArg_X87Mem : tgCallArg_XmmMem;
				a.type = piece->type;
				a.size = piece->size;
				a.reg = piece->reg;
				a.ext = piece->ext;
				a.mem = tg_mem_offset(direct_result, piece->src_offset);
				tg_array_add(&args, a);
			}
		}
	}
	memset(&c, 0, sizeof(c));
	c.target_sym = -1;
	tg_slice_from_array(tgCallArgSlice, &args, &c.args);
	c.sse_count = -1;
	tg_array_add(&p->calls, c);
	i = tg_instr(tgOp_Ret);
	i.imm = p->calls.count-1;
	tg_emit(p, i);
}

TG_INTERNAL void tg_param_in(tgProc *p, tgAbiPiece const *piece, tgMem dst) {
	tgParamIn in;
	memset(&in, 0, sizeof(in));
	in.loc = piece->loc;
	in.reg = piece->reg;
	in.type = piece->type;
	in.size = piece->size;
	in.stack_offset = piece->stack_offset;
	in.dst = dst;
	tg_array_add(&p->params_in, in);
}

TG_INTERNAL i32 tg_param_ptr_local(tgProc *p, tgAbiFunc *abi, tgAbiArg const *arg, i32 part) {
	i32 l = tg_add_local_raw(p, 8, 8);
	tg_param_in(p, &abi->pieces.data[arg->piece_index+part], tg_mem(tgMem_Local, (u32)l, 0));
	return l;
}

// SysV x86-64 C varargs: the prologue spills the argument registers the fixed parameters
// leave unused into the register save area, where va_arg finds them.
// gp_offset 0..48: rdi, rsi, rdx, rcx, r8, r9; fp_offset 48..176: xmm0-xmm7, 16 bytes each.
enum {
	TG_VA_GP_END   = 48,
	TG_VA_SAVE_SIZE = 176,
	TG_VA_LIST_SIZE = 24, // gp_offset u32, fp_offset u32, overflow_arg_area, reg_save_area
};

// AAPCS64 (arm64 Linux) C varargs: the prologue spills x0-x7 and q0-q7 that the fixed
// parameters leave unused into a register save area, x registers in its first 64 bytes,
// q registers in the 128 after. va_list's gr_top and vr_top point at the end of each part.
enum {
	TG_A64_VA_GR_SIZE   = 64,
	TG_A64_VA_SAVE_SIZE = 192,
	TG_A64_VA_LIST_SIZE = 32, // stack, gr_top, vr_top, gr_offs i32, vr_offs i32
};

TG_INTERNAL void tg_a64_va_save_area(tgProc *p, tgAbiFunc *abi) {
	i32 i;
	p->va_save_local = tg_add_local_raw(p, TG_A64_VA_SAVE_SIZE, 16);
	for (i = abi->gpr_count; i < 8; i++) {
		tgParamIn in;
		memset(&in, 0, sizeof(in));
		in.loc = tgLoc_Gpr;
		in.reg = (u8)i;
		in.type = tgType_I64;
		in.size = 8;
		in.dst = tg_mem(tgMem_Local, (u32)p->va_save_local, 8*i);
		tg_array_add(&p->params_in, in);
	}
	for (i = abi->xmm_count; i < 8; i++) {
		tgParamIn in;
		memset(&in, 0, sizeof(in));
		in.loc = tgLoc_Xmm;
		in.reg = (u8)i;
		in.type = tgType_V128;
		in.size = 16;
		in.dst = tg_mem(tgMem_Local, (u32)p->va_save_local, TG_A64_VA_GR_SIZE + 16*i);
		tg_array_add(&p->params_in, in);
	}
}

TG_INTERNAL void tg_va_save_area(tgProc *p, tgAbiFunc *abi) {
	i32 i;
	if (tg_is_arm64()) {
		// macOS needs no save area: every vararg is on the stack
		if (!tg_is_darwin()) tg_a64_va_save_area(p, abi);
		return;
	}
	p->va_save_local = tg_add_local_raw(p, TG_VA_SAVE_SIZE, 16);
	for (i = abi->gpr_count; i < 6; i++) {
		tgParamIn in;
		memset(&in, 0, sizeof(in));
		in.loc = tgLoc_Gpr;
		in.reg = tg_build_sysv_int_regs[i];
		in.type = tgType_I64;
		in.size = 8;
		in.dst = tg_mem(tgMem_Local, (u32)p->va_save_local, 8*i);
		tg_array_add(&p->params_in, in);
	}
	for (i = abi->xmm_count; i < 8; i++) {
		tgParamIn in;
		memset(&in, 0, sizeof(in));
		in.loc = tgLoc_Xmm;
		in.reg = (u8)i;
		in.type = tgType_V128;
		in.size = 16;
		in.dst = tg_mem(tgMem_Local, (u32)p->va_save_local, TG_VA_GP_END + 16*i);
		tg_array_add(&p->params_in, in);
	}
}

// Receives the parameters: each one lands in a local, or for ByVal in
// the incoming stack area, and p->param_vals holds its value (a scalar) or address.
TG_INTERNAL void tg_begin_params(tgProc *p) {
	tyg_sig const *sig = p->sig;
	tgAbiFunc *abi = p->abi;
	i32 param_index;

	if (abi->has_sret) {
		p->sret_local = tg_param_ptr_local(p, abi, &abi->sret, 0);
	}

	for (param_index = 0; param_index < sig->count; param_index++) {
		tyg_type const *type = sig->params[param_index];
		tgAbiArg const *arg = &abi->params.data[param_index];
		tgMem m;
		i32 i;
		memset(&m, 0, sizeof(m));
		switch (arg->kind) {
		case tgArg_Ignore: {
			m = tg_mem(tgMem_Local, (u32)tg_add_local_raw(p, type->size, type->align), 0);
			tg_memzero(p, m, type->size);
			break;
		}
		case tgArg_Direct: {
			i64 size = type->size;
			for (i = 0; i < arg->piece_count; i++) {
				tgAbiPiece const *piece = &abi->pieces.data[arg->piece_index+i];
				size = tg_max(size, (i64)(piece->src_offset + piece->size));
			}
			m = tg_mem(tgMem_Local, (u32)tg_add_local_raw(p, size, tg_max(type->align, (i64)8)), 0);
			for (i = 0; i < arg->piece_count; i++) {
				tgAbiPiece const *piece = &abi->pieces.data[arg->piece_index+i];
				tg_param_in(p, piece, tg_mem_offset(m, piece->src_offset));
			}
			break;
		}
		case tgArg_Indirect: {
			i64 sz = type->size;
			if (arg->piece_count > 1) {
				// a value split into parts, each behind its own pointer
				m = tg_mem(tgMem_Local, (u32)tg_add_local_raw(p, type->size, type->align), 0);
				for (i = 0; i < arg->piece_count; i++) {
					i32 pl = tg_param_ptr_local(p, abi, arg, i);
					i64 off = (i64)i*arg->copy_part;
					tg_memcopy(p, tg_mem_offset(m, off), tg_mem(tgMem_Reg, tg_load(p, tgType_I64, tg_mem(tgMem_Local, (u32)pl, 0)), 0), tg_min((i64)arg->copy_part, sz - off));
				}
				break;
			}
			{
				// the caller passes a copy, which the procedure may write
				i32 l = tg_param_ptr_local(p, abi, arg, 0);
				m = tg_mem(tgMem_Reg, tg_load(p, tgType_I64, tg_mem(tgMem_Local, (u32)l, 0)), 0);
			}
			break;
		}
		case tgArg_ByVal: {
			// copy into the frame so the debugger and everything else can treat it as a local
			m = tg_mem(tgMem_Local, (u32)tg_add_local_raw(p, type->size, type->align), 0);
			tg_memcopy(p, m, tg_mem(tgMem_Incoming, 0, arg->stack_offset), type->size);
			break;
		}
		}
		{
			tgType st = tg_t_scalar(type);
			u32 v = st != tgType_None ? tg_load(p, st, m) : tg_lea_mem(p, m);
			tg_array_add(&p->param_vals, v);
		}
	}

	if ((sig->flags & TYG_SIG_VARIADIC) && tg_is_win64()) {
		tg_win64_va_spill(p, abi);
	} else if (sig->flags & TYG_SIG_VARIADIC) {
		tg_va_save_area(p, abi);
	}
}

// Finishes the body.
TG_INTERNAL void tg_end_proc(tgProc *p) {
	isize bi;
	if (!tg_curr_terminated(p)) {
		// C lets a non-void function reach its end as long as the caller does not use the
		// value, so every procedure returns here, not only one without results.
		tgMem none;
		memset(&none, 0, sizeof(none));
		tg_emit_ret(p, false, 0, none);
	}
	// every placed block must end in a terminator
	for (bi = 0; bi < p->order.count; bi++) {
		tgBlock *b = p->order.data[bi];
		if (!tg_block_terminated(b)) {
			tgInstr i = tg_instr(tgOp_Unreachable);
			tg_array_add(&b->instrs, i);
		}
	}
	// and jump only to placed blocks
	for (bi = 0; bi < p->order.count; bi++) {
		tgBlock *b = p->order.data[bi];
		tgInstr const *last = &b->instrs.data[b->instrs.count-1];
		if (last->op == tgOp_Jump && !p->blocks.data[last->imm]->placed) {
			tg_fail(p, "jump to a block that is never started");
		}
		if (last->op == tgOp_Branch && (!p->blocks.data[last->imm]->placed || !p->blocks.data[last->c]->placed)) {
			tg_fail(p, "branch to a block that is never started");
		}
	}
}

////////////////////////////////////////////////////////////////
// Conversions between the public API and the IR
////////////////////////////////////////////////////////////////

TG_INTERNAL tgType tg_scalar_of(tyg_scalar s) {
	switch (s) {
	case TYG_I8:  return tgType_I8;
	case TYG_I16: return tgType_I16;
	case TYG_I32: return tgType_I32;
	case TYG_I64: return tgType_I64;
	case TYG_F32: return tgType_F32;
	case TYG_F64: return tgType_F64;
	case TYG_VOID:
	case TYG_F80: break;
	}
	return tgType_None;
}

TG_INTERNAL tyg_scalar tg_public_scalar(tgType t) {
	switch (t) {
	case tgType_I8:  return TYG_I8;
	case tgType_I16: return TYG_I16;
	case tgType_I32: return TYG_I32;
	case tgType_I64: return TYG_I64;
	case tgType_F32: return TYG_F32;
	case tgType_F64: return TYG_F64;
	}
	return TYG_VOID;
}

// Whether the builder may go on: the procedure exists and has not failed. Every public
// builder call starts here, which also makes the procedure's arena and target current.
TG_INTERNAL bool tg_b_ok(tgProc *p) {
	if (p == NULL) return false;
	tg_arena_cur = &p->m->proc_arena;
	tg_cur_target = p->m->target;
	return p->fail_reason == NULL;
}

// A fresh value for a call made after a failure, so the caller can go on.
TG_INTERNAL u32 tg_dummy(tgProc *p, tgType t) {
	if (p == NULL) return 0;
	return tg_new_vreg(p, t != tgType_None ? t : tgType_I64);
}

// The type of a value the caller passed, or None after recording the failure.
TG_INTERNAL tgType tg_val_check(tgProc *p, tyg_val v) {
	if (v == 0 || (isize)v >= p->vregs.count) {
		tg_fail(p, "invalid value");
		return tgType_None;
	}
	return p->vregs.data[v];
}

TG_INTERNAL bool tg_ptr_check(tgProc *p, tyg_val v) {
	tgType t = tg_val_check(p, v);
	if (t == tgType_None) return false;
	if (t != tgType_I64) {
		tg_fail(p, "a pointer value is not TYG_I64");
		return false;
	}
	return true;
}

// An address x + c, or x + (y + c), goes into the access as [x + c] or [(x + y) + c]: the
// offset costs nothing there, and the neighbors of an element (a[i - 1], a[i + 1]) share
// one x + y, which tg_cse_proc makes once per block.
TG_INTERNAL void tg_fold_address(tgProc *p, u32 *base, i64 *off) {
	tgVregInfo const *vi = &p->vinfo.data[*base];
	if (vi->kbase != 0) {
		i64 o = *off + vi->kadd;
		if (o >= -(1ll << 30) && o < (1ll << 30) && p->vregs.data[vi->kbase] == tgType_I64) {
			*base = vi->kbase;
			*off = o;
		}
		return;
	}
	if (vi->suma == 0) return;
	u32 x = vi->suma, y = vi->sumb;
	if (p->vinfo.data[y].kbase == 0) {
		u32 t = x;
		x = y;
		y = t;
	}
	tgVregInfo const *yi = &p->vinfo.data[y];
	if (yi->kbase == 0 || p->vregs.data[yi->kbase] != tgType_I64) return;
	i64 o = *off + yi->kadd;
	if (o < -(1ll << 30) || o >= (1ll << 30)) return;
	*base = tg_binop(p, tgOp_Add, tgType_I64, x, yi->kbase);
	*off = o;
}

TG_INTERNAL bool tg_addr_to_mem(tgProc *p, tyg_addr a, tgMem *out) {
	tgMem m;
	if (a.offset < INT32_MIN || a.offset > INT32_MAX) {
		tg_fail(p, "address offset out of range");
		return false;
	}
	switch (a.kind) {
	case TYG_ADDR_VAL: {
		if (!tg_ptr_check(p, (tyg_val)a.base)) return false;
		u32 base = (u32)a.base;
		i64 off = a.offset;
		tg_fold_address(p, &base, &off);
		m = tg_mem(tgMem_Reg, base, (i32)off);
		break;
	}
	case TYG_ADDR_LOCAL:
		if (a.base < 0 || a.base >= p->locals.count) {
			tg_fail(p, "invalid local");
			return false;
		}
		m = tg_mem(tgMem_Local, (u32)a.base, (i32)a.offset);
		break;
	case TYG_ADDR_SYM:
		if (a.base < 0 || a.base >= p->m->symbols.count) {
			tg_fail(p, "invalid symbol");
			return false;
		}
		tg_alias_take_tls(p->m, (i32)a.base);
		if (p->m->symbols.data[a.base].flags & tgSymbolFlag_TLS) {
			m = tg_tls_mem(p, a.base);
			m = tg_mem_offset(m, a.offset);
		} else {
			m = tg_mem(tgMem_Sym, (u32)a.base, (i32)a.offset);
		}
		break;
	default:
		tg_fail(p, "invalid address kind");
		return false;
	}
	if (a.align > 0) {
		if (!tg_is_pow2(a.align)) {
			tg_fail(p, "address alignment is not a power of two");
			return false;
		}
		// below 256 bytes the known alignment fits, above it any type is aligned
		if (a.align < 256) m.align = (u8)a.align;
	}
	*out = m;
	return true;
}

TG_INTERNAL bool tg_block_check(tgProc *p, tyg_block b) {
	if (b < 0 || b >= p->blocks.count) {
		tg_fail(p, "invalid block");
		return false;
	}
	return true;
}


// A condition as the IR's branches and selects take it: a byte that is 0 or not.
TG_INTERNAL u32 tg_cond_bool(tgProc *p, u32 cond) {
	tgType t = p->vregs.data[cond];
	if (t == tgType_I8) return cond;
	if (tg_type_is_float(t)) return tg_cmp(p, tgCond_FNE, t, cond, tg_fconst(p, t, 0.0));
	return tg_cmp(p, tgCond_NE, t, cond, tg_iconst(p, t, 0));
}

// The frame or symbol memory that the pointer v is the address of, when v comes from a
// tg_lea in the current or the entry block, like the parameters' addresses.
TG_INTERNAL bool tg_val_addr_mem(tgProc *p, u32 v, tgMem *out) {
	tgBlock *look[2];
	i32 k;
	look[0] = p->curr;
	look[1] = p->blocks.count > 0 ? p->blocks.data[0] : NULL;
	for (k = 0; k < 2; k++) {
		tgBlock *b = look[k];
		isize i;
		if (b == NULL || (k == 1 && b == look[0])) continue;
		for (i = b->instrs.count-1; i >= 0; i--) {
			tgInstr const *in = &b->instrs.data[i];
			if (in->dst == v && in->op == tgOp_Lea && (in->mem.kind == tgMem_Local || in->mem.kind == tgMem_Sym)) {
				*out = in->mem;
				return true;
			}
		}
	}
	return false;
}

// The local an incoming parameter's pointer is stored in, when v is that pointer loaded in the
// entry block, like the address of an aggregate the caller passes by pointer to a copy.
TG_INTERNAL bool tg_val_param_ptr_local(tgProc *p, u32 v, i32 *out) {
	tgBlock *b = p->blocks.count > 0 ? p->blocks.data[0] : NULL;
	if (b == NULL) return false;
	for (isize i = 0; i < b->instrs.count; i++) {
		tgInstr const *in = &b->instrs.data[i];
		if (in->dst != v || in->op != tgOp_Load || in->mem.kind != tgMem_Local || in->mem.offset != 0) continue;
		for (isize k = 0; k < p->params_in.count; k++) {
			tgMem const *d = &p->params_in.data[k].dst;
			if (d->kind == tgMem_Local && d->base == in->mem.base && d->offset == 0 && p->params_in.data[k].size == 8) {
				*out = (i32)in->mem.base;
				return true;
			}
		}
		return false;
	}
	return false;
}

////////////////////////////////////////////////////////////////
// Procedures
////////////////////////////////////////////////////////////////

// Starts building a procedure.
tyg_proc *tyg_proc_begin(tyg_module *m, tyg_sym sym, const tyg_sig *sig) {
	tgProc *p;
	char const *reason = NULL;
	tgBlock *entry;
	i32 i;
	if (m == NULL) return NULL;
	if (m->proc != NULL) {
		tg_set_error(m, "tyg_proc_begin: a procedure is already being built");
		return NULL;
	}
	if (sig == NULL || sym < 0 || sym >= m->symbols.count) {
		tg_set_error(m, "tyg_proc_begin: invalid symbol or signature");
		return NULL;
	}

	tg_arena_cur = &m->proc_arena;
	tg_arena_reset(&m->proc_arena);
	tg_cur_target = m->target;

	p = tg_alloc_item(tgProc);
	p->m = m;
	p->sig = sig;
	p->sym = sym;
	p->name = m->symbols.data[sym].name;
	p->vregs.arena = tg_arena();
	p->vinfo.arena = tg_arena();
	p->blocks.arena = tg_arena();
	p->order.arena = tg_arena();
	p->locals.arena = tg_arena();
	p->calls.arena = tg_arena();
	p->params_in.arena = tg_arena();
	p->param_vals.arena = tg_arena();
	p->debug_vars.arena = tg_arena();
	p->debug_scope_parent.arena = tg_arena();
	p->inline_sites.arena = tg_arena();
	tg_array_reserve(&p->vregs, 256);
	tg_array_add(&p->vregs, tgType_None); // vreg 0 is invalid
	{
		tgVregInfo none = {0, ~0ull, false, 0, 0, 0, 0};
		tg_array_add(&p->vinfo, none);
	}
	tg_array_add(&p->debug_scope_parent, -1);
	p->file_id = -1;
	p->sret_local = -1;
	p->va_save_local = -1;
	m->proc = p;

	entry = tg_new_block(p);
	tg_start_block(p, entry, true);

	if (m->symbols.data[sym].section != tgSection_Undef) {
		tg_fail(p, "the symbol is already defined");
	}
	p->abi = tg_sig_abi((tyg_sig *)sig, &reason);
	if (p->abi == NULL) {
		tg_fail(p, reason ? reason : "abi");
	}
	if (p->fail_reason == NULL) {
		tg_begin_params(p);
	} else {
		for (i = 0; i < sig->count; i++) {
			tgType st = tg_t_scalar(sig->params[i]);
			tg_array_add(&p->param_vals, tg_dummy(p, st));
		}
	}
	return p;
}

// Discards the procedure being built.
TG_INTERNAL void tg_proc_discard(tgModule *m) {
	tg_arena_reset(&m->proc_arena);
	tg_arena_cur = NULL;
	m->proc = NULL;
}

void tyg_proc_abort(tyg_proc *p) {
	if (p == NULL) return;
	tg_proc_discard(p->m);
}

int tyg_proc_end(tyg_proc *p) {
	tgModule *m;
	tgLowerOut out;
	if (p == NULL) return 1;
	m = p->m;
	if (tg_b_ok(p)) {
		tg_end_proc(p);
	}
	if (p->fail_reason != NULL) {
		tg_set_error(m, "%.*s: %s", (int)p->name.len, p->name.text, p->fail_reason);
		tg_proc_discard(m);
		return 1;
	}

	// the procedure counts as defined from here on, and calls to itself see that
	m->symbols.data[p->sym].section = tgSection_Text;
	m->symbols.data[p->sym].flags |= tgSymbolFlag_Func;

	// the procedure's arena holds the code and its records until they are appended
	memset(&out, 0, sizeof(out));
	out.text.arena = tg_arena();
	out.relocs.arena = tg_arena();
	out.lines.arena = tg_arena();
	out.pending.arena = tg_arena();
	tg_fold_const_locals(p);
	tg_forward_mem_copies(p);
	tg_reuse_loads(p);
	tg_forward_copies(p);
	tg_pair_loads(p);
	tg_cleanup_proc(p);
	if (tg_is_arm64()) {
		a64_lower_proc(p, &out);
	} else {
		x64_lower_proc(p, &out);
	}
	out.section = p->code_section != 0 ? p->code_section : tgSection_Text;
	tg_append_lowered(m, &out);
	tg_proc_discard(m);
	return 0;
}

int tyg_proc_section(tyg_proc *p, const char *section) {
	if (p == NULL) return 1;
	tgModule *m = p->m;
	if (m->target.os != TYG_OS_LINUX) {
		tg_set_error(m, "tyg_proc_section: named sections are not supported on this target yet");
		return 1;
	}
	if (section == NULL || section[0] == 0) {
		tg_set_error(m, "tyg_proc_section: no section name");
		return 1;
	}
	isize i = tg_named_section_get(m, section, TYG_SECF_EXEC, "tyg_proc_section");
	if (i < 0) return 1;
	p->code_section = (u8)(tgSection_COUNT + i);
	return 0;
}

tyg_local tyg_local_new(tyg_proc *p, int64_t size, int64_t align) {
	if (!tg_b_ok(p)) return p ? (tyg_local)tg_add_local_raw(p, 1, 1) : 0;
	if (size < 0 || (align != 0 && !tg_is_pow2(align))) {
		tg_fail(p, "tyg_local_new: bad size or alignment");
		return (tyg_local)tg_add_local_raw(p, 1, 1);
	}
	return (tyg_local)tg_add_local_raw(p, size, align);
}

tyg_val tyg_param(tyg_proc *p, int index) {
	if (p == NULL) return 0;
	tg_b_ok(p);
	if (index < 0 || index >= p->param_vals.count) {
		tg_fail(p, "tyg_param: no such parameter");
		return tg_dummy(p, tgType_I64);
	}
	return p->param_vals.data[index];
}

tyg_block tyg_block_new(tyg_proc *p) {
	if (p == NULL) return 0;
	tg_b_ok(p);
	return tg_new_block(p)->index;
}

void tyg_block_set(tyg_proc *p, tyg_block b) {
	tgBlock *blk;
	if (!tg_b_ok(p)) return;
	if (!tg_block_check(p, b)) return;
	blk = p->blocks.data[b];
	if (blk == p->curr) return;
	if (blk->placed) {
		// back to a block started before: it goes on where it stopped
		if (tg_block_terminated(blk)) {
			tg_fail(p, "tyg_block_set: the block is already terminated");
			return;
		}
		p->curr = blk;
		return;
	}
	tg_start_block(p, blk, true);
}

tyg_block tyg_block_current(tyg_proc *p) {
	if (p == NULL || p->curr == NULL) return 0;
	return p->curr->index;
}

int tyg_block_terminated(tyg_proc *p) {
	if (p == NULL) return 1;
	return tg_curr_terminated(p) ? 1 : 0;
}

////////////////////////////////////////////////////////////////
// Values
////////////////////////////////////////////////////////////////

tyg_val tyg_iconst(tyg_proc *p, tyg_scalar type, int64_t value) {
	tgType t = tg_scalar_of(type);
	if (!tg_b_ok(p)) return tg_dummy(p, t);
	if (!tg_type_is_int(t)) {
		tg_fail(p, "tyg_iconst: not an integer type");
		return tg_dummy(p, t);
	}
	return tg_iconst(p, t, tg_fold_norm(t, value));
}

tyg_val tyg_fconst(tyg_proc *p, tyg_scalar type, double value) {
	tgType t = tg_scalar_of(type);
	if (!tg_b_ok(p)) return tg_dummy(p, t);
	if (!tg_type_is_float(t)) {
		tg_fail(p, "tyg_fconst: not a float type");
		return tg_dummy(p, t);
	}
	return tg_fconst(p, t, value);
}

tyg_val tyg_addr_of(tyg_proc *p, tyg_addr a) {
	tgMem m;
	if (!tg_b_ok(p)) return tg_dummy(p, tgType_I64);
	if (!tg_addr_to_mem(p, a, &m)) return tg_dummy(p, tgType_I64);
	return tg_lea_mem(p, m);
}

tyg_scalar tyg_val_type(tyg_proc *p, tyg_val v) {
	if (p == NULL || v == 0 || (isize)v >= p->vregs.count) return TYG_VOID;
	return tg_public_scalar(p->vregs.data[v]);
}

////////////////////////////////////////////////////////////////
// Memory
////////////////////////////////////////////////////////////////

tyg_val tyg_load(tyg_proc *p, tyg_scalar type, tyg_addr a, unsigned flags) {
	tgType t = tg_scalar_of(type);
	tgMem m;
	if (!tg_b_ok(p)) return tg_dummy(p, t);
	if (t == tgType_None) {
		tg_fail(p, "tyg_load: void type");
		return tg_dummy(p, t);
	}
	if (!tg_addr_to_mem(p, a, &m)) return tg_dummy(p, t);
	if (flags & TYG_MEM_VOLATILE) {
		tgInstr i = tg_instr_t(tgOp_Load, t);
		i.dst = tg_new_vreg(p, t);
		i.mem = m;
		i.flags = tgInstrFlag_Volatile;
		tg_emit(p, i);
		return i.dst;
	}
	return tg_load(p, t, m);
}

void tyg_store(tyg_proc *p, tyg_scalar type, tyg_addr a, tyg_val v, unsigned flags) {
	tgType t = tg_scalar_of(type);
	tgMem m;
	tgInstr i;
	if (!tg_b_ok(p)) return;
	if (tg_val_check(p, v) == tgType_None) return;
	if (t == tgType_None || p->vregs.data[v] != t) {
		tg_fail(p, "tyg_store: the value's type is not the store's");
		return;
	}
	if (!tg_addr_to_mem(p, a, &m)) return;
	i = tg_instr_t(tgOp_Store, t);
	i.a = v;
	i.mem = m;
	if (flags & TYG_MEM_VOLATILE) i.flags = tgInstrFlag_Volatile;
	tg_emit(p, i);
}

void tyg_copy(tyg_proc *p, tyg_addr dst, tyg_addr src, int64_t size) {
	tgMem d, s;
	if (!tg_b_ok(p)) return;
	if (size < 0) { tg_fail(p, "tyg_copy: negative size"); return; }
	if (!tg_addr_to_mem(p, dst, &d) || !tg_addr_to_mem(p, src, &s)) return;
	tg_memcopy(p, d, s, size);
}

void tyg_move(tyg_proc *p, tyg_addr dst, tyg_addr src, int64_t size) {
	tgMem d, s;
	if (!tg_b_ok(p)) return;
	if (size < 0) { tg_fail(p, "tyg_move: negative size"); return; }
	if (!tg_addr_to_mem(p, dst, &d) || !tg_addr_to_mem(p, src, &s)) return;
	tg_memmove(p, d, s, size);
}

void tyg_zero(tyg_proc *p, tyg_addr dst, int64_t size) {
	tgMem d;
	if (!tg_b_ok(p)) return;
	if (size < 0) { tg_fail(p, "tyg_zero: negative size"); return; }
	if (!tg_addr_to_mem(p, dst, &d)) return;
	tg_memzero(p, d, size);
}

// A size for the dynamic memory operations: a 64-bit integer.
TG_INTERNAL u32 tg_size_i64(tgProc *p, u32 v) {
	tgType t = tg_val_check(p, v);
	if (t == tgType_I64 || t == tgType_None) return v;
	if (!tg_type_is_int(t)) {
		tg_fail(p, "a size is not an integer");
		return v;
	}
	return tg_convop(p, tgOp_Zext, tgType_I64, t, v);
}

TG_INTERNAL void tg_mem_dyn(tgProc *p, tgOp op, u32 dst, u32 src, u32 size) {
	tgInstr i;
	if (!tg_ptr_check(p, dst) || !tg_ptr_check(p, src)) return;
	size = tg_size_i64(p, size);
	if (p->fail_reason) return;
	i = tg_instr(op);
	i.a = dst;
	i.b = src;
	i.c = size;
	tg_emit(p, i);
}

void tyg_copy_dyn(tyg_proc *p, tyg_val dst, tyg_val src, tyg_val size) {
	if (!tg_b_ok(p)) return;
	tg_mem_dyn(p, tgOp_MemCopyDyn, dst, src, size);
}

void tyg_move_dyn(tyg_proc *p, tyg_val dst, tyg_val src, tyg_val size) {
	if (!tg_b_ok(p)) return;
	tg_mem_dyn(p, tgOp_MemMoveDyn, dst, src, size);
}

void tyg_set_dyn(tyg_proc *p, tyg_val dst, tyg_val byte, tyg_val size) {
	tgType bt;
	tgInstr i;
	if (!tg_b_ok(p)) return;
	if (!tg_ptr_check(p, dst)) return;
	bt = tg_val_check(p, byte);
	if (bt == tgType_None) return;
	if (!tg_type_is_int(bt)) {
		tg_fail(p, "tyg_set_dyn: the byte is not an integer");
		return;
	}
	if (bt != tgType_I8) byte = tg_convop(p, tgOp_Trunc, tgType_I8, bt, byte);
	size = tg_size_i64(p, size);
	if (p->fail_reason) return;
	i = tg_instr(tgOp_MemSetDyn);
	i.a = dst;
	i.b = byte;
	i.c = size;
	tg_emit(p, i);
}

tyg_val tyg_alloca(tyg_proc *p, tyg_val size, int64_t align) {
	tgInstr i;
	if (!tg_b_ok(p)) return tg_dummy(p, tgType_I64);
	if (align < 0 || (align != 0 && !tg_is_pow2(align)) || align > 4096) {
		tg_fail(p, "tyg_alloca: bad alignment");
		return tg_dummy(p, tgType_I64);
	}
	size = tg_size_i64(p, size);
	if (p->fail_reason) return tg_dummy(p, tgType_I64);
	i = tg_instr_t(tgOp_Alloca, tgType_I64);
	i.a = size;
	i.imm = tg_max(align, (i64)16);
	i.dst = tg_new_vreg(p, tgType_I64);
	tg_emit(p, i);
	return i.dst;
}

////////////////////////////////////////////////////////////////
// Arithmetic
////////////////////////////////////////////////////////////////

tyg_val tyg_bin(tyg_proc *p, tyg_binop op, tyg_val a, tyg_val b) {
	tgType ta, tb;
	tgOp iop = tgOp_Nop;
	bool is_float = false;
	if (!tg_b_ok(p)) return tg_dummy(p, p && a && (isize)a < p->vregs.count ? p->vregs.data[a] : tgType_I64);
	ta = tg_val_check(p, a);
	tb = tg_val_check(p, b);
	if (ta == tgType_None || tb == tgType_None) return tg_dummy(p, ta);
	switch (op) {
	case TYG_ADD:  iop = tgOp_Add;  break;
	case TYG_SUB:  iop = tgOp_Sub;  break;
	case TYG_MUL:  iop = tgOp_Mul;  break;
	case TYG_SDIV: iop = tgOp_SDiv; break;
	case TYG_UDIV: iop = tgOp_UDiv; break;
	case TYG_SREM: iop = tgOp_SRem; break;
	case TYG_UREM: iop = tgOp_URem; break;
	case TYG_AND:  iop = tgOp_And;  break;
	case TYG_OR:   iop = tgOp_Or;   break;
	case TYG_XOR:  iop = tgOp_Xor;  break;
	case TYG_SHL:  iop = tgOp_Shl;  break;
	case TYG_LSHR: iop = tgOp_LShr; break;
	case TYG_ASHR: iop = tgOp_AShr; break;
	case TYG_FADD: iop = tgOp_FAdd; is_float = true; break;
	case TYG_FSUB: iop = tgOp_FSub; is_float = true; break;
	case TYG_FMUL: iop = tgOp_FMul; is_float = true; break;
	case TYG_FDIV: iop = tgOp_FDiv; is_float = true; break;
	default:
		tg_fail(p, "tyg_bin: invalid operation");
		return tg_dummy(p, ta);
	}
	if (ta != tb || (is_float ? !tg_type_is_float(ta) : !tg_type_is_int(ta))) {
		tg_fail(p, "tyg_bin: operand types");
		return tg_dummy(p, ta);
	}
	if ((iop == tgOp_Shl || iop == tgOp_LShr || iop == tgOp_AShr) && tg_type_size(ta) < 4) {
		// the count is taken modulo the bit width; x86 masks it to 5 bits for narrow values
		b = tg_binop(p, tgOp_And, ta, b, tg_iconst(p, ta, 8*tg_type_size(ta) - 1));
	}
	return tg_binop(p, iop, ta, a, b);
}

tyg_val tyg_un(tyg_proc *p, tyg_unop op, tyg_val a) {
	tgType t;
	if (!tg_b_ok(p)) return tg_dummy(p, tgType_I64);
	t = tg_val_check(p, a);
	if (t == tgType_None) return tg_dummy(p, t);
	switch (op) {
	case TYG_NEG:
	case TYG_NOT:
		if (!tg_type_is_int(t)) break;
		return tg_unop(p, op == TYG_NEG ? tgOp_Neg : tgOp_Not, t, a);
	case TYG_FNEG:
		if (!tg_type_is_float(t)) break;
		return tg_unop(p, tgOp_FNeg, t, a);
	}
	tg_fail(p, "tyg_un: operand type");
	return tg_dummy(p, t);
}

tyg_val tyg_cmp(tyg_proc *p, tyg_cond c, tyg_val a, tyg_val b) {
	tgType ta, tb;
	bool fc;
	if (!tg_b_ok(p)) return tg_dummy(p, tgType_I8);
	ta = tg_val_check(p, a);
	tb = tg_val_check(p, b);
	if (ta == tgType_None || tb == tgType_None) return tg_dummy(p, tgType_I8);
	if ((int)c < (int)TYG_EQ || (int)c > (int)TYG_FGE) {
		tg_fail(p, "tyg_cmp: invalid condition");
		return tg_dummy(p, tgType_I8);
	}
	fc = c >= TYG_FEQ;
	if (ta != tb || (fc ? !tg_type_is_float(ta) : !tg_type_is_int(ta))) {
		tg_fail(p, "tyg_cmp: operand types");
		return tg_dummy(p, tgType_I8);
	}
	// tyg_cond and tgCond list the conditions in the same order
	return tg_cmp(p, (tgCond)(tgCond_EQ + (c - TYG_EQ)), ta, a, b);
}

tyg_val tyg_convert(tyg_proc *p, tyg_conv op, tyg_scalar to, tyg_val a) {
	tgType dst = tg_scalar_of(to);
	tgType src;
	tgOp iop = tgOp_Nop;
	bool ok = false;
	if (!tg_b_ok(p)) return tg_dummy(p, dst);
	src = tg_val_check(p, a);
	if (src == tgType_None || dst == tgType_None) {
		tg_fail(p, "tyg_convert: void type");
		return tg_dummy(p, dst);
	}
	switch (op) {
	case TYG_ZEXT:
	case TYG_SEXT:
		ok = tg_type_is_int(src) && tg_type_is_int(dst) && tg_type_size(dst) >= tg_type_size(src);
		iop = op == TYG_ZEXT ? tgOp_Zext : tgOp_Sext;
		break;
	case TYG_TRUNC:
		ok = tg_type_is_int(src) && tg_type_is_int(dst) && tg_type_size(dst) <= tg_type_size(src);
		iop = tgOp_Trunc;
		break;
	case TYG_SITOF:
	case TYG_UITOF:
		ok = tg_type_is_int(src) && tg_type_is_float(dst);
		iop = op == TYG_SITOF ? tgOp_SIToF : tgOp_UIToF;
		break;
	case TYG_FTOSI:
	case TYG_FTOUI:
		ok = tg_type_is_float(src) && tg_type_is_int(dst);
		iop = op == TYG_FTOSI ? tgOp_FToSI : tgOp_FToUI;
		break;
	case TYG_FEXT:
		ok = src == tgType_F32 && dst == tgType_F64;
		iop = tgOp_FExt;
		break;
	case TYG_FTRUNC:
		ok = src == tgType_F64 && dst == tgType_F32;
		iop = tgOp_FTrunc;
		break;
	case TYG_BITCAST:
		ok = tg_type_size(src) == tg_type_size(dst);
		iop = tgOp_Bitcast;
		break;
	}
	if (!ok) {
		tg_fail(p, "tyg_convert: operand types");
		return tg_dummy(p, dst);
	}
	if (src == dst && (iop == tgOp_Zext || iop == tgOp_Sext || iop == tgOp_Trunc || iop == tgOp_Bitcast)) {
		return a;
	}
	return tg_convop(p, iop, dst, src, a);
}

tyg_val tyg_select(tyg_proc *p, tyg_val cond, tyg_val a, tyg_val b) {
	tgType tc, ta, tb;
	if (!tg_b_ok(p)) return tg_dummy(p, tgType_I64);
	tc = tg_val_check(p, cond);
	ta = tg_val_check(p, a);
	tb = tg_val_check(p, b);
	if (tc == tgType_None || ta == tgType_None || tb == tgType_None) return tg_dummy(p, ta);
	if (ta != tb) {
		tg_fail(p, "tyg_select: operand types");
		return tg_dummy(p, ta);
	}
	return tg_select(p, ta, tg_cond_bool(p, cond), a, b);
}

////////////////////////////////////////////////////////////////
// long double
//
// x86-64 SysV computes on the x87 unit with tgOp_X87, whose operands are pointers to the
// values in memory. On arm64 macOS and Windows x64 long double is a double: the calls load,
// compute and store doubles.
////////////////////////////////////////////////////////////////

// Whether the builder may go on with a long double operation.
TG_INTERNAL bool tg_ld_ok(tgProc *p) {
	if (!tg_b_ok(p)) return false;
	if (!tg_ld_supported()) {
		tg_fail(p, "long double is not supported on this target yet");
		return false;
	}
	return true;
}

// Whether long double is a double on this target.
TG_INTERNAL bool tg_ld_is_f64(void) {
	return tg_is_arm64() || tg_is_win64();
}

TG_INTERNAL u32 tg_x87(tgProc *p, tgX87Op op, tgType type, tgType dst_type, u32 a, u32 b, u32 c, i64 imm) {
	tgInstr i = tg_instr_t(tgOp_X87, type);
	i.aux = op;
	i.a = a;
	i.b = b;
	i.c = c;
	i.imm = imm;
	if (dst_type != tgType_None) i.dst = tg_new_vreg(p, dst_type);
	tg_emit(p, i);
	if (op == tgX87_Cmp) tg_note_umax(p, i.dst, 1);
	return i.dst;
}

void tyg_ld_bin(tyg_proc *p, tyg_binop op, tyg_addr dst, tyg_addr a, tyg_addr b) {
	tgMem md, ma, mb;
	tgX87Op xop;
	tgOp fop;
	if (!tg_ld_ok(p)) return;
	if (!tg_addr_to_mem(p, dst, &md) || !tg_addr_to_mem(p, a, &ma) || !tg_addr_to_mem(p, b, &mb)) return;
	switch (op) {
	case TYG_FADD: xop = tgX87_Add; fop = tgOp_FAdd; break;
	case TYG_FSUB: xop = tgX87_Sub; fop = tgOp_FSub; break;
	case TYG_FMUL: xop = tgX87_Mul; fop = tgOp_FMul; break;
	case TYG_FDIV: xop = tgX87_Div; fop = tgOp_FDiv; break;
	default:
		tg_fail(p, "tyg_ld_bin: invalid operation");
		return;
	}
	if (tg_ld_is_f64()) {
		u32 x = tg_load(p, tgType_F64, ma);
		u32 y = tg_load(p, tgType_F64, mb);
		tg_store(p, tgType_F64, md, tg_binop(p, fop, tgType_F64, x, y));
		return;
	}
	tg_x87(p, xop, tgType_None, tgType_None, tg_lea_mem(p, md), tg_lea_mem(p, ma), tg_lea_mem(p, mb), 0);
}

void tyg_ld_neg(tyg_proc *p, tyg_addr dst, tyg_addr a) {
	tgMem md, ma;
	if (!tg_ld_ok(p)) return;
	if (!tg_addr_to_mem(p, dst, &md) || !tg_addr_to_mem(p, a, &ma)) return;
	if (tg_ld_is_f64()) {
		tg_store(p, tgType_F64, md, tg_unop(p, tgOp_FNeg, tgType_F64, tg_load(p, tgType_F64, ma)));
		return;
	}
	tg_x87(p, tgX87_Neg, tgType_None, tgType_None, tg_lea_mem(p, md), tg_lea_mem(p, ma), 0, 0);
}

tyg_val tyg_ld_cmp(tyg_proc *p, tyg_cond c, tyg_addr a, tyg_addr b) {
	tgMem ma, mb;
	tgCond cond;
	if (!tg_ld_ok(p)) return tg_dummy(p, tgType_I8);
	if (!tg_addr_to_mem(p, a, &ma) || !tg_addr_to_mem(p, b, &mb)) return tg_dummy(p, tgType_I8);
	if ((int)c < (int)TYG_FEQ || (int)c > (int)TYG_FGE) {
		tg_fail(p, "tyg_ld_cmp: invalid condition");
		return tg_dummy(p, tgType_I8);
	}
	// tyg_cond and tgCond list the conditions in the same order
	cond = (tgCond)(tgCond_EQ + (c - TYG_EQ));
	if (tg_ld_is_f64()) {
		u32 x = tg_load(p, tgType_F64, ma);
		u32 y = tg_load(p, tgType_F64, mb);
		return tg_cmp(p, cond, tgType_F64, x, y);
	}
	return tg_x87(p, tgX87_Cmp, tgType_None, tgType_I8, 0, tg_lea_mem(p, ma), tg_lea_mem(p, mb), cond);
}

void tyg_ld_from(tyg_proc *p, tyg_conv op, tyg_addr dst, tyg_val a) {
	tgMem md;
	tgType src;
	if (!tg_ld_ok(p)) return;
	if (!tg_addr_to_mem(p, dst, &md)) return;
	src = tg_val_check(p, a);
	if (src == tgType_None) return;
	if ((op == TYG_SITOF || op == TYG_UITOF) ? !tg_type_is_int(src) : (op != TYG_FEXT || !tg_type_is_float(src))) {
		tg_fail(p, "tyg_ld_from: operation or operand type");
		return;
	}
	if (tg_ld_is_f64()) {
		u32 v = a;
		if (op == TYG_SITOF) v = tg_convop(p, tgOp_SIToF, tgType_F64, src, a);
		else if (op == TYG_UITOF) v = tg_convop(p, tgOp_UIToF, tgType_F64, src, a);
		else if (src == tgType_F32) v = tg_convop(p, tgOp_FExt, tgType_F64, src, a);
		tg_store(p, tgType_F64, md, v);
		return;
	}
	{
		u32 d = tg_lea_mem(p, md);
		if (op == TYG_FEXT) {
			tg_x87(p, tgX87_FromF, src, tgType_None, d, 0, a, 0);
		} else if (op == TYG_UITOF && src == tgType_I64) {
			tg_x87(p, tgX87_FromUI, src, tgType_None, d, 0, a, 0);
		} else if (op == TYG_UITOF) {
			// fits a signed 64-bit int
			tg_x87(p, tgX87_FromSI, tgType_I64, tgType_None, d, 0, tg_convop(p, tgOp_Zext, tgType_I64, src, a), 0);
		} else if (tg_type_size(src) < 4) {
			tg_x87(p, tgX87_FromSI, tgType_I32, tgType_None, d, 0, tg_convop(p, tgOp_Sext, tgType_I32, src, a), 0);
		} else {
			tg_x87(p, tgX87_FromSI, src, tgType_None, d, 0, a, 0);
		}
	}
}

tyg_val tyg_ld_to(tyg_proc *p, tyg_conv op, tyg_scalar to, tyg_addr a) {
	tgType dst = tg_scalar_of(to);
	tgMem ma;
	u32 pa;
	if (!tg_ld_ok(p)) return tg_dummy(p, dst);
	if (!tg_addr_to_mem(p, a, &ma)) return tg_dummy(p, dst);
	if (dst == tgType_None || ((op == TYG_FTOSI || op == TYG_FTOUI) ? !tg_type_is_int(dst) : (op != TYG_FTRUNC || !tg_type_is_float(dst)))) {
		tg_fail(p, "tyg_ld_to: operation or result type");
		return tg_dummy(p, dst);
	}
	if (tg_ld_is_f64()) {
		u32 v = tg_load(p, tgType_F64, ma);
		if (op == TYG_FTOSI) return tg_convop(p, tgOp_FToSI, dst, tgType_F64, v);
		if (op == TYG_FTOUI) return tg_convop(p, tgOp_FToUI, dst, tgType_F64, v);
		if (dst == tgType_F32) return tg_convop(p, tgOp_FTrunc, dst, tgType_F64, v);
		return v;
	}
	pa = tg_lea_mem(p, ma);
	if (op == TYG_FTRUNC) return tg_x87(p, tgX87_ToF, dst, dst, 0, pa, 0, 0);
	if (op == TYG_FTOUI && dst == tgType_I64) return tg_x87(p, tgX87_ToUI, dst, dst, 0, pa, 0, 0);
	if (dst == tgType_I32 || dst == tgType_I64) {
		// an unsigned 32-bit result is the low half of a signed 64-bit one
		tgType via = op == TYG_FTOUI ? tgType_I64 : dst;
		u32 v = tg_x87(p, tgX87_ToSI, via, via, 0, pa, 0, 0);
		return via == dst ? v : tg_convop(p, tgOp_Trunc, dst, via, v);
	}
	return tg_convop(p, tgOp_Trunc, dst, tgType_I32, tg_x87(p, tgX87_ToSI, tgType_I32, tgType_I32, 0, pa, 0, 0));
}

////////////////////////////////////////////////////////////////
// Control flow
////////////////////////////////////////////////////////////////

void tyg_jump(tyg_proc *p, tyg_block target) {
	if (!tg_b_ok(p)) return;
	if (!tg_block_check(p, target)) return;
	tg_jump(p, p->blocks.data[target]);
}

void tyg_branch(tyg_proc *p, tyg_val cond, tyg_block if_true, tyg_block if_false) {
	if (!tg_b_ok(p)) return;
	if (tg_val_check(p, cond) == tgType_None) return;
	if (!tg_block_check(p, if_true) || !tg_block_check(p, if_false)) return;
	if (tg_curr_terminated(p)) return;
	tg_branch(p, tg_cond_bool(p, cond), p->blocks.data[if_true], p->blocks.data[if_false]);
}

void tyg_ret(tyg_proc *p, tyg_val v) {
	tgMem none;
	tgType rt;
	if (!tg_b_ok(p)) return;
	memset(&none, 0, sizeof(none));
	if (tg_curr_terminated(p)) return;
	if (v == 0) {
		// C allows a bare return in a non-void function, when the caller does not use the value
		tg_emit_ret(p, false, 0, none);
		return;
	}
	rt = p->sig->ret != NULL ? tg_t_scalar(p->sig->ret) : tgType_None;
	if (tg_val_check(p, v) == tgType_None) return;
	if (rt == tgType_None || p->vregs.data[v] != rt) {
		tg_fail(p, "tyg_ret: the value's type is not the result's");
		return;
	}
	tg_emit_ret(p, true, v, none);
}

void tyg_ret_mem(tyg_proc *p, tyg_addr a) {
	tgMem m;
	tyg_type const *rt;
	if (!tg_b_ok(p)) return;
	if (!tg_addr_to_mem(p, a, &m)) return;
	if (tg_curr_terminated(p)) return;
	rt = p->sig->ret;
	if (rt == NULL || rt->kind == tgT_Void) {
		tg_fail(p, "tyg_ret_mem: the procedure has no result");
		return;
	}
	if (p->abi->has_sret) {
		tgMem dst = tg_mem(tgMem_Reg, tg_load(p, tgType_I64, tg_mem(tgMem_Local, (u32)p->sret_local, 0)), 0);
		tg_memcopy(p, dst, m, rt->size);
		tg_emit_ret(p, false, 0, m);
		return;
	}
	tg_emit_ret(p, true, 0, m);
}

void tyg_trap(tyg_proc *p) {
	if (!tg_b_ok(p)) return;
	if (tg_curr_terminated(p)) return;
	tg_emit(p, tg_instr(tgOp_Trap));
	tg_unreachable(p);
}

void tyg_debug_trap(tyg_proc *p) {
	if (!tg_b_ok(p)) return;
	tg_emit(p, tg_instr(tgOp_DebugTrap));
}

void tyg_unreachable(tyg_proc *p) {
	if (!tg_b_ok(p)) return;
	tg_unreachable(p);
}

////////////////////////////////////////////////////////////////
// Calls
////////////////////////////////////////////////////////////////

tyg_val tyg_call(tyg_proc *p, const tyg_sig *sig, tyg_callee callee,
                 const tyg_arg *args, int count,
                 const tyg_type *const *vararg_types, const tyg_addr *result) {
	tgType rs = tgType_None;
	tgAbiFunc *abi;
	char const *reason = NULL;
	tgArgValue *values;
	i32 direct_sym = -1;
	u32 target_vreg = 0;
	tgMem result_mem;
	bool has_result_mem = false;
	bool aggregate_vararg = false;
	tyg_type const *ret;
	int i;

	if (p == NULL) return 0;
	if (sig != NULL && sig->ret != NULL) rs = tg_t_scalar(sig->ret);
	if (!tg_b_ok(p)) return rs != tgType_None ? tg_dummy(p, rs) : 0;
	if (sig == NULL || count < sig->count || (count > 0 && args == NULL)) {
		tg_fail(p, "tyg_call: bad signature or arguments");
		return rs != tgType_None ? tg_dummy(p, rs) : 0;
	}
	if (count > sig->count && (!(sig->flags & TYG_SIG_VARIADIC) || vararg_types == NULL)) {
		tg_fail(p, "tyg_call: extra arguments to a procedure that is not variadic, or no vararg types");
		return rs != tgType_None ? tg_dummy(p, rs) : 0;
	}
	ret = sig->ret;

	// the callee
	if (callee.sym != TYG_NO_SYM) {
		if (callee.sym < 0 || callee.sym >= p->m->symbols.count) {
			tg_fail(p, "tyg_call: invalid symbol");
			return rs != tgType_None ? tg_dummy(p, rs) : 0;
		}
		direct_sym = callee.sym;
	} else {
		isize k;
		if (!tg_ptr_check(p, callee.val)) return rs != tgType_None ? tg_dummy(p, rs) : 0;
		target_vreg = callee.val;
		// a direct call when the value is the address of a symbol
		if (p->curr != NULL) {
			for (k = p->curr->instrs.count-1; k >= 0; k--) {
				tgInstr const *in = &p->curr->instrs.data[k];
				if (in->dst == target_vreg && in->op == tgOp_Lea) {
					if (in->mem.kind == tgMem_Sym && in->mem.offset == 0) {
						direct_sym = (i32)in->mem.base;
					}
					break;
				}
			}
		}
	}

	// the arguments, checked against their types
	values = tg_alloc_items(tgArgValue, count > 0 ? count : 1);
	for (i = 0; i < count; i++) {
		tyg_type const *t = i < sig->count ? sig->params[i] : vararg_types[i - sig->count];
		tgType st;
		if (t == NULL) {
			tg_fail(p, "tyg_call: no vararg type");
			return rs != tgType_None ? tg_dummy(p, rs) : 0;
		}
		st = tg_t_scalar(t);
		values[i].type = t;
		if (st != tgType_None) {
			tgType vt = tg_val_check(p, args[i].val);
			if (vt == tgType_None) return rs != tgType_None ? tg_dummy(p, rs) : 0;
			if (vt != st) {
				tg_fail(p, "tyg_call: an argument's type is not its parameter's");
				return rs != tgType_None ? tg_dummy(p, rs) : 0;
			}
			values[i].val = args[i].val;
			if (i >= sig->count && st == tgType_F32) {
				// C promotes a float vararg to double
				values[i].val = tg_convop(p, tgOp_FExt, tgType_F64, tgType_F32, values[i].val);
				values[i].type = tyg_type_scalar(p->m, TYG_F64);
			}
		} else {
			if (args[i].val != 0) {
				tg_fail(p, "tyg_call: an aggregate argument is passed as a value");
				return rs != tgType_None ? tg_dummy(p, rs) : 0;
			}
			if (!tg_addr_to_mem(p, args[i].mem, &values[i].mem)) return rs != tgType_None ? tg_dummy(p, rs) : 0;
			if (i >= sig->count) aggregate_vararg = true;
		}
	}
	if (result != NULL && ret != NULL && ret->kind != tgT_Void && rs == tgType_None) {
		if (!tg_addr_to_mem(p, *result, &result_mem)) return 0;
		has_result_mem = true;
	}

	if (tg_is_arm64() && !tg_is_darwin() && count > sig->count) {
		// AAPCS64 passes every vararg like a fixed parameter of its type, aggregates too
		aggregate_vararg = true;
	} else if (tg_is_arm64() && aggregate_vararg) {
		// macOS puts aggregate varargs on the stack, see tg_a64_varargs
		aggregate_vararg = false;
	}

	if (aggregate_vararg && !tg_is_win64()) {
		// SysV passes a vararg like a fixed parameter of its type, so the call is made as
		// one to a signature with every argument fixed, and the callee reads the registers
		// it used from al and its va_list. AAPCS64 does the same, without al.
		tyg_sig *all;
		all = tg_alloc_item(tyg_sig);
		all->m = p->m;
		all->ret = ret;
		all->params = tg_alloc_items(tyg_type const *, count);
		for (i = 0; i < count; i++) all->params[i] = values[i].type;
		all->count = count;
		all->flags = sig->flags & ~(u32)TYG_SIG_VARIADIC;
		abi = tg_sig_abi(all, &reason);
		if (abi == NULL) {
			tg_fail(p, reason ? reason : "abi");
			return rs != tgType_None ? tg_dummy(p, rs) : 0;
		}
		{
			isize call_index = p->calls.count;
			u32 r = tg_emit_call_internal(p, abi, ret, sig->flags, direct_sym, target_vreg, values, count, has_result_mem ? &result_mem : NULL);
			// the call's al: the vector registers the arguments use
			if (p->calls.count > call_index) p->calls.data[call_index].sse_count = abi->xmm_count;
			if (p->fail_reason && rs != tgType_None) return tg_dummy(p, rs);
			return r;
		}
	}

	abi = tg_sig_abi((tyg_sig *)sig, &reason);
	if (abi == NULL) {
		tg_fail(p, reason ? reason : "abi");
		return rs != tgType_None ? tg_dummy(p, rs) : 0;
	}
	{
		u32 r = tg_emit_call_internal(p, abi, ret, sig->flags, direct_sym, target_vreg, values, count, has_result_mem ? &result_mem : NULL);
		if (p->fail_reason && rs != tgType_None) return tg_dummy(p, rs);
		return r;
	}
}

////////////////////////////////////////////////////////////////
// C varargs, Win64
//
// The va_list is a char * into the 8 byte slots of the arguments: the home slots of rcx, rdx,
// r8 and r9 in the shadow area, which the prologue fills (see tg_win64_va_spill), then the
// stack arguments right above them. A value of 1, 2, 4 or 8 bytes is in its slot, any other
// is passed by reference: the slot holds a pointer to it.
////////////////////////////////////////////////////////////////

TG_INTERNAL void tg_win64_va_start(tgProc *p, u32 ap) {
	if (!(p->sig->flags & TYG_SIG_VARIADIC)) {
		tg_fail(p, "tyg_va_start: the procedure is not variadic");
		return;
	}
	// the slot after the last fixed parameter
	tg_store(p, tgType_I64, tg_mem(tgMem_Reg, ap, 0), tg_lea_mem(p, tg_mem(tgMem_Incoming, 0, 8*p->abi->gpr_count)));
}

TG_INTERNAL u32 tg_win64_va_arg(tgProc *p, u32 ap, tyg_type const *t) {
	tgMem ap_m = tg_mem(tgMem_Reg, ap, 0);
	u32 slot = tg_load(p, tgType_I64, ap_m);
	tg_store(p, tgType_I64, ap_m, tg_ptr_add_const(p, slot, 8));
	if (t->size == 1 || t->size == 2 || t->size == 4 || t->size == 8) return slot;
	return tg_load(p, tgType_I64, tg_mem(tgMem_Reg, slot, 0));
}

////////////////////////////////////////////////////////////////
// C varargs, SysV x86-64
//
// The va_list is {u32 gp_offset; u32 fp_offset; void *overflow_arg_area; void *reg_save_area;}.
// The prologue spills the argument registers the fixed parameters leave into the procedure's
// register save area (see tg_va_save_area), and va_arg takes an argument from there while the
// registers its class needs last, then from the stack.
////////////////////////////////////////////////////////////////

TG_INTERNAL bool tg_va_check(tgProc *p, u32 ap) {
	if (!tg_ptr_check(p, ap)) return false;
	return true;
}

// The arm64 forms, below.
TG_INTERNAL void tg_a64_va_start(tgProc *p, u32 ap);
TG_INTERNAL u32  tg_a64_va_arg(tgProc *p, u32 ap, tyg_type const *t);

void tyg_va_start(tyg_proc *p, tyg_val ap) {
	tgAbiFunc *abi;
	if (!tg_b_ok(p)) return;
	if (!tg_va_check(p, ap)) return;
	if (tg_is_win64()) {
		tg_win64_va_start(p, ap);
		return;
	}
	if (!(p->sig->flags & TYG_SIG_VARIADIC)) {
		tg_fail(p, "tyg_va_start: the procedure is not variadic");
		return;
	}
	if (tg_is_arm64()) {
		tg_a64_va_start(p, ap);
		return;
	}
	abi = p->abi;
	tg_store(p, tgType_I32, tg_mem(tgMem_Reg, ap, 0), tg_iconst(p, tgType_I32, 8*abi->gpr_count));
	tg_store(p, tgType_I32, tg_mem(tgMem_Reg, ap, 4), tg_iconst(p, tgType_I32, TG_VA_GP_END + 16*abi->xmm_count));
	tg_store(p, tgType_I64, tg_mem(tgMem_Reg, ap, 8), tg_lea_mem(p, tg_mem(tgMem_Incoming, 0, abi->stack_size)));
	tg_store(p, tgType_I64, tg_mem(tgMem_Reg, ap, 16), tg_lea_mem(p, tg_mem(tgMem_Local, (u32)p->va_save_local, 0)));
}

// The next argument from the overflow area, its address stored to res.
TG_INTERNAL void tg_va_arg_overflow(tgProc *p, u32 ap, tyg_type const *t, tgMem res, bool advance) {
	tgMem ov_m = tg_mem(tgMem_Reg, ap, 8);
	u32 ov = tg_load(p, tgType_I64, ov_m);
	if (t->align > 8) {
		ov = tg_binop(p, tgOp_And, tgType_I64, tg_ptr_add_const(p, ov, t->align-1), tg_iconst(p, tgType_I64, -t->align));
	}
	tg_store(p, tgType_I64, res, ov);
	if (advance) {
		tg_store(p, tgType_I64, ov_m, tg_ptr_add_const(p, ov, tg_align_up(t->size, 8)));
	}
}

tyg_val tyg_va_arg(tyg_proc *p, tyg_val ap, const tyg_type *t) {
	tyg_sig *one;
	tgAbiFunc *abi;
	tgAbiArg const *arg;
	char const *reason = NULL;
	tgMem res;
	i32 ngp = 0, nfp = 0, i;

	if (!tg_b_ok(p)) return tg_dummy(p, tgType_I64);
	if (!tg_va_check(p, ap)) return tg_dummy(p, tgType_I64);
	if (t == NULL) {
		tg_fail(p, "tyg_va_arg: no type");
		return tg_dummy(p, tgType_I64);
	}
	if (tg_t_unsupported_ld(t)) {
		tg_fail(p, "long double is not supported on this target yet");
		return tg_dummy(p, tgType_I64);
	}
	if (tg_is_arm64()) return tg_a64_va_arg(p, ap, t);
	if (tg_is_win64()) return tg_win64_va_arg(p, ap, t);

	// classify the type as the only parameter of a signature
	one = tg_alloc_item(tyg_sig);
	one->m = p->m;
	one->params = tg_alloc_items(tyg_type const *, 1);
	one->params[0] = t;
	one->count = 1;
	one->ret = tyg_type_scalar(p->m, TYG_VOID);
	abi = tg_sig_abi(one, &reason);
	if (abi == NULL) {
		tg_fail(p, reason ? reason : "abi");
		return tg_dummy(p, tgType_I64);
	}
	arg = &abi->params.data[0];

	// where the argument's address goes from the paths
	res = tg_mem(tgMem_Local, (u32)tg_add_local_raw(p, 8, 8), 0);

	if (arg->kind != tgArg_Direct || arg->piece_count == 0) {
		// in memory, or a zero sized type that takes no room
		tg_va_arg_overflow(p, ap, t, res, arg->kind != tgArg_Ignore);
		return tg_load(p, tgType_I64, res);
	}

	for (i = 0; i < arg->piece_count; i++) {
		tgAbiPiece const *piece = &abi->pieces.data[arg->piece_index+i];
		if (piece->loc == tgLoc_Gpr) ngp++;
		else if (piece->loc == tgLoc_Xmm) nfp++;
	}
	if (ngp + nfp != arg->piece_count) {
		tg_fail(p, "tyg_va_arg: unexpected classification");
		return tg_dummy(p, tgType_I64);
	}

	{
		tgMem gp_m = tg_mem(tgMem_Reg, ap, 0);
		tgMem fp_m = tg_mem(tgMem_Reg, ap, 4);
		u32 gp = 0, fp = 0, fits = 0;
		tgBlock *in_regs = tg_new_block(p);
		tgBlock *in_mem  = tg_new_block(p);
		tgBlock *join    = tg_new_block(p);
		u32 save, addr;

		if (ngp > 0) {
			gp = tg_load(p, tgType_I32, gp_m);
			fits = tg_cmp(p, tgCond_ULE, tgType_I32, gp, tg_iconst(p, tgType_I32, TG_VA_GP_END - 8*ngp));
		}
		if (nfp > 0) {
			u32 c;
			fp = tg_load(p, tgType_I32, fp_m);
			c = tg_cmp(p, tgCond_ULE, tgType_I32, fp, tg_iconst(p, tgType_I32, TG_VA_SAVE_SIZE - 16*nfp));
			fits = fits ? tg_binop(p, tgOp_And, tgType_I8, fits, c) : c;
		}
		tg_branch(p, fits, in_regs, in_mem);

		// from the register save area
		tg_start_block(p, in_regs, true);
		save = tg_load(p, tgType_I64, tg_mem(tgMem_Reg, ap, 16));
		if (arg->piece_count == 1) {
			// one register holds the whole value
			u32 off = abi->pieces.data[arg->piece_index].loc == tgLoc_Gpr ? gp : fp;
			addr = tg_binop(p, tgOp_Add, tgType_I64, save, tg_convop(p, tgOp_Zext, tgType_I64, tgType_I32, off));
		} else {
			// the pieces come from separate registers, put them together
			i32 gi = 0, fi = 0;
			tgMem tmp = tg_mem(tgMem_Local, (u32)tg_add_local_raw(p, tg_align_up(tg_max(t->size, (i64)1), 8), tg_max(t->align, (i64)8)), 0);
			for (i = 0; i < arg->piece_count; i++) {
				tgAbiPiece const *piece = &abi->pieces.data[arg->piece_index+i];
				u32 off;
				u32 src;
				if (piece->loc == tgLoc_Gpr) {
					off = tg_convop(p, tgOp_Zext, tgType_I64, tgType_I32, gp);
					off = tg_binop(p, tgOp_Add, tgType_I64, off, tg_iconst(p, tgType_I64, 8*gi++));
				} else {
					off = tg_convop(p, tgOp_Zext, tgType_I64, tgType_I32, fp);
					off = tg_binop(p, tgOp_Add, tgType_I64, off, tg_iconst(p, tgType_I64, 16*fi++));
				}
				src = tg_binop(p, tgOp_Add, tgType_I64, save, off);
				tg_memcopy(p, tg_mem_offset(tmp, piece->src_offset), tg_mem(tgMem_Reg, src, 0), piece->size);
			}
			addr = tg_lea_mem(p, tmp);
		}
		tg_store(p, tgType_I64, res, addr);
		if (ngp > 0) tg_store(p, tgType_I32, gp_m, tg_binop(p, tgOp_Add, tgType_I32, gp, tg_iconst(p, tgType_I32, 8*ngp)));
		if (nfp > 0) tg_store(p, tgType_I32, fp_m, tg_binop(p, tgOp_Add, tgType_I32, fp, tg_iconst(p, tgType_I32, 16*nfp)));
		tg_jump(p, join);

		// from the stack
		tg_start_block(p, in_mem, true);
		tg_va_arg_overflow(p, ap, t, res, true);
		tg_jump(p, join);

		tg_start_block(p, join, true);
		return tg_load(p, tgType_I64, res);
	}
}

void tyg_va_copy(tyg_proc *p, tyg_val dst, tyg_val src) {
	i64 size = TG_VA_LIST_SIZE;
	if (!tg_b_ok(p)) return;
	if (!tg_va_check(p, dst) || !tg_va_check(p, src)) return;
	if (tg_is_arm64()) size = tg_is_darwin() ? 8 : TG_A64_VA_LIST_SIZE;
	if (tg_is_win64()) size = 8;
	tg_memcopy(p, tg_mem(tgMem_Reg, dst, 0), tg_mem(tgMem_Reg, src, 0), size);
}

////////////////////////////////////////////////////////////////
// C varargs, arm64
//
// macOS: the va_list is a char * to the next argument on the stack. Every vararg sits in
// 8 byte slots, an aggregate of up to 16 bytes or a float aggregate as its bytes (aligned
// to 16 when its type is), a larger one as a pointer to a copy.
//
// Linux (AAPCS64): the va_list is {void *stack; void *gr_top; void *vr_top; i32 gr_offs;
// i32 vr_offs;}. The offsets count up from minus the size of the unused part of each half
// of the register save area (see tg_a64_va_save_area) to 0; va_arg takes an argument from
// there while the registers its class needs last, then from the stack, like clang's
// EmitAAPCSVAArg.
////////////////////////////////////////////////////////////////

TG_INTERNAL void tg_a64_va_start(tgProc *p, u32 ap) {
	tgAbiFunc *abi = p->abi;
	u32 stack = tg_lea_mem(p, tg_mem(tgMem_Incoming, 0, (i32)tg_align_up(abi->stack_size, 8)));
	if (tg_is_darwin()) {
		tg_store(p, tgType_I64, tg_mem(tgMem_Reg, ap, 0), stack);
		return;
	}
	TG_ASSERT(p->va_save_local >= 0);
	tg_store(p, tgType_I64, tg_mem(tgMem_Reg, ap, 0), stack);
	tg_store(p, tgType_I64, tg_mem(tgMem_Reg, ap, 8), tg_lea_mem(p, tg_mem(tgMem_Local, (u32)p->va_save_local, TG_A64_VA_GR_SIZE)));
	tg_store(p, tgType_I64, tg_mem(tgMem_Reg, ap, 16), tg_lea_mem(p, tg_mem(tgMem_Local, (u32)p->va_save_local, TG_A64_VA_SAVE_SIZE)));
	tg_store(p, tgType_I32, tg_mem(tgMem_Reg, ap, 24), tg_iconst(p, tgType_I32, -8*(8 - abi->gpr_count)));
	tg_store(p, tgType_I32, tg_mem(tgMem_Reg, ap, 28), tg_iconst(p, tgType_I32, -16*(8 - abi->xmm_count)));
}

// Whether a vararg of type t is passed as a pointer to a copy: an aggregate larger than 16
// bytes that is not a float aggregate.
TG_INTERNAL bool tg_a64_va_indirect(tyg_type const *t) {
	tgType base = tgType_None;
	i64 members = 0;
	return !tg_t_is_scalar(t) && t->size > 16 && !a64_hfa(t, &base, &members);
}

TG_INTERNAL u32 tg_a64_va_arg(tgProc *p, u32 ap, tyg_type const *t) {
	bool indirect = tg_a64_va_indirect(t);
	tgType base = tgType_None;
	i64 members = 0;
	bool hfa = !tg_t_is_scalar(t) && a64_hfa(t, &base, &members);
	bool fpr = hfa || t->kind == tgT_F32 || t->kind == tgT_F64;
	tgMem stack_m = tg_mem(tgMem_Reg, ap, 0);

	if (tg_is_darwin()) {
		u32 at = tg_load(p, tgType_I64, stack_m);
		if (!indirect && t->align > 8) {
			at = tg_binop(p, tgOp_And, tgType_I64, tg_ptr_add_const(p, at, t->align-1), tg_iconst(p, tgType_I64, -t->align));
		}
		tg_store(p, tgType_I64, stack_m, tg_ptr_add_const(p, at, indirect ? 8 : tg_align_up(t->size, 8)));
		if (indirect) return tg_load(p, tgType_I64, tg_mem(tgMem_Reg, at, 0));
		return at;
	}

	{
		// the register save area
		i32 reg_size = indirect ? 8 : fpr ? 16*(hfa ? (i32)members : 1) : (i32)tg_align_up(t->size, 8);
		tgMem offs_m = tg_mem(tgMem_Reg, ap, fpr ? 28 : 24);
		tgMem top_m = tg_mem(tgMem_Reg, ap, fpr ? 16 : 8);
		tgBlock *maybe_reg, *in_reg, *on_stack, *join;
		tgMem res;
		u32 offs, new_offs, addr, on;

		if (t->size == 0) {
			// nothing was passed
			return tg_load(p, tgType_I64, stack_m);
		}
		// where the argument's address goes from the paths
		res = tg_mem(tgMem_Local, (u32)tg_add_local_raw(p, 8, 8), 0);
		maybe_reg = tg_new_block(p);
		in_reg    = tg_new_block(p);
		on_stack  = tg_new_block(p);
		join      = tg_new_block(p);
		offs = tg_load(p, tgType_I32, offs_m);

		if (!fpr && !indirect && t->align > 8) {
			// an even register pair
			offs = tg_binop(p, tgOp_And, tgType_I32, tg_binop(p, tgOp_Add, tgType_I32, offs, tg_iconst(p, tgType_I32, 15)), tg_iconst(p, tgType_I32, -16));
		}
		// a negative offset is in the save area, else every register of the class is used
		tg_branch(p, tg_cmp(p, tgCond_SGE, tgType_I32, offs, tg_iconst(p, tgType_I32, 0)), on_stack, maybe_reg);

		// the offset moves past the argument's registers, whether they hold it or not
		tg_start_block(p, maybe_reg, true);
		new_offs = tg_binop(p, tgOp_Add, tgType_I32, offs, tg_iconst(p, tgType_I32, reg_size));
		tg_store(p, tgType_I32, offs_m, new_offs);
		tg_branch(p, tg_cmp(p, tgCond_SLE, tgType_I32, new_offs, tg_iconst(p, tgType_I32, 0)), in_reg, on_stack);

		tg_start_block(p, in_reg, true);
		addr = tg_binop(p, tgOp_Add, tgType_I64, tg_load(p, tgType_I64, top_m), tg_convop(p, tgOp_Sext, tgType_I64, tgType_I32, offs));
		if (indirect) {
			addr = tg_load(p, tgType_I64, tg_mem(tgMem_Reg, addr, 0));
		} else if (hfa && members > 1) {
			// the members are 16 bytes apart in the save area, put them together
			i32 bsize = tg_type_size(base);
			i64 i;
			tgMem tmp = tg_mem(tgMem_Local, (u32)tg_add_local_raw(p, tg_align_up(t->size, 8), tg_max(t->align, (i64)8)), 0);
			for (i = 0; i < members; i++) {
				u32 v = tg_load(p, base, tg_mem(tgMem_Reg, addr, (i32)(16*i)));
				tg_store(p, base, tg_mem_offset(tmp, i*bsize), v);
			}
			addr = tg_lea_mem(p, tmp);
		}
		tg_store(p, tgType_I64, res, addr);
		tg_jump(p, join);

		// from the stack
		tg_start_block(p, on_stack, true);
		on = tg_load(p, tgType_I64, stack_m);
		if (!indirect && t->align > 8) {
			on = tg_binop(p, tgOp_And, tgType_I64, tg_ptr_add_const(p, on, 15), tg_iconst(p, tgType_I64, -16));
		}
		tg_store(p, tgType_I64, stack_m, tg_ptr_add_const(p, on, indirect ? 8 : tg_align_up(t->size, 8)));
		if (indirect) on = tg_load(p, tgType_I64, tg_mem(tgMem_Reg, on, 0));
		tg_store(p, tgType_I64, res, on);
		tg_jump(p, join);

		tg_start_block(p, join, true);
		return tg_load(p, tgType_I64, res);
	}
}

////////////////////////////////////////////////////////////////
// Atomics
////////////////////////////////////////////////////////////////

void tyg_fence(tyg_proc *p) {
	if (!tg_b_ok(p)) return;
	tg_emit(p, tg_instr(tgOp_AtomicFence));
}

TG_INTERNAL tgType tg_float_bits_type(tgType t) {
	return t == tgType_F32 ? tgType_I32 : t == tgType_F64 ? tgType_I64 : t;
}

tyg_val tyg_atomic_load(tyg_proc *p, tyg_scalar type, tyg_addr a) {
	tgType t = tg_scalar_of(type);
	tgMem m;
	tgInstr i;
	if (!tg_b_ok(p)) return tg_dummy(p, t);
	if (t == tgType_None) {
		tg_fail(p, "tyg_atomic_load: void type");
		return tg_dummy(p, t);
	}
	if (!tg_addr_to_mem(p, a, &m)) return tg_dummy(p, t);
	i = tg_instr_t(tgOp_AtomicLoad, t);
	i.dst = tg_new_vreg(p, t);
	i.mem = m;
	tg_emit(p, i);
	return i.dst;
}

void tyg_atomic_store(tyg_proc *p, tyg_scalar type, tyg_addr a, tyg_val v) {
	tgType st = tg_scalar_of(type);
	tgType it;
	tgMem m;
	tgInstr i;
	u32 r = v;
	if (!tg_b_ok(p)) return;
	if (tg_val_check(p, v) == tgType_None) return;
	if (st == tgType_None || p->vregs.data[v] != st) {
		tg_fail(p, "tyg_atomic_store: the value's type is not the store's");
		return;
	}
	if (!tg_addr_to_mem(p, a, &m)) return;
	it = tg_float_bits_type(st);
	if (it != st) r = tg_convop(p, tgOp_Bitcast, it, st, r);
	i = tg_instr_t(tgOp_AtomicStore, it);
	i.a = r;
	i.mem = m;
	tg_emit(p, i);
}

tyg_val tyg_atomic_rmw(tyg_proc *p, tyg_rmw op, tyg_addr a, tyg_val v) {
	tgType st;
	tgMem m;
	tgInstr i;
	if (!tg_b_ok(p)) return tg_dummy(p, tgType_I64);
	st = tg_val_check(p, v);
	if (st == tgType_None) return tg_dummy(p, st);
	if (!tg_type_is_int(st)) {
		tg_fail(p, "atomic op type");
		return tg_dummy(p, st);
	}
	if ((int)op < (int)TYG_RMW_XCHG || (int)op > (int)TYG_RMW_NAND) {
		tg_fail(p, "tyg_atomic_rmw: invalid operation");
		return tg_dummy(p, st);
	}
	if (!tg_addr_to_mem(p, a, &m)) return tg_dummy(p, st);
	i = tg_instr_t(tgOp_AtomicRmw, st);
	// tyg_rmw and tgRmwOp list the operations in the same order
	i.aux = (u8)(tgRmw_Xchg + (op - TYG_RMW_XCHG));
	i.a = v;
	i.mem = m;
	i.dst = tg_new_vreg(p, st);
	tg_emit(p, i);
	return i.dst;
}

tyg_val tyg_atomic_cas(tyg_proc *p, tyg_addr a, tyg_val expected, tyg_val desired, tyg_val *ok) {
	tgType st, dt, it;
	tgMem m;
	tgInstr i;
	u32 oldv = expected, newv = desired, loaded;
	if (ok) *ok = 0;
	if (!tg_b_ok(p)) {
		if (ok && p) *ok = tg_dummy(p, tgType_I8);
		return tg_dummy(p, tgType_I64);
	}
	st = tg_val_check(p, expected);
	dt = tg_val_check(p, desired);
	if (st == tgType_None || dt == tgType_None || st != dt) {
		tg_fail(p, "tyg_atomic_cas: operand types");
		if (ok) *ok = tg_dummy(p, tgType_I8);
		return tg_dummy(p, st);
	}
	if (!tg_addr_to_mem(p, a, &m)) {
		if (ok) *ok = tg_dummy(p, tgType_I8);
		return tg_dummy(p, st);
	}
	it = tg_float_bits_type(st);
	if (it != st) {
		oldv = tg_convop(p, tgOp_Bitcast, it, st, oldv);
		newv = tg_convop(p, tgOp_Bitcast, it, st, newv);
	}
	i = tg_instr_t(tgOp_AtomicCas, it);
	i.a = oldv;
	i.b = newv;
	i.mem = m;
	i.dst = tg_new_vreg(p, it);
	i.c = tg_new_vreg(p, tgType_I8);
	tg_emit(p, i);
	tg_note_umax(p, i.c, 1);
	loaded = i.dst;
	if (it != st) loaded = tg_convop(p, tgOp_Bitcast, st, it, loaded);
	if (ok) *ok = i.c;
	return loaded;
}

////////////////////////////////////////////////////////////////
// Intrinsics
////////////////////////////////////////////////////////////////

tyg_val tyg_intrin(tyg_proc *p, tyg_intrinsic op, tyg_val a) {
	tgType t;
	tgInstr i;
	if (!tg_b_ok(p)) return tg_dummy(p, tgType_I64);
	switch (op) {
	case TYG_SQRT:
		t = tg_val_check(p, a);
		if (t == tgType_None) return tg_dummy(p, t);
		if (!tg_type_is_float(t)) break;
		return tg_unop(p, tgOp_Sqrt, t, a);
	case TYG_BSWAP:
	case TYG_POPCOUNT:
	case TYG_CTZ:
	case TYG_CLZ: {
		tgOp iop = op == TYG_BSWAP ? tgOp_Bswap : op == TYG_POPCOUNT ? tgOp_Popcount : op == TYG_CTZ ? tgOp_Ctz : tgOp_Clz;
		t = tg_val_check(p, a);
		if (t == tgType_None) return tg_dummy(p, t);
		if (!tg_type_is_int(t)) break;
		return tg_unop(p, iop, t, a);
	}
	case TYG_FRAME_ADDRESS:
	case TYG_RETURN_ADDRESS:
	case TYG_STACK_POINTER:
		i = tg_instr_t(op == TYG_FRAME_ADDRESS ? tgOp_FrameAddress : op == TYG_RETURN_ADDRESS ? tgOp_ReturnAddress : tgOp_StackPointer, tgType_I64);
		i.dst = tg_new_vreg(p, tgType_I64);
		tg_emit(p, i);
		return i.dst;
	case TYG_CPU_RELAX:
		tg_emit(p, tg_instr(tgOp_CpuRelax));
		return 0;
	case TYG_STACK_RESTORE:
		if (!tg_ptr_check(p, a)) return 0;
		i = tg_instr(tgOp_StackRestore);
		i.a = a;
		tg_emit(p, i);
		return 0;
	default:
		tg_fail(p, "tyg_intrin: invalid intrinsic");
		return tg_dummy(p, tgType_I64);
	}
	tg_fail(p, "tyg_intrin: operand type");
	return tg_dummy(p, t);
}

////////////////////////////////////////////////////////////////
// Debug info
////////////////////////////////////////////////////////////////

void tyg_loc(tyg_proc *p, tyg_file file, int line, int column) {
	if (!tg_b_ok(p)) return;
	tg_set_debug_loc(p, file, line, column);
}

void tyg_debug_proc(tyg_proc *p, const char *name, tyg_file file, int line, const tyg_dtype *type) {
	if (!tg_b_ok(p)) return;
	p->debug_name = tg_module_str(p->m, name);
	p->decl_file = file;
	p->decl_line = line;
	p->debug_type = type;
	// the entry starts on the declaration's line, before the parameters' code that
	// tyg_proc_begin put there
	if (line > 0 && file > 0 && p->last_line <= 0 && p->blocks.count > 0) {
		tgBlock *entry = p->blocks.data[0];
		tgInstr loc = tg_instr(tgOp_Loc);
		loc.imm = line;
		loc.a = (u32)file;
		loc.b = 0;
		tg_array_add(&entry->instrs, loc);
		memmove(&entry->instrs.data[1], &entry->instrs.data[0], sizeof(tgInstr) * (usize)(entry->instrs.count - 1));
		entry->instrs.data[0] = loc;
		p->last_line = line;
		p->last_column = 0;
		p->file_id = file;
	}
}

void tyg_scope_begin(tyg_proc *p) {
	if (!tg_b_ok(p)) return;
	tg_open_scope(p);
}

void tyg_scope_end(tyg_proc *p) {
	if (!tg_b_ok(p)) return;
	if (p->debug_scope <= 0) {
		tg_fail(p, "tyg_scope_end: no open scope");
		return;
	}
	tg_close_scope(p);
}

// Records a debug variable or parameter of the current scope.
TG_INTERNAL void tg_add_debug_var(tgProc *p, char const *name, tyg_dtype const *t, tyg_addr where, tyg_file file, int line, bool is_param, bool in_prologue) {
	tgDebugVar v;
	tgMem m;
	if (name == NULL || name[0] == 0) return;
	if (where.offset < INT32_MIN || where.offset > INT32_MAX) {
		tg_fail(p, "debug variable offset out of range");
		return;
	}
	memset(&v, 0, sizeof(v));
	v.local = -1;
	v.sym = -1;
	switch (where.kind) {
	case TYG_ADDR_LOCAL:
		if (where.base < 0 || where.base >= p->locals.count) {
			tg_fail(p, "debug variable: invalid local");
			return;
		}
		v.local = where.base;
		v.offset = (i32)where.offset;
		break;
	case TYG_ADDR_SYM:
		if (where.base < 0 || where.base >= p->m->symbols.count) {
			tg_fail(p, "debug variable: invalid symbol");
			return;
		}
		v.sym = where.base;
		v.offset = (i32)where.offset;
		break;
	case TYG_ADDR_VAL:
		// the address of a local or a symbol, like an aggregate parameter's
		if (!tg_ptr_check(p, (tyg_val)where.base)) return;
		if (where.offset == 0 && tg_val_param_ptr_local(p, (u32)where.base, &v.local)) {
			// passed by pointer: the parameter's local holds the variable's address
			v.by_ref = true;
			break;
		}
		if (!tg_val_addr_mem(p, (u32)where.base, &m)) {
			tg_fail(p, "debug variable: the address is not a local's or a symbol's");
			return;
		}
		m = tg_mem_offset(m, where.offset);
		if (m.kind == tgMem_Local) v.local = (i32)m.base;
		else v.sym = (i32)m.base;
		v.offset = m.offset;
		break;
	default:
		tg_fail(p, "debug variable: invalid address kind");
		return;
	}
	// a local's offset goes in frame_offset_fixup, which the lowering adds the local's frame
	// offset to
	if (v.local >= 0) v.frame_offset_fixup = v.offset;
	v.name = tg_module_str(p->m, name);
	v.type = t;
	v.is_param = is_param;
	v.line = line;
	v.file_id = file;
	v.scope = p->debug_scope;
	tg_array_add(&p->debug_vars, v);
	if (in_prologue && p->blocks.count > 0 && p->curr == p->blocks.data[0] && !tg_curr_terminated(p)) {
		// the prologue ends for the debugger once the parameters are where it was told,
		// like LLVM's after their stores: a breakpoint on the procedure stops there
		tgBlock *entry = p->blocks.data[0];
		if (p->params_ready > 0) entry->instrs.data[p->params_ready-1].imm = 0;
		tgInstr mark = tg_instr(tgOp_Nop);
		mark.imm = 2;
		tg_emit(p, mark);
		p->params_ready = entry->instrs.count;
	}
}

void tyg_debug_var(tyg_proc *p, const char *name, const tyg_dtype *t, tyg_addr where, tyg_file file, int line) {
	if (!tg_b_ok(p)) return;
	tg_add_debug_var(p, name, t, where, file, line, false, false);
}

void tyg_debug_param(tyg_proc *p, const char *name, const tyg_dtype *t, tyg_addr where, tyg_file file, int line) {
	if (!tg_b_ok(p)) return;
	tg_add_debug_var(p, name, t, where, file, line, true, true);
}

void tyg_debug_var_ref(tyg_proc *p, const char *name, const tyg_dtype *t, tyg_addr where, tyg_file file, int line) {
	isize n;
	if (!tg_b_ok(p)) return;
	if (where.kind != TYG_ADDR_LOCAL) {
		tg_fail(p, "tyg_debug_var_ref: the pointer must be in a local");
		return;
	}
	n = p->debug_vars.count;
	// in the entry block it is set up like a parameter: the prologue ends after it
	tg_add_debug_var(p, name, t, where, file, line, false, true);
	if (p->debug_vars.count > n) tg_array_last(&p->debug_vars)->by_ref = true;
}
