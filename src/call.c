// The public calls, and C varargs for each ABI: the variadic arguments of a call, and
// va_start and va_arg in a variadic procedure.


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
