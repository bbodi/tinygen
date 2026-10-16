// Calls the functions test/smoke.c builds with tinygen, and checks them.
// The same file builds on x86-64 Linux and Windows: the 64-bit integers are long long,
// since a Windows long has 32 bits.
#include <stdio.h>
#include <setjmp.h>

struct V { double x, y; };
struct Big { long long a, b, c; };
struct P { int x, y; };
struct C3 { char a, b, c; };
struct Q { long long a, b; };

typedef struct Point { int x, y; } Point;
enum Color { RED, GREEN = 5, BLUE };
struct Flags { unsigned a : 3; unsigned b : 5; int c; };
struct Node {
	struct Node *next; const char *name; double vals[3]; enum Color color;
	struct Flags flags; union { int i; float f; } u;
};

int tg_add(int a, int b);
int tg_abort_me(void);
long long tg_fib(long long n);
long long tg_sum(long long n);
struct V tg_vscale(struct V v, double k);
long long tg_big_sum(struct Big b);
struct Big tg_make_big(long long x);
int tg_greet(int n);
int tg_vsum(int count, ...);
double tg_vmix(int n, ...);
double tg_call_vmix(void);
double tg_mix(int a, double b, long long c, float d, int e, double f, struct P g);
struct P tg_pswap(struct P p);
int tg_c3sum(struct C3 c);
double tg_fkeep(double x);
long long tg_call_cb(long long (*cb)(long long), long long x);
long long tg_bigframe(long long n);
long long tg_bigalloca(long long n);
int tg_tls_bump(void);
long long tg_read_ext(void);
int tg_weak_answer(void);
long long tg_node_sum(struct Node *n, Point p);
long long tg_copy_fwd(struct Q *q, struct Q s);
long long tg_q_diff(struct Q q) { return q.a - q.b; }
long long tg_pair(const signed char *b, const short *h, const int *w);
long long tg_store_fwd(long long *a, long long *b);
extern Point tg_origin;
#ifndef _WIN32
extern int tg_prot_var;
int tg_prot_get(void);
int tg_sec_fn(int x);
#endif
#ifdef __linux__
// the linker's bounds of the section tg_code
extern char __start_tg_code[], __stop_tg_code[];
#endif

long long tg_ext_value = 1234;
#if defined(__MINGW32__)
// tg_read_ext reads it through __imp_tg_ext_value: link and lld make that pointer for a local
// definition (LNK4217), GNU ld does not
long long *__imp_tg_ext_value = &tg_ext_value;
#endif

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static jmp_buf jump;

static long long twice(long long x) { return 2*x; }

// longjmp across tg_call_cb's frame: on Windows the unwinder walks it with its .pdata/.xdata
static long long escape(long long x) {
	longjmp(jump, (int)x);
	return 0;
}

int main(void) {
	CHECK(tg_abort_me() == 7);
	CHECK(tg_add(2, 40) == 42);
	CHECK(tg_add(-5, 3) == -2);
	CHECK(tg_fib(20) == 6765);
	CHECK(tg_sum(100) == 4950);
	struct V v = tg_vscale((struct V){1.5, -2.0}, 2.0);
	CHECK(v.x == 3.0 && v.y == -4.0);
	CHECK(tg_big_sum((struct Big){1, 20, 300}) == 321);
	struct Big b = tg_make_big(7);
	CHECK(b.a == 7 && b.b == 14 && b.c == 21);
	CHECK(tg_greet(42) == 14); /* prints "hello 42 1.50\n" */
	CHECK(tg_vsum(0) == 0);
	CHECK(tg_vsum(3, 1, 2, 3) == 6);
	CHECK(tg_vsum(8, 1, 2, 3, 4, 5, 6, 7, 8) == 36); /* past the registers */
	CHECK(tg_vmix(2, 0.5, (struct P){1, 2}, 0.25, (struct P){3, 4}, (struct Big){5, 6, 7}) == 28.75);
	CHECK(tg_call_vmix() == 28.75);
	CHECK(tg_mix(1, 2.5, 3, 4.5f, 5, 6.25, (struct P){7, 8}) == 37.25);
	struct P sw = tg_pswap((struct P){3, -9});
	CHECK(sw.x == -9 && sw.y == 3);
	CHECK(tg_c3sum((struct C3){1, -2, 30}) == 29);
	CHECK(tg_fkeep(1.5) == 6.0);
	CHECK(tg_call_cb(twice, 20) == 41);
	int jumped = setjmp(jump);
	if (jumped == 0) {
		tg_call_cb(escape, 77);
		CHECK(!"longjmp did not leave tg_call_cb");
	} else {
		CHECK(jumped == 77);
	}
	CHECK(tg_bigframe(5) == 15);
	CHECK(tg_bigalloca(256*1024) == 3);
	CHECK(tg_tls_bump() == 43);
	CHECK(tg_tls_bump() == 45);
	CHECK(tg_read_ext() == 1234);
	CHECK(tg_weak_answer() == 7);
	CHECK(tg_origin.x == 10 && tg_origin.y == 20);
#ifndef _WIN32
	CHECK(tg_prot_var == 41 && tg_prot_get() == 42);
	CHECK(tg_sec_fn(5) == 11 && tg_sec_fn(-3) == -1);
#endif
#ifdef __linux__
	CHECK((char *)tg_sec_fn >= __start_tg_code && (char *)tg_sec_fn < __stop_tg_code);
#endif
	struct Node n = {0};
	n.color = GREEN;
	n.flags.c = 100;
	CHECK(tg_node_sum(&n, (Point){1, 2}) == 108);
	struct Q q = {1, 2};
	CHECK(tg_copy_fwd(&q, (struct Q){3, 4}) == 504109);
	CHECK(q.a == 100 && q.b == 9);
	{
		signed char pb[3] = {-3, -100, 5};
		short ph[3] = {-30000, 1234, 7};
		int pw[3] = {-2000000000, 1999999999, 9};
		CHECK(tg_pair(pb + 0, ph, pw) == (-100 - 2*-3 + 7*-30000 - 1234 + 3*1999999999LL - -2000000000LL));
		CHECK(tg_pair(pb + 1, ph + 1, pw + 1) == (5 - 2*-100 + 7*1234 - 7 + 3*9 - 1999999999LL));
	}
	{
		long long sa[3] = {0, 0, 0}, sb = 0;
		CHECK(tg_store_fwd(sa, &sb) == 6 + 0x2222222211111111LL + 6000);
		CHECK(tg_store_fwd(sa, sa + 1) == 7 + 0x2222222211111111LL + 6000);
		CHECK(tg_store_fwd(sa, sa) == 6 + 7 + 6000);
	}
	printf(failures ? "%d FAILED\n" : "all passed\n", failures);
	return failures != 0;
}
