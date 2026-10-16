// A text dump of a procedure's IR, for debugging: tg_dump_proc(p, stderr).

static char const *const tg_op_names[tgOp_COUNT] = {
	"nop", "loc", "scope",
	"iconst", "fconst", "lea", "load", "store", "copy",
	"add", "sub", "mul", "sdiv", "udiv", "srem", "urem", "and", "or", "xor", "shl", "lshr", "ashr",
	"fadd", "fsub", "fmul", "fdiv",
	"neg", "not", "fneg",
	"icmp", "fcmp",
	"zext", "sext", "trunc", "sitof", "uitof", "ftosi", "ftoui", "fext", "ftrunc", "bitcast",
	"select",
	"memcopy", "memmove", "memzero", "memcopy.dyn", "memmove.dyn", "memset.dyn",
	"call",
	"jump", "branch", "ret", "unreachable", "trap", "debugtrap",
	"fence", "atomic.load", "atomic.store", "atomic.rmw", "atomic.cas",
	"rdcycle", "cpurelax", "prefetch", "stackptr", "frameaddr", "retaddr", "sqrt", "fma",
	"bswap", "popcount", "ctz", "clz", "syscall", "mulovf", "mulhiu", "cpuid", "xgetbv",
	"vec128", "valgrind", "alloca", "tlsaddr", "ftohalf", "halftof",
	"stackrestore", "x87",
};

static char const *const tg_type_names[tgType_COUNT] = {"none", "i8", "i16", "i32", "i64", "f32", "f64", "v128"};

static char const *const tg_cond_names[] = {
	"eq", "ne", "slt", "sle", "sgt", "sge", "ult", "ule", "ugt", "uge",
	"feq", "fne", "flt", "fle", "fgt", "fge",
};

static char const *const tg_rmw_names[] = {"xchg", "add", "sub", "and", "or", "xor", "nand"};

TG_INTERNAL char const *tg_dump_type_name(tgType t) {
	return t < tgType_COUNT ? tg_type_names[t] : "?";
}

TG_INTERNAL void tg_dump_mem(tgProc *p, tgMem m, FILE *f) {
	switch (m.kind) {
	case tgMem_None:     fprintf(f, "[none]"); return;
	case tgMem_Reg:      fprintf(f, "[v%u", m.base); break;
	case tgMem_Local:    fprintf(f, "[local%u", m.base); break;
	case tgMem_Incoming: fprintf(f, "[incoming"); break;
	case tgMem_Sym:
		if ((isize)m.base < p->m->symbols.count) {
			tgString name = p->m->symbols.data[m.base].name;
			fprintf(f, "[%.*s", (int)name.len, name.text);
		} else {
			fprintf(f, "[sym%u", m.base);
		}
		break;
	default: fprintf(f, "[?%d", m.kind); break;
	}
	if (m.offset != 0) fprintf(f, "%+d", m.offset);
	fprintf(f, "]");
	if (m.align != 0) fprintf(f, " align %d", m.align);
}

TG_INTERNAL void tg_dump_call(tgProc *p, tgCall const *c, FILE *f) {
	static char const *const kinds[] = {"gpr", "xmm", "gpr.mem", "xmm.mem", "stack", "stack.mem", "x87.mem"};
	static char const *const exts[] = {"", " zext", " sext"};
	isize i;
	if (c->target_sym >= 0 && c->target_sym < p->m->symbols.count) {
		tgString name = p->m->symbols.data[c->target_sym].name;
		fprintf(f, " %.*s", (int)name.len, name.text);
	} else if (c->target_vreg != 0) {
		fprintf(f, " v%u", c->target_vreg);
	}
	if (c->sse_count >= 0) fprintf(f, " al=%d", c->sse_count);
	if (c->stack_size != 0) fprintf(f, " stack=%d", c->stack_size);
	if (c->returns_twice) fprintf(f, " returns_twice");
	fprintf(f, " (");
	for (i = 0; i < c->args.count; i++) {
		tgCallArg const *a = &c->args.data[i];
		if (i > 0) fprintf(f, ", ");
		fprintf(f, "%s %s/%d", a->kind < 7 ? kinds[a->kind] : "?", tg_dump_type_name(a->type), a->size);
		if (a->kind == tgCallArg_Gpr || a->kind == tgCallArg_Xmm || a->kind == tgCallArg_GprMem || a->kind == tgCallArg_XmmMem) fprintf(f, " r%d", a->reg);
		if (a->kind == tgCallArg_Stack || a->kind == tgCallArg_StackMem) fprintf(f, " @%d", a->stack_offset);
		if (a->ext < 3) fprintf(f, "%s", exts[a->ext]);
		if (a->kind == tgCallArg_Gpr || a->kind == tgCallArg_Xmm || a->kind == tgCallArg_Stack) {
			fprintf(f, " v%u", a->vreg);
		} else {
			fprintf(f, " ");
			tg_dump_mem(p, a->mem, f);
		}
	}
	fprintf(f, ")");
	if (c->rets.count > 0) {
		fprintf(f, " -> (");
		for (i = 0; i < c->rets.count; i++) {
			tgCallRet const *r = &c->rets.data[i];
			if (i > 0) fprintf(f, ", ");
			fprintf(f, "%s r%d %s/%d -> ", r->loc == tgLoc_Gpr ? "gpr" : r->loc == tgLoc_X87 ? "st0" : "xmm", r->reg, tg_dump_type_name(r->type), r->size);
			tg_dump_mem(p, r->dst, f);
		}
		fprintf(f, ")");
	}
}

TG_INTERNAL void tg_dump_instr(tgProc *p, tgInstr const *in, FILE *f) {
	fprintf(f, "    ");
	if (in->dst != 0) fprintf(f, "v%u = ", in->dst);
	fprintf(f, "%s", in->op < tgOp_COUNT ? tg_op_names[in->op] : "?");
	if (in->type != tgType_None) fprintf(f, ".%s", tg_dump_type_name(in->type));
	if (in->flags & tgInstrFlag_Volatile) fprintf(f, " volatile");
	switch (in->op) {
	case tgOp_Nop:
		break;
	case tgOp_Loc:
		fprintf(f, " file %u line %lld col %u", in->a, (long long)in->imm, in->b);
		break;
	case tgOp_Scope:
		fprintf(f, " %lld", (long long)in->imm);
		break;
	case tgOp_IConst:
		fprintf(f, " %lld", (long long)in->imm);
		break;
	case tgOp_FConst: {
		f64 d = 0;
		if (in->type == tgType_F32) {
			u32 bits = (u32)in->imm;
			f32 x;
			memcpy(&x, &bits, 4);
			d = x;
		} else {
			memcpy(&d, &in->imm, 8);
		}
		fprintf(f, " %g", d);
		break;
	}
	case tgOp_Lea:
	case tgOp_Load:
	case tgOp_AtomicLoad:
		fprintf(f, " ");
		tg_dump_mem(p, in->mem, f);
		break;
	case tgOp_Store:
	case tgOp_AtomicStore:
		fprintf(f, " ");
		tg_dump_mem(p, in->mem, f);
		fprintf(f, ", v%u", in->a);
		break;
	case tgOp_ICmp:
	case tgOp_FCmp:
		fprintf(f, " %s v%u, v%u", in->aux < 16 ? tg_cond_names[in->aux] : "?", in->a, in->b);
		break;
	case tgOp_Zext: case tgOp_Sext: case tgOp_Trunc:
	case tgOp_SIToF: case tgOp_UIToF: case tgOp_FToSI: case tgOp_FToUI:
	case tgOp_FExt: case tgOp_FTrunc: case tgOp_Bitcast:
		fprintf(f, " v%u.%s", in->a, tg_dump_type_name(in->aux));
		break;
	case tgOp_Select:
		fprintf(f, " v%u ? v%u : v%u", in->a, in->b, in->c);
		break;
	case tgOp_MemCopy:
	case tgOp_MemMove:
		fprintf(f, " [v%u] <- [v%u], %lld", in->a, in->b, (long long)in->imm);
		break;
	case tgOp_MemZero:
		fprintf(f, " ");
		tg_dump_mem(p, in->mem, f);
		fprintf(f, ", %lld", (long long)in->imm);
		break;
	case tgOp_MemCopyDyn:
	case tgOp_MemMoveDyn:
	case tgOp_MemSetDyn:
		fprintf(f, " v%u, v%u, v%u", in->a, in->b, in->c);
		break;
	case tgOp_Call:
	case tgOp_Ret:
	case tgOp_Syscall:
		if (in->imm >= 0 && in->imm < p->calls.count) tg_dump_call(p, &p->calls.data[in->imm], f);
		break;
	case tgOp_Jump:
		fprintf(f, " b%lld", (long long)in->imm);
		break;
	case tgOp_Branch:
		fprintf(f, " v%u, b%lld, b%u", in->a, (long long)in->imm, in->c);
		break;
	case tgOp_AtomicRmw:
		fprintf(f, " %s ", in->aux < 7 ? tg_rmw_names[in->aux] : "?");
		tg_dump_mem(p, in->mem, f);
		fprintf(f, ", v%u", in->a);
		break;
	case tgOp_AtomicCas:
		fprintf(f, " ");
		tg_dump_mem(p, in->mem, f);
		fprintf(f, ", v%u, v%u -> ok v%u", in->a, in->b, in->c);
		break;
	case tgOp_Alloca:
		fprintf(f, " v%u align %lld", in->a, (long long)in->imm);
		break;
	case tgOp_TlsAddr:
		fprintf(f, " sym %lld", (long long)in->imm);
		break;
	case tgOp_X87: {
		static char const *const x87_names[] = {"add", "sub", "mul", "div", "neg", "cmp", "fromsi", "fromui", "fromf", "tosi", "toui", "tof"};
		fprintf(f, ".%s", in->aux < 12 ? x87_names[in->aux] : "?");
		if (in->aux == tgX87_Cmp) fprintf(f, " %s", in->imm >= 0 && in->imm < 16 ? tg_cond_names[in->imm] : "?");
		if (in->a) fprintf(f, " [v%u]", in->a);
		if (in->b) fprintf(f, ", [v%u]", in->b);
		if (in->c) fprintf(f, ", %sv%u%s", in->aux == tgX87_Cmp || in->aux <= tgX87_Div ? "[" : "", in->c, in->aux == tgX87_Cmp || in->aux <= tgX87_Div ? "]" : "");
		break;
	}
	default:
		if (in->a) fprintf(f, " v%u", in->a);
		if (in->b) fprintf(f, ", v%u", in->b);
		if (in->c) fprintf(f, ", v%u", in->c);
		if (in->imm) fprintf(f, " imm %lld", (long long)in->imm);
		if (in->mem.kind != tgMem_None) {
			fprintf(f, " ");
			tg_dump_mem(p, in->mem, f);
		}
		break;
	}
	fprintf(f, "\n");
}

// The placed blocks in order, then the blocks that were never placed (dead code).
TG_INTERNAL void tg_dump_proc(tgProc *p, FILE *f) {
	isize i, j;
	fprintf(f, "proc %.*s", (int)p->name.len, p->name.text);
	if (p->debug_name.len) fprintf(f, " (%.*s)", (int)p->debug_name.len, p->debug_name.text);
	if (p->fail_reason) fprintf(f, " FAILED: %s", p->fail_reason);
	fprintf(f, "\n");
	for (i = 0; i < p->locals.count; i++) {
		tgLocal const *l = &p->locals.data[i];
		fprintf(f, "  local%d: size %lld align %lld", (int)i, (long long)l->size, (long long)l->align);
		if (l->over_align) fprintf(f, " over_align %lld", (long long)l->over_align);
		if ((i32)i == p->sret_local) fprintf(f, " (sret)");
		if ((i32)i == p->va_save_local) fprintf(f, " (va save area)");
		fprintf(f, "\n");
	}
	for (i = 0; i < p->params_in.count; i++) {
		tgParamIn const *in = &p->params_in.data[i];
		fprintf(f, "  param_in %s", in->loc == tgLoc_Gpr ? "gpr" : in->loc == tgLoc_Xmm ? "xmm" : "stack");
		if (in->loc == tgLoc_Stack) fprintf(f, " @%d", in->stack_offset);
		else fprintf(f, " r%d", in->reg);
		fprintf(f, " %s/%d -> ", tg_dump_type_name(in->type), in->size);
		tg_dump_mem(p, in->dst, f);
		fprintf(f, "\n");
	}
	for (i = 0; i < p->param_vals.count; i++) {
		fprintf(f, "  param %d = v%u\n", (int)i, p->param_vals.data[i]);
	}
	for (i = 0; i < p->debug_vars.count; i++) {
		tgDebugVar const *v = &p->debug_vars.data[i];
		fprintf(f, "  %s %.*s scope %d line %d: ", v->is_param ? "param" : "var", (int)v->name.len, v->name.text, v->scope, v->line);
		if (v->local >= 0) fprintf(f, "local%d%+d\n", v->local, v->offset);
		else fprintf(f, "sym%d%+d\n", v->sym, v->offset);
	}
	if (p->debug_scope_parent.count > 1) {
		fprintf(f, "  scope parents:");
		for (i = 0; i < p->debug_scope_parent.count; i++) fprintf(f, " %d", p->debug_scope_parent.data[i]);
		fprintf(f, "\n");
	}
	for (i = 0; i < p->order.count; i++) {
		tgBlock *b = p->order.data[i];
		fprintf(f, "  b%d: (scope %d)\n", b->index, b->debug_scope);
		for (j = 0; j < b->instrs.count; j++) tg_dump_instr(p, &b->instrs.data[j], f);
	}
	for (i = 0; i < p->blocks.count; i++) {
		tgBlock *b = p->blocks.data[i];
		if (b->placed) continue;
		fprintf(f, "  b%d: (never placed)\n", b->index);
		for (j = 0; j < b->instrs.count; j++) tg_dump_instr(p, &b->instrs.data[j], f);
	}
}
