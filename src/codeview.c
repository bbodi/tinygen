// CodeView debug info in .debug$S (symbols, line tables) and .debug$T (types), from which
// the linker builds the PDB.
//
// The types are the front end's tyg_dtype, written as MSVC and clang-cl write C types:
//   - base types are the predefined simple type indices (T_INT4, T_REAL64, T_RCHAR, ...), and a
//     pointer to one is the simple type in 64-bit pointer mode (0x600 | index);
//   - const and volatile are LF_MODIFIER, folded into the LF_POINTER of a qualified pointer;
//   - a typedef has no type record: it stands for its type and gets an S_UDT symbol;
//   - a named struct, union or class is a forward reference wherever a type refers to it, and
//     its definition follows later, so records may refer to each other and themselves. A
//     variable's type is the complete definition, like clang's getCompleteTypeIndex. An
//     anonymous record is defined in place, and the members of an anonymous member record are
//     flattened into the enclosing one;
//   - a bitfield is an LF_MEMBER of an LF_BITFIELD at the offset of its storage unit;
//   - an enum is an LF_ENUM of LF_ENUMERATEs;
//   - a function type is an LF_PROCEDURE with an LF_ARGLIST; "..." is a trailing T_NOTYPE.
//
// The RAD Debugger type views and the language specific types are gone.

enum {
	TG_CV_LF_MODIFIER   = 0x1001,
	TG_CV_LF_POINTER    = 0x1002,
	TG_CV_LF_PROCEDURE  = 0x1008,
	TG_CV_LF_ARGLIST    = 0x1201,
	TG_CV_LF_FIELDLIST  = 0x1203,
	TG_CV_LF_BITFIELD   = 0x1205,
	TG_CV_LF_INDEX      = 0x1404,
	TG_CV_LF_ENUMERATE  = 0x1502,
	TG_CV_LF_ARRAY      = 0x1503,
	TG_CV_LF_CLASS      = 0x1504,
	TG_CV_LF_STRUCTURE  = 0x1505,
	TG_CV_LF_UNION      = 0x1506,
	TG_CV_LF_ENUM       = 0x1507,
	TG_CV_LF_MEMBER     = 0x150d,

	TG_CV_LF_CHAR       = 0x8000,
	TG_CV_LF_SHORT      = 0x8001,
	TG_CV_LF_USHORT     = 0x8002,
	TG_CV_LF_LONG       = 0x8003,
	TG_CV_LF_ULONG      = 0x8004,
	TG_CV_LF_QUADWORD   = 0x8009,
	TG_CV_LF_UQUADWORD  = 0x800a,

	TG_CV_S_END         = 0x0006,
	TG_CV_S_FRAMEPROC   = 0x1012,
	TG_CV_S_OBJNAME     = 0x1101,
	TG_CV_S_BLOCK32     = 0x1103,
	TG_CV_S_CONSTANT    = 0x1107,
	TG_CV_S_UDT         = 0x1108,
	TG_CV_S_LDATA32     = 0x110c,
	TG_CV_S_GDATA32     = 0x110d,
	TG_CV_S_LPROC32     = 0x110f,
	TG_CV_S_GPROC32     = 0x1110,
	TG_CV_S_REGREL32    = 0x1111,
	TG_CV_S_LTHREAD32   = 0x1112,
	TG_CV_S_GTHREAD32   = 0x1113,
	TG_CV_S_COMPILE3    = 0x113c,
	TG_CV_S_LOCAL       = 0x113e,
	TG_CV_S_DEFRANGE_REGISTER = 0x1141,

	TG_CV_DEBUG_S_SYMBOLS     = 0xf1,
	TG_CV_DEBUG_S_LINES       = 0xf2,
	TG_CV_DEBUG_S_STRINGTABLE = 0xf3,
	TG_CV_DEBUG_S_FILECHKSMS  = 0xf4,

	TG_CV_PROP_FWDREF       = 0x80,
	TG_CV_LVARFLAG_ISPARAM  = 0x1,

	TG_CV_AMD64_RBP = 334,
};

// simple types
enum {
	TG_CV_T_NOTYPE  = 0x00,
	TG_CV_T_VOID    = 0x03,
	TG_CV_T_CHAR    = 0x10, // signed char
	TG_CV_T_SHORT   = 0x11,
	TG_CV_T_LONG    = 0x12,
	TG_CV_T_QUAD    = 0x13,
	TG_CV_T_UCHAR   = 0x20,
	TG_CV_T_USHORT  = 0x21,
	TG_CV_T_ULONG   = 0x22,
	TG_CV_T_UQUAD   = 0x23,
	TG_CV_T_BOOL08  = 0x30,
	TG_CV_T_BOOL16  = 0x31,
	TG_CV_T_BOOL32  = 0x32,
	TG_CV_T_BOOL64  = 0x33,
	TG_CV_T_REAL32  = 0x40,
	TG_CV_T_REAL64  = 0x41,
	TG_CV_T_REAL80  = 0x42,
	TG_CV_T_REAL16  = 0x46,
	TG_CV_T_RCHAR   = 0x70, // char
	TG_CV_T_WCHAR   = 0x71,
	TG_CV_T_INT4    = 0x74,
	TG_CV_T_UINT4   = 0x75,
	TG_CV_T_INT128  = 0x78,
	TG_CV_T_UINT128 = 0x79,
	TG_CV_T_CHAR16  = 0x7a,
	TG_CV_T_CHAR32  = 0x7b,
	TG_CV_T_CHAR8   = 0x7c,
	TG_CV_T_PTR64   = 0x600, // the mode of a 64-bit pointer to a simple type
};

typedef struct tgCv {
	tgCoffWriter *       w;
	tgBytes *            t;
	u32                  next_index;
	tgMap                types;     // dtype -> the index a reference to it uses
	tgMap                complete;  // named record -> its definition
	tgMap                refs;      // dtype -> reference to its complete type, for variables reached through a pointer
	tgDtypeConstPtrArray to_define; // named records forward referenced, defined at the end
	tgDtypeConstPtrArray udts;      // typedefs and named records, for S_UDT
	tgMap                udt_seen;
} tgCv;

TG_INTERNAL void tg_cv_numeric(tgBytes *b, i64 v) {
	if (v >= 0 && v < 0x8000) {
		tgb_u16(b, (u16)v);
	} else if (v >= -128 && v < 128) {
		tgb_u16(b, TG_CV_LF_CHAR); tgb_u8(b, (u8)(i8)v);
	} else if (v >= -32768 && v < 32768) {
		tgb_u16(b, TG_CV_LF_SHORT); tgb_u16(b, (u16)(i16)v);
	} else if (v >= 0 && v < 0x10000) {
		tgb_u16(b, TG_CV_LF_USHORT); tgb_u16(b, (u16)v);
	} else if (v >= INT32_MIN && v <= INT32_MAX) {
		tgb_u16(b, TG_CV_LF_LONG); tgb_u32(b, (u32)(i32)v);
	} else if (v >= 0 && v <= (i64)UINT32_MAX) {
		tgb_u16(b, TG_CV_LF_ULONG); tgb_u32(b, (u32)v);
	} else {
		tgb_u16(b, TG_CV_LF_QUADWORD); tgb_u64(b, (u64)v);
	}
}

// The numeric leaf of an unsigned value, which may not fit an i64.
TG_INTERNAL void tg_cv_unumeric(tgBytes *b, u64 v) {
	if (v <= (u64)INT32_MAX) {
		tg_cv_numeric(b, (i64)v);
	} else if (v <= UINT32_MAX) {
		tgb_u16(b, TG_CV_LF_ULONG); tgb_u32(b, (u32)v);
	} else {
		tgb_u16(b, TG_CV_LF_UQUADWORD); tgb_u64(b, v);
	}
}

TG_INTERNAL void tg_cv_name(tgBytes *b, tgString s) {
	tgb_bytes(b, s.text, s.len);
	tgb_u8(b, 0);
}

// a type record: length, kind, then the caller's bytes; returns where it starts
TG_INTERNAL isize tg_cv_type_begin(tgCv *cv, u16 kind) {
	isize at = cv->t->count;
	tgb_u16(cv->t, 0);
	tgb_u16(cv->t, kind);
	return at;
}

TG_INTERNAL u32 tg_cv_type_end(tgCv *cv, isize at) {
	tgBytes *b = cv->t;
	// LF_PAD3 LF_PAD2 LF_PAD1
	while (b->count % 4 != 0) tgb_u8(b, (u8)(0xf0 | (4 - b->count % 4)));
	u16 len = (u16)(b->count - at - 2);
	memmove(b->data + at, &len, 2);
	return cv->next_index++;
}

// fields are padded inside a field list too
TG_INTERNAL void tg_cv_field_pad(tgBytes *b) {
	while (b->count % 4 != 0) tgb_u8(b, (u8)(0xf0 | (4 - b->count % 4)));
}

TG_INTERNAL u32 tg_cv_type(tgCv *cv, tyg_dtype const *t);
TG_INTERNAL u32 tg_cv_complete_type(tgCv *cv, tyg_dtype const *t);

// LF_POINTER, 64-bit: `reference` makes it an lvalue reference, `mods` 1 const, 2 volatile
TG_INTERNAL u32 tg_cv_pointer(tgCv *cv, u32 referent, bool reference, u32 mods) {
	isize at = tg_cv_type_begin(cv, TG_CV_LF_POINTER);
	tgb_u32(cv->t, referent);
	u32 attr = 0x0c | ((reference ? 1u : 0u) << 5) | (8u << 13); // 64-bit, size 8
	if (mods & 1) attr |= 1u << 10;
	if (mods & 2) attr |= 1u << 9;
	tgb_u32(cv->t, attr);
	return tg_cv_type_end(cv, at);
}

TG_INTERNAL u32 tg_cv_array(tgCv *cv, u32 elem, i64 size) {
	isize at = tg_cv_type_begin(cv, TG_CV_LF_ARRAY);
	tgb_u32(cv->t, elem);
	tgb_u32(cv->t, TG_CV_T_UQUAD); // the index type
	tg_cv_numeric(cv->t, size);
	tgb_u8(cv->t, 0); // no name
	return tg_cv_type_end(cv, at);
}

// t without its typedefs, const and volatile
TG_INTERNAL tyg_dtype const *tg_cv_strip(tyg_dtype const *t) {
	while (t != NULL && (t->kind == tgDt_Typedef || t->kind == tgDt_Const || t->kind == tgDt_Volatile)) t = t->target;
	return t;
}

// the size of t, which a record completed after t was made has only now
TG_INTERNAL i64 tg_cv_size(tyg_dtype const *t) {
	t = tg_cv_strip(t);
	if (t == NULL) return 0;
	switch (t->kind) {
	case tgDt_Pointer: return 8;
	case tgDt_Array:   return t->count > 0 ? t->count * tg_cv_size(t->target) : 0;
	case tgDt_Enum:    return t->size > 0 ? t->size : (t->target ? tg_cv_size(t->target) : 4);
	}
	return t->size;
}

TG_INTERNAL bool tg_cv_named(tyg_dtype const *t, char const *name) {
	return tg_str_eq(t->name, tg_str(name));
}

// The simple type of a base type, or 0.
TG_INTERNAL u32 tg_cv_base(tyg_dtype const *t) {
	i64 size = t->size;
	switch (t->encoding) {
	case TYG_DT_SIGNED_CHAR:
		if (size == 1) return tg_cv_named(t, "char") ? TG_CV_T_RCHAR : TG_CV_T_CHAR;
		break;
	case TYG_DT_UNSIGNED_CHAR:
		if (size == 1) return tg_cv_named(t, "char") ? TG_CV_T_RCHAR : tg_cv_named(t, "char8_t") ? TG_CV_T_CHAR8 : TG_CV_T_UCHAR;
		if (size == 2) return tg_cv_named(t, "wchar_t") ? TG_CV_T_WCHAR : TG_CV_T_CHAR16;
		if (size == 4) return TG_CV_T_CHAR32;
		break;
	case TYG_DT_FLOAT:
		switch (size) {
		case 2:  return TG_CV_T_REAL16;
		case 4:  return TG_CV_T_REAL32;
		case 8:  return TG_CV_T_REAL64;
		case 10: case 16: return TG_CV_T_REAL80;
		}
		return 0;
	case TYG_DT_BOOL:
		switch (size) {
		case 1: return TG_CV_T_BOOL08;
		case 2: return TG_CV_T_BOOL16;
		case 4: return TG_CV_T_BOOL32;
		case 8: return TG_CV_T_BOOL64;
		}
		return 0;
	}
	bool u = t->encoding == TYG_DT_UNSIGNED || t->encoding == TYG_DT_UNSIGNED_CHAR;
	bool is_long = tg_cv_named(t, "long") || tg_cv_named(t, "long int") || tg_cv_named(t, "unsigned long") ||
	               tg_cv_named(t, "long unsigned int") || tg_cv_named(t, "unsigned long int");
	switch (size) {
	case 1:  return u ? TG_CV_T_UCHAR : TG_CV_T_CHAR;
	case 2:  return u ? (tg_cv_named(t, "wchar_t") ? TG_CV_T_WCHAR : TG_CV_T_USHORT) : TG_CV_T_SHORT;
	case 4:  return is_long ? (u ? TG_CV_T_ULONG : TG_CV_T_LONG) : (u ? TG_CV_T_UINT4 : TG_CV_T_INT4);
	case 8:  return u ? TG_CV_T_UQUAD : TG_CV_T_QUAD;
	case 16: return u ? TG_CV_T_UINT128 : TG_CV_T_INT128;
	}
	return 0;
}

// A field list from serialized fields. A record holds at most 64K, so a long list is
// split, each part continued (LF_INDEX) by the one written before it.
TG_INTERNAL u32 tg_cv_fieldlist(tgCv *cv, tgBytes const *fields, tgIsizeArray const *starts) {
	isize const limit = 0xff00 - 16;
	isize n = starts->count;
	u32 next = 0;
	isize end = fields->count;
	isize first = n;
	while (first > 0) {
		// the last fields that fit
		isize i = first - 1;
		while (i > 0 && end - starts->data[i-1] <= limit) i--;
		tgBytes *b = cv->t;
		isize at = tg_cv_type_begin(cv, TG_CV_LF_FIELDLIST);
		tgb_bytes(b, fields->data + starts->data[i], end - starts->data[i]);
		if (next != 0) {
			tgb_u16(b, TG_CV_LF_INDEX);
			tgb_u16(b, 0);
			tgb_u32(b, next);
		}
		next = tg_cv_type_end(cv, at);
		end = starts->data[i];
		first = i;
	}
	if (next == 0) {
		next = tg_cv_type_end(cv, tg_cv_type_begin(cv, TG_CV_LF_FIELDLIST));
	}
	return next;
}

TG_INTERNAL u16 tg_cv_record_kind(tyg_dtype const *t) {
	switch (t->record_kind) {
	case TYG_DT_UNION: return TG_CV_LF_UNION;
	case TYG_DT_CLASS: return TG_CV_LF_CLASS;
	}
	return TG_CV_LF_STRUCTURE;
}

TG_INTERNAL tgString tg_cv_record_name(tyg_dtype const *t) {
	return t->name.len > 0 ? t->name : tg_str_lit("<unnamed-tag>");
}

// The head of a struct, union or class record after its field list.
TG_INTERNAL u32 tg_cv_record(tgCv *cv, tyg_dtype const *t, u16 count, u16 props, u32 field_list, i64 size) {
	u16 kind = tg_cv_record_kind(t);
	tgBytes *b = cv->t;
	isize at = tg_cv_type_begin(cv, kind);
	tgb_u16(b, count);
	tgb_u16(b, props);
	tgb_u32(b, field_list);
	if (kind != TG_CV_LF_UNION) {
		tgb_u32(b, 0); // derived from
		tgb_u32(b, 0); // vtable shape
	}
	tg_cv_numeric(b, size);
	tg_cv_name(b, tg_cv_record_name(t));
	return tg_cv_type_end(cv, at);
}

// The members of a record, at `base` bytes into the record being defined. An anonymous member
// that is a record contributes its members, like clang's collectMemberInfo.
TG_INTERNAL void tg_cv_members(tgCv *cv, tyg_dtype const *t, i64 base, tgBytes *fields, tgIsizeArray *starts) {
	for (isize i = 0; i < t->members.count; i++) {
		tgDtMember const *f = &t->members.data[i];
		if (f->name.len == 0) {
			tyg_dtype const *inner = tg_cv_strip(f->type);
			if (f->bit_size == 0 && inner != NULL && inner->kind == tgDt_Record && inner->complete) {
				tg_cv_members(cv, inner, base + f->offset, fields, starts);
			}
			continue; // or bitfield padding
		}
		u32 type = 0;
		if (f->bit_size > 0) {
			u32 under = tg_cv_type(cv, f->type);
			isize at = tg_cv_type_begin(cv, TG_CV_LF_BITFIELD);
			tgb_u32(cv->t, under);
			tgb_u8(cv->t, (u8)f->bit_size);
			tgb_u8(cv->t, (u8)(f->bit_offset - 8*f->offset));
			type = tg_cv_type_end(cv, at);
		} else {
			type = tg_cv_type(cv, f->type);
		}
		tg_array_add(starts, fields->count);
		tgb_u16(fields, TG_CV_LF_MEMBER);
		tgb_u16(fields, 3); // public
		tgb_u32(fields, type);
		tg_cv_numeric(fields, base + f->offset);
		tg_cv_name(fields, f->name);
		tg_cv_field_pad(fields);
	}
}

// The definition of a complete record.
TG_INTERNAL u32 tg_cv_record_def(tgCv *cv, tyg_dtype const *t) {
	tgBytes fields;
	tgIsizeArray starts;
	memset(&fields, 0, sizeof(fields));
	memset(&starts, 0, sizeof(starts));
	tg_cv_members(cv, t, 0, &fields, &starts);
	u32 field_list = tg_cv_fieldlist(cv, &fields, &starts);
	u16 count = (u16)tg_min(starts.count, (isize)0xffff);
	tg_array_free(&fields);
	tg_array_free(&starts);
	return tg_cv_record(cv, t, count, 0, field_list, t->size);
}

TG_INTERNAL u32 tg_cv_enum(tgCv *cv, tyg_dtype const *t) {
	u32 utype = t->target != NULL ? tg_cv_type(cv, t->target) : TG_CV_T_INT4;
	tyg_dtype const *under = tg_cv_strip(t->target);
	bool is_unsigned = under != NULL && under->kind == tgDt_Base &&
	                   (under->encoding == TYG_DT_UNSIGNED || under->encoding == TYG_DT_UNSIGNED_CHAR || under->encoding == TYG_DT_BOOL);
	tgBytes fields;
	tgIsizeArray starts;
	memset(&fields, 0, sizeof(fields));
	memset(&starts, 0, sizeof(starts));
	for (isize i = 0; i < t->enumerators.count; i++) {
		tgDtEnumerator const *e = &t->enumerators.data[i];
		tg_array_add(&starts, fields.count);
		tgb_u16(&fields, TG_CV_LF_ENUMERATE);
		tgb_u16(&fields, 3);
		if (is_unsigned) tg_cv_unumeric(&fields, (u64)e->value);
		else tg_cv_numeric(&fields, e->value);
		tg_cv_name(&fields, e->name);
		tg_cv_field_pad(&fields);
	}
	u32 field_list = tg_cv_fieldlist(cv, &fields, &starts);
	tg_array_free(&fields);
	tg_array_free(&starts);
	tgBytes *b = cv->t;
	isize at = tg_cv_type_begin(cv, TG_CV_LF_ENUM);
	tgb_u16(b, (u16)tg_min(t->enumerators.count, (isize)0xffff));
	tgb_u16(b, 0);
	tgb_u32(b, utype);
	tgb_u32(b, field_list);
	tg_cv_name(b, t->name.len > 0 ? t->name : tg_str_lit("<unnamed-tag>"));
	return tg_cv_type_end(cv, at);
}

TG_INTERNAL void tg_cv_add_udt(tgCv *cv, tyg_dtype const *t) {
	u64 key = (u64)(uintptr_t)t;
	if (tg_map_get(&cv->udt_seen, key) != NULL) return;
	tg_map_set(&cv->udt_seen, key, 1);
	tg_array_add(&cv->udts, t);
}

// const and volatile on `t`, which is not itself a qualifier
TG_INTERNAL u32 tg_cv_modified(tgCv *cv, tyg_dtype const *t, u32 mods) {
	if (t != NULL && t->kind == tgDt_Pointer) {
		// the qualifiers of a pointer go in its LF_POINTER, like MSVC
		return tg_cv_pointer(cv, tg_cv_type(cv, t->target), false, mods);
	}
	u32 base = tg_cv_type(cv, t);
	isize at = tg_cv_type_begin(cv, TG_CV_LF_MODIFIER);
	tgb_u32(cv->t, base);
	tgb_u16(cv->t, (u16)mods);
	return tg_cv_type_end(cv, at);
}

TG_INTERNAL u32 tg_cv_func(tgCv *cv, tyg_dtype const *t) {
	u32 ret = tg_cv_type(cv, t->target);
	u32 *args = (u32 *)calloc((usize)t->param_count + 1, sizeof(u32));
	TG_ASSERT(args != NULL);
	u32 n = 0;
	for (i32 i = 0; i < t->param_count; i++) args[n++] = tg_cv_type(cv, t->params[i]);
	if (t->variadic) args[n++] = TG_CV_T_NOTYPE;
	isize al = tg_cv_type_begin(cv, TG_CV_LF_ARGLIST);
	tgb_u32(cv->t, n);
	for (u32 i = 0; i < n; i++) tgb_u32(cv->t, args[i]);
	u32 arglist = tg_cv_type_end(cv, al);
	free(args);
	isize at = tg_cv_type_begin(cv, TG_CV_LF_PROCEDURE);
	tgb_u32(cv->t, ret);
	tgb_u8(cv->t, 0); // near C
	tgb_u8(cv->t, 0);
	tgb_u16(cv->t, (u16)n);
	tgb_u32(cv->t, arglist);
	return tg_cv_type_end(cv, at);
}

// The index a reference to t uses: a forward reference for a named record.
TG_INTERNAL u32 tg_cv_type(tgCv *cv, tyg_dtype const *t) {
	if (t == NULL) return TG_CV_T_VOID;
	u64 key = (u64)(uintptr_t)t;
	i64 *found = tg_map_get(&cv->types, key);
	if (found) return (u32)*found;

	u32 index = 0;
	switch (t->kind) {
	case tgDt_Void:
		index = TG_CV_T_VOID;
		break;
	case tgDt_Base:
		index = tg_cv_base(t);
		if (index == 0) {
			// a size no simple type has: raw bytes
			index = tg_cv_array(cv, TG_CV_T_UCHAR, t->size);
		}
		break;
	case tgDt_Pointer: {
		u32 to = tg_cv_type(cv, t->target);
		if (to < 0x1000 && (to & 0xf00) == 0) {
			index = TG_CV_T_PTR64 | to;
		} else {
			index = tg_cv_pointer(cv, to, false, 0);
		}
		break;
	}
	case tgDt_Const:
	case tgDt_Volatile: {
		u32 mods = 0;
		tyg_dtype const *base = t;
		while (base != NULL && (base->kind == tgDt_Const || base->kind == tgDt_Volatile || base->kind == tgDt_Typedef)) {
			if (base->kind == tgDt_Const) mods |= 1;
			if (base->kind == tgDt_Volatile) mods |= 2;
			if (base->kind == tgDt_Typedef) tg_cv_type(cv, base); // its S_UDT
			base = base->target;
		}
		index = tg_cv_modified(cv, base, mods);
		break;
	}
	case tgDt_Typedef:
		// CodeView has no typedef type: the type stands in, and an S_UDT names it
		index = tg_cv_type(cv, t->target);
		tg_cv_add_udt(cv, t);
		break;
	case tgDt_Array:
		index = tg_cv_array(cv, tg_cv_type(cv, t->target), tg_cv_size(t));
		break;
	case tgDt_Func:
		index = tg_cv_func(cv, t);
		break;
	case tgDt_Record:
		if (t->name.len == 0 && t->complete) {
			// anonymous: no forward reference could find it by name
			index = tg_cv_record_def(cv, t);
			tg_map_set(&cv->complete, key, index);
			break;
		}
		index = tg_cv_record(cv, t, 0, TG_CV_PROP_FWDREF, 0, 0);
		if (t->complete) {
			tg_array_add(&cv->to_define, t);
			tg_cv_add_udt(cv, t);
		}
		break;
	case tgDt_Enum:
		index = tg_cv_enum(cv, t);
		if (t->name.len > 0) tg_cv_add_udt(cv, t);
		break;
	default:
		index = TG_CV_T_NOTYPE;
		break;
	}
	tg_map_set(&cv->types, key, index);
	return index;
}

// The index of t for a variable: a record's definition rather than its forward reference.
TG_INTERNAL u32 tg_cv_complete_type(tgCv *cv, tyg_dtype const *t) {
	if (t == NULL) return TG_CV_T_VOID;
	if (t->kind == tgDt_Typedef) {
		tg_cv_type(cv, t); // its S_UDT
		while (t != NULL && t->kind == tgDt_Typedef) t = t->target;
		if (t == NULL) return TG_CV_T_VOID;
	}
	if (t->kind != tgDt_Record || !t->complete) return tg_cv_type(cv, t);
	u64 key = (u64)(uintptr_t)t;
	i64 *found = tg_map_get(&cv->complete, key);
	if (found) return (u32)*found;
	tg_cv_type(cv, t); // the forward reference first, like MSVC
	found = tg_map_get(&cv->complete, key);
	if (found) return (u32)*found;
	u32 index = tg_cv_record_def(cv, t);
	tg_map_set(&cv->complete, key, index);
	return index;
}

////////////////////////////////////////////////////////////////
// Symbols
////////////////////////////////////////////////////////////////

// a symbol record: length, kind, then the caller's bytes
TG_INTERNAL isize tg_cv_sym_begin(tgBytes *b, u16 kind) {
	isize at = b->count;
	tgb_u16(b, 0);
	tgb_u16(b, kind);
	return at;
}

TG_INTERNAL void tg_cv_sym_end(tgBytes *b, isize at) {
	while (b->count % 4 != 0) tgb_u8(b, 0);
	u16 len = (u16)(b->count - at - 2);
	memmove(b->data + at, &len, 2);
}

TG_INTERNAL isize tg_cv_subsection_begin(tgBytes *b, u32 kind) {
	tgb_u32(b, kind);
	isize at = b->count;
	tgb_u32(b, 0);
	return at;
}

TG_INTERNAL void tg_cv_subsection_end(tgBytes *b, isize at) {
	u32 len = (u32)(b->count - at - 4);
	memmove(b->data + at, &len, 4);
	while (b->count % 4 != 0) tgb_u8(b, 0);
}

// secrel32 + section16 of a code offset (sym < 0), or of a symbol plus `addend`
TG_INTERNAL void tg_cv_addr(tgCoffWriter *w, tgBytes *b, i32 sym, i64 offset) {
	tgModule *m = w->m;
	tgCoffSec sec = tgCoff_Text;
	i64 off = offset;
	i32 rsym = -1;
	if (sym >= 0) {
		tgSymbol const *s = &m->symbols.data[sym];
		if ((s->flags & tgSymbolFlag_Global) || s->section == tgSection_Undef) {
			rsym = sym;
		} else {
			sec = tg_coff_section_of(s->section);
			off = tg_coff_symbol_offset(w, s) + offset;
		}
	}
	tgCoffReloc r1;
	r1.offset = (u32)b->count;
	r1.sym = rsym;
	r1.sym_section = sec;
	r1.type = TG_IMAGE_REL_AMD64_SECREL;
	tg_array_add(&w->relocs[tgCoff_DebugS], r1);
	tgb_u32(b, (u32)off);
	tgCoffReloc r2 = r1;
	r2.offset = (u32)b->count;
	r2.type = TG_IMAGE_REL_AMD64_SECTION;
	tg_array_add(&w->relocs[tgCoff_DebugS], r2);
	tgb_u16(b, 0);
}

// secrel32 + section16 of `offset` bytes into a procedure's code: relative to its symbol, like
// clang, which tools such as llvm-readobj expect of line tables, or else to .text
TG_INTERNAL void tg_cv_code_addr(tgCoffWriter *w, tgBytes *b, tgProcDebug const *pd, i64 offset) {
	if (pd->sym >= 0 && w->m->symbols.data[pd->sym].section == tgSection_Text && w->m->symbols.data[pd->sym].offset == pd->start) {
		tg_cv_addr(w, b, pd->sym, offset);
	} else {
		tg_cv_addr(w, b, -1, pd->start + offset);
	}
}

TG_INTERNAL u16 tg_cv_reg_of_dwarf(u8 dwarf_reg) {
	// DWARF numbers rax, rdx, rcx, rbx, rsi, rdi, rbp, rsp, then r8..r15, then xmm0..xmm15
	static u16 const low[8] = {328, 331, 330, 329, 332, 333, 334, 335};
	if (dwarf_reg < 8) return low[dwarf_reg];
	if (dwarf_reg >= 17) return (u16)(dwarf_reg < 25 ? 154 + (dwarf_reg - 17) : 252 + (dwarf_reg - 25));
	return (u16)(336 + (dwarf_reg - 8));
}

// The register of a variable of `size` bytes in the integer register dwarf_reg: its 8, 16 or
// 32 bit part, as MSVC and clang name it. Visual Studio drops a variable whose register is
// wider than its type.
TG_INTERNAL u16 tg_cv_reg_sized(u8 dwarf_reg, i64 size) {
	static u16 const r32[8] = {17, 19, 18, 20, 23, 24, 22, 21}; // eax, edx, ecx, ebx, esi, edi, ebp, esp
	static u16 const r16[8] = {9, 11, 10, 12, 15, 16, 14, 13};
	static u16 const r8[8]  = {1, 3, 2, 4, 324, 325, 326, 327};
	if (dwarf_reg >= 16 || size >= 8 || size <= 0) return tg_cv_reg_of_dwarf(dwarf_reg);
	if (dwarf_reg < 8) return size > 2 ? r32[dwarf_reg] : size == 2 ? r16[dwarf_reg] : r8[dwarf_reg];
	return (u16)((size > 2 ? 360 : size == 2 ? 352 : 344) + (dwarf_reg - 8));
}

// the size of t's values, through typedefs and qualifiers
TG_INTERNAL i64 tg_cv_value_size(tyg_dtype const *t) {
	while (t != NULL && t->size == 0 && t->target != NULL && (t->kind == tgDt_Typedef || t->kind == tgDt_Const || t->kind == tgDt_Volatile)) t = t->target;
	return t != NULL ? t->size : 8;
}

// Where each lexical scope's code is, and which scopes get a block. Like LLVM, a scope
// without variables of its own, or whose code is interleaved with a sibling's, gets no
// block: its variables and blocks go to the parent.
typedef struct tgCvScopes {
	isize        n;
	i32 *        lo;   // the hull of the scope's code, cold code left out
	i32 *        hi;
	i32 *        eff;  // the scope whose block holds the scope's variables
	tgI32Array * kids; // the blocks directly inside a block
} tgCvScopes;

TG_INTERNAL void tg_cv_scopes_free(tgCvScopes *sc) {
	for (isize s = 0; s < sc->n; s++) tg_array_free(&sc->kids[s]);
	free(sc->lo); free(sc->hi); free(sc->eff); free(sc->kids);
}

TG_INTERNAL i32 tg_cv_scope_parent(tgProcDebug const *pd, i32 s) {
	return s < pd->scope_parent.count ? pd->scope_parent.data[s] : -1;
}

// whether t is s or inside it, a scope's id is above its parent's
TG_INTERNAL bool tg_cv_scope_inside(tgProcDebug const *pd, i32 t, i32 s) {
	while (t > s) t = tg_cv_scope_parent(pd, t);
	return t == s;
}

TG_INTERNAL void tg_cv_scopes(tgCvScopes *sc, tgProcDebug const *pd, u32 len) {
	isize n = tg_max(pd->scope_parent.count, (isize)1);
	sc->n = n;
	sc->lo = (i32 *)calloc((usize)n, sizeof(i32));
	sc->hi = (i32 *)calloc((usize)n, sizeof(i32));
	sc->eff = (i32 *)calloc((usize)n, sizeof(i32));
	sc->kids = (tgI32Array *)calloc((usize)n, sizeof(tgI32Array));
	TG_ASSERT(sc->lo && sc->hi && sc->eff && sc->kids);
	for (isize s = 0; s < n; s++) {
		sc->lo[s] = INT32_MAX;
		sc->hi[s] = 0;
	}
	tgScopeMarkArray const *marks = &pd->scope_marks;
	for (isize k = 0; k < marks->count; k++) {
		i32 lo = marks->data[k].code_offset;
		i32 hi = k+1 < marks->count ? marks->data[k+1].code_offset : (i32)len;
		if (marks->data[k].cold || hi <= lo) continue;
		for (i32 s = marks->data[k].scope; s >= 0 && s < n; s = tg_cv_scope_parent(pd, s)) {
			sc->lo[s] = tg_min(sc->lo[s], lo);
			sc->hi[s] = tg_max(sc->hi[s], hi);
		}
	}
	i32 *own = (i32 *)calloc((usize)n, sizeof(i32));
	TG_ASSERT(own != NULL);
	for (isize i = 0; i < pd->vars.count; i++) {
		i32 s = pd->vars.data[i].scope;
		if (s >= 0 && s < n) own[s] += 1;
	}
	// parents first, by the ids
	for (isize s = 0; s < n; s++) {
		bool ok = s == 0 || (own[s] > 0 && sc->lo[s] < sc->hi[s]);
		for (isize k = 0; k < marks->count; k++) {
			if (!ok || s == 0) break;
			i32 lo = marks->data[k].code_offset;
			i32 hi = k+1 < marks->count ? marks->data[k+1].code_offset : (i32)len;
			if (marks->data[k].cold || hi <= lo || hi <= sc->lo[s] || lo >= sc->hi[s]) continue;
			i32 t = marks->data[k].scope;
			if (!tg_cv_scope_inside(pd, t, (i32)s) && !tg_cv_scope_inside(pd, (i32)s, t)) ok = false;
		}
		i32 parent = tg_cv_scope_parent(pd, (i32)s);
		sc->eff[s] = ok || parent < 0 ? (i32)s : sc->eff[parent];
		if (ok && s > 0 && parent >= 0) tg_array_add(&sc->kids[sc->eff[parent]], (i32)s);
	}
	free(own);
}

// What the symbols of one procedure need.
typedef struct tgCvProc {
	tgCv *              cv;
	tgBytes *           b;
	tgProcDebug const * pd;
	tgCvScopes          sc;
	u32                 len;
} tgCvProc;

TG_INTERNAL u32 tg_cv_var_type(tgCv *cv, tyg_dtype const *t, bool by_ref) {
	u32 index = tg_cv_complete_type(cv, t);
	if (!by_ref) return index;
	u64 key = (u64)(uintptr_t)t;
	i64 *found = tg_map_get(&cv->refs, key);
	if (found) return (u32)*found;
	u32 ref = tg_cv_pointer(cv, index, true, 0);
	tg_map_set(&cv->refs, key, ref);
	return ref;
}

TG_INTERNAL void tg_cv_emit_var(tgCvProc *cp, tgDebugVar const *v) {
	tgCv *cv = cp->cv;
	tgBytes *b = cp->b;
	tgModule *m = cv->w->m;
	isize at = 0;
	if (v->local < 0) {
		if (v->sym < 0) return;
		tgSymbol const *s = &m->symbols.data[v->sym];
		bool tls = (s->flags & tgSymbolFlag_TLS) != 0;
		at = tg_cv_sym_begin(b, tls ? TG_CV_S_LTHREAD32 : TG_CV_S_LDATA32);
		tgb_u32(b, tg_cv_var_type(cv, v->type, false));
		tg_cv_addr(cv->w, b, v->sym, v->offset);
		tg_cv_name(b, v->name);
		tg_cv_sym_end(b, at);
		return;
	}
	if (v->in_reg && v->by_ref) {
		// the register holds the variable's address: it is at [reg+0]
		at = tg_cv_sym_begin(b, TG_CV_S_REGREL32);
		tgb_u32(b, 0);
		tgb_u32(b, tg_cv_var_type(cv, v->type, false));
		tgb_u16(b, tg_cv_reg_of_dwarf(v->dwarf_reg));
		tg_cv_name(b, v->name);
		tg_cv_sym_end(b, at);
		return;
	}
	if (v->in_reg) {
		// S_LOCAL with def ranges rather than S_REGISTER, which the RAD Debugger ignores
		at = tg_cv_sym_begin(b, TG_CV_S_LOCAL);
		tgb_u32(b, tg_cv_var_type(cv, v->type, false));
		tgb_u16(b, v->is_param ? TG_CV_LVARFLAG_ISPARAM : 0);
		tg_cv_name(b, v->name);
		tg_cv_sym_end(b, at);
		// in the register from the end of the prologue, which moves the parameters there, to
		// the end, within the variable's block: Visual Studio drops a variable whose range
		// leaves its block. In ranges of at most 0xffff bytes.
		u32 from = cp->pd->prologue_end > 0 ? (u32)cp->pd->prologue_end : 0;
		u32 to = cp->len;
		i32 s = cp->sc.eff[v->scope >= 0 && v->scope < cp->sc.n ? v->scope : 0];
		if (s > 0) {
			from = tg_max(from, (u32)cp->sc.lo[s]);
			to = tg_min(to, (u32)cp->sc.hi[s]);
		}
		for (u32 lo = from; lo < to; lo += 0xffff) {
			at = tg_cv_sym_begin(b, TG_CV_S_DEFRANGE_REGISTER);
			tgb_u16(b, tg_cv_reg_sized(v->dwarf_reg, tg_cv_value_size(v->type)));
			tgb_u16(b, 0); // not may-be-available
			tg_cv_code_addr(cv->w, b, cp->pd, lo);
			tgb_u16(b, (u16)tg_min(to - lo, 0xffffu));
			tg_cv_sym_end(b, at);
		}
		return;
	}
	at = tg_cv_sym_begin(b, TG_CV_S_REGREL32);
	tgb_u32(b, (u32)v->frame_offset_fixup);
	tgb_u32(b, tg_cv_var_type(cv, v->type, v->by_ref));
	tgb_u16(b, TG_CV_AMD64_RBP);
	tg_cv_name(b, v->name);
	tg_cv_sym_end(b, at);
}

// a lexical block per scope that has variables, nested like the scopes
TG_INTERNAL void tg_cv_emit_scope(tgCvProc *cp, i32 s) {
	tgBytes *b = cp->b;
	tgProcDebug const *pd = cp->pd;
	for (isize i = 0; i < pd->vars.count; i++) {
		tgDebugVar const *v = &pd->vars.data[i];
		i32 vs = v->scope >= 0 && v->scope < cp->sc.n ? v->scope : 0;
		if (cp->sc.eff[vs] == s) tg_cv_emit_var(cp, v);
	}
	tgI32Array const *kids = &cp->sc.kids[s];
	for (isize k = 0; k < kids->count; k++) {
		i32 c = kids->data[k];
		isize at = tg_cv_sym_begin(b, TG_CV_S_BLOCK32);
		tgb_u32(b, 0); // parent, end: the linker fills them in
		tgb_u32(b, 0);
		tgb_u32(b, (u32)(cp->sc.hi[c] - cp->sc.lo[c]));
		tg_cv_code_addr(cp->cv->w, b, pd, cp->sc.lo[c]);
		tg_cv_name(b, tg_str_lit(""));
		tg_cv_sym_end(b, at);
		tg_cv_emit_scope(cp, c);
		at = tg_cv_sym_begin(b, TG_CV_S_END);
		tg_cv_sym_end(b, at);
	}
}

// One block of a line table: the lines of one file.
typedef struct tgCvLine { u32 offset; i32 line; i32 column; } tgCvLine;
TG_ARRAY(tgCvLineArray, tgCvLine);

typedef struct tgCvLines {
	tgBytes *     b;
	tgCvLineArray lines;
	i32           file;
} tgCvLines;

TG_INTERNAL void tg_cv_lines_flush(tgCvLines *l) {
	tgBytes *b = l->b;
	if (l->lines.count == 0) return;
	tgb_u32(b, (u32)(8*(l->file-1))); // the file's entry in the checksums
	tgb_u32(b, (u32)l->lines.count);
	tgb_u32(b, (u32)(12 + 12*l->lines.count));
	// not marked as statements, like LLVM
	for (isize i = 0; i < l->lines.count; i++) {
		tgb_u32(b, l->lines.data[i].offset);
		tgb_u32(b, (u32)tg_max(l->lines.data[i].line, 0) & 0xffffff);
	}
	for (isize i = 0; i < l->lines.count; i++) {
		tgb_u16(b, (u16)tg_clamp(l->lines.data[i].column, 0, 0xffff));
		tgb_u16(b, 0);
	}
	l->lines.count = 0;
}

TG_INTERNAL void tg_cv_add_line(tgCvLines *l, i32 f, u32 offset, i32 line, i32 column) {
	if (f != l->file) {
		tg_cv_lines_flush(l);
		l->file = f;
	}
	if (l->lines.count > 0 && tg_array_last(&l->lines)->offset == offset) {
		tg_array_last(&l->lines)->line = line;
		tg_array_last(&l->lines)->column = column;
		return;
	}
	tgCvLine e;
	e.offset = offset;
	e.line = line;
	e.column = column;
	tg_array_add(&l->lines, e);
}

TG_INTERNAL void tg_codeview_emit(tgCoffWriter *w) {
	tgModule *m = w->m;
	tgCv cv_;
	memset(&cv_, 0, sizeof(cv_));
	tgCv *cv = &cv_;
	cv->w = w;
	cv->t = &w->sec[tgCoff_DebugT];
	cv->next_index = 0x1000;
	tgb_u32(cv->t, 4); // CV_SIGNATURE_C13

	tgBytes *b = &w->sec[tgCoff_DebugS];
	tgb_u32(b, 4); // CV_SIGNATURE_C13

	{
		isize ss = tg_cv_subsection_begin(b, TG_CV_DEBUG_S_SYMBOLS);
		isize at = tg_cv_sym_begin(b, TG_CV_S_OBJNAME);
		tgb_u32(b, 0);
		tg_cv_name(b, m->object_path.len > 0 ? m->object_path : m->source_name);
		tg_cv_sym_end(b, at);
		at = tg_cv_sym_begin(b, TG_CV_S_COMPILE3);
		tgb_u32(b, 0);     // C, no flags
		tgb_u16(b, 0xd0);  // x64
		for (isize i = 0; i < 8; i++) tgb_u16(b, 0); // front and back end versions
		tg_cv_name(b, tg_str_lit("tinygen"));
		tg_cv_sym_end(b, at);
		tg_cv_subsection_end(b, ss);
	}

	bool has_files = m->files.count > 0;
	for (isize k = 0; k < m->proc_debug.count; k++) {
		tgProcDebug const *pd = &m->proc_debug.data[k];
		isize ss = tg_cv_subsection_begin(b, TG_CV_DEBUG_S_SYMBOLS);
		u32 proc_type = 0;
		{
			tyg_dtype const *pt = pd->type;
			while (pt != NULL && pt->kind == tgDt_Typedef) pt = pt->target;
			if (pt != NULL && pt->kind == tgDt_Func) proc_type = tg_cv_type(cv, pt);
		}
		u32 len = (u32)(pd->end - pd->start);
		bool global = pd->sym >= 0 && (m->symbols.data[pd->sym].flags & tgSymbolFlag_Global) != 0;
		isize at = tg_cv_sym_begin(b, global ? TG_CV_S_GPROC32 : TG_CV_S_LPROC32);
		tgb_u32(b, 0); // parent, end, next: the linker fills them in
		tgb_u32(b, 0);
		tgb_u32(b, 0);
		tgb_u32(b, len);
		// debug start: where the parameters are in place, so a breakpoint there shows them
		u32 body = pd->prologue_end > 0 ? (u32)pd->prologue_end : pd->win_alloc_at ? pd->win_alloc_at : pd->win_setfp_at;
		tgb_u32(b, body);
		tgb_u32(b, len);
		tgb_u32(b, proc_type);
		tg_cv_code_addr(w, b, pd, 0);
		tgb_u8(b, 0);
		tg_cv_name(b, pd->name.len > 0 ? pd->name : pd->link_name);
		tg_cv_sym_end(b, at);

		at = tg_cv_sym_begin(b, TG_CV_S_FRAMEPROC);
		tgb_u32(b, (u32)pd->win_alloc_size);
		tgb_u32(b, 0);
		tgb_u32(b, 0);
		tgb_u32(b, (u32)(8*pd->win_push_count + 16*pd->win_xmm_count));
		tgb_u32(b, 0);
		tgb_u16(b, 0);
		tgb_u32(b, (2u << 14) | (2u << 16)); // locals and parameters off rbp
		tg_cv_sym_end(b, at);

		tgCvProc cp;
		memset(&cp, 0, sizeof(cp));
		cp.cv = cv;
		cp.b = b;
		cp.pd = pd;
		cp.len = len;
		tg_cv_scopes(&cp.sc, pd, len);
		tg_cv_emit_scope(&cp, 0);
		tg_cv_scopes_free(&cp.sc);
		at = tg_cv_sym_begin(b, TG_CV_S_END);
		tg_cv_sym_end(b, at);
		tg_cv_subsection_end(b, ss);

		// lines, one block per run of the same file
		bool prologue_line = pd->line > 0 && (pd->line_entry_count == 0 || m->lines.data[pd->line_entry_start].code_offset != 0);
		bool any_line = prologue_line;
		for (i32 i = 0; i < pd->line_entry_count && !any_line; i++) {
			i32 f = m->lines.data[pd->line_entry_start + i].file_id;
			any_line = f > 0 && f <= m->files.count;
		}
		if (!has_files || !any_line) continue;
		ss = tg_cv_subsection_begin(b, TG_CV_DEBUG_S_LINES);
		tg_cv_code_addr(w, b, pd, 0);
		tgb_u16(b, 1); // CV_LINES_HAVE_COLUMNS
		tgb_u32(b, len);
		tgCvLines l;
		memset(&l, 0, sizeof(l));
		l.b = b;
		l.file = tg_max(pd->file_id, 1);
		// the prologue gets the declaration's line, debuggers look up the entry address; the end of
		// the prologue an entry of its own, after the rows of the front end's parameter stores
		i32 cur_file = tg_max(pd->file_id, 1), cur_line = pd->line, cur_column = 0;
		bool prologue_pending = prologue_line && pd->prologue_end > 0;
		if (prologue_line) tg_cv_add_line(&l, cur_file, 0, pd->line, 0);
		for (i32 i = 0; i < pd->line_entry_count; i++) {
			tgLineEntry const *e = &m->lines.data[pd->line_entry_start + i];
			if (e->file_id <= 0 || e->file_id > m->files.count) continue;
			if (prologue_pending && e->code_offset >= pd->prologue_end) {
				tg_cv_add_line(&l, cur_file, (u32)pd->prologue_end, cur_line, cur_column);
				prologue_pending = false;
			}
			tg_cv_add_line(&l, e->file_id, (u32)e->code_offset, e->line, e->column);
			cur_file = e->file_id;
			cur_line = e->line;
			cur_column = e->column;
		}
		if (prologue_pending) tg_cv_add_line(&l, cur_file, (u32)pd->prologue_end, cur_line, cur_column);
		tg_cv_lines_flush(&l);
		tg_array_free(&l.lines);
		tg_cv_subsection_end(b, ss);
	}

	// globals and constants
	if (m->global_debug.count > 0) {
		isize ss = tg_cv_subsection_begin(b, TG_CV_DEBUG_S_SYMBOLS);
		for (isize k = 0; k < m->global_debug.count; k++) {
			tgGlobalDebug const *g = &m->global_debug.data[k];
			if (g->sym < 0) {
				isize at = tg_cv_sym_begin(b, TG_CV_S_CONSTANT);
				tgb_u32(b, tg_cv_complete_type(cv, g->type));
				tg_cv_numeric(b, g->value);
				tg_cv_name(b, g->name);
				tg_cv_sym_end(b, at);
				continue;
			}
			tgSymbol const *s = &m->symbols.data[g->sym];
			bool tls = (s->flags & tgSymbolFlag_TLS) != 0;
			bool global = (s->flags & tgSymbolFlag_Global) != 0;
			u16 kind = tls ? (global ? TG_CV_S_GTHREAD32 : TG_CV_S_LTHREAD32) : (global ? TG_CV_S_GDATA32 : TG_CV_S_LDATA32);
			isize at = tg_cv_sym_begin(b, kind);
			tgb_u32(b, tg_cv_complete_type(cv, g->type));
			tg_cv_addr(w, b, g->sym, 0);
			tg_cv_name(b, g->name);
			tg_cv_sym_end(b, at);
		}
		tg_cv_subsection_end(b, ss);
	}

	// the definitions of the forward referenced records, which may forward reference more
	for (isize i = 0; i < cv->to_define.count; i++) {
		tg_cv_complete_type(cv, cv->to_define.data[i]);
	}

	// the typedefs and named types, by name
	if (cv->udts.count > 0) {
		isize ss = tg_cv_subsection_begin(b, TG_CV_DEBUG_S_SYMBOLS);
		for (isize i = 0; i < cv->udts.count; i++) {
			tyg_dtype const *t = cv->udts.data[i];
			if (t->name.len == 0) continue;
			isize at = tg_cv_sym_begin(b, TG_CV_S_UDT);
			tgb_u32(b, tg_cv_complete_type(cv, t));
			tg_cv_name(b, t->name);
			tg_cv_sym_end(b, at);
		}
		tg_cv_subsection_end(b, ss);
		// a typedef of a record seen only here defines it now; its members may need more
		for (isize i = 0; i < cv->to_define.count; i++) {
			tg_cv_complete_type(cv, cv->to_define.data[i]);
		}
	}

	// file names and their (absent) checksums
	if (has_files) {
		tgBytes strings;
		memset(&strings, 0, sizeof(strings));
		tgb_u8(&strings, 0);
		isize ss = tg_cv_subsection_begin(b, TG_CV_DEBUG_S_FILECHKSMS);
		for (isize i = 0; i < m->files.count; i++) {
			tgString f = m->files.data[i];
			tgb_u32(b, (u32)strings.count);
			tgb_u8(b, 0); // checksum size
			tgb_u8(b, 0); // no checksum
			tgb_u16(b, 0);
			// debuggers match `file:line` against native paths, like LLVM writes them
			isize start = strings.count;
			tg_cv_name(&strings, f);
			for (isize j = start; j < strings.count; j++) {
				if (strings.data[j] == '/') strings.data[j] = '\\';
			}
		}
		tg_cv_subsection_end(b, ss);
		ss = tg_cv_subsection_begin(b, TG_CV_DEBUG_S_STRINGTABLE);
		tgb_bytes(b, strings.data, strings.count);
		tg_cv_subsection_end(b, ss);
		tg_array_free(&strings);
	}

	tg_map_free(&cv->types);
	tg_map_free(&cv->complete);
	tg_map_free(&cv->refs);
	tg_map_free(&cv->udt_seen);
	tg_array_free(&cv->to_define);
	tg_array_free(&cv->udts);
}
