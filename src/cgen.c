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
	"#define TYG_STR2(x) #x\n"
	"#define TYG_STR(x) TYG_STR2(x)\n"
	"#if defined(__GNUC__) || defined(__clang__)\n"
	"#define TYG_ALIGNED(n) __attribute__((aligned(n)))\n"
	"#define TYG_THREAD __thread\n"
	"#define TYG_WEAK __attribute__((weak))\n"
	"#define TYG_SECTION(name) __attribute__((section(name)))\n"
	"#define TYG_ASM_NAME(name) __asm__(TYG_STR(__USER_LABEL_PREFIX__) name)\n"
	"#define TYG_COMMON __attribute__((common))\n"
	"#define TYG_NORETURN __attribute__((noreturn))\n"
	"#define TYG_UNUSED __attribute__((unused))\n"
	"#define TYG_USED __attribute__((used))\n"
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
	"#define TYG_COMMON\n"
	"#define TYG_NORETURN __declspec(noreturn)\n"
	"#define TYG_UNUSED\n"
	"#define TYG_USED\n"
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
	"#define TYG_COMMON\n"
	"#define TYG_NORETURN\n"
	"#define TYG_UNUSED\n"
	"#define TYG_USED\n"
	"#define TYG_HIDDEN\n"
	"#define TYG_PROTECTED\n"
	"#define TYG_EXPORT\n"
	"#define TYG_ALIAS(target)\n"
	"#endif\n"
	"#if defined(TYG_TARGET_WINDOWS)\n"
	"typedef double TYG_F80;\n"
	"#else\n"
	"typedef long double TYG_F80;\n"
	"#endif\n";

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
	"offsetof", "va_start", "va_arg", "va_end", "va_copy",
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
TG_INTERNAL tgString tg_c_sym_name(tgModule *m, i32 sym) {
	if (m->c_names.count < m->symbols.count) tg_array_resize(&m->c_names, m->symbols.count);
	tgString *name = &m->c_names.data[sym];
	if (name->len == 0) {
		*name = m->symbols.data[sym].name;
		if (!tg_c_name_ok(*name)) {
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

// Prints the symbol's name, then its asm label when the C name is not its own.
TG_INTERNAL void tg_c_sym_declarator(tgModule *m, tgBytes *b, i32 sym, char const *suffix) {
	tgString name = tg_c_sym_name(m, sym);
	tg_c_printf(b, "%s%s", name.text, suffix);
	if (name.text != m->symbols.data[sym].name.text) {
		tg_c_puts(b, " TYG_ASM_NAME(");
		tg_c_string(b, m->symbols.data[sym].name);
		tg_c_puts(b, ")");
	}
}

// Records sym's signature for its prototype. The first one stays.
TG_INTERNAL void tg_c_sym_sig_set(tgModule *m, i32 sym, tyg_sig const *sig) {
	if (m->c_sigs.count < m->symbols.count) tg_array_resize(&m->c_sigs, m->symbols.count);
	if (m->c_sigs.data[sym] == NULL) m->c_sigs.data[sym] = sig;
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
	tg_array_add_elems(&m->c_types, b.data, b.count);
	tg_array_free(&b);
	return n;
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

// Prints the prototype of a function symbol, without the semicolon.
TG_INTERNAL void tg_c_proto(tgModule *m, tgBytes *b, i32 sym) {
	tgSymbol const *s = &m->symbols.data[sym];
	tyg_sig const *sig = tg_c_sym_sig(m, sym);
	if (sig == NULL && s->alias_of != 0) sig = tg_c_sym_sig(m, (i32)(tg_alias_target(m, sym) - m->symbols.data));
	if (sig != NULL && (sig->flags & TYG_SIG_NORETURN)) tg_c_puts(b, "TYG_NORETURN ");
	tg_c_attrs(m, b, sym, tg_c_storage(s, s->section != tgSection_Undef));
	tgBytes params, d;
	memset(&params, 0, sizeof(params));
	memset(&d, 0, sizeof(d));
	tg_c_puts(&params, "(");
	if (sig == NULL || (sig->count == 0 && !(sig->flags & TYG_SIG_VARIADIC))) tg_c_puts(&params, "void");
	for (i32 i = 0; sig != NULL && i < sig->count; i++) {
		if (i > 0) tg_c_puts(&params, ", ");
		tg_c_type_decl(m, &params, sig->params[i], "");
	}
	if (sig != NULL && (sig->flags & TYG_SIG_VARIADIC) && sig->count > 0) tg_c_puts(&params, ", ...");
	tg_c_puts(&params, ")");
	tg_array_add(&params, (u8)0);
	tg_c_sym_declarator(m, &d, sym, (char const *)params.data);
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
	tg_c_sym_declarator(m, b, sym, suffix);
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
			tg_c_proto(m, &decls, i);
		} else {
			tg_c_data_decl(m, &decls, i, data, false);
		}
		if (s->alias_of != 0) {
			tg_c_puts(&decls, " TYG_ALIAS(");
			tg_c_string(&decls, tg_c_sym_name(m, data));
			tg_c_puts(&decls, ")");
		}
		tg_c_puts(&decls, ";\n");
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
		tg_array_add_elems(out, m->c_types.data, m->c_types.count);
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
