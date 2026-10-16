// Builds a few functions through the public API into an object for one target.
// smoke_main.c calls them and checks the results.
//
//   cc -std=c99 -Iinclude test/smoke.c src/tinygen.c -o smoke
//   ./smoke smoke.o [linux-x64|linux-arm64|macos-arm64|macos-x64|windows|windows-dwarf]
//
// Without a target it builds for linux-x64. "linux" is linux-x64.
#include "tinygen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static tyg_module *m;
static tyg_file file;
static int windows;
static int is_elf;

static void check(int failed) {
	if (failed) {
		fprintf(stderr, "tinygen: %s\n", tyg_error(m));
		exit(1);
	}
}

static const tyg_type *i32t(void) { return tyg_type_scalar(m, TYG_I32); }
static const tyg_type *i64t(void) { return tyg_type_scalar(m, TYG_I64); }
static const tyg_type *f32t(void) { return tyg_type_scalar(m, TYG_F32); }
static const tyg_type *f64t(void) { return tyg_type_scalar(m, TYG_F64); }

static tyg_arg val_arg(tyg_val v) { tyg_arg a = {0, {TYG_ADDR_VAL, 0, 0, 0}}; a.val = v; return a; }

// int add(int a, int b) { return a + b; }
// int tg_abort_me(void) { return 7; }, after a first attempt that is abandoned halfway
static void build_abort(void) {
	tyg_sig *sig = tyg_sig_new(m, i32t(), NULL, 0, 0);
	tyg_sym sym = tyg_symbol(m, "tg_abort_me", TYG_SYM_FUNC | TYG_SYM_GLOBAL);
	tyg_proc *p = tyg_proc_begin(m, sym, sig);
	tyg_val x = tyg_bin(p, TYG_ADD, tyg_iconst(p, TYG_I32, 1), tyg_iconst(p, TYG_I32, 2));
	(void)x;
	tyg_proc_abort(p);
	p = tyg_proc_begin(m, sym, sig);
	tyg_ret(p, tyg_iconst(p, TYG_I32, 7));
	check(tyg_proc_end(p));
}

static void build_add(void) {
	const tyg_type *params[2] = {i32t(), i32t()};
	tyg_sig *sig = tyg_sig_new(m, i32t(), params, 2, 0);
	// exported from the image on Windows, which puts /EXPORT in .drectve
	tyg_sym sym = tyg_symbol(m, "tg_add", TYG_SYM_FUNC | TYG_SYM_GLOBAL | (windows ? TYG_SYM_EXPORT : 0));
	tyg_proc *p = tyg_proc_begin(m, sym, sig);
	tyg_loc(p, file, 1, 1);
	tyg_val r = tyg_bin(p, TYG_ADD, tyg_param(p, 0), tyg_param(p, 1));
	tyg_ret(p, r);
	check(tyg_proc_end(p));
}

// long long fib(long long n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static void build_fib(void) {
	const tyg_type *params[1] = {i64t()};
	tyg_sig *sig = tyg_sig_new(m, i64t(), params, 1, 0);
	tyg_sym sym = tyg_symbol(m, "tg_fib", TYG_SYM_FUNC | TYG_SYM_GLOBAL);
	tyg_proc *p = tyg_proc_begin(m, sym, sig);
	tyg_loc(p, file, 3, 1);
	tyg_val n = tyg_param(p, 0);
	tyg_block small = tyg_block_new(p), big = tyg_block_new(p);
	tyg_branch(p, tyg_cmp(p, TYG_SLT, n, tyg_iconst(p, TYG_I64, 2)), small, big);
	tyg_block_set(p, small);
	tyg_ret(p, n);
	tyg_block_set(p, big);
	tyg_arg a1 = val_arg(tyg_bin(p, TYG_SUB, n, tyg_iconst(p, TYG_I64, 1)));
	tyg_val f1 = tyg_call(p, sig, tyg_direct(sym), &a1, 1, NULL, NULL);
	tyg_arg a2 = val_arg(tyg_bin(p, TYG_SUB, n, tyg_iconst(p, TYG_I64, 2)));
	tyg_val f2 = tyg_call(p, sig, tyg_direct(sym), &a2, 1, NULL, NULL);
	tyg_ret(p, tyg_bin(p, TYG_ADD, f1, f2));
	check(tyg_proc_end(p));
}

// long long sum(long long n) { long long s = 0; for (long long i = 0; i < n; i++) s += i; return s; }
// with s and i as debug variables in locals
static void build_sum(void) {
	const tyg_type *params[1] = {i64t()};
	tyg_sig *sig = tyg_sig_new(m, i64t(), params, 1, 0);
	tyg_sym sym = tyg_symbol(m, "tg_sum", TYG_SYM_FUNC | TYG_SYM_GLOBAL);
	tyg_proc *p = tyg_proc_begin(m, sym, sig);
	const tyg_dtype *ll_dt = tyg_dt_base(m, "long long", 8, TYG_DT_SIGNED);
	const tyg_dtype *pt[1] = {ll_dt};
	tyg_debug_proc(p, "tg_sum", file, 5, tyg_dt_func(m, ll_dt, pt, 1, 0));
	tyg_loc(p, file, 5, 1);
	tyg_local nl = tyg_local_new(p, 8, 8);
	tyg_store(p, TYG_I64, tyg_at_local(nl, 0), tyg_param(p, 0), 0);
	tyg_debug_param(p, "n", ll_dt, tyg_at_local(nl, 0), file, 5);
	// n_ref reaches n through a pointer, as a captured variable would
	tyg_local nref = tyg_local_new(p, 8, 8);
	tyg_store(p, TYG_I64, tyg_at_local(nref, 0), tyg_addr_of(p, tyg_at_local(nl, 0)), 0);
	tyg_debug_var_ref(p, "n_ref", ll_dt, tyg_at_local(nref, 0), file, 5);
	tyg_local s = tyg_local_new(p, 8, 8), i = tyg_local_new(p, 8, 8);
	tyg_debug_var(p, "s", ll_dt, tyg_at_local(s, 0), file, 5);
	tyg_debug_var(p, "i", ll_dt, tyg_at_local(i, 0), file, 5);
	tyg_store(p, TYG_I64, tyg_at_local(s, 0), tyg_iconst(p, TYG_I64, 0), 0);
	tyg_store(p, TYG_I64, tyg_at_local(i, 0), tyg_iconst(p, TYG_I64, 0), 0);
	tyg_block head = tyg_block_new(p), body = tyg_block_new(p), done = tyg_block_new(p);
	tyg_jump(p, head);
	tyg_block_set(p, head);
	tyg_loc(p, file, 6, 1);
	tyg_val iv = tyg_load(p, TYG_I64, tyg_at_local(i, 0), 0);
	tyg_branch(p, tyg_cmp(p, TYG_SLT, iv, tyg_load(p, TYG_I64, tyg_at_local(nl, 0), 0)), body, done);
	tyg_block_set(p, body);
	tyg_loc(p, file, 7, 1);
	tyg_val sv = tyg_load(p, TYG_I64, tyg_at_local(s, 0), 0);
	tyg_val iv2 = tyg_load(p, TYG_I64, tyg_at_local(i, 0), 0);
	tyg_store(p, TYG_I64, tyg_at_local(s, 0), tyg_bin(p, TYG_ADD, sv, iv2), 0);
	tyg_store(p, TYG_I64, tyg_at_local(i, 0), tyg_bin(p, TYG_ADD, iv2, tyg_iconst(p, TYG_I64, 1)), 0);
	tyg_jump(p, head);
	tyg_block_set(p, done);
	tyg_loc(p, file, 8, 1);
	tyg_ret(p, tyg_load(p, TYG_I64, tyg_at_local(s, 0), 0));
	check(tyg_proc_end(p));
}

// struct V { double x, y; };  struct V vscale(struct V v, double k)
// SysV: two xmm registers each way. Win64: by pointer to a copy, the result through sret.
static void build_vscale(void) {
	tyg_field fields[2] = {{f64t(), 0}, {f64t(), 8}};
	const tyg_type *v = tyg_type_record(m, 16, 8, fields, 2);
	const tyg_type *params[2] = {v, f64t()};
	tyg_sig *sig = tyg_sig_new(m, v, params, 2, 0);
	tyg_sym sym = tyg_symbol(m, "tg_vscale", TYG_SYM_FUNC | TYG_SYM_GLOBAL);
	tyg_proc *p = tyg_proc_begin(m, sym, sig);
	tyg_loc(p, file, 10, 1);
	tyg_val vp = tyg_param(p, 0), k = tyg_param(p, 1);
	tyg_local out = tyg_local_new(p, 16, 8);
	tyg_val x = tyg_load(p, TYG_F64, tyg_at_val(vp, 0), 0);
	tyg_val y = tyg_load(p, TYG_F64, tyg_at_val(vp, 8), 0);
	tyg_store(p, TYG_F64, tyg_at_local(out, 0), tyg_bin(p, TYG_FMUL, x, k), 0);
	tyg_store(p, TYG_F64, tyg_at_local(out, 8), tyg_bin(p, TYG_FMUL, y, k), 0);
	tyg_ret_mem(p, tyg_at_local(out, 0));
	check(tyg_proc_end(p));
}

// struct Big { long long a, b, c; };  long long big_sum(struct Big b) -- on the stack, or by pointer on Win64
// struct Big make_big(long long x) -- returned through sret
static void build_big(void) {
	tyg_field fields[3] = {{i64t(), 0}, {i64t(), 8}, {i64t(), 16}};
	const tyg_type *big = tyg_type_record(m, 24, 8, fields, 3);
	{
		const tyg_type *params[1] = {big};
		tyg_sig *sig = tyg_sig_new(m, i64t(), params, 1, 0);
		tyg_proc *p = tyg_proc_begin(m, tyg_symbol(m, "tg_big_sum", TYG_SYM_FUNC | TYG_SYM_GLOBAL), sig);
		// b as a debug parameter: in the frame, or on Win64 and arm64 through the pointer to the copy
		const tyg_dtype *ll_dt = tyg_dt_base(m, "long long", 8, TYG_DT_SIGNED);
		tyg_dtype *big_dt = tyg_dt_record(m, TYG_DT_STRUCT, "Big", file, 13);
		tyg_dt_member(m, big_dt, "a", ll_dt, 0);
		tyg_dt_member(m, big_dt, "b", ll_dt, 8);
		tyg_dt_member(m, big_dt, "c", ll_dt, 16);
		tyg_dt_record_complete(m, big_dt, 24, 8);
		const tyg_dtype *pt[1] = {big_dt};
		tyg_debug_proc(p, "tg_big_sum", file, 14, tyg_dt_func(m, ll_dt, pt, 1, 0));
		tyg_loc(p, file, 14, 1);
		tyg_val b = tyg_param(p, 0);
		tyg_debug_param(p, "b", big_dt, tyg_at_val(b, 0), file, 14);
		tyg_val s = tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I64, tyg_at_val(b, 0), 0), tyg_load(p, TYG_I64, tyg_at_val(b, 8), 0));
		tyg_ret(p, tyg_bin(p, TYG_ADD, s, tyg_load(p, TYG_I64, tyg_at_val(b, 16), 0)));
		check(tyg_proc_end(p));
	}
	{
		const tyg_type *params[1] = {i64t()};
		tyg_sig *sig = tyg_sig_new(m, big, params, 1, 0);
		tyg_proc *p = tyg_proc_begin(m, tyg_symbol(m, "tg_make_big", TYG_SYM_FUNC | TYG_SYM_GLOBAL), sig);
		tyg_val x = tyg_param(p, 0);
		tyg_local out = tyg_local_new(p, 24, 8);
		tyg_store(p, TYG_I64, tyg_at_local(out, 0), x, 0);
		tyg_store(p, TYG_I64, tyg_at_local(out, 8), tyg_bin(p, TYG_MUL, x, tyg_iconst(p, TYG_I64, 2)), 0);
		tyg_store(p, TYG_I64, tyg_at_local(out, 16), tyg_bin(p, TYG_MUL, x, tyg_iconst(p, TYG_I64, 3)), 0);
		tyg_ret_mem(p, tyg_at_local(out, 0));
		check(tyg_proc_end(p));
	}
}

// const char tg_fmt[] = "%s %d %.2f\n"; const char *tg_msg = "hello";
// int greet(int n) { return printf(tg_fmt, tg_msg, n, 1.5); }
static void build_greet(void) {
	static const char fmt[] = "%s %d %.2f\n";
	static const char msg[] = "hello";
	tyg_sym fmt_sym = tyg_symbol(m, "tg_fmt", TYG_SYM_GLOBAL);
	check(tyg_data(m, fmt_sym, TYG_SECTION_RODATA, fmt, sizeof(fmt), 1));
	tyg_sym msg_text = tyg_symbol(m, ".Lmsg", 0);
	check(tyg_data(m, msg_text, TYG_SECTION_RODATA, msg, sizeof(msg), 1));
	tyg_sym msg_sym = tyg_symbol(m, "tg_msg", TYG_SYM_GLOBAL);
	static const unsigned char zero8[8] = {0};
	check(tyg_data(m, msg_sym, TYG_SECTION_DATA, zero8, 8, 8));
	check(tyg_data_reloc(m, msg_sym, 0, 8, msg_text, 0));

	const tyg_type *pparams[1] = {tyg_type_ptr(m)};
	tyg_sig *printf_sig = tyg_sig_new(m, i32t(), pparams, 1, TYG_SIG_VARIADIC);
	tyg_sym printf_sym = tyg_symbol(m, "printf", TYG_SYM_FUNC | TYG_SYM_GLOBAL | TYG_SYM_FOREIGN);

	const tyg_type *params[1] = {i32t()};
	tyg_sig *sig = tyg_sig_new(m, i32t(), params, 1, 0);
	tyg_proc *p = tyg_proc_begin(m, tyg_symbol(m, "tg_greet", TYG_SYM_FUNC | TYG_SYM_GLOBAL), sig);
	tyg_loc(p, file, 20, 1);
	tyg_arg args[4];
	args[0] = val_arg(tyg_addr_of(p, tyg_at_sym(fmt_sym, 0)));
	args[1] = val_arg(tyg_load(p, TYG_I64, tyg_at_sym(msg_sym, 0), 0));
	args[2] = val_arg(tyg_param(p, 0));
	args[3] = val_arg(tyg_fconst(p, TYG_F64, 1.5));
	const tyg_type *vtypes[3] = {tyg_type_ptr(m), i32t(), f64t()};
	tyg_val r = tyg_call(p, printf_sig, tyg_direct(printf_sym), args, 4, vtypes, NULL);
	tyg_ret(p, r);
	check(tyg_proc_end(p));
}

// int tg_vsum(int count, ...) -- sums `count` int varargs with va_start/va_arg
static void build_vsum(void) {
	const tyg_type *params[1] = {i32t()};
	tyg_sig *sig = tyg_sig_new(m, i32t(), params, 1, TYG_SIG_VARIADIC);
	tyg_proc *p = tyg_proc_begin(m, tyg_symbol(m, "tg_vsum", TYG_SYM_FUNC | TYG_SYM_GLOBAL), sig);
	tyg_local ap = tyg_local_new(p, tyg_va_list_size(m), tyg_va_list_align(m));
	tyg_local s = tyg_local_new(p, 4, 4), i = tyg_local_new(p, 4, 4);
	tyg_val apv = tyg_addr_of(p, tyg_at_local(ap, 0));
	tyg_va_start(p, apv);
	tyg_store(p, TYG_I32, tyg_at_local(s, 0), tyg_iconst(p, TYG_I32, 0), 0);
	tyg_store(p, TYG_I32, tyg_at_local(i, 0), tyg_iconst(p, TYG_I32, 0), 0);
	tyg_block head = tyg_block_new(p), body = tyg_block_new(p), done = tyg_block_new(p);
	tyg_jump(p, head);
	tyg_block_set(p, head);
	tyg_val iv = tyg_load(p, TYG_I32, tyg_at_local(i, 0), 0);
	tyg_branch(p, tyg_cmp(p, TYG_SLT, iv, tyg_param(p, 0)), body, done);
	tyg_block_set(p, body);
	tyg_val at = tyg_va_arg(p, tyg_addr_of(p, tyg_at_local(ap, 0)), i32t());
	tyg_val x = tyg_load(p, TYG_I32, tyg_at_val(at, 0), 0);
	tyg_store(p, TYG_I32, tyg_at_local(s, 0), tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I32, tyg_at_local(s, 0), 0), x), 0);
	tyg_store(p, TYG_I32, tyg_at_local(i, 0), tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I32, tyg_at_local(i, 0), 0), tyg_iconst(p, TYG_I32, 1)), 0);
	tyg_jump(p, head);
	tyg_block_set(p, done);
	tyg_ret(p, tyg_load(p, TYG_I32, tyg_at_local(s, 0), 0));
	check(tyg_proc_end(p));
}

// struct P { int x, y; };  struct Big as above
// double tg_vmix(int n, ...) -- n pairs of (double, struct P) varargs, then one struct Big:
// returns the sum of the doubles, the x and y of each P, and a, b, c of the Big
static void build_vmix(void) {
	tyg_field pf[2] = {{i32t(), 0}, {i32t(), 4}};
	const tyg_type *pt = tyg_type_record(m, 8, 4, pf, 2);
	tyg_field bf[3] = {{i64t(), 0}, {i64t(), 8}, {i64t(), 16}};
	const tyg_type *big = tyg_type_record(m, 24, 8, bf, 3);
	const tyg_type *params[1] = {i32t()};
	tyg_sig *sig = tyg_sig_new(m, f64t(), params, 1, TYG_SIG_VARIADIC);
	tyg_proc *p = tyg_proc_begin(m, tyg_symbol(m, "tg_vmix", TYG_SYM_FUNC | TYG_SYM_GLOBAL), sig);
	tyg_loc(p, file, 30, 1);
	tyg_local ap = tyg_local_new(p, tyg_va_list_size(m), tyg_va_list_align(m));
	tyg_local s = tyg_local_new(p, 8, 8), i = tyg_local_new(p, 4, 4);
	tyg_va_start(p, tyg_addr_of(p, tyg_at_local(ap, 0)));
	tyg_store(p, TYG_F64, tyg_at_local(s, 0), tyg_fconst(p, TYG_F64, 0), 0);
	tyg_store(p, TYG_I32, tyg_at_local(i, 0), tyg_iconst(p, TYG_I32, 0), 0);
	tyg_block head = tyg_block_new(p), body = tyg_block_new(p), done = tyg_block_new(p);
	tyg_jump(p, head);
	tyg_block_set(p, head);
	tyg_branch(p, tyg_cmp(p, TYG_SLT, tyg_load(p, TYG_I32, tyg_at_local(i, 0), 0), tyg_param(p, 0)), body, done);
	tyg_block_set(p, body);
	tyg_val d = tyg_load(p, TYG_F64, tyg_at_val(tyg_va_arg(p, tyg_addr_of(p, tyg_at_local(ap, 0)), f64t()), 0), 0);
	tyg_val pp = tyg_va_arg(p, tyg_addr_of(p, tyg_at_local(ap, 0)), pt);
	tyg_val xy = tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I32, tyg_at_val(pp, 0), 0), tyg_load(p, TYG_I32, tyg_at_val(pp, 4), 0));
	tyg_val acc = tyg_bin(p, TYG_FADD, tyg_load(p, TYG_F64, tyg_at_local(s, 0), 0), d);
	acc = tyg_bin(p, TYG_FADD, acc, tyg_convert(p, TYG_SITOF, TYG_F64, xy));
	tyg_store(p, TYG_F64, tyg_at_local(s, 0), acc, 0);
	tyg_store(p, TYG_I32, tyg_at_local(i, 0), tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I32, tyg_at_local(i, 0), 0), tyg_iconst(p, TYG_I32, 1)), 0);
	tyg_jump(p, head);
	tyg_block_set(p, done);
	tyg_val bp = tyg_va_arg(p, tyg_addr_of(p, tyg_at_local(ap, 0)), big);
	tyg_val bs = tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I64, tyg_at_val(bp, 0), 0), tyg_load(p, TYG_I64, tyg_at_val(bp, 8), 0));
	bs = tyg_bin(p, TYG_ADD, bs, tyg_load(p, TYG_I64, tyg_at_val(bp, 16), 0));
	tyg_ret(p, tyg_bin(p, TYG_FADD, tyg_load(p, TYG_F64, tyg_at_local(s, 0), 0), tyg_convert(p, TYG_SITOF, TYG_F64, bs)));
	check(tyg_proc_end(p));
}

// double tg_call_vmix(void) { return tg_vmix(2, 0.5, (struct P){1, 2}, 0.25, (struct P){3, 4}, (struct Big){5, 6, 7}); }
// aggregate and float varargs from a tinygen caller
static void build_call_vmix(void) {
	tyg_field pf[2] = {{i32t(), 0}, {i32t(), 4}};
	const tyg_type *pt = tyg_type_record(m, 8, 4, pf, 2);
	tyg_field bf[3] = {{i64t(), 0}, {i64t(), 8}, {i64t(), 16}};
	const tyg_type *big = tyg_type_record(m, 24, 8, bf, 3);
	const tyg_type *params[1] = {i32t()};
	tyg_sig *vsig = tyg_sig_new(m, f64t(), params, 1, TYG_SIG_VARIADIC);
	tyg_sig *sig = tyg_sig_new(m, f64t(), NULL, 0, 0);
	tyg_proc *p = tyg_proc_begin(m, tyg_symbol(m, "tg_call_vmix", TYG_SYM_FUNC | TYG_SYM_GLOBAL), sig);
	tyg_local p1 = tyg_local_new(p, 8, 4), p2 = tyg_local_new(p, 8, 4), b = tyg_local_new(p, 24, 8);
	tyg_store(p, TYG_I32, tyg_at_local(p1, 0), tyg_iconst(p, TYG_I32, 1), 0);
	tyg_store(p, TYG_I32, tyg_at_local(p1, 4), tyg_iconst(p, TYG_I32, 2), 0);
	tyg_store(p, TYG_I32, tyg_at_local(p2, 0), tyg_iconst(p, TYG_I32, 3), 0);
	tyg_store(p, TYG_I32, tyg_at_local(p2, 4), tyg_iconst(p, TYG_I32, 4), 0);
	for (int k = 0; k < 3; k++) tyg_store(p, TYG_I64, tyg_at_local(b, 8*k), tyg_iconst(p, TYG_I64, 5+k), 0);
	tyg_arg args[6];
	args[0] = val_arg(tyg_iconst(p, TYG_I32, 2));
	args[1] = val_arg(tyg_fconst(p, TYG_F64, 0.5));
	args[2].val = 0; args[2].mem = tyg_at_local(p1, 0);
	args[3] = val_arg(tyg_fconst(p, TYG_F64, 0.25));
	args[4].val = 0; args[4].mem = tyg_at_local(p2, 0);
	args[5].val = 0; args[5].mem = tyg_at_local(b, 0);
	const tyg_type *vtypes[5] = {f64t(), pt, f64t(), pt, big};
	tyg_val r = tyg_call(p, vsig, tyg_direct(tyg_symbol(m, "tg_vmix", 0)), args, 6, vtypes, NULL);
	tyg_ret(p, r);
	check(tyg_proc_end(p));
}

// double tg_mix(int a, double b, long long c, float d, int e, double f, struct P g)
//   = a + b + c + d + e + f + g.x + g.y -- past the four register slots
static void build_mix(void) {
	tyg_field pf[2] = {{i32t(), 0}, {i32t(), 4}};
	const tyg_type *pt = tyg_type_record(m, 8, 4, pf, 2);
	const tyg_type *params[7] = {i32t(), f64t(), i64t(), f32t(), i32t(), f64t(), pt};
	tyg_sig *sig = tyg_sig_new(m, f64t(), params, 7, 0);
	tyg_proc *p = tyg_proc_begin(m, tyg_symbol(m, "tg_mix", TYG_SYM_FUNC | TYG_SYM_GLOBAL), sig);
	tyg_val acc = tyg_convert(p, TYG_SITOF, TYG_F64, tyg_param(p, 0));
	acc = tyg_bin(p, TYG_FADD, acc, tyg_param(p, 1));
	acc = tyg_bin(p, TYG_FADD, acc, tyg_convert(p, TYG_SITOF, TYG_F64, tyg_param(p, 2)));
	acc = tyg_bin(p, TYG_FADD, acc, tyg_convert(p, TYG_FEXT, TYG_F64, tyg_param(p, 3)));
	acc = tyg_bin(p, TYG_FADD, acc, tyg_convert(p, TYG_SITOF, TYG_F64, tyg_param(p, 4)));
	acc = tyg_bin(p, TYG_FADD, acc, tyg_param(p, 5));
	tyg_val g = tyg_param(p, 6);
	tyg_val xy = tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I32, tyg_at_val(g, 0), 0), tyg_load(p, TYG_I32, tyg_at_val(g, 4), 0));
	acc = tyg_bin(p, TYG_FADD, acc, tyg_convert(p, TYG_SITOF, TYG_F64, xy));
	tyg_ret(p, acc);
	check(tyg_proc_end(p));
}

// struct P tg_pswap(struct P p) { return (struct P){p.y, p.x}; } -- 8 bytes, in rax both ways
// struct C3 { char a, b, c; };  int tg_c3sum(struct C3 c) { return c.a + c.b + c.c; } -- by pointer on Win64
static void build_small_structs(void) {
	tyg_field pf[2] = {{i32t(), 0}, {i32t(), 4}};
	const tyg_type *pt = tyg_type_record(m, 8, 4, pf, 2);
	{
		const tyg_type *params[1] = {pt};
		tyg_sig *sig = tyg_sig_new(m, pt, params, 1, 0);
		tyg_proc *p = tyg_proc_begin(m, tyg_symbol(m, "tg_pswap", TYG_SYM_FUNC | TYG_SYM_GLOBAL), sig);
		tyg_val in = tyg_param(p, 0);
		tyg_local out = tyg_local_new(p, 8, 4);
		tyg_store(p, TYG_I32, tyg_at_local(out, 0), tyg_load(p, TYG_I32, tyg_at_val(in, 4), 0), 0);
		tyg_store(p, TYG_I32, tyg_at_local(out, 4), tyg_load(p, TYG_I32, tyg_at_val(in, 0), 0), 0);
		tyg_ret_mem(p, tyg_at_local(out, 0));
		check(tyg_proc_end(p));
	}
	{
		const tyg_type *i8t = tyg_type_int(m, TYG_I8, 1);
		tyg_field cf[3] = {{i8t, 0}, {i8t, 1}, {i8t, 2}};
		const tyg_type *c3 = tyg_type_record(m, 3, 1, cf, 3);
		const tyg_type *params[1] = {c3};
		tyg_sig *sig = tyg_sig_new(m, i32t(), params, 1, 0);
		tyg_proc *p = tyg_proc_begin(m, tyg_symbol(m, "tg_c3sum", TYG_SYM_FUNC | TYG_SYM_GLOBAL), sig);
		tyg_val c = tyg_param(p, 0);
		tyg_val s = tyg_iconst(p, TYG_I32, 0);
		for (int k = 0; k < 3; k++) {
			s = tyg_bin(p, TYG_ADD, s, tyg_convert(p, TYG_SEXT, TYG_I32, tyg_load(p, TYG_I8, tyg_at_val(c, k), 0)));
		}
		tyg_ret(p, s);
		check(tyg_proc_end(p));
	}
}

// double tg_fkeep(double x) { double y = x * 3; tg_add(1, 2); return y + x; }
// y and x live across a call: Win64 keeps them in callee saved xmm registers
static void build_fkeep(void) {
	const tyg_type *params[1] = {f64t()};
	tyg_sig *sig = tyg_sig_new(m, f64t(), params, 1, 0);
	tyg_proc *p = tyg_proc_begin(m, tyg_symbol(m, "tg_fkeep", TYG_SYM_FUNC | TYG_SYM_GLOBAL), sig);
	tyg_val x = tyg_param(p, 0);
	tyg_val y = tyg_bin(p, TYG_FMUL, x, tyg_fconst(p, TYG_F64, 3.0));
	const tyg_type *aparams[2] = {i32t(), i32t()};
	tyg_sig *asig = tyg_sig_new(m, i32t(), aparams, 2, 0);
	tyg_arg args[2];
	args[0] = val_arg(tyg_iconst(p, TYG_I32, 1));
	args[1] = val_arg(tyg_iconst(p, TYG_I32, 2));
	tyg_call(p, asig, tyg_direct(tyg_symbol(m, "tg_add", 0)), args, 2, NULL, NULL);
	tyg_ret(p, tyg_bin(p, TYG_FADD, y, x));
	check(tyg_proc_end(p));
}

// long long tg_call_cb(long long (*cb)(long long), long long x) { return cb(x) + 1; }
// smoke_main.c's callback may longjmp out, which on Windows unwinds through this frame
static void build_call_cb(void) {
	const tyg_type *cparams[1] = {i64t()};
	tyg_sig *csig = tyg_sig_new(m, i64t(), cparams, 1, 0);
	const tyg_type *params[2] = {tyg_type_ptr(m), i64t()};
	tyg_sig *sig = tyg_sig_new(m, i64t(), params, 2, 0);
	tyg_proc *p = tyg_proc_begin(m, tyg_symbol(m, "tg_call_cb", TYG_SYM_FUNC | TYG_SYM_GLOBAL), sig);
	tyg_loc(p, file, 40, 1);
	tyg_arg a = val_arg(tyg_param(p, 1));
	tyg_val r = tyg_call(p, csig, tyg_indirect(tyg_param(p, 0)), &a, 1, NULL, NULL);
	tyg_ret(p, tyg_bin(p, TYG_ADD, r, tyg_iconst(p, TYG_I64, 1)));
	check(tyg_proc_end(p));
}

// long long tg_bigframe(long long n) { volatile long long a[2500]; a[0] = n; a[2499] = 2*n; return a[0] + a[2499]; }
// a frame past a page: Win64 probes its pages
static void build_bigframe(void) {
	const tyg_type *params[1] = {i64t()};
	tyg_sig *sig = tyg_sig_new(m, i64t(), params, 1, 0);
	tyg_proc *p = tyg_proc_begin(m, tyg_symbol(m, "tg_bigframe", TYG_SYM_FUNC | TYG_SYM_GLOBAL), sig);
	tyg_local a = tyg_local_new(p, 8*2500, 8);
	tyg_val n = tyg_param(p, 0);
	tyg_store(p, TYG_I64, tyg_at_local(a, 0), n, TYG_MEM_VOLATILE);
	tyg_store(p, TYG_I64, tyg_at_local(a, 8*2499), tyg_bin(p, TYG_MUL, n, tyg_iconst(p, TYG_I64, 2)), TYG_MEM_VOLATILE);
	tyg_val r = tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I64, tyg_at_local(a, 0), TYG_MEM_VOLATILE), tyg_load(p, TYG_I64, tyg_at_local(a, 8*2499), TYG_MEM_VOLATILE));
	tyg_ret(p, r);
	check(tyg_proc_end(p));
}

// long long tg_bigalloca(long long n) { volatile char *b = alloca(n); b[0] = 1; b[n-1] = 2; return b[0] + b[n-1]; }
// the bottom of a block many pages long is written first: Win64 probes its pages
static void build_bigalloca(void) {
	const tyg_type *params[1] = {i64t()};
	tyg_sig *sig = tyg_sig_new(m, i64t(), params, 1, 0);
	tyg_proc *p = tyg_proc_begin(m, tyg_symbol(m, "tg_bigalloca", TYG_SYM_FUNC | TYG_SYM_GLOBAL), sig);
	tyg_val n = tyg_param(p, 0);
	tyg_val b = tyg_alloca(p, n, 16);
	tyg_val last = tyg_bin(p, TYG_ADD, b, tyg_bin(p, TYG_SUB, n, tyg_iconst(p, TYG_I64, 1)));
	tyg_store(p, TYG_I8, tyg_at_val(b, 0), tyg_iconst(p, TYG_I8, 1), TYG_MEM_VOLATILE);
	tyg_store(p, TYG_I8, tyg_at_val(last, 0), tyg_iconst(p, TYG_I8, 2), TYG_MEM_VOLATILE);
	tyg_val lo = tyg_convert(p, TYG_ZEXT, TYG_I64, tyg_load(p, TYG_I8, tyg_at_val(b, 0), TYG_MEM_VOLATILE));
	tyg_val hi = tyg_convert(p, TYG_ZEXT, TYG_I64, tyg_load(p, TYG_I8, tyg_at_val(last, 0), TYG_MEM_VOLATILE));
	tyg_ret(p, tyg_bin(p, TYG_ADD, lo, hi));
	check(tyg_proc_end(p));
}

// _Thread_local int tg_tls_a = 41; _Thread_local int tg_tls_b;
// int tg_tls_bump(void) { tg_tls_b += 1; return ++tg_tls_a + tg_tls_b; }
static void build_tls(void) {
	static const int init = 41;
	tyg_sym a = tyg_symbol(m, "tg_tls_a", TYG_SYM_GLOBAL | TYG_SYM_TLS);
	check(tyg_data(m, a, TYG_SECTION_TDATA, &init, 4, 4));
	tyg_sym b = tyg_symbol(m, "tg_tls_b", TYG_SYM_TLS);
	check(tyg_data(m, b, TYG_SECTION_TBSS, NULL, 4, 4));
	tyg_sig *sig = tyg_sig_new(m, i32t(), NULL, 0, 0);
	tyg_proc *p = tyg_proc_begin(m, tyg_symbol(m, "tg_tls_bump", TYG_SYM_FUNC | TYG_SYM_GLOBAL), sig);
	tyg_val bv = tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I32, tyg_at_sym(b, 0), 0), tyg_iconst(p, TYG_I32, 1));
	tyg_store(p, TYG_I32, tyg_at_sym(b, 0), bv, 0);
	tyg_val av = tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I32, tyg_at_sym(a, 0), 0), tyg_iconst(p, TYG_I32, 1));
	tyg_store(p, TYG_I32, tyg_at_sym(a, 0), av, 0);
	tyg_ret(p, tyg_bin(p, TYG_ADD, av, bv));
	check(tyg_proc_end(p));
}

// extern long long tg_ext_value; -- defined by smoke_main.c, reached as foreign data (__imp_ on Windows)
// long long tg_read_ext(void) { return tg_ext_value; }
// int tg_weak_answer(void) { return 7; } -- a weak definition
static void build_ext_weak(void) {
	tyg_sym ext = tyg_symbol(m, "tg_ext_value", TYG_SYM_GLOBAL | TYG_SYM_FOREIGN);
	tyg_sig *sig = tyg_sig_new(m, i64t(), NULL, 0, 0);
	tyg_proc *p = tyg_proc_begin(m, tyg_symbol(m, "tg_read_ext", TYG_SYM_FUNC | TYG_SYM_GLOBAL), sig);
	tyg_ret(p, tyg_load(p, TYG_I64, tyg_at_sym(ext, 0), 0));
	check(tyg_proc_end(p));

	tyg_sig *wsig = tyg_sig_new(m, i32t(), NULL, 0, 0);
	p = tyg_proc_begin(m, tyg_symbol(m, "tg_weak_answer", TYG_SYM_FUNC | TYG_SYM_GLOBAL | TYG_SYM_WEAK), wsig);
	tyg_ret(p, tyg_iconst(p, TYG_I32, 7));
	check(tyg_proc_end(p));
}

// int tg_prot_var = 41;                    -- protected visibility on ELF,
// int tg_prot_get(void) { return tg_prot_var + 1; }  default on Mach-O
static void build_protected(void) {
	static const int v = 41;
	tyg_sym var = tyg_symbol(m, "tg_prot_var", TYG_SYM_GLOBAL | TYG_SYM_PROTECTED);
	check(tyg_data(m, var, TYG_SECTION_DATA, &v, 4, 4));
	tyg_sig *sig = tyg_sig_new(m, i32t(), NULL, 0, 0);
	tyg_proc *p = tyg_proc_begin(m, tyg_symbol(m, "tg_prot_get", TYG_SYM_FUNC | TYG_SYM_GLOBAL | TYG_SYM_PROTECTED), sig);
	tyg_ret(p, tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I32, tyg_at_sym(var, 0), 0), tyg_iconst(p, TYG_I32, 1)));
	check(tyg_proc_end(p));
}

// Code in the section tg_code, on ELF; elsewhere tyg_proc_section fails and it stays in .text.
//
//   static int tg_sec_twice(int x) { return tg_add(x, x); }   // line 60, calls into .text
//   int tg_sec_fn(int x) {                                    // 62
//       int y = 0;                                            // 63
//       if (x > 0) {                                          // 64
//           int z = tg_sec_twice(x);                          // 65
//           y = z + 1;                                        // 66
//       } else y = -1;                                        // 67
//       return y;                                             // 68
//   }
static void build_in_section(void) {
	const tyg_dtype *int_dt = tyg_dt_base(m, "int", 4, TYG_DT_SIGNED);
	const tyg_dtype *pt[1] = {int_dt};
	const tyg_dtype *fn_dt = tyg_dt_func(m, int_dt, pt, 1, 0);
	const tyg_type *params[1] = {i32t()};
	tyg_sig *sig = tyg_sig_new(m, i32t(), params, 1, 0);

	tyg_sym twice = tyg_symbol(m, "tg_sec_twice", TYG_SYM_FUNC);
	tyg_proc *p = tyg_proc_begin(m, twice, sig);
	int failed = tyg_proc_section(p, "tg_code");
	if (failed != !is_elf) {
		fprintf(stderr, "tyg_proc_section: %s\n", failed ? tyg_error(m) : "no error off ELF");
		exit(1);
	}
	tyg_debug_proc(p, "tg_sec_twice", file, 60, fn_dt);
	tyg_loc(p, file, 60, 1);
	tyg_arg args[2];
	args[0] = val_arg(tyg_param(p, 0));
	args[1] = val_arg(tyg_param(p, 0));
	const tyg_type *aparams[2] = {i32t(), i32t()};
	tyg_sig *add_sig = tyg_sig_new(m, i32t(), aparams, 2, 0);
	tyg_ret(p, tyg_call(p, add_sig, tyg_direct(tyg_symbol(m, "tg_add", 0)), args, 2, NULL, NULL));
	check(tyg_proc_end(p));

	p = tyg_proc_begin(m, tyg_symbol(m, "tg_sec_fn", TYG_SYM_FUNC | TYG_SYM_GLOBAL), sig);
	if (is_elf) check(tyg_proc_section(p, "tg_code"));
	tyg_debug_proc(p, "tg_sec_fn", file, 62, fn_dt);
	tyg_loc(p, file, 62, 1);
	tyg_local xl = tyg_local_new(p, 4, 4);
	tyg_store(p, TYG_I32, tyg_at_local(xl, 0), tyg_param(p, 0), 0);
	tyg_debug_param(p, "x", int_dt, tyg_at_local(xl, 0), file, 62);
	tyg_loc(p, file, 63, 5);
	tyg_local y = tyg_local_new(p, 4, 4);
	tyg_debug_var(p, "y", int_dt, tyg_at_local(y, 0), file, 63);
	tyg_store(p, TYG_I32, tyg_at_local(y, 0), tyg_iconst(p, TYG_I32, 0), 0);
	tyg_loc(p, file, 64, 5);
	tyg_block then1 = tyg_block_new(p), other = tyg_block_new(p), then2 = tyg_block_new(p), done = tyg_block_new(p);
	tyg_val x = tyg_load(p, TYG_I32, tyg_at_local(xl, 0), 0);
	tyg_branch(p, tyg_cmp(p, TYG_SGT, x, tyg_iconst(p, TYG_I32, 0)), then1, other);
	tyg_scope_begin(p);
	tyg_block_set(p, then1);
	tyg_loc(p, file, 65, 9);
	tyg_local z = tyg_local_new(p, 4, 4);
	tyg_debug_var(p, "z", int_dt, tyg_at_local(z, 0), file, 65);
	tyg_arg a = val_arg(tyg_load(p, TYG_I32, tyg_at_local(xl, 0), 0));
	tyg_store(p, TYG_I32, tyg_at_local(z, 0), tyg_call(p, sig, tyg_direct(twice), &a, 1, NULL, NULL), 0);
	tyg_jump(p, then2);
	tyg_block_set(p, then2);
	tyg_loc(p, file, 66, 9);
	tyg_store(p, TYG_I32, tyg_at_local(y, 0), tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I32, tyg_at_local(z, 0), 0), tyg_iconst(p, TYG_I32, 1)), 0);
	tyg_jump(p, done);
	tyg_scope_end(p);
	tyg_block_set(p, other);
	tyg_loc(p, file, 67, 12);
	tyg_store(p, TYG_I32, tyg_at_local(y, 0), tyg_iconst(p, TYG_I32, -1), 0);
	tyg_jump(p, done);
	tyg_block_set(p, done);
	tyg_loc(p, file, 68, 5);
	tyg_ret(p, tyg_load(p, TYG_I32, tyg_at_local(y, 0), 0));
	check(tyg_proc_end(p));
}

// On macOS, whose linker takes no 4 byte address in data, tyg_data_reloc fails for one
static void check_abs32(void) {
	static const int zero = 0;
	tyg_sym s = tyg_symbol(m, "tg_addr32", 0);
	check(tyg_data(m, s, TYG_SECTION_DATA, &zero, 4, 4));
	if (tyg_data_reloc(m, s, 0, 4, s, 0) == 0) {
		fprintf(stderr, "a 4 byte data relocation did not fail\n");
		exit(1);
	}
}

// Debug info for a debugger to show:
//
//   typedef struct Point { int x, y; } Point;
//   enum Color { RED, GREEN = 5, BLUE };
//   struct Flags { unsigned a : 3; unsigned b : 5; int c; };
//   struct Node {
//       struct Node *next; const char *name; double vals[3]; enum Color color;
//       struct Flags flags; union { int i; float f; };
//   };
//   Point tg_origin = {10, 20};
//
//   long long tg_node_sum(struct Node *n, Point p) {   // line 50
//       long long t = p.x + p.y;                       // 51
//       {                                              // 52
//           int inner = n->flags.c;                    // 53
//           t += inner + n->color;                     // 54
//       }
//       return t;                                      // 56
//   }
static void build_node_sum(void) {
	const tyg_dtype *int_dt = tyg_dt_base(m, "int", 4, TYG_DT_SIGNED);
	const tyg_dtype *uint_dt = tyg_dt_base(m, "unsigned int", 4, TYG_DT_UNSIGNED);
	const tyg_dtype *ll_dt = tyg_dt_base(m, "long long", 8, TYG_DT_SIGNED);
	const tyg_dtype *char_dt = tyg_dt_base(m, "char", 1, TYG_DT_SIGNED_CHAR);
	const tyg_dtype *dbl_dt = tyg_dt_base(m, "double", 8, TYG_DT_FLOAT);
	const tyg_dtype *flt_dt = tyg_dt_base(m, "float", 4, TYG_DT_FLOAT);

	tyg_dtype *point = tyg_dt_record(m, TYG_DT_STRUCT, "Point", file, 44);
	tyg_dt_member(m, point, "x", int_dt, 0);
	tyg_dt_member(m, point, "y", int_dt, 4);
	tyg_dt_record_complete(m, point, 8, 4);
	const tyg_dtype *point_td = tyg_dt_typedef(m, "Point", point, file, 44);

	tyg_dtype *color = tyg_dt_enum(m, "Color", uint_dt, file, 45);
	tyg_dt_enumerator(m, color, "RED", 0);
	tyg_dt_enumerator(m, color, "GREEN", 5);
	tyg_dt_enumerator(m, color, "BLUE", 6);

	tyg_dtype *flags = tyg_dt_record(m, TYG_DT_STRUCT, "Flags", file, 46);
	tyg_dt_bitfield(m, flags, "a", uint_dt, 0, 3);
	tyg_dt_bitfield(m, flags, "b", uint_dt, 3, 5);
	tyg_dt_member(m, flags, "c", int_dt, 4);
	tyg_dt_record_complete(m, flags, 8, 4);

	tyg_dtype *u = tyg_dt_record(m, TYG_DT_UNION, NULL, file, 47);
	tyg_dt_member(m, u, "i", int_dt, 0);
	tyg_dt_member(m, u, "f", flt_dt, 0);
	tyg_dt_record_complete(m, u, 4, 4);

	tyg_dtype *node = tyg_dt_record(m, TYG_DT_STRUCT, "Node", file, 47);
	const tyg_dtype *node_ptr = tyg_dt_pointer(m, node);
	tyg_dt_member(m, node, "next", node_ptr, 0);
	tyg_dt_member(m, node, "name", tyg_dt_pointer(m, tyg_dt_const(m, char_dt)), 8);
	tyg_dt_member(m, node, "vals", tyg_dt_array(m, dbl_dt, 3), 16);
	tyg_dt_member(m, node, "color", color, 40);
	tyg_dt_member(m, node, "flags", flags, 44);
	tyg_dt_member(m, node, NULL, u, 52);
	tyg_dt_record_complete(m, node, 56, 8);

	// the global
	static const int origin[2] = {10, 20};
	tyg_sym og = tyg_symbol(m, "tg_origin", TYG_SYM_GLOBAL | (windows ? TYG_SYM_EXPORT : 0)); // exported data on Windows
	check(tyg_data(m, og, TYG_SECTION_DATA, origin, 8, 4));
	tyg_debug_global(m, "tg_origin", point_td, og, file, 48);

	tyg_field pf[2] = {{i32t(), 0}, {i32t(), 4}};
	const tyg_type *pt = tyg_type_record(m, 8, 4, pf, 2);
	const tyg_type *params[2] = {tyg_type_ptr(m), pt};
	tyg_sig *sig = tyg_sig_new(m, i64t(), params, 2, 0);
	tyg_proc *p = tyg_proc_begin(m, tyg_symbol(m, "tg_node_sum", TYG_SYM_FUNC | TYG_SYM_GLOBAL), sig);
	const tyg_dtype *fparams[2] = {node_ptr, point_td};
	tyg_debug_proc(p, "tg_node_sum", file, 50, tyg_dt_func(m, ll_dt, fparams, 2, 0));
	tyg_local nl = tyg_local_new(p, 8, 8);
	tyg_store(p, TYG_I64, tyg_at_local(nl, 0), tyg_param(p, 0), 0);
	tyg_debug_param(p, "n", node_ptr, tyg_at_local(nl, 0), file, 50);
	tyg_val pp = tyg_param(p, 1);
	tyg_debug_param(p, "p", point_td, tyg_at_val(pp, 0), file, 50);

	tyg_loc(p, file, 51, 5);
	tyg_local t = tyg_local_new(p, 8, 8);
	tyg_debug_var(p, "t", ll_dt, tyg_at_local(t, 0), file, 51);
	tyg_val xy = tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I32, tyg_at_val(pp, 0), 0), tyg_load(p, TYG_I32, tyg_at_val(pp, 4), 0));
	tyg_store(p, TYG_I64, tyg_at_local(t, 0), tyg_convert(p, TYG_SEXT, TYG_I64, xy), 0);

	tyg_scope_begin(p);
	tyg_loc(p, file, 53, 9);
	tyg_local inner = tyg_local_new(p, 4, 4);
	tyg_debug_var(p, "inner", int_dt, tyg_at_local(inner, 0), file, 53);
	tyg_val np = tyg_load(p, TYG_I64, tyg_at_local(nl, 0), 0);
	tyg_store(p, TYG_I32, tyg_at_local(inner, 0), tyg_load(p, TYG_I32, tyg_at_val(np, 48), 0), 0);
	tyg_loc(p, file, 54, 9);
	tyg_val add = tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I32, tyg_at_local(inner, 0), 0), tyg_load(p, TYG_I32, tyg_at_val(np, 40), 0));
	tyg_store(p, TYG_I64, tyg_at_local(t, 0), tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I64, tyg_at_local(t, 0), 0), tyg_convert(p, TYG_SEXT, TYG_I64, add)), 0);
	tyg_scope_end(p);

	tyg_loc(p, file, 56, 5);
	tyg_ret(p, tyg_load(p, TYG_I64, tyg_at_local(t, 0), 0));
	check(tyg_proc_end(p));
}

// struct Q { long long a, b; };  long long tg_q_diff(struct Q q) is C, in smoke_main.c
// long long tg_copy_fwd(struct Q *q, struct Q s) {
//   struct Q t = *q; long long r = t.a; q->a = 100; r += t.a * 10;   // t keeps the old q->a
//   struct Q u = s, w = u; u.b = 5; r += w.b * 1000 + u.b * 100000; // w keeps the old u.b
//   struct Q t3 = *q; q->b = 9; return r + tg_q_diff(t3);           // t3 keeps the old q->b
// }
// Reads of the copies read their sources until a write in between, and s lands in its copy.
static void build_copy_fwd(void) {
	tyg_field qf[2] = {{i64t(), 0}, {i64t(), 8}};
	const tyg_type *qt = tyg_type_record(m, 16, 8, qf, 2);
	const tyg_type *dparams[1] = {qt};
	tyg_sig *dsig = tyg_sig_new(m, i64t(), dparams, 1, 0);
	const tyg_type *params[2] = {tyg_type_ptr(m), qt};
	tyg_sig *sig = tyg_sig_new(m, i64t(), params, 2, 0);
	tyg_proc *p = tyg_proc_begin(m, tyg_symbol(m, "tg_copy_fwd", TYG_SYM_FUNC | TYG_SYM_GLOBAL), sig);
	tyg_val q = tyg_param(p, 0);
	tyg_local sl = tyg_local_new(p, 16, 8);
	tyg_copy(p, tyg_at_local(sl, 0), tyg_at_val(tyg_param(p, 1), 0), 16);
	tyg_loc(p, file, 60, 1);
	tyg_local t = tyg_local_new(p, 16, 8);
	tyg_copy(p, tyg_at_local(t, 0), tyg_at_val(q, 0), 16);
	tyg_val r = tyg_load(p, TYG_I64, tyg_at_local(t, 0), 0);
	tyg_store(p, TYG_I64, tyg_at_val(q, 0), tyg_iconst(p, TYG_I64, 100), 0);
	r = tyg_bin(p, TYG_ADD, r, tyg_bin(p, TYG_MUL, tyg_load(p, TYG_I64, tyg_at_local(t, 0), 0), tyg_iconst(p, TYG_I64, 10)));
	tyg_loc(p, file, 61, 1);
	tyg_local u = tyg_local_new(p, 16, 8), w = tyg_local_new(p, 16, 8);
	tyg_copy(p, tyg_at_local(u, 0), tyg_at_local(sl, 0), 16);
	tyg_copy(p, tyg_at_local(w, 0), tyg_at_local(u, 0), 16);
	tyg_store(p, TYG_I64, tyg_at_local(u, 8), tyg_iconst(p, TYG_I64, 5), 0);
	tyg_val wb = tyg_bin(p, TYG_MUL, tyg_load(p, TYG_I64, tyg_at_local(w, 8), 0), tyg_iconst(p, TYG_I64, 1000));
	tyg_val ub = tyg_bin(p, TYG_MUL, tyg_load(p, TYG_I64, tyg_at_local(u, 8), 0), tyg_iconst(p, TYG_I64, 100000));
	r = tyg_bin(p, TYG_ADD, r, tyg_bin(p, TYG_ADD, wb, ub));
	tyg_loc(p, file, 62, 1);
	tyg_local t3 = tyg_local_new(p, 16, 8);
	tyg_copy(p, tyg_at_local(t3, 0), tyg_at_val(q, 0), 16);
	tyg_store(p, TYG_I64, tyg_at_val(q, 8), tyg_iconst(p, TYG_I64, 9), 0);
	tyg_arg args[1];
	args[0].val = 0;
	args[0].mem = tyg_at_local(t3, 0);
	tyg_val d = tyg_call(p, dsig, tyg_direct(tyg_symbol(m, "tg_q_diff", TYG_SYM_FUNC | TYG_SYM_GLOBAL)), args, 1, NULL, NULL);
	tyg_ret(p, tyg_bin(p, TYG_ADD, r, d));
	check(tyg_proc_end(p));
}

// long long tg_pair(const signed char *b, const short *h, const int *w)
//   { return b[1] - 2*b[0] + 7*h[0] - h[1] + 3*w[1] - w[0]; }
// Adjacent loads, in either order, that arm64 makes one wider load of both.
static void build_pair(void) {
	const tyg_type *params[3] = {tyg_type_ptr(m), tyg_type_ptr(m), tyg_type_ptr(m)};
	tyg_sig *sig = tyg_sig_new(m, i64t(), params, 3, 0);
	tyg_proc *p = tyg_proc_begin(m, tyg_symbol(m, "tg_pair", TYG_SYM_FUNC | TYG_SYM_GLOBAL), sig);
	tyg_val b = tyg_param(p, 0), h = tyg_param(p, 1), w = tyg_param(p, 2);
	tyg_loc(p, file, 70, 1);
	tyg_val b1 = tyg_convert(p, TYG_SEXT, TYG_I64, tyg_load(p, TYG_I8, tyg_at_val(b, 1), 0));
	tyg_val b0 = tyg_convert(p, TYG_SEXT, TYG_I64, tyg_load(p, TYG_I8, tyg_at_val(b, 0), 0));
	tyg_val h0 = tyg_convert(p, TYG_SEXT, TYG_I64, tyg_load(p, TYG_I16, tyg_at_val(h, 0), 0));
	tyg_val h1 = tyg_convert(p, TYG_SEXT, TYG_I64, tyg_load(p, TYG_I16, tyg_at_val(h, 2), 0));
	tyg_val w1 = tyg_convert(p, TYG_SEXT, TYG_I64, tyg_load(p, TYG_I32, tyg_at_val(w, 4), 0));
	tyg_val w0 = tyg_convert(p, TYG_SEXT, TYG_I64, tyg_load(p, TYG_I32, tyg_at_val(w, 0), 0));
	tyg_val r = tyg_bin(p, TYG_SUB, b1, tyg_bin(p, TYG_MUL, b0, tyg_iconst(p, TYG_I64, 2)));
	r = tyg_bin(p, TYG_ADD, r, tyg_bin(p, TYG_MUL, h0, tyg_iconst(p, TYG_I64, 7)));
	r = tyg_bin(p, TYG_SUB, r, h1);
	r = tyg_bin(p, TYG_ADD, r, tyg_bin(p, TYG_MUL, w1, tyg_iconst(p, TYG_I64, 3)));
	r = tyg_bin(p, TYG_SUB, r, w0);
	tyg_ret(p, r);
	check(tyg_proc_end(p));
}

// long long tg_store_fwd(long long *a, long long *b) {
//   a[0] = 0x1111111111111111; a[1] = 6; ((int *)a)[1] = 0x22222222; b[0] = 7; return a[1] + a[0];
// }
// Loads after stores through pointers: a later store through another pointer or over part of
// the value makes them load again.
static void build_store_fwd(void) {
	const tyg_type *params[2] = {tyg_type_ptr(m), tyg_type_ptr(m)};
	tyg_sig *sig = tyg_sig_new(m, i64t(), params, 2, 0);
	tyg_proc *p = tyg_proc_begin(m, tyg_symbol(m, "tg_store_fwd", TYG_SYM_FUNC | TYG_SYM_GLOBAL), sig);
	tyg_val a = tyg_param(p, 0), b = tyg_param(p, 1);
	tyg_loc(p, file, 80, 1);
	tyg_store(p, TYG_I64, tyg_at_val(a, 0), tyg_iconst(p, TYG_I64, 0x1111111111111111LL), 0);
	tyg_store(p, TYG_I64, tyg_at_val(a, 8), tyg_iconst(p, TYG_I64, 6), 0);
	tyg_val first = tyg_load(p, TYG_I64, tyg_at_val(a, 8), 0);
	tyg_store(p, TYG_I32, tyg_at_val(a, 4), tyg_iconst(p, TYG_I32, 0x22222222), 0);
	tyg_store(p, TYG_I64, tyg_at_val(b, 0), tyg_iconst(p, TYG_I64, 7), 0);
	tyg_val r = tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I64, tyg_at_val(a, 8), 0), tyg_load(p, TYG_I64, tyg_at_val(a, 0), 0));
	tyg_ret(p, tyg_bin(p, TYG_ADD, r, tyg_bin(p, TYG_MUL, first, tyg_iconst(p, TYG_I64, 1000))));
	check(tyg_proc_end(p));
}

int main(int argc, char **argv) {
	char const *out = argc > 1 ? argv[1] : "smoke.o";
	char const *target = argc > 2 ? argv[2] : "linux-x64";
	tyg_target t = {TYG_ARCH_X64, TYG_OS_LINUX, 1, 1};
	if (strcmp(target, "linux-x64") == 0 || strcmp(target, "linux") == 0) {
	} else if (strcmp(target, "linux-arm64") == 0) {
		t.arch = TYG_ARCH_ARM64;
	} else if (strcmp(target, "macos-arm64") == 0) {
		t.arch = TYG_ARCH_ARM64;
		t.os = TYG_OS_DARWIN;
	} else if (strcmp(target, "macos-x64") == 0) {
		t.os = TYG_OS_DARWIN;
	} else if (strcmp(target, "windows") == 0) {
		t.os = TYG_OS_WINDOWS;
		windows = 1;
	} else if (strcmp(target, "windows-dwarf") == 0) {
		// DWARF in the COFF object, for gdb
		t.os = TYG_OS_WINDOWS;
		t.debug_info = TYG_DEBUG_DWARF;
		windows = 1;
	} else {
		fprintf(stderr, "usage: smoke out.o [linux-x64|linux-arm64|macos-arm64|macos-x64|windows|windows-dwarf]\n");
		return 2;
	}
	is_elf = t.os == TYG_OS_LINUX;
	m = tyg_module_new(t, "smoke.tg");
	if (!m) { fprintf(stderr, "no module\n"); return 1; }
	file = tyg_debug_file(m, "smoke.tg");
	tyg_module_language(m, TYG_LANG_C11);
	build_abort();
	build_add();
	build_fib();
	build_sum();
	build_vscale();
	build_big();
	build_greet();
	build_vsum();
	build_vmix();
	build_call_vmix();
	build_mix();
	build_small_structs();
	build_fkeep();
	build_call_cb();
	build_bigframe();
	build_bigalloca();
	build_tls();
	build_ext_weak();
	if (!windows) build_protected();
	if (!windows) build_in_section();
	build_node_sum();
	build_copy_fwd();
	build_pair();
	build_store_fwd();
	if (!windows && !is_elf) check_abs32();
	check(tyg_module_write(m, out));
	tyg_module_free(m);
	return 0;
}
