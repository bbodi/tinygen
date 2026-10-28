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

int main(void) {
	check_data();
	check_ops();
	check_conversions();
	check_memory();
	check_calls();
	printf("%d checks, %d failures\n", checks, failures);
	return failures != 0;
}
