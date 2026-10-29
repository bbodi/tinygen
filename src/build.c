// The public procedure builder: tyg_proc_begin ... tyg_proc_end and the calls that build a
// procedure's IR in between, up to calls. ir.c makes the instructions, args.c places the
// arguments, and call.c holds the public calls and C varargs.

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

// Typed memory, see tinygen.h. A type found through the current one extends the path,
// anything else starts a new one, whose outer type is the type itself.
static void tg_addr_type(tyg_addr *a, tyg_type const *through, tyg_type const *t, i64 off) {
	if (a->type != NULL && a->type == through) {
		a->path_offset += off;
	} else {
		a->outer = through;
		a->path_offset = off;
	}
	a->type = t;
}

tyg_addr tyg_at_type(tyg_addr a, const tyg_type *t) {
	if (t == NULL) {
		a.type = a.outer = NULL;
		return a;
	}
	if (a.type == t) return a;
	// the type at the start of the current one keeps the path: a record's first field, an
	// array's first element, else the type stands alone
	if (a.type != NULL && a.type->kind == tgT_Array && a.type->elem == t) tg_addr_type(&a, a.type, t, 0);
	else if (a.type != NULL && a.type->kind == tgT_Record) {
		i32 k = 0;
		while (k < a.type->field_count && !(a.type->fields[k].offset == 0 && a.type->fields[k].type == t)) k++;
		if (k < a.type->field_count) tg_addr_type(&a, a.type, t, 0);
		else tg_addr_type(&a, t, t, 0);
	} else tg_addr_type(&a, t, t, 0);
	return a;
}

tyg_addr tyg_at_field(tyg_addr a, const tyg_type *record, int field) {
	if (record == NULL || record->kind != tgT_Record || field < 0 || field >= record->field_count) {
		a.type = a.outer = NULL; // not a field: the access stays untyped
		return a;
	}
	a.offset += record->fields[field].offset;
	tg_addr_type(&a, record, record->fields[field].type, record->fields[field].offset);
	return a;
}

tyg_addr tyg_at_elem(tyg_addr a, const tyg_type *array, int64_t index) {
	if (array == NULL || array->kind != tgT_Array || index < 0 || index >= array->count) {
		a.type = a.outer = NULL;
		return a;
	}
	a.offset += index * array->elem->size;
	tg_addr_type(&a, array, array->elem, index * array->elem->size);
	return a;
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
		// the C names a thread local like any variable
		if ((p->m->symbols.data[a.base].flags & tgSymbolFlag_TLS) && !tg_is_c_output()) {
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
	if (a.type != NULL && tg_is_c_output()) m.tag = tg_c_tag(p->m, a.outer, a.type, a.path_offset);
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
	if (!tg_c_check_not_declared(m, sym, "tyg_proc_begin")) return NULL;
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
	p->c_va_types.arena = tg_arena();
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
	if (m->target.output == TYG_OUTPUT_C) {
		// the C compiler receives the parameters: p->abi stays NULL, and each parameter is
		// a tgOp_Param, its value or for an aggregate or a long double a pointer to it
		tg_c_sym_sig_set(m, sym, sig, true);
		if ((sig->flags & TYG_SIG_VARIADIC) && sig->count == 0) tg_fail(p, "C output: a variadic signature needs a named parameter");
		for (i = 0; i < sig->count; i++) {
			tgType st = tg_t_scalar(sig->params[i]);
			tgInstr in = tg_instr_t(tgOp_Param, st != tgType_None ? st : tgType_I64);
			in.dst = tg_new_vreg(p, in.type);
			in.imm = i;
			tg_emit(p, in);
			tg_array_add(&p->param_vals, in.dst);
		}
		return p;
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
	if (m->target.output == TYG_OUTPUT_C) {
		// the builder's IR as a C function, without the passes; on failure the symbol stays
		// undefined and m->c_procs unchanged
		tgSymbol *s = &m->symbols.data[p->sym];
		u16 flags = s->flags;
		TG_CHECK_IR_AFTER(p, "the builder");
		s->section = p->code_section != 0 ? p->code_section : tgSection_Text;
		s->flags |= tgSymbolFlag_Func;
		if (!tg_c_proc(p)) {
			s->section = tgSection_Undef;
			s->flags = flags;
			tg_set_error(m, "%.*s: %s", (int)p->name.len, p->name.text, p->fail_reason);
			tg_proc_discard(m);
			return 1;
		}
		tg_proc_discard(m);
		return 0;
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

	// The passes over the IR, in order. Each walks p->order, recounts the definitions and uses
	// it needs, and acts only on vregs with one definition. None adds a block or a local, and
	// none gives a vreg a second definition, so each pass may rely on the builder's single
	// definitions. Each turns what it removes into a Nop, which keeps positions stable.
	TG_CHECK_IR_AFTER(p, "the builder");
	// A load of a local that one store sets to a constant becomes that constant. The store
	// stays for tg_cleanup_proc.
	tg_fold_const_locals(p);
	TG_CHECK_IR_AFTER(p, "tg_fold_const_locals");
	// A read of a memcopy's destination reads its source, and a memcopy nothing reads goes.
	// It also rewrites p->params_in and the memory operands of call arguments.
	tg_forward_mem_copies(p);
	TG_CHECK_IR_AFTER(p, "tg_forward_mem_copies");
	// A load that repeats one in its block becomes a Copy of the earlier value, defined in the
	// same block, so that the next pass can drop it.
	tg_reuse_loads(p);
	TG_CHECK_IR_AFTER(p, "tg_reuse_loads");
	// Those copies and repeated pure operations go, and their readers read the value itself.
	// Without this pass the copies stay, which the lowerings handle.
	tg_forward_copies(p);
	TG_CHECK_IR_AFTER(p, "tg_forward_copies");
	// arm64: two narrow loads next to each other become one wide load. The only pass that adds
	// vregs, three per pair, each with one definition.
	tg_pair_loads(p);
	TG_CHECK_IR_AFTER(p, "tg_pair_loads");
	// Last, since it removes what the passes before leave dead: unused values and loads, and
	// stores to locals that nothing reads or that a later store overwrites.
	tg_cleanup_proc(p);
	TG_CHECK_IR_AFTER(p, "tg_cleanup_proc");
	// tg_alloc_regs sizes its facts from the final p->vregs.count
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
// compute and store doubles. The C output takes the tgOp_X87 form on every target, which it
// prints as the C compiler's long double; on arm64 Linux that is binary128, whose 16 bytes
// aligned to 16 tyg_type_scalar already gives.
////////////////////////////////////////////////////////////////

// Whether the builder may go on with a long double operation.
TG_INTERNAL bool tg_ld_ok(tgProc *p) {
	if (!tg_b_ok(p)) return false;
	if (!tg_is_c_output() && !tg_ld_supported()) {
		tg_fail(p, "long double is not supported on this target yet");
		return false;
	}
	return true;
}

// Whether long double is a double the operations load and store as such.
TG_INTERNAL bool tg_ld_is_f64(void) {
	return !tg_is_c_output() && (tg_is_arm64() || tg_is_win64());
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
	if (!tg_is_c_output() && p->abi->has_sret) {
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
			// in C mode an aggregate parameter is the C parameter itself, which its tgOp_Param
			// points at; any other pointer names nothing
			for (isize k = 0; tg_is_c_output() && where.offset == 0 && k < p->param_vals.count; k++) {
				if (p->param_vals.data[k] == (u32)where.base && tg_t_scalar(p->sig->params[k]) == tgType_None) v.c_param = (i32)k + 1;
			}
			if (v.c_param != 0) break;
			if (tg_is_c_output()) return;
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
