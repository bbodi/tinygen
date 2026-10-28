// Builds functions that exercise the calling convention and the lowering against C code
// from the system compiler: float aggregates, small, packed and 16 byte aligned structs, results in
// memory, arguments past the registers, char and short, C varargs both ways, thread locals,
// atomics at every width, alloca, large frames, a branch over more code than a conditional
// branch reaches, and long double (x87 on x86-64 Linux and macOS, a double on arm64 macOS and
// Windows, an error on arm64 Linux).
// abi_main.c holds the C half and checks the results.
//
//   cc -std=c99 -Iinclude test/abi.c src/tinygen.c -o abi
//   ./abi abi.o [linux-x64|linux-arm64|macos-arm64|macos-x64|windows] [nopic]
//   cc test/abi_main.c abi.o -o abi_run -lpthread -lm && ./abi_run
//
// For windows, see test/WINDOWS.md.
#include "tinygen.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static tyg_module *m;
static tyg_file file;

static int c_failed = -1; // the "c" target goes on after a failure and counts it

static void check(int failed) {
	if (failed) {
		fprintf(stderr, "tinygen: %s\n", tyg_error(m));
		if (c_failed < 0) exit(1);
		c_failed++;
	}
}

static const tyg_type *T(tyg_scalar s) { return tyg_type_scalar(m, s); }
static const tyg_type *TI(tyg_scalar s, int sgn) { return tyg_type_int(m, s, sgn); }

static const tyg_type *f3t, *d4t, *cst, *lit, *a16t, *l3t;

static void make_types(void) {
	tyg_field f3[3] = {{0, 0}, {0, 4}, {0, 8}};
	f3[0].type = f3[1].type = f3[2].type = T(TYG_F32);
	f3t = tyg_type_record(m, 12, 4, f3, 3);
	tyg_field d4[1] = {{0, 0}};
	d4[0].type = tyg_type_array(m, T(TYG_F64), 4);
	d4t = tyg_type_record(m, 32, 8, d4, 1);
	tyg_field cs[2] = {{0, 0}, {0, 2}};
	cs[0].type = TI(TYG_I8, 1);
	cs[1].type = TI(TYG_I16, 1);
	cst = tyg_type_record(m, 4, 2, cs, 2);
	tyg_field li[2] = {{0, 0}, {0, 8}};
	li[0].type = T(TYG_I64);
	li[1].type = T(TYG_I32);
	lit = tyg_type_record(m, 16, 8, li, 2);
	tyg_field a16[2] = {{0, 0}, {0, 8}};
	a16[0].type = a16[1].type = T(TYG_I64);
	a16t = tyg_type_record(m, 16, 16, a16, 2);
	tyg_field l3[3] = {{0, 0}, {0, 8}, {0, 16}};
	l3[0].type = l3[1].type = l3[2].type = T(TYG_I64);
	l3t = tyg_type_record(m, 24, 8, l3, 3);
}

static tyg_proc *begin(const char *name, const tyg_type *ret, const tyg_type *const *params, int n, unsigned flags, tyg_sig **sig_out) {
	tyg_sig *sig = tyg_sig_new(m, ret, params, n, flags);
	tyg_proc *p = tyg_proc_begin(m, tyg_symbol(m, name, TYG_SYM_FUNC | TYG_SYM_GLOBAL), sig);
	if (!p) check(1);
	if (sig_out) *sig_out = sig;
	tyg_loc(p, file, 1, 1);
	return p;
}

static tyg_val ld(tyg_proc *p, tyg_scalar t, tyg_addr a) { return tyg_load(p, t, a, 0); }
static void st(tyg_proc *p, tyg_scalar t, tyg_addr a, tyg_val v) { tyg_store(p, t, a, v, 0); }
static tyg_val k64(tyg_proc *p, int64_t v) { return tyg_iconst(p, TYG_I64, v); }
static tyg_val kd(tyg_proc *p, double v) { return tyg_fconst(p, TYG_F64, v); }
static tyg_val add(tyg_proc *p, tyg_val a, tyg_val b) { return tyg_bin(p, tyg_val_type(p, a) >= TYG_F32 ? TYG_FADD : TYG_ADD, a, b); }
static tyg_val mul(tyg_proc *p, tyg_val a, tyg_val b) { return tyg_bin(p, tyg_val_type(p, a) >= TYG_F32 ? TYG_FMUL : TYG_MUL, a, b); }
// a signed integer or a float to double
static tyg_val tod(tyg_proc *p, tyg_val v) {
	tyg_scalar t = tyg_val_type(p, v);
	if (t == TYG_F64) return v;
	if (t == TYG_F32) return tyg_convert(p, TYG_FEXT, TYG_F64, v);
	return tyg_convert(p, TYG_SITOF, TYG_F64, v);
}
static tyg_val sx64(tyg_proc *p, tyg_val v) { return tyg_val_type(p, v) == TYG_I64 ? v : tyg_convert(p, TYG_SEXT, TYG_I64, v); }
static tyg_arg val(tyg_val v) { tyg_arg a; memset(&a, 0, sizeof(a)); a.val = v; return a; }
static tyg_arg mem(tyg_addr at) { tyg_arg a; memset(&a, 0, sizeof(a)); a.mem = at; return a; }

// struct F3 tg_f3_scale(struct F3 v, float k)
static void build_f3_scale(void) {
	const tyg_type *ps[2] = {f3t, T(TYG_F32)};
	tyg_proc *p = begin("tg_f3_scale", f3t, ps, 2, 0, NULL);
	tyg_val v = tyg_param(p, 0), k = tyg_param(p, 1);
	tyg_local out = tyg_local_new(p, 12, 4);
	for (int i = 0; i < 3; i++) st(p, TYG_F32, tyg_at_local(out, 4*i), mul(p, ld(p, TYG_F32, tyg_at_val(v, 4*i)), k));
	tyg_ret_mem(p, tyg_at_local(out, 0));
	check(tyg_proc_end(p));
}

// struct D4 tg_d4_rev(struct D4 v)
static void build_d4_rev(void) {
	const tyg_type *ps[1] = {d4t};
	tyg_proc *p = begin("tg_d4_rev", d4t, ps, 1, 0, NULL);
	tyg_val v = tyg_param(p, 0);
	tyg_local out = tyg_local_new(p, 32, 8);
	for (int i = 0; i < 4; i++) st(p, TYG_F64, tyg_at_local(out, 8*i), ld(p, TYG_F64, tyg_at_val(v, 8*(3-i))));
	tyg_ret_mem(p, tyg_at_local(out, 0));
	check(tyg_proc_end(p));
}

// int tg_cs_sum(struct CS v) { return v.c + v.s; }
static void build_cs_sum(void) {
	const tyg_type *ps[1] = {cst};
	tyg_proc *p = begin("tg_cs_sum", T(TYG_I32), ps, 1, 0, NULL);
	tyg_val v = tyg_param(p, 0);
	tyg_val c = tyg_convert(p, TYG_SEXT, TYG_I32, ld(p, TYG_I8, tyg_at_val(v, 0)));
	tyg_val s = tyg_convert(p, TYG_SEXT, TYG_I32, ld(p, TYG_I16, tyg_at_val(v, 2)));
	tyg_ret(p, add(p, c, s));
	check(tyg_proc_end(p));
}

// struct LI tg_li_make(long long a, int b)
static void build_li_make(void) {
	const tyg_type *ps[2] = {T(TYG_I64), T(TYG_I32)};
	tyg_proc *p = begin("tg_li_make", lit, ps, 2, 0, NULL);
	tyg_local out = tyg_local_new(p, 16, 8);
	st(p, TYG_I64, tyg_at_local(out, 0), tyg_param(p, 0));
	st(p, TYG_I32, tyg_at_local(out, 8), tyg_param(p, 1));
	tyg_ret_mem(p, tyg_at_local(out, 0));
	check(tyg_proc_end(p));
}

// long long tg_a16_sum(int x, struct A16 v) { return x + v.a + v.b; }
static void build_a16_sum(void) {
	const tyg_type *ps[2] = {T(TYG_I32), a16t};
	tyg_proc *p = begin("tg_a16_sum", T(TYG_I64), ps, 2, 0, NULL);
	tyg_val v = tyg_param(p, 1);
	tyg_val r = add(p, sx64(p, tyg_param(p, 0)), add(p, ld(p, TYG_I64, tyg_at_val(v, 0)), ld(p, TYG_I64, tyg_at_val(v, 8))));
	tyg_ret(p, r);
	check(tyg_proc_end(p));
}

// The parameters of tg_many and c_many: 9 ints, 9 doubles, an F3, a signed char and an
// unsigned short, a 16 byte aligned struct and an LI, all past the registers.
static int many_params(const tyg_type **ps) {
	int n = 0;
	for (int i = 0; i < 9; i++) ps[n++] = T(TYG_I32);
	for (int i = 0; i < 9; i++) ps[n++] = T(TYG_F64);
	ps[n++] = f3t;
	ps[n++] = TI(TYG_I8, 1);
	ps[n++] = TI(TYG_I16, 0);
	ps[n++] = a16t;
	ps[n++] = lit;
	return n;
}

// double tg_many(...) { return sum of i_k*(k+1) + d_k*(k+1) + h.x + 2*h.y + 3*h.z + c + s + a16.a - a16.b + li.a + li.b; }
static void build_many(void) {
	const tyg_type *ps[32];
	int n = many_params(ps);
	tyg_proc *p = begin("tg_many", T(TYG_F64), ps, n, 0, NULL);
	tyg_val s = kd(p, 0);
	for (int i = 0; i < 18; i++) s = add(p, s, mul(p, tod(p, tyg_param(p, i)), kd(p, (double)(i % 9 + 1))));
	tyg_val h = tyg_param(p, 18);
	for (int i = 0; i < 3; i++) s = add(p, s, mul(p, tod(p, ld(p, TYG_F32, tyg_at_val(h, 4*i))), kd(p, i + 1)));
	s = add(p, s, tod(p, tyg_param(p, 19)));
	s = add(p, s, tod(p, tyg_convert(p, TYG_ZEXT, TYG_I32, tyg_param(p, 20))));
	tyg_val a16 = tyg_param(p, 21), li = tyg_param(p, 22);
	s = add(p, s, tod(p, ld(p, TYG_I64, tyg_at_val(a16, 0))));
	s = tyg_bin(p, TYG_FSUB, s, tod(p, ld(p, TYG_I64, tyg_at_val(a16, 8))));
	s = add(p, s, tod(p, ld(p, TYG_I64, tyg_at_val(li, 0))));
	s = add(p, s, tod(p, ld(p, TYG_I32, tyg_at_val(li, 8))));
	tyg_ret(p, s);
	check(tyg_proc_end(p));
}

// int tg_small(signed char a, unsigned short b, short c) { return a + b + c; }
// signed char tg_small_ret(int x) { return (signed char)x; }
static void build_small(void) {
	{
		const tyg_type *ps[3] = {TI(TYG_I8, 1), TI(TYG_I16, 0), TI(TYG_I16, 1)};
		tyg_proc *p = begin("tg_small", T(TYG_I32), ps, 3, 0, NULL);
		tyg_val a = tyg_convert(p, TYG_SEXT, TYG_I32, tyg_param(p, 0));
		tyg_val b = tyg_convert(p, TYG_ZEXT, TYG_I32, tyg_param(p, 1));
		tyg_val c = tyg_convert(p, TYG_SEXT, TYG_I32, tyg_param(p, 2));
		tyg_ret(p, add(p, add(p, a, b), c));
		check(tyg_proc_end(p));
	}
	{
		const tyg_type *ps[1] = {T(TYG_I32)};
		tyg_proc *p = begin("tg_small_ret", TI(TYG_I8, 1), ps, 1, 0, NULL);
		tyg_ret(p, tyg_convert(p, TYG_TRUNC, TYG_I8, tyg_param(p, 0)));
		check(tyg_proc_end(p));
	}
}

// struct B3 { unsigned char x[3]; }, which goes on the stack in an 8 byte slot as clang's i64:
// int tg_stk_b3(long long a0, ..., long long a7, struct B3 s, int after)
//     { return s.x[0] + s.x[1] + s.x[2] + after; }
// int tg_call_stk_b3(void) { struct B3 s = {1, 2, 3}; return c_stk_b3(0, ..., 7, s, 36); }
static void build_stk_b3(void) {
	tyg_field f[1] = {{0, 0}};
	f[0].type = tyg_type_array(m, TI(TYG_I8, 0), 3);
	const tyg_type *b3 = tyg_type_record(m, 3, 1, f, 1);
	const tyg_type *ps[10];
	for (int i = 0; i < 8; i++) ps[i] = T(TYG_I64);
	ps[8] = b3;
	ps[9] = T(TYG_I32);
	{
		tyg_proc *p = begin("tg_stk_b3", T(TYG_I32), ps, 10, 0, NULL);
		tyg_val sv = tyg_param(p, 8);
		tyg_val r = tyg_param(p, 9);
		for (int i = 0; i < 3; i++) r = add(p, r, tyg_convert(p, TYG_ZEXT, TYG_I32, ld(p, TYG_I8, tyg_at_val(sv, i))));
		tyg_ret(p, r);
		check(tyg_proc_end(p));
	}
	{
		tyg_proc *p = begin("tg_call_stk_b3", T(TYG_I32), NULL, 0, 0, NULL);
		tyg_sig *sig = tyg_sig_new(m, T(TYG_I32), ps, 10, 0);
		tyg_local sl = tyg_local_new(p, 3, 1);
		tyg_arg args[10];
		for (int i = 0; i < 8; i++) args[i] = val(k64(p, i));
		for (int i = 0; i < 3; i++) st(p, TYG_I8, tyg_at_local(sl, i), tyg_iconst(p, TYG_I8, i + 1));
		args[8] = mem(tyg_at_local(sl, 0));
		args[9] = val(tyg_iconst(p, TYG_I32, 36));
		tyg_ret(p, tyg_call(p, sig, tyg_direct(tyg_symbol(m, "c_stk_b3", TYG_SYM_FUNC | TYG_SYM_GLOBAL | TYG_SYM_FOREIGN)), args, 10, NULL, NULL));
		check(tyg_proc_end(p));
	}
}

// Fills the arguments of c_many/tg_many with the values abi_main.c expects.
static int many_args(tyg_proc *p, tyg_arg *args, tyg_local f3, tyg_local a16, tyg_local li) {
	int n = 0;
	for (int i = 0; i < 9; i++) args[n++] = val(tyg_iconst(p, TYG_I32, -(i + 1)));
	for (int i = 0; i < 9; i++) args[n++] = val(kd(p, 0.5 * (i + 1)));
	for (int i = 0; i < 3; i++) st(p, TYG_F32, tyg_at_local(f3, 4*i), tyg_fconst(p, TYG_F32, 1.25 * (i + 1)));
	args[n++] = mem(tyg_at_local(f3, 0));
	args[n++] = val(tyg_iconst(p, TYG_I8, -7));
	args[n++] = val(tyg_iconst(p, TYG_I16, 65000));
	st(p, TYG_I64, tyg_at_local(a16, 0), k64(p, 1000));
	st(p, TYG_I64, tyg_at_local(a16, 8), k64(p, 300));
	args[n++] = mem(tyg_at_local(a16, 0));
	st(p, TYG_I64, tyg_at_local(li, 0), k64(p, 1LL << 40));
	st(p, TYG_I32, tyg_at_local(li, 8), tyg_iconst(p, TYG_I32, -9));
	args[n++] = mem(tyg_at_local(li, 0));
	return n;
}

// Packed records, aligned to 1 as #pragma pack(1) lays them out (Goose's fixed structs are such
// records). A field off its natural alignment makes SysV x86-64 pass the whole record in memory;
// an aligned one still goes in registers. abi_main.c declares the same records:
//   PA { u8 a; float b; }                    5 bytes, b misaligned
//   PB { float a; u8 b; }                    5, aligned: one integer register
//   PC { int a; double b; }                  12, b misaligned
//   PD { u8 t; struct { float x, y; } v; }   9, v's floats misaligned
//   PE { long long a; u8 b; }                9, aligned: two integer registers
//   PF { u8 a; int b[2]; }                   9, the array misaligned
//   PG { short a; u8 b, c; float d; double e; }  16, aligned: an integer and an SSE register
#define NPK 7
static const tyg_type *pkt[NPK];
static const int pk_size[NPK] = {5, 5, 12, 9, 9, 9, 16};

static void make_pk_types(void) {
	const tyg_type *u8 = TI(TYG_I8, 0), *f32 = T(TYG_F32), *f64 = T(TYG_F64), *i32 = T(TYG_I32), *i64 = T(TYG_I64);
	tyg_field pa[2] = {{0, 0}, {0, 1}}, pb[2] = {{0, 0}, {0, 4}}, pc[2] = {{0, 0}, {0, 4}};
	tyg_field v[2] = {{0, 0}, {0, 4}}, pd[2] = {{0, 0}, {0, 1}}, pe[2] = {{0, 0}, {0, 8}};
	tyg_field pf[2] = {{0, 0}, {0, 1}}, pg[5] = {{0, 0}, {0, 2}, {0, 3}, {0, 4}, {0, 8}};
	pa[0].type = u8;  pa[1].type = f32;
	pb[0].type = f32; pb[1].type = u8;
	pc[0].type = i32; pc[1].type = f64;
	v[0].type = v[1].type = f32;
	pd[0].type = u8;  pd[1].type = tyg_type_record(m, 8, 1, v, 2);
	pe[0].type = i64; pe[1].type = u8;
	pf[0].type = u8;  pf[1].type = tyg_type_array(m, i32, 2);
	pg[0].type = TI(TYG_I16, 1); pg[1].type = pg[2].type = u8; pg[3].type = f32; pg[4].type = f64;
	pkt[0] = tyg_type_record(m, 5, 1, pa, 2);
	pkt[1] = tyg_type_record(m, 5, 1, pb, 2);
	pkt[2] = tyg_type_record(m, 12, 1, pc, 2);
	pkt[3] = tyg_type_record(m, 9, 1, pd, 2);
	pkt[4] = tyg_type_record(m, 9, 1, pe, 2);
	pkt[5] = tyg_type_record(m, 9, 1, pf, 2);
	pkt[6] = tyg_type_record(m, 16, 1, pg, 5);
}

// The bytes of packed record i at a, each times (100*i + its offset + 1), as abi_main.c's pk_sum
static tyg_val pk_sum_at(tyg_proc *p, int i, tyg_addr a) {
	tyg_val s = k64(p, 0);
	for (int j = 0; j < pk_size[i]; j++) {
		tyg_addr b = a;
		b.offset += j;
		tyg_val byte = tyg_convert(p, TYG_ZEXT, TYG_I64, ld(p, TYG_I8, b));
		s = add(p, s, mul(p, byte, k64(p, 100*i + j + 1)));
	}
	return s;
}

// The parameters of tg_pk_sum and c_pk_sum: five long longs, which leave one integer register,
// the seven records, and a long long
static int pk_sum_params(const tyg_type **ps) {
	int n = 0;
	for (int i = 0; i < 5; i++) ps[n++] = T(TYG_I64);
	for (int i = 0; i < NPK; i++) ps[n++] = pkt[i];
	ps[n++] = T(TYG_I64);
	return n;
}

// struct Pi tg_pk_echo<i>(struct Pi v) { return v; }
// long long tg_pk_sum(k0..k4, PA..PG, last) { return sum of k_n*(n+1) + each pk_sum + 1000*last; }
// long long tg_pk_call(void): c_pk_sum and each c_pk_echo<i>, on records filled as pk_fill does
static void build_pk(void) {
	make_pk_types();
	for (int i = 0; i < NPK; i++) {
		char name[32];
		snprintf(name, sizeof(name), "tg_pk_echo%d", i);
		tyg_proc *p = begin(name, pkt[i], &pkt[i], 1, 0, NULL);
		tyg_ret_mem(p, tyg_at_val(tyg_param(p, 0), 0));
		check(tyg_proc_end(p));
	}
	{
		const tyg_type *ps[16];
		int n = pk_sum_params(ps);
		tyg_proc *p = begin("tg_pk_sum", T(TYG_I64), ps, n, 0, NULL);
		tyg_val s = k64(p, 0);
		for (int i = 0; i < 5; i++) s = add(p, s, mul(p, tyg_param(p, i), k64(p, i + 1)));
		for (int i = 0; i < NPK; i++) s = add(p, s, pk_sum_at(p, i, tyg_at_val(tyg_param(p, 5 + i), 0)));
		s = add(p, s, mul(p, tyg_param(p, 5 + NPK), k64(p, 1000)));
		tyg_ret(p, s);
		check(tyg_proc_end(p));
	}
	{
		tyg_proc *p = begin("tg_pk_call", T(TYG_I64), NULL, 0, 0, NULL);
		tyg_local recs[NPK];
		tyg_arg args[16];
		const tyg_type *ps[16];
		int n = pk_sum_params(ps);
		for (int i = 0; i < 5; i++) args[i] = val(k64(p, -(i + 1) * 1000003LL));
		for (int i = 0; i < NPK; i++) {
			recs[i] = tyg_local_new(p, pk_size[i], 1);
			for (int j = 0; j < pk_size[i]; j++)
				st(p, TYG_I8, tyg_at_local(recs[i], j), tyg_iconst(p, TYG_I8, (i*37 + j*11 + 1) & 0xff));
			args[5 + i] = mem(tyg_at_local(recs[i], 0));
		}
		args[5 + NPK] = val(k64(p, 77));
		tyg_sig *sig = tyg_sig_new(m, T(TYG_I64), ps, n, 0);
		tyg_val r = tyg_call(p, sig, tyg_direct(tyg_symbol(m, "c_pk_sum", TYG_SYM_FUNC | TYG_SYM_GLOBAL | TYG_SYM_FOREIGN)), args, n, NULL, NULL);
		for (int i = 0; i < NPK; i++) {
			char name[32];
			snprintf(name, sizeof(name), "c_pk_echo%d", i);
			tyg_sig *es = tyg_sig_new(m, pkt[i], &pkt[i], 1, 0);
			tyg_local out = tyg_local_new(p, pk_size[i], 1);
			tyg_addr at = tyg_at_local(out, 0);
			tyg_arg a = mem(tyg_at_local(recs[i], 0));
			tyg_call(p, es, tyg_direct(tyg_symbol(m, name, TYG_SYM_FUNC | TYG_SYM_GLOBAL | TYG_SYM_FOREIGN)), &a, 1, NULL, &at);
			r = add(p, r, mul(p, pk_sum_at(p, i, at), k64(p, i + 2)));
		}
		tyg_ret(p, r);
		check(tyg_proc_end(p));
	}
}

// double tg_call_c(void): calls C with every kind of argument and result and folds the results
static void build_call_c(void) {
	tyg_proc *p = begin("tg_call_c", T(TYG_F64), NULL, 0, 0, NULL);
	tyg_val r;
	// c_many
	{
		const tyg_type *ps[32];
		int n = many_params(ps);
		tyg_sig *sig = tyg_sig_new(m, T(TYG_F64), ps, n, 0);
		tyg_local f3 = tyg_local_new(p, 12, 4), a16 = tyg_local_new(p, 16, 16), li = tyg_local_new(p, 16, 8);
		tyg_arg args[32];
		many_args(p, args, f3, a16, li);
		r = tyg_call(p, sig, tyg_direct(tyg_symbol(m, "c_many", TYG_SYM_FUNC | TYG_SYM_GLOBAL | TYG_SYM_FOREIGN)), args, n, NULL, NULL);
	}
	// struct F3 c_f3(void), struct D4 c_d4(void), struct LI c_li(void), struct L3 c_l3(long long), struct A16 c_a16(void)
	{
		const tyg_type *rets[5] = {f3t, d4t, lit, l3t, a16t};
		const char *names[5] = {"c_f3", "c_d4", "c_li", "c_l3", "c_a16"};
		for (int k = 0; k < 5; k++) {
			const tyg_type *ps[1] = {T(TYG_I64)};
			tyg_sig *sig = tyg_sig_new(m, rets[k], ps, k == 3 ? 1 : 0, 0);
			tyg_local out = tyg_local_new(p, tyg_type_size(rets[k]), tyg_type_align(rets[k]));
			tyg_addr at = tyg_at_local(out, 0);
			tyg_arg a = val(k64(p, 5));
			tyg_call(p, sig, tyg_direct(tyg_symbol(m, names[k], TYG_SYM_FUNC | TYG_SYM_GLOBAL | TYG_SYM_FOREIGN)), &a, k == 3 ? 1 : 0, NULL, &at);
			switch (k) {
			case 0: for (int i = 0; i < 3; i++) r = add(p, r, mul(p, tod(p, ld(p, TYG_F32, tyg_at_local(out, 4*i))), kd(p, 10 * (i + 1)))); break;
			case 1: for (int i = 0; i < 4; i++) r = add(p, r, mul(p, ld(p, TYG_F64, tyg_at_local(out, 8*i)), kd(p, 100 * (i + 1)))); break;
			case 2: r = add(p, r, add(p, tod(p, ld(p, TYG_I64, tyg_at_local(out, 0))), tod(p, ld(p, TYG_I32, tyg_at_local(out, 8))))); break;
			case 3: for (int i = 0; i < 3; i++) r = add(p, r, mul(p, tod(p, ld(p, TYG_I64, tyg_at_local(out, 8*i))), kd(p, 1000 * (i + 1)))); break;
			case 4: r = add(p, r, tod(p, tyg_bin(p, TYG_SUB, ld(p, TYG_I64, tyg_at_local(out, 0)), ld(p, TYG_I64, tyg_at_local(out, 8))))); break;
			}
		}
	}
	// signed char c_schar(void) and unsigned short c_ushort(void): only the low bits mean something
	{
		tyg_sig *s1 = tyg_sig_new(m, TI(TYG_I8, 1), NULL, 0, 0);
		tyg_sig *s2 = tyg_sig_new(m, TI(TYG_I16, 0), NULL, 0, 0);
		tyg_val a = tyg_call(p, s1, tyg_direct(tyg_symbol(m, "c_schar", TYG_SYM_FUNC | TYG_SYM_FOREIGN | TYG_SYM_GLOBAL)), NULL, 0, NULL, NULL);
		tyg_val b = tyg_call(p, s2, tyg_direct(tyg_symbol(m, "c_ushort", TYG_SYM_FUNC | TYG_SYM_FOREIGN | TYG_SYM_GLOBAL)), NULL, 0, NULL, NULL);
		r = add(p, r, tod(p, tyg_convert(p, TYG_SEXT, TYG_I32, a)));
		r = add(p, r, tod(p, tyg_convert(p, TYG_ZEXT, TYG_I32, b)));
	}
	// a call through a pointer: int (*)(signed char, unsigned short, short) = c_small
	{
		const tyg_type *ps[3] = {TI(TYG_I8, 1), TI(TYG_I16, 0), TI(TYG_I16, 1)};
		tyg_sig *sig = tyg_sig_new(m, T(TYG_I32), ps, 3, 0);
		tyg_sym fs = tyg_symbol(m, "c_small_ptr", TYG_SYM_GLOBAL | TYG_SYM_FOREIGN);
		tyg_val fp = ld(p, TYG_I64, tyg_at_sym(fs, 0));
		tyg_arg args[3] = {val(tyg_iconst(p, TYG_I8, -100)), val(tyg_iconst(p, TYG_I16, 60000)), val(tyg_iconst(p, TYG_I16, -30000))};
		r = add(p, r, tod(p, tyg_call(p, sig, tyg_indirect(fp), args, 3, NULL, NULL)));
	}
	tyg_ret(p, r);
	check(tyg_proc_end(p));
}

// The vararg sequence of c_vread and tg_vread after the count: int 7, double 2.5, F3, L3,
// LI, A16, int -3, D4, CS, then 9 doubles 0.25*(k+1), and a long long.
static void vararg_values(tyg_proc *p, tyg_arg *args, const tyg_type **vt, int *n) {
	tyg_local f3 = tyg_local_new(p, 12, 4), l3 = tyg_local_new(p, 24, 8), li = tyg_local_new(p, 16, 8);
	tyg_local a16 = tyg_local_new(p, 16, 16), d4 = tyg_local_new(p, 32, 8), cs = tyg_local_new(p, 4, 2);
	int k = *n;
	args[k] = val(tyg_iconst(p, TYG_I32, 7)); vt[k - 1] = T(TYG_I32); k++;
	args[k] = val(kd(p, 2.5)); vt[k - 1] = T(TYG_F64); k++;
	for (int i = 0; i < 3; i++) st(p, TYG_F32, tyg_at_local(f3, 4*i), tyg_fconst(p, TYG_F32, 0.5 + i));
	args[k] = mem(tyg_at_local(f3, 0)); vt[k - 1] = f3t; k++;
	for (int i = 0; i < 3; i++) st(p, TYG_I64, tyg_at_local(l3, 8*i), k64(p, 10 * (i + 1)));
	args[k] = mem(tyg_at_local(l3, 0)); vt[k - 1] = l3t; k++;
	st(p, TYG_I64, tyg_at_local(li, 0), k64(p, 100));
	st(p, TYG_I32, tyg_at_local(li, 8), tyg_iconst(p, TYG_I32, 200));
	args[k] = mem(tyg_at_local(li, 0)); vt[k - 1] = lit; k++;
	st(p, TYG_I64, tyg_at_local(a16, 0), k64(p, 1000));
	st(p, TYG_I64, tyg_at_local(a16, 8), k64(p, 2000));
	args[k] = mem(tyg_at_local(a16, 0)); vt[k - 1] = a16t; k++;
	args[k] = val(tyg_iconst(p, TYG_I32, -3)); vt[k - 1] = T(TYG_I32); k++;
	for (int i = 0; i < 4; i++) st(p, TYG_F64, tyg_at_local(d4, 8*i), kd(p, 1.5 * (i + 1)));
	args[k] = mem(tyg_at_local(d4, 0)); vt[k - 1] = d4t; k++;
	st(p, TYG_I8, tyg_at_local(cs, 0), tyg_iconst(p, TYG_I8, -2));
	st(p, TYG_I16, tyg_at_local(cs, 2), tyg_iconst(p, TYG_I16, 300));
	args[k] = mem(tyg_at_local(cs, 0)); vt[k - 1] = cst; k++;
	for (int i = 0; i < 9; i++) { args[k] = val(kd(p, 0.25 * (i + 1))); vt[k - 1] = T(TYG_F64); k++; }
	args[k] = val(k64(p, 123456789012LL)); vt[k - 1] = T(TYG_I64); k++;
	*n = k;
}

// double tg_call_vread(void) { return c_vread(N, ...the sequence...); }
static void build_call_vread(void) {
	tyg_proc *p = begin("tg_call_vread", T(TYG_F64), NULL, 0, 0, NULL);
	const tyg_type *ps[1] = {T(TYG_I32)};
	tyg_sig *sig = tyg_sig_new(m, T(TYG_F64), ps, 1, TYG_SIG_VARIADIC);
	tyg_arg args[32];
	const tyg_type *vt[32];
	int n = 1;
	args[0] = val(tyg_iconst(p, TYG_I32, 19));
	vararg_values(p, args, vt, &n);
	tyg_ret(p, tyg_call(p, sig, tyg_direct(tyg_symbol(m, "c_vread", TYG_SYM_FUNC | TYG_SYM_GLOBAL | TYG_SYM_FOREIGN)), args, n, vt, NULL));
	check(tyg_proc_end(p));
}

// double tg_vread(int n, ...): reads the sequence like c_vread, the first int twice through a va_copy
static void build_vread(void) {
	const tyg_type *ps[1] = {T(TYG_I32)};
	tyg_proc *p = begin("tg_vread", T(TYG_F64), ps, 1, TYG_SIG_VARIADIC, NULL);
	int64_t vsize = tyg_va_list_size(m);
	tyg_local ap = tyg_local_new(p, vsize, tyg_va_list_align(m)), ap2 = tyg_local_new(p, vsize, tyg_va_list_align(m));
	tyg_val apv = tyg_addr_of(p, tyg_at_local(ap, 0));
	tyg_va_start(p, apv);
	tyg_va_copy(p, tyg_addr_of(p, tyg_at_local(ap2, 0)), apv);
	tyg_val s = tod(p, tyg_param(p, 0));
	tyg_val x;
	x = tyg_va_arg(p, tyg_addr_of(p, tyg_at_local(ap2, 0)), T(TYG_I32));
	s = add(p, s, tod(p, ld(p, TYG_I32, tyg_at_val(x, 0))));                      // 7 from the copy
	x = tyg_va_arg(p, apv, T(TYG_I32));
	s = add(p, s, tod(p, ld(p, TYG_I32, tyg_at_val(x, 0))));                      // 7
	x = tyg_va_arg(p, apv, T(TYG_F64));
	s = add(p, s, mul(p, ld(p, TYG_F64, tyg_at_val(x, 0)), kd(p, 2)));            // 2.5
	x = tyg_va_arg(p, apv, f3t);
	for (int i = 0; i < 3; i++) s = add(p, s, mul(p, tod(p, ld(p, TYG_F32, tyg_at_val(x, 4*i))), kd(p, 3 + i)));
	x = tyg_va_arg(p, apv, l3t);
	for (int i = 0; i < 3; i++) s = add(p, s, mul(p, tod(p, ld(p, TYG_I64, tyg_at_val(x, 8*i))), kd(p, 7 + i)));
	x = tyg_va_arg(p, apv, lit);
	s = add(p, s, tod(p, ld(p, TYG_I64, tyg_at_val(x, 0))));
	s = add(p, s, mul(p, tod(p, ld(p, TYG_I32, tyg_at_val(x, 8))), kd(p, 3)));
	x = tyg_va_arg(p, apv, a16t);
	s = add(p, s, tod(p, ld(p, TYG_I64, tyg_at_val(x, 0))));
	s = add(p, s, mul(p, tod(p, ld(p, TYG_I64, tyg_at_val(x, 8))), kd(p, 5)));
	x = tyg_va_arg(p, apv, T(TYG_I32));
	s = add(p, s, mul(p, tod(p, ld(p, TYG_I32, tyg_at_val(x, 0))), kd(p, 11)));
	x = tyg_va_arg(p, apv, d4t);
	for (int i = 0; i < 4; i++) s = add(p, s, mul(p, ld(p, TYG_F64, tyg_at_val(x, 8*i)), kd(p, 13 + i)));
	x = tyg_va_arg(p, apv, cst);
	s = add(p, s, mul(p, tod(p, ld(p, TYG_I8, tyg_at_val(x, 0))), kd(p, 17)));
	s = add(p, s, mul(p, tod(p, ld(p, TYG_I16, tyg_at_val(x, 2))), kd(p, 19)));
	for (int i = 0; i < 9; i++) {
		x = tyg_va_arg(p, apv, T(TYG_F64));
		s = add(p, s, mul(p, ld(p, TYG_F64, tyg_at_val(x, 0)), kd(p, 23 + i)));
	}
	x = tyg_va_arg(p, apv, T(TYG_I64));
	s = add(p, s, tod(p, ld(p, TYG_I64, tyg_at_val(x, 0))));
	tyg_ret(p, s);
	check(tyg_proc_end(p));
}

// int tg_snprintf(char *buf): snprintf(buf, 200, "%d %.2f ... %s %lld", ...) with more doubles than registers
static void build_snprintf(void) {
	static const char fmt[] = "%d %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %s %lld %c";
	static const char word[] = "word";
	tyg_sym fs = tyg_symbol(m, "tg_snfmt", 0);
	check(tyg_data(m, fs, TYG_SECTION_RODATA, fmt, sizeof(fmt), 1));
	tyg_sym ws = tyg_symbol(m, "tg_word", 0);
	check(tyg_data(m, ws, TYG_SECTION_RODATA, word, sizeof(word), 1));
	const tyg_type *sps[3] = {tyg_type_ptr(m), T(TYG_I64), tyg_type_ptr(m)};
	tyg_sig *snsig = tyg_sig_new(m, T(TYG_I32), sps, 3, TYG_SIG_VARIADIC);
	const tyg_type *ps[1] = {tyg_type_ptr(m)};
	tyg_proc *p = begin("tg_snprintf", T(TYG_I32), ps, 1, 0, NULL);
	tyg_arg args[20];
	const tyg_type *vt[20];
	int n = 0;
	args[n++] = val(tyg_param(p, 0));
	args[n++] = val(k64(p, 200));
	args[n++] = val(tyg_addr_of(p, tyg_at_sym(fs, 0)));
	args[n] = val(tyg_iconst(p, TYG_I32, -42)); vt[n - 3] = T(TYG_I32); n++;
	for (int i = 0; i < 10; i++) { args[n] = val(kd(p, 1.25 * (i + 1))); vt[n - 3] = T(TYG_F64); n++; }
	args[n] = val(tyg_addr_of(p, tyg_at_sym(ws, 0))); vt[n - 3] = tyg_type_ptr(m); n++;
	args[n] = val(k64(p, -1234567890123LL)); vt[n - 3] = T(TYG_I64); n++;
	args[n] = val(tyg_iconst(p, TYG_I32, 'z')); vt[n - 3] = T(TYG_I32); n++;
	tyg_ret(p, tyg_call(p, snsig, tyg_direct(tyg_symbol(m, "snprintf", TYG_SYM_FUNC | TYG_SYM_GLOBAL | TYG_SYM_FOREIGN)), args, n, vt, NULL));
	check(tyg_proc_end(p));
}

// _Thread_local int tg_tls_counter = 5; _Thread_local long long tg_tls_big;
// int tg_tls_bump(void) { tg_tls_big += 3; return ++tg_tls_counter; }
// long long tg_tls_get_big(void) { return tg_tls_big; }
static void build_tls(void) {
	static const int five = 5;
	tyg_sym c = tyg_symbol(m, "tg_tls_counter", TYG_SYM_GLOBAL | TYG_SYM_TLS);
	check(tyg_data(m, c, TYG_SECTION_TDATA, &five, 4, 4));
	tyg_sym b = tyg_symbol(m, "tg_tls_big", TYG_SYM_GLOBAL | TYG_SYM_TLS);
	check(tyg_data(m, b, TYG_SECTION_TBSS, NULL, 8, 8));
	{
		tyg_proc *p = begin("tg_tls_bump", T(TYG_I32), NULL, 0, 0, NULL);
		st(p, TYG_I64, tyg_at_sym(b, 0), add(p, ld(p, TYG_I64, tyg_at_sym(b, 0)), k64(p, 3)));
		tyg_val v = add(p, ld(p, TYG_I32, tyg_at_sym(c, 0)), tyg_iconst(p, TYG_I32, 1));
		st(p, TYG_I32, tyg_at_sym(c, 0), v);
		tyg_ret(p, v);
		check(tyg_proc_end(p));
	}
	{
		tyg_proc *p = begin("tg_tls_get_big", T(TYG_I64), NULL, 0, 0, NULL);
		tyg_ret(p, ld(p, TYG_I64, tyg_at_sym(b, 0)));
		check(tyg_proc_end(p));
	}
}

// long long tg_misc(long long x): alloca, an over-aligned local, a large frame, atomics,
// bit operations, division, select and conversions; abi_main.c computes the same in C
static void build_misc(void) {
	static const long long zero = 0;
	tyg_sym g = tyg_symbol(m, "tg_atomic_cell", TYG_SYM_GLOBAL);
	check(tyg_data(m, g, TYG_SECTION_DATA, &zero, 8, 8));
	const tyg_type *ps[1] = {T(TYG_I64)};
	tyg_proc *p = begin("tg_misc", T(TYG_I64), ps, 1, 0, NULL);
	tyg_val x = tyg_param(p, 0);
	tyg_local big = tyg_local_new(p, 100000, 8);
	tyg_local al = tyg_local_new(p, 64, 64);
	// big[99992] = x * 3; r = big[99992]
	st(p, TYG_I64, tyg_at_local(big, 99992), mul(p, x, k64(p, 3)));
	st(p, TYG_I64, tyg_at_local(big, 8), k64(p, 11));
	tyg_val r = add(p, ld(p, TYG_I64, tyg_at_local(big, 99992)), ld(p, TYG_I64, tyg_at_local(big, 8)));
	// the over-aligned local's address is a multiple of 64
	tyg_val alp = tyg_addr_of(p, tyg_at_local(al, 0));
	r = add(p, r, tyg_bin(p, TYG_AND, alp, k64(p, 63)));
	st(p, TYG_I64, tyg_at_local(al, 56), k64(p, 5));
	r = add(p, r, ld(p, TYG_I64, tyg_at_local(al, 56)));
	// alloca((x & 255) + 24) aligned to 32: fill and read back
	tyg_val n = add(p, tyg_bin(p, TYG_AND, x, k64(p, 255)), k64(p, 24));
	tyg_val ap = tyg_alloca(p, n, 32);
	r = add(p, r, tyg_bin(p, TYG_AND, ap, k64(p, 31)));
	tyg_set_dyn(p, ap, tyg_iconst(p, TYG_I8, 0x11), n);
	r = add(p, r, tyg_convert(p, TYG_ZEXT, TYG_I64, ld(p, TYG_I8, tyg_at_val(ap, 7))));
	// atomics on the global
	tyg_atomic_store(p, TYG_I64, tyg_at_sym(g, 0), k64(p, 40));
	r = add(p, r, tyg_atomic_rmw(p, TYG_RMW_ADD, tyg_at_sym(g, 0), k64(p, 2)));     // 40, cell 42
	r = add(p, r, tyg_atomic_rmw(p, TYG_RMW_XCHG, tyg_at_sym(g, 0), k64(p, 7)));    // 42, cell 7
	r = add(p, r, tyg_atomic_rmw(p, TYG_RMW_NAND, tyg_at_sym(g, 0), k64(p, 3)));    // 7, cell ~(7&3) = -4
	tyg_val ok = 0;
	r = add(p, r, tyg_atomic_cas(p, tyg_at_sym(g, 0), k64(p, -4), k64(p, 9), &ok)); // -4, cell 9
	r = add(p, r, tyg_convert(p, TYG_ZEXT, TYG_I64, ok));
	r = add(p, r, tyg_atomic_load(p, TYG_I64, tyg_at_sym(g, 0)));                   // 9
	// bits of x
	r = add(p, r, tyg_intrin(p, TYG_POPCOUNT, x));
	r = add(p, r, tyg_intrin(p, TYG_CTZ, x));
	r = add(p, r, tyg_intrin(p, TYG_CLZ, x));
	r = add(p, r, tyg_bin(p, TYG_LSHR, tyg_intrin(p, TYG_BSWAP, x), k64(p, 56)));
	tyg_val x32 = tyg_convert(p, TYG_TRUNC, TYG_I32, x);
	r = add(p, r, sx64(p, tyg_intrin(p, TYG_CLZ, x32)));
	// division
	r = add(p, r, tyg_bin(p, TYG_SDIV, tyg_un(p, TYG_NEG, x), k64(p, 7)));
	r = add(p, r, tyg_bin(p, TYG_SREM, tyg_un(p, TYG_NEG, x), k64(p, 7)));
	r = add(p, r, tyg_bin(p, TYG_UDIV, x, k64(p, 5)));
	// select on a float compare, and conversions
	tyg_val xd = tyg_convert(p, TYG_SITOF, TYG_F64, x);
	tyg_val sel = tyg_select(p, tyg_cmp(p, TYG_FGT, xd, kd(p, 100.5)), k64(p, 1000), k64(p, 2000));
	r = add(p, r, sel);
	tyg_val xf = tyg_convert(p, TYG_FTRUNC, TYG_F32, tyg_bin(p, TYG_FDIV, xd, kd(p, 4)));
	r = add(p, r, tyg_convert(p, TYG_FTOSI, TYG_I64, tyg_intrin(p, TYG_SQRT, tyg_convert(p, TYG_FEXT, TYG_F64, xf))));
	r = add(p, r, tyg_convert(p, TYG_FTOUI, TYG_I64, tyg_convert(p, TYG_UITOF, TYG_F64, tyg_bin(p, TYG_SHL, x, k64(p, 2)))));
	tyg_ret(p, r);
	check(tyg_proc_end(p));
}

// For each width N in 8, 16, 32, 64 and each read-modify-write op:
//   uintN_t tg_rmw_<op>_N(uintN_t *p, uintN_t v) { return the old *p; *p = *p op v; }
//   uintN_t tg_cas_N(uintN_t *p, uintN_t expected, uintN_t desired, int *ok)
static void build_atomics(void) {
	static const struct { const char *name; tyg_rmw op; } ops[7] = {
		{"xchg", TYG_RMW_XCHG}, {"add", TYG_RMW_ADD}, {"sub", TYG_RMW_SUB}, {"and", TYG_RMW_AND},
		{"or", TYG_RMW_OR}, {"xor", TYG_RMW_XOR}, {"nand", TYG_RMW_NAND},
	};
	static const tyg_scalar sizes[4] = {TYG_I8, TYG_I16, TYG_I32, TYG_I64};
	char name[64];
	for (int k = 0; k < 4; k++) {
		const tyg_type *t = TI(sizes[k], 0);
		int bits = 8 << k;
		for (int i = 0; i < 7; i++) {
			const tyg_type *ps[2] = {tyg_type_ptr(m), t};
			snprintf(name, sizeof(name), "tg_rmw_%s_%d", ops[i].name, bits);
			tyg_proc *p = begin(name, t, ps, 2, 0, NULL);
			tyg_ret(p, tyg_atomic_rmw(p, ops[i].op, tyg_at_val(tyg_param(p, 0), 0), tyg_param(p, 1)));
			check(tyg_proc_end(p));
		}
		const tyg_type *ps[4] = {tyg_type_ptr(m), t, t, tyg_type_ptr(m)};
		snprintf(name, sizeof(name), "tg_cas_%d", bits);
		tyg_proc *p = begin(name, t, ps, 4, 0, NULL);
		tyg_val ok = 0;
		tyg_val old = tyg_atomic_cas(p, tyg_at_val(tyg_param(p, 0), 0), tyg_param(p, 1), tyg_param(p, 2), &ok);
		st(p, TYG_I32, tyg_at_val(tyg_param(p, 3), 0), tyg_convert(p, TYG_ZEXT, TYG_I32, ok));
		tyg_ret(p, old);
		check(tyg_proc_end(p));
	}
}

// long long tg_far(long long x): a conditional branch over about 4 MB of code
static void build_far(void) {
	const tyg_type *ps[1] = {T(TYG_I64)};
	tyg_proc *p = begin("tg_far", T(TYG_I64), ps, 1, 0, NULL);
	tyg_local acc = tyg_local_new(p, 8, 8);
	tyg_block body = tyg_block_new(p), done = tyg_block_new(p);
	st(p, TYG_I64, tyg_at_local(acc, 0), tyg_param(p, 0));
	tyg_branch(p, tyg_cmp(p, TYG_SGT, tyg_param(p, 0), k64(p, 0)), body, done);
	tyg_block_set(p, body);
	for (int i = 0; i < 250000; i++) {
		tyg_val v = tyg_load(p, TYG_I64, tyg_at_local(acc, 0), TYG_MEM_VOLATILE);
		tyg_store(p, TYG_I64, tyg_at_local(acc, 0), tyg_bin(p, TYG_ADD, v, k64(p, 1)), TYG_MEM_VOLATILE);
	}
	tyg_jump(p, done);
	tyg_block_set(p, done);
	tyg_ret(p, ld(p, TYG_I64, tyg_at_local(acc, 0)));
	check(tyg_proc_end(p));
}

////////////////////////////////////////////////////////////////
// long double
////////////////////////////////////////////////////////////////

static const tyg_type *ldt, *ldst, *ldit; // long double, struct LDS {long double x;}, struct LDI {int i; long double x;}
static const tyg_type *ldut;              // union LDU {long double x; struct {long long a, b;} p;}
static int64_t ldsz, ldal;                // the size and alignment of long double
static int ld_is_double;                  // arm64 macOS and Windows

static void make_ld_types(void) {
	ldt = T(TYG_F80);
	ldsz = tyg_type_size(ldt);
	ldal = tyg_type_align(ldt);
	ld_is_double = ldsz == 8;
	tyg_field f1[1] = {{0, 0}};
	f1[0].type = ldt;
	ldst = tyg_type_record(m, ldsz, ldal, f1, 1);
	tyg_field f2[2] = {{0, 0}, {0, 0}};
	f2[0].type = T(TYG_I32);
	f2[1].type = ldt;
	f2[1].offset = ldal;
	ldit = tyg_type_record(m, ldal + ldsz, ldal, f2, 2);
	tyg_field l2[2] = {{0, 0}, {0, 8}};
	l2[0].type = l2[1].type = T(TYG_I64);
	tyg_field u[2] = {{0, 0}, {0, 0}};
	u[0].type = ldt;
	u[1].type = tyg_type_record(m, 16, 8, l2, 2);
	ldut = tyg_type_record(m, 16, ldal > 8 ? ldal : 8, u, 2);
}

// The target's bytes of the long double v: a double, or for x87 the 64-bit mantissa with its
// explicit integer bit, then the sign and the 15-bit exponent. Normal numbers and zero only.
static void ld_bytes(double v, unsigned char out[16]) {
	uint64_t bits, mant;
	memset(out, 0, 16);
	memcpy(&bits, &v, 8);
	if (ld_is_double) {
		memcpy(out, &bits, 8);
		return;
	}
	int sign = (int)(bits >> 63), exp = (int)((bits >> 52) & 0x7ff);
	if (exp != 0) {
		mant = (1ull << 63) | ((bits & ((1ull << 52) - 1)) << 11);
		exp = exp - 1023 + 16383;
		memcpy(out, &mant, 8);
	}
	out[8] = (unsigned char)exp;
	out[9] = (unsigned char)((exp >> 8) | (sign << 7));
}

// stores the long double constant v at `at`
static void ldk(tyg_proc *p, tyg_addr at, double v) {
	unsigned char b[16];
	uint64_t lo;
	uint16_t hi;
	ld_bytes(v, b);
	memcpy(&lo, b, 8);
	st(p, TYG_I64, at, k64(p, (int64_t)lo));
	if (ld_is_double) return;
	memcpy(&hi, b + 8, 2);
	at.offset += 8;
	st(p, TYG_I16, at, tyg_iconst(p, TYG_I16, (int16_t)hi));
}

static tyg_local ld_local(tyg_proc *p) { return tyg_local_new(p, ldsz, ldal); }

// void tg_ld_ops(long double a, long double b, long double *out):
// out = {a+b, a-b, a*b, a/b, -a, a*b-b computed in place, a+0.5}
static void build_ld_ops(void) {
	const tyg_type *ps[3] = {ldt, ldt, tyg_type_ptr(m)};
	tyg_proc *p = begin("tg_ld_ops", T(TYG_VOID), ps, 3, 0, NULL);
	tyg_addr a = tyg_at_val(tyg_param(p, 0), 0), b = tyg_at_val(tyg_param(p, 1), 0);
	tyg_val out = tyg_param(p, 2);
	tyg_ld_bin(p, TYG_FADD, tyg_at_val(out, 0*ldsz), a, b);
	tyg_ld_bin(p, TYG_FSUB, tyg_at_val(out, 1*ldsz), a, b);
	tyg_ld_bin(p, TYG_FMUL, tyg_at_val(out, 2*ldsz), a, b);
	tyg_ld_bin(p, TYG_FDIV, tyg_at_val(out, 3*ldsz), a, b);
	tyg_ld_neg(p, tyg_at_val(out, 4*ldsz), a);
	tyg_local t = ld_local(p);
	tyg_debug_var(p, "t", tyg_dt_base(m, "long double", ldsz, TYG_DT_FLOAT), tyg_at_local(t, 0), file, 1);
	tyg_copy(p, tyg_at_local(t, 0), a, ldsz);
	tyg_ld_bin(p, TYG_FMUL, tyg_at_local(t, 0), tyg_at_local(t, 0), b);
	tyg_ld_bin(p, TYG_FSUB, tyg_at_local(t, 0), tyg_at_local(t, 0), b);
	tyg_copy(p, tyg_at_val(out, 5*ldsz), tyg_at_local(t, 0), ldsz);
	tyg_local k = ld_local(p);
	ldk(p, tyg_at_local(k, 0), 0.5);
	tyg_ld_bin(p, TYG_FADD, tyg_at_val(out, 6*ldsz), a, tyg_at_local(k, 0));
	tyg_ret(p, 0);
	check(tyg_proc_end(p));
}

// int tg_ld_cmp(long double a, long double b): bit i is condition i of ==, !=, <, <=, >, >=
static void build_ld_cmp(void) {
	const tyg_type *ps[2] = {ldt, ldt};
	tyg_proc *p = begin("tg_ld_cmp", T(TYG_I32), ps, 2, 0, NULL);
	tyg_addr a = tyg_at_val(tyg_param(p, 0), 0), b = tyg_at_val(tyg_param(p, 1), 0);
	tyg_val r = tyg_iconst(p, TYG_I32, 0);
	for (int i = 0; i < 6; i++) {
		tyg_val c = tyg_convert(p, TYG_ZEXT, TYG_I32, tyg_ld_cmp(p, (tyg_cond)(TYG_FEQ + i), a, b));
		r = tyg_bin(p, TYG_OR, r, tyg_bin(p, TYG_SHL, c, tyg_iconst(p, TYG_I32, i)));
	}
	tyg_ret(p, r);
	check(tyg_proc_end(p));
}

// tg_ld_to_X(long double x) and long double tg_ld_from_X(X v) for each scalar type X
static void build_ld_convs(void) {
	static const struct { const char *name; tyg_scalar s; int sgn; } ts[8] = {
		{"ll", TYG_I64, 1}, {"ull", TYG_I64, 0}, {"i", TYG_I32, 1}, {"u", TYG_I32, 0},
		{"sc", TYG_I8, 1}, {"us", TYG_I16, 0}, {"d", TYG_F64, 0}, {"f", TYG_F32, 0},
	};
	for (int i = 0; i < 8; i++) {
		int fl = ts[i].s == TYG_F32 || ts[i].s == TYG_F64;
		const tyg_type *xt = fl ? T(ts[i].s) : TI(ts[i].s, ts[i].sgn);
		char name[64];
		{
			const tyg_type *ps[1] = {ldt};
			snprintf(name, sizeof(name), "tg_ld_to_%s", ts[i].name);
			tyg_proc *p = begin(name, xt, ps, 1, 0, NULL);
			tyg_conv op = fl ? TYG_FTRUNC : ts[i].sgn ? TYG_FTOSI : TYG_FTOUI;
			tyg_ret(p, tyg_ld_to(p, op, ts[i].s, tyg_at_val(tyg_param(p, 0), 0)));
			check(tyg_proc_end(p));
		}
		{
			const tyg_type *ps[1] = {xt};
			snprintf(name, sizeof(name), "tg_ld_from_%s", ts[i].name);
			tyg_proc *p = begin(name, ldt, ps, 1, 0, NULL);
			tyg_conv op = fl ? TYG_FEXT : ts[i].sgn ? TYG_SITOF : TYG_UITOF;
			tyg_local out = ld_local(p);
			tyg_ld_from(p, op, tyg_at_local(out, 0), tyg_param(p, 0));
			tyg_ret_mem(p, tyg_at_local(out, 0));
			check(tyg_proc_end(p));
		}
	}
}

// struct LDS tg_lds_twice(struct LDS v); struct LDI tg_ldi_make(int i, long double x);
// long double tg_ldi_sum(struct LDI v, struct LDS w) { return v.i + v.x + w.x; }
static void build_ld_structs(void) {
	{
		const tyg_type *ps[1] = {ldst};
		tyg_proc *p = begin("tg_lds_twice", ldst, ps, 1, 0, NULL);
		tyg_addr v = tyg_at_val(tyg_param(p, 0), 0);
		tyg_local out = ld_local(p);
		tyg_ld_bin(p, TYG_FADD, tyg_at_local(out, 0), v, v);
		tyg_ret_mem(p, tyg_at_local(out, 0));
		check(tyg_proc_end(p));
	}
	{
		const tyg_type *ps[2] = {T(TYG_I32), ldt};
		tyg_proc *p = begin("tg_ldi_make", ldit, ps, 2, 0, NULL);
		tyg_local out = tyg_local_new(p, tyg_type_size(ldit), ldal);
		st(p, TYG_I32, tyg_at_local(out, 0), tyg_param(p, 0));
		tyg_copy(p, tyg_at_local(out, ldal), tyg_at_val(tyg_param(p, 1), 0), ldsz);
		tyg_ret_mem(p, tyg_at_local(out, 0));
		check(tyg_proc_end(p));
	}
	{
		const tyg_type *ps[2] = {ldit, ldst};
		tyg_proc *p = begin("tg_ldi_sum", ldt, ps, 2, 0, NULL);
		tyg_val v = tyg_param(p, 0), w = tyg_param(p, 1);
		tyg_local t = ld_local(p);
		tyg_ld_from(p, TYG_SITOF, tyg_at_local(t, 0), ld(p, TYG_I32, tyg_at_val(v, 0)));
		tyg_ld_bin(p, TYG_FADD, tyg_at_local(t, 0), tyg_at_local(t, 0), tyg_at_val(v, ldal));
		tyg_ld_bin(p, TYG_FADD, tyg_at_local(t, 0), tyg_at_local(t, 0), tyg_at_val(w, 0));
		tyg_ret_mem(p, tyg_at_local(t, 0));
		check(tyg_proc_end(p));
	}
	// the union's integer fields make both eightbytes INTEGER on x86-64, despite the long double
	{
		const tyg_type *ps[2] = {T(TYG_I64), T(TYG_I64)};
		tyg_proc *p = begin("tg_ldu_make", ldut, ps, 2, 0, NULL);
		tyg_local out = tyg_local_new(p, 16, 8);
		st(p, TYG_I64, tyg_at_local(out, 0), tyg_param(p, 0));
		st(p, TYG_I64, tyg_at_local(out, 8), tyg_param(p, 1));
		tyg_ret_mem(p, tyg_at_local(out, 0));
		check(tyg_proc_end(p));
	}
	{
		const tyg_type *ps[1] = {ldut};
		tyg_proc *p = begin("tg_ldu_sum", T(TYG_I64), ps, 1, 0, NULL);
		tyg_val u = tyg_param(p, 0);
		tyg_ret(p, add(p, ld(p, TYG_I64, tyg_at_val(u, 0)), ld(p, TYG_I64, tyg_at_val(u, 8))));
		check(tyg_proc_end(p));
	}
}

// long double tg_ld_call_c(long double x): calls C with long doubles and folds the results,
// see expect_ld_call_c in abi_main.c
static void build_ld_call_c(void) {
	const tyg_type *ps1[1] = {ldt};
	tyg_proc *p = begin("tg_ld_call_c", ldt, ps1, 1, 0, NULL);
	tyg_addr x = tyg_at_val(tyg_param(p, 0), 0);
	tyg_local r = ld_local(p), s = ld_local(p), t = tyg_local_new(p, tyg_type_size(ldit), ldal), u = ld_local(p);
	unsigned fl = TYG_SYM_FUNC | TYG_SYM_GLOBAL | TYG_SYM_FOREIGN;
	{
		// long double c_ld_mix(int a, long double b, double c, long double d, int e)
		const tyg_type *ps[5] = {T(TYG_I32), ldt, T(TYG_F64), ldt, T(TYG_I32)};
		tyg_sig *sig = tyg_sig_new(m, ldt, ps, 5, 0);
		tyg_arg args[5] = {val(tyg_iconst(p, TYG_I32, 3)), mem(x), val(kd(p, 2.5)), mem(x), val(tyg_iconst(p, TYG_I32, 7))};
		tyg_addr res = tyg_at_local(r, 0);
		tyg_call(p, sig, tyg_direct(tyg_symbol(m, "c_ld_mix", fl)), args, 5, NULL, &res);
	}
	{
		// struct LDS c_lds(long double v); struct LDI c_ldi(long double v)
		tyg_sig *sig = tyg_sig_new(m, ldst, ps1, 1, 0);
		tyg_arg a = mem(x);
		tyg_addr res = tyg_at_local(s, 0);
		tyg_call(p, sig, tyg_direct(tyg_symbol(m, "c_lds", fl)), &a, 1, NULL, &res);
		tyg_ld_bin(p, TYG_FADD, tyg_at_local(r, 0), tyg_at_local(r, 0), tyg_at_local(s, 0));
		sig = tyg_sig_new(m, ldit, ps1, 1, 0);
		res = tyg_at_local(t, 0);
		tyg_call(p, sig, tyg_direct(tyg_symbol(m, "c_ldi", fl)), &a, 1, NULL, &res);
		tyg_ld_bin(p, TYG_FADD, tyg_at_local(r, 0), tyg_at_local(r, 0), tyg_at_local(t, ldal));
		tyg_ld_from(p, TYG_SITOF, tyg_at_local(u, 0), ld(p, TYG_I32, tyg_at_local(t, 0)));
		tyg_ld_bin(p, TYG_FADD, tyg_at_local(r, 0), tyg_at_local(r, 0), tyg_at_local(u, 0));
		// a call whose long double result is dropped
		tyg_call(p, tyg_sig_new(m, ldt, ps1, 1, 0), tyg_direct(tyg_symbol(m, "c_ld_count", fl)), &a, 1, NULL, NULL);
	}
	{
		// long double c_ld_vsum(int n, ...) with 6 triples (double, long double, int)
		const tyg_type *ps[1] = {T(TYG_I32)};
		tyg_sig *sig = tyg_sig_new(m, ldt, ps, 1, TYG_SIG_VARIADIC);
		static const double ds[6] = {1.5, -2.0, 0.5, 8.0, 0.75, 3.0};
		static const int ks[6] = {2, 3, -4, 5, 6, -7};
		tyg_addr ls[6];
		tyg_arg args[19];
		const tyg_type *vt[18];
		int n = 0;
		ls[0] = x; ls[1] = tyg_at_local(r, 0); ls[2] = x; ls[3] = tyg_at_local(s, 0); ls[4] = x; ls[5] = tyg_at_local(r, 0);
		args[n++] = val(tyg_iconst(p, TYG_I32, 6));
		for (int i = 0; i < 6; i++) {
			args[n] = val(kd(p, ds[i])); vt[n - 1] = T(TYG_F64); n++;
			args[n] = mem(ls[i]); vt[n - 1] = ldt; n++;
			args[n] = val(tyg_iconst(p, TYG_I32, ks[i])); vt[n - 1] = T(TYG_I32); n++;
		}
		tyg_addr res = tyg_at_local(u, 0);
		tyg_call(p, sig, tyg_direct(tyg_symbol(m, "c_ld_vsum", fl)), args, n, vt, &res);
		tyg_ld_bin(p, TYG_FADD, tyg_at_local(r, 0), tyg_at_local(r, 0), tyg_at_local(u, 0));
	}
	tyg_ret_mem(p, tyg_at_local(r, 0));
	check(tyg_proc_end(p));
}

// long double tg_ld_vsum(int n, ...): like c_ld_vsum, for n == 6
static void build_ld_vsum(void) {
	const tyg_type *ps[1] = {T(TYG_I32)};
	tyg_proc *p = begin("tg_ld_vsum", ldt, ps, 1, TYG_SIG_VARIADIC, NULL);
	int64_t vsize = tyg_va_list_size(m);
	tyg_local ap = tyg_local_new(p, vsize, tyg_va_list_align(m));
	tyg_val apv = tyg_addr_of(p, tyg_at_local(ap, 0));
	tyg_local s = ld_local(p), t = ld_local(p), u = ld_local(p);
	tyg_va_start(p, apv);
	tyg_ld_from(p, TYG_SITOF, tyg_at_local(s, 0), tyg_param(p, 0));
	for (int i = 0; i < 6; i++) {
		tyg_val d = ld(p, TYG_F64, tyg_at_val(tyg_va_arg(p, apv, T(TYG_F64)), 0));
		tyg_ld_from(p, TYG_FEXT, tyg_at_local(t, 0), d);
		tyg_ld_bin(p, TYG_FMUL, tyg_at_local(t, 0), tyg_at_local(t, 0), tyg_at_val(tyg_va_arg(p, apv, ldt), 0));
		tyg_val k = ld(p, TYG_I32, tyg_at_val(tyg_va_arg(p, apv, T(TYG_I32)), 0));
		tyg_ld_from(p, TYG_SITOF, tyg_at_local(u, 0), k);
		tyg_ld_bin(p, TYG_FADD, tyg_at_local(t, 0), tyg_at_local(t, 0), tyg_at_local(u, 0));
		tyg_ld_bin(p, TYG_FADD, tyg_at_local(s, 0), tyg_at_local(s, 0), tyg_at_local(t, 0));
	}
	tyg_ret_mem(p, tyg_at_local(s, 0));
	check(tyg_proc_end(p));
}

// long double tg_ld_g = 1.25; long double tg_ld_g_add(long double v) { return tg_ld_g += v; }
static void build_ld_global(void) {
	unsigned char b[16];
	tyg_sym g = tyg_symbol(m, "tg_ld_g", TYG_SYM_GLOBAL);
	ld_bytes(1.25, b);
	check(tyg_data(m, g, TYG_SECTION_DATA, b, ldsz, ldal));
	tyg_debug_global(m, "tg_ld_g", tyg_dt_base(m, "long double", ldsz, TYG_DT_FLOAT), g, file, 1);
	const tyg_type *ps[1] = {ldt};
	tyg_proc *p = begin("tg_ld_g_add", ldt, ps, 1, 0, NULL);
	tyg_ld_bin(p, TYG_FADD, tyg_at_sym(g, 0), tyg_at_sym(g, 0), tyg_at_val(tyg_param(p, 0), 0));
	tyg_ret_mem(p, tyg_at_sym(g, 0));
	check(tyg_proc_end(p));
}

static void build_ld(void) {
	make_ld_types();
	build_ld_ops();
	build_ld_cmp();
	build_ld_convs();
	build_ld_structs();
	build_ld_call_c();
	build_ld_vsum();
	build_ld_global();
}

// arm64 Linux has no long double yet: a procedure that uses it fails with a clear error
static void check_ld_unsupported(void) {
	const tyg_type *ps[1] = {T(TYG_F80)};
	tyg_proc *p = begin("tg_ld_unsupported", T(TYG_VOID), ps, 1, 0, NULL);
	tyg_ret(p, 0);
	if (tyg_proc_end(p) == 0 || strstr(tyg_error(m), "long double") == NULL) {
		fprintf(stderr, "long double did not fail: %s\n", tyg_error(m) ? tyg_error(m) : "no error");
		exit(1);
	}
}

int main(int argc, char **argv) {
	tyg_target t = {TYG_ARCH_X64, TYG_OS_LINUX, 1, 1};
	const char *target = argc > 2 ? argv[2] : "linux-x64";
	if (strcmp(target, "linux-x64") == 0) {
		t.arch = TYG_ARCH_X64; t.os = TYG_OS_LINUX;
	} else if (strcmp(target, "linux-arm64") == 0) {
		t.arch = TYG_ARCH_ARM64; t.os = TYG_OS_LINUX;
	} else if (strcmp(target, "macos-arm64") == 0) {
		t.arch = TYG_ARCH_ARM64; t.os = TYG_OS_DARWIN;
	} else if (strcmp(target, "macos-x64") == 0) {
		t.arch = TYG_ARCH_X64; t.os = TYG_OS_DARWIN;
	} else if (strcmp(target, "windows") == 0) {
		t.arch = TYG_ARCH_X64; t.os = TYG_OS_WINDOWS;
	} else if (strcmp(target, "c") == 0) {
		// C source for the host; every failing procedure is reported
		t = tyg_host_target();
		t.output = TYG_OUTPUT_C;
		c_failed = 0;
	} else {
		fprintf(stderr, "unknown target %s\n", target);
		return 1;
	}
	if (argc > 3 && strcmp(argv[3], "nopic") == 0) t.pic = 0;
	m = tyg_module_new(t, "abi.tg");
	if (!m) { fprintf(stderr, "no module\n"); return 1; }
	file = tyg_debug_file(m, "abi.tg");
	make_types();
	build_f3_scale();
	build_d4_rev();
	build_cs_sum();
	build_li_make();
	build_a16_sum();
	build_many();
	build_small();
	build_stk_b3();
	build_call_c();
	build_pk();
	build_call_vread();
	build_vread();
	build_snprintf();
	build_tls();
	build_misc();
	build_far();
	build_atomics();
	if (t.arch == TYG_ARCH_ARM64 && t.os == TYG_OS_LINUX) check_ld_unsupported();
	else build_ld();
	check(tyg_module_write(m, argc > 1 ? argv[1] : "abi.o"));
	tyg_module_free(m);
	return c_failed > 0;
}
