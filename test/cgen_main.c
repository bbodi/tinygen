// Calls what cgen.c builds and checks it against the C compiler's own arithmetic. Linked
// with the C output or with a native object, so both backends meet the same checks.
// Shift counts are masked like the IR's (tyg_bin masks a narrow count by the width - 1);
// division avoids zero and INT_MIN / -1, which stay undefined in C.
#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Step 1: data
extern const char cd_greeting[];
extern unsigned char cd_blob[24], cd_zeros[64];
extern __thread unsigned char cd_tls[32];
extern int64_t cd_weak;
extern void *const cd_ptrs[4];
unsigned char cd_ext[4];

// Step 2: procedure bodies
#define BINOPS(T, U, W) X(add, (T)(U)(ua + ub)) X(sub, (T)(U)(ua - ub)) X(mul, (T)(U)(ua * ub)) \
	X(sdiv, (T)(a / b)) X(udiv, (T)(ua / ub)) X(srem, (T)(a % b)) X(urem, (T)(ua % ub)) \
	X(and, (T)(a & b)) X(or, (T)(a | b)) X(xor, (T)(a ^ b)) \
	X(shl, (T)(U)(ua << (ub & W))) X(lshr, (T)(ua >> (ub & W))) X(ashr, (T)(a >> (ub & W)))
#define CONDS X(eq, a == b) X(ne, a != b) X(slt, a < b) X(sle, a <= b) X(sgt, a > b) X(sge, a >= b) \
	X(ult, ua < ub) X(ule, ua <= ub) X(ugt, ua > ub) X(uge, ua >= ub)
#define FCONDS X(feq, ==) X(fne, !=) X(flt, <) X(fle, <=) X(fgt, >) X(fge, >=)
#define X(n, e) int8_t tg_b8_##n(int8_t, int8_t); int64_t tg_b64_##n(int64_t, int64_t);
BINOPS(, , )
#undef X
#define X(n, e) int8_t tg_c8_##n(int8_t, int8_t); int8_t tg_c32_##n(int32_t, int32_t);
CONDS
#undef X
#define X(n, op) int8_t tg_cf_##n(double, double);
FCONDS
#undef X
double tg_f_add(double, double); double tg_f_sub(double, double); float tg_f_mul(float, float); float tg_f_div(float, float);
int8_t tg_u8_neg(int8_t); int8_t tg_u8_not(int8_t); int64_t tg_u64_neg(int64_t); int64_t tg_u64_not(int64_t); double tg_uf_neg(double);
int64_t tg_sext_8_64(int8_t); int32_t tg_zext_16_32(int16_t); int8_t tg_trunc_64_8(int64_t); float tg_sitof_8_32(int8_t);
double tg_uitof_64_64(int64_t); float tg_uitof_32_32(int32_t); int8_t tg_ftosi_32_8(float); int64_t tg_ftosi_64_64(double);
int64_t tg_ftoui_64_64(double); int32_t tg_ftoui_64_32(double); double tg_fext(float); float tg_ftrunc(double);
int64_t tg_bitcast_f64_i64(double); float tg_bitcast_i32_f32(int32_t); int64_t tg_select(int32_t, int64_t, int64_t); double tg_fconsts(int32_t);
extern int32_t tg_counter;
int32_t tg_volatile(int32_t *, int32_t); int64_t tg_mem(const int64_t *); void tg_fill(char *, const char *, int32_t, int64_t);
int32_t tg_trap_if(int32_t); void tg_store_void(int64_t *, int64_t); char *tg_ptr_add(char *, int64_t);
struct R { int32_t a; int64_t b; };
int64_t tg_rec_sum(struct R, int32_t);

// Step 3: calls; cc_ext, cc_note and cc_jump are called by the generated code
struct P { int a, b; };
struct M { long long a; double b; };
struct B { long long a, b, c; };
int cc_call_static(int); int cc_call_ind(int); long long cc_two_sigs(long long); int cc_fmt(char *, int);
double cc_vmix(int, ...); double cc_call_vmix(void); struct M cc_mk_mix(long long, double); struct B cc_mk_big(long long);
long long cc_sum_big(struct B, struct M); long long cc_use_structs(long long); long long cc_sj(long long);

// Step 4: atomics, intrinsics, alloca, long double
#define WIDTHS X(8) X(16) X(32) X(64)
#define RMWS(N) R(xchg, N, v) R(add, N, o + v) R(sub, N, o - v) R(and, N, o & v) R(or, N, o | v) R(xor, N, o ^ v) R(nand, N, ~(o & v))
#define R(op, N, e) uint##N##_t tg_rmw_##op##_##N(uint##N##_t *, uint##N##_t);
#define X(N) RMWS(N) uint##N##_t tg_cas_##N(uint##N##_t *, uint##N##_t, uint##N##_t, int32_t *); \
	uint##N##_t tg_bswap_##N(uint##N##_t); uint##N##_t tg_popcount_##N(uint##N##_t); \
	uint##N##_t tg_ctz_##N(uint##N##_t); uint##N##_t tg_clz_##N(uint##N##_t);
WIDTHS
#undef X
#undef R
int32_t tg_aload_32(int32_t *); int64_t tg_aload_64(int64_t *); double tg_aload_f64(double *);
void tg_astore_32(int32_t *, int32_t); void tg_astore_64(int64_t *, int64_t); void tg_astore_f64(double *, double);
void tg_fence_pair(int64_t *, int64_t, int64_t); float tg_sqrt_f32(float); double tg_sqrt_f64(double);
void *tg_frame_addr(void); void *tg_ret_addr(void); int64_t tg_alloca_loop(int64_t, int32_t, void **);
#if !(defined(__aarch64__) && defined(__linux__)) // no native long double on arm64 Linux yet
#define HAVE_LD 1
long double tg_ld_add(long double, long double); long double tg_ld_sub(long double, long double);
long double tg_ld_mul(long double, long double); long double tg_ld_div(long double, long double);
long double tg_ld_neg(long double); int32_t tg_ld_cmp(long double, long double); long double tg_ld_call(long double);
long double tg_ld_from_i32(int32_t); long double tg_ld_from_i64(int64_t); long double tg_ld_from_u64(uint64_t);
long double tg_ld_from_f32(float); long double tg_ld_from_f64(double);
int32_t tg_ld_to_i32(long double); int64_t tg_ld_to_i64(long double); uint64_t tg_ld_to_u64(long double);
float tg_ld_to_f32(long double); double tg_ld_to_f64(long double);
#endif

long long cc_ext(long long a, long long b) { return a * 100 + b; }
static long long noted;
void cc_note(long long a) { noted = a; }
void cc_jump(jmp_buf b, int v) { longjmp(b, v); }

static int checks, failures;
#define EQ(got, want) do { long long g_ = (long long)(got), w_ = (long long)(want); checks++; \
	if (g_ != w_) { failures++; printf("line %d: %s = %lld, want %lld\n", __LINE__, #got, g_, w_); } } while (0)
// bitwise, so -0.0 and the NaNs count
#define FEQ(got, want) do { double g_ = (got), w_ = (want); checks++; \
	if (memcmp(&g_, &w_, 8) != 0) { failures++; printf("line %d: %s = %a, want %a\n", __LINE__, #got, g_, w_); } } while (0)
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("line %d: %s\n", __LINE__, #c); } } while (0)

static void check_data(void) {
	void *p0, *p2;
	memcpy(&p0, cd_blob, 8);
	memcpy(&p2, cd_blob + 16, 8);
	CHECK(p0 == (void *)&puts);
	CHECK(p2 == (void *)(cd_greeting + 3));
	CHECK(cd_blob[8] == 0xa8 && cd_blob[15] == 0xaf);
	CHECK(strcmp(cd_greeting, "hello, tinygen") == 0);
	CHECK((uintptr_t)cd_blob % 8 == 0);
	CHECK((uintptr_t)cd_zeros % 16 == 0 && cd_zeros[0] == 0 && cd_zeros[63] == 0);
	CHECK((uintptr_t)cd_tls % 8 == 0 && cd_tls[31] == 0);
	cd_tls[31] = 9;
	CHECK(cd_tls[31] == 9);
	CHECK(cd_weak == 7);
	CHECK(*(const int *)cd_ptrs[0] == 42);
	CHECK(((const unsigned char *)cd_ptrs[1])[-1] == 1 && ((const unsigned char *)cd_ptrs[1])[1] == 3);
	int32_t (*dot)(void);
	memcpy(&dot, &cd_ptrs[2], sizeof(dot));
	EQ(dot(), -5);
	CHECK(cd_ptrs[3] == (void *)cd_ext);
}

static void check_ops(void) {
	static const int8_t v8[][2] = {{100, 100}, {-7, 2}, {7, -2}, {-100, 7}, {-128, 3}, {5, 9}, {-1, 15}, {0x40, 1}, {77, 8}, {-3, -128}};
	static const int64_t v64[][2] = {{INT64_MAX, 1}, {-7, 2}, {7, -2}, {INT64_MIN, 3}, {123456789012345, 64}, {-1, 65}, {-5, 127}, {0x4000000000000000, 63}};
	for (size_t i = 0; i < sizeof(v8) / sizeof(v8[0]); i++) {
		int8_t a = v8[i][0], b = v8[i][1];
		uint8_t ua = (uint8_t)a, ub = (uint8_t)b;
#define X(n, e) EQ(tg_b8_##n(a, b), e);
		BINOPS(int8_t, uint8_t, 7)
#undef X
#define X(n, e) EQ(tg_c8_##n(a, b), e);
		CONDS
#undef X
		EQ(tg_u8_neg(a), (int8_t)(uint8_t)(0u - ua));
		EQ(tg_u8_not(a), (int8_t)~a);
	}
	for (size_t i = 0; i < sizeof(v8) / sizeof(v8[0]); i++) {
		int32_t a = v8[i][0] * 1000003, b = v8[i][1];
		uint32_t ua = (uint32_t)a, ub = (uint32_t)b;
#define X(n, e) EQ(tg_c32_##n(a, b), e);
		CONDS
#undef X
	}
	for (size_t i = 0; i < sizeof(v64) / sizeof(v64[0]); i++) {
		int64_t a = v64[i][0], b = v64[i][1];
		uint64_t ua = (uint64_t)a, ub = (uint64_t)b;
#define X(n, e) EQ(tg_b64_##n(a, b), e);
		BINOPS(int64_t, uint64_t, 63)
#undef X
		EQ(tg_u64_neg(a), (int64_t)(0u - ua));
		EQ(tg_u64_not(a), ~a);
	}
	static const double fv[][2] = {{1.5, 2.5}, {-0.0, 0.0}, {3, 3}, {-1e300, 1e300}, {NAN, 1}};
	for (size_t i = 0; i < sizeof(fv) / sizeof(fv[0]); i++) {
		double a = fv[i][0], b = fv[i][1];
#define X(n, op) EQ(tg_cf_##n(a, b), a op b); EQ(tg_cf_##n(b, a), b op a);
		FCONDS
#undef X
		FEQ(tg_f_add(a, b), a + b);
		FEQ(tg_f_sub(a, b), a - b);
		FEQ(tg_uf_neg(a), -a);
	}
	FEQ(tg_f_mul(1.5f, 3.25f), (double)(1.5f * 3.25f));
	FEQ(tg_f_div(1.0f, 3.0f), (double)(1.0f / 3.0f));
}

static void check_conversions(void) {
	EQ(tg_sext_8_64(-5), -5);
	EQ(tg_zext_16_32(-30000), 35536);
	EQ(tg_trunc_64_8(0x1234567890abcdefLL), (int8_t)0xef);
	FEQ(tg_sitof_8_32(-128), -128.0);
	FEQ(tg_uitof_64_64(-1), 18446744073709551615.0);
	FEQ(tg_uitof_32_32(-1), (double)4294967295.0f);
	EQ(tg_ftosi_32_8(-100.7f), -100);
	EQ(tg_ftosi_64_64(-9e15), -9000000000000000LL);
	EQ(tg_ftoui_64_64(1.5e19), (int64_t)15000000000000000000ULL);
	EQ(tg_ftoui_64_32(3e9), (int32_t)3000000000u);
	FEQ(tg_fext(0.1f), (double)0.1f);
	FEQ(tg_ftrunc(0.1), (double)0.1f);
	EQ(tg_bitcast_f64_i64(1.0), 0x3ff0000000000000LL);
	FEQ(tg_bitcast_i32_f32(0x40490fdb), (double)3.14159274101257324f);
	EQ(tg_select(1, 10, 20), 10);
	EQ(tg_select(0, 10, 20), 20);
	EQ(tg_select(-256, 10, 20), 10);
	FEQ(tg_fconsts(0), 3.14159);
	FEQ(tg_fconsts(1), -0.0);
	FEQ(tg_fconsts(2), (double)INFINITY);
	CHECK(isnan(tg_fconsts(3)));
}

static void check_memory(void) {
	int32_t q[2] = {0, 0};
	EQ(tg_volatile(q, 41), 42);
	EQ(q[1], 42);
	EQ(tg_volatile(q, 9), 52);
	EQ(tg_counter, 52);
	int64_t src[4] = {1000, 200, 30, 4};
	EQ(tg_mem(src), 1000 + 0 + 0 + 30);
	char dst[16], from[8] = "abcdefg";
	memset(dst, '.', sizeof(dst));
	tg_fill(dst, from, 'z', 4);
	EQ(memcmp(dst, "abcdzzzz........", 16), 0);
	EQ(tg_trap_if(21), 42);
	EQ(tg_trap_if(-12344), -24688);
	int64_t cell = 0;
	tg_store_void(&cell, -77);
	EQ(cell, -77);
	EQ(tg_ptr_add(dst, 5) - dst, 5);
	struct R r = {-3, 1000000000000LL};
	EQ(tg_rec_sum(r, 7), -3 + 7000000000000LL);
}

static void check_calls(void) {
	char buf[64];
	EQ(cc_call_static(20), 41);
	EQ(cc_call_ind(-4), -5);
	EQ(cc_two_sigs(3), 300 + 5 + 300 + 7);
	CHECK(cc_fmt(buf, 30) == 16 && strcmp(buf, "30 -5 2.50 60:ok") == 0);
	CHECK(cc_fmt(buf, 4) == 14 && strcmp(buf, "4 -") == 0);
	struct P p1 = {1, 2}, p2 = {5, 6};
	FEQ(cc_vmix(1, (signed char)-3, 2.5f, p1, 10LL), -3 - 3 + 2.5 + 3 + 10);
	FEQ(cc_vmix(2, (signed char)-3, 2.5f, p1, 10LL, (signed char)4, 0.25f, p2, -20LL), -3 - 3 + 2.5 + 3 + 10 + 4 + 0.25 + 11 - 20);
	FEQ(cc_vmix(0, (signed char)9), 9);
	FEQ(cc_call_vmix(), 2 * (-3 - 3 + 2.5 + 3 + 10 + 4 + 0.25 + 11 - 20));
	struct M mm = cc_mk_mix(-7, 1.25);
	CHECK(mm.a == -7 && mm.b == 1.25);
	struct B bb = cc_mk_big(40);
	CHECK(bb.a == 40 && bb.b == 41 && bb.c == 42);
	EQ(cc_sum_big(bb, mm), 40 + 41 + 42 - 7 + 1);
	EQ(cc_use_structs(100), 100 + 101 + 102 + 100 + 0);
	EQ(noted, 100);
	EQ(cc_sj(5), 15 + 7 + 6);
	EQ(cc_sj(-2), -6 + 7 - 1);
}

// what the lowerings give: the count of a zero is the width
static unsigned ref_popcount(uint64_t x) { unsigned n = 0; for (; x; x &= x - 1) n++; return n; }
static unsigned ref_ctz(uint64_t x, unsigned bits) { unsigned n = 0; while (n < bits && !(x >> n & 1)) n++; return n; }
static unsigned ref_clz(uint64_t x, unsigned bits) { unsigned n = 0; while (n < bits && !(x >> (bits - 1 - n) & 1)) n++; return n; }
static uint64_t ref_bswap(uint64_t x, unsigned bits) { uint64_t r = 0; for (unsigned i = 0; i < bits; i += 8) r = r << 8 | (x >> i & 0xff); return r; }

static void check_atomics(void) {
	static const uint64_t vs[][2] = {{0, 0}, {5, 3}, {0xf0f0f0f0f0f0f0f0ull, 0x0ff00ff00ff00ff0ull}, {~0ull, 1}, {0x8000000000000001ull, 0x7fffffffffffffffull}};
	for (size_t i = 0; i < sizeof(vs) / sizeof(vs[0]); i++) {
#define R(op, N, e) { uint##N##_t c = (uint##N##_t)vs[i][0], o = c, v = (uint##N##_t)vs[i][1]; \
		EQ(tg_rmw_##op##_##N(&c, v), o); EQ(c, (uint##N##_t)(e)); }
#define X(N) RMWS(N) { uint##N##_t c = (uint##N##_t)vs[i][0], v = (uint##N##_t)vs[i][1]; int32_t ok = 7; \
		EQ(tg_cas_##N(&c, (uint##N##_t)(c + 1), v, &ok), (uint##N##_t)vs[i][0]); EQ(ok, 0); EQ(c, (uint##N##_t)vs[i][0]); \
		EQ(tg_cas_##N(&c, c, v, &ok), (uint##N##_t)vs[i][0]); EQ(ok, 1); EQ(c, v); }
		WIDTHS
#undef X
#undef R
	}
	int32_t a32 = -9;
	int64_t a64 = INT64_MIN + 3, pair[2] = {0, 0};
	double af = -2.75;
	EQ(tg_aload_32(&a32), -9);
	EQ(tg_aload_64(&a64), INT64_MIN + 3);
	FEQ(tg_aload_f64(&af), -2.75);
	tg_astore_32(&a32, 123456);
	tg_astore_64(&a64, -1);
	tg_astore_f64(&af, 1e-300);
	CHECK(a32 == 123456 && a64 == -1 && af == 1e-300);
	tg_fence_pair(pair, 11, -22);
	CHECK(pair[0] == 11 && pair[1] == -22);
}

static void check_intrinsics(void) {
	static const uint64_t vs[] = {0, 1, 0x80, 0x8000, 0x80000000u, 0x8000000000000000ull, 0x0123456789abcdefull, 0x10, 0xfff0, ~0ull};
	for (size_t i = 0; i < sizeof(vs) / sizeof(vs[0]); i++) {
#define X(N) { uint##N##_t x = (uint##N##_t)vs[i]; EQ(tg_bswap_##N(x), (uint##N##_t)ref_bswap(x, N)); \
		EQ(tg_popcount_##N(x), ref_popcount(x)); EQ(tg_ctz_##N(x), ref_ctz(x, N)); EQ(tg_clz_##N(x), ref_clz(x, N)); }
		WIDTHS
#undef X
	}
	FEQ(tg_sqrt_f32(2.0f), (double)sqrtf(2.0f));
	FEQ(tg_sqrt_f64(2.0), sqrt(2.0));
	FEQ(tg_sqrt_f64(0.0), 0.0);
	CHECK(isnan(tg_sqrt_f64(-1.0)));
	CHECK(tg_frame_addr() != NULL);
	CHECK(tg_ret_addr() != NULL);
	void *ptrs[6];
	EQ(tg_alloca_loop(100, 6, ptrs), 2 * (1 + 2 + 3 + 4 + 5 + 6));
	EQ(tg_alloca_loop(3, 1, ptrs), 2);
	EQ(tg_alloca_loop(4000, 6, ptrs), 2 * 21);
	for (int i = 0; i < 6; i++) {
		CHECK((uintptr_t)ptrs[i] % 64 == 0);
		for (int j = 0; j < i; j++) CHECK(ptrs[i] != ptrs[j]);
	}
}

#ifdef HAVE_LD
// bit i is condition i of ==, !=, <, <=, >, >=
static int ld_conds(long double a, long double b) {
	return (a == b) | (a != b) << 1 | (a < b) << 2 | (a <= b) << 3 | (a > b) << 4 | (a >= b) << 5;
}

static void check_long_double(void) {
	long double third = 1.0L / 3, big = 12345678901234567.0L, nan = (long double)NAN;
	static const long double vs[] = {1.5L, -0.25L, 7.0L, 1e300L};
	CHECK(tg_ld_add(third, big) == third + big);
	CHECK(tg_ld_sub(third, big) == third - big);
	CHECK(tg_ld_mul(third, big) == third * big);
	CHECK(tg_ld_div(big, third) == big / third);
	CHECK(tg_ld_neg(third) == -third);
	CHECK(isnan((double)tg_ld_add(nan, third)));
	CHECK(tg_ld_call(third) == -1.0L);
	for (size_t i = 0; i < 4; i++) {
		EQ(tg_ld_cmp(vs[i], third), ld_conds(vs[i], third));
		EQ(tg_ld_cmp(third, vs[i]), ld_conds(third, vs[i]));
	}
	EQ(tg_ld_cmp(third, third), ld_conds(third, third));
	EQ(tg_ld_cmp(nan, third), 2);
	EQ(tg_ld_cmp(third, nan), 2);
	EQ(tg_ld_cmp(nan, nan), 2);
	CHECK(tg_ld_from_i32(INT32_MIN) == (long double)INT32_MIN);
	CHECK(tg_ld_from_i64(-1234567890123456789LL) == (long double)-1234567890123456789LL);
	CHECK(tg_ld_from_u64(18446744073709551615ull) == (long double)18446744073709551615ull);
	CHECK(tg_ld_from_f32(0.1f) == (long double)0.1f);
	CHECK(tg_ld_from_f64(0.1) == (long double)0.1);
	EQ(tg_ld_to_i32(-2147483.75L), -2147483);
	EQ(tg_ld_to_i64(-9e17L - 0.5L), (int64_t)(-9e17L - 0.5L));
	EQ(tg_ld_to_u64(1.8e19L), (int64_t)(uint64_t)1.8e19L);
	FEQ(tg_ld_to_f32(third), (double)(float)third);
	FEQ(tg_ld_to_f64(third), (double)third);
}
#endif

// Step 6: debug names and types
struct DbgPoint { int32_t x, y; };
int64_t dbg_scalars(int64_t a, int32_t b); int64_t dbg_point(struct DbgPoint pt); int64_t dbg_node(int64_t x);

static void check_debug(void) {
	struct DbgPoint pt = {40, -2};
	EQ(dbg_scalars(10, 3), 10 + 3 + 6 + 7 + 200 + 3 + 1 - 3 + 100);
	EQ(dbg_scalars(-50, -4), -50 - 4 - 8 + 7 + 200 + 3 + 1 - 3 + 100);
	EQ(dbg_point(pt), 42);
	EQ(dbg_node(10), 10 + 1 + 2 + 3 * 10 + 6 + 2 + 5 + 3 + 0xab + 20 + 1000);
	EQ(dbg_node(-7), -7 + 1 + 2 - 3 * 7 + 6 + 2 + 5 + 3 + 0xab - 14 + 1000);
}

// Step 7: what the header text of cgen.c declares. host_inl is the header's static inline in
// the C output, and this definition for the object, which has no header.
int64_t host_counter = 5;
int64_t host_twice(int64_t x) { return 2 * x; }
double host_mix(void *p, double d) { return *(double *)p * 10 + d; }
int64_t host_inl(int64_t x) { return x * 3; }
int64_t hd_twice_inc(int64_t); double hd_mix(void *, double); int64_t hd_counter_add(int64_t);
void *hd_counter_addr(void); int64_t hd_inline(int64_t);

static void check_host(void) {
	double d = 1.5;
	EQ(hd_twice_inc(20), 41);
	FEQ(hd_mix(&d, 0.25), 15.25);
	EQ(hd_counter_add(3), 8);
	EQ(hd_counter_add(-10), -2);
	CHECK(hd_counter_addr() == (void *)&host_counter && host_counter == -2);
	EQ(hd_inline(7), 21 + 14);
}

int main(void) {
	check_data();
	check_ops();
	check_conversions();
	check_memory();
	check_calls();
	check_atomics();
	check_intrinsics();
	check_debug();
	check_host();
#ifdef HAVE_LD
	check_long_double();
#endif
	printf("%d checks, %d failures\n", checks, failures);
	return failures != 0;
}
