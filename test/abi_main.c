// The C half of test/abi.c: calls the functions tinygen built, provides the ones they call,
// and checks every result against the same computation in C.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

struct F3 { float x, y, z; };
struct D4 { double a[4]; };
struct CS { signed char c; short s; };
struct LI { long long a; int b; };
struct A16 { _Alignas(16) long long a; long long b; };
struct L3 { long long a, b, c; };

struct F3 tg_f3_scale(struct F3 v, float k);
struct D4 tg_d4_rev(struct D4 v);
int tg_cs_sum(struct CS v);
struct LI tg_li_make(long long a, int b);
long long tg_a16_sum(int x, struct A16 v);
double tg_many(int i0, int i1, int i2, int i3, int i4, int i5, int i6, int i7, int i8,
               double d0, double d1, double d2, double d3, double d4, double d5, double d6, double d7, double d8,
               struct F3 h, signed char c, unsigned short s, struct A16 a16, struct LI li);
int tg_small(signed char a, unsigned short b, short c);
signed char tg_small_ret(int x);
// The packed records of abi.c's build_pk
#pragma pack(push, 1)
struct PA { unsigned char a; float b; };
struct PB { float a; unsigned char b; };
struct PC { int a; double b; };
struct PD { unsigned char t; struct PDV { float x, y; } v; };
struct PE { long long a; unsigned char b; };
struct PF { unsigned char a; int b[2]; };
struct PG { short a; unsigned char b, c; float d; double e; };
#pragma pack(pop)
struct PA tg_pk_echo0(struct PA v);
struct PB tg_pk_echo1(struct PB v);
struct PC tg_pk_echo2(struct PC v);
struct PD tg_pk_echo3(struct PD v);
struct PE tg_pk_echo4(struct PE v);
struct PF tg_pk_echo5(struct PF v);
struct PG tg_pk_echo6(struct PG v);
long long tg_pk_sum(long long k0, long long k1, long long k2, long long k3, long long k4, struct PA a,
                    struct PB b, struct PC c, struct PD d, struct PE e, struct PF f, struct PG g, long long last);
long long tg_pk_call(void);
double tg_call_c(void);
double tg_call_vread(void);
double tg_vread(int n, ...);
int tg_snprintf(char *buf);
int tg_tls_bump(void);
long long tg_tls_get_big(void);
// GCC's MinGW port has only emulated thread locals (__emutls), which cannot reach the native
// Windows ones of tinygen (and MSVC and clang): there they are checked through tinygen's functions
#if defined(__MINGW32__) && !defined(__clang__)
#define TLS_DIRECT 0
#else
#define TLS_DIRECT 1
extern _Thread_local int tg_tls_counter;
extern _Thread_local long long tg_tls_big;
#endif
long long tg_misc(long long x);
long long tg_far(long long x);

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

////////////////////////////////////////////////////////////////
// long double: x87 on x86-64 Linux and macOS, a double on arm64 macOS and Windows, not on arm64
// Linux yet
////////////////////////////////////////////////////////////////

#include <float.h>
#if defined(_WIN32) && LDBL_MANT_DIG != 53
// MinGW's gcc has the x87 type, tinygen MSVC's
#error "long double is a double on Windows, as with MSVC: build with gcc -mlong-double-64"
#endif
#if !(defined(__aarch64__) && !defined(__APPLE__))
#define HAVE_LD 1
#include <limits.h>

struct LDS { long double x; };
struct LDI { int i; long double x; };

void tg_ld_ops(long double a, long double b, long double *out);
int tg_ld_cmp(long double a, long double b);
long long tg_ld_to_ll(long double x);
unsigned long long tg_ld_to_ull(long double x);
int tg_ld_to_i(long double x);
unsigned tg_ld_to_u(long double x);
signed char tg_ld_to_sc(long double x);
unsigned short tg_ld_to_us(long double x);
double tg_ld_to_d(long double x);
float tg_ld_to_f(long double x);
long double tg_ld_from_ll(long long v);
long double tg_ld_from_ull(unsigned long long v);
long double tg_ld_from_i(int v);
long double tg_ld_from_u(unsigned v);
long double tg_ld_from_sc(signed char v);
long double tg_ld_from_us(unsigned short v);
long double tg_ld_from_d(double v);
long double tg_ld_from_f(float v);
struct LDS tg_lds_twice(struct LDS v);
struct LDI tg_ldi_make(int i, long double x);
long double tg_ldi_sum(struct LDI v, struct LDS w);
long double tg_ld_call_c(long double x);
long double tg_ld_vsum(int n, ...);
extern long double tg_ld_g;
long double tg_ld_g_add(long double v);

long double c_ld_mix(int a, long double b, double c, long double d, int e) { return a + b * c - d / e; }
struct LDS c_lds(long double v) { struct LDS r = {v * 3}; return r; }
struct LDI c_ldi(long double v) { struct LDI r = {5, v - 1}; return r; }
static int ld_count;
long double c_ld_count(long double v) { ld_count++; return v; }

long double c_ld_vsum(int n, ...) {
	va_list ap;
	va_start(ap, n);
	long double s = n;
	for (int i = 0; i < n; i++) {
		double d = va_arg(ap, double);
		long double l = va_arg(ap, long double);
		int k = va_arg(ap, int);
		long double t = (long double)d * l;
		t += k;
		s += t;
	}
	va_end(ap);
	return s;
}

static long double expect_ld_call_c(long double x) {
	long double r = c_ld_mix(3, x, 2.5, x, 7);
	struct LDS s = c_lds(x);
	r += s.x;
	struct LDI t = c_ldi(x);
	r += t.x;
	r += t.i;
	r += c_ld_vsum(6, 1.5, x, 2, -2.0, r, 3, 0.5, x, -4, 8.0, s.x, 5, 0.75, x, 6, 3.0, r, -7);
	return r;
}

static int ld_cmp_mask(long double a, long double b) {
	return (a == b) | (a != b) << 1 | (a < b) << 2 | (a <= b) << 3 | (a > b) << 4 | (a >= b) << 5;
}

static void test_ld(void) {
	long double a = 1.0L + 0x1p-60L, b = 3.0L;
	long double out[7];
	tg_ld_ops(a, b, out);
	CHECK(out[0] == a + b && out[1] == a - b && out[2] == a * b && out[3] == a / b && out[4] == -a);
	CHECK(out[5] == a * b - b && out[6] == a + 0.5L);
	tg_ld_ops(-7.25L, 0.1L, out);
	CHECK(out[0] == -7.25L + 0.1L && out[1] == -7.25L - 0.1L && out[2] == -7.25L * 0.1L && out[3] == -7.25L / 0.1L && out[4] == 7.25L);

	long double nan = NAN;
	long double cmps[][2] = {{1, 2}, {2, 1}, {2, 2}, {1, 1.0L + 0x1p-60L}, {-0.0L, 0.0L}};
	for (int i = 0; i < 5; i++) CHECK(tg_ld_cmp(cmps[i][0], cmps[i][1]) == ld_cmp_mask(cmps[i][0], cmps[i][1]));
	CHECK(tg_ld_cmp(nan, 1) == 2 && tg_ld_cmp(1, nan) == 2 && tg_ld_cmp(nan, nan) == 2);

	CHECK(tg_ld_to_ll(-3.9L) == -3 && tg_ld_to_ll(9.2e18L) == 9200000000000000000LL && tg_ld_to_ll(-9223372036854775808.0L) == LLONG_MIN);
	CHECK(tg_ld_to_ull(18446744073709549568.0L) == 18446744073709549568ULL && tg_ld_to_ull(9223372036854775808.0L) == 1ULL << 63);
	CHECK(tg_ld_to_ull(3.9L) == 3 && tg_ld_to_ull(1e19L) == 10000000000000000000ULL && tg_ld_to_ull(0x1p63L - 1) == (unsigned long long)(0x1p63L - 1));
#if LDBL_MANT_DIG == 64 // a double cannot hold these
	CHECK(tg_ld_to_ull(18446744073709551615.0L) == ULLONG_MAX && tg_ld_to_ll(9223372036854775807.0L) == LLONG_MAX);
#endif
	CHECK(tg_ld_to_i(-2147483648.0L) == INT_MIN && tg_ld_to_i(2147483647.5L) == INT_MAX && tg_ld_to_i(-0.5L) == 0);
	CHECK(tg_ld_to_u(4294967295.9L) == UINT_MAX && tg_ld_to_u(3e9L) == 3000000000u);
	CHECK(tg_ld_to_sc(-100.7L) == -100 && tg_ld_to_sc(127.0L) == 127);
	CHECK(tg_ld_to_us(65535.5L) == 65535 && tg_ld_to_us(7.99L) == 7);
	CHECK(tg_ld_to_d(1.0L / 3) == (double)(1.0L / 3) && tg_ld_to_f(1.0L / 3) == (float)(1.0L / 3) && tg_ld_to_f(1e30L) == 1e30f);
	CHECK(tg_ld_to_d(a) == 1.0);

	CHECK(tg_ld_from_ll(LLONG_MIN) == (long double)LLONG_MIN && tg_ld_from_ll(-5) == -5.0L && tg_ld_from_ll(LLONG_MAX) == (long double)LLONG_MAX);
	CHECK(tg_ld_from_ull(ULLONG_MAX) == (long double)ULLONG_MAX && tg_ld_from_ull(1ULL << 63) == 0x1p63L && tg_ld_from_ull(12345) == 12345.0L);
	CHECK(tg_ld_from_ull((1ULL << 63) + 1) == (long double)((1ULL << 63) + 1));
	CHECK(tg_ld_from_i(INT_MIN) == (long double)INT_MIN && tg_ld_from_i(-7) == -7.0L);
	CHECK(tg_ld_from_u(UINT_MAX) == (long double)UINT_MAX && tg_ld_from_u(0x80000000u) == 0x1p31L);
	CHECK(tg_ld_from_sc(-128) == -128.0L && tg_ld_from_us(65535) == 65535.0L);
	CHECK(tg_ld_from_d(0.1) == (long double)0.1 && tg_ld_from_f(0.1f) == (long double)0.1f);
	CHECK(signbit((double)tg_ld_from_d(-0.0)) && tg_ld_from_d(-0.0) == 0); // MinGW has signbitl only for x87

	struct LDS s = {a};
	CHECK(tg_lds_twice(s).x == a + a);
	struct LDI li = tg_ldi_make(-9, a);
	CHECK(li.i == -9 && li.x == a);
	CHECK(tg_ldi_sum(li, s) == -9 + a + a);

	long double x = 0.1L;
	ld_count = 0;
	long double got = tg_ld_call_c(x), want = expect_ld_call_c(x);
	CHECK(got == want && ld_count == 1);
	if (got != want) printf("  tg_ld_call_c %.21Lg, want %.21Lg\n", got, want);
	got = tg_ld_vsum(6, 1.5, x, 2, -2.0, a, 3, 0.5, x, -4, 8.0, b, 5, 0.75, x, 6, 3.0, a, -7);
	want = c_ld_vsum(6, 1.5, x, 2, -2.0, a, 3, 0.5, x, -4, 8.0, b, 5, 0.75, x, 6, 3.0, a, -7);
	CHECK(got == want);
	if (got != want) printf("  tg_ld_vsum %.21Lg, want %.21Lg\n", got, want);

	CHECK(tg_ld_g == 1.25L);
	CHECK(tg_ld_g_add(a) == 1.25L + a && tg_ld_g == 1.25L + a);
}
#endif

////////////////////////////////////////////////////////////////
// called by tinygen code
////////////////////////////////////////////////////////////////

double c_many(int i0, int i1, int i2, int i3, int i4, int i5, int i6, int i7, int i8,
              double d0, double d1, double d2, double d3, double d4, double d5, double d6, double d7, double d8,
              struct F3 h, signed char c, unsigned short s, struct A16 a16, struct LI li) {
	int is[9] = {i0, i1, i2, i3, i4, i5, i6, i7, i8};
	double ds[9] = {d0, d1, d2, d3, d4, d5, d6, d7, d8};
	double r = 0;
	for (int k = 0; k < 9; k++) r += is[k] * (double)(k + 1);
	for (int k = 0; k < 9; k++) r += ds[k] * (double)(k + 1);
	r += h.x * 1.0 + h.y * 2.0 + h.z * 3.0;
	r += c;
	r += s;
	r += (double)a16.a - (double)a16.b;
	r += (double)li.a + li.b;
	return r;
}

struct F3 c_f3(void) { struct F3 v = {1.5f, -2.5f, 4.0f}; return v; }
struct D4 c_d4(void) { struct D4 v = {{0.5, 1.5, 2.5, 3.5}}; return v; }
struct LI c_li(void) { struct LI v = {77, -66}; return v; }
struct L3 c_l3(long long x) { struct L3 v = {x, 2*x, 3*x}; return v; }
struct A16 c_a16(void) { struct A16 v; v.a = 900; v.b = 33; return v; }
signed char c_schar(void) { return -5; }
unsigned short c_ushort(void) { return 65000; }
int c_small(signed char a, unsigned short b, short c) { return a + b + c; }
int (*c_small_ptr)(signed char, unsigned short, short) = c_small;
#if defined(__MINGW32__)
// tinygen reads the foreign c_small_ptr through __imp_c_small_ptr: link and lld make that pointer
// for a local definition (LNK4217), GNU ld does not
int (**__imp_c_small_ptr)(signed char, unsigned short, short) = &c_small_ptr;
#endif

static double expect_call_c(void) {
	struct F3 h = {1.25f, 2.5f, 3.75f};
	struct A16 a16; a16.a = 1000; a16.b = 300;
	struct LI li = {1LL << 40, -9};
	double r = c_many(-1, -2, -3, -4, -5, -6, -7, -8, -9, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, h, -7, 65000, a16, li);
	struct F3 f = c_f3();
	r += f.x * 10.0 + f.y * 20.0 + f.z * 30.0;
	struct D4 d = c_d4();
	for (int i = 0; i < 4; i++) r += d.a[i] * (100.0 * (i + 1));
	struct LI l = c_li();
	r += (double)l.a + (double)l.b;
	struct L3 l3 = c_l3(5);
	r += (double)l3.a * 1000 + (double)l3.b * 2000 + (double)l3.c * 3000;
	struct A16 a = c_a16();
	r += (double)(a.a - a.b);
	r += c_schar();
	r += c_ushort();
	r += c_small(-100, 60000, -30000);
	return r;
}

// reads the sequence of tg_call_vread / tg_vread after the first int
static double vread(va_list ap) {
	double s = 0;
	s += va_arg(ap, int);
	s += va_arg(ap, double) * 2;
	struct F3 f3 = va_arg(ap, struct F3);
	s += f3.x * 3.0 + f3.y * 4.0 + f3.z * 5.0;
	struct L3 l3 = va_arg(ap, struct L3);
	s += l3.a * 7.0 + l3.b * 8.0 + l3.c * 9.0;
	struct LI li = va_arg(ap, struct LI);
	s += (double)li.a + li.b * 3.0;
	struct A16 a16 = va_arg(ap, struct A16);
	s += (double)a16.a + a16.b * 5.0;
	s += va_arg(ap, int) * 11.0;
	struct D4 d4 = va_arg(ap, struct D4);
	for (int i = 0; i < 4; i++) s += d4.a[i] * (13.0 + i);
	struct CS cs = va_arg(ap, struct CS);
	s += cs.c * 17.0 + cs.s * 19.0;
	for (int i = 0; i < 9; i++) s += va_arg(ap, double) * (23.0 + i);
	s += (double)va_arg(ap, long long);
	return s;
}

// like tg_vread: the first int is read through a copy of the va_list, then again
double c_vread(int n, ...) {
	va_list ap, ap2;
	va_start(ap, n);
	va_copy(ap2, ap);
	double s = n;
	s += va_arg(ap2, int);
	va_end(ap2);
	s += vread(ap);
	va_end(ap);
	return s;
}

static double expect_vread(void) {
	struct F3 f3 = {0.5f, 1.5f, 2.5f};
	struct L3 l3 = {10, 20, 30};
	struct LI li = {100, 200};
	struct A16 a16; a16.a = 1000; a16.b = 2000;
	struct D4 d4 = {{1.5, 3.0, 4.5, 6.0}};
	struct CS cs = {-2, 300};
	return c_vread(19, 7, 2.5, f3, l3, li, a16, -3, d4, cs, 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.25, 123456789012LL);
}

// the thread locals of a fresh thread start from their initial values
static int tls_thread_check(void) {
#if TLS_DIRECT
	if (tg_tls_counter != 5 || tg_tls_big != 0) return 0;
#endif
	int ok = tg_tls_bump() == 6 && tg_tls_get_big() == 3;
#if TLS_DIRECT
	ok = ok && tg_tls_counter == 6;
#endif
	return ok;
}

#if defined(_WIN32)
static DWORD WINAPI tls_thread(LPVOID arg) { (void)arg; return (DWORD)tls_thread_check(); }
static int run_tls_thread(void) {
	DWORD res = 0;
	HANDLE th = CreateThread(NULL, 0, tls_thread, NULL, 0, NULL);
	if (th == NULL) return 0;
	WaitForSingleObject(th, INFINITE);
	GetExitCodeThread(th, &res);
	CloseHandle(th);
	return res == 1;
}
#else
static void *tls_thread(void *arg) { (void)arg; return tls_thread_check() ? (void *)1 : (void *)0; }
static int run_tls_thread(void) {
	pthread_t th;
	void *res = NULL;
	if (pthread_create(&th, NULL, tls_thread, NULL) != 0) return 0;
	pthread_join(th, &res);
	return res == (void *)1;
}
#endif

// tg_rmw_<op>_N and tg_cas_N for N in 8, 16, 32, 64
#define ATOMIC_DECLS(N) \
	uint##N##_t tg_rmw_xchg_##N(uint##N##_t *p, uint##N##_t v); \
	uint##N##_t tg_rmw_add_##N(uint##N##_t *p, uint##N##_t v); \
	uint##N##_t tg_rmw_sub_##N(uint##N##_t *p, uint##N##_t v); \
	uint##N##_t tg_rmw_and_##N(uint##N##_t *p, uint##N##_t v); \
	uint##N##_t tg_rmw_or_##N(uint##N##_t *p, uint##N##_t v); \
	uint##N##_t tg_rmw_xor_##N(uint##N##_t *p, uint##N##_t v); \
	uint##N##_t tg_rmw_nand_##N(uint##N##_t *p, uint##N##_t v); \
	uint##N##_t tg_cas_##N(uint##N##_t *p, uint##N##_t expected, uint##N##_t desired, int *ok);
ATOMIC_DECLS(8)
ATOMIC_DECLS(16)
ATOMIC_DECLS(32)
ATOMIC_DECLS(64)

// Each op once on a cell that starts with its top bit set, against the same in C; the
// bytes around the cell stay. A CAS that fails, then one that succeeds.
#define ATOMIC_TEST(N) \
	static void test_atomics_##N(void) { \
		typedef uint##N##_t U; \
		typedef U (*rmw_fn)(U *, U); \
		static const rmw_fn fns[7] = {tg_rmw_xchg_##N, tg_rmw_add_##N, tg_rmw_sub_##N, tg_rmw_and_##N, \
		                              tg_rmw_or_##N, tg_rmw_xor_##N, tg_rmw_nand_##N}; \
		const U start = (U)0xA5C3F00F8E7D6B59ull | (U)((U)1 << (N - 1)); \
		const U v = (U)0x3C96E1D2B4A58770ull; \
		for (int i = 0; i < 7; i++) { \
			struct { U lo, cell, hi; } c; \
			c.lo = c.hi = (U)0x5A5A5A5A5A5A5A5Aull; \
			c.cell = start; \
			U old = fns[i](&c.cell, v); \
			U want; \
			switch (i) { \
			case 0: want = v; break; \
			case 1: want = (U)(start + v); break; \
			case 2: want = (U)(start - v); break; \
			case 3: want = (U)(start & v); break; \
			case 4: want = (U)(start | v); break; \
			case 5: want = (U)(start ^ v); break; \
			default: want = (U)~(start & v); break; \
			} \
			CHECK(old == start && c.cell == want); \
			if (old != start || c.cell != want) printf("  %d bit op %d: old %llx cell %llx, want %llx\n", N, i, \
			                                          (unsigned long long)old, (unsigned long long)c.cell, (unsigned long long)want); \
			CHECK(c.lo == (U)0x5A5A5A5A5A5A5A5Aull && c.hi == (U)0x5A5A5A5A5A5A5A5Aull); \
		} \
		U cell = start; \
		int ok = -1; \
		CHECK(tg_cas_##N(&cell, (U)(start - 1), v, &ok) == start && ok == 0 && cell == start); \
		CHECK(tg_cas_##N(&cell, start, v, &ok) == start && ok == 1 && cell == v); \
	}
ATOMIC_TEST(8)
ATOMIC_TEST(16)
ATOMIC_TEST(32)
ATOMIC_TEST(64)

#ifndef _WIN32
// Threads that add to one cell at once, through tg_rmw_add_32 and a tg_cas_64 loop, lose no update
enum { CONTEND_THREADS = 4, CONTEND_ROUNDS = 100000 };
static uint32_t contend_add;
static uint64_t contend_cas;
static void *contend_thread(void *arg) {
	uint64_t seen = 0;
	(void)arg;
	for (int i = 0; i < CONTEND_ROUNDS; i++) {
		tg_rmw_add_32(&contend_add, 1);
		int ok = 0;
		while (!ok) {
			uint64_t cur = tg_cas_64(&contend_cas, seen, seen + 1, &ok);
			seen = ok ? seen + 1 : cur;
		}
	}
	return NULL;
}
static void test_atomics_contended(void) {
	pthread_t th[CONTEND_THREADS];
	for (int i = 0; i < CONTEND_THREADS; i++) CHECK(pthread_create(&th[i], NULL, contend_thread, NULL) == 0);
	for (int i = 0; i < CONTEND_THREADS; i++) pthread_join(th[i], NULL);
	CHECK(contend_add == CONTEND_THREADS * CONTEND_ROUNDS);
	CHECK(contend_cas == (uint64_t)CONTEND_THREADS * CONTEND_ROUNDS);
}
#endif

// the bit operations in plain C, for compilers without GCC's builtins (MSVC)
static int popcount64(unsigned long long x) { int n = 0; for (; x; x &= x - 1) n++; return n; }
static int ctz64(unsigned long long x) { int n = 0; while (!(x & 1)) { x >>= 1; n++; } return n; }
static int clz64(unsigned long long x) { int n = 0; while (!(x >> 63)) { x <<= 1; n++; } return n; }
static int clz32(unsigned x) { int n = 0; while (!(x >> 31)) { x <<= 1; n++; } return n; }
static unsigned long long bswap64(unsigned long long x) {
	unsigned long long r = 0;
	for (int i = 0; i < 8; i++) { r = (r << 8) | (x & 0xff); x >>= 8; }
	return r;
}

static long long expect_misc(long long x) {
	long long r = x * 3 + 11;
	r += 0;      // the over-aligned local's low bits
	r += 5;
	r += 0;      // alloca's low bits
	r += 0x11;
	long long cell = 40;
	r += cell; cell += 2;
	r += cell; cell = 7;
	r += cell; cell = ~(cell & 3);
	r += cell; cell = 9;
	r += 1;
	r += cell;
	r += popcount64((unsigned long long)x);
	r += x ? ctz64((unsigned long long)x) : 64;
	r += x ? clz64((unsigned long long)x) : 64;
	r += (long long)(bswap64((unsigned long long)x) >> 56);
	r += (unsigned)x ? clz32((unsigned)x) : 32;
	r += (-x) / 7;
	r += (-x) % 7;
	r += (long long)((unsigned long long)x / 5);
	r += (double)x > 100.5 ? 1000 : 2000;
	float xf = (float)((double)x / 4);
	r += (long long)sqrt((double)xf);
	r += (long long)(unsigned long long)(double)(unsigned long long)(x << 2);
	return r;
}

////////////////////////////////////////////////////////////////
// Packed records
////////////////////////////////////////////////////////////////

static void pk_fill(void *v, int i, int size) {
	for (int j = 0; j < size; j++) ((unsigned char *)v)[j] = (unsigned char)((i*37 + j*11 + 1) & 0xff);
}
static long long pk_sum(const void *v, int i, int size) {
	long long s = 0;
	for (int j = 0; j < size; j++) s += ((const unsigned char *)v)[j] * (long long)(100*i + j + 1);
	return s;
}
long long c_pk_sum(long long k0, long long k1, long long k2, long long k3, long long k4, struct PA a,
                   struct PB b, struct PC c, struct PD d, struct PE e, struct PF f, struct PG g, long long last) {
	return k0 + 2*k1 + 3*k2 + 4*k3 + 5*k4 + pk_sum(&a, 0, 5) + pk_sum(&b, 1, 5) + pk_sum(&c, 2, 12) +
	       pk_sum(&d, 3, 9) + pk_sum(&e, 4, 9) + pk_sum(&f, 5, 9) + pk_sum(&g, 6, 16) + 1000*last;
}
struct PA c_pk_echo0(struct PA v) { return v; }
struct PB c_pk_echo1(struct PB v) { return v; }
struct PC c_pk_echo2(struct PC v) { return v; }
struct PD c_pk_echo3(struct PD v) { return v; }
struct PE c_pk_echo4(struct PE v) { return v; }
struct PF c_pk_echo5(struct PF v) { return v; }
struct PG c_pk_echo6(struct PG v) { return v; }

static void test_pk(void) {
	struct PA a; struct PB b; struct PC c; struct PD d; struct PE e; struct PF f; struct PG g;
	CHECK(sizeof a == 5 && sizeof b == 5 && sizeof c == 12 && sizeof d == 9 && sizeof e == 9 && sizeof f == 9 && sizeof g == 16);
	pk_fill(&a, 0, 5); pk_fill(&b, 1, 5); pk_fill(&c, 2, 12); pk_fill(&d, 3, 9);
	pk_fill(&e, 4, 9); pk_fill(&f, 5, 9); pk_fill(&g, 6, 16);
	{ struct PA r = tg_pk_echo0(a); CHECK(memcmp(&r, &a, sizeof a) == 0); }
	{ struct PB r = tg_pk_echo1(b); CHECK(memcmp(&r, &b, sizeof b) == 0); }
	{ struct PC r = tg_pk_echo2(c); CHECK(memcmp(&r, &c, sizeof c) == 0); }
	{ struct PD r = tg_pk_echo3(d); CHECK(memcmp(&r, &d, sizeof d) == 0); }
	{ struct PE r = tg_pk_echo4(e); CHECK(memcmp(&r, &e, sizeof e) == 0); }
	{ struct PF r = tg_pk_echo5(f); CHECK(memcmp(&r, &f, sizeof f) == 0); }
	{ struct PG r = tg_pk_echo6(g); CHECK(memcmp(&r, &g, sizeof g) == 0); }
	{
		long long want = c_pk_sum(1, -2, 3, -4, 5, a, b, c, d, e, f, g, -6);
		long long got = tg_pk_sum(1, -2, 3, -4, 5, a, b, c, d, e, f, g, -6);
		CHECK(got == want);
		if (got != want) printf("  tg_pk_sum %lld, want %lld\n", got, want);
	}
	{
		long long want = c_pk_sum(-1000003, -2000006, -3000009, -4000012, -5000015, a, b, c, d, e, f, g, 77) +
		                 2*pk_sum(&a, 0, 5) + 3*pk_sum(&b, 1, 5) + 4*pk_sum(&c, 2, 12) + 5*pk_sum(&d, 3, 9) +
		                 6*pk_sum(&e, 4, 9) + 7*pk_sum(&f, 5, 9) + 8*pk_sum(&g, 6, 16);
		long long got = tg_pk_call();
		CHECK(got == want);
		if (got != want) printf("  tg_pk_call %lld, want %lld\n", got, want);
	}
}

int main(void) {
	struct F3 f = tg_f3_scale((struct F3){1.0f, -2.0f, 0.5f}, 3.0f);
	CHECK(f.x == 3.0f && f.y == -6.0f && f.z == 1.5f);
	struct D4 d = tg_d4_rev((struct D4){{1, 2, 3, 4}});
	CHECK(d.a[0] == 4 && d.a[1] == 3 && d.a[2] == 2 && d.a[3] == 1);
	CHECK(tg_cs_sum((struct CS){-3, -1000}) == -1003);
	struct LI li = tg_li_make(1LL << 50, -77);
	CHECK(li.a == 1LL << 50 && li.b == -77);
	struct A16 a16; a16.a = 5; a16.b = 1LL << 33;
	CHECK(tg_a16_sum(-1, a16) == 4 + (1LL << 33));
	{
		struct F3 h = {1.25f, 2.5f, 3.75f};
		struct A16 a; a.a = 1000; a.b = 300;
		struct LI l = {1LL << 40, -9};
		double want = c_many(-1, -2, -3, -4, -5, -6, -7, -8, -9, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, h, -7, 65000, a, l);
		double got = tg_many(-1, -2, -3, -4, -5, -6, -7, -8, -9, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, h, -7, 65000, a, l);
		CHECK(got == want);
	}
	CHECK(tg_small(-100, 60000, -30000) == 29900);
	CHECK(tg_small_ret(0x1234fe) == -2);
	CHECK(tg_call_c() == expect_call_c());
	test_pk();
	{
		double want = expect_vread();
		CHECK(tg_call_vread() == want);
		struct F3 f3 = {0.5f, 1.5f, 2.5f};
		struct L3 l3 = {10, 20, 30};
		struct LI vli = {100, 200};
		struct A16 va; va.a = 1000; va.b = 2000;
		struct D4 d4 = {{1.5, 3.0, 4.5, 6.0}};
		struct CS cs = {-2, 300};
		double got = tg_vread(19, 7, 2.5, f3, l3, vli, va, -3, d4, cs, 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.25, 123456789012LL);
		CHECK(got == want);
		if (got != want) printf("  tg_vread %.17g, want %.17g\n", got, want);
	}
	{
		char buf[200], want[200];
		int n = tg_snprintf(buf);
		snprintf(want, sizeof(want), "%d %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %s %lld %c",
		         -42, 1.25, 2.5, 3.75, 5.0, 6.25, 7.5, 8.75, 10.0, 11.25, 12.5, "word", -1234567890123LL, 'z');
		CHECK(n == (int)strlen(want) && strcmp(buf, want) == 0);
		if (strcmp(buf, want) != 0) printf("  got  %s\n  want %s\n", buf, want);
	}
	{
#if TLS_DIRECT
		CHECK(tg_tls_counter == 5);
		CHECK(tg_tls_bump() == 6 && tg_tls_bump() == 7 && tg_tls_counter == 7 && tg_tls_get_big() == 6 && tg_tls_big == 6);
		CHECK(run_tls_thread());
		CHECK(tg_tls_counter == 7);
#else
		CHECK(tg_tls_bump() == 6 && tg_tls_bump() == 7 && tg_tls_get_big() == 6);
		CHECK(run_tls_thread());
		CHECK(tg_tls_bump() == 8); // the other thread's counter was its own
#endif
	}
	{
		long long xs[4] = {1, 77, 1000003, 0x123456789aLL};
		for (int i = 0; i < 4; i++) {
			long long got = tg_misc(xs[i]), want = expect_misc(xs[i]);
			CHECK(got == want);
			if (got != want) printf("  tg_misc(%lld) = %lld, want %lld\n", xs[i], got, want);
		}
	}
#ifdef HAVE_LD
	test_ld();
#endif
	test_atomics_8();
	test_atomics_16();
	test_atomics_32();
	test_atomics_64();
#ifndef _WIN32
	test_atomics_contended();
#endif
	CHECK(tg_far(5) == 250005);
	CHECK(tg_far(-5) == -5);
	printf(failures ? "%d FAILED\n" : "all passed\n", failures);
	return failures != 0;
}
