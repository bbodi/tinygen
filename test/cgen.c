// Builds data, procedures and calls through the public API into one module, written as C
// source for the host or as an object for a native target. cgen_main.c checks the results,
// so the same main runs against both backends.
//
//   cc -std=c99 -Iinclude test/cgen.c src/tinygen.c -o cgen
//   ./cgen out.c [c|linux-x64|linux-arm64|macos-arm64|macos-x64|windows]
//   cc -std=c99 -Wall -Wextra out.c test/cgen_main.c -o cgen_main && ./cgen_main
//
// "c", the default, writes C (TYG_OUTPUT_C) for the host target.
#include "tinygen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static tyg_module *m;
static tyg_file file;

static void check_at(int failed, int line) {
	if (failed) {
		fprintf(stderr, "cgen.c:%d: %s\n", line, tyg_error(m));
		exit(1);
	}
}
#define check(x) check_at((x), __LINE__)

static const tyg_type *ty(tyg_scalar s) { return tyg_type_scalar(m, s); }
static tyg_val k32(tyg_proc *p, long long v) { return tyg_iconst(p, TYG_I32, v); }
static tyg_val k64(tyg_proc *p, long long v) { return tyg_iconst(p, TYG_I64, v); }
static tyg_arg V(tyg_val v) { tyg_arg a; memset(&a, 0, sizeof(a)); a.val = v; return a; }
static tyg_arg M(tyg_addr at) { tyg_arg a; memset(&a, 0, sizeof(a)); a.mem = at; return a; }

// A signature of up to three parameters: the first NULL ends them.
static tyg_sig *sig(const tyg_type *ret, const tyg_type *p0, const tyg_type *p1, const tyg_type *p2, unsigned flags) {
	const tyg_type *ps[3] = {p0, p1, p2};
	int n = p0 ? p1 ? p2 ? 3 : 2 : 1 : 0;
	return tyg_sig_new(m, ret, ps, n, flags);
}

static tyg_proc *begin(const char *name, unsigned flags, tyg_sig *s) {
	return tyg_proc_begin(m, tyg_symbol(m, name, TYG_SYM_FUNC | flags), s);
}

static tyg_sym data(const char *name, unsigned flags, tyg_section section, const void *bytes, int64_t size, int64_t align) {
	tyg_sym s = tyg_symbol(m, name, flags);
	check(tyg_data(m, s, section, bytes, size, align));
	return s;
}

// Step 1: data. cd_blob holds &puts, bytes, &cd_greeting[3]; cd_ptrs points at what
// cgen_main.c cannot name: a static, a data symbol and a procedure whose names are not C
// identifiers; and at cd_ext, which this module leaves undefined and cgen_main.c defines.
static void build_data(void) {
	static const char greeting[] = "hello, tinygen";
	unsigned char blob[24], odd[3] = {1, 2, 3}, ptrs[32] = {0};
	int local = 42;
	long long weak = 7;
	for (int i = 0; i < 24; i++) blob[i] = (unsigned char)(0xa0 + i);
	tyg_sym str = data("cd_greeting", TYG_SYM_GLOBAL, TYG_SECTION_RODATA, greeting, sizeof(greeting), 1);
	tyg_sym bl = data("cd_blob", TYG_SYM_GLOBAL, TYG_SECTION_DATA, blob, sizeof(blob), 8);
	check(tyg_data_reloc(m, bl, 0, 8, tyg_symbol(m, "puts", TYG_SYM_FUNC | TYG_SYM_FOREIGN | TYG_SYM_GLOBAL), 0));
	check(tyg_data_reloc(m, bl, 16, 8, str, 3));
	data("cd_zeros", TYG_SYM_GLOBAL, TYG_SECTION_BSS, NULL, 64, 16);
	data("cd_tls", TYG_SYM_GLOBAL, TYG_SECTION_TBSS, NULL, 32, 8);
	data("cd_weak", TYG_SYM_GLOBAL | TYG_SYM_WEAK, TYG_SECTION_DATA, &weak, sizeof(weak), 8);
	tyg_sym pt = data("cd_ptrs", TYG_SYM_GLOBAL, TYG_SECTION_RODATA, ptrs, sizeof(ptrs), 8);
	check(tyg_data_reloc(m, pt, 0, 8, data("cd_local", 0, TYG_SECTION_DATA, &local, sizeof(local), 4), 0));
	check(tyg_data_reloc(m, pt, 8, 8, data("cd.odd$name", TYG_SYM_GLOBAL, TYG_SECTION_DATA, odd, sizeof(odd), 1), 1));
	check(tyg_data_reloc(m, pt, 16, 8, tyg_symbol(m, "tg.dot$name", TYG_SYM_FUNC | TYG_SYM_GLOBAL), 0));
	check(tyg_data_reloc(m, pt, 24, 8, tyg_symbol(m, "cd_ext", TYG_SYM_GLOBAL), 0));
}

// Step 2: procedure bodies.

// T name(T a, T b) { return a op b; }
static void build_bin(const char *name, tyg_scalar s, tyg_binop op) {
	tyg_proc *p = begin(name, TYG_SYM_GLOBAL, sig(ty(s), ty(s), ty(s), NULL, 0));
	tyg_ret(p, tyg_bin(p, op, tyg_param(p, 0), tyg_param(p, 1)));
	check(tyg_proc_end(p));
}

static void build_un(const char *name, tyg_scalar s, tyg_unop op) {
	tyg_proc *p = begin(name, TYG_SYM_GLOBAL, sig(ty(s), ty(s), NULL, NULL, 0));
	tyg_ret(p, tyg_un(p, op, tyg_param(p, 0)));
	check(tyg_proc_end(p));
}

// int8_t name(T a, T b) { return a cond b; }
static void build_cmp(const char *name, tyg_scalar s, tyg_cond c) {
	tyg_proc *p = begin(name, TYG_SYM_GLOBAL, sig(ty(TYG_I8), ty(s), ty(s), NULL, 0));
	tyg_ret(p, tyg_cmp(p, c, tyg_param(p, 0), tyg_param(p, 1)));
	check(tyg_proc_end(p));
}

static void build_conv(const char *name, tyg_conv op, tyg_scalar from, tyg_scalar to) {
	tyg_proc *p = begin(name, TYG_SYM_GLOBAL, sig(ty(to), ty(from), NULL, NULL, 0));
	tyg_ret(p, tyg_convert(p, op, to, tyg_param(p, 0)));
	check(tyg_proc_end(p));
}

static const char *const bin_names[] = {"add", "sub", "mul", "sdiv", "udiv", "srem", "urem", "and", "or", "xor", "shl", "lshr", "ashr"};
static const tyg_binop bin_ops[] = {TYG_ADD, TYG_SUB, TYG_MUL, TYG_SDIV, TYG_UDIV, TYG_SREM, TYG_UREM, TYG_AND, TYG_OR, TYG_XOR, TYG_SHL, TYG_LSHR, TYG_ASHR};
static const char *const cond_names[] = {"eq", "ne", "slt", "sle", "sgt", "sge", "ult", "ule", "ugt", "uge", "feq", "fne", "flt", "fle", "fgt", "fge"};

static void build_ops(void) {
	char name[64];
	for (int i = 0; i < 13; i++) {
		snprintf(name, sizeof(name), "tg_b8_%s", bin_names[i]);
		build_bin(name, TYG_I8, bin_ops[i]);
		snprintf(name, sizeof(name), "tg_b64_%s", bin_names[i]);
		build_bin(name, TYG_I64, bin_ops[i]);
	}
	build_bin("tg_f_add", TYG_F64, TYG_FADD);
	build_bin("tg_f_sub", TYG_F64, TYG_FSUB);
	build_bin("tg_f_mul", TYG_F32, TYG_FMUL);
	build_bin("tg_f_div", TYG_F32, TYG_FDIV);
	build_un("tg_u8_neg", TYG_I8, TYG_NEG);
	build_un("tg_u8_not", TYG_I8, TYG_NOT);
	build_un("tg_u64_neg", TYG_I64, TYG_NEG);
	build_un("tg_u64_not", TYG_I64, TYG_NOT);
	build_un("tg_uf_neg", TYG_F64, TYG_FNEG);
	for (int c = 0; c < 10; c++) {
		snprintf(name, sizeof(name), "tg_c8_%s", cond_names[c]);
		build_cmp(name, TYG_I8, (tyg_cond)(TYG_EQ + c));
		snprintf(name, sizeof(name), "tg_c32_%s", cond_names[c]);
		build_cmp(name, TYG_I32, (tyg_cond)(TYG_EQ + c));
	}
	for (int c = 10; c < 16; c++) {
		snprintf(name, sizeof(name), "tg_cf_%s", cond_names[c]);
		build_cmp(name, TYG_F64, (tyg_cond)(TYG_EQ + c));
	}
	build_conv("tg_sext_8_64", TYG_SEXT, TYG_I8, TYG_I64);
	build_conv("tg_zext_16_32", TYG_ZEXT, TYG_I16, TYG_I32);
	build_conv("tg_trunc_64_8", TYG_TRUNC, TYG_I64, TYG_I8);
	build_conv("tg_sitof_8_32", TYG_SITOF, TYG_I8, TYG_F32);
	build_conv("tg_uitof_64_64", TYG_UITOF, TYG_I64, TYG_F64);
	build_conv("tg_uitof_32_32", TYG_UITOF, TYG_I32, TYG_F32);
	build_conv("tg_ftosi_32_8", TYG_FTOSI, TYG_F32, TYG_I8);
	build_conv("tg_ftosi_64_64", TYG_FTOSI, TYG_F64, TYG_I64);
	build_conv("tg_ftoui_64_64", TYG_FTOUI, TYG_F64, TYG_I64);
	build_conv("tg_ftoui_64_32", TYG_FTOUI, TYG_F64, TYG_I32);
	build_conv("tg_fext", TYG_FEXT, TYG_F32, TYG_F64);
	build_conv("tg_ftrunc", TYG_FTRUNC, TYG_F64, TYG_F32);
	build_conv("tg_bitcast_f64_i64", TYG_BITCAST, TYG_F64, TYG_I64);
	build_conv("tg_bitcast_i32_f32", TYG_BITCAST, TYG_I32, TYG_F32);
}

// int64_t tg_select(int32_t c, int64_t a, int64_t b) { return c ? a : b; }
// double tg_fconsts(int32_t i): i == 0: 3.14159, 1: -0.0, 2: inf (f32), else nan
static void build_select(void) {
	tyg_proc *p = begin("tg_select", TYG_SYM_GLOBAL, sig(ty(TYG_I64), ty(TYG_I32), ty(TYG_I64), ty(TYG_I64), 0));
	tyg_ret(p, tyg_select(p, tyg_param(p, 0), tyg_param(p, 1), tyg_param(p, 2)));
	check(tyg_proc_end(p));
	p = begin("tg_fconsts", TYG_SYM_GLOBAL, sig(ty(TYG_F64), ty(TYG_I32), NULL, NULL, 0));
	tyg_val i = tyg_param(p, 0);
	double inf = 1e308 * 10;
	tyg_val r = tyg_fconst(p, TYG_F64, inf - inf);
	r = tyg_select(p, tyg_cmp(p, TYG_EQ, i, k32(p, 2)), tyg_convert(p, TYG_FEXT, TYG_F64, tyg_fconst(p, TYG_F32, inf)), r);
	r = tyg_select(p, tyg_cmp(p, TYG_EQ, i, k32(p, 1)), tyg_fconst(p, TYG_F64, -0.0), r);
	r = tyg_select(p, tyg_cmp(p, TYG_EQ, i, k32(p, 0)), tyg_fconst(p, TYG_F64, 3.14159), r);
	tyg_ret(p, r);
	check(tyg_proc_end(p));
}

// int32_t tg_counter;
// int32_t tg_volatile(int32_t *q, int32_t v) { *(volatile)&q[1] = v + 1; tg_counter += *(volatile)&q[1]; return tg_counter; }
// int64_t tg_mem(const int64_t *src): copy src[0..4) to a local l, zero l[1], move l[1..3) to
// l[2..4), return l[1] + l[0] + l[1] + l[2] + l[3]; with a debug variable and #line
// void tg_fill(char *dst, const char *src, int32_t byte, int64_t n) { memcpy(dst, src, n); memset(dst + n, byte, n); }
static void build_memory(void) {
	const tyg_type *ptr = tyg_type_ptr(m), *i32 = ty(TYG_I32), *i64 = ty(TYG_I64);
	int zero = 0;
	tyg_sym counter = data("tg_counter", TYG_SYM_GLOBAL, TYG_SECTION_DATA, &zero, 4, 4);
	tyg_proc *p = begin("tg_volatile", TYG_SYM_GLOBAL, sig(i32, ptr, i32, NULL, 0));
	tyg_addr q1 = tyg_at_val(tyg_param(p, 0), 4);
	tyg_store(p, TYG_I32, q1, tyg_bin(p, TYG_ADD, tyg_param(p, 1), k32(p, 1)), TYG_MEM_VOLATILE);
	tyg_val x = tyg_load(p, TYG_I32, q1, TYG_MEM_VOLATILE);
	tyg_val c = tyg_load(p, TYG_I32, tyg_at_sym(counter, 0), TYG_MEM_VOLATILE);
	tyg_store(p, TYG_I32, tyg_at_sym(counter, 0), tyg_bin(p, TYG_ADD, c, x), TYG_MEM_VOLATILE);
	tyg_ret(p, tyg_load(p, TYG_I32, tyg_at_sym(counter, 0), 0));
	check(tyg_proc_end(p));

	p = begin("tg_mem", TYG_SYM_GLOBAL, sig(i64, ptr, NULL, NULL, 0));
	tyg_local l = tyg_local_new(p, 32, 32);
	tyg_debug_var(p, "l", tyg_dt_base(m, "long long", 8, TYG_DT_SIGNED), tyg_at_local(l, 0), file, 30);
	tyg_loc(p, file, 31, 1);
	tyg_copy(p, tyg_at_local(l, 0), tyg_at_val(tyg_param(p, 0), 0), 32);
	tyg_zero(p, tyg_at_local(l, 8), 8);
	tyg_val s = tyg_load(p, TYG_I64, tyg_at_local(l, 8), 0);
	tyg_move(p, tyg_at_local(l, 16), tyg_at_local(l, 8), 16);
	for (int i = 0; i < 4; i++) s = tyg_bin(p, TYG_ADD, s, tyg_load(p, TYG_I64, tyg_at_local(l, 8 * i), 0));
	tyg_ret(p, s);
	check(tyg_proc_end(p));

	// no tyg_ret: the builder returns at the end
	const tyg_type *params[4] = {ptr, ptr, i32, i64};
	p = tyg_proc_begin(m, tyg_symbol(m, "tg_fill", TYG_SYM_FUNC | TYG_SYM_GLOBAL), tyg_sig_new(m, NULL, params, 4, 0));
	tyg_val dst = tyg_param(p, 0), n = tyg_param(p, 3);
	tyg_copy_dyn(p, dst, tyg_param(p, 1), n);
	tyg_set_dyn(p, tyg_bin(p, TYG_ADD, dst, n), tyg_param(p, 2), n);
	check(tyg_proc_end(p));
}

// int32_t tg_trap_if(int32_t x) { if (x == 12345) trap; return x * 2; }
// void tg_store_void(int64_t *q, int64_t v) { *q = v; return; }
// char *tg_ptr_add(char *q, int64_t n) { return q + n; }
// struct R { int32_t a; int64_t b; }; int64_t tg_rec_sum(struct R r, int32_t k) { return r.a + r.b * k; }
static void build_misc(void) {
	const tyg_type *ptr = tyg_type_ptr(m), *i32 = ty(TYG_I32), *i64 = ty(TYG_I64);
	tyg_proc *p = begin("tg_trap_if", TYG_SYM_GLOBAL, sig(i32, i32, NULL, NULL, 0));
	tyg_loc(p, file, 40, 1);
	tyg_block bad = tyg_block_new(p), ok = tyg_block_new(p);
	tyg_branch(p, tyg_cmp(p, TYG_EQ, tyg_param(p, 0), k32(p, 12345)), bad, ok);
	tyg_block_set(p, bad);
	tyg_trap(p);
	tyg_block_set(p, ok);
	tyg_ret(p, tyg_bin(p, TYG_MUL, tyg_param(p, 0), k32(p, 2)));
	check(tyg_proc_end(p));

	p = begin("tg_store_void", TYG_SYM_GLOBAL, sig(ty(TYG_VOID), ptr, i64, NULL, 0));
	tyg_store(p, TYG_I64, tyg_at_val(tyg_param(p, 0), 0), tyg_param(p, 1), 0);
	tyg_ret(p, 0);
	check(tyg_proc_end(p));

	p = begin("tg_ptr_add", TYG_SYM_GLOBAL, sig(ptr, ptr, i64, NULL, 0));
	tyg_ret(p, tyg_bin(p, TYG_ADD, tyg_param(p, 0), tyg_param(p, 1)));
	check(tyg_proc_end(p));

	tyg_field fields[2] = {{NULL, 0}, {NULL, 8}};
	fields[0].type = i32;
	fields[1].type = i64;
	p = begin("tg_rec_sum", TYG_SYM_GLOBAL, sig(i64, tyg_type_record(m, 16, 8, fields, 2), i32, NULL, 0));
	tyg_val rp = tyg_param(p, 0);
	tyg_val a = tyg_convert(p, TYG_SEXT, TYG_I64, tyg_load(p, TYG_I32, tyg_at_val(rp, 0), 0));
	tyg_val b = tyg_load(p, TYG_I64, tyg_at_val(rp, 8), 0);
	tyg_val k = tyg_convert(p, TYG_SEXT, TYG_I64, tyg_param(p, 1));
	tyg_ret(p, tyg_bin(p, TYG_ADD, a, tyg_bin(p, TYG_MUL, b, k)));
	check(tyg_proc_end(p));

	// a static procedure nothing calls, a debug trap, and a name that is not a C identifier
	p = begin("tg_static_helper", 0, sig(i32, NULL, NULL, NULL, 0));
	tyg_ret(p, k32(p, 1));
	check(tyg_proc_end(p));
	p = begin("tg_debug_trap_never_called", TYG_SYM_GLOBAL, sig(NULL, NULL, NULL, NULL, 0));
	tyg_debug_trap(p);
	tyg_unreachable(p);
	check(tyg_proc_end(p));
	p = begin("tg.dot$name", TYG_SYM_GLOBAL, sig(i32, NULL, NULL, NULL, 0));
	tyg_ret(p, k32(p, -5));
	check(tyg_proc_end(p));
}

// Step 3: calls.

// static int cc_twice(int x) { return 2 * x; }   int cc_call_static(int x) { return cc_twice(x) + 1; }
// int (*cc_fptr)(int) = cc_twice;                int cc_call_ind(int x) { return cc_fptr(x) + 3; }
// long long cc_two_sigs(long long a): cc_ext(a, 5) + cc_ext((void *)a, 7), two signatures for
// one symbol, then memcpy (a libc name, mangled) of the sum to a local and back
static void build_calls(void) {
	const tyg_type *i32 = ty(TYG_I32), *i64 = ty(TYG_I64), *ptr = tyg_type_ptr(m);
	static const unsigned char zero8[8];
	tyg_sig *s_ii = sig(i32, i32, NULL, NULL, 0);
	tyg_sym twice = tyg_symbol(m, "cc_twice", TYG_SYM_FUNC);
	tyg_proc *p = tyg_proc_begin(m, twice, s_ii);
	tyg_ret(p, tyg_bin(p, TYG_MUL, tyg_param(p, 0), k32(p, 2)));
	check(tyg_proc_end(p));
	p = begin("cc_call_static", TYG_SYM_GLOBAL, s_ii);
	tyg_arg a = V(tyg_param(p, 0));
	tyg_ret(p, tyg_bin(p, TYG_ADD, tyg_call(p, s_ii, tyg_direct(twice), &a, 1, NULL, NULL), k32(p, 1)));
	check(tyg_proc_end(p));

	tyg_sym fptr = data("cc_fptr", TYG_SYM_GLOBAL, TYG_SECTION_DATA, zero8, 8, 8);
	check(tyg_data_reloc(m, fptr, 0, 8, twice, 0));
	p = begin("cc_call_ind", TYG_SYM_GLOBAL, s_ii);
	a = V(tyg_param(p, 0));
	tyg_val f = tyg_load(p, TYG_I64, tyg_at_sym(fptr, 0), 0);
	tyg_ret(p, tyg_bin(p, TYG_ADD, tyg_call(p, s_ii, tyg_indirect(f), &a, 1, NULL, NULL), k32(p, 3)));
	check(tyg_proc_end(p));

	tyg_sym ext = tyg_symbol(m, "cc_ext", TYG_SYM_FUNC);
	p = begin("cc_two_sigs", TYG_SYM_GLOBAL, sig(i64, i64, NULL, NULL, 0));
	tyg_arg as[3] = {V(tyg_param(p, 0)), V(k64(p, 5)), V(0)};
	tyg_val r1 = tyg_call(p, sig(i64, i64, i64, NULL, 0), tyg_direct(ext), as, 2, NULL, NULL);
	as[1] = V(k64(p, 7));
	tyg_val r2 = tyg_call(p, sig(i64, ptr, i64, NULL, 0), tyg_direct(ext), as, 2, NULL, NULL);
	tyg_local l1 = tyg_local_new(p, 8, 8), l2 = tyg_local_new(p, 8, 8);
	tyg_store(p, TYG_I64, tyg_at_local(l1, 0), tyg_bin(p, TYG_ADD, r1, r2), 0);
	as[0] = V(tyg_addr_of(p, tyg_at_local(l2, 0)));
	as[1] = V(tyg_addr_of(p, tyg_at_local(l1, 0)));
	as[2] = V(k64(p, 8));
	tyg_call(p, sig(ptr, ptr, ptr, i64, 0), tyg_direct(tyg_symbol(m, "memcpy", TYG_SYM_FUNC)), as, 3, NULL, NULL);
	tyg_ret(p, tyg_load(p, TYG_I64, tyg_at_local(l2, 0), 0));
	check(tyg_proc_end(p));
}

// int cc_fmt(char *buf, int n) { return snprintf(buf, n, "%d %d %.2f %d:%s", n, (signed char)-5, 2.5f, n * 2, "ok"); }
// double cc_vmix(int n, ...): n groups of (signed char, double, struct P {int a, b;}, long long)
// summed, plus the first char again through a va_copy. A float vararg is fetched as the
// double it was promoted to: native va_arg of F32 reads half of the slot.
// double cc_call_vmix(void): cc_vmix(2, -3, 2.5f, (P){1, 2}, 10LL, 4, 0.25f, (P){5, 6}, -20LL),
// directly and through a pointer in data
static void build_varargs(void) {
	const tyg_type *i8 = tyg_type_int(m, TYG_I8, 1), *i32 = ty(TYG_I32), *i64 = ty(TYG_I64);
	const tyg_type *f32 = ty(TYG_F32), *f64 = ty(TYG_F64), *ptr = tyg_type_ptr(m);
	tyg_field pf[2] = {{NULL, 0}, {NULL, 4}};
	pf[0].type = pf[1].type = i32;
	const tyg_type *pair = tyg_type_record(m, 8, 4, pf, 2);
	tyg_sym fmt = data("cc_fmtstr", 0, TYG_SECTION_RODATA, "%d %d %.2f %d:%s", 17, 1);
	tyg_sym okstr = data("cc_okstr", 0, TYG_SECTION_RODATA, "ok", 3, 1);
	tyg_proc *p = begin("cc_fmt", TYG_SYM_GLOBAL, sig(i32, ptr, i32, NULL, 0));
	const tyg_type *fvt[5] = {i32, i8, f32, i32, ptr};
	tyg_arg fa[8] = {V(tyg_param(p, 0)), V(tyg_convert(p, TYG_SEXT, TYG_I64, tyg_param(p, 1))),
	                 V(tyg_addr_of(p, tyg_at_sym(fmt, 0))), V(tyg_param(p, 1)), V(tyg_iconst(p, TYG_I8, -5)),
	                 V(tyg_fconst(p, TYG_F32, 2.5)), V(tyg_bin(p, TYG_MUL, tyg_param(p, 1), k32(p, 2))),
	                 V(tyg_addr_of(p, tyg_at_sym(okstr, 0)))};
	tyg_sym snp = tyg_symbol(m, "snprintf", TYG_SYM_FUNC);
	tyg_ret(p, tyg_call(p, sig(i32, ptr, i64, ptr, TYG_SIG_VARIADIC), tyg_direct(snp), fa, 8, fvt, NULL));
	check(tyg_proc_end(p));

	tyg_sym vmix = tyg_symbol(m, "cc_vmix", TYG_SYM_FUNC | TYG_SYM_GLOBAL);
	tyg_sig *s_vmix = sig(f64, i32, NULL, NULL, TYG_SIG_VARIADIC);
	p = tyg_proc_begin(m, vmix, s_vmix);
	int64_t vs = tyg_va_list_size(m);
	tyg_local ap = tyg_local_new(p, vs, 8), ap2 = tyg_local_new(p, vs, 8), acc = tyg_local_new(p, 8, 8), i = tyg_local_new(p, 4, 4);
	tyg_val apv = tyg_addr_of(p, tyg_at_local(ap, 0));
	tyg_va_start(p, apv);
	tyg_va_copy(p, tyg_addr_of(p, tyg_at_local(ap2, 0)), apv);
	tyg_val c0 = tyg_load(p, TYG_I8, tyg_at_val(tyg_va_arg(p, tyg_addr_of(p, tyg_at_local(ap2, 0)), i8), 0), 0);
	tyg_store(p, TYG_F64, tyg_at_local(acc, 0), tyg_convert(p, TYG_SITOF, TYG_F64, c0), 0);
	tyg_store(p, TYG_I32, tyg_at_local(i, 0), k32(p, 0), 0);
	tyg_block head = tyg_block_new(p), body = tyg_block_new(p), done = tyg_block_new(p);
	tyg_jump(p, head);
	tyg_block_set(p, head);
	tyg_branch(p, tyg_cmp(p, TYG_SLT, tyg_load(p, TYG_I32, tyg_at_local(i, 0), 0), tyg_param(p, 0)), body, done);
	tyg_block_set(p, body);
	apv = tyg_addr_of(p, tyg_at_local(ap, 0));
	tyg_val c = tyg_load(p, TYG_I8, tyg_at_val(tyg_va_arg(p, apv, i8), 0), 0);
	tyg_val f = tyg_load(p, TYG_F64, tyg_at_val(tyg_va_arg(p, apv, f64), 0), 0);
	tyg_val pp = tyg_va_arg(p, apv, pair);
	tyg_val pab = tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I32, tyg_at_val(pp, 0), 0), tyg_load(p, TYG_I32, tyg_at_val(pp, 4), 0));
	tyg_val l = tyg_load(p, TYG_I64, tyg_at_val(tyg_va_arg(p, apv, i64), 0), 0);
	tyg_val s = tyg_bin(p, TYG_FADD, tyg_load(p, TYG_F64, tyg_at_local(acc, 0), 0), tyg_convert(p, TYG_SITOF, TYG_F64, c));
	s = tyg_bin(p, TYG_FADD, s, f);
	s = tyg_bin(p, TYG_FADD, s, tyg_convert(p, TYG_SITOF, TYG_F64, pab));
	s = tyg_bin(p, TYG_FADD, s, tyg_convert(p, TYG_SITOF, TYG_F64, l));
	tyg_store(p, TYG_F64, tyg_at_local(acc, 0), s, 0);
	tyg_store(p, TYG_I32, tyg_at_local(i, 0), tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I32, tyg_at_local(i, 0), 0), k32(p, 1)), 0);
	tyg_jump(p, head);
	tyg_block_set(p, done);
	tyg_ret(p, tyg_load(p, TYG_F64, tyg_at_local(acc, 0), 0));
	check(tyg_proc_end(p));

	static const unsigned char zero8[8];
	tyg_sym vptr = data("cc_vptr", 0, TYG_SECTION_DATA, zero8, 8, 8);
	check(tyg_data_reloc(m, vptr, 0, 8, vmix, 0));
	p = begin("cc_call_vmix", TYG_SYM_GLOBAL, sig(f64, NULL, NULL, NULL, 0));
	tyg_local p1 = tyg_local_new(p, 8, 4), p2 = tyg_local_new(p, 8, 4);
	tyg_store(p, TYG_I32, tyg_at_local(p1, 0), k32(p, 1), 0);
	tyg_store(p, TYG_I32, tyg_at_local(p1, 4), k32(p, 2), 0);
	tyg_store(p, TYG_I32, tyg_at_local(p2, 0), k32(p, 5), 0);
	tyg_store(p, TYG_I32, tyg_at_local(p2, 4), k32(p, 6), 0);
	const tyg_type *vt[8] = {i8, f32, pair, i64, i8, f32, pair, i64};
	tyg_arg va[9] = {V(k32(p, 2)), V(tyg_iconst(p, TYG_I8, -3)), V(tyg_fconst(p, TYG_F32, 2.5)), M(tyg_at_local(p1, 0)), V(k64(p, 10)),
	                 V(tyg_iconst(p, TYG_I8, 4)), V(tyg_fconst(p, TYG_F32, 0.25)), M(tyg_at_local(p2, 0)), V(k64(p, -20))};
	tyg_val r1 = tyg_call(p, s_vmix, tyg_direct(vmix), va, 9, vt, NULL);
	tyg_val r2 = tyg_call(p, s_vmix, tyg_indirect(tyg_load(p, TYG_I64, tyg_at_sym(vptr, 0), 0)), va, 9, vt, NULL);
	tyg_ret(p, tyg_bin(p, TYG_FADD, r1, r2));
	check(tyg_proc_end(p));
}

// struct M { long long a; double b; };  struct B { long long a, b, c; };
// struct M cc_mk_mix(long long a, double b) { return (M){a, b}; }
// struct B cc_mk_big(long long a) { return (B){a, a + 1, a + 2}; }
// long long cc_sum_big(struct B b, struct M m) { return b.a + b.b + b.c + m.a + (long long)m.b; }
// long long cc_use_structs(long long a): the results of cc_mk_big(a) and cc_mk_mix(a, 0.5) in
// locals passed on to cc_sum_big; cc_mk_big(a) once more with its result ignored, cc_note(a)
static void build_structs(void) {
	const tyg_type *i64 = ty(TYG_I64), *f64 = ty(TYG_F64);
	tyg_field mf[2] = {{NULL, 0}, {NULL, 8}}, bf[3] = {{NULL, 0}, {NULL, 8}, {NULL, 16}};
	mf[0].type = bf[0].type = bf[1].type = bf[2].type = i64;
	mf[1].type = f64;
	const tyg_type *mix = tyg_type_record(m, 16, 8, mf, 2), *big = tyg_type_record(m, 24, 8, bf, 3);
	tyg_sym mkmix = tyg_symbol(m, "cc_mk_mix", TYG_SYM_FUNC | TYG_SYM_GLOBAL), mkbig = tyg_symbol(m, "cc_mk_big", TYG_SYM_FUNC | TYG_SYM_GLOBAL);
	tyg_sig *s_mkmix = sig(mix, i64, f64, NULL, 0), *s_mkbig = sig(big, i64, NULL, NULL, 0);
	tyg_proc *p = tyg_proc_begin(m, mkmix, s_mkmix);
	tyg_local r = tyg_local_new(p, 16, 8);
	tyg_store(p, TYG_I64, tyg_at_local(r, 0), tyg_param(p, 0), 0);
	tyg_store(p, TYG_F64, tyg_at_local(r, 8), tyg_param(p, 1), 0);
	tyg_ret_mem(p, tyg_at_local(r, 0));
	check(tyg_proc_end(p));
	p = tyg_proc_begin(m, mkbig, s_mkbig);
	r = tyg_local_new(p, 24, 8);
	for (int k = 0; k < 3; k++) tyg_store(p, TYG_I64, tyg_at_local(r, 8 * k), tyg_bin(p, TYG_ADD, tyg_param(p, 0), k64(p, k)), 0);
	tyg_ret_mem(p, tyg_at_local(r, 0));
	check(tyg_proc_end(p));

	tyg_sym sumbig = tyg_symbol(m, "cc_sum_big", TYG_SYM_FUNC | TYG_SYM_GLOBAL);
	tyg_sig *s_sumbig = sig(i64, big, mix, NULL, 0);
	p = tyg_proc_begin(m, sumbig, s_sumbig);
	tyg_val b = tyg_param(p, 0), mm = tyg_param(p, 1), s = tyg_load(p, TYG_I64, tyg_at_val(mm, 0), 0);
	for (int k = 0; k < 3; k++) s = tyg_bin(p, TYG_ADD, s, tyg_load(p, TYG_I64, tyg_at_val(b, 8 * k), 0));
	tyg_ret(p, tyg_bin(p, TYG_ADD, s, tyg_convert(p, TYG_FTOSI, TYG_I64, tyg_load(p, TYG_F64, tyg_at_val(mm, 8), 0))));
	check(tyg_proc_end(p));

	p = begin("cc_use_structs", TYG_SYM_GLOBAL, sig(i64, i64, NULL, NULL, 0));
	tyg_local lb = tyg_local_new(p, 24, 8), lm = tyg_local_new(p, 16, 8);
	tyg_addr ab = tyg_at_local(lb, 0), am = tyg_at_local(lm, 0);
	tyg_arg a[2] = {V(tyg_param(p, 0)), V(tyg_fconst(p, TYG_F64, 0.5))};
	tyg_call(p, s_mkbig, tyg_direct(mkbig), a, 1, NULL, &ab);
	tyg_call(p, s_mkbig, tyg_direct(mkbig), a, 1, NULL, NULL);
	tyg_call(p, s_mkmix, tyg_direct(mkmix), a, 2, NULL, &am);
	tyg_call(p, sig(ty(TYG_VOID), i64, NULL, NULL, 0), tyg_direct(tyg_symbol(m, "cc_note", TYG_SYM_FUNC)), a, 1, NULL, NULL);
	tyg_arg sa[2] = {M(ab), M(am)};
	tyg_ret(p, tyg_call(p, s_sumbig, tyg_direct(sumbig), sa, 2, NULL, NULL));
	check(tyg_proc_end(p));
}

// long long cc_sj(long long x): a local and a value live across setjmp(cc_jmpbuf); the first
// time it counts the local up and calls cc_jump(cc_jmpbuf, 7), which longjmps back
static void build_setjmp(void) {
	const tyg_type *i32 = ty(TYG_I32), *i64 = ty(TYG_I64), *ptr = tyg_type_ptr(m);
	tyg_sym jb = data("cc_jmpbuf", 0, TYG_SECTION_BSS, NULL, 512, 16);
	tyg_proc *p = begin("cc_sj", TYG_SYM_GLOBAL, sig(i64, i64, NULL, NULL, 0));
	tyg_local cnt = tyg_local_new(p, 8, 8);
	tyg_val k = tyg_bin(p, TYG_MUL, tyg_param(p, 0), k64(p, 3));
	tyg_store(p, TYG_I64, tyg_at_local(cnt, 0), tyg_param(p, 0), 0);
	tyg_arg a[2] = {V(tyg_addr_of(p, tyg_at_sym(jb, 0))), V(k32(p, 7))};
	tyg_sym sj = tyg_symbol(m, "setjmp", TYG_SYM_FUNC);
	tyg_val r = tyg_call(p, sig(i32, ptr, NULL, NULL, TYG_SIG_RETURNS_TWICE), tyg_direct(sj), a, 1, NULL, NULL);
	tyg_block first = tyg_block_new(p), back = tyg_block_new(p);
	tyg_branch(p, tyg_cmp(p, TYG_EQ, r, k32(p, 0)), first, back);
	tyg_block_set(p, first);
	tyg_store(p, TYG_I64, tyg_at_local(cnt, 0), tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I64, tyg_at_local(cnt, 0), 0), k64(p, 1)), 0);
	tyg_sym jump = tyg_symbol(m, "cc_jump", TYG_SYM_FUNC);
	tyg_call(p, sig(ty(TYG_VOID), ptr, i32, NULL, TYG_SIG_NORETURN), tyg_direct(jump), a, 2, NULL, NULL);
	tyg_block_set(p, back);
	tyg_val s = tyg_bin(p, TYG_ADD, k, tyg_convert(p, TYG_SEXT, TYG_I64, r));
	tyg_ret(p, tyg_bin(p, TYG_ADD, s, tyg_load(p, TYG_I64, tyg_at_local(cnt, 0), 0)));
	check(tyg_proc_end(p));
}

int main(int argc, char **argv) {
	char const *out = argc > 1 ? argv[1] : "cgen-c.c";
	char const *target = argc > 2 ? argv[2] : "c";
	static const struct { const char *name; tyg_arch arch; tyg_os os; } targets[] = {
		{"linux-x64", TYG_ARCH_X64, TYG_OS_LINUX}, {"linux-arm64", TYG_ARCH_ARM64, TYG_OS_LINUX},
		{"macos-arm64", TYG_ARCH_ARM64, TYG_OS_DARWIN}, {"macos-x64", TYG_ARCH_X64, TYG_OS_DARWIN},
		{"windows", TYG_ARCH_X64, TYG_OS_WINDOWS},
	};
	tyg_target t = tyg_host_target();
	t.output = TYG_OUTPUT_C;
	if (strcmp(target, "c") != 0) {
		size_t i = 0;
		while (i < sizeof(targets) / sizeof(targets[0]) && strcmp(target, targets[i].name) != 0) i++;
		if (i == sizeof(targets) / sizeof(targets[0])) {
			fprintf(stderr, "usage: cgen out [c|linux-x64|linux-arm64|macos-arm64|macos-x64|windows]\n");
			return 2;
		}
		t.arch = targets[i].arch;
		t.os = targets[i].os;
		t.pic = t.debug_info = 1;
		t.output = TYG_OUTPUT_OBJECT;
	}
	m = tyg_module_new(t, "cgen.tg");
	if (!m) { fprintf(stderr, "no module\n"); return 1; }
	file = tyg_debug_file(m, "cgen.tg");
	build_ops();
	build_select();
	build_memory();
	build_misc();
	build_data();
	build_calls();
	build_varargs();
	build_structs();
	build_setjmp();
	check(tyg_module_write(m, out));
	tyg_module_free(m);
	return 0;
}
