// Building a procedure's IR: failure, new vregs and instructions, constant folding as the
// instructions are made, and the debug locations and scopes.
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
