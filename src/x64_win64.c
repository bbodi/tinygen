// Windows x64: the Win64 calling convention, matching clang's WinX86_64ABIInfo for C and
// LLVM's CC_X86_Win64_C, and the Windows prologue and epilogue.
//
// The classifier works on tyg_type, which has no vectors, i128 or language specific calling
// conventions.
//
// Every argument takes one slot: the first four go in rcx, rdx, r8, r9 or
// xmm0-xmm3 by position, the rest on the stack above a 32 byte shadow area the
// caller always reserves. Aggregates of 1, 2, 4 or 8 bytes travel as integers,
// all others by pointer to a copy the caller makes.
//
// Frame:
//   [rbp+incoming_base ...]  incoming stack arguments (the shadow area first)
//   [...]                    saved rbp, rsi, rdi, promoted registers (pushed), pad
//   [rbp ...]                saved xmm registers
//   [rbp-N ...]              locals and vreg slots
//   [rsp ...]                outgoing arguments, the shadow area first
// rbp is set after the pushes and the xmm save area, so the unwinder finds every
// saved register from it even after an alloca moved rsp.
//
// A variadic procedure spills rcx, rdx, r8 and r9 to their home slots in the shadow area,
// past the fixed parameters, so the varargs lie in one array of 8 byte slots: the Win64
// va_list is a plain pointer into it.

// Defined by build.c, which comes later in the unity build.
TG_INTERNAL void tg_fail(tgProc *p, char const *reason);
TG_INTERNAL u32  tg_convop(tgProc *p, tgOp op, tgType dst, tgType src, u32 a);
TG_INTERNAL i32  tg_add_local_raw(tgProc *p, i64 size, i64 align);
TG_INTERNAL void tg_store(tgProc *p, tgType t, tgMem mem, u32 v);
TG_INTERNAL void tg_memcopy(tgProc *p, tgMem dst, tgMem src, i64 size);
TG_INTERNAL u32  tg_lea_mem(tgProc *p, tgMem mem);

static u8 const tg_win64_int_regs[4] = {RCX, RDX, R8, R9};

typedef struct tgWin64Class {
	tgArgKind  kind;
	tgAbiPiece piece;
} tgWin64Class;

TG_INTERNAL tgType tg_win64_int_type(i64 size) {
	if (size <= 1) return tgType_I8;
	if (size <= 2) return tgType_I16;
	if (size <= 4) return tgType_I32;
	return tgType_I64;
}

TG_INTERNAL void tg_win64_set(tgWin64Class *c, tgType t, i32 size, tgLocKind loc) {
	c->piece.type = t;
	c->piece.size = size;
	c->piece.loc = loc;
}

TG_INTERNAL tgWin64Class tg_win64_classify(tyg_type const *t) {
	tgWin64Class c;
	memset(&c, 0, sizeof(c));
	c.kind = tgArg_Direct;
	switch (t->kind) {
	case tgT_Void:
		c.kind = tgArg_Ignore;
		break;
	case tgT_Record:
	case tgT_Array:
		switch (t->size) {
		case 1: case 2: case 4: case 8:
			tg_win64_set(&c, tg_win64_int_type(t->size), (i32)t->size, tgLoc_Gpr);
			break;
		default:
			c.kind = tgArg_Indirect;
			break;
		}
		break;
	case tgT_Int:
		// clang extends only bool, which a tyg_type does not tell from a char
		tg_win64_set(&c, tg_win64_int_type(t->size), (i32)t->size, tgLoc_Gpr);
		break;
	case tgT_F32: tg_win64_set(&c, tgType_F32, 4, tgLoc_Xmm); break;
	case tgT_F64: tg_win64_set(&c, tgType_F64, 8, tgLoc_Xmm); break;
	case tgT_F80: tg_win64_set(&c, tgType_F64, 8, tgLoc_Xmm); break; // MSVC's long double is a double
	case tgT_Ptr: tg_win64_set(&c, tgType_I64, 8, tgLoc_Gpr); break;
	}
	return c;
}

// Hands out the positional slots.
typedef struct tgWin64Assigner {
	i32 slot;
} tgWin64Assigner;

TG_INTERNAL void tg_win64_place(tgWin64Assigner *s, tgAbiPiece *p) {
	if (s->slot < 4) {
		p->reg = p->loc == tgLoc_Xmm ? (u8)s->slot : tg_win64_int_regs[s->slot];
	} else {
		p->loc = tgLoc_Stack;
		p->stack_offset = 32 + 8*(s->slot - 4);
	}
	s->slot += 1;
}

TG_INTERNAL void tg_win64_add_pointer_arg(tgAbiFunc *f, tgWin64Assigner *s, tgAbiArg *arg) {
	tgAbiPiece p;
	memset(&p, 0, sizeof(p));
	p.type = tgType_I64;
	p.size = 8;
	p.loc = tgLoc_Gpr;
	tg_win64_place(s, &p);
	arg->piece_index = (i32)f->pieces.count;
	arg->piece_count = 1;
	tg_array_add(&f->pieces, p);
}

// The Win64 description of sig, with the C varargs `extra` classified like fixed parameters,
// allocated in `arena`. Called by tg_abi_compute in x64_abi.c.
TG_INTERNAL tgAbiFunc *tg_abi_compute_win64(tyg_sig const *sig, tyg_type const *const *extra, i32 extra_count,
                                            tgArena *arena, char const **reason) {
	i32 total = sig->count + extra_count;
	tgAbiFunc *f = (tgAbiFunc *)tg_arena_alloc(arena, (isize)sizeof(tgAbiFunc), 16);
	f->c_vararg = (sig->flags & TYG_SIG_VARIADIC) != 0;
	f->pieces.arena = arena;
	f->params.arena = arena;
	tg_array_reserve(&f->pieces, total + 2);
	tg_array_reserve(&f->params, total);

	// the result
	tgWin64Class ret;
	memset(&ret, 0, sizeof(ret));
	ret.kind = tgArg_Ignore;
	if (sig->ret != NULL && sig->ret->kind != tgT_Void) {
		ret = tg_win64_classify(sig->ret);
		f->ret_type = sig->ret;
	}
	f->ret.type = f->ret_type;

	tgWin64Assigner s;
	memset(&s, 0, sizeof(s));
	f->ret.kind = ret.kind;
	if (ret.kind == tgArg_Indirect) {
		// the caller's result memory, its address comes back in rax
		f->has_sret = true;
		f->sret.kind = tgArg_Indirect;
		f->sret.type = f->ret_type;
		tg_win64_add_pointer_arg(f, &s, &f->sret);
	}

	for (i32 i = 0; i < total; i++) {
		tyg_type const *t = i < sig->count ? sig->params[i] : extra[i - sig->count];
		if (t == NULL || t->kind == tgT_Void) {
			*reason = "win64 abi param";
			return NULL;
		}
		tgWin64Class c = tg_win64_classify(t);
		tgAbiArg arg;
		memset(&arg, 0, sizeof(arg));
		arg.kind = c.kind;
		arg.type = t;
		// C: the callee may write its parameter, so it always gets a copy
		arg.copy = c.kind == tgArg_Indirect;
		switch (c.kind) {
		case tgArg_Ignore:
			break;
		case tgArg_Indirect:
			tg_win64_add_pointer_arg(f, &s, &arg);
			break;
		case tgArg_Direct:
			arg.piece_index = (i32)f->pieces.count;
			arg.piece_count = 1;
			tg_win64_place(&s, &c.piece);
			tg_array_add(&f->pieces, c.piece);
			break;
		default:
			*reason = "win64 abi param";
			return NULL;
		}
		tg_array_add(&f->params, arg);
	}
	f->gpr_count = s.slot;
	f->xmm_count = s.slot;
	f->stack_size = 32 + 8*tg_max(s.slot - 4, 0);

	if (ret.kind == tgArg_Direct) {
		tgAbiPiece p = ret.piece;
		p.reg = p.loc == tgLoc_Gpr ? (u8)RAX : (u8)0;
		f->ret.piece_index = (i32)f->pieces.count;
		f->ret.piece_count = 1;
		tg_array_add(&f->pieces, p);
	}
	return f;
}

// The C varargs of a call, args[arg_index..arg_count), after the fixed arguments. Returns the
// stack size of the call. An aggregate goes like a fixed parameter of its type, in a register or slot when it has 1, 2, 4 or 8 bytes, else by pointer to a copy.
TG_INTERNAL i32 tg_win64_varargs(tgProc *p, tgAbiFunc *abi, tgCallArgArray *call_args,
                                 tgArgValue const *args, isize arg_index, isize arg_count) {
	i32 slot = abi->gpr_count;
	for (; arg_index < arg_count; arg_index++) {
		tgArgValue const *v = &args[arg_index];
		tgType st = tg_t_scalar(v->type);
		tgCallArg a;
		memset(&a, 0, sizeof(a));
		if (st == tgType_None || v->val == 0) {
			i64 size = v->type->size;
			if (v->type->kind == tgT_F80) {
				// a double: in xmm, which the loop below copies to the integer register
				a.type = tgType_F64;
				a.size = 8;
				a.mem = v->mem;
				if (slot < 4) {
					a.kind = tgCallArg_XmmMem;
					a.reg = (u8)slot;
				} else {
					a.kind = tgCallArg_StackMem;
					a.stack_offset = 32 + 8*(slot - 4);
				}
			} else if (size == 1 || size == 2 || size == 4 || size == 8) {
				a.type = tg_win64_int_type(size);
				a.size = (i32)size;
				a.mem = v->mem;
				if (slot < 4) {
					a.kind = tgCallArg_GprMem;
					a.reg = tg_win64_int_regs[slot];
				} else {
					a.kind = tgCallArg_StackMem;
					a.stack_offset = 32 + 8*(slot - 4);
				}
			} else {
				tgMem m = tg_mem(tgMem_Local, (u32)tg_add_local_raw(p, tg_max(size, (i64)1), tg_max(v->type->align, (i64)1)), 0);
				tg_memcopy(p, m, v->mem, size);
				a.type = tgType_I64;
				a.size = 8;
				a.vreg = tg_lea_mem(p, m);
				if (slot < 4) {
					a.kind = tgCallArg_Gpr;
					a.reg = tg_win64_int_regs[slot];
				} else {
					a.kind = tgCallArg_Stack;
					a.stack_offset = 32 + 8*(slot - 4);
				}
			}
			slot += 1;
			tg_array_add(call_args, a);
			continue;
		}
		a.type = st;
		a.size = tg_type_size(st);
		a.vreg = v->val;
		if (tg_type_is_int(st) && tg_type_size(st) < 4) {
			a.ext = v->type->is_signed ? tgExt_Sign : tgExt_Zero;
		}
		if (tg_type_is_float(st) && st == tgType_F32) {
			a.vreg = tg_convop(p, tgOp_FExt, tgType_F64, tgType_F32, a.vreg);
			a.type = tgType_F64;
			a.size = 8;
		}
		if (slot < 4) {
			if (tg_type_is_float(a.type)) {
				a.kind = tgCallArg_Xmm;
				a.reg = (u8)slot;
			} else {
				a.kind = tgCallArg_Gpr;
				a.reg = tg_win64_int_regs[slot];
			}
		} else {
			a.kind = tgCallArg_Stack;
			a.stack_offset = 32 + 8*(slot - 4);
		}
		slot += 1;
		tg_array_add(call_args, a);
	}
	// a variadic callee may read a float from either register file
	isize n = call_args->count;
	for (isize i = 0; i < n; i++) {
		tgCallArg a = call_args->data[i];
		if (a.kind != tgCallArg_Xmm && a.kind != tgCallArg_XmmMem) continue;
		if (a.reg >= 4) continue;
		a.kind = a.kind == tgCallArg_Xmm ? tgCallArg_Gpr : tgCallArg_GprMem;
		a.reg = tg_win64_int_regs[a.reg];
		if (a.kind == tgCallArg_Gpr) {
			a.type = a.type == tgType_F64 ? tgType_I64 : tgType_I32;
		}
		a.ext = tgExt_None;
		tg_array_add(call_args, a);
	}
	return 32 + 8*tg_max(slot - 4, 0);
}

// A variadic procedure: the prologue spills the argument registers past the fixed parameters
// to their home slots, where the varargs that follow on the stack continue them.
TG_INTERNAL void tg_win64_va_spill(tgProc *p, tgAbiFunc *abi) {
	for (i32 i = abi->gpr_count; i < 4; i++) {
		tgParamIn in;
		memset(&in, 0, sizeof(in));
		in.loc = tgLoc_Gpr;
		in.reg = tg_win64_int_regs[i];
		in.type = tgType_I64;
		in.size = 8;
		in.dst = tg_mem(tgMem_Incoming, 0, 8*i);
		tg_array_add(&p->params_in, in);
	}
}

////////////////////////////////////////////////////////////////
// Prologue and epilogue
////////////////////////////////////////////////////////////////

// The prologue's code offset so far, for the unwind codes.
TG_INTERNAL u8 tg_win64_here(tgLower *L) {
	i64 off = tg_pos(&L->a) - L->proc_start;
	TG_ASSERT(off < 256);
	return (u8)off;
}

TG_INTERNAL void tg_win64_prologue(tgLower *L, tgProcDebug *dbg) {
	tgAsm *a = &L->a;

	u8 pushes[8];
	i32 n = 0;
	memset(pushes, 0, sizeof(pushes));
	pushes[n++] = RSI;
	pushes[n++] = RDI;
	for (i32 i = 0; i < L->saved_count; i++) {
		pushes[n++] = L->saved[i];
	}
	bool pad = (n % 2) != 0;
	i32 area = (pad ? 8 : 0) + 16*L->saved_v_count;
	i64 total = 8*(1 + n) + area + L->frame_size;

	if (total >= 4096) {
		// touch every page from the top down before rsp moves past it, like __chkstk
		// mov r10d, pages; mov r11, rsp; 1: sub r11, 4096; test [r11], r11; dec r10d; jnz 1b
		tg_mov_r_imm(a, R10, (u64)(total / 4096));
		tg_mov_r_rm(a, 8, R11, tg_r(RSP));
		i64 loop = tg_pos(a);
		tg_alu_rm_imm(a, ALU_SUB, 8, tg_r(R11), 4096);
		tg_test_rm_r(a, 8, tg_m(R11, 0), R11);
		tg_alu_rm_imm(a, ALU_SUB, 4, tg_r(R10), 1);
		i64 j = tg_jcc32(a, CC_NE);
		tg_patch_rel32(a, j, loop);
	}

	tg_push(a, RBP);
	dbg->win_push_reg[0] = RBP;
	dbg->win_push_at[0] = tg_win64_here(L);
	dbg->win_push_count = 1;
	for (i32 i = 0; i < n; i++) {
		tg_push(a, pushes[i]);
		dbg->win_push_reg[dbg->win_push_count] = pushes[i];
		dbg->win_push_at[dbg->win_push_count] = tg_win64_here(L);
		dbg->win_push_count += 1;
	}
	dbg->win_pad_at = 0;
	dbg->win_pad_size = area;
	if (area > 0) {
		tg_alu_rm_imm(a, ALU_SUB, 8, tg_r(RSP), area);
		dbg->win_pad_at = tg_win64_here(L);
	}
	tg_mov_rm_r(a, 8, tg_r(RBP), RSP);
	dbg->win_setfp_at = tg_win64_here(L);
	dbg->win_alloc_at = 0;
	dbg->win_alloc_size = L->frame_size;
	if (L->frame_size > 0) {
		tg_enc(a, TG_W, 0x81, 5, tg_r(RSP), 4); // sub rsp, imm32
		tg_u32(a, (u32)L->frame_size);
		dbg->win_alloc_at = tg_win64_here(L);
	}
	// rbp is 16 byte aligned
	dbg->win_xmm_count = L->saved_v_count;
	for (i32 i = 0; i < L->saved_v_count; i++) {
		tg_enc(a, TG_0F, 0x29, L->saved_v[i], tg_m(RBP, 16*i), 0); // movaps [rbp + 16*i], xmm
		dbg->win_xmm_reg[i] = L->saved_v[i];
		dbg->win_xmm_at[i] = tg_win64_here(L);
	}
	L->win_area = area;
	L->incoming_base = 16 + 8*n + area;
}

TG_INTERNAL void tg_win64_epilogue(tgLower *L) {
	tgAsm *a = &L->a;
	for (i32 i = 0; i < L->saved_v_count; i++) {
		tg_enc(a, TG_0F, 0x28, L->saved_v[i], tg_m(RBP, 16*i), 0); // movaps xmm, [rbp + 16*i]
	}
	// lea rsp, [rbp + area]: an epilogue form the unwinder recognizes
	tg_lea(a, RSP, tg_m(RBP, L->win_area));
	for (i32 i = L->saved_count-1; i >= 0; i--) {
		tg_pop(a, L->saved[i]);
	}
	tg_pop(a, RDI);
	tg_pop(a, RSI);
	tg_pop(a, RBP);
	tg_ret(a);
}

// A thread local: the module's TLS block from the TEB, plus the variable's offset in .tls.
TG_INTERNAL tgOpnd tg_win64_tls_opnd(tgLower *L, tgMem m, u8 scratch) {
	tgAsm *a = &L->a;
	i32 index = tg_lower_symbol(L->p->m, L->out, tg_str_lit("_tls_index"), 0, 0);
	// mov scratch32, [rip + _tls_index]
	tg_enc(a, 0, 0x8B, scratch, tg_m_sym(index, 0, tgReloc_PC32), 0);
	tg_shift_imm(a, 4, 8, tg_r(scratch), 3);
	// add scratch, gs:[0x58] (ThreadLocalStoragePointer)
	tg_b(a, 0x65);
	tg_b(a, (u8)(0x48 | ((scratch & 8) ? 4 : 0)));
	tg_b(a, 0x03);
	tg_b(a, (u8)(0x04 | ((scratch & 7) << 3)));
	tg_b(a, 0x25);
	tg_u32(a, 0x58);
	tg_mov_r_rm(a, 8, scratch, tg_m(scratch, 0));
	// lea scratch, [scratch + secrel32(sym)]
	u8 r = scratch & 7;
	tg_b(a, (u8)(0x48 | ((scratch & 8) ? 5 : 0)));
	tg_b(a, 0x8D);
	tg_b(a, (u8)(0x80 | (r << 3) | r));
	if (r == 4) tg_b(a, 0x24); // sib for r12
	tg_asm_reloc(a, tgReloc_SecRel32, tg_pos(a), (i32)m.base, 0);
	tg_u32(a, 0);
	return tg_m(scratch, m.offset);
}

// Foreign data lives in a DLL: its address is in the import table, at __imp_<name>.
TG_INTERNAL tgOpnd tg_win64_import_opnd(tgLower *L, tgMem m, u8 scratch) {
	// the module's symbols must not change while lowering, so the append names it
	i32 imp = tg_lower_import_symbol(L->out, (i32)m.base);
	tg_enc(&L->a, TG_W, 0x8B, scratch, tg_m_sym(imp, 0, tgReloc_PC32), 0);
	return tg_m(scratch, m.offset);
}
