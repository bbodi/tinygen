// Register allocation, which the x86-64 and arm64 lowerings share.
//
// Registers come from a linear scan over intervals in the block order. A vreg that lives inside
// one block has an interval from its definition to its last use. The other vregs and the scalar
// locals whose address is never taken get one interval that covers every point where a
// liveness analysis finds them live; a local that is a debug variable keeps its register in all
// of its scope. An interval takes a caller saved register when no clobbering instruction (a
// call) happens in it, a callee saved one otherwise; a parameter stays in the register it
// arrives in or moves to a callee saved one. A constant with one definition gets no register,
// the lowering materializes it at each use.

// tgRegAlloc::reg holds general registers as themselves and float registers as TG_FREG + n
enum {
	TG_NOREG = -1,
	TG_FREG  = 32,
};

// 0x80000000 does not fit a C enum
#define TG_LOCAL_ITEM 0x80000000u

// The registers an interval may take, bit n for register n
typedef struct tgRegPools {
	u32  caller_x, callee_x; // general registers
	u32  caller_v, callee_v; // float registers
	i64  callee_weight;      // an interval lighter than this takes a callee saved register only when one is already saved
} tgRegPools;

// What tg_alloc_regs finds about a vreg in its first pass over the instructions.
typedef struct tgVregFacts {
	i64  weight;    // its spill cost: its references, each more for the loops around it
	i32  block;     // the first block it appears in, or -1
	i32  defs;      // the instructions writing it
	i32  def_pos;   // the position of the first, or -1
	i32  first_use; // the positions of the first and last read, or -1
	i32  last_use;
	i32  def_block; // the block of the latest write so far, or -1
	i8   want;      // the argument register a call reads it from, TG_NOREG, or -2 for several
	bool multi;     // it appears in more than one block
	bool cross;     // a read is in another block than the latest write before it
} tgVregFacts;

TG_ARRAY(tgVregFactsArray, tgVregFacts);

typedef struct tgRegAlloc {
	tgVregFactsArray facts;
	tgI32Array uses;      // vreg -> number of reads
	tgI8Array  reg;       // vreg -> the register holding it for its whole life, or TG_NOREG
	tgBytes    is_const;  // vreg -> defined once by an int IConst, materialized at its uses
	tgI64Array cval;
	tgBytes    in_block;  // vreg -> defined once, and only read later in the same block
	tgBytes    clean;     // vreg -> its register holds 0 or 1
	tgI8Array  local_reg; // local -> the register holding it, or TG_NOREG
	tgI32Array via;       // vreg -> the local whose register it shares, or -1
	tgBytes    remat;     // vreg -> defined once as a frame address, computed at each use from rmem
	tgMemArray rmem;
	tgI32Array slot;      // vreg -> frame offset of its 8 byte slot, when it has no register
	u32        used_x, used_v; // every register handed out
	i32        max_call_stack;
	bool       sp_moves;  // an alloca moves the stack pointer
} tgRegAlloc;

TG_INTERNAL void tg_reg_alloc_free(tgRegAlloc *R) {
	tg_array_free(&R->uses);
	tg_array_free(&R->reg);
	tg_array_free(&R->is_const);
	tg_array_free(&R->cval);
	tg_array_free(&R->in_block);
	tg_array_free(&R->clean);
	tg_array_free(&R->local_reg);
	tg_array_free(&R->via);
	tg_array_free(&R->remat);
	tg_array_free(&R->rmem);
	tg_array_free(&R->slot);
	tg_array_free(&R->facts);
}

// What the target tells tg_alloc_regs:
//   clobbers(ctx, in):              whether `in` calls something or writes caller saved registers
//   wants(ctx, p, in, note, nctx):  note(nctx, vreg, reg) for each argument `in` reads in a fixed register
//   reads_first(ctx, in):           whether `in` reads all its operands before it writes its
//                                   result, so the result may go to a register an operand leaves
typedef void (*tgNoteWantProc)(void *ctx, u32 v, i32 r);

typedef struct tgRegTarget {
	void *ctx;
	bool (*clobbers)(void *ctx, tgInstr const *in);
	void (*wants)(void *ctx, tgProc *p, tgInstr const *in, tgNoteWantProc note, void *note_ctx);
	bool (*reads_first)(void *ctx, tgInstr const *in);
} tgRegTarget;

// per block, whether an item is read there before it is written (gen) or is written (kill)
typedef struct tgAllocEvent { i32 id; i32 block; bool kill; } tgAllocEvent;
TG_ARRAY(tgAllocEventArray, tgAllocEvent);

// the vregs and locals holding a register, listed under the position after their last
typedef struct tgAllocActive { u32 v; i32 next; bool dead; i32 len; } tgAllocActive;
TG_ARRAY(tgAllocActiveArray, tgAllocActive);

// The state the helpers of tg_alloc_regs share.
typedef struct tgAllocCtx {
	tgProc *           p;
	tgRegAlloc *       R;
	tgRegPools         pools;
	tgVregFacts *      fx;
	tgI32Array         clobbers;   // prefix counts of target.clobbers
	tgBoolArray        local_fp;
	tgI8Array          arrive;     // a parameter's register
	tgBoolArray        is_param;
	u32                arrive_v;   // the float registers parameters arrive in
	tgI64Array         bweight;
	i32                linear;
	tgBlock *          b;
	isize              bi;
	tgInstr const *    in;
	tgI32Array         gid;
	tgU32Array         gvreg;      // an item is a vreg, or a local with TG_LOCAL_ITEM set
	tgI32Array         lgid;
	tgI64Array         lweight;
	tgI32Array         glo;
	tgI32Array         ghi;
	tgBoolArray        glo_def;    // the interval starts where an instruction writes it
	tgAllocEventArray  events;
	tgI32Array         ev_block;   // the block of the item's last event
	tgBytes            ev_state;   // 1: gen noted, 2: killed
	u32                free_x, free_v;
	tgAllocActiveArray active;
	i32                owner[64];  // the active entry holding each general register, and each float register at TG_FREG + n
	tgI32Array         shared;     // a local keeps its register up to the last read of a vreg sharing it, or -1
	tgI32Array         expire;
	tgI32Array         ranked;     // the promotable locals, see tg_rank_promotable_locals
	tgI32Array         gstart;     // global ids by the start of their interval
} tgAllocCtx;

TG_INTERNAL void tg__alloc_note_want(void *ctx, u32 v, i32 r) {
	tgAllocCtx *X = (tgAllocCtx *)ctx;
	tgVregFacts *fx = X->fx;
	if (v == 0) return;
	fx[v].want = fx[v].want == TG_NOREG || fx[v].want == r ? (i8)r : (i8)-2;
}

TG_INTERNAL void tg__alloc_scan(void *ctx, u32 v, bool is_def) {
	tgAllocCtx *X = (tgAllocCtx *)ctx;
	tgVregFacts *fx = X->fx;
	tgRegAlloc *R = X->R;
	tgProc *p = X->p;
	tgBlock *b = X->b;
	tgInstr const *in = X->in;
	if (v == 0) return;
	fx[v].weight += X->bweight.data[b->index];
	if (fx[v].block < 0) {
		fx[v].block = b->index;
	} else if (fx[v].block != b->index) {
		fx[v].multi = true;
	}
	if (is_def) {
		if (fx[v].defs++ == 0) fx[v].def_pos = X->linear;
		fx[v].def_block = b->index;
		if (in->op == tgOp_IConst && tg_type_is_int(p->vregs.data[v])) {
			R->is_const.data[v] = true;
			R->cval.data[v] = in->imm;
		}
		if (in->op == tgOp_Lea && ((in->mem.kind == tgMem_Local && p->locals.data[in->mem.base].over_align <= 16) || in->mem.kind == tgMem_Incoming)) {
			R->remat.data[v] = true;
			R->rmem.data[v] = in->mem;
		}
		if ((in->op == tgOp_ICmp || in->op == tgOp_FCmp) || (in->op == tgOp_Load && in->type == tgType_I8)) {
			R->clean.data[v] = true;
		}
	} else {
		R->uses.data[v]++;
		if (fx[v].first_use < 0) fx[v].first_use = X->linear;
		fx[v].last_use = X->linear;
		if (fx[v].def_block != b->index) fx[v].cross = true;
	}
}

TG_INTERNAL void tg__alloc_note(tgAllocCtx *X, i32 id, bool is_def) {
	i32 bi = (i32)X->bi;
	i32 linear = X->linear;
	if (X->gvreg.data[id] & TG_LOCAL_ITEM) X->lweight.data[X->gvreg.data[id] & ~TG_LOCAL_ITEM] += X->bweight.data[X->b->index];
	if (X->ev_block.data[id] != bi) {
		X->ev_block.data[id] = bi;
		X->ev_state.data[id] = 0;
	}
	if (is_def) {
		if (!(X->ev_state.data[id] & 2)) {
			tgAllocEvent e;
			e.id = id;
			e.block = bi;
			e.kill = true;
			tg_array_add(&X->events, e);
		}
		X->ev_state.data[id] |= 2;
	} else if (X->ev_state.data[id] == 0) {
		tgAllocEvent e;
		e.id = id;
		e.block = bi;
		e.kill = false;
		tg_array_add(&X->events, e);
		X->ev_state.data[id] = 1;
	}
	if (linear < X->glo.data[id]) {
		X->glo.data[id] = linear;
		X->glo_def.data[id] = is_def;
	} else if (linear == X->glo.data[id] && !is_def) {
		X->glo_def.data[id] = false;
	}
	if (linear > X->ghi.data[id]) X->ghi.data[id] = linear;
}

TG_INTERNAL void tg__alloc_note_vreg(void *ctx, u32 v, bool is_def) {
	tgAllocCtx *X = (tgAllocCtx *)ctx;
	if (v != 0 && X->gid.data[v] >= 0) tg__alloc_note(X, X->gid.data[v], is_def);
}

TG_INTERNAL i32 tg__alloc_local_of(tgAllocCtx *X, tgMem const *m) {
	return m->kind == tgMem_Local && X->lgid.data[m->base] >= 0 ? (i32)m->base : -1;
}

TG_INTERNAL bool tg__alloc_writes(tgAllocCtx *X, tgInstr const *n, i32 l) {
	if (n->op == tgOp_Store) return tg__alloc_local_of(X, &n->mem) == l;
	if (n->op == tgOp_Call) {
		tgCall const *c = &X->p->calls.data[(isize)n->imm];
		for (isize i = 0; i < c->rets.count; i++) if (tg__alloc_local_of(X, &c->rets.data[i].dst) == l) return true;
	}
	return false;
}

TG_INTERNAL bool tg__alloc_reads(tgAllocCtx *X, tgInstr const *n, i32 l) {
	if (n->op == tgOp_Load) return tg__alloc_local_of(X, &n->mem) == l;
	if (n->op == tgOp_Call || n->op == tgOp_Ret || n->op == tgOp_Syscall) {
		tgCall const *c = &X->p->calls.data[(isize)n->imm];
		for (isize i = 0; i < c->args.count; i++) {
			tgCallArg const *arg = &c->args.data[i];
			if (arg->kind == tgCallArg_GprMem && tg__alloc_local_of(X, &arg->mem) == l) return true;
		}
	}
	return false;
}

TG_INTERNAL i64 tg__alloc_weight_of(tgAllocCtx *X, u32 v) {
	return (v & TG_LOCAL_ITEM) ? X->lweight.data[v & ~TG_LOCAL_ITEM] : X->fx[v].weight;
}

// gives v a register for [now, end] if one is free, a callee saved one when `calls`
TG_INTERNAL void tg__alloc_take(tgAllocCtx *X, u32 v, i32 end, bool calls) {
	tgRegAlloc *R = X->R;
	tgRegPools pools = X->pools;
	i32 linear = X->linear;
	bool is_local = (v & TG_LOCAL_ITEM) != 0;
	i32 l = (i32)(v & ~TG_LOCAL_ITEM);
	bool fp = is_local ? X->local_fp.data[l] : tg_type_is_float(X->p->vregs.data[v]);
	u32 *free = fp ? &X->free_v : &X->free_x;
	u32 pool = fp ? (calls ? pools.callee_v : pools.caller_v | pools.callee_v)
	              : (calls ? pools.callee_x : pools.caller_x | pools.callee_x);
	if (is_local && X->is_param.data[l]) {
		// the prologue moves a parameter into a register no other parameter arrives in
		pool = fp ? pool & ~X->arrive_v : pools.callee_x;
		i8 w = X->arrive.data[l];
		if (w >= 0 && !calls && (*free & (1u << w))) pool = 1u << w;
	}
	if (calls && tg__alloc_weight_of(X, v) + (is_local && X->is_param.data[l] ? 1 : 0) < pools.callee_weight) {
		// saving a callee saved register costs about what a few slot accesses do; a
		// parameter's slot also costs the prologue's store
		pool &= fp ? R->used_v : R->used_x;
	}
	u32 avail = *free & pool;
	if (avail == 0) {
		// the register of the active item that costs least in memory for each position it
		// covers, when that is less than v's
		i64 w = tg__alloc_weight_of(X, v);
		i64 wl = end - linear + 1;
		i32 best = -1;
		for (u32 r = 0; r < 32; r++) {
			if (!(pool & (1u << r))) continue;
			i32 k = X->owner[(fp ? TG_FREG : 0) + r];
			if (k < 0) continue;
			u32 y = X->active.data[k].v;
			if ((y & TG_LOCAL_ITEM) && X->shared.data[y & ~TG_LOCAL_ITEM] >= linear) continue;
			i64 wy = tg__alloc_weight_of(X, y);
			i64 ly = X->active.data[k].len;
			if (wy*wl < w*ly) { w = wy; wl = ly; best = k; }
		}
		if (best >= 0) {
			u32 y = X->active.data[best].v;
			i8 r = (y & TG_LOCAL_ITEM) ? R->local_reg.data[y & ~TG_LOCAL_ITEM] : R->reg.data[y];
			if (y & TG_LOCAL_ITEM) R->local_reg.data[y & ~TG_LOCAL_ITEM] = TG_NOREG;
			else                   R->reg.data[y] = TG_NOREG;
			X->active.data[best].dead = true;
			X->owner[r] = -1;
			*free |= 1u << (r >= TG_FREG ? r - TG_FREG : r);
			avail = *free & pool;
		}
	}
	if (avail == 0) return;
	// the caller saved ones first, they cost no save; from the top, where calls
	// want the fewest arguments
	u32 caller = avail & (fp ? pools.caller_v : pools.caller_x);
	u32 r = 0;
	if (caller) {
		r = 31;
		while ((caller & (1u << r)) == 0) r--;
	} else {
		while ((avail & (1u << r)) == 0) r++;
	}
	*free &= ~(1u << r);
	if (fp && is_local) {
		R->used_v |= 1u << r;
		R->local_reg.data[l] = (i8)(TG_FREG + r);
	} else if (fp) {
		R->used_v |= 1u << r;
		R->reg.data[v] = (i8)(TG_FREG + r);
	} else if (is_local) {
		R->used_x |= 1u << r;
		R->local_reg.data[l] = (i8)r;
	} else {
		R->used_x |= 1u << r;
		R->reg.data[v] = (i8)r;
	}
	X->owner[(fp ? TG_FREG : 0) + r] = (i32)X->active.count;
	tgAllocActive act;
	act.v = v;
	act.next = X->expire.data[end+1];
	act.dead = false;
	act.len = end - linear + 1;
	X->expire.data[end+1] = (i32)X->active.count;
	tg_array_add(&X->active, act);
}

// A copy or extension whose source dies here writes the source's register: no move. The
// lowering reads the source before it writes the result.
TG_INTERNAL bool tg__alloc_coalesce(tgAllocCtx *X, u32 v, i32 end) {
	tgRegAlloc *R = X->R;
	tgVregFacts *fx = X->fx;
	tgInstr const *in = X->in;
	switch (in->op) {
	case tgOp_Copy:
	case tgOp_Trunc:
	case tgOp_Bitcast:
	case tgOp_Zext:
	case tgOp_Sext:
		break;
	default:
		return false;
	}
	u32 a = in->a;
	if (in->dst != v || a == 0 || a == v || !R->in_block.data[a] || fx[a].last_use != X->linear) return false;
	i8 r = R->reg.data[a];
	bool fp = tg_type_is_float(X->p->vregs.data[v]);
	if (r == TG_NOREG || (r >= TG_FREG) != fp || tg_type_is_float(X->p->vregs.data[a]) != fp) return false;
	i32 k = X->owner[r];
	if (k < 0 || X->active.data[k].v != a || X->active.data[k].dead) return false;
	u32 bit = 1u << (fp ? r - TG_FREG : r);
	bool calls = X->clobbers.data[end+1] - X->clobbers.data[X->linear+1] > 0;
	if (calls && (bit & (fp ? X->pools.caller_v : X->pools.caller_x))) return false;
	X->active.data[k].dead = true;
	R->reg.data[v] = r;
	X->owner[r] = (i32)X->active.count;
	tgAllocActive act;
	act.v = v;
	act.next = X->expire.data[end+1];
	act.dead = false;
	act.len = end - X->linear + 1;
	X->expire.data[end+1] = (i32)X->active.count;
	tg_array_add(&X->active, act);
	return true;
}

TG_INTERNAL void tg__alloc_assign(void *ctx, u32 v, bool is_def) {
	tgAllocCtx *X = (tgAllocCtx *)ctx;
	tgRegAlloc *R = X->R;
	tgVregFacts *fx = X->fx;
	i32 linear = X->linear;
	if (v == 0 || !is_def || !R->in_block.data[v] || R->is_const.data[v] || R->remat.data[v] || R->reg.data[v] != TG_NOREG) return;
	tgType t = X->p->vregs.data[v];
	if (!tg_type_is_int(t) && !tg_type_is_float(t)) return;
	if (R->via.data[v] >= 0 && R->local_reg.data[R->via.data[v]] >= 0) {
		R->reg.data[v] = R->local_reg.data[R->via.data[v]];
		i32 l = R->via.data[v];
		X->shared.data[l] = tg_max(X->shared.data[l], fx[v].last_use >= 0 ? fx[v].last_use : linear);
		return;
	}
	i32 end = fx[v].last_use >= 0 ? fx[v].last_use : linear;
	if (tg__alloc_coalesce(X, v, end)) return;
	// read only as an argument, before that call: right in the argument register
	i8 w = fx[v].want;
	if (w >= 0 && R->uses.data[v] == 1 && X->clobbers.data[end] - X->clobbers.data[linear+1] == 0 &&
	    (w >= TG_FREG) == tg_type_is_float(t)) {
		u32 *free = tg_type_is_float(t) ? &X->free_v : &X->free_x;
		u32 bit = 1u << (w >= TG_FREG ? w - TG_FREG : w);
		if (*free & bit) {
			*free &= ~bit;
			R->reg.data[v] = w;
			if (w >= TG_FREG) R->used_v |= bit;
			else              R->used_x |= bit;
			X->owner[w] = (i32)X->active.count;
			tgAllocActive act;
			act.v = v;
			act.next = X->expire.data[end+1];
			act.dead = false;
			act.len = 0;
			X->expire.data[end+1] = (i32)X->active.count;
			tg_array_add(&X->active, act);
			return;
		}
	}
	tg__alloc_take(X, v, end, X->clobbers.data[end+1] - X->clobbers.data[linear+1] > 0);
}

// The block layout the interval phases share: each block's first and last position and its
// successors in the block order, and the first and last position of each debug scope.
typedef struct tgAllocFlow {
	tgI32Array bstart, bend, succ, opos;
	tgI32Array smin, smax;
	isize      sn;
} tgAllocFlow;

// The facts and results start empty, and the promotable scalar locals are ranked.
TG_INTERNAL void tg__alloc_setup(tgAllocCtx *X) {
	tgProc *p = X->p;
	tgRegAlloc *R = X->R;
	isize vreg_count = p->vregs.count;
	// Promotable scalar locals get live intervals like the vregs, a parameter's starting in the
	// register it arrives in
	tg_array_make(&R->local_reg, p->locals.count);
	for (isize i = 0; i < p->locals.count; i++) R->local_reg.data[i] = TG_NOREG;
	tg_rank_promotable_locals(p, 16, 1, 0, &X->ranked, &X->local_fp, true, true);
	tg_array_make(&X->arrive, p->locals.count); // a parameter's register
	for (isize i = 0; i < p->locals.count; i++) X->arrive.data[i] = TG_NOREG;
	tg_array_make(&X->is_param, p->locals.count);
	X->arrive_v = 0; // the float registers parameters arrive in
	for (isize i = 0; i < p->params_in.count; i++) {
		tgParamIn const *in = &p->params_in.data[i];
		if (in->loc == tgLoc_Xmm) X->arrive_v |= 1u << in->reg;
		if (in->dst.kind != tgMem_Local) continue;
		X->is_param.data[in->dst.base] = true;
		if (in->loc == tgLoc_Gpr || in->loc == tgLoc_Xmm) X->arrive.data[in->dst.base] = (i8)in->reg;
	}

	// one struct per vreg: the passes over the instructions touch all of it at once
	tg_array_make(&R->facts, vreg_count);
	tgVregFacts *fx = R->facts.data;
	X->fx = fx;
	tg_array_make_cap(&X->clobbers, 0, 256); // prefix counts of target.clobbers
	for (isize i = 0; i < vreg_count; i++) {
		tgVregFacts *x = &fx[i];
		x->block = -1;
		x->def_pos = -1;
		x->first_use = -1;
		x->last_use = -1;
		x->def_block = -1;
		x->want = TG_NOREG;
	}

	tg_array_make(&R->uses, vreg_count);
	tg_array_make(&R->reg, vreg_count);
	tg_array_make(&R->is_const, vreg_count);
	tg_array_make(&R->cval, vreg_count);
	tg_array_make(&R->in_block, vreg_count);
	tg_array_make(&R->clean, vreg_count);
	tg_array_make(&R->via, vreg_count);
	tg_array_make(&R->remat, vreg_count);
	tg_array_make(&R->rmem, vreg_count);
	for (isize i = 0; i < vreg_count; i++) {
		R->reg.data[i] = TG_NOREG;
		R->via.data[i] = -1;
	}
}

// The spill cost of an item: its references, each 8 times more for each loop around it;
// a loop is a jump back to or before its block.
TG_INTERNAL void tg__alloc_block_weights(tgAllocCtx *X) {
	tgProc *p = X->p;
	tg_array_make(&X->bweight, p->blocks.count);
	isize bc = p->order.count;
	tgIsizeArray pos;
	tgI32Array depth; // as differences first, see below
	tg_array_make(&pos, p->blocks.count);
	tg_array_make(&depth, bc + 1);
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
				depth.data[pos.data[t]] += 1;
				depth.data[i+1] -= 1;
			}
		}
	}
	for (isize i = 1; i < bc; i++) depth.data[i] += depth.data[i-1];
	for (isize i = 0; i < bc; i++) X->bweight.data[p->order.data[i]->index] = 1ll << (3*tg_min(depth.data[i], 6));
	tg_array_free(&pos);
	tg_array_free(&depth);
}

// One pass over the instructions in block order collects each vreg's facts: its definitions,
// uses and positions, the register a call wants it in, and where calls clobber registers.
TG_INTERNAL void tg__alloc_collect_facts(tgAllocCtx *X, tgRegTarget const *target) {
	tgProc *p = X->p;
	tgRegAlloc *R = X->R;
	tgVregFacts *fx = X->fx;
	isize vreg_count = p->vregs.count;
	X->linear = 0;
	R->max_call_stack = 0;
	R->sp_moves = false;
	tg_array_add(&X->clobbers, 0);
	for (isize bi = 0; bi < p->order.count; bi++) {
		tgBlock *b = p->order.data[bi];
		X->b = b;
		for (isize ii = 0; ii < b->instrs.count; ii++) {
			tgInstr const *in = &b->instrs.data[ii];
			X->in = in;
			tg_for_each_vreg(p, in, tg__alloc_scan, X);
			if (in->op == tgOp_Call) {
				R->max_call_stack = tg_max(R->max_call_stack, p->calls.data[(isize)in->imm].stack_size);
			}
			if (in->op == tgOp_Alloca || in->op == tgOp_StackRestore) R->sp_moves = true;
			target->wants(target->ctx, p, in, tg__alloc_note_want, X);
			tg_array_add(&X->clobbers, X->clobbers.data[X->linear] + (target->clobbers(target->ctx, in) ? 1 : 0));
			X->linear++;
		}
	}
	for (isize v = 1; v < vreg_count; v++) {
		if (fx[v].defs != 1) {
			R->is_const.data[v] = false;
			R->clean.data[v] = false;
			R->remat.data[v] = false;
		}
		R->in_block.data[v] = fx[v].defs == 1 && !fx[v].multi && (fx[v].first_use < 0 || fx[v].first_use > fx[v].def_pos);
	}
}

// The items that get one interval over the block order, each with a global id: the ranked
// locals, parameters first, then the int and float vregs that live past one block.
TG_INTERNAL void tg__alloc_number_items(tgAllocCtx *X) {
	tgProc *p = X->p;
	tgRegAlloc *R = X->R;
	tgVregFacts *fx = X->fx;
	isize vreg_count = p->vregs.count;
	tg_array_make(&X->gid, vreg_count);
	// an item is a vreg, or a local with TG_LOCAL_ITEM set
	tg_array_make_cap(&X->gvreg, 0, 64);
	tg_array_make(&X->lgid, p->locals.count);
	tg_array_make(&X->lweight, p->locals.count);
	for (isize i = 0; i < p->locals.count; i++) X->lgid.data[i] = -1;
	for (int pass = 0; pass < 2; pass++) {
		for (isize ri = 0; ri < X->ranked.count; ri++) {
			i32 l = X->ranked.data[ri];
			if (X->is_param.data[l] != (pass == 0)) continue;
			X->lgid.data[l] = (i32)X->gvreg.count;
			tg_array_add(&X->gvreg, TG_LOCAL_ITEM | (u32)l);
		}
	}
	for (isize v = 1; v < vreg_count; v++) {
		X->gid.data[v] = -1;
		tgType t = p->vregs.data[v];
		if (R->in_block.data[v] || fx[v].defs == 0 || R->is_const.data[v] || R->remat.data[v] || (!tg_type_is_int(t) && !tg_type_is_float(t))) continue;
		X->gid.data[v] = (i32)X->gvreg.count;
		tg_array_add(&X->gvreg, (u32)v);
	}
	isize gn = X->gvreg.count;
	tg_array_make(&X->glo, gn);
	tg_array_make(&X->ghi, gn);
	tg_array_make(&X->glo_def, gn); // the interval starts where an instruction writes it
	tg_array_make_cap(&X->gstart, 0, gn); // global ids by the start of their interval
}

// Per block, the events of each item (read before written, or written), the successors, and
// the positions each debug scope covers.
TG_INTERNAL void tg__alloc_scan_events(tgAllocCtx *X, tgAllocFlow *F) {
	tgProc *p = X->p;
	isize gn = X->gvreg.count;
	isize bc = p->order.count;
	tg_array_make(&F->bstart, bc);
	tg_array_make(&F->bend, bc);
	tg_array_make(&F->succ, 2*bc);
	tg_array_make(&F->opos, p->blocks.count);
	// per block, whether an item is read there before it is written (gen) or is written (kill)
	tg_array_make_cap(&X->events, 0, 4*gn);
	tg_array_make(&X->ev_block, gn); // the block of the item's last event
	tg_array_make(&X->ev_state, gn); // 1: gen noted, 2: killed
	for (isize i = 0; i < gn; i++) {
		X->glo.data[i] = INT32_MAX;
		X->ghi.data[i] = -1;
		X->ev_block.data[i] = -1;
	}
	for (isize bi = 0; bi < bc; bi++) F->opos.data[p->order.data[bi]->index] = (i32)bi;
	for (isize i = 0; i < p->params_in.count; i++) {
		tgParamIn const *in = &p->params_in.data[i];
		if (in->dst.kind != tgMem_Local || X->lgid.data[in->dst.base] < 0) continue;
		i32 id = X->lgid.data[in->dst.base];
		X->glo.data[id] = 0;
		X->ghi.data[id] = 0;
	}

	// the positions each debug scope covers
	F->sn = p->debug_scope_parent.count;
	tg_array_make(&F->smin, F->sn);
	tg_array_make(&F->smax, F->sn);
	for (isize i = 0; i < F->sn; i++) {
		F->smin.data[i] = INT32_MAX;
		F->smax.data[i] = -1;
	}

	X->linear = 0;
	for (isize bi = 0; bi < bc; bi++) {
		tgBlock *b = p->order.data[bi];
		X->b = b;
		X->bi = bi;
		i32 scope = b->debug_scope;
		F->succ.data[2*bi] = F->succ.data[2*bi+1] = -1;
		F->bstart.data[bi] = X->linear;
		for (isize ii = 0; ii < b->instrs.count; ii++) {
			tgInstr const *in = &b->instrs.data[ii];
			i32 linear = X->linear;
			if (in->op == tgOp_Scope) scope = (i32)in->imm;
			if (scope >= 0 && scope < F->sn) {
				F->smin.data[scope] = tg_min(F->smin.data[scope], linear);
				F->smax.data[scope] = tg_max(F->smax.data[scope], linear);
			}
			bool local = (in->op == tgOp_Load || in->op == tgOp_Store) && in->mem.kind == tgMem_Local && X->lgid.data[in->mem.base] >= 0;
			if (local && in->op == tgOp_Load) tg__alloc_note(X, X->lgid.data[in->mem.base], false);
			tgCall const *call = NULL;
			if (in->op == tgOp_Call || in->op == tgOp_Ret || in->op == tgOp_Syscall) call = &p->calls.data[(isize)in->imm];
			if (call) {
				for (isize ai = 0; ai < call->args.count; ai++) {
					tgCallArg const *arg = &call->args.data[ai];
					if (arg->kind == tgCallArg_GprMem && arg->mem.kind == tgMem_Local && X->lgid.data[arg->mem.base] >= 0) tg__alloc_note(X, X->lgid.data[arg->mem.base], false);
				}
			}
			// an instruction names the vregs it reads before the ones it writes
			tg_for_each_vreg(p, in, tg__alloc_note_vreg, X);
			if (local && in->op == tgOp_Store) tg__alloc_note(X, X->lgid.data[in->mem.base], true);
			if (call) {
				for (isize ri = 0; ri < call->rets.count; ri++) {
					tgCallRet const *r = &call->rets.data[ri];
					if (r->dst.kind == tgMem_Local && X->lgid.data[r->dst.base] >= 0) tg__alloc_note(X, X->lgid.data[r->dst.base], true);
				}
			}
			if (in->op == tgOp_Jump) {
				F->succ.data[2*bi] = (i32)in->imm;
			} else if (in->op == tgOp_Branch) {
				F->succ.data[2*bi] = (i32)in->imm;
				F->succ.data[2*bi+1] = (i32)in->c;
			}
			X->linear++;
		}
		F->bend.data[bi] = X->linear - 1;
		for (isize s = 2*bi; s < 2*bi+2; s++) {
			if (F->succ.data[s] >= 0) F->succ.data[s] = p->blocks.data[F->succ.data[s]]->placed ? F->opos.data[F->succ.data[s]] : -1;
		}
	}
}

// Each item is live into the blocks that read it first, and from there back through the
// predecessors until the blocks that write it.
TG_INTERNAL void tg__alloc_liveness(tgAllocCtx *X, tgAllocFlow *F) {
	tgProc *p = X->p;
	isize gn = X->gvreg.count;
	isize bc = p->order.count;
	tgI32Array pred_start, preds;
	tg_array_make(&pred_start, bc+1);
	tg_array_make(&preds, 2*bc);
	for (isize s = 0; s < 2*bc; s++) if (F->succ.data[s] >= 0) pred_start.data[F->succ.data[s]+1]++;
	for (isize i = 0; i < bc; i++) pred_start.data[i+1] += pred_start.data[i];
	{
		tgI32Array fill;
		tg_array_make(&fill, bc);
		for (isize s = 0; s < 2*bc; s++) {
			if (F->succ.data[s] >= 0) preds.data[pred_start.data[F->succ.data[s]] + fill.data[F->succ.data[s]]++] = (i32)(s/2);
		}
		tg_array_free(&fill);
	}
	tgI32Array ev_start, by_id;
	tg_array_make(&ev_start, gn+1);
	tg_array_make(&by_id, X->events.count);
	for (isize i = 0; i < X->events.count; i++) ev_start.data[X->events.data[i].id+1]++;
	for (isize i = 0; i < gn; i++) ev_start.data[i+1] += ev_start.data[i];
	{
		tgI32Array fill;
		tg_array_make(&fill, gn);
		for (isize i = 0; i < X->events.count; i++) by_id.data[ev_start.data[X->events.data[i].id] + fill.data[X->events.data[i].id]++] = (i32)i;
		tg_array_free(&fill);
	}
	// marks hold id+1 for the item being walked
	tgI32Array kill_mark, in_mark, out_mark, stack;
	tg_array_make(&kill_mark, bc);
	tg_array_make(&in_mark, bc);
	tg_array_make(&out_mark, bc);
	tg_array_make_cap(&stack, 0, 64);
	for (i32 id = 0; id < gn; id++) {
		i32 mark = id + 1;
		stack.count = 0;
		for (i32 k = ev_start.data[id]; k < ev_start.data[id+1]; k++) {
			tgAllocEvent const *e = &X->events.data[by_id.data[k]];
			if (e->kill) kill_mark.data[e->block] = mark;
			else         tg_array_add(&stack, e->block);
		}
		for (isize si = 0; si < stack.count; si++) in_mark.data[stack.data[si]] = mark;
		while (stack.count > 0) {
			i32 bi = tg_array_pop(&stack);
			if (F->bstart.data[bi] <= X->glo.data[id]) {
				X->glo.data[id] = F->bstart.data[bi];
				X->glo_def.data[id] = false;
			}
			for (i32 k = pred_start.data[bi]; k < pred_start.data[bi+1]; k++) {
				i32 pb = preds.data[k];
				if (out_mark.data[pb] != mark) {
					out_mark.data[pb] = mark;
					X->ghi.data[id] = tg_max(X->ghi.data[id], F->bend.data[pb]);
				}
				if (kill_mark.data[pb] != mark && in_mark.data[pb] != mark) {
					in_mark.data[pb] = mark;
					tg_array_add(&stack, pb);
				}
			}
		}
	}
	tg_array_free(&pred_start);
	tg_array_free(&preds);
	tg_array_free(&ev_start);
	tg_array_free(&by_id);
	tg_array_free(&kill_mark);
	tg_array_free(&in_mark);
	tg_array_free(&out_mark);
	tg_array_free(&stack);
}

// Copies between a promoted local and a vreg in one block. A load's vreg reads the local's
// register while nothing writes the local. A vreg whose only use is a store to the local is
// computed right in its register, when nothing reads or writes the local in between.
TG_INTERNAL void tg__alloc_local_copies(tgAllocCtx *X, tgAllocFlow *F, tgRegTarget const *target) {
	tgProc *p = X->p;
	tgRegAlloc *R = X->R;
	tgVregFacts *fx = X->fx;
	isize bc = p->order.count;
	tgI32Array alias_end; // last read through an alias
	tg_array_make(&alias_end, p->locals.count);
	for (isize i = 0; i < p->locals.count; i++) alias_end.data[i] = -1;
	// the positions writing each promoted local, in order: wpos[wstart[l] .. wstart[l+1])
	tgI32Array wstart, wpos;
	tg_array_make(&wstart, p->locals.count + 1);
	tg_array_make_cap(&wpos, 0, 64);
	for (int pass = 0; pass < 2; pass++) {
		tgI32Array fill;
		tg_array_make(&fill, p->locals.count);
		for (isize bi = 0; bi < bc; bi++) {
			tgBlock *b = p->order.data[bi];
			for (isize i = 0; i < b->instrs.count; i++) {
				tgInstr const *in = &b->instrs.data[i];
				i32 at = F->bstart.data[bi] + (i32)i;
#define TG_NOTE_WRITE(l_) do { i32 l__ = (l_); if (l__ >= 0) { \
					if (pass == 0) wstart.data[l__+1]++; \
					else           wpos.data[wstart.data[l__] + fill.data[l__]++] = at; } } while (0)
				if (in->op == tgOp_Store) {
					TG_NOTE_WRITE(tg__alloc_local_of(X, &in->mem));
				} else if (in->op == tgOp_Call) {
					tgCall const *c = &p->calls.data[(isize)in->imm];
					for (isize ri = 0; ri < c->rets.count; ri++) TG_NOTE_WRITE(tg__alloc_local_of(X, &c->rets.data[ri].dst));
				}
#undef TG_NOTE_WRITE
			}
		}
		tg_array_free(&fill);
		if (pass == 0) {
			for (isize l = 0; l < p->locals.count; l++) wstart.data[l+1] += wstart.data[l];
			tg_array_resize(&wpos, wstart.data[p->locals.count]);
		}
	}
	for (isize bi = 0; bi < bc; bi++) {
		tgBlock *b = p->order.data[bi];
		for (isize i = 0; i < b->instrs.count; i++) {
			tgInstr const *in = &b->instrs.data[i];
			i32 pos = F->bstart.data[bi] + (i32)i;
			if (in->op == tgOp_Load && tg__alloc_local_of(X, &in->mem) >= 0 && in->dst != 0 && X->gid.data[in->dst] >= 0) {
				// Read in other blocks too, as a loop variable the loop head loads and its body
				// reads: the local's register serves while nothing writes the local anywhere in the
				// vreg's interval, which starts here.
				i32 l = tg__alloc_local_of(X, &in->mem);
				u32 v = in->dst;
				i32 id = X->gid.data[v];
				if (fx[v].defs != 1 || R->uses.data[v] == 0 || tg_type_is_float(p->vregs.data[v]) != X->local_fp.data[l] ||
				    X->glo.data[id] != pos || X->ghi.data[id] < pos) continue;
				bool ok = true;
				for (i32 k = wstart.data[l]; ok && k < wstart.data[l+1]; k++) ok = wpos.data[k] <= pos || wpos.data[k] > X->ghi.data[id];
				if (!ok) continue;
				R->via.data[v] = l;
				alias_end.data[l] = tg_max(alias_end.data[l], X->ghi.data[id]);
				X->ghi.data[X->lgid.data[l]] = tg_max(X->ghi.data[X->lgid.data[l]], X->ghi.data[id]);
			} else if (in->op == tgOp_Load) {
				i32 l = tg__alloc_local_of(X, &in->mem);
				u32 v = in->dst;
				if (l < 0 || v == 0 || !R->in_block.data[v] || R->uses.data[v] == 0 || fx[v].last_use - pos > 256 ||
				    tg_type_is_float(p->vregs.data[v]) != X->local_fp.data[l]) continue;
				bool ok = true;
				for (isize j = i+1; ok && j <= i + (fx[v].last_use - pos); j++) ok = !tg__alloc_writes(X, &b->instrs.data[j], l);
				if (!ok) continue;
				R->via.data[v] = l;
				alias_end.data[l] = tg_max(alias_end.data[l], fx[v].last_use);
				X->ghi.data[X->lgid.data[l]] = tg_max(X->ghi.data[X->lgid.data[l]], fx[v].last_use);
			} else if (in->op == tgOp_Store) {
				i32 l = tg__alloc_local_of(X, &in->mem);
				u32 v = in->a;
				if (l < 0 || v == 0 || !R->in_block.data[v] || R->is_const.data[v] || R->remat.data[v] || R->via.data[v] >= 0 ||
				    !(X->local_fp.data[l] ? tg_type_is_float(p->vregs.data[v]) : tg_type_is_int(p->vregs.data[v])) || pos - fx[v].def_pos > 256 || alias_end.data[l] > fx[v].def_pos ||
				    fx[v].last_use - pos > 256) continue;
				isize d = fx[v].def_pos - F->bstart.data[bi];
				// the definition may read the local, as in `total = total + x`, when it reads before it writes
				if (alias_end.data[l] == fx[v].def_pos && !(target->reads_first && target->reads_first(target->ctx, &b->instrs.data[d]))) continue;
				bool ok = true;
				for (isize j = d; ok && j < i; j++) ok = !tg__alloc_reads(X, &b->instrs.data[j], l) && (j == d || !tg__alloc_writes(X, &b->instrs.data[j], l));
				// read again after the store, as the front end reads a value it just stored: the
				// local's register still holds it while nothing writes the local
				for (isize j = i+1; ok && j <= i + (fx[v].last_use - pos); j++) ok = !tg__alloc_writes(X, &b->instrs.data[j], l);
				if (!ok) continue;
				R->via.data[v] = l;
				i32 id = X->lgid.data[l];
				if (fx[v].def_pos < X->glo.data[id]) {
					X->glo.data[id] = fx[v].def_pos;
					X->glo_def.data[id] = true;
				}
				if (fx[v].last_use > pos) {
					alias_end.data[l] = tg_max(alias_end.data[l], fx[v].last_use);
					X->ghi.data[id] = tg_max(X->ghi.data[id], fx[v].last_use);
				}
			}
		}
	}
	tg_array_free(&alias_end);
	tg_array_free(&wstart);
	tg_array_free(&wpos);
}

// A debug variable keeps its register in all of its scope, the code where a debugger shows
// it. Scopes are made after their parents.
TG_INTERNAL void tg__alloc_scope_extents(tgAllocCtx *X, tgAllocFlow *F) {
	tgProc *p = X->p;
	for (isize i = F->sn-1; i > 0; i--) {
		i32 up = p->debug_scope_parent.data[i];
		if (up < 0 || F->smax.data[i] < 0) continue;
		F->smin.data[up] = tg_min(F->smin.data[up], F->smin.data[i]);
		F->smax.data[up] = tg_max(F->smax.data[up], F->smax.data[i]);
	}
	for (isize i = 0; i < p->debug_vars.count; i++) {
		tgDebugVar const *v = &p->debug_vars.data[i];
		if (v->local < 0 || X->lgid.data[v->local] < 0 || v->scope < 0 || v->scope >= F->sn || F->smax.data[v->scope] < 0) continue;
		i32 id = X->lgid.data[v->local];
		if (F->smin.data[v->scope] <= X->glo.data[id]) {
			X->glo.data[id] = F->smin.data[v->scope];
			X->glo_def.data[id] = false;
		}
		X->ghi.data[id] = tg_max(X->ghi.data[id], F->smax.data[v->scope]);
	}
}

// Orders the items by the start of their interval, with a counting sort.
TG_INTERNAL void tg__alloc_order_starts(tgAllocCtx *X) {
	isize gn = X->gvreg.count;
	tgI32Array first;
	tg_array_make(&first, X->linear+1);
	for (isize i = 0; i < gn; i++) {
		if (X->ghi.data[i] >= 0) first.data[X->glo.data[i]+1]++;
	}
	for (i32 i = 0; i < X->linear; i++) first.data[i+1] += first.data[i];
	tg_array_resize(&X->gstart, first.data[X->linear]);
	for (isize i = 0; i < gn; i++) {
		if (X->ghi.data[i] >= 0) X->gstart.data[first.data[X->glo.data[i]]++] = (i32)i;
	}
	tg_array_free(&first);
}

// Registers for the vregs, first come first served. When none is free, an interval takes the
// register of an active one that costs less in memory for each position it covers; the loser
// lives in its slot all its life.
TG_INTERNAL void tg__alloc_assign_regs(tgAllocCtx *X) {
	tgProc *p = X->p;
	tgRegAlloc *R = X->R;
	X->free_x = X->pools.caller_x | X->pools.callee_x;
	X->free_v = X->pools.caller_v | X->pools.callee_v;
	R->used_x = 0;
	R->used_v = 0;
	// the vregs and locals holding a register, listed under the position after their last
	tg_array_make_cap(&X->active, 0, 64);
	// the active entry holding each general register, and each float register at TG_FREG + n
	for (i32 i = 0; i < 64; i++) X->owner[i] = -1;
	// a local whose register a vreg shares keeps it
	tg_array_make(&X->shared, p->locals.count);
	for (isize i = 0; i < p->locals.count; i++) X->shared.data[i] = -1;
	tg_array_make(&X->expire, X->clobbers.count + 1);
	for (isize i = 0; i < X->expire.count; i++) X->expire.data[i] = -1;
	isize gs = 0;
	X->linear = 0;
	for (isize bi = 0; bi < p->order.count; bi++) {
		tgBlock *b = p->order.data[bi];
		for (isize ii = 0; ii < b->instrs.count; ii++) {
			tgInstr const *in = &b->instrs.data[ii];
			i32 linear = X->linear;
			for (i32 k = X->expire.data[linear]; k >= 0; k = X->active.data[k].next) {
				if (X->active.data[k].dead) continue;
				u32 v = X->active.data[k].v;
				i8 r = (v & TG_LOCAL_ITEM) ? R->local_reg.data[v & ~TG_LOCAL_ITEM] : R->reg.data[v];
				X->owner[r] = -1;
				if (r >= TG_FREG) X->free_v |= 1u << (r - TG_FREG);
				else              X->free_x |= 1u << r;
			}
			while (gs < X->gstart.count && X->glo.data[X->gstart.data[gs]] == linear) {
				i32 id = X->gstart.data[gs++];
				u32 gv = X->gvreg.data[id];
				if (!(gv & TG_LOCAL_ITEM) && R->via.data[gv] >= 0 && R->local_reg.data[R->via.data[gv]] >= 0) {
					// a load of a promoted local, read in other blocks: in the local's register
					i32 l = R->via.data[gv];
					R->reg.data[gv] = R->local_reg.data[l];
					X->shared.data[l] = tg_max(X->shared.data[l], X->ghi.data[id]);
					continue;
				}
				// a call that defines v writes it afterwards
				tg__alloc_take(X, X->gvreg.data[id], X->ghi.data[id], X->clobbers.data[X->ghi.data[id]+1] - X->clobbers.data[linear + (X->glo_def.data[id] ? 1 : 0)] > 0);
			}
			X->in = in;
			tg_for_each_vreg(p, in, tg__alloc_assign, X);
			X->linear++;
		}
	}
}

// Gives registers to the vregs and promotable locals of p. The target tells which
// instructions clobber registers and which arguments want which register; see tgRegTarget.
TG_INTERNAL void tg_alloc_regs(tgProc *p, tgRegAlloc *R, tgRegPools pools, tgRegTarget const *target) {
	tgAllocCtx ctx;
	tgAllocCtx *X = &ctx;
	memset(X, 0, sizeof(*X));
	X->p = p;
	X->R = R;
	X->pools = pools;
	tg__alloc_setup(X);
	tg__alloc_block_weights(X);
	tg__alloc_collect_facts(X, target);
	tg__alloc_number_items(X);

	// The intervals: liveness over the blocks, then the copies between promoted locals and
	// vregs, then the debug scopes, then the order of their starts.
	if (X->gvreg.count > 0 && p->order.count > 0) {
		tgAllocFlow flow;
		memset(&flow, 0, sizeof(flow));
		tg__alloc_scan_events(X, &flow);
		tg__alloc_liveness(X, &flow);
		tg__alloc_local_copies(X, &flow, target);
		tg__alloc_scope_extents(X, &flow);
		tg__alloc_order_starts(X);
		tg_array_free(&flow.bstart);
		tg_array_free(&flow.bend);
		tg_array_free(&flow.succ);
		tg_array_free(&flow.opos);
		tg_array_free(&flow.smin);
		tg_array_free(&flow.smax);
		tg_array_free(&X->events);
		tg_array_free(&X->ev_block);
		tg_array_free(&X->ev_state);
	}

	tg__alloc_assign_regs(X);

	// the other vregs used only in the block that defines them share slots, given by tg_alloc_slots
	tg_array_make(&R->slot, p->vregs.count);

	tg_array_free(&X->ranked);
	tg_array_free(&X->local_fp);
	tg_array_free(&X->arrive);
	tg_array_free(&X->is_param);
	tg_array_free(&X->clobbers);
	tg_array_free(&X->bweight);
	tg_array_free(&X->gid);
	tg_array_free(&X->gvreg);
	tg_array_free(&X->lgid);
	tg_array_free(&X->lweight);
	tg_array_free(&X->glo);
	tg_array_free(&X->ghi);
	tg_array_free(&X->glo_def);
	tg_array_free(&X->gstart);
	tg_array_free(&X->shared);
	tg_array_free(&X->active);
	tg_array_free(&X->expire);
}

// The state the helper of tg_alloc_slots shares.
typedef struct tgSlotCtx {
	tgRegAlloc *  R;
	tgVregFacts const *fx;
	tgI32Array    free_slots;
	tgU32Array    to_free;
	i32 *         cur;
	i32           linear;
} tgSlotCtx;

TG_INTERNAL void tg__slots_visit(void *ctx, u32 v, bool is_def) {
	tgSlotCtx *S = (tgSlotCtx *)ctx;
	tgRegAlloc *R = S->R;
	tgVregFacts const *fx = S->fx;
	if (v == 0 || R->reg.data[v] != TG_NOREG || R->is_const.data[v] || R->remat.data[v]) return;
	if (is_def) {
		i32 s = 0;
		if (!fx[v].cross && S->free_slots.count > 0) {
			s = tg_array_pop(&S->free_slots);
		} else {
			*S->cur += 8;
			s = -*S->cur;
		}
		R->slot.data[v] = s;
		if (!fx[v].cross && fx[v].last_use < S->linear) {
			tg_array_add(&S->to_free, v); // never used
		}
	} else if (!fx[v].cross && fx[v].last_use == S->linear) {
		tg_array_add(&S->to_free, v);
	}
}

// Frame slots for the vregs without a register, below `*cur` bytes under the frame pointer,
// which grows by what they take.
TG_INTERNAL void tg_alloc_slots(tgProc *p, tgRegAlloc *R, i32 *cur) {
	// the reads tg_alloc_regs found; nothing changed the IR since
	tgSlotCtx S;
	memset(&S, 0, sizeof(S));
	S.R = R;
	S.fx = R->facts.data;
	S.cur = cur;
	S.linear = 0;

	tg_array_make_cap(&S.free_slots, 0, 64);
	tg_array_make_cap(&S.to_free, 0, 8);
	S.linear = 0;
	for (isize bi = 0; bi < p->order.count; bi++) {
		tgBlock *b = p->order.data[bi];
		for (isize ii = 0; ii < b->instrs.count; ii++) {
			tgInstr const *in = &b->instrs.data[ii];
			S.to_free.count = 0;
			tg_for_each_vreg(p, in, tg__slots_visit, &S);
			for (isize i = 0; i < S.to_free.count; i++) {
				// a vreg may appear twice in one instruction
				bool dup = false;
				for (isize j = 0; j < i; j++) if (S.to_free.data[j] == S.to_free.data[i]) { dup = true; break; }
				if (!dup) tg_array_add(&S.free_slots, R->slot.data[S.to_free.data[i]]);
			}
			S.linear++;
		}
	}
	tg_array_free(&S.free_slots);
	tg_array_free(&S.to_free);
}
