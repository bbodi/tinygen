// A synthetic front end for timing tinygen: many small procedures with loops, locals,
// calls, float math, aggregate copies, scopes and debug info, plus many data symbols.
//
//     cc -std=c99 -O2 -Iinclude test/bench.c src/tinygen.c -o build/bench
//     build/bench build/bench.o macos-arm64 40000 50000
//
// It prints the time of each phase. The object is deterministic, so it also serves to
// check that a change leaves the output byte-identical.
#define _POSIX_C_SOURCE 200112L // clock_gettime under -std=c99 on glibc
#include "tinygen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

static double now_ms(void) {
#ifdef _WIN32
	LARGE_INTEGER c, f;
	QueryPerformanceCounter(&c);
	QueryPerformanceFrequency(&f);
	return (double)c.QuadPart*1e3/(double)f.QuadPart;
#else
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec*1e3 + t.tv_nsec/1e6;
#endif
}

static tyg_module *m;

static int c_failed = -1; // the "c" target goes on after a failure and counts it

static void check(int r) {
	if (r) {
		fprintf(stderr, "bench: %s\n", tyg_error(m));
		if (c_failed < 0) exit(1);
		c_failed++;
	}
}

int main(int argc, char **argv) {
	char const *out = argc > 1 ? argv[1] : "bench.o";
	char const *target = argc > 2 ? argv[2] : "linux-x64";
	int nproc = argc > 3 ? atoi(argv[3]) : 20000;
	int ndata = argc > 4 ? atoi(argv[4]) : 20000;
	tyg_target t = {TYG_ARCH_X64, TYG_OS_LINUX, 1, 1};
	if (strcmp(target, "linux-x64") == 0) {
	} else if (strcmp(target, "linux-arm64") == 0) {
		t.arch = TYG_ARCH_ARM64;
	} else if (strcmp(target, "macos-arm64") == 0) {
		t.arch = TYG_ARCH_ARM64;
		t.os = TYG_OS_DARWIN;
	} else if (strcmp(target, "macos-x64") == 0) {
		t.os = TYG_OS_DARWIN;
	} else if (strcmp(target, "windows") == 0) {
		t.os = TYG_OS_WINDOWS;
	} else if (strcmp(target, "c") == 0) {
		// C source for the host; every failing procedure is reported
		t = tyg_host_target();
		t.output = TYG_OUTPUT_C;
		c_failed = 0;
	} else {
		fprintf(stderr, "usage: bench out.o [linux-x64|linux-arm64|macos-arm64|macos-x64|windows|c] [procs] [data symbols]\n");
		return 2;
	}
	if (nproc < 1) nproc = 1;

	double t0 = now_ms();
	m = tyg_module_new(t, "bench.tg");
	tyg_type const *i64t = tyg_type_scalar(m, TYG_I64);
	tyg_type const *f64t = tyg_type_scalar(m, TYG_F64);
	tyg_field pair_fields[3] = {{i64t, 0}, {i64t, 8}, {f64t, 16}};
	tyg_type const *pair = tyg_type_record(m, 24, 8, pair_fields, 3);
	tyg_type const *params[3] = {i64t, i64t, pair};
	tyg_sig *sig = tyg_sig_new(m, i64t, params, 3, 0);
	tyg_file file = tyg_debug_file(m, "bench.tg");
	tyg_dtype const *ll = tyg_dt_base(m, "long", 8, TYG_DT_SIGNED);
	tyg_dtype const *dbl = tyg_dt_base(m, "double", 8, TYG_DT_FLOAT);
	tyg_dtype *pair_dt = tyg_dt_record(m, TYG_DT_STRUCT, "Pair", file, 1);
	tyg_dt_member(m, pair_dt, "a", ll, 0);
	tyg_dt_member(m, pair_dt, "b", ll, 8);
	tyg_dt_member(m, pair_dt, "f", dbl, 16);
	tyg_dt_record_complete(m, pair_dt, 24, 8);
	tyg_dtype const *dparams[3] = {ll, ll, pair_dt};
	tyg_dtype const *ftype = tyg_dt_func(m, ll, dparams, 3, 0);

	char name[128];
	tyg_sym *syms = (tyg_sym *)malloc(sizeof(tyg_sym) * (size_t)nproc);
	for (int i = 0; i < nproc; i++) {
		snprintf(name, sizeof(name), "package_module_some_long_procedure_name_%d", i);
		syms[i] = tyg_symbol(m, name, TYG_SYM_FUNC | TYG_SYM_GLOBAL);
	}
	tyg_sym ext = tyg_symbol(m, "external_counter", TYG_SYM_GLOBAL | TYG_SYM_FOREIGN);
	for (int i = 0; i < ndata; i++) {
		snprintf(name, sizeof(name), "string_literal_constant_number_%d_with_padding", i);
		tyg_sym s = tyg_symbol(m, name, 0);
		check(tyg_data(m, s, TYG_SECTION_RODATA, name, 32, 1));
	}
	double t1 = now_ms();

	for (int i = 0; i < nproc; i++) {
		tyg_proc *p = tyg_proc_begin(m, syms[i], sig);
		int line = 1 + i*20;
		tyg_debug_proc(p, "proc", file, line, ftype);
		tyg_loc(p, file, line, 1);
		tyg_local s = tyg_local_new(p, 8, 8);
		tyg_local k = tyg_local_new(p, 8, 8);
		tyg_local f = tyg_local_new(p, 8, 8);
		tyg_local cp = tyg_local_new(p, 24, 8);
		tyg_debug_param(p, "n", ll, tyg_at_val(tyg_param(p, 0), 0), file, line);
		tyg_debug_param(p, "x", ll, tyg_at_val(tyg_param(p, 1), 0), file, line);
		tyg_debug_param(p, "pr", pair_dt, tyg_at_val(tyg_param(p, 2), 0), file, line);
		tyg_store(p, TYG_I64, tyg_at_local(s, 0), tyg_iconst(p, TYG_I64, 0), 0);
		tyg_store(p, TYG_I64, tyg_at_local(k, 0), tyg_iconst(p, TYG_I64, 0), 0);
		tyg_store(p, TYG_F64, tyg_at_local(f, 0), tyg_load(p, TYG_F64, tyg_at_val(tyg_param(p, 2), 16), 0), 0);
		tyg_copy(p, tyg_at_local(cp, 0), tyg_at_val(tyg_param(p, 2), 0), 24);
		tyg_debug_var(p, "s", ll, tyg_at_local(s, 0), file, line + 1);
		tyg_debug_var(p, "k", ll, tyg_at_local(k, 0), file, line + 1);
		tyg_debug_var(p, "f", dbl, tyg_at_local(f, 0), file, line + 1);
		tyg_debug_var(p, "cp", pair_dt, tyg_at_local(cp, 0), file, line + 1);

		tyg_block head = tyg_block_new(p), body = tyg_block_new(p), odd = tyg_block_new(p);
		tyg_block next = tyg_block_new(p), done = tyg_block_new(p);
		tyg_jump(p, head);
		tyg_block_set(p, head);
		tyg_loc(p, file, line + 2, 1);
		tyg_val kv = tyg_load(p, TYG_I64, tyg_at_local(k, 0), 0);
		tyg_branch(p, tyg_cmp(p, TYG_SLT, kv, tyg_param(p, 0)), body, done);

		tyg_block_set(p, body);
		tyg_scope_begin(p);
		tyg_local tmp = tyg_local_new(p, 8, 8);
		tyg_debug_var(p, "tmp", ll, tyg_at_local(tmp, 0), file, line + 3);
		for (int j = 0; j < 3; j++) {
			tyg_loc(p, file, line + 3 + j, 5);
			tyg_val sv = tyg_load(p, TYG_I64, tyg_at_local(s, 0), 0);
			tyg_val kk = tyg_load(p, TYG_I64, tyg_at_local(k, 0), 0);
			tyg_arg args[3];
			memset(args, 0, sizeof(args));
			args[0].val = tyg_bin(p, TYG_ADD, sv, kk);
			args[1].val = tyg_param(p, 1);
			args[2].mem = tyg_at_local(cp, 0);
			tyg_val r = tyg_call(p, sig, tyg_direct(syms[(i*7 + j*13 + 1) % nproc]), args, 3, NULL, NULL);
			tyg_store(p, TYG_I64, tyg_at_local(tmp, 0), r, 0);
			tyg_val mixed = tyg_bin(p, TYG_XOR, r, tyg_bin(p, TYG_MUL, sv, tyg_iconst(p, TYG_I64, j + 3)));
			tyg_store(p, TYG_I64, tyg_at_local(s, 0), mixed, 0);
		}
		tyg_val fv = tyg_load(p, TYG_F64, tyg_at_local(f, 0), 0);
		fv = tyg_bin(p, TYG_FADD, tyg_bin(p, TYG_FMUL, fv, tyg_fconst(p, TYG_F64, 0.5)), tyg_convert(p, TYG_SITOF, TYG_F64, kv));
		tyg_store(p, TYG_F64, tyg_at_local(f, 0), fv, 0);
		tyg_val c = tyg_load(p, TYG_I64, tyg_at_sym(ext, 0), 0);
		tyg_store(p, TYG_I64, tyg_at_sym(ext, 0), tyg_bin(p, TYG_ADD, c, tyg_iconst(p, TYG_I64, 1)), 0);
		tyg_scope_end(p);
		tyg_val bit = tyg_bin(p, TYG_AND, kv, tyg_iconst(p, TYG_I64, 1));
		tyg_branch(p, tyg_cmp(p, TYG_NE, bit, tyg_iconst(p, TYG_I64, 0)), odd, next);

		tyg_block_set(p, odd);
		tyg_loc(p, file, line + 8, 5);
		tyg_store(p, TYG_I64, tyg_at_local(s, 0), tyg_bin(p, TYG_SHL, tyg_load(p, TYG_I64, tyg_at_local(s, 0), 0), tyg_iconst(p, TYG_I64, 1)), 0);
		tyg_jump(p, next);

		tyg_block_set(p, next);
		tyg_loc(p, file, line + 9, 5);
		tyg_store(p, TYG_I64, tyg_at_local(k, 0), tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I64, tyg_at_local(k, 0), 0), tyg_iconst(p, TYG_I64, 1)), 0);
		tyg_jump(p, head);

		tyg_block_set(p, done);
		tyg_loc(p, file, line + 10, 1);
		tyg_val res = tyg_bin(p, TYG_ADD, tyg_load(p, TYG_I64, tyg_at_local(s, 0), 0), tyg_convert(p, TYG_FTOSI, TYG_I64, tyg_load(p, TYG_F64, tyg_at_local(f, 0), 0)));
		tyg_ret(p, res);
		check(tyg_proc_end(p));
	}
	double t2 = now_ms();
	check(tyg_module_write(m, out));
	double t3 = now_ms();
	tyg_module_free(m);
	free(syms);
	printf("%-12s procs %6d data %6d | symbols %7.1f ms | procedures %8.1f ms (%5.2f us each) | write %7.1f ms | total %8.1f ms\n",
	       target, nproc, ndata, t1 - t0, t2 - t1, (t2 - t1)*1e3/nproc, t3 - t2, t3 - t0);
	return c_failed > 0;
}
