// C output (TYG_OUTPUT_C): the module as one C99 translation unit. tg_write_c prints the
// target, a prelude of macros, the records of the signatures, a declaration of every
// symbol, the data with its relocations as pointers, and the procedures' text.

#include <stdarg.h>

// Appends printf formatted text.
TG_INTERNAL void tg_c_printf(tgBytes *b, char const *fmt, ...) {
	char small[256];
	va_list va;
	va_start(va, fmt);
	int n = vsnprintf(small, sizeof(small), fmt, va);
	va_end(va);
	TG_ASSERT(n >= 0);
	if (n < (int)sizeof(small)) {
		tg_array_add_elems(b, (u8 const *)small, n);
		return;
	}
	tg_array_reserve(b, b->count + n + 1);
	va_start(va, fmt);
	vsnprintf((char *)b->data + b->count, (usize)n + 1, fmt, va);
	va_end(va);
	b->count += n;
}

TG_INTERNAL void tg_c_puts(tgBytes *b, char const *s) {
	tg_array_add_elems(b, (u8 const *)s, (isize)strlen(s));
}

static char const tg_c_prelude[] =
	"#if defined(TYG_TARGET_X64) && !(defined(__x86_64__) || defined(_M_X64))\n"
	"#error \"this file is for x86-64\"\n"
	"#endif\n"
	"#if defined(TYG_TARGET_ARM64) && !(defined(__aarch64__) || defined(_M_ARM64))\n"
	"#error \"this file is for arm64\"\n"
	"#endif\n"
	"#include <stdint.h>\n"
	"#include <string.h>\n"
	"#include <stddef.h>\n"
	"#include <stdlib.h>\n"
	"#include <math.h>\n"
	"#include <stdarg.h>\n"
	"#if defined(__clang__)\n" // the prototypes of libc functions take void * for const char *
	"#pragma clang diagnostic ignored \"-Wincompatible-library-redeclaration\"\n"
	"#elif defined(__GNUC__)\n"
	"#pragma GCC diagnostic ignored \"-Wbuiltin-declaration-mismatch\"\n"
	"#endif\n"
	"#define TYG_STR2(x) #x\n"
	"#define TYG_STR(x) TYG_STR2(x)\n"
	"#if defined(__GNUC__) || defined(__clang__)\n"
	"#define TYG_ALIGNED(n) __attribute__((aligned(n)))\n"
	"#define TYG_THREAD __thread\n"
	"#define TYG_WEAK __attribute__((weak))\n"
	"#define TYG_SECTION(name) __attribute__((section(name)))\n"
	"#define TYG_ASM_NAME(name) __asm__(TYG_STR(__USER_LABEL_PREFIX__) name)\n"
	"#define TYG_ALTERNATE_NAME(c, name)\n"
	"#define TYG_COMMON __attribute__((common))\n"
	"#define TYG_NORETURN __attribute__((noreturn))\n"
	"#define TYG_RETURNS_TWICE __attribute__((returns_twice))\n"
	"#define TYG_UNUSED __attribute__((unused))\n"
	"#define TYG_USED __attribute__((used))\n"
	"#define TYG_UNREACHABLE() __builtin_unreachable()\n"
	"#define TYG_TRAP() __builtin_trap()\n"
	"#if defined(__clang__)\n"
	"#define TYG_DEBUG_TRAP() __builtin_debugtrap()\n"
	"#elif defined(__x86_64__)\n"
	"#define TYG_DEBUG_TRAP() __asm__ volatile(\"int3\")\n"
	"#elif defined(__aarch64__)\n"
	"#define TYG_DEBUG_TRAP() __asm__ volatile(\"brk #0xf000\")\n"
	"#else\n"
	"#define TYG_DEBUG_TRAP() __builtin_trap()\n"
	"#endif\n"
	"#if defined(_WIN32)\n"
	"#define TYG_HIDDEN\n"
	"#define TYG_PROTECTED\n"
	"#define TYG_EXPORT __declspec(dllexport)\n"
	"#else\n"
	"#define TYG_HIDDEN __attribute__((visibility(\"hidden\")))\n"
	"#define TYG_EXPORT __attribute__((visibility(\"default\")))\n"
	"#if defined(__APPLE__)\n"
	"#define TYG_PROTECTED\n"
	"#else\n"
	"#define TYG_PROTECTED __attribute__((visibility(\"protected\")))\n"
	"#endif\n"
	"#endif\n"
	"#if defined(__APPLE__)\n"
	"#define TYG_ALIAS(target)\n"
	"#else\n"
	"#define TYG_ALIAS(target) __attribute__((alias(target)))\n"
	"#endif\n"
	"#elif defined(_MSC_VER)\n"
	"#define TYG_ALIGNED(n) __declspec(align(n))\n"
	"#define TYG_THREAD __declspec(thread)\n"
	"#define TYG_WEAK\n"
	"#define TYG_SECTION(name)\n"
	"#define TYG_ASM_NAME(name)\n"
	"#define TYG_ALTERNATE_NAME(c, name) __pragma(comment(linker, \"/alternatename:\" #c \"=\" name))\n"
	"#define TYG_COMMON\n"
	"#define TYG_NORETURN __declspec(noreturn)\n"
	"#define TYG_RETURNS_TWICE\n"
	"#define TYG_UNUSED\n"
	"#define TYG_USED\n"
	"#define TYG_UNREACHABLE() __assume(0)\n"
	"#define TYG_TRAP() abort()\n"
	"#define TYG_DEBUG_TRAP() __debugbreak()\n"
	"#define TYG_HIDDEN\n"
	"#define TYG_PROTECTED\n"
	"#define TYG_EXPORT __declspec(dllexport)\n"
	"#define TYG_ALIAS(target)\n"
	"#else\n"
	"#define TYG_ALIGNED(n)\n"
	"#define TYG_THREAD\n"
	"#define TYG_WEAK\n"
	"#define TYG_SECTION(name)\n"
	"#define TYG_ASM_NAME(name)\n"
	"#define TYG_ALTERNATE_NAME(c, name)\n"
	"#define TYG_COMMON\n"
	"#define TYG_NORETURN\n"
	"#define TYG_RETURNS_TWICE\n"
	"#define TYG_UNUSED\n"
	"#define TYG_USED\n"
	"#define TYG_UNREACHABLE() abort()\n"
	"#define TYG_TRAP() abort()\n"
	"#define TYG_DEBUG_TRAP() ((void)0)\n"
	"#define TYG_HIDDEN\n"
	"#define TYG_PROTECTED\n"
	"#define TYG_EXPORT\n"
	"#define TYG_ALIAS(target)\n"
	"#endif\n"
	// scalar loads (LD), stores (ST) and bitcasts (MV) of 1, 2, 4 and 8 bytes: memcpy, which the
	// compilers turn into plain moves; TinyCC would call it, and has no aliasing rules to break
	"#if defined(__TINYC__)\n"
	"#define TYG_LD_(n, dst, p) (*(uint##n##_t *)(dst) = *(uint##n##_t *)(p))\n"
	"#else\n"
	"#define TYG_LD_(n, dst, p) ((void)memcpy((dst), (p), (n) / 8))\n"
	"#endif\n"
	"#define TYG_LD8(dst, p) TYG_LD_(8, dst, p)\n"
	"#define TYG_LD16(dst, p) TYG_LD_(16, dst, p)\n"
	"#define TYG_LD32(dst, p) TYG_LD_(32, dst, p)\n"
	"#define TYG_LD64(dst, p) TYG_LD_(64, dst, p)\n"
	"#define TYG_ST8(p, src) TYG_LD_(8, p, src)\n"
	"#define TYG_ST16(p, src) TYG_LD_(16, p, src)\n"
	"#define TYG_ST32(p, src) TYG_LD_(32, p, src)\n"
	"#define TYG_ST64(p, src) TYG_LD_(64, p, src)\n"
	"#define TYG_MV8(dst, src) TYG_LD_(8, dst, src)\n"
	"#define TYG_MV16(dst, src) TYG_LD_(16, dst, src)\n"
	"#define TYG_MV32(dst, src) TYG_LD_(32, dst, src)\n"
	"#define TYG_MV64(dst, src) TYG_LD_(64, dst, src)\n"
	"#if defined(__TINYC__)\n" // TinyCC takes the gcc branch, but has no __builtin_trap, no brk and no __USER_LABEL_PREFIX__
	"#undef TYG_TRAP\n"
	"#define TYG_TRAP() abort()\n"
	"#undef TYG_DEBUG_TRAP\n"
	"#define TYG_DEBUG_TRAP() abort()\n"
	"#undef TYG_ASM_NAME\n"
	"#if defined(__leading_underscore)\n"
	"#define TYG_ASM_NAME(name) __asm__(\"_\" name)\n"
	"#else\n"
	"#define TYG_ASM_NAME(name) __asm__(name)\n"
	"#endif\n"
	"#endif\n"
	"#if defined(TYG_TARGET_WINDOWS)\n"
	"typedef double TYG_F80;\n"
	"#else\n"
	"typedef long double TYG_F80;\n"
	"#endif\n";

// The parts of the prelude a module's procedures may use, see tgProc.c_uses.
enum {
	tgCUse_Atomics = 1, // tg_c_atomics_prelude
	tgCUse_Intrin  = 2, // tg_c_intrin_prelude
	tgCUse_Ld      = 4, // tg_c_ld_prelude
};

// The intrinsics and alloca. CTZ and CLZ of zero are the width, like the lowerings'.
static char const tg_c_intrin_prelude[] =
	"#if defined(__GNUC__) || defined(__clang__)\n"
	"#define TYG_BSWAP_16(x) __builtin_bswap16(x)\n"
	"#define TYG_BSWAP_32(x) __builtin_bswap32(x)\n"
	"#define TYG_BSWAP_64(x) __builtin_bswap64(x)\n"
	"#define TYG_POPCOUNT_32(x) __builtin_popcount(x)\n"
	"#define TYG_POPCOUNT_64(x) __builtin_popcountll(x)\n"
	"#define TYG_CTZ_32(x) ((x) ? __builtin_ctz(x) : 32)\n"
	"#define TYG_CTZ_64(x) ((x) ? __builtin_ctzll(x) : 64)\n"
	"#define TYG_CLZ_32(x) ((x) ? __builtin_clz(x) : 32)\n"
	"#define TYG_CLZ_64(x) ((x) ? __builtin_clzll(x) : 64)\n"
	"#define TYG_FRAME_ADDRESS() __builtin_frame_address(0)\n"
	"#define TYG_RETURN_ADDRESS() __builtin_return_address(0)\n"
	"#define TYG_ALLOCA(n) __builtin_alloca(n)\n"
	"#if defined(__x86_64__) || defined(__i386__)\n"
	"#define TYG_CPU_RELAX() __builtin_ia32_pause()\n"
	"#elif defined(__aarch64__) || defined(__arm__)\n"
	"#define TYG_CPU_RELAX() __asm__ __volatile__(\"yield\")\n"
	"#else\n"
	"#define TYG_CPU_RELAX() ((void)0)\n"
	"#endif\n"
	"#elif defined(_MSC_VER)\n"
	"#include <intrin.h>\n"
	"#include <malloc.h>\n"
	"#define TYG_BSWAP_16(x) _byteswap_ushort(x)\n"
	"#define TYG_BSWAP_32(x) _byteswap_ulong(x)\n"
	"#define TYG_BSWAP_64(x) _byteswap_uint64(x)\n"
	"static __inline uint32_t tyg_popcount64(uint64_t x) {\n"
	"\tx = x - ((x >> 1) & 0x5555555555555555ull);\n"
	"\tx = (x & 0x3333333333333333ull) + ((x >> 2) & 0x3333333333333333ull);\n"
	"\treturn (uint32_t)((((x + (x >> 4)) & 0x0f0f0f0f0f0f0f0full) * 0x0101010101010101ull) >> 56);\n"
	"}\n"
	"static __inline uint32_t tyg_ctz64(uint64_t x) { unsigned long i; return _BitScanForward64(&i, x) ? (uint32_t)i : 64; }\n"
	"static __inline uint32_t tyg_clz64(uint64_t x) { unsigned long i; return _BitScanReverse64(&i, x) ? 63 - (uint32_t)i : 64; }\n"
	"#define TYG_POPCOUNT_32(x) tyg_popcount64(x)\n"
	"#define TYG_POPCOUNT_64(x) tyg_popcount64(x)\n"
	"#define TYG_CTZ_32(x) ((x) ? tyg_ctz64(x) : 32)\n"
	"#define TYG_CTZ_64(x) tyg_ctz64(x)\n"
	"#define TYG_CLZ_32(x) (tyg_clz64(x) - 32)\n"
	"#define TYG_CLZ_64(x) tyg_clz64(x)\n"
	"#define TYG_FRAME_ADDRESS() ((void *)((char *)_AddressOfReturnAddress() - sizeof(void *)))\n"
	"#define TYG_RETURN_ADDRESS() _ReturnAddress()\n"
	"#define TYG_ALLOCA(n) _alloca(n)\n"
	"#if defined(_M_ARM64)\n"
	"#define TYG_CPU_RELAX() __yield()\n"
	"#else\n"
	"#define TYG_CPU_RELAX() _mm_pause()\n"
	"#endif\n"
	"#endif\n"
	"#if defined(__TINYC__)\n" // no __builtin_bswap or __builtin_ia32_pause; the rest it has
	"#undef TYG_BSWAP_16\n"
	"#undef TYG_BSWAP_32\n"
	"#undef TYG_BSWAP_64\n"
	"#undef TYG_CPU_RELAX\n"
	"static TYG_UNUSED uint16_t tyg_bswap16(uint16_t x) { return (uint16_t)(x >> 8 | x << 8); }\n"
	"static TYG_UNUSED uint32_t tyg_bswap32(uint32_t x) { return x >> 24 | (x >> 8 & 0xff00u) | (x << 8 & 0xff0000u) | x << 24; }\n"
	"static TYG_UNUSED uint64_t tyg_bswap64(uint64_t x) { return (uint64_t)tyg_bswap32((uint32_t)x) << 32 | tyg_bswap32((uint32_t)(x >> 32)); }\n"
	"#define TYG_BSWAP_16(x) tyg_bswap16(x)\n"
	"#define TYG_BSWAP_32(x) tyg_bswap32(x)\n"
	"#define TYG_BSWAP_64(x) tyg_bswap64(x)\n"
	"#define TYG_CPU_RELAX() ((void)0)\n"
	"#endif\n"
	"#define TYG_BSWAP_8(x) (x)\n"
	"#define TYG_POPCOUNT_8(x) TYG_POPCOUNT_32((uint32_t)(x))\n"
	"#define TYG_POPCOUNT_16(x) TYG_POPCOUNT_32((uint32_t)(x))\n"
	"#define TYG_CTZ_8(x) TYG_CTZ_32((uint32_t)(x) | 0x100u)\n" // a bit above the value: zero counts 8
	"#define TYG_CTZ_16(x) TYG_CTZ_32((uint32_t)(x) | 0x10000u)\n"
	"#define TYG_CLZ_8(x) (TYG_CLZ_32((uint32_t)(x)) - 24)\n"
	"#define TYG_CLZ_16(x) (TYG_CLZ_32((uint32_t)(x)) - 16)\n";

// long double through memcpy: the operands are bytes in locals or anywhere else.
static char const tg_c_ld_prelude[] =
	"#if defined(__TINYC__)\n"
	"static TYG_UNUSED TYG_F80 tyg_ld_get(uint64_t p) { return *(TYG_F80 *)(uintptr_t)p; }\n"
	"static TYG_UNUSED void tyg_ld_set(uint64_t p, TYG_F80 x) { *(TYG_F80 *)(uintptr_t)p = x; }\n"
	"#else\n"
	"static TYG_UNUSED TYG_F80 tyg_ld_get(uint64_t p) { TYG_F80 x; memcpy(&x, (void *)(uintptr_t)p, sizeof(x)); return x; }\n"
	"static TYG_UNUSED void tyg_ld_set(uint64_t p, TYG_F80 x) { memcpy((void *)(uintptr_t)p, &x, sizeof(x)); }\n"
	"#endif\n";

// The names of the tgRmwOp operations in the TYG_ATOMIC_<name>_<bits> macros.
static char const *const tg_c_rmw_names[] = {"XCHG", "FETCH_ADD", "FETCH_SUB", "FETCH_AND", "FETCH_OR", "FETCH_XOR", "FETCH_NAND"};

// Prints the sequentially consistent atomics, for each width N in bits: TYG_ATOMIC_LOAD_N(p),
// TYG_ATOMIC_STORE_N(p, v), TYG_ATOMIC_<rmw>_N(p, v) returning the old value,
// TYG_ATOMIC_CAS_N(p, expected, desired, &ok) returning the old value, and TYG_FENCE().
// p is a uintN_t *. MSVC has a nand, a load and a sub only through other operations.
TG_INTERNAL void tg_c_atomics_prelude(tgBytes *b) {
	static char const *const gnu[] = {"__atomic_exchange_n", "__atomic_fetch_add", "__atomic_fetch_sub", "__atomic_fetch_and", "__atomic_fetch_or", "__atomic_fetch_xor", "__atomic_fetch_nand"};
	static char const *const ms[] = {"_InterlockedExchange", "_InterlockedExchangeAdd", "_InterlockedExchangeAdd", "_InterlockedAnd", "_InterlockedOr", "_InterlockedXor", NULL};
	static char const *const ms_suffix[] = {"8", "16", "", "64"}, *const ms_type[] = {"char", "short", "long", "__int64"};
	// TinyCC has the __atomic_ builtins of <stdatomic.h> and the fetch ones, not __atomic_exchange_n
	tg_c_puts(b, "#if defined(__TINYC__)\n#include <stdatomic.h>\n"
	             "#define __atomic_exchange_n(p, v, o) atomic_exchange_explicit((p), (v), (o))\n#endif\n");
	tg_c_puts(b, "#if defined(__GNUC__) || defined(__clang__)\n#define TYG_FENCE() __atomic_thread_fence(__ATOMIC_SEQ_CST)\n");
	for (int w = 0; w < 4; w++) {
		int n = 8 << w;
		tg_c_printf(b, "#define TYG_ATOMIC_LOAD_%d(p) __atomic_load_n((p), __ATOMIC_SEQ_CST)\n", n);
		tg_c_printf(b, "#define TYG_ATOMIC_STORE_%d(p, v) __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)\n", n);
		for (int op = 0; op < 7; op++) tg_c_printf(b, "#define TYG_ATOMIC_%s_%d(p, v) %s((p), (v), __ATOMIC_SEQ_CST)\n", tg_c_rmw_names[op], n, gnu[op]);
		tg_c_printf(b, "static TYG_UNUSED uint%d_t tyg_cas%d(uint%d_t *p, uint%d_t e, uint%d_t d, uint8_t *ok) {\n"
		               "\t*ok = (uint8_t)__atomic_compare_exchange_n(p, &e, d, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);\n\treturn e;\n}\n", n, n, n, n, n);
	}
	tg_c_puts(b, "#elif defined(_MSC_VER)\n#include <intrin.h>\n#if defined(_M_ARM64)\n#define TYG_FENCE() __dmb(_ARM64_BARRIER_ISH)\n"
	             "#else\n#define TYG_FENCE() __faststorefence()\n#endif\n");
	for (int w = 0; w < 4; w++) {
		int n = 8 << w;
		char const *sx = ms_suffix[w], *t = ms_type[w];
		tg_c_printf(b, "#define TYG_ATOMIC_LOAD_%d(p) ((uint%d_t)_InterlockedOr%s((%s volatile *)(p), 0))\n", n, n, sx, t);
		tg_c_printf(b, "#define TYG_ATOMIC_STORE_%d(p, v) ((void)_InterlockedExchange%s((%s volatile *)(p), (%s)(v)))\n", n, sx, t, t);
		for (int op = 0; op < 6; op++) {
			char const *neg = op == tgRmw_Sub ? "0u - " : "";
			tg_c_printf(b, "#define TYG_ATOMIC_%s_%d(p, v) ((uint%d_t)%s%s((%s volatile *)(p), (%s)(%s(v))))\n", tg_c_rmw_names[op], n, n, ms[op], sx, t, t, neg);
		}
		tg_c_printf(b, "static __inline uint%d_t tyg_nand%d(uint%d_t *p, uint%d_t v) {\n"
		               "\tuint%d_t o = *(volatile uint%d_t *)p, w;\n"
		               "\twhile ((w = (uint%d_t)_InterlockedCompareExchange%s((%s volatile *)p, (%s)(uint%d_t)~(o & v), (%s)o)) != o) o = w;\n"
		               "\treturn o;\n}\n", n, n, n, n, n, n, n, sx, t, t, n, t);
		tg_c_printf(b, "#define TYG_ATOMIC_FETCH_NAND_%d(p, v) tyg_nand%d((p), (v))\n", n, n);
		tg_c_printf(b, "static __inline uint%d_t tyg_cas%d(uint%d_t *p, uint%d_t e, uint%d_t d, uint8_t *ok) {\n"
		               "\tuint%d_t o = (uint%d_t)_InterlockedCompareExchange%s((%s volatile *)p, (%s)d, (%s)e);\n"
		               "\t*ok = o == e;\n\treturn o;\n}\n", n, n, n, n, n, n, n, sx, t, t, t);
	}
	tg_c_puts(b, "#endif\n");
	for (int w = 0; w < 4; w++) tg_c_printf(b, "#define TYG_ATOMIC_CAS_%d(p, e, d, ok) tyg_cas%d((p), (e), (d), (ok))\n", 8 << w, 8 << w);
}

////////////////////////////////////////////////////////////////
// Names
////////////////////////////////////////////////////////////////

// Names a symbol may not have in the C: keywords, and what the prelude's headers declare,
// with the POSIX names glibc and macOS add to them by default.
static char const *const tg_c_reserved[] = {
	"auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else",
	"enum", "extern", "float", "for", "goto", "if", "inline", "int", "long", "register",
	"restrict", "return", "short", "signed", "sizeof", "static", "struct", "switch", "typedef",
	"union", "unsigned", "void", "volatile", "while", "asm", "typeof", "alignas", "alignof",
	"bool", "true", "false", "nullptr", "static_assert", "thread_local", "constexpr",
	"offsetof", "va_start", "va_arg", "va_end", "va_copy", "va_list", "unix", "linux", // GNU modes predefine the last two
	"memcpy", "memmove", "memset", "memcmp", "memchr", "memccpy", "strcpy", "strncpy", "strcat",
	"strncat", "strcmp", "strncmp", "strcoll", "strxfrm", "strchr", "strrchr", "strspn",
	"strcspn", "strpbrk", "strstr", "strtok", "strtok_r", "strlen", "strnlen", "strerror",
	"strdup", "strndup", "stpcpy", "strcasecmp", "strncasecmp", "index", "rindex", "bzero",
	"bcopy", "bcmp", "ffs",
	"atof", "atoi", "atol", "atoll", "strtod", "strtof", "strtold", "strtol", "strtoll",
	"strtoul", "strtoull", "rand", "srand", "random", "srandom", "calloc", "free", "malloc",
	"realloc", "aligned_alloc", "posix_memalign", "alloca", "abort", "atexit", "exit",
	"quick_exit", "at_quick_exit", "getenv", "setenv", "unsetenv", "putenv", "system",
	"bsearch", "qsort", "abs", "labs", "llabs", "div", "ldiv", "lldiv", "mblen", "mbtowc",
	"wctomb", "mbstowcs", "wcstombs", "mkstemp", "realpath",
	"fpclassify", "isfinite", "isinf", "isnan", "isnormal", "signbit", "isgreater",
	"isgreaterequal", "isless", "islessequal", "islessgreater", "isunordered",
	"j0", "j1", "jn", "y0", "y1", "yn", "gamma", "finite", "drem", "significand",
};

// math.h functions, which also exist with an f and an l suffix.
static char const *const tg_c_reserved_math[] = {
	"acos", "asin", "atan", "atan2", "cos", "sin", "tan", "acosh", "asinh", "atanh", "cosh",
	"sinh", "tanh", "exp", "exp2", "expm1", "frexp", "ilogb", "ldexp", "log", "log10", "log1p",
	"log2", "logb", "modf", "scalbn", "scalbln", "cbrt", "fabs", "hypot", "pow", "sqrt", "erf",
	"erfc", "lgamma", "tgamma", "ceil", "floor", "nearbyint", "rint", "lrint", "llrint",
	"round", "lround", "llround", "trunc", "fmod", "remainder", "remquo", "copysign", "nan",
	"nextafter", "nexttoward", "fdim", "fmax", "fmin", "fma",
};

TG_INTERNAL bool tg_c_in_list(tgString name, char const *const *list, isize count) {
	for (isize i = 0; i < count; i++) {
		if (tg_str_eq(name, tg_str(list[i]))) return true;
	}
	return false;
}

// Whether a symbol keeps its name in the C: a C identifier that no keyword, header or
// prelude macro has, and none of the names later steps give values, blocks and
// parameters (v1, l1, B1, p1).
TG_INTERNAL bool tg_c_name_ok(tgString name) {
	if (name.len == 0 || (name.text[0] >= '0' && name.text[0] <= '9')) return false;
	bool has_lower = false;
	bool digits_after_first = name.len > 1;
	for (isize i = 0; i < name.len; i++) {
		char c = name.text[i];
		bool lower = c >= 'a' && c <= 'z', digit = c >= '0' && c <= '9';
		if (!lower && !digit && !(c >= 'A' && c <= 'Z') && c != '_') return false;
		has_lower |= lower;
		if (i > 0 && !digit) digits_after_first = false;
	}
	if (!has_lower) return false; // the headers' macros, like INT8_MAX
	if (tg_str_has_prefix(name, tg_str_lit("tyg_")) || tg_str_has_prefix(name, tg_str_lit("TYG_"))) return false;
	if (name.len >= 2 && name.text[name.len - 2] == '_' && name.text[name.len - 1] == 't') return false;
	if (digits_after_first && strchr("vlBp", name.text[0]) != NULL) return false;
	if (tg_c_in_list(name, tg_c_reserved, tg_count_of(tg_c_reserved))) return false;
	tgString stem = name;
	if (name.text[name.len - 1] == 'f' || name.text[name.len - 1] == 'l') stem.len--;
	return !tg_c_in_list(name, tg_c_reserved_math, tg_count_of(tg_c_reserved_math)) &&
	       !tg_c_in_list(stem, tg_c_reserved_math, tg_count_of(tg_c_reserved_math));
}

// The symbol's name in the C: its own, or tyg_s<index> with the real name as its asm label.
// A debug type's typedef or enumerator took a name only no symbol had then.
TG_INTERNAL tgString tg_c_sym_name(tgModule *m, i32 sym) {
	if (m->c_names.count < m->symbols.count) tg_array_resize(&m->c_names, m->symbols.count);
	tgString *name = &m->c_names.data[sym];
	if (name->len == 0) {
		*name = m->symbols.data[sym].name;
		if (!tg_c_name_ok(*name) || tg_strmap_get(&m->c_dnames, *name) != NULL) {
			char buf[32];
			snprintf(buf, sizeof(buf), "tyg_s%d", sym);
			*name = tg_module_string(m, tg_str(buf));
		}
	}
	return *name;
}

// A C string literal of s.
TG_INTERNAL void tg_c_string(tgBytes *b, tgString s) {
	tg_array_add(b, (u8)'"');
	for (isize i = 0; i < s.len; i++) {
		u8 c = (u8)s.text[i];
		if (c == '"' || c == '\\') tg_c_printf(b, "\\%c", c);
		else if (c < 0x20 || c >= 0x7f || c == '?') tg_c_printf(b, "\\%03o", c); // '?': no trigraphs
		else tg_array_add(b, c);
	}
	tg_array_add(b, (u8)'"');
}

// Prints the symbol's name, then its asm label when the C name is not its own. A function
// definition takes no label: its prototype has it.
TG_INTERNAL void tg_c_sym_declarator(tgModule *m, tgBytes *b, i32 sym, char const *suffix, bool label) {
	tgString name = tg_c_sym_name(m, sym);
	tg_c_printf(b, "%s%s", name.text, suffix);
	if (label && name.text != m->symbols.data[sym].name.text) {
		tg_c_puts(b, " TYG_ASM_NAME(");
		tg_c_string(b, m->symbols.data[sym].name);
		tg_c_puts(b, ")");
	}
}

// Records sym's signature for its prototype. The first one stays, unless `def`: the
// definition's replaces it, so that the prototype and the definition agree.
TG_INTERNAL void tg_c_sym_sig_set(tgModule *m, i32 sym, tyg_sig const *sig, bool def) {
	if (m->c_sigs.count < m->symbols.count) tg_array_resize(&m->c_sigs, m->symbols.count);
	if (m->c_sigs.data[sym] == NULL || def) m->c_sigs.data[sym] = sig;
}

TG_INTERNAL tyg_sig const *tg_c_sym_sig(tgModule *m, i32 sym) {
	return sym < m->c_sigs.count ? m->c_sigs.data[sym] : NULL;
}

////////////////////////////////////////////////////////////////
// Types
////////////////////////////////////////////////////////////////

TG_INTERNAL i32 tg_c_record(tgModule *m, tyg_type const *t);

// Prints `type name`, or the type alone for an empty name. A record or an array is a
// struct tyg_r<N>, defined in m->c_types.
TG_INTERNAL void tg_c_type_decl(tgModule *m, tgBytes *b, tyg_type const *t, char const *name) {
	char const *space = name[0] ? " " : "";
	if (t == &tg_t_f80_apple) t = &tg_t_f80; // a record of one double: the long double it stands for
	switch (t->kind) {
	case tgT_Void: tg_c_printf(b, "void%s%s", space, name); break;
	case tgT_Int:  tg_c_printf(b, "%sint%d_t%s%s", t->is_signed ? "" : "u", t->bits, space, name); break;
	case tgT_F32:  tg_c_printf(b, "float%s%s", space, name); break;
	case tgT_F64:  tg_c_printf(b, "double%s%s", space, name); break;
	case tgT_Ptr:  tg_c_printf(b, "void *%s", name); break;
	case tgT_F80:  tg_c_printf(b, "TYG_F80%s%s", space, name); break;
	case tgT_Array:
	case tgT_Record: tg_c_printf(b, "struct tyg_r%d%s%s", tg_c_record(m, t), space, name); break;
	}
}

// A member of a struct. An array member keeps its dimensions, as C lays them out.
TG_INTERNAL void tg_c_member(tgModule *m, tgBytes *b, tyg_type const *t, char const *name) {
	tyg_type const *elem = t;
	while (elem->kind == tgT_Array) elem = elem->elem;
	tg_c_puts(b, "\t");
	tg_c_type_decl(m, b, elem, name);
	for (; t->kind == tgT_Array; t = t->elem) tg_c_printf(b, "[%lld]", (long long)t->count);
	tg_c_puts(b, ";\n");
}

// The fields of a laid out record, in offset order.
TG_INTERNAL i32 *tg_c_fields_by_offset(tyg_type const *t) {
	i32 *order = (i32 *)malloc(sizeof(i32) * (usize)(t->field_count + 1));
	TG_ASSERT(order != NULL);
	for (i32 i = 0; i < t->field_count; i++) {
		i32 j = i;
		for (; j > 0 && t->fields[order[j - 1]].offset > t->fields[i].offset; j--) order[j] = order[j - 1];
		order[j] = i;
	}
	return order;
}

// The number N of the struct tyg_r<N> for a record or an array, which defines it in
// m->c_types after the structs of its fields. A record whose fields sit where C puts them
// is a plain struct, so the C compiler classifies it like the ABI code does. Any other
// is packed, with explicit padding and its alignment; overlapping fields go into a union
// of packed structs, one per lane of fields that do not overlap.
TG_INTERNAL i32 tg_c_record(tgModule *m, tyg_type const *t) {
	i64 *found = tg_map_get(&m->c_type_ids, (u64)(uintptr_t)t);
	if (found != NULL) return (i32)*found;
	i32 n = (i32)m->c_type_ids.count;
	tg_map_set(&m->c_type_ids, (u64)(uintptr_t)t, n);
	tgBytes b;
	memset(&b, 0, sizeof(b));
	if (t->kind == tgT_Array) {
		tg_c_printf(&b, "struct tyg_r%d {\n", n);
		tg_c_member(m, &b, t, "a");
		tg_c_puts(&b, "};\n");
	} else {
		i32 *order = tg_c_fields_by_offset(t);
		i32 *lane = (i32 *)malloc(sizeof(i32) * (usize)(t->field_count + 1));
		i64 *lane_end = (i64 *)malloc(sizeof(i64) * (usize)(t->field_count + 1));
		TG_ASSERT(lane != NULL && lane_end != NULL);
		bool natural = true;
		i64 end = 0, max_align = 1;
		i32 lanes = 0;
		for (i32 k = 0; k < t->field_count; k++) {
			tyg_field const *f = &t->fields[order[k]];
			lane[k] = -1;
			if (f->type->size == 0) continue;
			if (f->offset != tg_align_up(end, f->type->align)) natural = false;
			end = f->offset + f->type->size;
			max_align = tg_max(max_align, f->type->align);
			i32 l = 0;
			while (l < lanes && lane_end[l] > f->offset) l++;
			if (l == lanes) lanes++;
			lane[k] = l;
			lane_end[l] = f->offset + f->type->size;
		}
		natural = natural && t->align >= max_align && t->size == tg_align_up(end, t->align);
		if (!natural) tg_c_puts(&b, "#pragma pack(push, 1)\n");
		tg_c_puts(&b, "struct ");
		if (!natural || t->align > max_align) tg_c_printf(&b, "TYG_ALIGNED(%lld) ", (long long)t->align);
		tg_c_printf(&b, "tyg_r%d {\n", n);
		if (lanes > 1) tg_c_puts(&b, "union {\n");
		i32 pads = 0;
		for (i32 l = 0; l < lanes || (l == 0 && !natural); l++) {
			if (lanes > 1) tg_c_puts(&b, "struct {\n");
			i64 at = 0;
			for (i32 k = 0; k < t->field_count; k++) {
				tyg_field const *f = &t->fields[order[k]];
				if (lane[k] != l) continue;
				if (!natural && f->offset > at) tg_c_printf(&b, "\tunsigned char tyg_pad%d[%lld];\n", pads++, (long long)(f->offset - at));
				char name[32];
				snprintf(name, sizeof(name), "f%d", order[k]);
				tg_c_member(m, &b, f->type, name);
				at = f->offset + f->type->size;
			}
			if (!natural && t->size > at) tg_c_printf(&b, "\tunsigned char tyg_pad%d[%lld];\n", pads++, (long long)(t->size - at));
			if (lanes > 1) tg_c_printf(&b, "} l%d;\n", l);
		}
		if (lanes > 1) tg_c_puts(&b, "} u;\n");
		tg_c_puts(&b, "};\n");
		if (!natural) tg_c_puts(&b, "#pragma pack(pop)\n");
		free(order);
		free(lane);
		free(lane_end);
	}
	// reading a struct out of a byte local through a cast breaks C's aliasing rules, so
	// values move through memcpy, which the compilers turn into plain moves
	tg_c_printf(&b, "#if defined(__TINYC__)\n"
	                "static TYG_UNUSED struct tyg_r%d tyg_r%d_get(char const *p) { return *(struct tyg_r%d const *)p; }\n"
	                "static TYG_UNUSED void tyg_r%d_set(char *p, struct tyg_r%d x) { *(struct tyg_r%d *)p = x; }\n"
	                "#else\n", n, n, n, n, n, n);
	tg_c_printf(&b, "static TYG_UNUSED struct tyg_r%d tyg_r%d_get(char const *p) { struct tyg_r%d x; memcpy(&x, p, sizeof(x)); return x; }\n", n, n, n);
	tg_c_printf(&b, "static TYG_UNUSED void tyg_r%d_set(char *p, struct tyg_r%d x) { memcpy(p, &x, sizeof(x)); }\n#endif\n", n, n);
	tg_array_add_elems(&m->c_types, b.data, b.count);
	tg_array_free(&b);
	return n;
}

////////////////////////////////////////////////////////////////
// Debug types
//
// The debug types of the named locals become declarations in m->c_dtypes, so that the C
// compiler's debug info has the source's types. A type has a C form when C lays it out at
// the debug info's size and member offsets; a pointer to one without a form points to an
// incomplete struct, or is a void *. A qualifier is printed under a pointer only: the C
// writes the objects it declares.
////////////////////////////////////////////////////////////////

enum {
	tgCDt_Known    = 1,  // flags Form and Enum, size and align are set
	tgCDt_Form     = 2,  // it has a C form
	tgCDt_Enum     = 4,  // an enum in the C, with its enumerators
	tgCDt_Declared = 8,  // its head and what that names are in m->c_dtypes
	tgCDt_Defined  = 16, // a record: complete in m->c_dtypes
	tgCDt_Busy     = 32, // being laid out or defined
};

// See tg_c_debug_names, which sizes m->c_dts.
TG_INTERNAL tgCDtype *tg_c_dt(tgModule *m, tyg_dtype const *t) {
	return &m->c_dts.data[t->index];
}

// The C type of a base type and its alignment, or NULL.
TG_INTERNAL char const *tg_c_dt_base(tgModule *m, tyg_dtype const *t, i64 *align) {
	static char const *const ints[2][4] = {{"int8_t", "int16_t", "int32_t", "int64_t"}, {"uint8_t", "uint16_t", "uint32_t", "uint64_t"}};
	i64 s = t->size;
	int w = s == 1 ? 0 : s == 2 ? 1 : s == 4 ? 2 : s == 8 ? 3 : -1;
	*align = s;
	switch (t->encoding) {
	case TYG_DT_FLOAT: {
		tyg_type const *f80 = tyg_type_scalar(m, TYG_F80);
		if (s == 4 || s == 8) return s == 4 ? "float" : "double";
		*align = f80->align;
		return s == f80->size && m->target.os != TYG_OS_WINDOWS ? "TYG_F80" : NULL; // see the prelude
	}
	case TYG_DT_SIGNED_CHAR:   if (s == 1) return "signed char"; break;
	case TYG_DT_UNSIGNED_CHAR: if (s == 1) return "unsigned char"; break;
	case TYG_DT_BOOL:          if (s == 1) return "_Bool"; break;
	}
	return w < 0 ? NULL : ints[t->encoding != TYG_DT_SIGNED && t->encoding != TYG_DT_SIGNED_CHAR][w];
}

// Lays t out like C does, as tg_c_record checks a record; returns whether it has a C form.
// A record has one when its members, which are not bitfields, sit where C puts them.
TG_INTERNAL bool tg_c_dt_form(tgModule *m, tyg_dtype const *t) {
	if (t == NULL) return false;
	tgCDtype *c = tg_c_dt(m, t);
	if (c->flags & (tgCDt_Known | tgCDt_Busy)) return (c->flags & tgCDt_Form) != 0; // Busy: a record holding itself
	c->flags |= tgCDt_Busy;
	i64 size = 0, align = 1;
	bool form = false, is_enum = false;
	switch (t->kind) {
	case tgDt_Base: form = tg_c_dt_base(m, t, &align) != NULL; size = t->size; break;
	case tgDt_Pointer: form = true; size = align = 8; break;
	case tgDt_Const:
	case tgDt_Volatile:
	case tgDt_Typedef:
	case tgDt_Enum:
	case tgDt_Array:
		form = tg_c_dt_form(m, t->target) && (t->kind != tgDt_Array || t->count > 0);
		if (!form) break;
		size = tg_c_dt(m, t->target)->size * (t->kind == tgDt_Array ? t->count : 1);
		align = tg_c_dt(m, t->target)->align;
		is_enum = t->kind == tgDt_Enum && size == 4 && t->enumerators.count > 0;
		for (isize i = 0; is_enum && i < t->enumerators.count; i++) is_enum = t->enumerators.data[i].value >= INT32_MIN && t->enumerators.data[i].value <= INT32_MAX;
		break;
	case tgDt_Record: {
		bool is_union = t->record_kind == TYG_DT_UNION;
		form = t->complete && t->members.count > 0;
		for (isize i = 0; form && i < t->members.count; i++) {
			tgDtMember const *f = &t->members.data[i];
			form = f->bit_size == 0 && tg_c_dt_form(m, f->type);
			if (!form) break;
			tgCDtype const *ft = tg_c_dt(m, f->type);
			form = f->offset == (is_union ? 0 : tg_align_up(size, ft->align));
			size = is_union ? tg_max(size, ft->size) : f->offset + ft->size;
			align = tg_max(align, ft->align);
		}
		form = form && t->align == align && t->size == tg_align_up(size, align);
		size = t->size;
		break;
	}
	}
	c->flags = (u8)((c->flags & ~tgCDt_Busy) | tgCDt_Known | (form ? tgCDt_Form : 0) | (is_enum ? tgCDt_Enum : 0));
	c->size = size;
	c->align = align;
	return form;
}

// Whether t can be named in the C, as a pointer's target, a typedef's or a function type's
// part: a type with a C form, void, a record (incomplete without a form) or made of those.
TG_INTERNAL bool tg_c_dt_spellable(tgModule *m, tyg_dtype const *t) {
	if (t == NULL) return true; // a function's void result
	switch (t->kind) {
	case tgDt_Void:
	case tgDt_Record:
	case tgDt_Pointer:
		return true;
	case tgDt_Const:
	case tgDt_Volatile:
	case tgDt_Typedef:
		return tg_c_dt_spellable(m, t->target);
	case tgDt_Array:
		return tg_c_dt_form(m, t->target);
	case tgDt_Func:
		for (i32 i = 0; i < t->param_count; i++) {
			if (t->params[i]->kind == tgDt_Void || !tg_c_dt_spellable(m, t->params[i])) return false;
		}
		return tg_c_dt_spellable(m, t->target);
	}
	return tg_c_dt_form(m, t);
}

// Takes `name` in `names`, m->c_dnames or m->c_dtags, when it is a usable C name that is
// free there; a file scope name must also not be a symbol's.
TG_INTERNAL bool tg_c_dt_take(tgModule *m, tgStrMap *names, tgString name) {
	if (!tg_c_name_ok(name) || tg_strmap_get(names, name) != NULL) return false;
	if (names == &m->c_dnames && tg_strmap_get(&m->symbol_map, name) != NULL) return false;
	tg_strmap_set(names, name, 1);
	return true;
}

// Sets t's head to `kw` and its source name when it can take that, else tyg_t<index>. kw
// is "struct ", "union " or "enum " for a tag, "" for a typedef.
TG_INTERNAL void tg_c_dt_name(tgModule *m, tyg_dtype const *t, char const *kw) {
	tgBytes s;
	memset(&s, 0, sizeof(s));
	if (tg_c_dt_take(m, kw[0] ? &m->c_dtags : &m->c_dnames, t->name)) tg_c_printf(&s, "%s%s", kw, t->name.text);
	else tg_c_printf(&s, "%styg_t%d", kw, t->index);
	tg_c_dt(m, t)->head = tg_module_string(m, tg_str_make((char const *)s.data, s.count));
	tg_array_free(&s);
}

// Prints `inner`, a declarator, declared as t, which tg_c_dt_declare declared. A qualifier
// counts only with `qualify`, under a pointer.
TG_INTERNAL void tg_c_dt_decl(tgModule *m, tgBytes *b, tyg_dtype const *t, char const *inner, bool qualify) {
	char const *sp = inner[0] ? " " : "";
	tyg_dtype const *to;
	tgBytes d;
	memset(&d, 0, sizeof(d));
	if (t == NULL) {
		tg_c_printf(b, "void%s%s", sp, inner);
		return;
	}
	switch (t->kind) {
	case tgDt_Pointer:
		if (!tg_c_dt_spellable(m, t->target)) {
			tg_c_printf(b, "void *%s", inner);
			return;
		}
		for (to = t->target; to->kind == tgDt_Const || to->kind == tgDt_Volatile; to = to->target) {}
		tg_c_printf(&d, to->kind == tgDt_Array || to->kind == tgDt_Func ? "(*%s)" : "*%s", inner);
		qualify = true;
		break;
	case tgDt_Const:
	case tgDt_Volatile:
		if (qualify && t->target->kind != tgDt_Func) tg_c_printf(&d, "%s%s%s", t->kind == tgDt_Const ? "const" : "volatile", sp, inner);
		else tg_c_puts(&d, inner);
		break;
	case tgDt_Array:
		if (t->count >= 0) tg_c_printf(&d, "%s[%lld]", inner, (long long)t->count);
		else tg_c_printf(&d, "%s[]", inner);
		break;
	case tgDt_Func:
		tg_c_printf(&d, "%s(", inner);
		for (i32 i = 0; i < t->param_count; i++) {
			if (i > 0) tg_c_puts(&d, ", ");
			tg_c_dt_decl(m, &d, t->params[i], "", false);
		}
		if (t->param_count == 0 && !t->variadic) tg_c_puts(&d, "void");
		else if (t->variadic && t->param_count > 0) tg_c_puts(&d, ", ...");
		tg_c_puts(&d, ")");
		qualify = false;
		break;
	default:
		if (tg_c_dt(m, t)->head.len > 0) {
			tg_c_printf(b, "%s%s%s", tg_c_dt(m, t)->head.text, sp, inner);
			return;
		}
		tg_c_puts(&d, inner); // an enum without its own name: its underlying type
		break;
	}
	tg_array_add(&d, (u8)0);
	tg_c_dt_decl(m, b, t->target, (char const *)d.data, qualify);
	tg_array_free(&d);
}

TG_INTERNAL void tg_c_dt_define(tgModule *m, tyg_dtype const *t);

// Makes t nameable in the C: appends what its spelling names to m->c_dtypes. A record
// that is not complete there yet is declared.
TG_INTERNAL void tg_c_dt_declare(tgModule *m, tyg_dtype const *t) {
	if (t == NULL) return;
	tgCDtype *c = tg_c_dt(m, t);
	tgBytes *out = &m->c_dtypes;
	if (t->kind == tgDt_Record) {
		if (c->head.len == 0) tg_c_dt_name(m, t, t->record_kind == TYG_DT_UNION ? "union " : "struct ");
		if (!(c->flags & (tgCDt_Declared | tgCDt_Defined))) tg_c_printf(out, "%s;\n", c->head.text);
		c->flags |= tgCDt_Declared;
		return;
	}
	if (c->flags & tgCDt_Declared) return;
	c->flags |= tgCDt_Declared;
	tg_c_dt_form(m, t);
	switch (t->kind) {
	case tgDt_Void: c->head = tg_str_lit("void"); break;
	case tgDt_Base: {
		i64 align;
		char const *ct = tg_c_dt_base(m, t, &align);
		if (ct == NULL) break;
		if (tg_c_dt_take(m, &m->c_dnames, t->name)) {
			c->head = t->name;
			tg_c_printf(out, "typedef %s %s;\n", ct, t->name.text);
		} else {
			c->head = tg_str(ct);
		}
		break;
	}
	case tgDt_Pointer:
		if (!tg_c_dt_spellable(m, t->target)) break;
		tg_c_dt_declare(m, t->target); // a record before its first use, which may be in a prototype
		tg_c_dt_define(m, t->target);
		break;
	case tgDt_Const:
	case tgDt_Volatile:
		tg_c_dt_declare(m, t->target);
		break;
	case tgDt_Array:
		tg_c_dt_define(m, t->target);
		break;
	case tgDt_Func:
		tg_c_dt_declare(m, t->target);
		for (i32 i = 0; i < t->param_count; i++) tg_c_dt_declare(m, t->params[i]);
		break;
	case tgDt_Typedef:
		tg_c_dt_declare(m, t->target);
		tg_c_dt_name(m, t, "");
		tg_c_puts(out, "typedef ");
		tg_c_dt_decl(m, out, t->target, c->head.text, false);
		tg_c_puts(out, ";\n");
		break;
	case tgDt_Enum:
		if (!(c->flags & tgCDt_Enum)) {
			// not an int: its underlying type, named when it can be
			tg_c_dt_declare(m, t->target);
			if (!tg_c_dt_form(m, t) || !tg_c_dt_take(m, &m->c_dnames, t->name)) break;
			c->head = t->name;
			tg_c_puts(out, "typedef ");
			tg_c_dt_decl(m, out, t->target, c->head.text, false);
			tg_c_puts(out, ";\n");
			break;
		}
		tg_c_dt_name(m, t, "enum ");
		tg_c_printf(out, "%s {\n", c->head.text);
		for (isize i = 0; i < t->enumerators.count; i++) {
			tgDtEnumerator const *e = &t->enumerators.data[i];
			if (tg_c_dt_take(m, &m->c_dnames, e->name)) tg_c_printf(out, "\t%s", e->name.text);
			else tg_c_printf(out, "\ttyg_t%d_%d", t->index, (int)i);
			tg_c_printf(out, " = %lld,\n", (long long)e->value);
		}
		tg_c_puts(out, "};\n");
		break;
	}
}

// Makes t complete in the C, with every type it holds; a record with a C form is defined
// after the types of its members and their targets.
TG_INTERNAL void tg_c_dt_define(tgModule *m, tyg_dtype const *t) {
	if (t == NULL) return;
	tgCDtype *c = tg_c_dt(m, t);
	if (t->kind != tgDt_Record || !tg_c_dt_form(m, t)) {
		tg_c_dt_declare(m, t);
		if (t->kind == tgDt_Const || t->kind == tgDt_Volatile || t->kind == tgDt_Typedef) tg_c_dt_define(m, t->target);
		return;
	}
	if (c->flags & (tgCDt_Defined | tgCDt_Busy)) return;
	c->flags |= tgCDt_Busy;
	if (c->head.len == 0) tg_c_dt_name(m, t, t->record_kind == TYG_DT_UNION ? "union " : "struct ");
	for (isize i = 0; i < t->members.count; i++) tg_c_dt_define(m, t->members.data[i].type);
	tgBytes b;
	memset(&b, 0, sizeof(b));
	tg_c_printf(&b, "%s {\n", c->head.text);
	for (isize i = 0; i < t->members.count; i++) {
		tgDtMember const *f = &t->members.data[i];
		bool ok = tg_c_name_ok(f->name);
		for (isize j = 0; ok && j < i; j++) ok = !tg_str_eq(f->name, t->members.data[j].name);
		char name[32];
		snprintf(name, sizeof(name), "tyg_m%d", (int)i);
		tg_c_puts(&b, "\t");
		tg_c_dt_decl(m, &b, f->type, ok ? f->name.text : name, false);
		tg_c_puts(&b, ";\n");
	}
	tg_c_puts(&b, "};\n");
	tg_array_add_elems(&m->c_dtypes, b.data, b.count);
	tg_array_free(&b);
	c->flags = (u8)((c->flags & ~tgCDt_Busy) | tgCDt_Defined);
}

////////////////////////////////////////////////////////////////
// Symbols
////////////////////////////////////////////////////////////////

TG_INTERNAL bool tg_c_is_func(tgModule *m, i32 sym) {
	tgSymbol const *s = &m->symbols.data[sym];
	return (s->flags & tgSymbolFlag_Func) || s->section == tgSection_Text || tg_c_sym_sig(m, sym) != NULL;
}

// Whether the symbol's data is const: read-only data.
TG_INTERNAL bool tg_c_is_const(tgModule *m, tgSection sec) {
	if (sec < tgSection_COUNT) return sec == tgSection_Rodata;
	return (tg_named_section(m, sec)->flags & (TYG_SECF_WRITE | TYG_SECF_TLS)) == 0;
}

// The storage class of a symbol's declaration: a symbol defined here is static unless it
// is global.
TG_INTERNAL char const *tg_c_storage(tgSymbol const *s, bool defined) {
	return defined && !(s->flags & tgSymbolFlag_Global) ? "static " : "extern ";
}

// Prints the attributes of a symbol's declaration and the storage class. Static data is
// kept like in an object, a static function is not warned about.
TG_INTERNAL void tg_c_attrs(tgModule *m, tgBytes *b, i32 sym, char const *storage) {
	tgSymbol const *s = &m->symbols.data[sym];
	if (s->flags & tgSymbolFlag_Weak) tg_c_puts(b, "TYG_WEAK ");
	// the visibility the object writers give: on ELF hidden, then protected, win over
	// export; Mach-O exports a hidden symbol; COFF exports definitions only
	bool elf = m->target.os == TYG_OS_LINUX;
	bool exported = (s->flags & tgSymbolFlag_Export) && (m->target.os != TYG_OS_WINDOWS || s->section != tgSection_Undef);
	bool hidden = (s->flags & tgSymbolFlag_Hidden) && (elf || !exported);
	if (hidden) tg_c_puts(b, "TYG_HIDDEN ");
	else if (elf && (s->flags & tgSymbolFlag_Protected)) tg_c_puts(b, "TYG_PROTECTED ");
	else if (exported) tg_c_puts(b, "TYG_EXPORT ");
	if (s->alias_of == 0 && s->section >= tgSection_COUNT) {
		tg_c_puts(b, "TYG_SECTION(");
		tg_c_string(b, tg_named_section(m, s->section)->name);
		tg_c_puts(b, ") ");
	}
	if (strcmp(storage, "static ") == 0) tg_c_puts(b, tg_c_is_func(m, sym) ? "TYG_UNUSED " : "TYG_USED ");
	tg_c_puts(b, storage);
}

// The C name of the parameter k of the procedure being printed: its source name, or p<k>.
TG_INTERNAL char const *tg_c_param_name(tgProc *p, i32 k, char buf[16]) {
	tgString name = p->c_names[p->locals.count + k];
	if (name.len > 0) return name.text;
	snprintf(buf, 16, "p%d", (int)k);
	return buf;
}

// Prints the parameter list of a signature, with the parameter names of `def`, if given.
TG_INTERNAL void tg_c_params(tgModule *m, tgBytes *b, tyg_sig const *sig, tgProc *def) {
	tg_c_puts(b, "(");
	if (sig == NULL || (sig->count == 0 && !(sig->flags & TYG_SIG_VARIADIC))) tg_c_puts(b, "void");
	for (i32 i = 0; sig != NULL && i < sig->count; i++) {
		char buf[16];
		if (i > 0) tg_c_puts(b, ", ");
		tg_c_type_decl(m, b, sig->params[i], def != NULL ? tg_c_param_name(def, i, buf) : "");
	}
	if (sig != NULL && (sig->flags & TYG_SIG_VARIADIC) && sig->count > 0) tg_c_puts(b, ", ...");
	tg_c_puts(b, ")");
	tg_array_add(b, (u8)0);
	b->count--; // zero terminated, for tg_c_type_decl
}

// Whether two types are one C type: records and arrays are by identity, see tg_c_record.
TG_INTERNAL bool tg_c_type_same(tyg_type const *a, tyg_type const *b) {
	if (a == b) return true;
	if (a == NULL || b == NULL || a->kind != b->kind || a->kind == tgT_Record || a->kind == tgT_Array) return false;
	return a->kind != tgT_Int || (a->bits == b->bits && a->is_signed == b->is_signed);
}

// Whether two signatures declare the same C function type.
TG_INTERNAL bool tg_c_sig_equal(tyg_sig const *a, tyg_sig const *b) {
	if (a == b) return true;
	if (a == NULL || b == NULL || a->count != b->count || ((a->flags ^ b->flags) & TYG_SIG_VARIADIC)) return false;
	if (!tg_c_type_same(a->ret, b->ret)) return false;
	for (i32 i = 0; i < a->count; i++) {
		if (!tg_c_type_same(a->params[i], b->params[i])) return false;
	}
	return true;
}

// Prints the prototype of a function symbol, without the semicolon, or with `def` the
// header of its definition, whose parameters are named.
TG_INTERNAL void tg_c_proto(tgModule *m, tgBytes *b, i32 sym, tgProc *def) {
	tgSymbol const *s = &m->symbols.data[sym];
	tyg_sig const *sig = tg_c_sym_sig(m, sym);
	if (sig == NULL && s->alias_of != 0) sig = tg_c_sym_sig(m, (i32)(tg_alias_target(m, sym) - m->symbols.data));
	if (sig != NULL && (sig->flags & TYG_SIG_NORETURN)) tg_c_puts(b, "TYG_NORETURN ");
	if (sig != NULL && (sig->flags & TYG_SIG_RETURNS_TWICE)) tg_c_puts(b, "TYG_RETURNS_TWICE ");
	char const *storage = tg_c_storage(s, s->section != tgSection_Undef);
	tg_c_attrs(m, b, sym, def && strcmp(storage, "extern ") == 0 ? "" : storage);
	tgBytes params, d;
	memset(&params, 0, sizeof(params));
	memset(&d, 0, sizeof(d));
	tg_c_params(m, &params, sig, def);
	tg_c_sym_declarator(m, &d, sym, (char const *)params.data, def == NULL);
	tg_array_add(&d, (u8)0);
	tg_c_type_decl(m, b, sig ? sig->ret : tyg_type_scalar(m, TYG_VOID), (char const *)d.data);
	tg_array_free(&params);
	tg_array_free(&d);
}

// Prints the declaration of a data symbol, without the semicolon. `data` is the symbol
// whose data it is, the target for an alias.
TG_INTERNAL void tg_c_data_decl(tgModule *m, tgBytes *b, i32 sym, i32 data, bool def) {
	tgSymbol const *s = &m->symbols.data[sym], *d = &m->symbols.data[data];
	bool defined = d->section != tgSection_Undef || d->common_align != 0;
	i64 align = d->common_align ? d->common_align : d->align;
	if (align > 1 && defined && s->alias_of == 0) tg_c_printf(b, "TYG_ALIGNED(%lld) ", (long long)align);
	char const *storage = tg_c_storage(s, defined);
	if (d->common_align != 0) {
		tg_c_puts(b, "TYG_COMMON ");
		storage = ""; // a common symbol is a tentative definition
	} else if (def && (s->flags & tgSymbolFlag_Global)) {
		storage = "";
	}
	tg_c_attrs(m, b, sym, storage);
	if (s->flags & tgSymbolFlag_TLS) tg_c_puts(b, "TYG_THREAD ");
	if (d->section != tgSection_Undef && tg_c_is_const(m, d->section)) tg_c_puts(b, "const ");
	char suffix[48];
	if (!defined) {
		tg_c_puts(b, "unsigned char ");
		snprintf(suffix, sizeof(suffix), "[]");
	} else if (d->common_align != 0 || tg_section_is_nobits(m, d->section)) {
		tg_c_puts(b, "unsigned char ");
		snprintf(suffix, sizeof(suffix), "[%lld]", (long long)tg_max(d->size, 1));
	} else {
		tg_c_printf(b, "struct tyg_d%d ", data);
		suffix[0] = 0;
	}
	tg_c_sym_declarator(m, b, sym, suffix, true);
}

// Prints the struct tyg_d<sym> of a symbol's initialized data and the data's initializer:
// byte runs, and pointers at the relocations.
TG_INTERNAL bool tg_c_data_struct(tgModule *m, tgBytes *types, tgBytes *init, i32 sym) {
	tgSymbol const *s = &m->symbols.data[sym];
	u8 const *bytes = tg_section_bytes(m, s->section)->data + s->offset;
	tgI32Array relocs;
	memset(&relocs, 0, sizeof(relocs));
	for (isize i = 0; i < m->relocs.count; i++) {
		tgReloc const *r = &m->relocs.data[i];
		if (r->section != s->section || r->offset < s->offset || r->offset >= s->offset + s->size) continue;
		isize j = relocs.count;
		tg_array_add(&relocs, (i32)i);
		for (; j > 0 && m->relocs.data[relocs.data[j - 1]].offset > r->offset; j--) relocs.data[j] = relocs.data[j - 1];
		relocs.data[j] = (i32)i;
	}
	tg_c_printf(types, "struct tyg_d%d {\n", sym);
	tg_c_puts(init, " = {");
	i64 at = 0;
	i32 runs = 0;
	bool ok = true;
	for (isize k = 0; k <= relocs.count; k++) {
		tgReloc const *r = k < relocs.count ? &m->relocs.data[relocs.data[k]] : NULL;
		i64 next = r != NULL ? r->offset - s->offset : tg_max(s->size, 1);
		if (next < at || (r != NULL && r->kind != tgReloc_Abs64)) {
			tg_set_error(m, "C output: overlapping or 4 byte relocations in %s", s->name.text);
			ok = false;
			break;
		}
		if (next > at) {
			tg_c_printf(types, "\tunsigned char b%d[%lld];\n", runs++, (long long)(next - at));
			bool zero = true;
			for (i64 i = at; i < next && i < s->size; i++) zero = zero && bytes[i] == 0;
			tg_c_puts(init, zero ? "\n\t{0}," : "\n\t{");
			for (i64 i = at; !zero && i < next; i++) {
				u8 c = i < s->size ? bytes[i] : 0;
				if ((i - at) % 16 == 0 && i > at) tg_c_puts(init, "\n\t ");
				u8 hex[5] = {'0', 'x', (u8)"0123456789abcdef"[c >> 4], (u8)"0123456789abcdef"[c & 15], ','};
				tg_array_add_elems(init, hex, 5);
			}
			if (!zero) {
				init->count--;
				tg_c_puts(init, "},");
			}
		}
		if (r == NULL) break;
		bool func = tg_c_is_func(m, r->sym);
		char const *target = tg_c_sym_name(m, r->sym).text;
		if (func && r->addend == 0) {
			tg_c_printf(types, "\tvoid (*p%d)(void);\n", (int)k);
			tg_c_printf(init, "\n\t(void (*)(void))&%s,", target);
		} else {
			tg_c_printf(types, "\tvoid *p%d;\n", (int)k);
			if (r->addend == 0) tg_c_printf(init, "\n\t(void *)&%s,", target);
			else tg_c_printf(init, "\n\t(void *)((char *)&%s + %lld),", target, (long long)r->addend);
		}
		at = next + 8;
	}
	init->count--; // the last comma
	tg_c_puts(init, "\n}");
	tg_c_puts(types, "};\n");
	tg_array_free(&relocs);
	return ok;
}

////////////////////////////////////////////////////////////////
// Procedures
//
// tyg_proc_end prints the builder's IR as one C function: every local is a byte array and
// every vreg a variable, both declared at the top since the blocks are labels that gotos
// reach. A vreg has an unsigned type, so that arithmetic wraps, and a pointer is a uint64_t.
////////////////////////////////////////////////////////////////

static char const *const tg_c_utypes[tgType_COUNT] = {"void", "uint8_t", "uint16_t", "uint32_t", "uint64_t", "float", "double", "void"};
static char const *const tg_c_itypes[tgType_COUNT] = {"void", "int8_t", "int16_t", "int32_t", "int64_t", "float", "double", "void"};

// The C operators of the tgCond conditions, signed or not by the casts around them.
static char const *const tg_c_conds[] = {"==", "!=", "<", "<=", ">", ">=", "<", "<=", ">", ">=", "==", "!=", "<", "<=", ">", ">="};

// Prints a memory operand as a char * expression.
TG_INTERNAL void tg_c_mem(tgProc *p, tgBytes *b, tgMem m) {
	char off[32] = "";
	if (m.offset != 0) snprintf(off, sizeof(off), " %c %lld", m.offset < 0 ? '-' : '+', m.offset < 0 ? -(long long)m.offset : (long long)m.offset);
	switch (m.kind) {
	case tgMem_Reg:   tg_c_printf(b, "((char *)(uintptr_t)v%u%s)", m.base, off); return;
	case tgMem_Local:
		if (p->c_names[m.base].len > 0) tg_c_printf(b, "((char *)&%s%s)", p->c_names[m.base].text, off);
		else tg_c_printf(b, "((char *)l%u%s)", m.base, off);
		return;
	case tgMem_Sym:
		// ISO C converts a function pointer to an integer, not to a data pointer
		tg_c_printf(b, tg_c_is_func(p->m, (i32)m.base) ? "((char *)(uintptr_t)&%s%s)" : "((char *)&%s%s)", tg_c_sym_name(p->m, (i32)m.base).text, off);
		return;
	}
	tg_fail(p, "C output: incoming stack memory is not supported");
}

// Fails the procedure on an op the C output does not print yet.
TG_INTERNAL void tg_c_unsupported(tgProc *p, tgOp op) {
	char const *name = op < tgOp_COUNT ? tg_op_names[op] : "?";
	usize n = strlen(name) + 40;
	char *msg = tg_alloc_items(char, n); // in the procedure's arena, which lives until the error is set
	snprintf(msg, n, "C output: %s is not supported yet", name);
	tg_fail(p, msg);
}

// Prints the aggregate or long double value of type t at memory m, read with memcpy.
TG_INTERNAL void tg_c_mem_val(tgProc *p, tgBytes *b, tyg_type const *t, tgMem m) {
	if (t->kind == tgT_F80 || t == &tg_t_f80_apple) {
		p->c_uses |= tgCUse_Ld;
		tg_c_puts(b, "tyg_ld_get((uint64_t)(uintptr_t)");
	} else {
		tg_c_printf(b, "tyg_r%d_get(", tg_c_record(p->m, t));
	}
	tg_c_mem(p, b, m);
	tg_c_puts(b, ")");
}

// Prints the start of a store of such a value to memory m: `tyg_rN_set(m, `; the caller
// prints the value and `)`.
TG_INTERNAL void tg_c_mem_set_begin(tgProc *p, tgBytes *b, tyg_type const *t, tgMem m) {
	if (t->kind == tgT_F80 || t == &tg_t_f80_apple) {
		p->c_uses |= tgCUse_Ld;
		tg_c_puts(b, "tyg_ld_set((uint64_t)(uintptr_t)");
	} else {
		tg_c_printf(b, "tyg_r%d_set(", tg_c_record(p->m, t));
	}
	tg_c_mem(p, b, m);
	tg_c_puts(b, ", ");
}

// Prints a call's expression: the callee by its name when its prototype has the call's
// signature, else through a pointer to a function of the signature's type.
TG_INTERNAL void tg_c_call_expr(tgProc *p, tgBytes *b, tgCall const *call) {
	tgModule *m = p->m;
	tyg_sig const *sig = call->c->sig;
	tyg_type const *rt = sig->ret != NULL ? sig->ret : tyg_type_scalar(m, TYG_VOID);
	if (call->target_sym >= 0 && tg_c_sig_equal(tg_c_sym_sig(m, call->target_sym), sig)) {
		tg_c_puts(b, tg_c_sym_name(m, call->target_sym).text);
	} else {
		tgBytes d;
		memset(&d, 0, sizeof(d));
		tg_c_puts(&d, "(*)");
		tg_c_params(m, &d, sig, NULL);
		tg_c_puts(b, "((");
		tg_c_type_decl(m, b, rt, (char const *)d.data);
		if (call->target_sym >= 0) tg_c_printf(b, ")(uintptr_t)&%s)", tg_c_sym_name(m, call->target_sym).text);
		else tg_c_printf(b, ")(uintptr_t)v%u)", call->target_vreg);
		tg_array_free(&d);
	}
	tg_c_puts(b, "(");
	for (isize i = 0; i < call->args.count; i++) {
		tgCallArg const *a = &call->args.data[i];
		tyg_type const *t = call->c->values[i].type;
		if (i > 0) tg_c_puts(b, ", ");
		if (a->kind == tgCallArg_StackMem) {
			tg_c_mem_val(p, b, t, a->mem);
		} else if (t->kind == tgT_Ptr) {
			tg_c_printf(b, "(void *)(uintptr_t)v%u", a->vreg);
		} else {
			tg_c_puts(b, "(");
			tg_c_type_decl(m, b, t, "");
			tg_c_printf(b, ")v%u", a->vreg);
		}
	}
	tg_c_puts(b, ")");
}

// Prints a call as a statement, its result stored to its vreg or memory.
TG_INTERNAL void tg_c_call(tgProc *p, tgBytes *b, tgCall const *call) {
	tyg_sig const *sig = call->c->sig;
	if ((sig->flags & TYG_SIG_VARIADIC) && sig->count == 0) {
		tg_fail(p, "C output: a variadic signature needs a named parameter");
		return;
	}
	if (call->result_vreg != 0 && sig->ret->kind == tgT_Ptr) {
		tg_c_printf(b, "v%u = (uint64_t)(uintptr_t)", call->result_vreg);
	} else if (call->result_vreg != 0) {
		tg_c_printf(b, "v%u = (%s)", call->result_vreg, tg_c_utypes[p->vregs.data[call->result_vreg]]);
	} else if (call->rets.count == 1) {
		tg_c_mem_set_begin(p, b, sig->ret, call->rets.data[0].dst);
		tg_c_call_expr(p, b, call);
		tg_c_puts(b, ");");
		return;
	} else if (sig->ret != NULL && sig->ret->kind != tgT_Void) {
		tg_c_puts(b, "(void)");
	}
	tg_c_call_expr(p, b, call);
	tg_c_puts(b, ";");
}

// Prints `return` with the procedure's result: the value of a Ret, or a zero one for a Ret
// without a value in a procedure that has a result, which C does not allow.
TG_INTERNAL void tg_c_ret(tgProc *p, tgBytes *b, tgInstr const *in) {
	tgCall const *call = &p->calls.data[in->imm];
	tyg_type const *rt = p->sig->ret;
	if (rt == NULL || rt->kind == tgT_Void) {
		tg_c_puts(b, "return;");
	} else if (call->args.count == 1 && call->args.data[0].kind == tgCallArg_StackMem) {
		tg_c_puts(b, "return ");
		tg_c_mem_val(p, b, rt, call->args.data[0].mem);
		tg_c_puts(b, ";");
	} else if (call->args.count == 1 && rt->kind == tgT_Ptr) {
		tg_c_printf(b, "return (void *)(uintptr_t)v%u;", call->args.data[0].vreg);
	} else if (tg_t_is_scalar(rt) || rt->kind == tgT_F80) {
		tg_c_puts(b, "return (");
		tg_c_type_decl(p->m, b, rt, "");
		if (call->args.count == 1) tg_c_printf(b, ")v%u;", call->args.data[0].vreg);
		else tg_c_puts(b, ")0;");
	} else {
		tg_c_puts(b, "{ ");
		tg_c_type_decl(p->m, b, rt, "tyg_r");
		tg_c_puts(b, "; memset(&tyg_r, 0, sizeof(tyg_r)); return tyg_r; }");
	}
}

// Whether a scalar of `size` bytes has a TYG_LD/ST/MV macro.
TG_INTERNAL bool tg_c_scalar_bits(i32 size) {
	return size == 1 || size == 2 || size == 4 || size == 8;
}

// Prints an atomic access of `bits` to in->mem, see tg_c_atomics_prelude. A float load
// goes through an integer.
TG_INTERNAL void tg_c_atomic(tgProc *p, tgBytes *b, tgInstr const *in, int bits, char const *amp) {
	char ptr[16];
	snprintf(ptr, sizeof(ptr), "(uint%d_t *)", bits);
	p->c_uses |= tgCUse_Atomics;
	switch (in->op) {
	case tgOp_AtomicLoad:
		if (tg_type_is_float(p->vregs.data[in->dst])) tg_c_printf(b, "{ uint%d_t tyg_t = ", bits);
		else tg_c_printf(b, "v%u = ", in->dst);
		tg_c_printf(b, "TYG_ATOMIC_LOAD_%d(%s", bits, ptr);
		tg_c_mem(p, b, in->mem);
		if (tg_type_is_float(p->vregs.data[in->dst])) tg_c_printf(b, "); memcpy(%sv%u, &tyg_t, %d); }", amp, in->dst, bits / 8);
		else tg_c_puts(b, ");");
		return;
	case tgOp_AtomicStore:
		tg_c_printf(b, "TYG_ATOMIC_STORE_%d(%s", bits, ptr);
		tg_c_mem(p, b, in->mem);
		tg_c_printf(b, ", v%u);", in->a);
		return;
	case tgOp_AtomicRmw:
		if (in->aux >= tg_count_of(tg_c_rmw_names)) {
			tg_fail(p, "C output: invalid atomic operation");
			return;
		}
		tg_c_printf(b, "v%u = TYG_ATOMIC_%s_%d(%s", in->dst, tg_c_rmw_names[in->aux], bits, ptr);
		tg_c_mem(p, b, in->mem);
		tg_c_printf(b, ", v%u);", in->a);
		return;
	case tgOp_AtomicCas:
		tg_c_printf(b, "{ uint8_t tyg_ok; v%u = TYG_ATOMIC_CAS_%d(%s", in->dst, bits, ptr);
		tg_c_mem(p, b, in->mem);
		tg_c_printf(b, ", v%u, v%u, &tyg_ok); v%u = tyg_ok; }", in->a, in->b, in->c);
		return;
	}
}

// Prints a tgOp_X87 as the C compiler's long double, see tgX87Op. T and S are the unsigned
// and signed C types of the instruction's type.
TG_INTERNAL void tg_c_x87(tgProc *p, tgBytes *b, tgInstr const *in, char const *T, char const *S) {
	static char const ops[] = "+-*/";
	u32 d = in->dst, a = in->a, pb = in->b, c = in->c;
	p->c_uses |= tgCUse_Ld;
	switch (in->aux) {
	case tgX87_Add:
	case tgX87_Sub:
	case tgX87_Mul:
	case tgX87_Div:
		tg_c_printf(b, "tyg_ld_set(v%u, tyg_ld_get(v%u) %c tyg_ld_get(v%u));", a, pb, ops[in->aux - tgX87_Add], c);
		return;
	case tgX87_Neg:    tg_c_printf(b, "tyg_ld_set(v%u, -tyg_ld_get(v%u));", a, pb); return;
	case tgX87_FromSI: tg_c_printf(b, "tyg_ld_set(v%u, (TYG_F80)(%s)v%u);", a, S, c); return;
	case tgX87_FromUI:
	case tgX87_FromF:  tg_c_printf(b, "tyg_ld_set(v%u, (TYG_F80)v%u);", a, c); return;
	case tgX87_ToSI:   tg_c_printf(b, "v%u = (%s)(%s)tyg_ld_get(v%u);", d, T, S, pb); return;
	case tgX87_ToUI:
	case tgX87_ToF:    tg_c_printf(b, "v%u = (%s)tyg_ld_get(v%u);", d, T, pb); return;
	case tgX87_Cmp:
		if (in->imm >= tgCond_FEQ && in->imm <= tgCond_FGE) {
			tg_c_printf(b, "v%u = (uint8_t)(tyg_ld_get(v%u) %s tyg_ld_get(v%u));", d, pb, tg_c_conds[in->imm], c);
			return;
		}
		break;
	}
	tg_fail(p, "C output: invalid long double operation");
}

// Prints one instruction as a statement. `line` and `file` are the last #line's.
TG_INTERNAL void tg_c_instr(tgProc *p, tgBytes *b, tgInstr const *in, i32 *line, i32 *file) {
	tgModule *m = p->m;
	tgType t = in->dst != 0 ? p->vregs.data[in->dst] : in->type;
	i32 size = tg_type_size(t);
	char const *T = tg_c_utypes[t], *S = tg_c_itypes[t];
	// what a narrow value computes in, unsigned: uint16_t * uint16_t is an int that overflows
	char const *W = size == 8 ? "uint64_t" : "uint32_t", *SW = size == 8 ? "int64_t" : "int32_t";
	char const *op = NULL;
	u32 d = in->dst, a = in->a, c = in->c;
	unsigned shift_mask = size == 8 ? 63 : 31; // the x86 shifts, see tg_fold_shift
	char const *amp = p->c_returns_twice ? "(void *)&" : "&"; // memcpy of a volatile vreg
	switch (in->op) {
	case tgOp_Nop:
	case tgOp_Scope:
		return;
	case tgOp_Loc:
		if (in->imm <= 0 || in->a == 0 || (isize)in->a > m->files.count) return;
		if ((i32)in->imm == *line && (i32)in->a == *file) return;
		*line = (i32)in->imm;
		*file = (i32)in->a;
		tg_c_printf(b, "#line %d ", *line);
		tg_c_string(b, m->files.data[in->a - 1]);
		tg_c_puts(b, "\n");
		return;
	}
	tg_c_puts(b, "\t");
	switch (in->op) {
	case tgOp_Param: {
		tyg_type const *pt = p->sig->params[in->imm];
		char buf[16];
		char const *name = tg_c_param_name(p, (i32)in->imm, buf);
		if (tg_t_scalar(pt) == tgType_None) tg_c_printf(b, "v%u = (uint64_t)(uintptr_t)&%s;", d, name);
		else if (pt->kind == tgT_Ptr) tg_c_printf(b, "v%u = (uint64_t)(uintptr_t)%s;", d, name);
		else tg_c_printf(b, "v%u = (%s)%s;", d, T, name);
		break;
	}
	case tgOp_IConst: {
		unsigned long long k = (u64)in->imm & tg_width_mask(t);
		tg_c_printf(b, size == 8 ? "v%u = UINT64_C(0x%llx);" : "v%u = 0x%llxu;", d, k);
		break;
	}
	case tgOp_FConst: {
		f64 x = 0;
		if (t == tgType_F32) {
			u32 bits = (u32)in->imm;
			f32 f;
			memcpy(&f, &bits, 4);
			x = f;
		} else {
			memcpy(&x, &in->imm, 8);
		}
		if (x - x == 0) tg_c_printf(b, "v%u = %a%s;", d, x, t == tgType_F32 ? "f" : ""); // finite: a hex float, exact
		else if (t == tgType_F32) tg_c_printf(b, "{ uint32_t tyg_t = 0x%xu; memcpy(%sv%u, &tyg_t, 4); }", (unsigned)in->imm, amp, d);
		else tg_c_printf(b, "{ uint64_t tyg_t = UINT64_C(0x%llx); memcpy(%sv%u, &tyg_t, 8); }", (unsigned long long)in->imm, amp, d);
		break;
	}
	case tgOp_Lea:
		tg_c_printf(b, "v%u = (uint64_t)(uintptr_t)", d);
		tg_c_mem(p, b, in->mem);
		tg_c_puts(b, ";");
		break;
	// after a setjmp returns again only volatile accesses see what changed since its first return
	case tgOp_Load:
		if ((in->flags & tgInstrFlag_Volatile) || p->c_returns_twice) {
			tg_c_printf(b, "v%u = *(volatile %s *)", d, T);
			tg_c_mem(p, b, in->mem);
			tg_c_puts(b, ";");
		} else {
			if (tg_c_scalar_bits(size)) tg_c_printf(b, "TYG_LD%d(&v%u, ", size * 8, d);
			else tg_c_printf(b, "memcpy(&v%u, ", d);
			tg_c_mem(p, b, in->mem);
			if (tg_c_scalar_bits(size)) tg_c_puts(b, ");");
			else tg_c_printf(b, ", %d);", size);
		}
		break;
	case tgOp_Store:
		if ((in->flags & tgInstrFlag_Volatile) || p->c_returns_twice) {
			tg_c_printf(b, "*(volatile %s *)", T);
			tg_c_mem(p, b, in->mem);
			tg_c_printf(b, " = v%u;", a);
		} else {
			if (tg_c_scalar_bits(size)) tg_c_printf(b, "TYG_ST%d(", size * 8);
			else tg_c_puts(b, "memcpy(");
			tg_c_mem(p, b, in->mem);
			if (tg_c_scalar_bits(size)) tg_c_printf(b, ", &v%u);", a);
			else tg_c_printf(b, ", &v%u, %d);", a, size);
		}
		break;
	case tgOp_Copy: tg_c_printf(b, "v%u = v%u;", d, a); break;
	case tgOp_Add: op = "+"; goto wrap;
	case tgOp_Sub: op = "-"; goto wrap;
	case tgOp_Mul: op = "*"; goto wrap;
	case tgOp_And: op = "&"; goto wrap;
	case tgOp_Or:  op = "|"; goto wrap;
	case tgOp_Xor: op = "^"; goto wrap;
	wrap:
		tg_c_printf(b, "v%u = (%s)((%s)v%u %s (%s)v%u);", d, T, W, a, op, W, in->b);
		break;
	case tgOp_UDiv: tg_c_printf(b, "v%u = (%s)(v%u / v%u);", d, T, a, in->b); break;
	case tgOp_URem: tg_c_printf(b, "v%u = (%s)(v%u %% v%u);", d, T, a, in->b); break;
	case tgOp_SDiv: tg_c_printf(b, "v%u = (%s)((%s)v%u / (%s)v%u);", d, T, S, a, S, in->b); break;
	case tgOp_SRem: tg_c_printf(b, "v%u = (%s)((%s)v%u %% (%s)v%u);", d, T, S, a, S, in->b); break;
	case tgOp_Shl:  tg_c_printf(b, "v%u = (%s)((%s)v%u << (v%u & %u));", d, T, W, a, in->b, shift_mask); break;
	case tgOp_LShr: tg_c_printf(b, "v%u = (%s)((%s)v%u >> (v%u & %u));", d, T, W, a, in->b, shift_mask); break;
	case tgOp_AShr: tg_c_printf(b, "v%u = (%s)((%s)(%s)v%u >> (v%u & %u));", d, T, SW, S, a, in->b, shift_mask); break;
	case tgOp_FAdd: tg_c_printf(b, "v%u = v%u + v%u;", d, a, in->b); break;
	case tgOp_FSub: tg_c_printf(b, "v%u = v%u - v%u;", d, a, in->b); break;
	case tgOp_FMul: tg_c_printf(b, "v%u = v%u * v%u;", d, a, in->b); break;
	case tgOp_FDiv: tg_c_printf(b, "v%u = v%u / v%u;", d, a, in->b); break;
	case tgOp_Neg:  tg_c_printf(b, "v%u = (%s)(0u - v%u);", d, T, a); break;
	case tgOp_Not:  tg_c_printf(b, "v%u = (%s)~v%u;", d, T, a); break;
	case tgOp_FNeg: tg_c_printf(b, "v%u = -v%u;", d, a); break;
	case tgOp_ICmp:
	case tgOp_FCmp: {
		bool is_signed = in->aux >= tgCond_SLT && in->aux <= tgCond_SGE;
		char const *cast = is_signed ? tg_c_itypes[in->type] : "";
		if (in->aux >= tg_count_of(tg_c_conds)) {
			tg_fail(p, "C output: invalid condition");
			break;
		}
		tg_c_printf(b, is_signed ? "v%u = (uint8_t)((%s)v%u %s (%s)v%u);" : "v%u = (uint8_t)(%sv%u %s %sv%u);",
		            d, cast, a, tg_c_conds[in->aux], cast, in->b);
		break;
	}
	case tgOp_Sext:  tg_c_printf(b, "v%u = (%s)(%s)(%s)v%u;", d, T, S, tg_c_itypes[in->aux], a); break;
	case tgOp_SIToF: tg_c_printf(b, "v%u = (%s)(%s)v%u;", d, T, tg_c_itypes[in->aux], a); break;
	// through a 32 or 64-bit integer, like the lowerings: out of its range it stays undefined
	case tgOp_FToSI: tg_c_printf(b, "v%u = (%s)(%s)v%u;", d, T, SW, a); break;
	case tgOp_FToUI:
		if (size == 8) tg_c_printf(b, "v%u = (%s)v%u;", d, T, a);
		else tg_c_printf(b, "v%u = (%s)(int64_t)v%u;", d, T, a);
		break;
	case tgOp_Zext:
	case tgOp_Trunc:
	case tgOp_UIToF:
	case tgOp_FExt:
	case tgOp_FTrunc:
		tg_c_printf(b, "v%u = (%s)v%u;", d, T, a);
		break;
	case tgOp_Bitcast:
		if (tg_c_scalar_bits(size)) tg_c_printf(b, "TYG_MV%d(%sv%u, %sv%u);", size * 8, amp, d, amp, a);
		else tg_c_printf(b, "memcpy(%sv%u, %sv%u, %d);", amp, d, amp, a, size);
		break;
	case tgOp_Select:  tg_c_printf(b, "v%u = v%u ? v%u : v%u;", d, a, in->b, c); break;
	case tgOp_MemCopy:
	case tgOp_MemMove:
		tg_c_printf(b, "%s((void *)(uintptr_t)v%u, (void *)(uintptr_t)v%u, %lld);", in->op == tgOp_MemCopy ? "memcpy" : "memmove", a, in->b, (long long)in->imm);
		break;
	case tgOp_MemZero:
		tg_c_puts(b, "memset(");
		tg_c_mem(p, b, in->mem);
		tg_c_printf(b, ", 0, %lld);", (long long)in->imm);
		break;
	case tgOp_MemCopyDyn:
	case tgOp_MemMoveDyn:
		tg_c_printf(b, "%s((void *)(uintptr_t)v%u, (void *)(uintptr_t)v%u, (size_t)v%u);", in->op == tgOp_MemCopyDyn ? "memcpy" : "memmove", a, in->b, c);
		break;
	case tgOp_MemSetDyn:
		tg_c_printf(b, "memset((void *)(uintptr_t)v%u, (int)v%u, (size_t)v%u);", a, in->b, c);
		break;
	case tgOp_Jump:        tg_c_printf(b, "goto B%d;", (int)in->imm); break;
	case tgOp_Branch:      tg_c_printf(b, "if (v%u) goto B%d; else goto B%u;", a, (int)in->imm, c); break;
	case tgOp_Ret:         tg_c_ret(p, b, in); break;
	case tgOp_Call:
		if (p->calls.data[in->imm].c == NULL) tg_c_unsupported(p, in->op);
		else tg_c_call(p, b, &p->calls.data[in->imm]);
		break;
	case tgOp_VaStart: {
		char buf[16];
		tg_c_printf(b, "va_start(*(va_list *)(uintptr_t)v%u, %s);", a, tg_c_param_name(p, p->sig->count - 1, buf));
		break;
	}
	case tgOp_VaCopy:  tg_c_printf(b, "va_copy(*(va_list *)(uintptr_t)v%u, *(va_list *)(uintptr_t)v%u);", a, in->b); break;
	case tgOp_VaArg: {
		// fetched as the type the C default argument promotions give it
		tyg_type const *vt = p->c_va_types.data[in->imm];
		char const *promoted = vt->kind == tgT_F32 ? "double" : vt->kind == tgT_Int && vt->bits < 32 ? "int32_t" : NULL;
		tg_c_printf(b, "tyg_va%d = ", (int)in->imm);
		if (promoted != NULL) {
			tg_c_puts(b, "(");
			tg_c_type_decl(m, b, vt, "");
			tg_c_puts(b, ")");
		}
		tg_c_printf(b, "va_arg(*(va_list *)(uintptr_t)v%u, ", a);
		if (promoted != NULL) tg_c_puts(b, promoted);
		else tg_c_type_decl(m, b, vt, "");
		tg_c_printf(b, "); v%u = (uint64_t)(uintptr_t)&tyg_va%d;", d, (int)in->imm);
		break;
	}
	case tgOp_Unreachable: tg_c_puts(b, "TYG_UNREACHABLE();"); break;
	case tgOp_Trap:        tg_c_puts(b, "TYG_TRAP();"); break;
	case tgOp_DebugTrap:   tg_c_puts(b, "TYG_DEBUG_TRAP();"); break;
	case tgOp_AtomicFence:
		p->c_uses |= tgCUse_Atomics;
		tg_c_puts(b, "TYG_FENCE();");
		break;
	case tgOp_AtomicLoad:
	case tgOp_AtomicStore:
	case tgOp_AtomicRmw:
	case tgOp_AtomicCas:
		tg_c_atomic(p, b, in, size * 8, amp);
		break;
	case tgOp_Sqrt: tg_c_printf(b, "v%u = %s(v%u);", d, t == tgType_F32 ? "sqrtf" : "sqrt", a); break;
	case tgOp_Bswap:    op = "BSWAP"; goto intrin;
	case tgOp_Popcount: op = "POPCOUNT"; goto intrin;
	case tgOp_Ctz:      op = "CTZ"; goto intrin;
	case tgOp_Clz:      op = "CLZ"; goto intrin;
	intrin:
		p->c_uses |= tgCUse_Intrin;
		tg_c_printf(b, "v%u = (%s)TYG_%s_%d(v%u);", d, T, op, size * 8, a);
		break;
	case tgOp_FrameAddress:  op = "FRAME_ADDRESS"; goto addr;
	case tgOp_ReturnAddress: op = "RETURN_ADDRESS"; goto addr;
	addr:
		p->c_uses |= tgCUse_Intrin;
		tg_c_printf(b, "v%u = (uint64_t)(uintptr_t)TYG_%s();", d, op);
		break;
	case tgOp_CpuRelax:
		p->c_uses |= tgCUse_Intrin;
		tg_c_puts(b, "TYG_CPU_RELAX();");
		break;
	case tgOp_Alloca: {
		// alloca's memory is aligned to 16; more takes room to round the pointer up
		long long over = (long long)in->imm - 1;
		p->c_uses |= tgCUse_Intrin;
		if (in->imm <= 16) tg_c_printf(b, "v%u = (uint64_t)(uintptr_t)TYG_ALLOCA((size_t)v%u);", d, a);
		else tg_c_printf(b, "v%u = ((uint64_t)(uintptr_t)TYG_ALLOCA((size_t)v%u + %lld) + %lld) & ~(uint64_t)%lld;", d, a, over, over, over);
		break;
	}
	case tgOp_TlsAddr: // not built in C mode, see tg_addr_to_mem
		tg_c_printf(b, "v%u = (uint64_t)(uintptr_t)&%s;", d, tg_c_sym_name(m, (i32)in->imm).text);
		break;
	case tgOp_StackPointer:
	case tgOp_StackRestore:
		tg_fail(p, "C output: stack save and restore are not supported");
		break;
	case tgOp_X87:
		tg_c_x87(p, b, in, T, S);
		break;
	default:
		tg_c_unsupported(p, in->op);
		break;
	}
	tg_c_puts(b, "\n");
}

// A name for a variable of the procedure being printed: `name`, or with `suffix` the first
// of name_1, name_2, ... that is a usable C name, free in the procedure and at file scope.
// Empty when there is none.
TG_INTERNAL tgString tg_c_var_name(tgProc *p, tgString name, bool suffix) {
	tgModule *m = p->m;
	tgString out = {0};
	tgBytes s;
	memset(&s, 0, sizeof(s));
	for (int k = 0; k < (suffix ? 100 : 1) && out.len == 0; k++) {
		tg_array_clear(&s);
		tg_c_printf(&s, k == 0 ? "%.*s" : "%.*s_%d", (int)name.len, name.text, k);
		tgString try_name = tg_str_make((char const *)s.data, s.count);
		bool ok = tg_c_name_ok(try_name) && tg_strmap_get(&m->c_dnames, try_name) == NULL && tg_strmap_get(&m->symbol_map, try_name) == NULL;
		for (isize i = 0; ok && i < p->locals.count + p->sig->count; i++) ok = !tg_str_eq(p->c_names[i], try_name);
		if (ok) out = tg_module_string(m, try_name);
	}
	tg_array_free(&s);
	return out;
}

// The debug variable that names a local alone: not through a pointer, nor with others.
TG_INTERNAL tgDebugVar const *tg_c_local_var(tgProc *p, i32 local) {
	tgDebugVar const *found = NULL;
	for (isize i = 0; i < p->debug_vars.count; i++) {
		tgDebugVar const *v = &p->debug_vars.data[i];
		if (v->local != local) continue;
		if (found != NULL || v->by_ref) return NULL;
		found = v;
	}
	return found;
}

TG_INTERNAL tyg_dtype const *tg_c_local_dtype(tgProc *p, i32 local) {
	return tg_c_local_var(p, local)->type;
}

// The scalar parameter the entry block stores whole to the local, or -1.
TG_INTERNAL i32 tg_c_param_stored(tgProc *p, i32 local) {
	tgBlock const *entry = p->blocks.data[0];
	for (isize i = 0; i < entry->instrs.count; i++) {
		tgInstr const *in = &entry->instrs.data[i];
		if (in->op != tgOp_Store || in->mem.kind != tgMem_Local || in->mem.base != (u32)local || in->mem.offset != 0) continue;
		for (isize k = 0; k < p->param_vals.count; k++) {
			if (p->param_vals.data[k] == in->a && tg_t_scalar(p->sig->params[k]) != tgType_None) return (i32)k;
		}
	}
	return -1;
}

// Sets p->c_names from the debug variables, in their order, and declares the debug types
// of the named locals. A local takes the name of the one variable that covers it, when its
// type has a C form of the local's size and an alignment the local has; an aggregate
// parameter takes its own; a scalar parameter takes the name of the variable whose local
// it is stored to, and that local gets the name with a suffix.
TG_INTERNAL void tg_c_debug_names(tgProc *p) {
	tgModule *m = p->m;
	isize locals = p->locals.count;
	p->c_names = tg_alloc_items(tgString, locals + p->sig->count + 1);
	if (p->debug_vars.count == 0) return;
	if (m->c_dts.count < m->dtypes.count) tg_array_resize(&m->c_dts, m->dtypes.count);
	for (isize i = 0; i < p->debug_vars.count; i++) {
		tgDebugVar const *v = &p->debug_vars.data[i];
		if (v->c_param > 0 && p->c_names[locals + v->c_param - 1].len == 0) {
			p->c_names[locals + v->c_param - 1] = tg_c_var_name(p, v->name, true);
			continue;
		}
		if (v->local < 0 || v->by_ref || v->offset != 0 || v->type == NULL) continue;
		// the parameter takes the name first, so a backtrace shows it; its local gets name_1
		i32 k = v->is_param ? tg_c_param_stored(p, v->local) : -1;
		if (k >= 0 && p->c_names[locals + k].len == 0) p->c_names[locals + k] = tg_c_var_name(p, v->name, false);
		tgLocal const *l = &p->locals.data[v->local];
		if (tg_c_local_var(p, v->local) == v && tg_c_dt_form(m, v->type) && tg_c_dt(m, v->type)->size == l->size &&
		    tg_c_dt(m, v->type)->align <= tg_max(l->over_align ? l->over_align : l->align, (i64)1)) {
			tg_c_dt_define(m, v->type); // first: its typedefs take names a variable then avoids
			p->c_names[v->local] = tg_c_var_name(p, v->name, true);
		}
	}
}

// Prints the procedure being ended into m->c_procs, or on failure sets p->fail_reason and
// leaves m->c_procs as it was. The symbol counts as defined, for the definition's storage.
TG_INTERNAL bool tg_c_proc(tgProc *p) {
	tgModule *m = p->m;
	tgBytes b;
	memset(&b, 0, sizeof(b));
	tg_c_debug_names(p);
	tg_c_proto(m, &b, p->sym, p);
	tg_c_puts(&b, " {\n");
	// what changes between a setjmp and its longjmp is lost unless volatile
	char const *vol = p->c_returns_twice ? "volatile " : "";
	for (isize i = 0; i < p->locals.count; i++) {
		tgLocal const *l = &p->locals.data[i];
		i64 align = tg_max(l->over_align ? l->over_align : l->align, (i64)1);
		tyg_dtype const *t = p->c_names[i].len > 0 ? tg_c_local_dtype(p, (i32)i) : NULL;
		if (t == NULL) {
			tg_c_printf(&b, "\tTYG_ALIGNED(%lld) %sunsigned char l%d[%lld];\n", (long long)align, vol, (int)i, (long long)tg_max(l->size, (i64)1));
			continue;
		}
		// a named local: its debug type, at least as aligned as the local
		tgBytes d;
		memset(&d, 0, sizeof(d));
		tg_c_printf(&d, "%s%s", vol, p->c_names[i].text);
		tg_array_add(&d, (u8)0);
		tg_c_puts(&b, "\t");
		if (align > tg_c_dt(m, t)->align) tg_c_printf(&b, "TYG_ALIGNED(%lld) ", (long long)align);
		tg_c_dt_decl(m, &b, t, (char const *)d.data, false);
		tg_c_puts(&b, ";\n");
		tg_array_free(&d);
	}
	for (isize v = 1; v < p->vregs.count; v++) {
		tgType t = p->vregs.data[v];
		if (t != tgType_None) tg_c_printf(&b, "\tTYG_UNUSED %s%s v%d;\n", vol, tg_c_utypes[t], (int)v);
	}
	for (isize i = 0; i < p->c_va_types.count; i++) {
		char name[32];
		snprintf(name, sizeof(name), "tyg_va%d", (int)i);
		tg_c_puts(&b, "\t");
		tg_c_type_decl(m, &b, p->c_va_types.data[i], name);
		tg_c_puts(&b, ";\n");
	}
	// a label for each block a jump reaches, -Wall warns of the others; tg_end_proc made sure
	// that those are placed. The blocks that are not hold dead code.
	bool *target = tg_alloc_items(bool, p->blocks.count + 1);
	memset(target, 0, sizeof(bool) * (usize)(p->blocks.count + 1));
	for (isize bi = 0; bi < p->order.count; bi++) {
		tgBlock const *blk = p->order.data[bi];
		tgInstr const *last = blk->instrs.count > 0 ? &blk->instrs.data[blk->instrs.count - 1] : NULL;
		if (last != NULL && (last->op == tgOp_Jump || last->op == tgOp_Branch)) target[last->imm] = true;
		if (last != NULL && last->op == tgOp_Branch) target[last->c] = true;
	}
	i32 line = 0, file = 0;
	for (isize bi = 0; bi < p->order.count && p->fail_reason == NULL; bi++) {
		tgBlock *blk = p->order.data[bi];
		if (!tg_block_terminated(blk)) {
			tg_fail(p, "C output: a block does not end in a terminator");
			break;
		}
		if (target[blk->index]) tg_c_printf(&b, "B%d:;\n", blk->index);
		for (isize ii = 0; ii < blk->instrs.count; ii++) tg_c_instr(p, &b, &blk->instrs.data[ii], &line, &file);
	}
	tg_c_puts(&b, "}\n");
	if (p->fail_reason == NULL) {
		tg_array_add_elems(&m->c_procs, b.data, b.count);
		m->c_uses |= p->c_uses;
	}
	tg_array_free(&b);
	return p->fail_reason == NULL;
}

////////////////////////////////////////////////////////////////
// The translation unit
////////////////////////////////////////////////////////////////

TG_INTERNAL bool tg_write_c(tgModule *m, tgBytes *out) {
	tgBytes structs, decls, defs;
	memset(&structs, 0, sizeof(structs));
	memset(&decls, 0, sizeof(decls));
	memset(&defs, 0, sizeof(defs));
	bool ok = true;
	for (i32 i = 0; i < (i32)m->symbols.count && ok; i++) {
		tgSymbol const *s = &m->symbols.data[i];
		i32 data = s->alias_of != 0 ? (i32)(tg_alias_target(m, i) - m->symbols.data) : i;
		if (tg_c_is_func(m, data)) {
			tg_c_proto(m, &decls, i, NULL);
		} else {
			tg_c_data_decl(m, &decls, i, data, false);
		}
		if (s->alias_of != 0) {
			tg_c_puts(&decls, " TYG_ALIAS(");
			tg_c_string(&decls, tg_c_sym_name(m, data));
			tg_c_puts(&decls, ")");
		}
		tg_c_puts(&decls, ";\n");
		if (s->section == tgSection_Undef && s->common_align == 0 && s->alias_of == 0 && tg_c_sym_name(m, i).text != s->name.text) {
			// MSVC has no asm labels
			tg_c_printf(&decls, "TYG_ALTERNATE_NAME(%s, ", tg_c_sym_name(m, i).text);
			tg_c_string(&decls, s->name);
			tg_c_puts(&decls, ")\n");
		}
		if (s->alias_of != 0 || s->common_align != 0 || s->section == tgSection_Undef || tg_c_is_func(m, i)) continue;
		tg_c_data_decl(m, &defs, i, i, true);
		if (tg_section_is_nobits(m, s->section)) tg_c_puts(&defs, " = {0}");
		else ok = tg_c_data_struct(m, &structs, &defs, i);
		tg_c_puts(&defs, ";\n");
	}
	if (ok) {
		static char const *const archs[] = {"X64", "ARM64"};
		static char const *const oses[] = {"LINUX", "DARWIN", "WINDOWS"};
		tg_c_puts(out, "/* Generated by tinygen. */\n");
		tg_c_printf(out, "#define TYG_TARGET_%s 1\n#define TYG_TARGET_%s 1\n", archs[m->target.arch], oses[m->target.os]);
		tg_c_puts(out, tg_c_prelude);
		if (m->c_uses & tgCUse_Atomics) tg_c_atomics_prelude(out);
		if (m->c_uses & tgCUse_Intrin) tg_c_puts(out, tg_c_intrin_prelude);
		if (m->c_uses & tgCUse_Ld) {
			// the C compiler's long double has the size the module laid it out with
			tg_c_printf(out, "typedef char tyg_f80_size[sizeof(TYG_F80) == %lld ? 1 : -1];\n", (long long)tyg_type_size(tyg_type_scalar(m, TYG_F80)));
			tg_c_puts(out, tg_c_ld_prelude);
		}
		tg_array_add_elems(out, m->c_types.data, m->c_types.count);
		if (m->c_dtypes.count > 0) tg_c_puts(out, "/* the debug types */\n");
		tg_array_add_elems(out, m->c_dtypes.data, m->c_dtypes.count);
		if (structs.count > 0) {
			tg_c_puts(out, "#pragma pack(push, 1)\n");
			tg_array_add_elems(out, structs.data, structs.count);
			tg_c_puts(out, "#pragma pack(pop)\n");
		}
		tg_array_add_elems(out, decls.data, decls.count);
		tg_array_add_elems(out, defs.data, defs.count);
		tg_array_add_elems(out, m->c_procs.data, m->c_procs.count);
	}
	tg_array_free(&structs);
	tg_array_free(&decls);
	tg_array_free(&defs);
	return ok;
}
