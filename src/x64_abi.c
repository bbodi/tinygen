// System V x86-64 calling convention, matching what clang produces for C.
//
// Ported from x64_abi.cpp, which classified its LLVM-lowered types (a port of
// lbAbiAmd64SysV) and then assigned registers like LLVM's CC_X86_64_C. Here the
// classifier works on tyg_type directly, following clang's X86_64ABIInfo::classify
// for C; the piece and register assignment machinery is the original's.
//
// tg_abi_compute dispatches arm64 to a64_abi.c and Win64 to x64_win64.c.

// Defined by x64_win64.c, which comes later in the unity build.
TG_INTERNAL tgAbiFunc *tg_abi_compute_win64(tyg_sig const *sig, tyg_type const *const *extra, i32 extra_count,
                                            tgArena *arena, char const **reason);

////////////////////////////////////////////////////////////////
// Classification (clang's X86_64ABIInfo::classify, without vectors)
////////////////////////////////////////////////////////////////

typedef u8 tgRegClass;
enum {
	tgRegClass_NoClass,
	tgRegClass_Int,
	tgRegClass_SSE,
	tgRegClass_Memory,
	tgRegClass_X87,   // the low eightbyte of a long double
	tgRegClass_X87Up, // its high eightbyte
};

// The classes of the (at most two) eightbytes of a value, and how far into each
// eightbyte its data reaches, which sizes the register pieces.
typedef struct tgSysVClass {
	tgRegClass cls[2];
	i32        data_end[2]; // bytes from the eightbyte's start
	i32        words;
} tgSysVClass;

// The merge of the classes of two fields in the same eightbyte (psABI 3.2.3, 4.)
TG_INTERNAL void tg_sysv_unify(tgSysVClass *c, i64 word, tgRegClass newv) {
	tgRegClass oldv;
	TG_ASSERT(0 <= word && word < c->words);
	oldv = c->cls[word];
	if (oldv == newv || newv == tgRegClass_NoClass) return;
	if (oldv == tgRegClass_NoClass) {
		c->cls[word] = newv;
	} else if (oldv == tgRegClass_Memory || newv == tgRegClass_Memory) {
		c->cls[word] = tgRegClass_Memory;
	} else if (oldv == tgRegClass_Int || newv == tgRegClass_Int) {
		c->cls[word] = tgRegClass_Int;
	} else if (oldv == tgRegClass_X87 || oldv == tgRegClass_X87Up || newv == tgRegClass_X87 || newv == tgRegClass_X87Up) {
		c->cls[word] = tgRegClass_Memory;
	} else {
		c->cls[word] = tgRegClass_SSE;
	}
}

TG_INTERNAL void tg_sysv_data(tgSysVClass *c, i64 off, i64 size) {
	i64 word = off / 8;
	i32 end = (i32)(off + size - word*8);
	if (c->data_end[word] < end) c->data_end[word] = end;
}

// Classifies t at byte offset `off` of the value being passed.
TG_INTERNAL void tg_sysv_classify_with(tyg_type const *t, tgSysVClass *c, i64 off) {
	i64 i;
	if (t->size == 0) {
		// an empty record or a zero length array takes no part
		return;
	}
	if (off % t->align != 0) {
		// an unaligned field makes the whole value MEMORY
		i64 e = (off + t->size + 7) / 8;
		for (i = off / 8; i < e; i++) {
			tg_sysv_unify(c, i, tgRegClass_Memory);
		}
		return;
	}
	switch (t->kind) {
	case tgT_Int:
	case tgT_Ptr:
		// aligned, so it never crosses an eightbyte
		tg_sysv_unify(c, off/8, tgRegClass_Int);
		tg_sysv_data(c, off, t->size);
		break;
	case tgT_F32:
	case tgT_F64:
		tg_sysv_unify(c, off/8, tgRegClass_SSE);
		tg_sysv_data(c, off, t->size);
		break;
	case tgT_F80:
		tg_sysv_unify(c, off/8, tgRegClass_X87);
		tg_sysv_unify(c, off/8 + 1, tgRegClass_X87Up);
		break;
	case tgT_Array: {
		i64 elem_sz = t->elem->size;
		for (i = 0; i < t->count; i++) {
			tg_sysv_classify_with(t->elem, c, off + i*elem_sz);
		}
		break;
	}
	case tgT_Record:
		// overlapping fields, a union, merge like consecutive ones
		for (i = 0; i < t->field_count; i++) {
			tg_sysv_classify_with(t->fields[i].type, c, off + t->fields[i].offset);
		}
		break;
	default:
		TG_PANIC("unhandled type");
	}
}

// Returns false when the value is MEMORY. An all NoClass result is passed as nothing.
// An argument with an x87 eightbyte is MEMORY too; `x87_ret` says whether a result of
// type t is a long double returned in st(0): the classes X87 and X87UP and nothing else.
TG_INTERNAL bool tg_sysv_classify(tyg_type const *t, tgSysVClass *c, bool *x87_ret) {
	i32 i;
	memset(c, 0, sizeof(*c));
	*x87_ret = false;
	if (t->size > 16) {
		// larger than two eightbytes and not a vector: MEMORY
		return false;
	}
	c->words = (i32)((t->size + 7) / 8);
	tg_sysv_classify_with(t, c, 0);
	// post merger cleanup: if one eightbyte is MEMORY, all are
	for (i = 0; i < c->words; i++) {
		if (c->cls[i] == tgRegClass_Memory) return false;
	}
	for (i = 0; i < c->words; i++) {
		if (c->cls[i] == tgRegClass_X87 || c->cls[i] == tgRegClass_X87Up) {
			// psABI: X87UP not preceded by X87 is MEMORY, and an x87 argument goes in memory
			*x87_ret = c->words == 2 && c->cls[0] == tgRegClass_X87 && c->cls[1] == tgRegClass_X87Up;
			return false;
		}
	}
	return true;
}

TG_INTERNAL tgType tg_sysv_int_type(i64 bytes) {
	if (bytes <= 1) return tgType_I8;
	if (bytes <= 2) return tgType_I16;
	if (bytes <= 4) return tgType_I32;
	return tgType_I64;
}

// The register pieces of a classified aggregate, one per eightbyte that holds data,
// each sized to the data it carries, like clang's GetINTEGERTypeAtOffset and
// GetSSETypeAtOffset: struct {float a, b, c;} is <2 x float> and float.
TG_INTERNAL void tg_sysv_pieces(tgSysVClass const *c, tgAbiPieceArray *out, i32 *needed_int, i32 *needed_sse) {
	i32 i;
	*needed_int = 0;
	*needed_sse = 0;
	for (i = 0; i < c->words; i++) {
		tgAbiPiece p;
		memset(&p, 0, sizeof(p));
		p.src_offset = 8*i;
		switch (c->cls[i]) {
		case tgRegClass_NoClass:
			// just padding, it takes no register
			continue;
		case tgRegClass_Int:
			p.size = c->data_end[i];
			p.type = tg_sysv_int_type(p.size);
			p.loc  = tgLoc_Gpr;
			*needed_int += 1;
			break;
		case tgRegClass_SSE:
			if (c->data_end[i] <= 4) {
				p.type = tgType_F32;
				p.size = 4;
			} else {
				p.type = tgType_F64;
				p.size = 8;
			}
			p.loc = tgLoc_Xmm;
			*needed_sse += 1;
			break;
		default:
			TG_PANIC("unhandled class");
		}
		tg_array_add(out, p);
	}
}

// Small integers are sign or zero extended to 32 bits by the caller, and results by
// the callee, as clang does for char, short and _Bool.
TG_INTERNAL tgExtKind tg_sysv_ext_for(tyg_type const *t) {
	if (t->kind != tgT_Int || t->size >= 4) return tgExt_None;
	return t->is_signed ? tgExt_Sign : tgExt_Zero;
}

typedef struct tgSysVArgResult {
	tgArgKind       kind;
	i32             byval_align;
	tgAbiPieceArray pieces;
	i32             needed_int;
	i32             needed_sse;
	bool            is_aggregate;
	bool            x87_ret;      // ByVal as an argument, but a result comes back in st(0)
} tgSysVArgResult;

// amd64_type: how a value of type t is passed or returned, before register assignment.
TG_INTERNAL tgSysVArgResult tg_sysv_type(tyg_type const *t) {
	tgSysVArgResult res;
	tgSysVClass c;
	memset(&res, 0, sizeof(res)); // pieces on the heap, freed by the caller

	if (t->kind == tgT_Void) {
		res.kind = tgArg_Ignore;
		return res;
	}
	if (tg_t_is_scalar(t)) {
		tgAbiPiece p;
		memset(&p, 0, sizeof(p));
		p.type = tg_t_scalar(t);
		p.size = (i32)t->size;
		p.ext  = tg_sysv_ext_for(t);
		if (t->kind == tgT_F32 || t->kind == tgT_F64) {
			p.loc = tgLoc_Xmm;
			res.needed_sse = 1;
		} else {
			p.loc = tgLoc_Gpr;
			res.needed_int = 1;
		}
		res.kind = tgArg_Direct;
		tg_array_add(&res.pieces, p);
		return res;
	}

	res.is_aggregate = true;
	if (!tg_sysv_classify(t, &c, &res.x87_ret)) {
		res.kind = tgArg_ByVal;
		res.byval_align = (i32)tg_max((i64)8, t->align);
		return res;
	}
	tg_sysv_pieces(&c, &res.pieces, &res.needed_int, &res.needed_sse);
	// an empty struct, or one of only padding, is not passed at all
	res.kind = res.pieces.count == 0 ? tgArg_Ignore : tgArg_Direct;
	return res;
}

////////////////////////////////////////////////////////////////
// Building the function description
////////////////////////////////////////////////////////////////

// Mirrors LLVM's CC_X86_64_C/RetCC_X86_64_C: hands out registers and stack slots.
typedef struct tgAbiAssigner {
	i32 gpr;
	i32 xmm;
	i32 stack;
} tgAbiAssigner;

static u8 const tg_sysv_int_regs[6] = {RDI, RSI, RDX, RCX, R8, R9};

TG_INTERNAL void tg_abi_assign_piece(tgAbiAssigner *s, tgAbiPiece *p) {
	if (p->loc == tgLoc_Gpr) {
		if (s->gpr < 6) {
			p->reg = tg_sysv_int_regs[s->gpr++];
			return;
		}
		p->loc = tgLoc_Stack;
		s->stack = (i32)tg_align_up(s->stack, 8);
		p->stack_offset = s->stack;
		s->stack += 8;
	} else {
		if (s->xmm < 8) {
			p->reg = (u8)(s->xmm++);
			return;
		}
		p->loc = tgLoc_Stack;
		s->stack = (i32)tg_align_up(s->stack, 8);
		p->stack_offset = s->stack;
		s->stack += 8;
	}
}

TG_INTERNAL void tg_abi_add_pointer_arg(tgAbiFunc *f, tgAbiAssigner *s, tgAbiArg *arg) {
	tgAbiPiece p;
	memset(&p, 0, sizeof(p));
	p.type = tgType_I64;
	p.size = 8;
	p.loc = tgLoc_Gpr;
	tg_abi_assign_piece(s, &p);
	arg->piece_index = (i32)f->pieces.count;
	arg->piece_count = 1;
	tg_array_add(&f->pieces, p);
}

TG_INTERNAL bool tg_abi_assign_arg(tgAbiFunc *f, tgAbiAssigner *s, tgAbiArg *arg, tgAbiPieceArray const *pieces, i32 byval_size) {
	isize i;
	switch (arg->kind) {
	case tgArg_Ignore:
		return true;
	case tgArg_Indirect:
		tg_abi_add_pointer_arg(f, s, arg);
		return true;
	case tgArg_ByVal: {
		i32 align = tg_max(8, arg->byval_align);
		s->stack = (i32)tg_align_up(s->stack, align);
		arg->stack_offset = s->stack;
		arg->byval_size = byval_size;
		s->stack += (i32)tg_align_up(byval_size, 8);
		return true;
	}
	case tgArg_Direct:
		arg->piece_index = (i32)f->pieces.count;
		arg->piece_count = (i32)pieces->count;
		for (i = 0; i < pieces->count; i++) {
			tgAbiPiece p = pieces->data[i];
			tg_abi_assign_piece(s, &p);
			tg_array_add(&f->pieces, p);
		}
		return true;
	}
	return false;
}

TG_INTERNAL bool tg_abi_assign_ret(tgAbiFunc *f, tgAbiArg *arg, tgAbiPieceArray const *pieces) {
	isize i;
	i32 gpr = 0;
	i32 xmm = 0;
	arg->piece_index = (i32)f->pieces.count;
	arg->piece_count = (i32)pieces->count;
	for (i = 0; i < pieces->count; i++) {
		tgAbiPiece p = pieces->data[i];
		if (p.loc == tgLoc_X87) {
			// st(0) holds it, no register to hand out
		} else if (p.loc == tgLoc_Gpr) {
			if (gpr >= 2) return false;
			p.reg = gpr++ == 0 ? RAX : RDX;
		} else {
			if (xmm >= 2) return false;
			p.reg = (u8)(xmm++);
		}
		tg_array_add(&f->pieces, p);
	}
	return true;
}

// The SysV description of a call with the fixed parameters of sig followed by the
// C varargs `extra`, allocated in `arena`. Varargs are classified like any other
// argument, so xmm_count of the result is what a variadic call passes in al.
TG_INTERNAL tgAbiFunc *tg_abi_compute_sysv(tyg_sig const *sig, tyg_type const *const *extra, i32 extra_count,
                                           tgArena *arena, char const **reason) {
	tgAbiFunc *f;
	tgAbiAssigner s;
	tgSysVArgResult rr;
	i32 i;
	i32 total = sig->count + extra_count;

	f = (tgAbiFunc *)tg_arena_alloc(arena, (isize)sizeof(tgAbiFunc), 16);
	f->c_vararg = (sig->flags & TYG_SIG_VARIADIC) != 0;
	f->pieces.arena = arena;
	f->params.arena = arena;
	tg_array_reserve(&f->pieces, total + 3);
	tg_array_reserve(&f->params, total);

	// the result is classified first, as clang does: an sret pointer takes the first gpr
	f->ret_type = sig->ret->kind == tgT_Void ? NULL : sig->ret;
	rr = tg_sysv_type(sig->ret);

	memset(&s, 0, sizeof(s));
	if (rr.x87_ret) {
		// a long double result, or a record of just one: st(0)
		tgAbiPiece p;
		memset(&p, 0, sizeof(p));
		p.loc = tgLoc_X87;
		p.size = 16;
		f->ret.kind = tgArg_Direct;
		f->ret.type = sig->ret;
		rr.kind = tgArg_Direct;
		rr.pieces.count = 0;
		tg_array_add(&rr.pieces, p);
	} else if (rr.kind == tgArg_ByVal) {
		f->has_sret = true;
		f->sret.kind = tgArg_Indirect;
		f->sret.type = sig->ret;
		tg_abi_add_pointer_arg(f, &s, &f->sret);
		f->ret.kind = tgArg_Indirect;
		f->ret.type = sig->ret;
	} else if (rr.kind == tgArg_Direct) {
		f->ret.kind = tgArg_Direct;
		f->ret.type = sig->ret;
	} else {
		f->ret.kind = tgArg_Ignore;
		f->ret.type = f->ret_type;
	}

	for (i = 0; i < total; i++) {
		tyg_type const *t = i < sig->count ? sig->params[i] : extra[i - sig->count];
		tgSysVArgResult r;
		tgAbiArg arg;
		bool ok;
		memset(&arg, 0, sizeof(arg));
		if (t == NULL || t->kind == tgT_Void) {
			*reason = "abi param";
			tg_array_free(&rr.pieces);
			return NULL;
		}
		r = tg_sysv_type(t);
		if (r.kind == tgArg_Direct) {
			// clang: an aggregate that does not fit in the free registers goes
			// entirely to memory and leaves them to later arguments; a scalar
			// goes to the stack by itself
			bool fits = s.gpr + r.needed_int <= 6 && s.xmm + r.needed_sse <= 8;
			if (!fits && r.is_aggregate) {
				r.kind = tgArg_ByVal;
				r.byval_align = (i32)tg_max((i64)8, t->align);
			}
		}
		arg.kind = r.kind;
		arg.type = t;
		arg.byval_align = r.byval_align;
		ok = tg_abi_assign_arg(f, &s, &arg, &r.pieces, (i32)t->size);
		tg_array_free(&r.pieces);
		if (!ok) {
			*reason = "abi param";
			tg_array_free(&rr.pieces);
			return NULL;
		}
		tg_array_add(&f->params, arg);
	}
	f->gpr_count = s.gpr;
	f->xmm_count = s.xmm;
	f->stack_size = s.stack;

	if (f->ret.kind == tgArg_Direct) {
		if (!tg_abi_assign_ret(f, &f->ret, &rr.pieces)) {
			*reason = "abi return";
			tg_array_free(&rr.pieces);
			return NULL;
		}
	}
	tg_array_free(&rr.pieces);
	return f;
}

TG_INTERNAL bool tg_t_unsupported_ld(tyg_type const *t);

// long double fails here on the targets that do not support it, before their ABI code sees it.
TG_INTERNAL bool tg_abi_check_ld(tyg_sig const *sig, tyg_type const *const *extra, i32 extra_count, char const **reason) {
	i32 i;
	bool bad = tg_t_unsupported_ld(sig->ret);
	for (i = 0; i < sig->count && !bad; i++) bad = tg_t_unsupported_ld(sig->params[i]);
	for (i = 0; i < extra_count && !bad; i++) bad = tg_t_unsupported_ld(extra[i]);
	if (bad) *reason = "long double is not supported on this target yet";
	return !bad;
}

TG_INTERNAL bool tg_abi_check_sig(tyg_sig const *sig, char const **reason) {
	return tg_abi_check_ld(sig, NULL, 0, reason);
}

// Returns NULL when the signature uses something this backend does not support yet.
// The result lives in the module's arena.
TG_INTERNAL tgAbiFunc *tg_abi_compute(tyg_sig const *sig, char const **reason) {
	if (!tg_abi_check_sig(sig, reason)) return NULL;
	if (tg_is_arm64()) return a64_abi_compute(sig, NULL, 0, &sig->m->arena, reason);
	if (tg_is_win64()) return tg_abi_compute_win64(sig, NULL, 0, &sig->m->arena, reason);
	return tg_abi_compute_sysv(sig, NULL, 0, &sig->m->arena, reason);
}

// The description of one call of a variadic sig with the varargs of the given types,
// in the procedure's arena. params has one entry per fixed and per variadic argument.
TG_INTERNAL tgAbiFunc *tg_abi_compute_call(tyg_sig const *sig, tyg_type const *const *vararg_types, i32 vararg_count,
                                           char const **reason) {
	if (!tg_abi_check_ld(sig, vararg_types, vararg_count, reason)) return NULL;
	if (tg_is_arm64()) return a64_abi_compute(sig, vararg_types, vararg_count, tg_arena(), reason);
	if (tg_is_win64()) return tg_abi_compute_win64(sig, vararg_types, vararg_count, tg_arena(), reason);
	return tg_abi_compute_sysv(sig, vararg_types, vararg_count, tg_arena(), reason);
}

// The sig's abi, computed on first use.
TG_INTERNAL tgAbiFunc *tg_sig_abi(tyg_sig *sig, char const **reason) {
	if (!sig->abi_done) {
		char const *why = NULL;
		sig->abi = tg_abi_compute(sig, &why);
		sig->abi_reason = why;
		sig->abi_done = true;
	}
	*reason = sig->abi_reason;
	return sig->abi;
}
