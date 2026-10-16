// The arm64 calling conventions of Apple and of Linux (AAPCS64), which must match clang's
// because procedures from both compilers call each other.
//
// The classifier follows clang's AArch64ABIInfo for C, and the assignment of the pieces
// follows LLVM's CC_AArch64_DarwinPCS and CC_AArch64_AAPCS:
//   - a scalar takes the next x or v register, then a stack slot of its own size on Apple,
//     of at least 8 bytes on Linux
//   - a homogeneous float aggregate of up to four members takes consecutive v registers,
//     and a 9 to 16 byte aggregate two x registers, all or nothing: when they do not fit,
//     every register of the class counts as used and the members go onto the stack
//   - any other aggregate of up to 8 bytes takes one x register, or an 8 byte stack slot
//   - a 16 byte aligned aggregate of up to 16 bytes is an i128 to clang: two consecutive x
//     registers, starting at an even one on Linux; Apple drops that rule but never starts
//     it at x7; on the stack it is 16 byte aligned
//   - anything larger than 16 bytes is passed by pointer to a copy, and returned through x8
//   - Apple makes the caller extend an integer narrower than 32 bits, and the callee its
//     result; Linux extends neither
//   - a C vararg goes onto the stack in an 8 byte slot on Apple (build.c), and is assigned
//     like a fixed argument on Linux

// Apple's arm64 calling convention, rather than AAPCS64
TG_INTERNAL bool a64_is_apple(void) {
	return tg_is_darwin();
}

enum {
	A64_SRET_REG = 8, // x8 carries the address of a result returned in memory
};

////////////////////////////////////////////////////////////////
// Homogeneous float aggregates
////////////////////////////////////////////////////////////////

// The walk over a value's scalar leaves, like clang's isHomogeneousAggregate on C types.
typedef struct a64HfaWalk {
	tgTKind kind;    // tgT_F32 or tgT_F64 once the first leaf is seen, else tgT_Void
	i64     size;    // of that kind
	u32     covered; // bit n: a leaf fills member slot n
	bool    ok;
} a64HfaWalk;

TG_INTERNAL void a64_hfa_walk(a64HfaWalk *w, tyg_type const *t, i64 off) {
	i64 i;
	if (!w->ok || t->size == 0) {
		// an empty record or a zero length array occupies nothing
		return;
	}
	switch (t->kind) {
	case tgT_F32:
	case tgT_F64: {
		i64 slot;
		if (w->kind == tgT_Void) {
			w->kind = t->kind;
			w->size = t->size;
		} else if (w->kind != t->kind) {
			w->ok = false;
			return;
		}
		if (off % w->size != 0) {
			w->ok = false;
			return;
		}
		slot = off / w->size;
		if (slot >= 4) {
			w->ok = false;
			return;
		}
		w->covered |= 1u << slot;
		return;
	}
	case tgT_Array:
		for (i = 0; i < t->count && w->ok; i++) {
			a64_hfa_walk(w, t->elem, off + i*t->elem->size);
		}
		return;
	case tgT_Record:
		// overlapping fields, a union, cover the same slots
		for (i = 0; i < t->field_count && w->ok; i++) {
			a64_hfa_walk(w, t->fields[i].type, off + t->fields[i].offset);
		}
		return;
	default:
		w->ok = false;
		return;
	}
}

// Whether the record or array t is a homogeneous float aggregate: up to four members of one
// float type that fill it exactly. A scalar float is not one; it is a register type.
TG_INTERNAL bool a64_hfa(tyg_type const *t, tgType *base, i64 *count) {
	a64HfaWalk w;
	i64 n;
	if (t->kind != tgT_Record && t->kind != tgT_Array) return false;
	if (t->size <= 0 || t->size > 32) return false;
	memset(&w, 0, sizeof(w));
	w.kind = tgT_Void;
	w.ok = true;
	a64_hfa_walk(&w, t, 0);
	if (!w.ok || w.kind == tgT_Void) return false;
	if (t->size % w.size != 0) return false;
	n = t->size / w.size;
	if (n < 1 || n > 4 || w.covered != (1u << n) - 1) return false;
	*base = w.kind == tgT_F32 ? tgType_F32 : tgType_F64;
	*count = n;
	return true;
}

////////////////////////////////////////////////////////////////
// Classification
////////////////////////////////////////////////////////////////

// How one value is passed, before registers are handed out.
typedef struct a64Class {
	tgArgKind  kind;
	tgAbiPiece pieces[4];
	i32        count;
	bool       block;       // the pieces need consecutive registers of one class
	i64        block_align; // stack alignment of the first piece when the block goes onto the stack
	i32        stack_size;  // block: the stack bytes of each piece, 0 for its own size
	bool       i128_pair;   // two consecutive x registers, x7 cannot hold the first half
} a64Class;

TG_INTERNAL tgType a64_int_piece_type(i64 size) {
	return size <= 1 ? tgType_I8 : size <= 2 ? tgType_I16 : size <= 4 ? tgType_I32 : tgType_I64;
}

TG_INTERNAL void a64_add_piece(a64Class *c, tgType type, i32 size, i32 offset, tgLocKind loc, tgExtKind ext) {
	tgAbiPiece p;
	TG_ASSERT(c->count < 4);
	memset(&p, 0, sizeof(p));
	p.type = type;
	p.size = size;
	p.src_offset = offset;
	p.loc = loc;
	p.ext = ext;
	c->pieces[c->count++] = p;
}

// A scalar, which takes one register. Apple extends an integer narrower than 32 bits.
TG_INTERNAL void a64_register_pieces(a64Class *c, tyg_type const *t) {
	switch (t->kind) {
	case tgT_Ptr: a64_add_piece(c, tgType_I64, 8, 0, tgLoc_Gpr, tgExt_None); break;
	case tgT_F32: a64_add_piece(c, tgType_F32, 4, 0, tgLoc_Xmm, tgExt_None); break;
	case tgT_F64: a64_add_piece(c, tgType_F64, 8, 0, tgLoc_Xmm, tgExt_None); break;
	case tgT_Int: {
		tgExtKind ext = tgExt_None;
		i32 size = (i32)t->size;
		if (size < 4 && a64_is_apple()) {
			ext = t->is_signed ? tgExt_Sign : tgExt_Zero;
		}
		a64_add_piece(c, a64_int_piece_type(size), size, 0, tgLoc_Gpr, ext);
		break;
	}
	default:
		TG_PANIC("a64: not a register type");
	}
}

TG_INTERNAL void a64_hfa_pieces(a64Class *c, tgType base, i64 count) {
	i64 i;
	i32 size = tg_type_size(base);
	c->block = true;
	c->block_align = size;
	for (i = 0; i < count; i++) {
		a64_add_piece(c, base, size, (i32)(i*size), tgLoc_Xmm, tgExt_None);
	}
}

// An aggregate of up to 16 bytes that is not a float aggregate: an i64, [2 x i64], or an
// i128 when it is 16 byte aligned.
TG_INTERNAL void a64_int_coerced_pieces(a64Class *c, tyg_type const *t) {
	i64 size = t->size;
	if (size == 0) {
		c->kind = tgArg_Ignore;
		return;
	}
	if (size <= 8) {
		// an i64 to clang, so its stack slot is 8 bytes even on Apple, unlike a narrow scalar's
		a64_add_piece(c, a64_int_piece_type(size), (i32)size, 0, tgLoc_Gpr, tgExt_None);
		c->block = true;
		c->block_align = 8;
		c->stack_size = 8;
		return;
	}
	a64_add_piece(c, tgType_I64, 8, 0, tgLoc_Gpr, tgExt_None);
	a64_add_piece(c, tgType_I64, (i32)(size - 8), 8, tgLoc_Gpr, tgExt_None);
	if (t->align >= 16) {
		c->i128_pair = true;
		return;
	}
	c->block = true;
	c->block_align = 8;
	c->stack_size = 8; // [2 x i64] takes 16 bytes of stack
}

// clang's classifyArgumentType
TG_INTERNAL a64Class a64_classify_arg(tyg_type const *t) {
	a64Class c;
	tgType base = tgType_None;
	i64 count = 0;
	memset(&c, 0, sizeof(c));
	c.kind = tgArg_Direct;
	if (tg_t_is_scalar(t)) {
		a64_register_pieces(&c, t);
	} else if (a64_hfa(t, &base, &count)) {
		a64_hfa_pieces(&c, base, count);
	} else if (t->size <= 16) {
		a64_int_coerced_pieces(&c, t);
	} else {
		c.kind = tgArg_Indirect;
	}
	return c;
}

// clang's classifyReturnType
TG_INTERNAL a64Class a64_classify_ret(tyg_type const *t) {
	a64Class c;
	tgType base = tgType_None;
	i64 count = 0;
	memset(&c, 0, sizeof(c));
	c.kind = tgArg_Direct;
	if (t->kind == tgT_Void) {
		c.kind = tgArg_Ignore;
	} else if (tg_t_is_scalar(t)) {
		// Apple extends a short result too, in the callee
		a64_register_pieces(&c, t);
	} else if (a64_hfa(t, &base, &count)) {
		a64_hfa_pieces(&c, base, count);
	} else if (t->size > 16) {
		c.kind = tgArg_Indirect;
	} else {
		a64_int_coerced_pieces(&c, t);
	}
	return c;
}

////////////////////////////////////////////////////////////////
// Building the function description
////////////////////////////////////////////////////////////////

typedef struct a64Assigner {
	i32 ngrn;  // next x register
	i32 nsrn;  // next v register
	i32 nsaa;  // next stack offset
} a64Assigner;

// The stack slot of a scalar of `size` bytes, which is also its alignment
TG_INTERNAL i32 a64_slot_size(i32 size) {
	if (!a64_is_apple()) return size <= 8 ? 8 : 16;
	return size <= 1 ? 1 : size <= 2 ? 2 : size <= 4 ? 4 : size <= 8 ? 8 : 16;
}

// `bytes`: the stack the piece takes, 0 for its slot; a member of a block after its first
// follows the previous member directly. (`packed` was a default argument.)
TG_INTERNAL void a64_assign_stack(a64Assigner *s, tgAbiPiece *p, i32 align, i32 bytes) {
	p->loc = tgLoc_Stack;
	s->nsaa = (i32)tg_align_up(s->nsaa, align);
	p->stack_offset = s->nsaa;
	s->nsaa += bytes > 0 ? bytes : a64_slot_size(p->size);
}

TG_INTERNAL void a64_assign_pieces(tgAbiFunc *f, a64Assigner *s, a64Class const *c) {
	isize first = f->pieces.count;
	tgAbiPiece *ps;
	isize n = c->count;
	isize i;
	tg_array_add_elems(&f->pieces, c->pieces, n);
	ps = f->pieces.data + first;

	if (c->i128_pair) {
		if (!a64_is_apple()) s->ngrn += s->ngrn & 1;
		if (s->ngrn + 2 <= 8) {
			ps[0].reg = (u8)s->ngrn++;
			ps[1].reg = (u8)s->ngrn++;
		} else {
			s->ngrn = 8;
			a64_assign_stack(s, &ps[0], 16, 8);
			a64_assign_stack(s, &ps[1], 8, 8);
		}
		return;
	}
	if (c->block) {
		bool gpr = ps[0].loc == tgLoc_Gpr;
		i32 *next = gpr ? &s->ngrn : &s->nsrn;
		if (*next + n <= 8) {
			for (i = 0; i < n; i++) ps[i].reg = (u8)(*next)++;
		} else {
			// the members are packed after the first, which Linux aligns to at least 8
			i32 align = a64_is_apple() ? (i32)c->block_align : tg_max((i32)c->block_align, 8);
			*next = 8;
			for (i = 0; i < n; i++) {
				a64_assign_stack(s, &ps[i], i == 0 ? align : 1, c->stack_size > 0 ? c->stack_size : ps[i].size);
			}
		}
		return;
	}
	for (i = 0; i < n; i++) {
		tgAbiPiece *p = &ps[i];
		i32 *next = p->loc == tgLoc_Gpr ? &s->ngrn : &s->nsrn;
		if (*next < 8) {
			p->reg = (u8)(*next)++;
		} else {
			a64_assign_stack(s, p, a64_slot_size(p->size), 0);
		}
	}
}

TG_INTERNAL void a64_add_pointer_arg(tgAbiFunc *f, a64Assigner *s, tgAbiArg *arg) {
	a64Class c;
	memset(&c, 0, sizeof(c));
	a64_add_piece(&c, tgType_I64, 8, 0, tgLoc_Gpr, tgExt_None);
	arg->piece_index = (i32)f->pieces.count;
	arg->piece_count = 1;
	a64_assign_pieces(f, s, &c);
}

// The arm64 description of a call with the fixed parameters of sig followed by `extra`,
// which are assigned like fixed parameters, allocated in `arena`.
TG_INTERNAL tgAbiFunc *a64_abi_compute(tyg_sig const *sig, tyg_type const *const *extra, i32 extra_count,
                                       tgArena *arena, char const **reason) {
	tgAbiFunc *f;
	a64Class ret;
	a64Assigner s;
	i32 total = sig->count + extra_count;
	i32 i;

	f = (tgAbiFunc *)tg_arena_alloc(arena, (isize)sizeof(tgAbiFunc), 16);
	f->c_vararg = (sig->flags & TYG_SIG_VARIADIC) != 0;
	f->pieces.arena = arena;
	f->params.arena = arena;
	tg_array_reserve(&f->pieces, 2*total + 5);
	tg_array_reserve(&f->params, total);

	// results first: a result returned through x8 takes no argument register
	f->ret_type = sig->ret->kind == tgT_Void ? NULL : sig->ret;
	ret = a64_classify_ret(sig->ret);
	if (ret.kind == tgArg_Indirect) {
		tgAbiPiece p;
		f->has_sret = true;
		f->sret.kind = tgArg_Indirect;
		f->sret.type = sig->ret;
		f->sret.piece_index = (i32)f->pieces.count;
		f->sret.piece_count = 1;
		memset(&p, 0, sizeof(p));
		p.type = tgType_I64;
		p.size = 8;
		p.loc = tgLoc_Gpr;
		p.reg = A64_SRET_REG;
		tg_array_add(&f->pieces, p);
		f->ret.kind = tgArg_Indirect;
		f->ret.type = sig->ret;
	} else if (ret.kind == tgArg_Direct) {
		f->ret.kind = tgArg_Direct;
		f->ret.type = sig->ret;
	} else {
		f->ret.kind = tgArg_Ignore;
		f->ret.type = f->ret_type;
	}

	memset(&s, 0, sizeof(s));
	for (i = 0; i < total; i++) {
		tyg_type const *t = i < sig->count ? sig->params[i] : extra[i - sig->count];
		tgAbiArg arg;
		a64Class c;
		memset(&arg, 0, sizeof(arg));
		if (t == NULL || t->kind == tgT_Void) {
			*reason = "abi param";
			return NULL;
		}
		c = a64_classify_arg(t);
		arg.type = t;
		arg.kind = c.kind;
		if (c.kind == tgArg_Direct && c.count == 0) arg.kind = tgArg_Ignore;
		switch (arg.kind) {
		case tgArg_Ignore:
			break;
		case tgArg_Indirect:
			a64_add_pointer_arg(f, &s, &arg);
			break;
		case tgArg_Direct:
			arg.piece_index = (i32)f->pieces.count;
			arg.piece_count = c.count;
			a64_assign_pieces(f, &s, &c);
			break;
		default:
			*reason = "abi param";
			return NULL;
		}
		tg_array_add(&f->params, arg);
	}
	f->gpr_count = s.ngrn;
	f->xmm_count = s.nsrn;
	f->stack_size = (i32)tg_align_up(s.nsaa, 8);

	if (f->ret.kind == tgArg_Direct) {
		i32 gpr = 0;
		i32 fpr = 0;
		f->ret.piece_index = (i32)f->pieces.count;
		f->ret.piece_count = ret.count;
		for (i = 0; i < ret.count; i++) {
			tgAbiPiece p = ret.pieces[i];
			p.reg = (u8)(p.loc == tgLoc_Gpr ? gpr++ : fpr++);
			tg_array_add(&f->pieces, p);
		}
	}
	return f;
}
