// The public types for the ABI (tyg_type), signatures (tyg_sig) and debug types
// (tyg_dtype). They live in the module's arena, as long as the module.

#include <ctype.h>
#include <stdarg.h>

////////////////////////////////////////////////////////////////
// Errors
////////////////////////////////////////////////////////////////

// Records why a public call failed, for tyg_error.
TG_INTERNAL void tg_set_error(tgModule *m, char const *fmt, ...) {
	va_list va;
	if (m == NULL) return;
	va_start(va, fmt);
	vsnprintf(m->error, sizeof(m->error), fmt, va);
	va_end(va);
	m->has_error = true;
}

// A zero terminated copy of a name in the module's arena. NULL is the empty name.
TG_INTERNAL tgString tg_module_str(tgModule *m, char const *s) {
	isize len;
	char *p;
	if (s == NULL || s[0] == 0) return tg_str_make("", 0);
	len = (isize)strlen(s);
	p = (char *)tg_arena_alloc(&m->arena, len + 1, 1);
	memcpy(p, s, (usize)len);
	return tg_str_make(p, len);
}

////////////////////////////////////////////////////////////////
// Types for the ABI
////////////////////////////////////////////////////////////////

// Scalars do not depend on the module, so they are shared.
static tyg_type const tg_t_void = {tgT_Void, false, 0, 0, 1, NULL, 0, NULL, 0};
static tyg_type const tg_t_f32  = {tgT_F32,  false, 0, 4, 4, NULL, 0, NULL, 0};
static tyg_type const tg_t_f64  = {tgT_F64,  false, 0, 8, 8, NULL, 0, NULL, 0};
static tyg_type const tg_t_ptr  = {tgT_Ptr,  false, 0, 8, 8, NULL, 0, NULL, 0};
static tyg_type const tg_t_ints[2][4] = {
	{
		{tgT_Int, false,  8, 1, 1, NULL, 0, NULL, 0},
		{tgT_Int, false, 16, 2, 2, NULL, 0, NULL, 0},
		{tgT_Int, false, 32, 4, 4, NULL, 0, NULL, 0},
		{tgT_Int, false, 64, 8, 8, NULL, 0, NULL, 0},
	},
	{
		{tgT_Int, true,  8, 1, 1, NULL, 0, NULL, 0},
		{tgT_Int, true, 16, 2, 2, NULL, 0, NULL, 0},
		{tgT_Int, true, 32, 4, 4, NULL, 0, NULL, 0},
		{tgT_Int, true, 64, 8, 8, NULL, 0, NULL, 0},
	},
};

// long double. On x86-64 SysV it is the x87 extended type in 16 bytes. On arm64 macOS it
// is a double, which as a record of one double passes like one everywhere. On Windows x64
// it is MSVC's double; a record of one double would pass in an integer register there, so
// it keeps its own kind, which the Win64 ABI passes like a double. arm64 Linux has IEEE
// binary128, which the ABI code cannot express yet: its type is laid out right and fails
// where it is used, see tg_ld_supported.
static tyg_type const tg_t_f80     = {tgT_F80, false, 0, 16, 16, NULL, 0, NULL, 0};
static tyg_type const tg_t_f80_win = {tgT_F80, false, 0, 8, 8, NULL, 0, NULL, 0};
static tyg_field tg_t_f80_apple_fields[1] = {{&tg_t_f64, 0}};
static tyg_type const tg_t_f80_apple = {tgT_Record, false, 0, 8, 8, NULL, 0, tg_t_f80_apple_fields, 1};

// Whether long double works on the current target.
TG_INTERNAL bool tg_ld_supported(void) {
	if (tg_is_arm64()) return tg_is_darwin();
	return true;
}

// Whether t is or holds a long double the current target does not support.
TG_INTERNAL bool tg_t_unsupported_ld(tyg_type const *t) {
	i32 i;
	if (t == NULL || tg_ld_supported()) return false;
	switch (t->kind) {
	case tgT_F80:    return true;
	case tgT_Array:  return tg_t_unsupported_ld(t->elem);
	case tgT_Record:
		for (i = 0; i < t->field_count; i++) {
			if (tg_t_unsupported_ld(t->fields[i].type)) return true;
		}
		return false;
	}
	return false;
}

TG_INTERNAL bool tg_t_is_void(tyg_type const *t) { return t->kind == tgT_Void; }

const tyg_type *tyg_type_int(tyg_module *m, tyg_scalar s, int is_signed) {
	int sign = is_signed ? 1 : 0;
	switch (s) {
	case TYG_I8:  return &tg_t_ints[sign][0];
	case TYG_I16: return &tg_t_ints[sign][1];
	case TYG_I32: return &tg_t_ints[sign][2];
	case TYG_I64: return &tg_t_ints[sign][3];
	default: break;
	}
	tg_set_error(m, "tyg_type_int: %d is not an integer type", (int)s);
	return NULL;
}

// An integer from tyg_type_scalar is signed, like C's char, short, int and long on x86-64.
const tyg_type *tyg_type_scalar(tyg_module *m, tyg_scalar s) {
	switch (s) {
	case TYG_VOID: return &tg_t_void;
	case TYG_I8:
	case TYG_I16:
	case TYG_I32:
	case TYG_I64:  return tyg_type_int(m, s, 1);
	case TYG_F32:  return &tg_t_f32;
	case TYG_F64:  return &tg_t_f64;
	case TYG_F80:
		if (m == NULL) return NULL;
		if (m->target.arch == TYG_ARCH_ARM64 && m->target.os == TYG_OS_DARWIN) return &tg_t_f80_apple;
		if (m->target.os == TYG_OS_WINDOWS) return &tg_t_f80_win;
		return &tg_t_f80;
	}
	tg_set_error(m, "tyg_type_scalar: unknown scalar %d", (int)s);
	return NULL;
}

const tyg_type *tyg_type_ptr(tyg_module *m) {
	(void)m;
	return &tg_t_ptr;
}

const tyg_type *tyg_type_array(tyg_module *m, const tyg_type *elem, int64_t count) {
	tyg_type *t;
	if (m == NULL) return NULL;
	if (elem == NULL || tg_t_is_void(elem)) {
		tg_set_error(m, "tyg_type_array: the element type is missing or void");
		return NULL;
	}
	if (count < 0 || (elem->size > 0 && count > INT64_MAX / elem->size)) {
		tg_set_error(m, "tyg_type_array: bad count %lld", (long long)count);
		return NULL;
	}
	t = (tyg_type *)tg_arena_alloc(&m->arena, (isize)sizeof(tyg_type), 16);
	t->kind  = tgT_Array;
	t->elem  = elem;
	t->count = count;
	t->size  = elem->size * count;
	t->align = elem->align;
	return t;
}

// Whether s is a C identifier.
TG_INTERNAL bool tg_is_ident(char const *s) {
	if (s == NULL || !(isalpha((unsigned char)s[0]) || s[0] == '_')) return false;
	for (char const *q = s; *q; q++) if (!(isalnum((unsigned char)*q) || *q == '_')) return false;
	return true;
}

TG_INTERNAL char *tg_arena_strdup(tgArena *a, char const *s) {
	usize n = strlen(s) + 1;
	char *d = (char *)tg_arena_alloc(a, (isize)n, 1);
	memcpy(d, s, n);
	return d;
}

int tyg_type_name(tyg_module *m, const tyg_type *record, const char *name, const char *const *field_names) {
	if (m == NULL) return 1;
	if (record == NULL || record->kind != tgT_Record || !tg_is_ident(name) || record->c_name != NULL) {
		tg_set_error(m, "tyg_type_name: a record not named yet, and an identifier");
		return 1;
	}
	if (tg_map_get(&m->c_type_ids, (u64)(uintptr_t)record) != NULL) {
		tg_set_error(m, "tyg_type_name: the record is already in the C output");
		return 1;
	}
	// a name another record has: name_2, name_3, ...
	char buf[256];
	snprintf(buf, sizeof(buf), "%s", name);
	// the C's struct tags, which the debug types' names share (c_dtags): taken, a suffix
	for (int k = 2; tg_strmap_get(&m->c_dtags, tg_str(buf)) != NULL; k++) snprintf(buf, sizeof(buf), "%s_%d", name, k);
	tyg_type *t = (tyg_type *)record;
	t->c_name = tg_arena_strdup(&m->arena, buf);
	tg_strmap_set(&m->c_dtags, tg_str(t->c_name), 1);
	if (field_names != NULL && record->field_count > 0) {
		char const **fs = (char const **)tg_arena_alloc(&m->arena, (isize)sizeof(char *) * record->field_count, 8);
		for (i32 i = 0; i < record->field_count; i++) {
			char const *f = field_names[i];
			bool ok = tg_is_ident(f);
			for (i32 j = 0; ok && j < i; j++) ok = fs[j] == NULL || strcmp(fs[j], f) != 0;
			fs[i] = ok ? tg_arena_strdup(&m->arena, f) : NULL;
		}
		t->c_fields = fs;
	}
	tg_array_add(&m->c_named, record);
	return 0;
}

const tyg_type *tyg_type_record(tyg_module *m, int64_t size, int64_t align,
                                const tyg_field *fields, int count) {
	tyg_type *t;
	int i;
	if (m == NULL) return NULL;
	if (size < 0 || !tg_is_pow2(align) || size % align != 0) {
		tg_set_error(m, "tyg_type_record: bad size %lld or alignment %lld", (long long)size, (long long)align);
		return NULL;
	}
	if (count < 0 || (count > 0 && fields == NULL)) {
		tg_set_error(m, "tyg_type_record: bad field count %d", count);
		return NULL;
	}
	for (i = 0; i < count; i++) {
		tyg_type const *ft = fields[i].type;
		if (ft == NULL || tg_t_is_void(ft)) {
			tg_set_error(m, "tyg_type_record: field %d has no type or is void", i);
			return NULL;
		}
		if (fields[i].offset < 0 || fields[i].offset > size - ft->size) {
			tg_set_error(m, "tyg_type_record: field %d at offset %lld does not fit in %lld bytes",
			             i, (long long)fields[i].offset, (long long)size);
			return NULL;
		}
	}
	t = (tyg_type *)tg_arena_alloc(&m->arena, (isize)sizeof(tyg_type), 16);
	t->kind  = tgT_Record;
	t->size  = size;
	t->align = align;
	t->field_count = count;
	if (count > 0) {
		t->fields = (tyg_field *)tg_arena_alloc(&m->arena, (isize)sizeof(tyg_field) * count, 16);
		memcpy(t->fields, fields, sizeof(tyg_field) * (usize)count);
	}
	return t;
}

int64_t tyg_type_size(const tyg_type *t)  { return t ? t->size : 0; }
int64_t tyg_type_align(const tyg_type *t) { return t ? t->align : 1; }

// SysV x86-64: {gp_offset, fp_offset, overflow_arg_area, reg_save_area}.
// AAPCS64 on Linux: {__stack, __gr_top, __vr_top, __gr_offs, __vr_offs}.
// Apple arm64 and Win64: a plain pointer.
int64_t tyg_va_list_size(tyg_module *m) {
	if (m == NULL) return 8;
	if (m->target.os == TYG_OS_WINDOWS) return 8;
	if (m->target.arch == TYG_ARCH_ARM64) return m->target.os == TYG_OS_DARWIN ? 8 : 32;
	return 24;
}

int64_t tyg_va_list_align(tyg_module *m) {
	(void)m;
	return 8;
}

tyg_sig *tyg_sig_new(tyg_module *m, const tyg_type *ret,
                     const tyg_type *const *params, int count, unsigned flags) {
	tyg_sig *s;
	int i;
	if (m == NULL) return NULL;
	if (count < 0 || (count > 0 && params == NULL)) {
		tg_set_error(m, "tyg_sig_new: bad parameter count %d", count);
		return NULL;
	}
	if (flags & ~(unsigned)(TYG_SIG_VARIADIC | TYG_SIG_RETURNS_TWICE | TYG_SIG_NORETURN)) {
		tg_set_error(m, "tyg_sig_new: unknown flags 0x%x", flags);
		return NULL;
	}
	for (i = 0; i < count; i++) {
		if (params[i] == NULL || tg_t_is_void(params[i])) {
			tg_set_error(m, "tyg_sig_new: parameter %d has no type or is void", i);
			return NULL;
		}
	}
	s = (tyg_sig *)tg_arena_alloc(&m->arena, (isize)sizeof(tyg_sig), 16);
	s->m     = m;
	s->ret   = ret ? ret : &tg_t_void; // NULL is void
	s->count = count;
	s->flags = flags;
	if (count > 0) {
		s->params = (tyg_type const **)tg_arena_alloc(&m->arena, (isize)sizeof(tyg_type const *) * count, 16);
		memcpy((void *)s->params, params, sizeof(tyg_type const *) * (usize)count);
	}
	return s;
}

////////////////////////////////////////////////////////////////
// Debug types
////////////////////////////////////////////////////////////////

TG_INTERNAL tyg_dtype *tg_dt_new(tgModule *m, tgDtKind kind) {
	tyg_dtype *d = (tyg_dtype *)tg_arena_alloc(&m->arena, (isize)sizeof(tyg_dtype), 16);
	d->kind  = kind;
	d->align = 1;
	d->members.arena     = &m->arena;
	d->enumerators.arena = &m->arena;
	d->index = (i32)m->dtypes.count;
	tg_array_add(&m->dtypes, d);
	return d;
}

const tyg_dtype *tyg_dt_void(tyg_module *m) {
	if (m == NULL) return NULL;
	return tg_dt_new(m, tgDt_Void);
}

const tyg_dtype *tyg_dt_base(tyg_module *m, const char *name, int64_t size, tyg_dt_encoding enc) {
	tyg_dtype *d;
	if (m == NULL) return NULL;
	if (size <= 0 || (u32)enc > (u32)TYG_DT_BOOL) {
		tg_set_error(m, "tyg_dt_base: bad size %lld or encoding %d", (long long)size, (int)enc);
		return NULL;
	}
	d = tg_dt_new(m, tgDt_Base);
	d->name     = tg_module_str(m, name);
	d->encoding = (u8)enc;
	d->size     = size;
	d->align    = tg_is_pow2(size) ? tg_min(size, 16) : 1;
	return d;
}

const tyg_dtype *tyg_dt_pointer(tyg_module *m, const tyg_dtype *to) {
	tyg_dtype *d;
	if (m == NULL) return NULL;
	if (to == NULL) {
		tg_set_error(m, "tyg_dt_pointer: no target type");
		return NULL;
	}
	d = tg_dt_new(m, tgDt_Pointer);
	d->target = to;
	d->size   = 8;
	d->align  = 8;
	return d;
}

// A qualifier or typedef takes the size of its type when it is made, so one of a
// record that is completed later has size 0. Debug info does not need it.
TG_INTERNAL tyg_dtype *tg_dt_wrap(tgModule *m, tgDtKind kind, tyg_dtype const *t, char const *what) {
	tyg_dtype *d;
	if (t == NULL) {
		tg_set_error(m, "%s: no type", what);
		return NULL;
	}
	d = tg_dt_new(m, kind);
	d->target = t;
	d->size   = t->size;
	d->align  = t->align;
	return d;
}

const tyg_dtype *tyg_dt_const(tyg_module *m, const tyg_dtype *t) {
	if (m == NULL) return NULL;
	return tg_dt_wrap(m, tgDt_Const, t, "tyg_dt_const");
}

const tyg_dtype *tyg_dt_volatile(tyg_module *m, const tyg_dtype *t) {
	if (m == NULL) return NULL;
	return tg_dt_wrap(m, tgDt_Volatile, t, "tyg_dt_volatile");
}

const tyg_dtype *tyg_dt_typedef(tyg_module *m, const char *name, const tyg_dtype *t, tyg_file file, int line) {
	tyg_dtype *d;
	if (m == NULL) return NULL;
	if (name == NULL || name[0] == 0) {
		tg_set_error(m, "tyg_dt_typedef: no name");
		return NULL;
	}
	d = tg_dt_wrap(m, tgDt_Typedef, t, "tyg_dt_typedef");
	if (d == NULL) return NULL;
	d->name    = tg_module_str(m, name);
	d->file_id = file;
	d->line    = line;
	return d;
}

const tyg_dtype *tyg_dt_array(tyg_module *m, const tyg_dtype *elem, int64_t count) {
	tyg_dtype *d;
	if (m == NULL) return NULL;
	if (elem == NULL) {
		tg_set_error(m, "tyg_dt_array: no element type");
		return NULL;
	}
	d = tg_dt_new(m, tgDt_Array);
	d->target = elem;
	d->count  = count < 0 ? -1 : count;
	d->size   = count < 0 ? 0 : elem->size * count;
	d->align  = elem->align;
	return d;
}

const tyg_dtype *tyg_dt_func(tyg_module *m, const tyg_dtype *ret, const tyg_dtype *const *params, int count, int variadic) {
	tyg_dtype *d;
	int i;
	if (m == NULL) return NULL;
	if (count < 0 || (count > 0 && params == NULL)) {
		tg_set_error(m, "tyg_dt_func: bad parameter count %d", count);
		return NULL;
	}
	for (i = 0; i < count; i++) {
		if (params[i] == NULL) {
			tg_set_error(m, "tyg_dt_func: parameter %d has no type", i);
			return NULL;
		}
	}
	d = tg_dt_new(m, tgDt_Func);
	d->target      = ret; // NULL is void
	d->variadic    = variadic != 0;
	d->param_count = count;
	if (count > 0) {
		d->params = (tyg_dtype const **)tg_arena_alloc(&m->arena, (isize)sizeof(tyg_dtype const *) * count, 16);
		memcpy((void *)d->params, params, sizeof(tyg_dtype const *) * (usize)count);
	}
	return d;
}

tyg_dtype *tyg_dt_record(tyg_module *m, tyg_dt_record_kind kind, const char *name, tyg_file file, int line) {
	tyg_dtype *d;
	if (m == NULL) return NULL;
	if ((u32)kind > (u32)TYG_DT_CLASS) {
		tg_set_error(m, "tyg_dt_record: bad kind %d", (int)kind);
		return NULL;
	}
	d = tg_dt_new(m, tgDt_Record);
	d->record_kind = (u8)kind;
	d->name        = tg_module_str(m, name);
	d->file_id     = file;
	d->line        = line;
	return d;
}

TG_INTERNAL bool tg_dt_check_member(tgModule *m, tyg_dtype *rec, tyg_dtype const *t, char const *what) {
	if (rec == NULL || rec->kind != tgDt_Record) {
		tg_set_error(m, "%s: not a record", what);
		return false;
	}
	if (rec->complete) {
		tg_set_error(m, "%s: the record is already complete", what);
		return false;
	}
	if (t == NULL) {
		tg_set_error(m, "%s: no member type", what);
		return false;
	}
	return true;
}

void tyg_dt_member(tyg_module *m, tyg_dtype *rec, const char *name, const tyg_dtype *t, int64_t offset) {
	tgDtMember mem;
	if (m == NULL) return;
	if (!tg_dt_check_member(m, rec, t, "tyg_dt_member")) return;
	if (offset < 0) {
		tg_set_error(m, "tyg_dt_member: bad offset %lld", (long long)offset);
		return;
	}
	memset(&mem, 0, sizeof(mem));
	mem.name       = tg_module_str(m, name);
	mem.type       = t;
	mem.offset     = offset;
	mem.bit_offset = offset * 8;
	tg_array_add(&rec->members, mem);
}

// bit_offset counts from the start of the record. `offset` is the start of the
// storage unit of t's size that holds the field, or its first byte when t has no size.
void tyg_dt_bitfield(tyg_module *m, tyg_dtype *rec, const char *name, const tyg_dtype *t, int64_t bit_offset, int bit_size) {
	tgDtMember mem;
	if (m == NULL) return;
	if (!tg_dt_check_member(m, rec, t, "tyg_dt_bitfield")) return;
	if (bit_offset < 0 || bit_size <= 0 || bit_size > 64) {
		tg_set_error(m, "tyg_dt_bitfield: bad bit offset %lld or size %d", (long long)bit_offset, bit_size);
		return;
	}
	memset(&mem, 0, sizeof(mem));
	mem.name       = tg_module_str(m, name);
	mem.type       = t;
	mem.offset     = t->size > 0 ? (bit_offset / (t->size*8)) * t->size : bit_offset / 8;
	mem.bit_offset = bit_offset;
	mem.bit_size   = bit_size;
	tg_array_add(&rec->members, mem);
}

void tyg_dt_record_complete(tyg_module *m, tyg_dtype *rec, int64_t size, int64_t align) {
	if (m == NULL) return;
	if (rec == NULL || rec->kind != tgDt_Record) {
		tg_set_error(m, "tyg_dt_record_complete: not a record");
		return;
	}
	if (size < 0 || !tg_is_pow2(align)) {
		tg_set_error(m, "tyg_dt_record_complete: bad size %lld or alignment %lld", (long long)size, (long long)align);
		return;
	}
	rec->size     = size;
	rec->align    = align;
	rec->complete = true;
}

tyg_dtype *tyg_dt_enum(tyg_module *m, const char *name, const tyg_dtype *underlying, tyg_file file, int line) {
	tyg_dtype *d;
	if (m == NULL) return NULL;
	if (underlying == NULL) {
		tg_set_error(m, "tyg_dt_enum: no underlying type");
		return NULL;
	}
	d = tg_dt_new(m, tgDt_Enum);
	d->name    = tg_module_str(m, name);
	d->target  = underlying;
	d->size    = underlying->size;
	d->align   = underlying->align;
	d->file_id = file;
	d->line    = line;
	return d;
}

void tyg_dt_enumerator(tyg_module *m, tyg_dtype *e, const char *name, int64_t value) {
	tgDtEnumerator en;
	if (m == NULL) return;
	if (e == NULL || e->kind != tgDt_Enum) {
		tg_set_error(m, "tyg_dt_enumerator: not an enum");
		return;
	}
	if (name == NULL || name[0] == 0) {
		tg_set_error(m, "tyg_dt_enumerator: no name");
		return;
	}
	en.name  = tg_module_str(m, name);
	en.value = value;
	tg_array_add(&e->enumerators, en);
}
