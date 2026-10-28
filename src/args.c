// The ABI side of calls, returns and parameters: the pieces of each argument and result
// go to their registers and stack slots, and the parameters land in locals.


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
	if (tg_is_c_output()) {
		// C output has no abi: the value, or the memory holding it, is the one argument,
		// which the IR checks see
		if (has_value) {
			tgCallArg a;
			memset(&a, 0, sizeof(a));
			a.kind = val != 0 ? tgCallArg_Gpr : tgCallArg_StackMem;
			a.type = p->vregs.data[val];
			a.vreg = val;
			a.mem = direct_result;
			tg_array_add(&args, a);
		}
	} else if (abi->has_sret && !tg_is_arm64()) {
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
