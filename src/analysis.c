// The passes tyg_proc_end runs over a procedure's IR before lowering, and the walks over the
// IR that the passes, the register allocator (regalloc.c) and both lowerings share.

TG_ARRAY(tgI8Array, i8);
TG_ARRAY(tgMemArray, tgMem);

// An array of n zeroed items in the current arena.
#define tg_array_make(a, n) do { \
	memset((a), 0, sizeof(*(a))); \
	(a)->arena = tg_arena(); \
	tg_array_resize((a), (n)); \
} while (0)

// An array of n zeroed items in the current arena, with room for c.
#define tg_array_make_cap(a, n, c) do { \
	tg_array_make((a), (n)); \
	tg_array_reserve((a), (c)); \
} while (0)

// f(ctx, vreg, is_def). tg_for_each_vreg is inlined into its callers, so that each pass
// calls its f directly or inlines it: the passes visit every operand of every instruction.
typedef void (*tgVregProc)(void *ctx, u32 v, bool is_def);

TG_FORCEINLINE void tg__use_mem(tgMem const *m, tgVregProc f, void *ctx) {
	if (m->kind == tgMem_Reg) f(ctx, m->base, false);
}

TG_FORCEINLINE void tg_for_each_vreg(tgProc *p, tgInstr const *in, tgVregProc f, void *ctx) {
	switch (in->op) {
	case tgOp_Nop:
	case tgOp_Loc:
	case tgOp_Scope:
	case tgOp_Jump:
	case tgOp_Unreachable:
	case tgOp_Trap:
	case tgOp_DebugTrap:
	case tgOp_AtomicFence:
	case tgOp_CpuRelax:
		break;
	case tgOp_IConst:
	case tgOp_FConst:
	case tgOp_ReadCycleCounter:
	case tgOp_StackPointer:
	case tgOp_FrameAddress:
	case tgOp_ReturnAddress:
	case tgOp_Param:
		f(ctx, in->dst, true);
		break;
	case tgOp_Lea:
	case tgOp_Load:
	case tgOp_AtomicLoad:
		tg__use_mem(&in->mem, f, ctx);
		f(ctx, in->dst, true);
		break;
	case tgOp_Store:
	case tgOp_AtomicStore:
		f(ctx, in->a, false);
		tg__use_mem(&in->mem, f, ctx);
		break;
	case tgOp_MemZero:
	case tgOp_Prefetch:
		tg__use_mem(&in->mem, f, ctx);
		break;
	case tgOp_MemCopy:
	case tgOp_MemMove:
	case tgOp_VaCopy:
		f(ctx, in->a, false);
		f(ctx, in->b, false);
		break;
	case tgOp_MemCopyDyn:
	case tgOp_MemMoveDyn:
	case tgOp_MemSetDyn:
		f(ctx, in->a, false);
		f(ctx, in->b, false);
		f(ctx, in->c, false);
		break;
	case tgOp_Select:
	case tgOp_Fma:
		f(ctx, in->a, false);
		f(ctx, in->b, false);
		f(ctx, in->c, false);
		f(ctx, in->dst, true);
		break;
	case tgOp_Branch:
	case tgOp_VaStart:
		f(ctx, in->a, false);
		break;
	case tgOp_AtomicRmw:
		f(ctx, in->a, false);
		tg__use_mem(&in->mem, f, ctx);
		f(ctx, in->dst, true);
		break;
	case tgOp_AtomicCas:
		f(ctx, in->a, false);
		f(ctx, in->b, false);
		tg__use_mem(&in->mem, f, ctx);
		f(ctx, in->dst, true);
		f(ctx, in->c, true);
		break;
	case tgOp_Cpuid:
	case tgOp_Xgetbv:
		f(ctx, in->a, false);
		f(ctx, in->b, false);
		tg__use_mem(&in->mem, f, ctx);
		break;
	case tgOp_Vec128:
		if (in->a) f(ctx, in->a, false);
		if (in->b) f(ctx, in->b, false);
		if (in->c) f(ctx, in->c, false);
		tg__use_mem(&in->mem, f, ctx);
		break;
	case tgOp_Valgrind:
		f(ctx, in->a, false);
		f(ctx, in->b, false);
		f(ctx, in->dst, true);
		break;
	case tgOp_X87:
		if (in->a) f(ctx, in->a, false);
		if (in->b) f(ctx, in->b, false);
		if (in->c) f(ctx, in->c, false);
		if (in->dst) f(ctx, in->dst, true);
		break;
	case tgOp_TlsAddr:
		f(ctx, in->dst, true);
		break;
	case tgOp_Alloca:
		f(ctx, in->a, false);
		f(ctx, in->dst, true);
		break;
	case tgOp_StackRestore:
		f(ctx, in->a, false);
		break;
	case tgOp_MulOvf:
		f(ctx, in->a, false);
		f(ctx, in->b, false);
		f(ctx, in->dst, true);
		f(ctx, in->c, true);
		break;
	case tgOp_Call:
	case tgOp_Ret:
	case tgOp_Syscall: {
		tgCall const *c = &p->calls.data[(isize)in->imm];
		if (c->target_vreg) f(ctx, c->target_vreg, false);
		for (isize i = 0; i < c->args.count; i++) {
			tgCallArg const *arg = &c->args.data[i];
			switch (arg->kind) {
			case tgCallArg_Gpr:
			case tgCallArg_Xmm:
			case tgCallArg_Stack:
				f(ctx, arg->vreg, false);
				break;
			default:
				tg__use_mem(&arg->mem, f, ctx);
				break;
			}
		}
		for (isize i = 0; i < c->rets.count; i++) {
			tg__use_mem(&c->rets.data[i].dst, f, ctx);
		}
		if (in->op == tgOp_Syscall || c->result_vreg != 0) f(ctx, c->result_vreg, true);
		break;
	}
	default:
		// unary and binary ops
		f(ctx, in->a, false);
		if (in->op >= tgOp_Add && in->op <= tgOp_FDiv) f(ctx, in->b, false);
		if (in->op == tgOp_MulHiU) f(ctx, in->b, false);
		if (in->op == tgOp_ICmp || in->op == tgOp_FCmp) f(ctx, in->b, false);
		f(ctx, in->dst, true);
		break;
	}
}

// Whether the op only computes its result, so it can be skipped when nothing reads it.
// Loads and divisions stay, they can fault.
TG_INTERNAL bool tg_op_is_pure(tgOp op) {
	switch (op) {
	case tgOp_IConst:
	case tgOp_FConst:
	case tgOp_Lea:
	case tgOp_Copy:
	case tgOp_Add:
	case tgOp_Sub:
	case tgOp_Mul:
	case tgOp_And:
	case tgOp_Or:
	case tgOp_Xor:
	case tgOp_Shl:
	case tgOp_LShr:
	case tgOp_AShr:
	case tgOp_FAdd:
	case tgOp_FSub:
	case tgOp_FMul:
	case tgOp_FDiv:
	case tgOp_Neg:
	case tgOp_Not:
	case tgOp_FNeg:
	case tgOp_ICmp:
	case tgOp_FCmp:
	case tgOp_Zext:
	case tgOp_Sext:
	case tgOp_Trunc:
	case tgOp_SIToF:
	case tgOp_UIToF:
	case tgOp_FToSI:
	case tgOp_FToUI:
	case tgOp_FExt:
	case tgOp_FTrunc:
	case tgOp_Bitcast:
	case tgOp_Select:
	case tgOp_Fma:
	case tgOp_Param:
		return true;
	}
	return false;
}

// The scalar locals that can live in a register, heaviest first. A local qualifies when
// every access is a plain whole-value load or store, all int or all float, so nothing can see
// its memory. `is_float` tells which locals hold a float.
// Each access weighs `loop_weight` inside a loop and `other_weight` elsewhere, and a
// local needs more than `min_weight`.
typedef struct tgRankedLocal { i32 weight; i32 index; } tgRankedLocal;
TG_ARRAY(tgRankedLocalArray, tgRankedLocal);

// heavier first, then in order
TG_INTERNAL int tg_ranked_local_cmp(void const *a, void const *b) {
	tgRankedLocal const *x = (tgRankedLocal const *)a;
	tgRankedLocal const *y = (tgRankedLocal const *)b;
	if (x->weight != y->weight) return x->weight > y->weight ? -1 : 1;
	return x->index < y->index ? -1 : x->index > y->index;
}

// With `call_mem`, a call may read a local as a whole register argument, and a call with one
// result may write it to one. With `by_ref_reg`, a local can hold a debug variable's address.
TG_INTERNAL bool tg_call_mem_is_local(tgProc *p, tgMem const *m, i32 size) {
	return m->kind == tgMem_Local && m->offset == 0 && p->locals.data[m->base].size == size;
}

// Per block in p->order, whether it is in a loop: a later block jumps back to or before it.
TG_INTERNAL void tg_blocks_in_loops(tgProc *p, tgBoolArray *in_loop) {
	isize bc = p->order.count;
	tg_array_make(in_loop, bc);
	tgIsizeArray pos;
	tg_array_make(&pos, p->blocks.count);
	// the loops starting at each block minus those ending before it, summed up below
	tgI32Array diff;
	tg_array_make(&diff, bc + 1);
	for (isize i = 0; i < bc; i++) pos.data[p->order.data[i]->index] = i;
	for (isize i = 0; i < bc; i++) {
		tgBlock *b = p->order.data[i];
		for (isize ii = 0; ii < b->instrs.count; ii++) {
			tgInstr const *in = &b->instrs.data[ii];
			i32 targets[2] = {-1, -1};
			if (in->op == tgOp_Jump) targets[0] = (i32)in->imm;
			if (in->op == tgOp_Branch) { targets[0] = (i32)in->imm; targets[1] = (i32)in->c; }
			for (i32 ti = 0; ti < 2; ti++) {
				i32 t = targets[ti];
				if (t < 0 || !p->blocks.data[t]->placed || pos.data[t] > i) continue;
				diff.data[pos.data[t]] += 1;
				diff.data[i+1] -= 1;
			}
		}
	}
	i32 loops = 0;
	for (isize i = 0; i < bc; i++) {
		loops += diff.data[i];
		in_loop->data[i] = loops > 0;
	}
	tg_array_free(&pos);
	tg_array_free(&diff);
}

TG_INTERNAL void tg__rank_bad(tgBoolArray *ok, tgMem const *m) {
	if (m->kind == tgMem_Local) ok->data[m->base] = false;
}

// kind: 1: read and written as an int, 2: as a float
TG_INTERNAL void tg__rank_access(tgBoolArray *ok, tgBytes *kind, i32 l, bool fp) {
	u8 k = fp ? 2 : 1;
	if (kind->data[l] != 0 && kind->data[l] != k) ok->data[l] = false;
	kind->data[l] = k;
}

// call_mem and by_ref_reg were default arguments, false.
TG_INTERNAL void tg_rank_promotable_locals(tgProc *p, i32 loop_weight, i32 other_weight, i32 min_weight, tgI32Array *ranked, tgBoolArray *is_float, bool call_mem, bool by_ref_reg) {
	tg_array_make_cap(ranked, 0, 8);
	isize n = p->locals.count;
	tg_array_make(is_float, n);
	if (n == 0) return;
	// after a second return from setjmp the locals must have the values they had at the
	// longjmp, as they do in memory; a callee saved register would be rolled back
	for (isize i = 0; i < p->calls.count; i++) {
		if (p->calls.data[i].returns_twice) return;
	}

	tgBoolArray ok;
	tgI32Array weight;
	tgBytes kind; // 1: read and written as an int, 2: as a float
	tg_array_make(&ok, n);
	tg_array_make(&weight, n);
	tg_array_make(&kind, n);
	for (isize i = 0; i < n; i++) {
		tgLocal const *l = &p->locals.data[i];
		ok.data[i] = l->over_align <= 16 && (l->size == 1 || l->size == 2 || l->size == 4 || l->size == 8);
	}

	isize bc = p->order.count;
	tgBoolArray in_loop;
	tg_blocks_in_loops(p, &in_loop);

	for (isize bi = 0; bi < bc; bi++) {
		tgBlock *blk = p->order.data[bi];
		for (isize ii = 0; ii < blk->instrs.count; ii++) {
			tgInstr const *in = &blk->instrs.data[ii];
			switch (in->op) {
			case tgOp_Load:
			case tgOp_Store: {
				if (in->mem.kind != tgMem_Local) break;
				tgLocal const *l = &p->locals.data[in->mem.base];
				bool fp = tg_type_is_float(in->type);
				bool plain = in->mem.offset == 0 && (tg_type_is_int(in->type) || (fp && l->size >= 4)) && tg_type_size(in->type) == l->size &&
				             !(in->flags & tgInstrFlag_Volatile);
				if (!plain) ok.data[in->mem.base] = false;
				tg__rank_access(&ok, &kind, (i32)in->mem.base, fp);
				weight.data[in->mem.base] += in_loop.data[bi] ? loop_weight : other_weight;
				break;
			}
			case tgOp_Lea:
			case tgOp_AtomicLoad:
			case tgOp_AtomicStore:
			case tgOp_MemZero:
			case tgOp_Prefetch:
			case tgOp_AtomicRmw:
			case tgOp_AtomicCas:
			case tgOp_Cpuid:
			case tgOp_Xgetbv:
			case tgOp_Vec128:
				tg__rank_bad(&ok, &in->mem);
				break;
			case tgOp_Call:
			case tgOp_Ret:
			case tgOp_Syscall: {
				tgCall const *c = &p->calls.data[(isize)in->imm];
				for (isize ai = 0; ai < c->args.count; ai++) {
					tgCallArg const *arg = &c->args.data[ai];
					if (arg->kind == tgCallArg_Gpr || arg->kind == tgCallArg_Xmm || arg->kind == tgCallArg_Stack) continue;
					if (call_mem && arg->kind == tgCallArg_GprMem && tg_call_mem_is_local(p, &arg->mem, arg->size)) {
						tg__rank_access(&ok, &kind, (i32)arg->mem.base, false);
						weight.data[arg->mem.base] += in_loop.data[bi] ? loop_weight : other_weight;
						continue;
					}
					tg__rank_bad(&ok, &arg->mem);
				}
				for (isize ri = 0; ri < c->rets.count; ri++) {
					tgCallRet const *r = &c->rets.data[ri];
					if (call_mem && in->op == tgOp_Call && c->rets.count == 1 && r->loc == tgLoc_Gpr && tg_call_mem_is_local(p, &r->dst, r->size)) {
						tg__rank_access(&ok, &kind, (i32)r->dst.base, false);
						weight.data[r->dst.base] += in_loop.data[bi] ? loop_weight : other_weight;
						continue;
					}
					tg__rank_bad(&ok, &r->dst);
				}
				break;
			}
			}
		}
	}
	for (isize i = 0; i < p->params_in.count; i++) {
		tgParamIn const *in = &p->params_in.data[i];
		if (in->dst.kind != tgMem_Local) continue;
		bool fp = in->loc == tgLoc_Xmm || (in->loc == tgLoc_Stack && tg_type_is_float(in->type));
		bool plain = (in->loc == tgLoc_Gpr || in->loc == tgLoc_Xmm || (in->loc == tgLoc_Stack && in->type != tgType_V128)) &&
		             in->dst.offset == 0 && in->size == p->locals.data[in->dst.base].size && (!fp || in->size >= 4);
		if (!plain) ok.data[in->dst.base] = false;
		tg__rank_access(&ok, &kind, (i32)in->dst.base, fp);
	}
	for (isize i = 0; i < p->debug_vars.count; i++) {
		tgDebugVar const *v = &p->debug_vars.data[i];
		if (v->local >= 0 && v->by_ref && !(by_ref_reg && v->frame_offset_fixup == 0)) ok.data[v->local] = false;
	}

	tgRankedLocalArray order;
	tg_array_make_cap(&order, 0, n);
	for (isize i = 0; i < n; i++) {
		is_float->data[i] = kind.data[i] == 2;
		if (!ok.data[i] || weight.data[i] <= min_weight) continue;
		tgRankedLocal r;
		r.weight = weight.data[i];
		r.index = (i32)i;
		tg_array_add(&order, r);
	}
	if (order.count <= 16) {
		// procedures have few promotable locals, too few for qsort's calls to pay off
		for (isize i = 1; i < order.count; i++) {
			tgRankedLocal r = order.data[i];
			isize j = i;
			for (; j > 0 && tg_ranked_local_cmp(&r, &order.data[j-1]) < 0; j--) order.data[j] = order.data[j-1];
			order.data[j] = r;
		}
	} else {
		tg_sort(order.data, order.count, tg_ranked_local_cmp);
	}
	for (isize i = 0; i < order.count; i++) tg_array_add(ranked, order.data[i].index);
	tg_array_free(&ok);
	tg_array_free(&weight);
	tg_array_free(&kind);
	tg_array_free(&in_loop);
	tg_array_free(&order);
}

// Removes what the builder leaves unused: pure values and frame loads that nothing reads,
// stores to frame locals that nothing reads, and stores that a later store in the same block
// overwrites before anything can read them. A debugger may look at a debug variable's local at
// any time, so its stores stay.
typedef struct tgCleanupAt { i32 block; i32 instr; } tgCleanupAt;
typedef struct tgCleanupCover { i32 local; u64 bytes; } tgCleanupCover;
TG_ARRAY(tgCleanupAtArray, tgCleanupAt);
TG_ARRAY(tgCleanupCoverArray, tgCleanupCover);

// The state the helpers of tg_cleanup_proc share.
typedef struct tgCleanup {
	tgProc *         p;
	tgI32Array       uses;
	tgCleanupAtArray def_at;
	tgI32Array       reads;  // the instructions that may read the local
	tgU32Array       work;
	i32              bi, ii; // the instruction being counted
} tgCleanup;

TG_INTERNAL i32 tg__cleanup_local_of(tgMem const *m) {
	return m->kind == tgMem_Local ? (i32)m->base : -1;
}

TG_INTERNAL void tg__cleanup_count(void *ctx, u32 v, bool is_def) {
	tgCleanup *C = (tgCleanup *)ctx;
	if (v == 0) return;
	if (is_def) {
		tgCleanupAt at;
		at.block = C->bi;
		at.instr = C->ii;
		C->def_at.data[v] = at;
	} else {
		C->uses.data[v]++;
	}
}

TG_INTERNAL void tg__cleanup_unuse(void *ctx, u32 v, bool is_def) {
	tgCleanup *C = (tgCleanup *)ctx;
	if (v != 0 && !is_def && --C->uses.data[v] == 0) tg_array_add(&C->work, v);
}

TG_INTERNAL void tg__cleanup_kill(tgCleanup *C, tgInstr *in) {
	tg_for_each_vreg(C->p, in, tg__cleanup_unuse, C);
	if (in->op == tgOp_Load) {
		i32 l = tg__cleanup_local_of(&in->mem);
		if (l >= 0) C->reads.data[l]--;
	}
	*in = tg_instr(tgOp_Nop);
}

TG_INTERNAL bool tg__cleanup_dead_value(tgCleanup *C, tgInstr const *in) {
	if (in->dst == 0 || C->uses.data[in->dst] != 0) return false;
	if (tg_op_is_pure(in->op)) return true;
	if (in->op != tgOp_Load || (in->flags & tgInstrFlag_Volatile)) return false;
	if (in->mem.kind == tgMem_Local || in->mem.kind == tgMem_Incoming) return true;
	// a symbol that resolves is readable: a front end's entry block loads global addresses that
	// some paths never use (an undefined weak symbol may resolve to 0, so its load stays)
	return in->mem.kind == tgMem_Sym && !(C->p->m->symbols.data[in->mem.base].flags & (tgSymbolFlag_Weak | tgSymbolFlag_TLS));
}

TG_INTERNAL void tg__cleanup_drain(tgCleanup *C) {
	while (C->work.count > 0) {
		u32 v = tg_array_pop(&C->work);
		tgCleanupAt at = C->def_at.data[v];
		tgInstr *in = &C->p->order.data[at.block]->instrs.data[at.instr];
		if (in->dst == v && tg__cleanup_dead_value(C, in)) tg__cleanup_kill(C, in);
	}
}

TG_INTERNAL void tg_cleanup_proc(tgProc *p) {
	isize vn = p->vregs.count;
	isize ln = p->locals.count;
	isize bc = p->order.count;
	tgCleanup C;
	memset(&C, 0, sizeof(C));
	C.p = p;
	tg_array_make(&C.uses, vn);
	tg_array_make(&C.def_at, vn);
	tgBoolArray keep;  // a debug variable's local
	tg_array_make(&keep, ln);
	tg_array_make(&C.reads, ln);
	tgCleanupAtArray stores; // stores to locals that may be dead
	tg_array_make_cap(&stores, 0, 64);
	tgBoolArray has_store;
	tg_array_make(&has_store, bc);
	tg_array_make_cap(&C.work, 0, 64);
	for (isize i = 0; i < p->debug_vars.count; i++) {
		tgDebugVar const *v = &p->debug_vars.data[i];
		if (v->local >= 0) keep.data[v->local] = true;
	}
	for (isize bi = 0; bi < bc; bi++) {
		tgBlock *b = p->order.data[bi];
		for (isize ii = 0; ii < b->instrs.count; ii++) {
			tgInstr const *in = &b->instrs.data[ii];
			C.bi = (i32)bi;
			C.ii = (i32)ii;
			tg_for_each_vreg(p, in, tg__cleanup_count, &C);
			switch (in->op) {
			case tgOp_Store:
			case tgOp_MemZero: {
				i32 l = tg__cleanup_local_of(&in->mem);
				if (l >= 0 && !keep.data[l] && !(in->flags & tgInstrFlag_Volatile)) {
					tgCleanupAt at;
					at.block = (i32)bi;
					at.instr = (i32)ii;
					tg_array_add(&stores, at);
					has_store.data[bi] = true;
				}
				break;
			}
			case tgOp_Call:
			case tgOp_Ret:
			case tgOp_Syscall: {
				tgCall const *c = &p->calls.data[(isize)in->imm];
				for (isize ai = 0; ai < c->args.count; ai++) {
					tgCallArg const *arg = &c->args.data[ai];
					if (arg->kind != tgCallArg_Gpr && arg->kind != tgCallArg_Xmm && arg->kind != tgCallArg_Stack) {
						i32 l = tg__cleanup_local_of(&arg->mem);
						if (l >= 0) C.reads.data[l]++;
					}
				}
				break;
			}
			default: {
				i32 l = tg__cleanup_local_of(&in->mem);
				if (l >= 0) C.reads.data[l]++;
				break;
			}
			}
		}
	}

	for (u32 v = 1; v < vn; v++) {
		if (C.uses.data[v] == 0) tg_array_add(&C.work, v);
	}
	tg__cleanup_drain(&C);

	// the locals that nothing reads any more
	for (isize si = 0; si < stores.count; si++) {
		tgCleanupAt at = stores.data[si];
		tgInstr *in = &p->order.data[at.block]->instrs.data[at.instr];
		if (in->op != tgOp_Store && in->op != tgOp_MemZero) continue;
		if (C.reads.data[tg__cleanup_local_of(&in->mem)] == 0) tg__cleanup_kill(&C, in);
	}
	tg__cleanup_drain(&C);

	// Stores that a later one in the block overwrites first. Going back, `covered` holds the
	// bytes the stores after this point write, below offset 64, of the locals nothing read since.
	tgCleanupCoverArray covered;
	tg_array_make_cap(&covered, 0, 16);
	for (isize bi = 0; bi < bc; bi++) {
		if (!has_store.data[bi]) continue;
		tgBlock *b = p->order.data[bi];
		covered.count = 0;
		for (isize ii = b->instrs.count-1; ii >= 0; ii--) {
			tgInstr *in = &b->instrs.data[ii];
			switch (in->op) {
			case tgOp_Nop:
			case tgOp_Loc:
			case tgOp_Scope:
			case tgOp_Jump:
			case tgOp_Branch:
			case tgOp_Lea:
				continue;
			case tgOp_Store:
			case tgOp_MemZero: {
				i32 l = tg__cleanup_local_of(&in->mem);
				i64 size = in->op == tgOp_Store ? tg_type_size(in->type) : in->imm;
				if (l < 0 || keep.data[l] || (in->flags & tgInstrFlag_Volatile) || in->mem.offset < 0 || in->mem.offset + size > 64) continue;
				u64 bytes = (size >= 64 ? ~0ull : ((1ull << size) - 1)) << in->mem.offset;
				isize k = 0;
				while (k < covered.count && covered.data[k].local != l) k++;
				if (k == covered.count) {
					tgCleanupCover c;
					c.local = l;
					c.bytes = 0;
					tg_array_add(&covered, c);
				}
				if ((covered.data[k].bytes & bytes) == bytes) {
					tg__cleanup_kill(&C, in);
				} else {
					covered.data[k].bytes |= bytes;
				}
				continue;
			}
			case tgOp_Load: {
				i32 l = tg__cleanup_local_of(&in->mem);
				if (l >= 0) {
					for (isize k = 0; k < covered.count; k++) {
						if (covered.data[k].local == l) covered.data[k].bytes = 0;
					}
					continue;
				}
				covered.count = 0; // through a pointer, it may read any local whose address is known
				continue;
			}
			}
			if (tg_op_is_pure(in->op)) continue;
			covered.count = 0;
		}
	}
	tg__cleanup_drain(&C);
	tg_array_free(&covered);
	tg_array_free(&C.uses);
	tg_array_free(&C.def_at);
	tg_array_free(&keep);
	tg_array_free(&C.reads);
	tg_array_free(&stores);
	tg_array_free(&has_store);
	tg_array_free(&C.work);
}

// A local that is no debug variable, whose address nothing takes, and that one store sets to a
// constant, holds that constant wherever it is read: its loads become the constant, which the
// lowerings put in an instruction's immediate or make at its use. A counted loop's bound
// (`for i in 3`) then takes no register. Read before the store, it held nothing defined anyway.
TG_INTERNAL void tg_fold_const_locals(tgProc *p) {
	isize n = p->locals.count;
	if (n == 0) return;
	tgBoolArray bad;
	tgI32Array stores;
	tgU32Array value;
	tg_array_make(&bad, n);
	tg_array_make(&stores, n);
	tg_array_make(&value, n);
	for (isize i = 0; i < p->params_in.count; i++) {
		tgParamIn const *in = &p->params_in.data[i];
		if (in->dst.kind == tgMem_Local) bad.data[in->dst.base] = true;
	}
	for (isize i = 0; i < p->debug_vars.count; i++) {
		if (p->debug_vars.data[i].local >= 0) bad.data[p->debug_vars.data[i].local] = true;
	}
	bool any = false;
	for (isize bi = 0; bi < p->order.count; bi++) {
		tgBlock *b = p->order.data[bi];
		for (isize ii = 0; ii < b->instrs.count; ii++) {
			tgInstr const *in = &b->instrs.data[ii];
			if (in->op == tgOp_Call || in->op == tgOp_Ret || in->op == tgOp_Syscall) {
				tgCall const *c = &p->calls.data[(isize)in->imm];
				for (isize ai = 0; ai < c->args.count; ai++) {
					tgCallArg const *arg = &c->args.data[ai];
					if (arg->kind != tgCallArg_Gpr && arg->kind != tgCallArg_Xmm && arg->kind != tgCallArg_Stack && arg->mem.kind == tgMem_Local) bad.data[arg->mem.base] = true;
				}
				for (isize ri = 0; ri < c->rets.count; ri++) {
					if (c->rets.data[ri].dst.kind == tgMem_Local) bad.data[c->rets.data[ri].dst.base] = true;
				}
				continue;
			}
			if (in->mem.kind != tgMem_Local) continue;
			u32 l = in->mem.base;
			bool plain = (in->op == tgOp_Load || in->op == tgOp_Store) && in->mem.offset == 0 && tg_type_is_int(in->type) &&
			             tg_type_size(in->type) == p->locals.data[l].size && !(in->flags & tgInstrFlag_Volatile);
			if (!plain) {
				bad.data[l] = true;
			} else if (in->op == tgOp_Store) {
				stores.data[l]++;
				value.data[l] = in->a;
				any = true;
			}
		}
	}
	if (!any) goto done;
	for (isize bi = 0; bi < p->order.count; bi++) {
		tgBlock *b = p->order.data[bi];
		for (isize ii = 0; ii < b->instrs.count; ii++) {
			tgInstr *in = &b->instrs.data[ii];
			if (in->op != tgOp_Load || in->mem.kind != tgMem_Local) continue;
			u32 l = in->mem.base;
			u32 v = value.data[l];
			if (bad.data[l] || stores.data[l] != 1 || v == 0 || (isize)v >= p->vinfo.count || !p->vinfo.data[v].is_k ||
			    p->vregs.data[v] != in->type || p->vregs.data[in->dst] != in->type) continue;
			u32 dst = in->dst;
			tgType t = in->type;
			*in = tg_instr(tgOp_IConst);
			in->type = t;
			in->dst = dst;
			in->imm = p->vinfo.data[v].k;
		}
	}
done:
	tg_array_free(&bad);
	tg_array_free(&stores);
	tg_array_free(&value);
}

// A load that repeats an earlier one in its block, from the same address with nothing between
// that may write memory, becomes a copy of the earlier one's value: a loop body that reads a slice
// header before each element read loads it once. An address is the same when its base vreg is a
// copy made here of the earlier base. A whole local whose address nothing takes is only written by
// stores that name it, so a store to it forgets only what was read from it, and a later load of
// the whole local becomes a copy of the stored value. Only vregs defined once take part, so no
// value changes under the table; the earlier load already ran, so the copy cannot hide a fault.
// tg_forward_copies then lets the copies' readers read the values themselves.
typedef struct tgSeenLoad { tgMem mem; tgType type; u32 dst; } tgSeenLoad;
TG_ARRAY(tgSeenLoadArray, tgSeenLoad);

typedef struct tgReuseCount { tgI32Array defs, def_block; i32 bi; } tgReuseCount;

TG_INTERNAL void tg__reuse_count(void *ctx, u32 v, bool is_def) {
	tgReuseCount *c = (tgReuseCount *)ctx;
	if (v == 0 || !is_def) return;
	c->defs.data[v]++;
	c->def_block.data[v] = c->bi;
}

TG_INTERNAL void tg_reuse_loads(tgProc *p) {
	isize vn = p->vregs.count;
	tgReuseCount rc;
	tg_array_make(&rc.defs, vn);
	tg_array_make(&rc.def_block, vn);
	tgI32Array defs = rc.defs;
	tgBoolArray escaped; // a local whose address is taken: a pointer or a call may write it
	tg_array_make(&escaped, p->locals.count);
	for (isize bi = 0; bi < p->order.count; bi++) {
		tgBlock *b = p->order.data[bi];
		rc.bi = (i32)bi;
		for (isize ii = 0; ii < b->instrs.count; ii++) {
			tgInstr const *in = &b->instrs.data[ii];
			tg_for_each_vreg(p, in, tg__reuse_count, &rc);
			if (in->op != tgOp_Load && in->op != tgOp_Store && in->mem.kind == tgMem_Local) escaped.data[in->mem.base] = true;
		}
	}
	tgU32Array same; // vreg -> the vreg it copies, from a load made a copy, else itself
	tg_array_make(&same, vn);
	for (isize v = 0; v < vn; v++) same.data[v] = (u32)v;
	tgSeenLoadArray seen;
	tg_array_make_cap(&seen, 0, 16);
	for (isize bi = 0; bi < p->order.count; bi++) {
		tgBlock *b = p->order.data[bi];
		seen.count = 0;
		for (isize ii = 0; ii < b->instrs.count; ii++) {
			tgInstr *in = &b->instrs.data[ii];
			if (in->op == tgOp_Load) {
				tgMem m = in->mem;
				if (m.kind == tgMem_Reg) m.base = same.data[m.base];
				bool ok = !(in->flags & tgInstrFlag_Volatile) && in->type != tgType_V128 &&
				          defs.data[in->dst] == 1 && (m.kind != tgMem_Reg || defs.data[m.base] <= 1);
				if (!ok) continue;
				isize k = 0;
				while (k < seen.count && !(seen.data[k].mem.kind == m.kind && seen.data[k].mem.base == m.base &&
				                           seen.data[k].mem.offset == m.offset && seen.data[k].type == in->type)) k++;
				if (k < seen.count) {
					u32 dst = in->dst;
					tgType t = in->type;
					*in = tg_instr(tgOp_Copy);
					in->type = t;
					in->dst = dst;
					in->a = seen.data[k].dst;
					same.data[dst] = seen.data[k].dst;
				} else if (seen.count < 64) {
					tgSeenLoad sl;
					sl.mem = m;
					sl.type = in->type;
					sl.dst = in->dst;
					tg_array_add(&seen, sl);
				}
				continue;
			}
			if (in->op == tgOp_Store && in->mem.kind == tgMem_Local && !escaped.data[in->mem.base] && !(in->flags & tgInstrFlag_Volatile)) {
				// forget what was read from this local, then remember the value now in it
				isize n = 0;
				for (isize k = 0; k < seen.count; k++) {
					tgSeenLoad const *e = &seen.data[k];
					if (!(e->mem.kind == tgMem_Local && e->mem.base == in->mem.base)) seen.data[n++] = *e;
				}
				seen.count = n;
				tgLocal const *l = &p->locals.data[in->mem.base];
				// a value made in this block, so that tg_forward_copies can drop the copy
				if (in->mem.offset == 0 && tg_type_size(in->type) == l->size && in->type != tgType_V128 && defs.data[in->a] == 1 &&
				    rc.def_block.data[in->a] == (i32)bi &&
				    p->vregs.data[in->a] == in->type && seen.count < 64) {
					tgSeenLoad sl;
					sl.mem = in->mem;
					sl.type = in->type;
					sl.dst = in->a;
					tg_array_add(&seen, sl);
				}
				continue;
			}
			if (in->op == tgOp_Store && in->mem.kind == tgMem_Reg && !(in->flags & tgInstrFlag_Volatile) && in->type != tgType_V128 &&
			    defs.data[same.data[in->mem.base]] <= 1) {
				// A pointer may reach any memory but a local whose address nothing takes. The same
				// pointer at another offset reaches other bytes, so what was read there stays, and a
				// later load of what this stores becomes a copy of the stored value.
				tgMem m = in->mem;
				m.base = same.data[m.base];
				i32 size = tg_type_size(in->type);
				isize n = 0;
				for (isize k = 0; k < seen.count; k++) {
					tgSeenLoad const *e = &seen.data[k];
					bool apart = e->mem.kind == tgMem_Reg && e->mem.base == m.base &&
					             (e->mem.offset + tg_type_size(e->type) <= m.offset || m.offset + size <= e->mem.offset);
					if ((e->mem.kind == tgMem_Local && !escaped.data[e->mem.base]) || apart) seen.data[n++] = *e;
				}
				seen.count = n;
				if (defs.data[in->a] == 1 && p->vregs.data[in->a] == in->type && seen.count < 64) {
					tgSeenLoad sl;
					sl.mem = m;
					sl.type = in->type;
					sl.dst = in->a;
					tg_array_add(&seen, sl);
				}
				continue;
			}
			switch (in->op) {
			case tgOp_Nop:
			case tgOp_Loc:
			case tgOp_Scope:
			case tgOp_Jump:
			case tgOp_Branch:
			case tgOp_Prefetch:
				continue;
			}
			if (tg_op_is_pure(in->op)) continue;
			seen.count = 0; // it may write memory
		}
	}
	tg_array_free(&seen);
	tg_array_free(&same);
	tg_array_free(&rc.defs);
	tg_array_free(&rc.def_block);
	tg_array_free(&escaped);
}

// A memcopy into a frame local T makes T a copy of its source S until either is written. When
// nothing but memcopies takes T's address, T's reads in the rest of the block read S instead: a
// load or a call's argument at T + k reads S + k, and a copy out of the whole of T copies out of
// S. A copy into T that is no debug variable and that nothing reads any more then goes away. A
// struct the front end copies to a temporary before it reads its fields or passes it on moves once
// or not at all.
// S is a local whose address only memcopies take, which only the instructions naming it write, or
// memory through a pointer or a symbol, which any write to memory but one to such a local may
// change. A debugger may write a debug variable, or memory, at a line, so past a line marker only
// copies between two locals that are no debug variables stay.
typedef struct tgCopyFact {
	u32   t;     // the local holding the copy
	i32   toff;  // where in it the copy starts
	i32   n;     // its size
	tgMem src;   // the source, by its own memory operand
	u32   sbase; // the copy's source pointer
	bool  plain; // the source is a local that only instructions naming it write
} tgCopyFact;

typedef struct tgCopyCount { tgI32Array defs, uses; } tgCopyCount;

TG_INTERNAL void tg__copy_count(void *ctx, u32 v, bool is_def) {
	tgCopyCount *c = (tgCopyCount *)ctx;
	if (v == 0) return;
	if (is_def) c->defs.data[v]++;
	else        c->uses.data[v]++;
}

// The copy that covers bytes [off, off + size) of local t, or -1.
TG_INTERNAL i32 tg__copy_find(tgCopyFact const *f, i32 nf, u32 t, i64 off, i64 size) {
	for (i32 k = nf - 1; k >= 0; k--) {
		if (f[k].t == t && off >= f[k].toff && off + size <= (i64)f[k].toff + f[k].n) return k;
	}
	return -1;
}

// Drops the copies a write to local l, or with l < 0 to memory past a pointer, may change.
TG_INTERNAL i32 tg__copy_kill(tgCopyFact *f, i32 nf, i64 l) {
	i32 n = 0;
	for (i32 k = 0; k < nf; k++) {
		bool dead = l >= 0 ? (f[k].t == (u32)l || (f[k].src.kind == tgMem_Local && f[k].src.base == (u32)l)) : !f[k].plain;
		if (!dead) f[n++] = f[k];
	}
	return n;
}

// A parameter whose prologue store lands in a local that only one copy in the entry block reads,
// copying it whole to another local before anything writes that one, lands there right away. A
// front end that stores an aggregate parameter's registers to a slot and copies the slot to the
// parameter's debug variable then stores it once.
TG_INTERNAL void tg__params_in_place(tgProc *p, tgI32Array const *lea_local, tgI32Array const *lea_off, tgBoolArray const *escaped,
                                     tgBoolArray const *dbg, tgI32Array *reads, tgI32Array const *writes) {
	if (p->params_in.count == 0 || p->order.count == 0) return;
	tgBlock *b = p->order.data[0];
	// the entry block runs once
	for (isize bi = 0; bi < p->order.count; bi++) {
		tgBlock const *x = p->order.data[bi];
		for (isize ii = 0; ii < x->instrs.count; ii++) {
			tgInstr const *in = &x->instrs.data[ii];
			if ((in->op == tgOp_Jump && in->imm == b->index) || (in->op == tgOp_Branch && (in->imm == b->index || in->c == (u32)b->index))) return;
		}
	}
	for (isize ii = 0; ii < b->instrs.count; ii++) {
		tgInstr *in = &b->instrs.data[ii];
		if (in->op != tgOp_MemCopy && in->op != tgOp_MemMove) continue;
		i32 t = lea_local->data[in->a], s = lea_local->data[in->b];
		if (t < 0 || s < 0 || s == t || escaped->data[s] || escaped->data[t] || dbg->data[s] || reads->data[s] != 1 || writes->data[s] != 0 ||
		    (in->flags & tgInstrFlag_Volatile)) continue;
		i64 soff = lea_off->data[in->b], toff = lea_off->data[in->a], n = in->imm;
		// every piece of s arrives in the copied bytes, and none of t arrives
		bool ok = true, some = false;
		for (isize i = 0; i < p->params_in.count && ok; i++) {
			tgMem const *d = &p->params_in.data[i].dst;
			if (d->kind != tgMem_Local) continue;
			if (d->base == (u32)t) ok = false;
			if (d->base != (u32)s) continue;
			some = true;
			ok = d->offset >= soff && d->offset + p->params_in.data[i].size <= soff + n;
		}
		if (!ok || !some) continue;
		// nothing before the copy writes t, which would now overwrite the parameter
		for (isize k = 0; k < ii && ok; k++) {
			tgInstr const *e = &b->instrs.data[k];
			switch (e->op) {
			case tgOp_Store:
			case tgOp_MemZero:
				ok = !(e->mem.kind == tgMem_Local && e->mem.base == (u32)t);
				break;
			case tgOp_MemCopy:
			case tgOp_MemMove:
				ok = lea_local->data[e->a] != t;
				break;
			case tgOp_Call:
			case tgOp_Syscall: {
				tgCall const *c = &p->calls.data[(isize)e->imm];
				for (isize ri = 0; ri < c->rets.count; ri++) {
					if (c->rets.data[ri].dst.kind == tgMem_Local && c->rets.data[ri].dst.base == (u32)t) ok = false;
				}
				break;
			}
			}
		}
		if (!ok) continue;
		for (isize i = 0; i < p->params_in.count; i++) {
			tgMem *d = &p->params_in.data[i].dst;
			if (d->kind == tgMem_Local && d->base == (u32)s) *d = tg_mem_offset(tg_mem(tgMem_Local, (u32)t, (i32)toff), d->offset - soff);
		}
		reads->data[s]--;
		*in = tg_instr(tgOp_Nop);
	}
}

TG_INTERNAL void tg_forward_mem_copies(tgProc *p) {
	isize vn = p->vregs.count;
	isize ln = p->locals.count;
	if (ln == 0) return;
	tgCopyCount cc;
	tg_array_make(&cc.defs, vn);
	tg_array_make(&cc.uses, vn);
	tgI32Array copy_uses; // a vreg's reads as a memcopy's pointer
	tg_array_make(&copy_uses, vn);
	tgI32Array lea_local; // a vreg that one lea makes of a local: the local, else -1
	tg_array_make(&lea_local, vn);
	tgI32Array lea_off;
	tg_array_make(&lea_off, vn);
	tgBoolArray escaped;  // a local whose address something but a memcopy takes
	tg_array_make(&escaped, ln);
	tgBoolArray dbg;
	tg_array_make(&dbg, ln);
	tgI32Array reads;
	tg_array_make(&reads, ln);
	tgI32Array writes;    // besides the prologue's
	tg_array_make(&writes, ln);
	tgMemArray lea_mem;   // what a lea makes, kind None when it is no lea
	tg_array_make(&lea_mem, vn);
	tgBoolArray fwd;      // a local some read of which now reads the copy's source
	tg_array_make(&fwd, ln);
	bool any = false;
	for (isize v = 0; v < vn; v++) lea_local.data[v] = -1;
	for (isize i = 0; i < ln; i++) {
		if (p->locals.data[i].over_align > 16) escaped.data[i] = true;
	}
	for (isize i = 0; i < p->debug_vars.count; i++) {
		tgDebugVar const *v = &p->debug_vars.data[i];
		if (v->local < 0) continue;
		dbg.data[v->local] = true;
		if (v->by_ref) escaped.data[v->local] = true;
	}
	for (isize bi = 0; bi < p->order.count; bi++) {
		tgBlock *b = p->order.data[bi];
		for (isize ii = 0; ii < b->instrs.count; ii++) {
			tgInstr const *in = &b->instrs.data[ii];
			tg_for_each_vreg(p, in, tg__copy_count, &cc);
			switch (in->op) {
			case tgOp_MemCopy:
			case tgOp_MemMove:
				copy_uses.data[in->a]++;
				copy_uses.data[in->b]++;
				any = true;
				break;
			case tgOp_Lea:
				lea_mem.data[in->dst] = in->mem;
				if (in->mem.kind == tgMem_Local) {
					lea_local.data[in->dst] = (i32)in->mem.base;
					lea_off.data[in->dst] = in->mem.offset;
				}
				break;
			case tgOp_Load:
			case tgOp_Store:
			case tgOp_MemZero:
				if (in->mem.kind == tgMem_Local && (in->flags & tgInstrFlag_Volatile)) escaped.data[in->mem.base] = true;
				if (in->op == tgOp_Load && in->mem.kind == tgMem_Local) reads.data[in->mem.base]++;
				if (in->op != tgOp_Load && in->mem.kind == tgMem_Local) writes.data[in->mem.base]++;
				break;
			case tgOp_Call:
			case tgOp_Ret:
			case tgOp_Syscall: {
				tgCall const *c = &p->calls.data[(isize)in->imm];
				for (isize ai = 0; ai < c->args.count; ai++) {
					tgCallArg const *arg = &c->args.data[ai];
					if (arg->kind == tgCallArg_Gpr || arg->kind == tgCallArg_Xmm || arg->kind == tgCallArg_Stack) continue;
					if (arg->mem.kind != tgMem_Local) continue;
					if (arg->kind == tgCallArg_GprMem || arg->kind == tgCallArg_XmmMem || arg->kind == tgCallArg_StackMem) {
						reads.data[arg->mem.base]++;
					} else {
						escaped.data[arg->mem.base] = true;
					}
				}
				for (isize ri = 0; ri < c->rets.count; ri++) {
					if (c->rets.data[ri].dst.kind == tgMem_Local) writes.data[c->rets.data[ri].dst.base]++;
				}
				break;
			}
			default:
				if (in->mem.kind == tgMem_Local) escaped.data[in->mem.base] = true;
				break;
			}
		}
	}
	if (!any) goto done;
	for (isize v = 1; v < vn; v++) {
		i32 l = lea_local.data[v];
		if (l < 0) continue;
		if (cc.defs.data[v] != 1 || cc.uses.data[v] != copy_uses.data[v]) {
			escaped.data[l] = true;
			lea_local.data[v] = -1;
		}
	}
	// a copy out of a local reads it, a copy into one writes it
	for (isize bi = 0; bi < p->order.count; bi++) {
		tgBlock *b = p->order.data[bi];
		for (isize ii = 0; ii < b->instrs.count; ii++) {
			tgInstr const *in = &b->instrs.data[ii];
			if (in->op != tgOp_MemCopy && in->op != tgOp_MemMove) continue;
			if (lea_local.data[in->b] >= 0) reads.data[lea_local.data[in->b]]++;
			if (lea_local.data[in->a] >= 0) writes.data[lea_local.data[in->a]]++;
		}
	}
	tg__params_in_place(p, &lea_local, &lea_off, &escaped, &dbg, &reads, &writes);

	tgCopyFact facts[16];
	bool forwarded = false;
	for (isize bi = 0; bi < p->order.count; bi++) {
		tgBlock *b = p->order.data[bi];
		i32 nf = 0;
		for (isize ii = 0; ii < b->instrs.count; ii++) {
			tgInstr *in = &b->instrs.data[ii];
			if (nf == 0 && in->op != tgOp_MemCopy && in->op != tgOp_MemMove) continue;
			switch (in->op) {
			case tgOp_Loc: {
				i32 n = 0;
				for (i32 k = 0; k < nf; k++) {
					tgCopyFact const *f = &facts[k];
					if (!dbg.data[f->t] && f->plain && !dbg.data[f->src.base]) facts[n++] = *f;
				}
				nf = n;
				continue;
			}
			case tgOp_Load: {
				if (in->mem.kind != tgMem_Local || (in->flags & tgInstrFlag_Volatile)) continue;
				i32 k = tg__copy_find(facts, nf, in->mem.base, in->mem.offset, tg_type_size(in->type));
				if (k < 0) continue;
				reads.data[in->mem.base]--;
				fwd.data[in->mem.base] = true;
				in->mem = tg_mem_offset(facts[k].src, in->mem.offset - facts[k].toff);
				forwarded = true;
				continue;
			}
			case tgOp_Call:
			case tgOp_Ret:
			case tgOp_Syscall: {
				tgCall *c = &p->calls.data[(isize)in->imm];
				for (isize ai = 0; ai < c->args.count; ai++) {
					tgCallArg *arg = &c->args.data[ai];
					if (arg->kind != tgCallArg_GprMem && arg->kind != tgCallArg_XmmMem && arg->kind != tgCallArg_StackMem) continue;
					if (arg->mem.kind != tgMem_Local) continue;
					i32 k = tg__copy_find(facts, nf, arg->mem.base, arg->mem.offset, arg->size);
					if (k < 0) continue;
					reads.data[arg->mem.base]--;
					fwd.data[arg->mem.base] = true;
					arg->mem = tg_mem_offset(facts[k].src, arg->mem.offset - facts[k].toff);
					forwarded = true;
				}
				if (in->op == tgOp_Ret) continue;
				for (isize ri = 0; ri < c->rets.count; ri++) {
					tgMem const *d = &c->rets.data[ri].dst;
					if (d->kind == tgMem_Local) nf = tg__copy_kill(facts, nf, d->base);
				}
				nf = tg__copy_kill(facts, nf, -1);
				continue;
			}
			case tgOp_MemCopy:
			case tgOp_MemMove: {
				i32 sl = lea_local.data[in->b];
				if (sl >= 0) {
					i32 k = tg__copy_find(facts, nf, (u32)sl, lea_off.data[in->b], in->imm);
					if (k >= 0 && lea_off.data[in->b] == facts[k].toff) {
						reads.data[sl]--;
						fwd.data[sl] = true;
						in->b = facts[k].sbase;
						forwarded = true;
					}
				}
				i32 dl = lea_local.data[in->a];
				if (dl >= 0 && escaped.data[dl]) dl = -1; // a pointer may reach it
				nf = tg__copy_kill(facts, nf, dl);
				if (dl < 0 || in->imm <= 0 || in->imm > 0x7fffffff || cc.defs.data[in->b] != 1 || (in->flags & tgInstrFlag_Volatile)) continue;
				// the source, by its own memory operand when a lea makes it
				tgCopyFact f;
				memset(&f, 0, sizeof(f));
				f.t = (u32)dl;
				f.toff = lea_off.data[in->a];
				f.n = (i32)in->imm;
				f.sbase = in->b;
				f.src = tg_mem(tgMem_Reg, in->b, 0);
				sl = lea_local.data[in->b];
				tgMem lm = lea_mem.data[in->b];
				if (sl >= 0) {
					if (sl == dl) continue;
					f.src = tg_mem(tgMem_Local, (u32)sl, lea_off.data[in->b]);
					f.plain = !escaped.data[sl];
				} else if (cc.defs.data[in->b] == 1 && (lm.kind == tgMem_Sym || (lm.kind == tgMem_Reg && cc.defs.data[lm.base] == 1))) {
					// the symbol, or the pointer and the offset, the lea adds up
					f.src = lm;
				}
				if (nf == 16) {
					memmove(facts, facts + 1, 15 * sizeof(facts[0]));
					nf--;
				}
				facts[nf++] = f;
				continue;
			}
			case tgOp_Store:
			case tgOp_MemZero:
				nf = tg__copy_kill(facts, nf, in->mem.kind == tgMem_Local && !escaped.data[in->mem.base] ? (i64)in->mem.base : -1);
				continue;
			case tgOp_MemCopyDyn:
			case tgOp_MemMoveDyn:
			case tgOp_MemSetDyn:
			case tgOp_AtomicStore:
			case tgOp_AtomicRmw:
			case tgOp_AtomicCas:
			case tgOp_AtomicFence:
			case tgOp_Cpuid:
			case tgOp_Xgetbv:
			case tgOp_Vec128:
			case tgOp_Valgrind:
			case tgOp_X87:
				nf = tg__copy_kill(facts, nf, -1);
				continue;
			}
		}
	}
	if (!forwarded) goto done;
	// the copies into temporaries that nothing reads now
	for (isize bi = 0; bi < p->order.count; bi++) {
		tgBlock *b = p->order.data[bi];
		for (isize ii = 0; ii < b->instrs.count; ii++) {
			tgInstr *in = &b->instrs.data[ii];
			if (in->op != tgOp_MemCopy && in->op != tgOp_MemMove) continue;
			i32 dl = lea_local.data[in->a];
			if (dl < 0 || escaped.data[dl] || dbg.data[dl] || reads.data[dl] != 0) continue;
			// a copy out of memory that nothing read anyway stays, it may fault
			if (!fwd.data[dl] && lea_local.data[in->b] < 0) continue;
			*in = tg_instr(tgOp_Nop);
		}
	}
done:
	tg_array_free(&cc.defs);
	tg_array_free(&cc.uses);
	tg_array_free(&copy_uses);
	tg_array_free(&lea_local);
	tg_array_free(&lea_off);
	tg_array_free(&escaped);
	tg_array_free(&dbg);
	tg_array_free(&reads);
	tg_array_free(&writes);
	tg_array_free(&lea_mem);
	tg_array_free(&fwd);
}

// Replaces each vreg that `in` reads by map[vreg]; the vregs it writes stay.
TG_INTERNAL void tg__rename_uses(tgProc *p, tgInstr *in, u32 const *map) {
#define TG_RN(x) ((x) = map[(x)])
#define TG_RN_MEM(m) do { if ((m).kind == tgMem_Reg) TG_RN((m).base); } while (0)
	switch (in->op) {
	case tgOp_Nop:
	case tgOp_Loc:
	case tgOp_Scope:
	case tgOp_Jump:
	case tgOp_Unreachable:
	case tgOp_Trap:
	case tgOp_DebugTrap:
	case tgOp_AtomicFence:
	case tgOp_CpuRelax:
	case tgOp_IConst:
	case tgOp_FConst:
	case tgOp_ReadCycleCounter:
	case tgOp_StackPointer:
	case tgOp_FrameAddress:
	case tgOp_ReturnAddress:
	case tgOp_TlsAddr:
	case tgOp_Param:
		break;
	case tgOp_Lea:
	case tgOp_Load:
	case tgOp_AtomicLoad:
	case tgOp_MemZero:
	case tgOp_Prefetch:
		TG_RN_MEM(in->mem);
		break;
	case tgOp_Store:
	case tgOp_AtomicStore:
	case tgOp_AtomicRmw:
		TG_RN(in->a);
		TG_RN_MEM(in->mem);
		break;
	case tgOp_MemCopy:
	case tgOp_MemMove:
	case tgOp_VaCopy:
	case tgOp_Valgrind:
	case tgOp_MulOvf:
		TG_RN(in->a);
		TG_RN(in->b);
		break;
	case tgOp_MemCopyDyn:
	case tgOp_MemMoveDyn:
	case tgOp_MemSetDyn:
	case tgOp_Select:
	case tgOp_Fma:
		TG_RN(in->a);
		TG_RN(in->b);
		TG_RN(in->c);
		break;
	case tgOp_Branch:
	case tgOp_Alloca:
	case tgOp_StackRestore:
	case tgOp_VaStart:
		TG_RN(in->a);
		break;
	case tgOp_AtomicCas:
	case tgOp_Cpuid:
	case tgOp_Xgetbv:
		TG_RN(in->a);
		TG_RN(in->b);
		TG_RN_MEM(in->mem);
		break;
	case tgOp_Vec128:
		TG_RN(in->a);
		TG_RN(in->b);
		TG_RN(in->c);
		TG_RN_MEM(in->mem);
		break;
	case tgOp_X87:
		TG_RN(in->a);
		TG_RN(in->b);
		TG_RN(in->c);
		break;
	case tgOp_Call:
	case tgOp_Ret:
	case tgOp_Syscall: {
		tgCall *c = &p->calls.data[(isize)in->imm];
		TG_RN(c->target_vreg);
		for (isize i = 0; i < c->args.count; i++) {
			tgCallArg *arg = &c->args.data[i];
			switch (arg->kind) {
			case tgCallArg_Gpr:
			case tgCallArg_Xmm:
			case tgCallArg_Stack:
				TG_RN(arg->vreg);
				break;
			default:
				TG_RN_MEM(arg->mem);
				break;
			}
		}
		for (isize i = 0; i < c->rets.count; i++) TG_RN_MEM(c->rets.data[i].dst);
		break;
	}
	default:
		// unary and binary ops
		TG_RN(in->a);
		if ((in->op >= tgOp_Add && in->op <= tgOp_FDiv) || in->op == tgOp_MulHiU || in->op == tgOp_ICmp || in->op == tgOp_FCmp) TG_RN(in->b);
		break;
	}
#undef TG_RN
#undef TG_RN_MEM
}

// A copy of a vreg defined earlier in the same block goes away: its readers read the source.
// Both must be defined once, and nothing in the block may read the copy before it is made (a
// value from the previous trip around a loop), so the source holds the copy's value wherever the
// copy is read. tg_reuse_loads leaves such copies, one per repeated load.
typedef struct tgForward {
	tgI32Array defs;
	tgI32Array def_block;
	tgI32Array def_at;
	tgBytes    early;    // read in its block before it is defined
	i32        bi, ii;
} tgForward;

// The ops a block may compute once for repeats with the same operands: pure, and unable to trap.
TG_INTERNAL bool tg__cse_op(tgOp op) {
	switch (op) {
	case tgOp_Add: case tgOp_Sub: case tgOp_Mul:
	case tgOp_And: case tgOp_Or: case tgOp_Xor:
	case tgOp_Shl: case tgOp_LShr: case tgOp_AShr:
	case tgOp_Neg: case tgOp_Not:
	case tgOp_Zext: case tgOp_Sext: case tgOp_Trunc:
	case tgOp_ICmp:
		return true;
	}
	return false;
}

TG_INTERNAL bool tg__cse_two(tgOp op) {
	return op != tgOp_Neg && op != tgOp_Not && op != tgOp_Zext && op != tgOp_Sext && op != tgOp_Trunc;
}

typedef struct tgCseEntry { tgOp op; tgType type; u32 aux; u32 a, b, dst; } tgCseEntry;

// A vreg that one instruction defines, or a constant: it holds one value wherever it is read.
TG_INTERNAL bool tg__cse_operand(tgProc *p, tgForward const *F, u32 v) {
	return v != 0 && (F->defs.data[v] == 1 || ((isize)v < p->vinfo.count && p->vinfo.data[v].is_k));
}

// the same vreg, or two of one type holding the same constant
TG_INTERNAL bool tg__same_value(tgProc *p, u32 x, u32 y) {
	if (x == y) return true;
	if (x == 0 || y == 0 || (isize)x >= p->vinfo.count || (isize)y >= p->vinfo.count) return false;
	tgVregInfo const *a = &p->vinfo.data[x], *b = &p->vinfo.data[y];
	return a->is_k && b->is_k && a->k == b->k && p->vregs.data[x] == p->vregs.data[y];
}


TG_INTERNAL void tg__forward_count(void *ctx, u32 v, bool is_def) {
	tgForward *F = (tgForward *)ctx;
	if (v == 0 || !is_def) return;
	F->defs.data[v]++;
	F->def_block.data[v] = F->bi;
	F->def_at.data[v] = F->ii;
}

TG_INTERNAL void tg__forward_early(void *ctx, u32 v, bool is_def) {
	tgForward *F = (tgForward *)ctx;
	if (v != 0 && !is_def && F->def_block.data[v] == F->bi && F->def_at.data[v] >= F->ii) F->early.data[v] = true;
}

TG_INTERNAL void tg_forward_copies(tgProc *p) {
	isize vn = p->vregs.count;
	tgForward F;
	memset(&F, 0, sizeof(F));
	tg_array_make(&F.defs, vn);
	tg_array_make(&F.def_block, vn);
	tg_array_make(&F.def_at, vn);
	tg_array_make(&F.early, vn);
	bool any = false;
	for (isize bi = 0; bi < p->order.count; bi++) {
		tgBlock *b = p->order.data[bi];
		F.bi = (i32)bi;
		for (isize ii = 0; ii < b->instrs.count; ii++) {
			tgInstr const *in = &b->instrs.data[ii];
			if (in->op == tgOp_Copy || tg__cse_op(in->op)) any = true;
			F.ii = (i32)ii;
			tg_for_each_vreg(p, in, tg__forward_count, &F);
		}
	}
	if (!any) goto done;
	for (isize bi = 0; bi < p->order.count; bi++) {
		tgBlock *b = p->order.data[bi];
		F.bi = (i32)bi;
		for (isize ii = 0; ii < b->instrs.count; ii++) {
			F.ii = (i32)ii;
			tg_for_each_vreg(p, &b->instrs.data[ii], tg__forward_early, &F);
		}
	}
	{
		tgU32Array map;
		tg_array_make(&map, vn);
		for (isize v = 0; v < vn; v++) map.data[v] = (u32)v;
		bool renamed = false;
		tgCseEntry cse[64];
		i32 ncse = 0;
		for (isize bi = 0; bi < p->order.count; bi++) {
			tgBlock *b = p->order.data[bi];
			ncse = 0;
			for (isize ii = 0; ii < b->instrs.count; ii++) {
				tgInstr *in = &b->instrs.data[ii];
				if (tg__cse_op(in->op)) {
					// an operation repeated with the same operands, after the copies are gone
					u32 v = in->dst;
					if (v == 0 || F.defs.data[v] != 1 || F.early.data[v]) continue;
					u32 a = map.data[in->a], c = tg__cse_two(in->op) ? map.data[in->b] : 0;
					if (!tg__cse_operand(p, &F, a) || (tg__cse_two(in->op) && !tg__cse_operand(p, &F, c))) continue;
					bool commutes = in->op == tgOp_Add || in->op == tgOp_Mul || in->op == tgOp_And || in->op == tgOp_Or || in->op == tgOp_Xor;
					i32 k = 0;
					for (; k < ncse; k++) {
						tgCseEntry const *e = &cse[k];
						if (e->op != in->op || e->type != in->type || e->aux != in->aux || p->vregs.data[e->dst] != p->vregs.data[v]) continue;
						if ((tg__same_value(p, e->a, a) && tg__same_value(p, e->b, c)) ||
						    (commutes && tg__same_value(p, e->a, c) && tg__same_value(p, e->b, a))) break;
					}
					if (k < ncse) {
						map.data[v] = cse[k].dst;
						*in = tg_instr(tgOp_Nop);
						renamed = true;
					} else {
						tgCseEntry e;
						e.op = in->op;
						e.type = in->type;
						e.aux = in->aux;
						e.a = a;
						e.b = c;
						e.dst = v;
						cse[ncse < 64 ? ncse++ : (ii % 64)] = e;
					}
					continue;
				}
				if (in->op != tgOp_Copy) continue;
				u32 v = in->dst, a = in->a;
				if (v == 0 || a == 0 || F.defs.data[v] != 1 || F.defs.data[a] != 1 || F.early.data[v] ||
				    F.def_block.data[a] != bi || F.def_at.data[a] >= ii ||
				    p->vregs.data[v] != p->vregs.data[a] || in->type != p->vregs.data[v]) continue;
				map.data[v] = map.data[a];
				*in = tg_instr(tgOp_Nop);
				renamed = true;
			}
		}
		if (renamed) {
			for (isize bi = 0; bi < p->order.count; bi++) {
				tgBlock *b = p->order.data[bi];
				for (isize ii = 0; ii < b->instrs.count; ii++) tg__rename_uses(p, &b->instrs.data[ii], map.data);
			}
		}
		tg_array_free(&map);
	}
done:
	tg_array_free(&F.defs);
	tg_array_free(&F.def_block);
	tg_array_free(&F.def_at);
	tg_array_free(&F.early);
}

// arm64: two loads of adjacent 1, 2 or 4 byte integers in a statement become one load of both,
// split in registers. An image kernel reads three neighbours per row, and loads, not arithmetic,
// limit how fast it runs: three loads and an add take longer than two loads, a shift, a mask and
// an add. The later load joins the earlier one when nothing between them may write memory or
// starts a line, so the debugger sees nothing move. Both bytes of the wider load are read anyway,
// so it touches no memory the two did not. Only memory through a pointer takes part: the frame's
// locals are mostly read soon after their stores, and a load spanning two stores waits for both.
TG_INTERNAL u32 tg_new_vreg(tgProc *p, tgType t);

typedef struct tgPairCount { tgI32Array defs; } tgPairCount;

TG_INTERNAL void tg__pair_count(void *ctx, u32 v, bool is_def) {
	tgPairCount *c = (tgPairCount *)ctx;
	if (v != 0 && is_def) c->defs.data[v]++;
}

TG_INTERNAL tgType tg__wider_int(tgType t) {
	switch (t) {
	case tgType_I8:  return tgType_I16;
	case tgType_I16: return tgType_I32;
	case tgType_I32: return tgType_I64;
	}
	return tgType_None;
}

TG_INTERNAL void tg_pair_loads(tgProc *p) {
	if (!tg_is_arm64()) return;
	tgPairCount pc;
	tg_array_make(&pc.defs, p->vregs.count);
	for (isize bi = 0; bi < p->order.count; bi++) {
		tgBlock *b = p->order.data[bi];
		for (isize ii = 0; ii < b->instrs.count; ii++) {
			tgInstr const *in = &b->instrs.data[ii];
			tg_for_each_vreg(p, in, tg__pair_count, &pc);
		}
	}
	isize nv = p->vregs.count; // the vregs the facts know
	tgI32Array seen;           // the loads since the last write or line, that may still pair
	tg_array_make_cap(&seen, 0, 16);
	tgI32Array partner;        // per instruction of the block: the later load joining it, or -1
	tg_array_make_cap(&partner, 0, 64);
	tgInstrArray out;
	tg_array_make_cap(&out, 0, 64);
	for (isize bi = 0; bi < p->order.count; bi++) {
		tgBlock *b = p->order.data[bi];
		isize n = b->instrs.count;
		tg_array_resize(&partner, n);
		for (isize ii = 0; ii < n; ii++) partner.data[ii] = -1;
		seen.count = 0;
		bool any = false;
		for (isize ii = 0; ii < n; ii++) {
			tgInstr const *in = &b->instrs.data[ii];
			if (in->op == tgOp_Load) {
				tgMem m = in->mem;
				tgType wt = tg__wider_int(in->type);
				bool ok = wt != tgType_None && !(in->flags & tgInstrFlag_Volatile) && in->dst != 0 && (isize)in->dst < nv &&
				          pc.defs.data[in->dst] == 1 && p->vregs.data[in->dst] == in->type &&
				          m.kind == tgMem_Reg && (isize)m.base < nv && pc.defs.data[m.base] == 1;
				if (!ok) continue;
				i32 s = tg_type_size(in->type);
				isize k = 0;
				for (; k < seen.count; k++) {
					tgInstr const *e = &b->instrs.data[seen.data[k]];
					if (partner.data[seen.data[k]] < 0 && e->type == in->type && e->mem.kind == m.kind && e->mem.base == m.base &&
					    (e->mem.offset == m.offset - s || e->mem.offset == m.offset + s)) break;
				}
				if (k < seen.count) {
					partner.data[seen.data[k]] = (i32)ii;
					seen.data[k] = seen.data[--seen.count];
					any = true;
				} else if (seen.count < 16) {
					tg_array_add(&seen, (i32)ii);
				}
				continue;
			}
			switch (in->op) {
			case tgOp_Nop:
			case tgOp_Scope:
			case tgOp_Lea:
			case tgOp_Prefetch:
				continue;
			}
			if (tg_op_is_pure(in->op)) continue;
			seen.count = 0; // it may write memory, or it starts a line
		}
		if (!any) continue;
		// the merged load where the earlier one was, the halves after it, the later one gone
		out.count = 0;
		for (isize ii = 0; ii < n; ii++) {
			tgInstr in = b->instrs.data[ii];
			i32 j = partner.data[ii];
			if (j < 0) {
				tg_array_add(&out, in);
				continue;
			}
			tgInstr later = b->instrs.data[j];
			b->instrs.data[j] = tg_instr(tgOp_Nop);
			bool first_low = in.mem.offset < later.mem.offset;
			u32 lo = first_low ? in.dst : later.dst, hi = first_low ? later.dst : in.dst;
			tgType t = in.type, wt = tg__wider_int(t);
			i32 s = tg_type_size(t);
			u32 w = tg_new_vreg(p, wt);
			u32 c = tg_new_vreg(p, wt);
			u32 sh = tg_new_vreg(p, wt);
			p->vinfo.data[c].is_k = true;
			p->vinfo.data[c].k = 8*s;
			p->vinfo.data[c].umax = (u64)(8*s);
			tgInstr x = tg_instr(tgOp_Load);
			x.type = wt;
			x.dst = w;
			x.mem = first_low ? in.mem : later.mem;
			x.mem.align = 0;
			tg_array_add(&out, x);
			x = tg_instr(tgOp_IConst);
			x.type = wt;
			x.dst = c;
			x.imm = 8*s;
			tg_array_add(&out, x);
			x = tg_instr(tgOp_LShr);
			x.type = wt;
			x.dst = sh;
			x.a = w;
			x.b = c;
			tg_array_add(&out, x);
			x = tg_instr(tgOp_Trunc);
			x.type = t;
			x.aux = wt;
			x.dst = hi;
			x.a = sh;
			tg_array_add(&out, x);
			x = tg_instr(tgOp_Trunc);
			x.type = t;
			x.aux = wt;
			x.dst = lo;
			x.a = w;
			tg_array_add(&out, x);
		}
		tg_array_resize(&b->instrs, out.count);
		memcpy(b->instrs.data, out.data, (size_t)out.count * sizeof(out.data[0]));
	}
	tg_array_free(&out);
	tg_array_free(&partner);
	tg_array_free(&seen);
	tg_array_free(&pc.defs);
}
