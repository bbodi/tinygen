// DWARF 4 debug info, shared by the ELF and Mach-O object writers, and the byte buffer
// helpers every object writer uses.
//
// The types are tinygen's debug types (tyg_dtype), written the way clang -gdwarf-4 writes
// C types.

TG_INTERNAL void tgb_u8 (tgBytes *b, u8  v) { tg_array_add(b, v); }
TG_INTERNAL void tgb_u16(tgBytes *b, u16 v) { tg_array_add_elems(b, (u8 *)&v, 2); }
TG_INTERNAL void tgb_u32(tgBytes *b, u32 v) { tg_array_add_elems(b, (u8 *)&v, 4); }
TG_INTERNAL void tgb_u64(tgBytes *b, u64 v) { tg_array_add_elems(b, (u8 *)&v, 8); }
TG_INTERNAL void tgb_bytes(tgBytes *b, void const *p, isize n) { if (n > 0) tg_array_add_elems(b, (u8 const *)p, n); }
TG_INTERNAL void tgb_str(tgBytes *b, tgString s) {
	tgb_bytes(b, s.text, s.len);
	tg_array_add(b, (u8)0);
}
TG_INTERNAL void tgb_cstr(tgBytes *b, char const *s) {
	tgb_str(b, tg_str(s));
}
TG_INTERNAL void tgb_uleb(tgBytes *b, u64 v) {
	do {
		u8 byte = v & 0x7f;
		v >>= 7;
		if (v != 0) byte |= 0x80;
		tg_array_add(b, byte);
	} while (v != 0);
}
TG_INTERNAL void tgb_sleb(tgBytes *b, i64 v) {
	bool more = true;
	while (more) {
		u8 byte = v & 0x7f;
		v >>= 7;
		if ((v == 0 && (byte & 0x40) == 0) || (v == -1 && (byte & 0x40) != 0)) {
			more = false;
		} else {
			byte |= 0x80;
		}
		tg_array_add(b, byte);
	}
}
TG_INTERNAL void tgb_align(tgBytes *b, isize align, u8 fill) {
	while (b->count % align != 0) tg_array_add(b, fill);
}
TG_INTERNAL void tgb_patch_u32(tgBytes *b, isize at, u32 v) {
	memmove(b->data + at, &v, 4);
}

////////////////////////////////////////////////////////////////
// DWARF
////////////////////////////////////////////////////////////////

enum {
	TDW_TAG_array_type       = 0x01,
	TDW_TAG_class_type       = 0x02,
	TDW_TAG_enumeration_type = 0x04,
	TDW_TAG_formal_parameter = 0x05,
	TDW_TAG_lexical_block    = 0x0b,
	TDW_TAG_member           = 0x0d,
	TDW_TAG_pointer_type     = 0x0f,
	TDW_TAG_compile_unit     = 0x11,
	TDW_TAG_structure_type   = 0x13,
	TDW_TAG_subroutine_type  = 0x15,
	TDW_TAG_typedef          = 0x16,
	TDW_TAG_union_type       = 0x17,
	TDW_TAG_unspecified_parameters = 0x18,
	TDW_TAG_inlined_subroutine = 0x1d,
	TDW_TAG_subrange_type    = 0x21,
	TDW_TAG_base_type        = 0x24,
	TDW_TAG_const_type       = 0x26,
	TDW_TAG_enumerator       = 0x28,
	TDW_TAG_subprogram       = 0x2e,
	TDW_TAG_variable         = 0x34,
	TDW_TAG_volatile_type    = 0x35,
	TDW_TAG_unspecified_type = 0x3b,

	TDW_CHILDREN_no  = 0,
	TDW_CHILDREN_yes = 1,

	TDW_AT_location        = 0x02,
	TDW_AT_name            = 0x03,
	TDW_AT_byte_size       = 0x0b,
	TDW_AT_bit_size        = 0x0d,
	TDW_AT_stmt_list       = 0x10,
	TDW_AT_low_pc          = 0x11,
	TDW_AT_high_pc         = 0x12,
	TDW_AT_language        = 0x13,
	TDW_AT_comp_dir        = 0x1b,
	TDW_AT_const_value     = 0x1c,
	TDW_AT_inline          = 0x20,
	TDW_AT_producer        = 0x25,
	TDW_AT_prototyped      = 0x27,
	TDW_AT_upper_bound     = 0x2f,
	TDW_AT_abstract_origin = 0x31,
	TDW_AT_count           = 0x37,
	TDW_AT_data_member_location = 0x38,
	TDW_AT_decl_file       = 0x3a,
	TDW_AT_decl_line       = 0x3b,
	TDW_AT_declaration     = 0x3c,
	TDW_AT_encoding        = 0x3e,
	TDW_AT_external        = 0x3f,
	TDW_AT_frame_base      = 0x40,
	TDW_AT_type            = 0x49,
	TDW_AT_ranges          = 0x55,
	TDW_AT_call_column     = 0x57,
	TDW_AT_call_file       = 0x58,
	TDW_AT_call_line       = 0x59,
	TDW_AT_data_bit_offset = 0x6b,
	TDW_AT_linkage_name    = 0x6e,

	TDW_INL_declared_inlined = 3,

	TDW_FORM_addr         = 0x01,
	TDW_FORM_data2        = 0x05,
	TDW_FORM_data4        = 0x06,
	TDW_FORM_data8        = 0x07,
	TDW_FORM_string       = 0x08,
	TDW_FORM_data1        = 0x0b,
	TDW_FORM_flag         = 0x0c,
	TDW_FORM_sdata        = 0x0d,
	TDW_FORM_udata        = 0x0f,
	TDW_FORM_ref4         = 0x13,
	TDW_FORM_sec_offset   = 0x17,
	TDW_FORM_exprloc      = 0x18,
	TDW_FORM_flag_present = 0x19,

	TDW_ATE_boolean       = 0x02,
	TDW_ATE_float         = 0x04,
	TDW_ATE_signed        = 0x05,
	TDW_ATE_signed_char   = 0x06,
	TDW_ATE_unsigned      = 0x07,
	TDW_ATE_unsigned_char = 0x08,
	TDW_ATE_UTF           = 0x10,

	TDW_OP_addr    = 0x03,
	TDW_OP_deref   = 0x06,
	TDW_OP_const8u = 0x0e,
	TDW_OP_GNU_push_tls_address = 0xe0,
	TDW_OP_form_tls_address = 0x9b,
	TDW_OP_fbreg = 0x91,
	TDW_OP_reg0  = 0x50,
	TDW_OP_reg6  = 0x56,
	TDW_OP_breg0 = 0x70,
	TDW_OP_regx  = 0x90,
	TDW_OP_constu = 0x10,
	TDW_OP_and    = 0x1a,
	TDW_OP_plus_uconst = 0x23,
	TDW_OP_call_frame_cfa = 0x9c,

	TDW_LANG_C99 = 0x0c,
};

// Variants of the abbreviations of types that may have a name and a declaration position.
enum {
	tgAbbrevVar_Anon      = 0, // no name, no decl_file/decl_line
	tgAbbrevVar_Named     = 1,
	tgAbbrevVar_AnonDecl  = 2, // decl_file and decl_line
	tgAbbrevVar_NamedDecl = 3,
	tgAbbrevVar_Incomplete = 4, // a record's declaration: its name and DW_AT_declaration
	tgAbbrevVar_COUNT     = 5,
};

typedef u32 tgAbbrev;
enum {
	tgAbbrev_None,
	tgAbbrev_CompileUnit,
	tgAbbrev_Subprogram,
	tgAbbrev_SubprogramNoChildren,
	tgAbbrev_Param,
	tgAbbrev_Var,
	tgAbbrev_BaseType,
	tgAbbrev_PointerType,
	tgAbbrev_VoidPointerType,
	tgAbbrev_ConstType,
	tgAbbrev_ConstVoidType,
	tgAbbrev_VolatileType,
	tgAbbrev_VolatileVoidType,
	tgAbbrev_Member,
	tgAbbrev_MemberAnon,
	tgAbbrev_BitMember,
	tgAbbrev_ArrayType,
	tgAbbrev_Subrange,
	tgAbbrev_SubrangeNoCount,
	tgAbbrev_Typedef,
	tgAbbrev_TypedefDecl,
	tgAbbrev_TypedefVoid,
	tgAbbrev_Enumerator,
	tgAbbrev_EnumeratorUnsigned,
	tgAbbrev_SubroutineType,
	tgAbbrev_SubroutineTypeVoid,
	tgAbbrev_SubroutineTypeLeaf,
	tgAbbrev_SubroutineTypeVoidLeaf,
	tgAbbrev_FormalParameterType,
	tgAbbrev_UnspecifiedParameters,
	tgAbbrev_UnspecifiedType,
	tgAbbrev_GlobalVar,
	tgAbbrev_Constant,
	tgAbbrev_SubprogramRet,
	tgAbbrev_SubprogramRetNoChildren,
	tgAbbrev_LexicalBlock,
	tgAbbrev_LexicalBlockRanges,
	tgAbbrev_AbstractSubprogram,
	tgAbbrev_InlinedSubroutine,
	tgAbbrev_InlinedSubroutineRanges,
	tgAbbrev_CompileUnitRanges, // code outside .text too, see tyg_proc_section
	// + tgAbbrevVar_*, Incomplete excluded
	tgAbbrev_EnumType,
	// + 5*record kind + tgAbbrevVar_*: struct, union, class
	tgAbbrev_RecordType = tgAbbrev_EnumType + tgAbbrevVar_Incomplete,
	tgAbbrev_COUNT = tgAbbrev_RecordType + 3*tgAbbrevVar_COUNT,
};

// One abbreviation; `attrs` are attribute and form pairs, ended by a 0.
TG_INTERNAL void tg_dwarf_abbrev(tgBytes *b, tgAbbrev code, u32 tag, bool children, u32 const *attrs) {
	tgb_uleb(b, code);
	tgb_uleb(b, tag);
	tgb_u8(b, (u8)(children ? TDW_CHILDREN_yes : TDW_CHILDREN_no));
	for (isize i = 0; attrs[i] != 0; i += 2) {
		tgb_uleb(b, attrs[i]);
		tgb_uleb(b, attrs[i+1]);
	}
	tgb_u8(b, 0);
	tgb_u8(b, 0);
}

#define TG_ABBREV(code, tag, children, ...) do { \
	u32 const tg__attrs[] = {__VA_ARGS__}; \
	tg_dwarf_abbrev(b, (code), (tag), (children), tg__attrs); \
} while (0)

// The name and decl position attributes of a type's variant, then `rest`, into `out`.
TG_INTERNAL void tg_dwarf_variant_attrs(u32 *out, i32 variant, u32 const *rest) {
	isize n = 0;
	if (variant == tgAbbrevVar_Incomplete) {
		out[n++] = TDW_AT_name; out[n++] = TDW_FORM_string;
		out[n++] = TDW_AT_declaration; out[n++] = TDW_FORM_flag_present;
		out[n] = 0;
		return;
	}
	if (variant & 1) { out[n++] = TDW_AT_name; out[n++] = TDW_FORM_string; }
	for (isize i = 0; rest[i] != 0; i++) out[n++] = rest[i];
	if (variant & 2) {
		out[n++] = TDW_AT_decl_file; out[n++] = TDW_FORM_udata;
		out[n++] = TDW_AT_decl_line; out[n++] = TDW_FORM_udata;
	}
	out[n] = 0;
}

TG_INTERNAL void tg_dwarf_abbrevs(tgBytes *b, bool cu_ranges) {
	TG_ABBREV(tgAbbrev_CompileUnit, TDW_TAG_compile_unit, true,
		TDW_AT_producer, TDW_FORM_string,
		TDW_AT_language, TDW_FORM_data2,
		TDW_AT_name, TDW_FORM_string,
		TDW_AT_comp_dir, TDW_FORM_string,
		TDW_AT_low_pc, TDW_FORM_addr,
		TDW_AT_high_pc, TDW_FORM_data8,
		TDW_AT_stmt_list, TDW_FORM_sec_offset, 0);
	if (cu_ranges) {
		// low_pc, the start of .text, is the base of the ranges that do not name their own
		TG_ABBREV(tgAbbrev_CompileUnitRanges, TDW_TAG_compile_unit, true,
			TDW_AT_producer, TDW_FORM_string,
			TDW_AT_language, TDW_FORM_data2,
			TDW_AT_name, TDW_FORM_string,
			TDW_AT_comp_dir, TDW_FORM_string,
			TDW_AT_low_pc, TDW_FORM_addr,
			TDW_AT_ranges, TDW_FORM_sec_offset,
			TDW_AT_stmt_list, TDW_FORM_sec_offset, 0);
	}
	TG_ABBREV(tgAbbrev_Subprogram, TDW_TAG_subprogram, true,
		TDW_AT_name, TDW_FORM_string,
		TDW_AT_linkage_name, TDW_FORM_string,
		TDW_AT_low_pc, TDW_FORM_addr,
		TDW_AT_high_pc, TDW_FORM_data4,
		TDW_AT_frame_base, TDW_FORM_exprloc,
		TDW_AT_decl_file, TDW_FORM_udata,
		TDW_AT_decl_line, TDW_FORM_udata,
		TDW_AT_external, TDW_FORM_flag, 0);
	TG_ABBREV(tgAbbrev_SubprogramNoChildren, TDW_TAG_subprogram, false,
		TDW_AT_name, TDW_FORM_string,
		TDW_AT_linkage_name, TDW_FORM_string,
		TDW_AT_low_pc, TDW_FORM_addr,
		TDW_AT_high_pc, TDW_FORM_data4,
		TDW_AT_frame_base, TDW_FORM_exprloc,
		TDW_AT_decl_file, TDW_FORM_udata,
		TDW_AT_decl_line, TDW_FORM_udata,
		TDW_AT_external, TDW_FORM_flag, 0);
	TG_ABBREV(tgAbbrev_SubprogramRet, TDW_TAG_subprogram, true,
		TDW_AT_name, TDW_FORM_string,
		TDW_AT_linkage_name, TDW_FORM_string,
		TDW_AT_low_pc, TDW_FORM_addr,
		TDW_AT_high_pc, TDW_FORM_data4,
		TDW_AT_frame_base, TDW_FORM_exprloc,
		TDW_AT_decl_file, TDW_FORM_udata,
		TDW_AT_decl_line, TDW_FORM_udata,
		TDW_AT_external, TDW_FORM_flag,
		TDW_AT_type, TDW_FORM_ref4, 0);
	TG_ABBREV(tgAbbrev_SubprogramRetNoChildren, TDW_TAG_subprogram, false,
		TDW_AT_name, TDW_FORM_string,
		TDW_AT_linkage_name, TDW_FORM_string,
		TDW_AT_low_pc, TDW_FORM_addr,
		TDW_AT_high_pc, TDW_FORM_data4,
		TDW_AT_frame_base, TDW_FORM_exprloc,
		TDW_AT_decl_file, TDW_FORM_udata,
		TDW_AT_decl_line, TDW_FORM_udata,
		TDW_AT_external, TDW_FORM_flag,
		TDW_AT_type, TDW_FORM_ref4, 0);
	TG_ABBREV(tgAbbrev_Param, TDW_TAG_formal_parameter, false,
		TDW_AT_name, TDW_FORM_string,
		TDW_AT_location, TDW_FORM_exprloc,
		TDW_AT_decl_line, TDW_FORM_udata,
		TDW_AT_type, TDW_FORM_ref4, 0);
	TG_ABBREV(tgAbbrev_Var, TDW_TAG_variable, false,
		TDW_AT_name, TDW_FORM_string,
		TDW_AT_location, TDW_FORM_exprloc,
		TDW_AT_decl_line, TDW_FORM_udata,
		TDW_AT_type, TDW_FORM_ref4, 0);
	TG_ABBREV(tgAbbrev_BaseType, TDW_TAG_base_type, false,
		TDW_AT_name, TDW_FORM_string,
		TDW_AT_encoding, TDW_FORM_data1,
		TDW_AT_byte_size, TDW_FORM_udata, 0);
	TG_ABBREV(tgAbbrev_PointerType, TDW_TAG_pointer_type, false,
		TDW_AT_byte_size, TDW_FORM_data1,
		TDW_AT_type, TDW_FORM_ref4, 0);
	// a pointer to no type is `void *`, as clang describes it
	TG_ABBREV(tgAbbrev_VoidPointerType, TDW_TAG_pointer_type, false,
		TDW_AT_byte_size, TDW_FORM_data1, 0);
	TG_ABBREV(tgAbbrev_ConstType, TDW_TAG_const_type, false,
		TDW_AT_type, TDW_FORM_ref4, 0);
	TG_ABBREV(tgAbbrev_ConstVoidType, TDW_TAG_const_type, false, 0);
	TG_ABBREV(tgAbbrev_VolatileType, TDW_TAG_volatile_type, false,
		TDW_AT_type, TDW_FORM_ref4, 0);
	TG_ABBREV(tgAbbrev_VolatileVoidType, TDW_TAG_volatile_type, false, 0);
	TG_ABBREV(tgAbbrev_Member, TDW_TAG_member, false,
		TDW_AT_name, TDW_FORM_string,
		TDW_AT_type, TDW_FORM_ref4,
		TDW_AT_data_member_location, TDW_FORM_udata, 0);
	TG_ABBREV(tgAbbrev_MemberAnon, TDW_TAG_member, false,
		TDW_AT_type, TDW_FORM_ref4,
		TDW_AT_data_member_location, TDW_FORM_udata, 0);
	TG_ABBREV(tgAbbrev_BitMember, TDW_TAG_member, false,
		TDW_AT_name, TDW_FORM_string,
		TDW_AT_type, TDW_FORM_ref4,
		TDW_AT_bit_size, TDW_FORM_udata,
		TDW_AT_data_bit_offset, TDW_FORM_udata, 0);
	TG_ABBREV(tgAbbrev_ArrayType, TDW_TAG_array_type, true,
		TDW_AT_type, TDW_FORM_ref4, 0);
	TG_ABBREV(tgAbbrev_Subrange, TDW_TAG_subrange_type, false,
		TDW_AT_count, TDW_FORM_udata, 0);
	TG_ABBREV(tgAbbrev_SubrangeNoCount, TDW_TAG_subrange_type, false, 0);
	TG_ABBREV(tgAbbrev_Typedef, TDW_TAG_typedef, false,
		TDW_AT_name, TDW_FORM_string,
		TDW_AT_type, TDW_FORM_ref4, 0);
	TG_ABBREV(tgAbbrev_TypedefDecl, TDW_TAG_typedef, false,
		TDW_AT_name, TDW_FORM_string,
		TDW_AT_type, TDW_FORM_ref4,
		TDW_AT_decl_file, TDW_FORM_udata,
		TDW_AT_decl_line, TDW_FORM_udata, 0);
	TG_ABBREV(tgAbbrev_TypedefVoid, TDW_TAG_typedef, false,
		TDW_AT_name, TDW_FORM_string, 0);
	TG_ABBREV(tgAbbrev_Enumerator, TDW_TAG_enumerator, false,
		TDW_AT_name, TDW_FORM_string,
		TDW_AT_const_value, TDW_FORM_sdata, 0);
	TG_ABBREV(tgAbbrev_EnumeratorUnsigned, TDW_TAG_enumerator, false,
		TDW_AT_name, TDW_FORM_string,
		TDW_AT_const_value, TDW_FORM_udata, 0);
	TG_ABBREV(tgAbbrev_SubroutineType, TDW_TAG_subroutine_type, true,
		TDW_AT_prototyped, TDW_FORM_flag_present,
		TDW_AT_type, TDW_FORM_ref4, 0);
	TG_ABBREV(tgAbbrev_SubroutineTypeVoid, TDW_TAG_subroutine_type, true,
		TDW_AT_prototyped, TDW_FORM_flag_present, 0);
	// `T (void)`: no parameter children, so no DW_CHILDREN_yes and no null entry
	TG_ABBREV(tgAbbrev_SubroutineTypeLeaf, TDW_TAG_subroutine_type, false,
		TDW_AT_prototyped, TDW_FORM_flag_present,
		TDW_AT_type, TDW_FORM_ref4, 0);
	TG_ABBREV(tgAbbrev_SubroutineTypeVoidLeaf, TDW_TAG_subroutine_type, false,
		TDW_AT_prototyped, TDW_FORM_flag_present, 0);
	TG_ABBREV(tgAbbrev_FormalParameterType, TDW_TAG_formal_parameter, false,
		TDW_AT_type, TDW_FORM_ref4, 0);
	TG_ABBREV(tgAbbrev_UnspecifiedParameters, TDW_TAG_unspecified_parameters, false, 0);
	TG_ABBREV(tgAbbrev_UnspecifiedType, TDW_TAG_unspecified_type, false,
		TDW_AT_name, TDW_FORM_string, 0);
	TG_ABBREV(tgAbbrev_GlobalVar, TDW_TAG_variable, false,
		TDW_AT_name, TDW_FORM_string,
		TDW_AT_type, TDW_FORM_ref4,
		TDW_AT_external, TDW_FORM_flag,
		TDW_AT_decl_file, TDW_FORM_udata,
		TDW_AT_decl_line, TDW_FORM_udata,
		TDW_AT_location, TDW_FORM_exprloc, 0);
	TG_ABBREV(tgAbbrev_Constant, TDW_TAG_variable, false,
		TDW_AT_name, TDW_FORM_string,
		TDW_AT_type, TDW_FORM_ref4,
		TDW_AT_external, TDW_FORM_flag_present,
		TDW_AT_const_value, TDW_FORM_sdata, 0);
	TG_ABBREV(tgAbbrev_LexicalBlock, TDW_TAG_lexical_block, true,
		TDW_AT_low_pc, TDW_FORM_addr,
		TDW_AT_high_pc, TDW_FORM_data4, 0);
	TG_ABBREV(tgAbbrev_LexicalBlockRanges, TDW_TAG_lexical_block, true,
		TDW_AT_ranges, TDW_FORM_sec_offset, 0);
	// an inlined procedure, which inlined_subroutine entries refer to
	TG_ABBREV(tgAbbrev_AbstractSubprogram, TDW_TAG_subprogram, false,
		TDW_AT_name, TDW_FORM_string,
		TDW_AT_decl_file, TDW_FORM_udata,
		TDW_AT_decl_line, TDW_FORM_udata,
		TDW_AT_inline, TDW_FORM_data1, 0);
	TG_ABBREV(tgAbbrev_InlinedSubroutine, TDW_TAG_inlined_subroutine, true,
		TDW_AT_abstract_origin, TDW_FORM_ref4,
		TDW_AT_low_pc, TDW_FORM_addr,
		TDW_AT_high_pc, TDW_FORM_data4,
		TDW_AT_call_file, TDW_FORM_udata,
		TDW_AT_call_line, TDW_FORM_udata,
		TDW_AT_call_column, TDW_FORM_udata, 0);
	TG_ABBREV(tgAbbrev_InlinedSubroutineRanges, TDW_TAG_inlined_subroutine, true,
		TDW_AT_abstract_origin, TDW_FORM_ref4,
		TDW_AT_ranges, TDW_FORM_sec_offset,
		TDW_AT_call_file, TDW_FORM_udata,
		TDW_AT_call_line, TDW_FORM_udata,
		TDW_AT_call_column, TDW_FORM_udata, 0);

	u32 attrs[32];
	for (i32 v = 0; v < tgAbbrevVar_Incomplete; v++) {
		u32 const rest[] = {TDW_AT_type, TDW_FORM_ref4, TDW_AT_byte_size, TDW_FORM_udata, 0};
		tg_dwarf_variant_attrs(attrs, v, rest);
		tg_dwarf_abbrev(b, (tgAbbrev)(tgAbbrev_EnumType + v), TDW_TAG_enumeration_type, true, attrs);
	}
	u32 const record_tags[3] = {TDW_TAG_structure_type, TDW_TAG_union_type, TDW_TAG_class_type};
	for (i32 k = 0; k < 3; k++) {
		for (i32 v = 0; v < tgAbbrevVar_COUNT; v++) {
			u32 const rest[] = {TDW_AT_byte_size, TDW_FORM_udata, 0};
			tg_dwarf_variant_attrs(attrs, v, rest);
			tg_dwarf_abbrev(b, (tgAbbrev)(tgAbbrev_RecordType + tgAbbrevVar_COUNT*k + v), record_tags[k], v != tgAbbrevVar_Incomplete, attrs);
		}
	}
	tgb_u8(b, 0);
}

#undef TG_ABBREV

// A reference to a type whose DIE is written later.
typedef struct tgDwarfPendingType {
	isize             at;
	tyg_dtype const * type;
} tgDwarfPendingType;

TG_ARRAY(tgDwarfPendingTypeArray, tgDwarfPendingType);
TG_ARRAY(tgDtypeConstPtrArray, tyg_dtype const *);

typedef struct tgDwarfTypes {
	tgBytes *               info;
	tgMap                   offsets;  // type -> DIE offset in .debug_info
	// types whose DIE is referenced before it is written
	tgDwarfPendingTypeArray pending;
	tgDtypeConstPtrArray    queue;
	tgMap                   queued;
	isize                   cu_start;
} tgDwarfTypes;

// What a reference to no type, or to void where a type is required, refers to.
static tyg_dtype const tg_dwarf_void_type;

TG_INTERNAL bool tg_dt_is_void(tyg_dtype const *t) {
	return t == NULL || t->kind == tgDt_Void;
}

TG_INTERNAL tyg_dtype const *tg_dwarf_ref_type(tyg_dtype const *t) {
	return t == NULL ? &tg_dwarf_void_type : t;
}

TG_INTERNAL void tg_dwarf_queue(tgDwarfTypes *dt, tyg_dtype const *t) {
	u64 key = (u64)(uintptr_t)t;
	if (tg_map_get(&dt->queued, key) == NULL) {
		tg_map_set(&dt->queued, key, 1);
		tg_array_add(&dt->queue, t);
	}
}

TG_INTERNAL void tg_dwarf_type_ref(tgDwarfTypes *dt, tyg_dtype const *t) {
	// writes a ref4 placeholder, patched once the type is written
	t = tg_dwarf_ref_type(t);
	tgDwarfPendingType pd;
	pd.at = dt->info->count;
	pd.type = t;
	tgb_u32(dt->info, 0);
	tg_array_add(&dt->pending, pd);
	tg_dwarf_queue(dt, t);
}

// The variant of a type's abbreviation by whether it has a name and a position.
TG_INTERNAL i32 tg_dwarf_variant(tyg_dtype const *t) {
	return (t->name.len > 0 ? tgAbbrevVar_Named : 0) | (t->file_id > 0 ? tgAbbrevVar_AnonDecl : 0);
}

TG_INTERNAL void tg_dwarf_variant_fields(tgBytes *b, tyg_dtype const *t, i32 variant, bool before) {
	if (before) {
		if (variant & 1) tgb_str(b, t->name);
	} else {
		if (variant & 2) {
			tgb_uleb(b, (u64)t->file_id);
			tgb_uleb(b, (u64)tg_max(t->line, 0));
		}
	}
}

// Whether an enum's values are unsigned, by its underlying type.
TG_INTERNAL bool tg_dwarf_enum_unsigned(tyg_dtype const *t) {
	while (t != NULL && (t->kind == tgDt_Typedef || t->kind == tgDt_Const || t->kind == tgDt_Volatile)) t = t->target;
	if (t == NULL || t->kind != tgDt_Base) return false;
	return t->encoding == TYG_DT_UNSIGNED || t->encoding == TYG_DT_UNSIGNED_CHAR || t->encoding == TYG_DT_BOOL;
}

TG_INTERNAL void tg_dwarf_write_type(tgDwarfTypes *dt, tyg_dtype const *t) {
	tgBytes *b = dt->info;
	tg_map_set(&dt->offsets, (u64)(uintptr_t)t, (i64)b->count);

	switch (t->kind) {
	case tgDt_Void:
		// only referenced where a type is required, e.g. a variable of type void
		tgb_uleb(b, tgAbbrev_UnspecifiedType);
		tgb_str(b, t->name.len > 0 ? t->name : tg_str_lit("void"));
		return;
	case tgDt_Base: {
		u8 enc = TDW_ATE_signed;
		switch (t->encoding) {
		case TYG_DT_SIGNED:        enc = TDW_ATE_signed; break;
		case TYG_DT_UNSIGNED:      enc = TDW_ATE_unsigned; break;
		case TYG_DT_SIGNED_CHAR:   enc = TDW_ATE_signed_char; break;
		case TYG_DT_UNSIGNED_CHAR: enc = TDW_ATE_unsigned_char; break;
		case TYG_DT_FLOAT:         enc = TDW_ATE_float; break;
		case TYG_DT_BOOL:          enc = TDW_ATE_boolean; break;
		}
		tgb_uleb(b, tgAbbrev_BaseType);
		tgb_str(b, t->name);
		tgb_u8(b, enc);
		tgb_uleb(b, (u64)t->size);
		return;
	}
	case tgDt_Pointer:
		if (tg_dt_is_void(t->target)) {
			tgb_uleb(b, tgAbbrev_VoidPointerType);
			tgb_u8(b, 8);
			return;
		}
		tgb_uleb(b, tgAbbrev_PointerType);
		tgb_u8(b, 8);
		tg_dwarf_type_ref(dt, t->target);
		return;
	case tgDt_Const:
		if (tg_dt_is_void(t->target)) {
			tgb_uleb(b, tgAbbrev_ConstVoidType);
			return;
		}
		tgb_uleb(b, tgAbbrev_ConstType);
		tg_dwarf_type_ref(dt, t->target);
		return;
	case tgDt_Volatile:
		if (tg_dt_is_void(t->target)) {
			tgb_uleb(b, tgAbbrev_VolatileVoidType);
			return;
		}
		tgb_uleb(b, tgAbbrev_VolatileType);
		tg_dwarf_type_ref(dt, t->target);
		return;
	case tgDt_Typedef:
		if (tg_dt_is_void(t->target)) {
			tgb_uleb(b, tgAbbrev_TypedefVoid);
			tgb_str(b, t->name);
			return;
		}
		tgb_uleb(b, t->file_id > 0 ? tgAbbrev_TypedefDecl : tgAbbrev_Typedef);
		tgb_str(b, t->name);
		tg_dwarf_type_ref(dt, t->target);
		if (t->file_id > 0) {
			tgb_uleb(b, (u64)t->file_id);
			tgb_uleb(b, (u64)tg_max(t->line, 0));
		}
		return;
	case tgDt_Array:
		tgb_uleb(b, tgAbbrev_ArrayType);
		tg_dwarf_type_ref(dt, t->target);
		if (t->count >= 0) {
			tgb_uleb(b, tgAbbrev_Subrange);
			tgb_uleb(b, (u64)t->count);
		} else {
			tgb_uleb(b, tgAbbrev_SubrangeNoCount);
		}
		tgb_u8(b, 0);
		return;
	case tgDt_Func: {
		bool leaf = t->param_count == 0 && !t->variadic;
		if (tg_dt_is_void(t->target)) {
			tgb_uleb(b, leaf ? tgAbbrev_SubroutineTypeVoidLeaf : tgAbbrev_SubroutineTypeVoid);
		} else {
			tgb_uleb(b, leaf ? tgAbbrev_SubroutineTypeLeaf : tgAbbrev_SubroutineType);
			tg_dwarf_type_ref(dt, t->target);
		}
		if (leaf) return;
		for (i32 i = 0; i < t->param_count; i++) {
			tgb_uleb(b, tgAbbrev_FormalParameterType);
			tg_dwarf_type_ref(dt, t->params[i]);
		}
		if (t->variadic) tgb_uleb(b, tgAbbrev_UnspecifiedParameters);
		tgb_u8(b, 0);
		return;
	}
	case tgDt_Record: {
		i32 kind = t->record_kind <= TYG_DT_CLASS ? t->record_kind : TYG_DT_STRUCT;
		tgAbbrev base = (tgAbbrev)(tgAbbrev_RecordType + tgAbbrevVar_COUNT*kind);
		if (!t->complete) {
			tgb_uleb(b, base + tgAbbrevVar_Incomplete);
			tgb_str(b, t->name);
			return;
		}
		i32 variant = tg_dwarf_variant(t);
		tgb_uleb(b, base + (u32)variant);
		tg_dwarf_variant_fields(b, t, variant, true);
		tgb_uleb(b, (u64)t->size);
		tg_dwarf_variant_fields(b, t, variant, false);
		for (isize i = 0; i < t->members.count; i++) {
			tgDtMember const *f = &t->members.data[i];
			if (f->bit_size > 0) {
				if (f->name.len == 0) continue; // padding, which clang leaves out too
				tgb_uleb(b, tgAbbrev_BitMember);
				tgb_str(b, f->name);
				tg_dwarf_type_ref(dt, f->type);
				tgb_uleb(b, (u64)f->bit_size);
				tgb_uleb(b, (u64)f->bit_offset);
				continue;
			}
			if (f->name.len > 0) {
				tgb_uleb(b, tgAbbrev_Member);
				tgb_str(b, f->name);
			} else {
				tgb_uleb(b, tgAbbrev_MemberAnon);
			}
			tg_dwarf_type_ref(dt, f->type);
			tgb_uleb(b, (u64)f->offset);
		}
		tgb_u8(b, 0);
		return;
	}
	case tgDt_Enum: {
		i32 variant = tg_dwarf_variant(t);
		i64 size = t->size;
		if (size == 0 && t->target != NULL) size = t->target->size;
		if (size == 0) size = 4;
		bool is_unsigned = tg_dwarf_enum_unsigned(t->target);
		tgb_uleb(b, tgAbbrev_EnumType + (u32)variant);
		tg_dwarf_variant_fields(b, t, variant, true);
		tg_dwarf_type_ref(dt, t->target);
		tgb_uleb(b, (u64)size);
		tg_dwarf_variant_fields(b, t, variant, false);
		for (isize i = 0; i < t->enumerators.count; i++) {
			tgDtEnumerator const *e = &t->enumerators.data[i];
			tgb_uleb(b, is_unsigned ? tgAbbrev_EnumeratorUnsigned : tgAbbrev_Enumerator);
			tgb_str(b, e->name);
			if (is_unsigned) tgb_uleb(b, (u64)e->value); else tgb_sleb(b, e->value);
		}
		tgb_u8(b, 0);
		return;
	}
	}
	TG_PANIC("unknown debug type kind");
}

// An address in .debug_info, .debug_line or .debug_ranges, relocated by the object writer.
typedef struct tgDwarfAddr {
	isize     offset;  // in the debug section
	i32       sym;     // a module symbol, or -1 for a code section
	i64       addend;  // the offset in the code section, when sym < 0
	tgSection section; // when sym < 0: .text, or a named section of code (ELF only)
} tgDwarfAddr;

TG_ARRAY(tgDwarfAddrArray, tgDwarfAddr);

// The DWARF sections, which every object format shares but for their relocations.
typedef struct tgDwarf {
	tgBytes abbrev;
	tgBytes info;
	tgBytes line;
	tgBytes ranges;
	tgDwarfAddrArray info_addrs;
	tgDwarfAddrArray line_addrs;
	tgDwarfAddrArray ranges_addrs; // only for code outside .text, see tyg_proc_section
	tgIsizeArray ranges_refs; // in .debug_info, the offsets into .debug_ranges, which ELF relocates
	isize abbrev_offset_at; // in .debug_info, the offsets into .debug_abbrev and .debug_line
	isize stmt_list_at;
} tgDwarf;

TG_INTERNAL void tg_dwarf_free(tgDwarf *d) {
	tg_array_free(&d->abbrev);
	tg_array_free(&d->info);
	tg_array_free(&d->line);
	tg_array_free(&d->ranges);
	tg_array_free(&d->info_addrs);
	tg_array_free(&d->line_addrs);
	tg_array_free(&d->ranges_addrs);
	tg_array_free(&d->ranges_refs);
}

// Starts a range list at the address of `addend` in `section`, a named section of code: a
// base address selection entry, since the list's base is otherwise the start of .text.
TG_INTERNAL void tg_dwarf_range_base(tgBytes *ranges, tgDwarfAddrArray *addrs, tgSection section, i64 addend) {
	tgDwarfAddr a;
	tgb_u64(ranges, ~(u64)0);
	a.offset = ranges->count;
	a.sym = -1;
	a.addend = addend;
	a.section = section;
	tg_array_add(addrs, a);
	tgb_u64(ranges, 0);
}

// The lexical blocks of a procedure. Like LLVM, a scope gets one when it has variables of its
// own and code; otherwise its variables and blocks go to the parent's block.
typedef struct tgDwarfScopeRange { i32 lo, hi, next; } tgDwarfScopeRange;
TG_ARRAY(tgDwarfScopeRangeArray, tgDwarfScopeRange);

typedef struct tgDwarfScopes {
	tgI32Array eff;                  // per scope: the scope whose block holds its variables
	tgI32Array kid, sib;             // the first block inside a block, the next block beside it
	tgI32Array head, tail;           // per scope: its code ranges, a list in `pool`
	tgI32Array var_head, var_tail, var_next; // the variables a block holds, in order
	tgI32Array site;                 // per scope: its index in inline_sites, or -1
	tgDwarfScopeRangeArray pool;
} tgDwarfScopes;

TG_INTERNAL i32 tg_dwarf_scope_parent(tgProcDebug const *pd, i32 s) {
	return s < pd->scope_parent.count ? pd->scope_parent.data[s] : -1;
}

TG_INTERNAL void tg_dwarf_scopes(tgDwarfScopes *sc, tgProcDebug const *pd, i32 len) {
	isize n = tg_max(pd->scope_parent.count, (isize)1);
	tg_array_resize(&sc->eff, n);
	tg_array_resize(&sc->kid, n);
	tg_array_resize(&sc->sib, n);
	tg_array_resize(&sc->head, n);
	tg_array_resize(&sc->tail, n);
	tg_array_resize(&sc->var_head, n);
	tg_array_resize(&sc->var_tail, n);
	tg_array_resize(&sc->var_next, pd->vars.count);
	tg_array_resize(&sc->site, n);
	sc->pool.count = 0;
	for (isize s = 0; s < n; s++) {
		sc->eff.data[s] = 0; // 1 here: the scope has variables of its own, or is an inlined body
		sc->kid.data[s] = sc->sib.data[s] = sc->head.data[s] = sc->tail.data[s] = sc->var_head.data[s] = sc->var_tail.data[s] = sc->site.data[s] = -1;
	}
	for (isize i = 0; i < pd->vars.count; i++) {
		tgDebugVar const *v = &pd->vars.data[i];
		if (v->scope < n) sc->eff.data[v->scope] = 1;
	}
	for (isize i = 0; i < pd->inline_sites.count; i++) {
		i32 s = pd->inline_sites.data[i].scope;
		if (s > 0 && s < n) {
			sc->eff.data[s] = 1;
			sc->site.data[s] = (i32)i;
		}
	}
	// the code of a scope, with its nested scopes' and its cold blocks', so it may be in pieces
	tgScopeMarkArray const *marks = &pd->scope_marks;
	for (isize k = 0; k < marks->count; k++) {
		i32 lo = marks->data[k].code_offset;
		i32 hi = k+1 < marks->count ? marks->data[k+1].code_offset : len;
		if (hi <= lo) continue;
		for (i32 s = marks->data[k].scope; s > 0; s = tg_dwarf_scope_parent(pd, s)) {
			if (sc->eff.data[s] == 0) continue;
			i32 t = sc->tail.data[s];
			if (t >= 0 && sc->pool.data[t].hi == lo) {
				sc->pool.data[t].hi = hi;
				continue;
			}
			tgDwarfScopeRange r;
			r.lo = lo; r.hi = hi; r.next = -1;
			i32 at = (i32)sc->pool.count;
			tg_array_add(&sc->pool, r);
			if (t >= 0) sc->pool.data[t].next = at; else sc->head.data[s] = at;
			sc->tail.data[s] = at;
		}
	}
	// parents first, by the ids; kids are linked in reverse, then put back in order
	sc->eff.data[0] = 0;
	for (isize s = 1; s < n; s++) {
		i32 pe = sc->eff.data[tg_dwarf_scope_parent(pd, (i32)s)];
		if (sc->eff.data[s] == 1 && sc->head.data[s] >= 0) {
			sc->eff.data[s] = (i32)s;
			sc->sib.data[s] = sc->kid.data[pe];
			sc->kid.data[pe] = (i32)s;
		} else {
			sc->eff.data[s] = pe;
		}
	}
	for (isize s = 0; s < n; s++) {
		i32 prev = -1;
		for (i32 c = sc->kid.data[s]; c >= 0; ) {
			i32 next = sc->sib.data[c];
			sc->sib.data[c] = prev;
			prev = c;
			c = next;
		}
		sc->kid.data[s] = prev;
	}
	for (isize i = 0; i < pd->vars.count; i++) {
		i32 s = pd->vars.data[i].scope < n ? sc->eff.data[pd->vars.data[i].scope] : 0;
		sc->var_next.data[i] = -1;
		if (sc->var_tail.data[s] >= 0) sc->var_next.data[sc->var_tail.data[s]] = (i32)i; else sc->var_head.data[s] = (i32)i;
		sc->var_tail.data[s] = (i32)i;
	}
}

// DW_AT_location of a symbol's storage: its address, or its offset in the thread's TLS block.
// On macOS the symbol of a thread local is its TLV descriptor, which lldb resolves with form_tls_address.
// Like LLVM, a thread local on arm64 Linux has an empty location: its linkers reject the relocation.
// So does one on Windows, where gdb has no way to find the thread's block from DWARF.
TG_INTERNAL void tg_dwarf_symbol_location(tgModule *m, tgBytes *b, tgDwarfAddrArray *addrs, i32 sym) {
	bool tls = (m->symbols.data[sym].flags & tgSymbolFlag_TLS) != 0;
	if (tls && ((tg_is_arm64() && !tg_is_darwin()) || tg_is_windows())) {
		tgb_uleb(b, 0);
		return;
	}
	i64 realign = m->symbols.data[sym].realign;
	u8 round_buf[32];
	tgBytes round;
	memset(&round, 0, sizeof(round));
	if (realign != 0) {
		// up to the next multiple of the alignment
		tgb_u8(&round, (u8)TDW_OP_plus_uconst);
		tgb_uleb(&round, (u64)(realign-1));
		tgb_u8(&round, (u8)TDW_OP_constu);
		tgb_uleb(&round, ~(u64)(realign-1));
		tgb_u8(&round, (u8)TDW_OP_and);
	}
	isize round_count = round.count;
	if (round_count) memcpy(round_buf, round.data, (usize)round_count);
	tg_array_free(&round);
	tgb_uleb(b, (tls ? 10 : 9) + (u64)round_count);
	tgb_u8(b, (u8)(tls ? TDW_OP_const8u : TDW_OP_addr));
	tgDwarfAddr r;
	r.offset = b->count;
	r.sym = sym;
	r.addend = 0;
	r.section = tgSection_Undef;
	tg_array_add(addrs, r);
	tgb_u64(b, 0);
	if (tls) tgb_u8(b, (u8)(tg_is_darwin() ? TDW_OP_form_tls_address : TDW_OP_GNU_push_tls_address));
	tgb_bytes(b, round_buf, round_count);
}

typedef struct tgDwarfProcs {
	tgModule *m;
	tgStrMap *abstract_dies; // inlined procedure -> its abstract entry's offset in the unit
	u8 frame_reg;
} tgDwarfProcs;

// The entries of procedures [begin, end), with offsets from the start of `info` and `ranges`.
// tg_dwarf_build appends them; the types they refer to are left to it.
typedef struct tgDwarfProcChunk {
	tgDwarfProcs *procs;
	isize begin, end;
	tgBytes info;
	tgBytes ranges;
	tgIsizeArray ranges_refs; // in info, the offsets into ranges
	tgDwarfAddrArray addrs;
	tgDwarfAddrArray ranges_addrs; // in ranges
	tgDwarfPendingTypeArray types;
	tgDwarfScopes sc;
	tgBytes expr; // a variable's location expression
} tgDwarfProcChunk;

TG_INTERNAL void tg_dwarf_chunk_type_ref(tgDwarfProcChunk *c, tyg_dtype const *t) {
	tgDwarfPendingType pd;
	pd.at = c->info.count;
	pd.type = tg_dwarf_ref_type(t);
	tg_array_add(&c->types, pd);
	tgb_u32(&c->info, 0);
}

TG_INTERNAL void tg_dwarf_chunk_var(tgDwarfProcChunk *c, tgDebugVar const *v) {
	tgBytes *b = &c->info;
	tgb_uleb(b, v->is_param ? tgAbbrev_Param : tgAbbrev_Var);
	tgb_str(b, v->name);
	if (v->local < 0) {
		tg_dwarf_symbol_location(c->procs->m, b, &c->addrs, v->sym);
		tgb_uleb(b, (u64)tg_max(v->line, 0));
		tg_dwarf_chunk_type_ref(c, v->type);
		return;
	}
	tgBytes *expr = &c->expr;
	tg_array_clear(expr);
	if (v->in_reg && v->by_ref) {
		tgb_u8(expr, (u8)(TDW_OP_breg0 + v->dwarf_reg));
		tgb_sleb(expr, 0);
	} else if (v->in_reg && v->dwarf_reg >= 32) {
		tgb_u8(expr, TDW_OP_regx);
		tgb_uleb(expr, v->dwarf_reg);
	} else if (v->in_reg) {
		tgb_u8(expr, (u8)(TDW_OP_reg0 + v->dwarf_reg));
	} else {
		// the frame pointer is the frame base
		tgb_u8(expr, TDW_OP_fbreg);
		tgb_sleb(expr, v->frame_offset_fixup);
		if (v->by_ref) {
			tgb_u8(expr, TDW_OP_deref);
		}
	}
	tgb_uleb(b, (u64)expr->count);
	tgb_bytes(b, expr->data, expr->count);
	tgb_uleb(b, (u64)tg_max(v->line, 0));
	tg_dwarf_chunk_type_ref(c, v->type);
}

TG_INTERNAL void tg_dwarf_chunk_origin(tgDwarfProcChunk *c, tgInlineSite const *site) {
	i64 *at = tg_strmap_get(c->procs->abstract_dies, site->name);
	TG_ASSERT(at != NULL);
	tgb_u32(&c->info, (u32)*at);
}

TG_INTERNAL void tg_dwarf_chunk_call_site(tgDwarfProcChunk *c, tgInlineSite const *site) {
	tgBytes *b = &c->info;
	tgb_uleb(b, (u64)tg_max(site->call_file, 1));
	tgb_uleb(b, (u64)tg_max(site->call_line, 0));
	tgb_uleb(b, (u64)tg_max(site->call_column, 0));
}

// a block's variables, then its blocks, nested like the scopes
TG_INTERNAL void tg_dwarf_chunk_block(tgDwarfProcChunk *c, tgProcDebug const *pd, i32 s) {
	tgBytes *b = &c->info;
	tgDwarfScopes const *sc = &c->sc;
	for (i32 i = sc->var_head.data[s]; i >= 0; i = sc->var_next.data[i]) tg_dwarf_chunk_var(c, &pd->vars.data[i]);
	for (i32 k = sc->kid.data[s]; k >= 0; k = sc->sib.data[k]) {
		tgDwarfScopeRange const *r = &sc->pool.data[sc->head.data[k]];
		tgInlineSite const *site = sc->site.data[k] >= 0 ? &pd->inline_sites.data[sc->site.data[k]] : NULL;
		if (r->next < 0) {
			tgb_uleb(b, site ? tgAbbrev_InlinedSubroutine : tgAbbrev_LexicalBlock);
			if (site) tg_dwarf_chunk_origin(c, site);
			tgDwarfAddr a;
			a.offset = b->count;
			a.sym = -1;
			a.addend = pd->start + r->lo;
			a.section = pd->section;
			tg_array_add(&c->addrs, a);
			tgb_u64(b, 0);
			tgb_u32(b, (u32)(r->hi - r->lo));
			if (site) tg_dwarf_chunk_call_site(c, site);
		} else {
			tgb_uleb(b, site ? tgAbbrev_InlinedSubroutineRanges : tgAbbrev_LexicalBlockRanges);
			if (site) tg_dwarf_chunk_origin(c, site);
			tg_array_add(&c->ranges_refs, b->count);
			tgb_u32(b, (u32)c->ranges.count);
			// offsets from the unit's low_pc, the start of the text section, or from the
			// procedure's start in another section
			i64 base = pd->start;
			if (pd->section != tgSection_Text) {
				tg_dwarf_range_base(&c->ranges, &c->ranges_addrs, pd->section, pd->start);
				base = 0;
			}
			for (i32 q = sc->head.data[k]; q >= 0; q = sc->pool.data[q].next) {
				tgb_u64(&c->ranges, (u64)(base + sc->pool.data[q].lo));
				tgb_u64(&c->ranges, (u64)(base + sc->pool.data[q].hi));
			}
			tgb_u64(&c->ranges, 0);
			tgb_u64(&c->ranges, 0);
			if (site) tg_dwarf_chunk_call_site(c, site);
		}
		tg_dwarf_chunk_block(c, pd, k);
		tgb_u8(b, 0);
	}
}

// Builds the chunk's entries into its arrays, which are empty. It reads the module and writes
// only the chunk; the scope arrays and `expr` keep their memory for the next chunk.
TG_INTERNAL void tg_dwarf_proc_chunk(tgDwarfProcChunk *c) {
	{
		tgModule *m = c->procs->m;
		tgBytes *b = &c->info;
		tgDwarfScopes *sc = &c->sc;

		for (isize i = c->begin; i < c->end; i++) {
			tgProcDebug const *pd = &m->proc_debug.data[i];
			bool has_children = pd->vars.count > 0 || pd->inline_sites.count > 0;
			// the result, so `finish` shows it
			tyg_dtype const *ret = NULL;
			if (pd->type != NULL && pd->type->kind == tgDt_Func && !tg_dt_is_void(pd->type->target)) {
				ret = pd->type->target;
			}
			if (ret) {
				tgb_uleb(b, has_children ? tgAbbrev_SubprogramRet : tgAbbrev_SubprogramRetNoChildren);
			} else {
				tgb_uleb(b, has_children ? tgAbbrev_Subprogram : tgAbbrev_SubprogramNoChildren);
			}
			// the source name, and the link name
			tgb_str(b, pd->name.len > 0 ? pd->name : pd->link_name);
			tgb_str(b, pd->link_name);
			tgDwarfAddr r;
			r.offset = b->count;
			r.sym = -1;
			r.addend = pd->start;
			r.section = pd->section;
			tg_array_add(&c->addrs, r);
			tgb_u64(b, 0);
			tgb_u32(b, (u32)(pd->end - pd->start));
			tgb_uleb(b, 1);
			tgb_u8(b, pd->naked ? TDW_OP_call_frame_cfa : c->procs->frame_reg);
			tgb_uleb(b, (u64)tg_max(pd->file_id, 1));
			tgb_uleb(b, (u64)tg_max(pd->line, 0));
			// external: visible to other objects, false for a local (static) symbol
			tgb_u8(b, (u8)(pd->sym >= 0 && (m->symbols.data[pd->sym].flags & tgSymbolFlag_Global) ? 1 : 0));
			if (ret) tg_dwarf_chunk_type_ref(c, ret);
			if (has_children) {
				tg_dwarf_scopes(sc, pd, (i32)(pd->end - pd->start));
				tg_dwarf_chunk_block(c, pd, 0);
				tgb_u8(b, 0);
			}
		}

	}
}

// The position of the last '/' past the first character, or -1.
TG_INTERNAL isize tg_dwarf_last_slash(tgString f) {
	for (isize j = f.len-1; j > 0; j--) if (f.text[j] == '/') return j;
	return -1;
}

// Builds the DWARF sections of the module. Free them with tg_dwarf_free.
TG_INTERNAL void tg_dwarf_build(tgModule *m, tgDwarf *d) {
	memset(d, 0, sizeof(*d));
	u8 const frame_reg = tg_is_arm64() ? TDW_OP_reg0 + 29 : TDW_OP_reg6; // x29 or rbp

	// code outside .text: the unit lists its sections' ranges
	bool cu_ranges = false;
	for (isize i = 0; i < m->proc_debug.count; i++) {
		if (m->proc_debug.data[i].section != tgSection_Text) cu_ranges = true;
	}
	tg_dwarf_abbrevs(&d->abbrev, cu_ranges);
	// the compilation directory, which file names in it are relative to. Apple's linker leaves
	// an object whose compile unit has an empty one out of the executable's debug map, so
	// dsymutil and lldb never find its DWARF: there it is at least ".".
	tgString cwd = m->comp_dir;
	if (cwd.len == 0 && tg_is_darwin()) cwd = tg_str_lit(".");

	// .debug_line
	{
		tgBytes *b = &d->line;
		isize start = b->count;
		tgb_u32(b, 0); // unit length
		tgb_u16(b, 4); // version
		isize header_len_at = b->count;
		tgb_u32(b, 0); // header length
		isize header_start = b->count;
		tgb_u8(b, 1);  // min instruction length
		tgb_u8(b, 1);  // max ops per instruction
		tgb_u8(b, 1);  // default is_stmt
		tgb_u8(b, (u8)(i8)-5); // line base
		tgb_u8(b, 14); // line range
		tgb_u8(b, 13); // opcode base
		u8 const std_lengths[12] = {0,1,1,1,1,0,0,0,1,0,0,1};
		tgb_bytes(b, std_lengths, 12);
		// directories and base names, so debuggers show short file names like LLVM's
		tgStrMap dirs;
		memset(&dirs, 0, sizeof(dirs));
		u64 *file_dirs = (u64 *)calloc((usize)tg_max(m->files.count, (isize)1), sizeof(u64));
		TG_ASSERT(file_dirs != NULL);
		for (isize i = 0; i < m->files.count; i++) {
			tgString f = m->files.data[i];
			isize slash = tg_dwarf_last_slash(f);
			file_dirs[i] = 0;
			if (slash < 0) continue;
			tgString dir = tg_str_make(f.text, slash);
			if (tg_str_eq(dir, cwd)) continue; // index 0 is the compilation directory
			i64 *found = tg_strmap_get(&dirs, dir);
			if (found == NULL) {
				tg_strmap_set(&dirs, dir, (i64)dirs.count + 1);
				tgb_str(b, dir);
				found = tg_strmap_get(&dirs, dir);
			}
			file_dirs[i] = (u64)*found;
		}
		tgb_u8(b, 0);
		for (isize i = 0; i < m->files.count; i++) {
			tgString f = m->files.data[i];
			isize slash = tg_dwarf_last_slash(f);
			bool short_name = slash >= 0 && (file_dirs[i] != 0 || tg_str_eq(tg_str_make(f.text, slash), cwd));
			tgb_str(b, short_name ? tg_str_make(f.text + slash + 1, f.len - slash - 1) : f);
			tgb_uleb(b, file_dirs[i]);
			tgb_uleb(b, 0);
			tgb_uleb(b, 0);
		}
		tg_strmap_free(&dirs);
		free(file_dirs);
		tgb_u8(b, 0);
		tgb_patch_u32(b, header_len_at, (u32)(b->count - header_start));

		for (isize pi = 0; pi < m->proc_debug.count; pi++) {
			tgProcDebug const *pd = &m->proc_debug.data[pi];
			// set address
			tgb_u8(b, 0);
			tgb_uleb(b, 9);
			tgb_u8(b, 2);
			tgDwarfAddr r;
			r.offset = b->count;
			r.sym = -1;
			r.addend = pd->start;
			r.section = pd->section;
			tg_array_add(&d->line_addrs, r);
			tgb_u64(b, 0);
			i64 cur_addr = 0;
			i64 cur_line = 1;
			i32 cur_file = 1;
			if (pd->line > 0 && (pd->line_entry_count == 0 || m->lines.data[pd->line_entry_start].code_offset != 0)) {
				// the prologue gets the declaration's line, debuggers look up the entry address
				if (pd->file_id > 0 && pd->file_id != cur_file) {
					tgb_u8(b, 4);
					tgb_uleb(b, (u64)pd->file_id);
					cur_file = pd->file_id;
				}
				tgb_u8(b, 3);
				tgb_sleb(b, pd->line - cur_line);
				cur_line = pd->line;
				tgb_u8(b, 1);
			}
			// the end of the prologue, which may follow the rows of the front end's parameter stores:
			// a row of its own, or the flag on the row at its address
			bool prologue_pending = pd->prologue_end > 0;
			for (i32 i = 0; i < pd->line_entry_count; i++) {
				tgLineEntry const *e = &m->lines.data[pd->line_entry_start + i];
				// a row that covers no code would still give its line an extra breakpoint location
				if (i+1 < pd->line_entry_count && m->lines.data[pd->line_entry_start + i+1].code_offset == e->code_offset) continue;
				if (prologue_pending && e->code_offset >= pd->prologue_end) {
					if (e->code_offset > pd->prologue_end) {
						tgb_u8(b, 2); // advance_pc
						tgb_uleb(b, (u64)(pd->prologue_end - cur_addr));
						cur_addr = pd->prologue_end;
						tgb_u8(b, 10); // set_prologue_end
						tgb_u8(b, 1);  // copy
					} else {
						tgb_u8(b, 10); // set_prologue_end, for e's row
					}
					prologue_pending = false;
				}
				if (e->file_id != cur_file) {
					tgb_u8(b, 4); // set_file
					tgb_uleb(b, (u64)e->file_id);
					cur_file = e->file_id;
				}
				if (e->code_offset != cur_addr) {
					tgb_u8(b, 2); // advance_pc
					tgb_uleb(b, (u64)(e->code_offset - cur_addr));
					cur_addr = e->code_offset;
				}
				if (e->line != cur_line) {
					tgb_u8(b, 3); // advance_line
					tgb_sleb(b, e->line - cur_line);
					cur_line = e->line;
				}
				tgb_u8(b, 5); // set_column
				tgb_uleb(b, (u64)tg_max(e->column, 0));
				tgb_u8(b, 1); // copy
			}
			if (prologue_pending) {
				tgb_u8(b, 2); // advance_pc
				tgb_uleb(b, (u64)(pd->prologue_end - cur_addr));
				cur_addr = pd->prologue_end;
				tgb_u8(b, 10); // set_prologue_end
				tgb_u8(b, 1);  // copy
			}
			i64 end = pd->end - pd->start;
			if (end != cur_addr) {
				tgb_u8(b, 2);
				tgb_uleb(b, (u64)(end - cur_addr));
			}
			// end sequence
			tgb_u8(b, 0);
			tgb_uleb(b, 1);
			tgb_u8(b, 1);
		}
		tgb_patch_u32(b, start, (u32)(b->count - start - 4));
	}

	// .debug_info
	{
		tgBytes *b = &d->info;
		tgDwarfTypes dt;
		memset(&dt, 0, sizeof(dt));
		dt.info = b;

		isize start = b->count;
		dt.cu_start = start;
		tgb_u32(b, 0); // unit length
		tgb_u16(b, 4); // version
		d->abbrev_offset_at = b->count;
		tgb_u32(b, 0); // abbrev offset
		tgb_u8(b, 8);  // address size

		tgb_uleb(b, cu_ranges ? tgAbbrev_CompileUnitRanges : tgAbbrev_CompileUnit);
		tgb_cstr(b, "tinygen");
		tgb_u16(b, m->language ? m->language : TDW_LANG_C99);
		tgString name = m->source_name;
		if (name.len == 0) name = m->files.count > 0 ? m->files.data[0] : tg_str_lit("tinygen");
		tgb_str(b, name);
		tgb_str(b, cwd);
		tgDwarfAddr rl;
		rl.offset = b->count;
		rl.sym = -1;
		rl.addend = 0;
		rl.section = tgSection_Text;
		tg_array_add(&d->info_addrs, rl);
		tgb_u64(b, 0); // low pc
		if (cu_ranges) {
			// .text, then each named section of code, first in .debug_ranges
			TG_ASSERT(d->ranges.count == 0);
			tg_array_add(&d->ranges_refs, b->count);
			tgb_u32(b, 0);
			i64 text_size = m->sections[tgSection_Text].count;
			if (text_size > 0) {
				tgb_u64(&d->ranges, 0);
				tgb_u64(&d->ranges, (u64)text_size);
			}
			for (isize i = 0; i < m->named_sections.count; i++) {
				tgNamedSection const *ns = &m->named_sections.data[i];
				if (!(ns->flags & TYG_SECF_EXEC) || ns->bytes.count == 0) continue;
				tg_dwarf_range_base(&d->ranges, &d->ranges_addrs, (tgSection)(tgSection_COUNT + i), 0);
				tgb_u64(&d->ranges, 0);
				tgb_u64(&d->ranges, (u64)ns->bytes.count);
			}
			tgb_u64(&d->ranges, 0);
			tgb_u64(&d->ranges, 0);
		} else {
			tgb_u64(b, (u64)m->sections[tgSection_Text].count); // high pc (length)
		}
		d->stmt_list_at = b->count;
		tgb_u32(b, 0); // stmt list

		for (isize i = 0; i < m->global_debug.count; i++) {
			tgGlobalDebug const *g = &m->global_debug.data[i];
			tgb_uleb(b, g->sym >= 0 ? tgAbbrev_GlobalVar : tgAbbrev_Constant);
			tgb_str(b, g->name);
			tg_dwarf_type_ref(&dt, g->type);
			if (g->sym >= 0) {
				tgb_u8(b, (u8)((m->symbols.data[g->sym].flags & tgSymbolFlag_Global) ? 1 : 0));
				tgb_uleb(b, (u64)tg_max(g->file_id, 1));
				tgb_uleb(b, (u64)tg_max(g->line, 0));
				tg_dwarf_symbol_location(m, b, &d->info_addrs, g->sym);
			} else {
				tgb_sleb(b, g->value);
			}
		}

		// one abstract entry per inlined procedure, by name
		tgStrMap abstract_dies;
		memset(&abstract_dies, 0, sizeof(abstract_dies));
		for (isize pi = 0; pi < m->proc_debug.count; pi++) {
			tgProcDebug const *pd = &m->proc_debug.data[pi];
			for (isize si = 0; si < pd->inline_sites.count; si++) {
				tgInlineSite const *site = &pd->inline_sites.data[si];
				if (tg_strmap_get(&abstract_dies, site->name) != NULL) continue;
				tg_strmap_set(&abstract_dies, site->name, (i64)(b->count - start));
				tgb_uleb(b, tgAbbrev_AbstractSubprogram);
				tgb_str(b, site->name);
				tgb_uleb(b, (u64)tg_max(site->decl_file, 1));
				tgb_uleb(b, (u64)tg_max(site->decl_line, 0));
				tgb_u8(b, TDW_INL_declared_inlined);
			}
		}

		// the procedures' entries, a chunk at a time, each appended as soon as it is built; the
		// chunk's arrays keep their memory from one chunk to the next
		tgDwarfProcs procs;
		memset(&procs, 0, sizeof(procs));
		procs.m = m;
		procs.abstract_dies = &abstract_dies;
		procs.frame_reg = frame_reg;
		isize const per_chunk = 64;
		tgDwarfProcChunk chunk;
		memset(&chunk, 0, sizeof(chunk));
		chunk.procs = &procs;
		for (isize first = 0; first < m->proc_debug.count; first += per_chunk) {
			tgDwarfProcChunk *c = &chunk;
			c->begin = first;
			c->end = tg_min(first + per_chunk, m->proc_debug.count);
			tg_array_clear(&c->info);
			tg_array_clear(&c->addrs);
			tg_array_clear(&c->ranges_refs);
			tg_array_clear(&c->ranges);
			tg_array_clear(&c->ranges_addrs);
			tg_array_clear(&c->types);
			tg_dwarf_proc_chunk(c);
			isize base = b->count;
			u32 ranges_base = (u32)d->ranges.count;
			tgb_bytes(b, c->info.data, c->info.count);
			for (isize i = 0; i < c->addrs.count; i++) {
				tgDwarfAddr a = c->addrs.data[i];
				a.offset += base;
				tg_array_add(&d->info_addrs, a);
			}
			for (isize i = 0; i < c->ranges_refs.count; i++) {
				isize at = c->ranges_refs.data[i];
				u32 v = 0;
				memmove(&v, b->data + base + at, 4);
				tgb_patch_u32(b, base + at, v + ranges_base);
				tg_array_add(&d->ranges_refs, base + at);
			}
			for (isize i = 0; i < c->ranges_addrs.count; i++) {
				tgDwarfAddr a = c->ranges_addrs.data[i];
				a.offset += ranges_base;
				tg_array_add(&d->ranges_addrs, a);
			}
			tgb_bytes(&d->ranges, c->ranges.data, c->ranges.count);
			for (isize i = 0; i < c->types.count; i++) {
				tgDwarfPendingType pd = c->types.data[i];
				pd.at += base;
				tg_array_add(&dt.pending, pd);
				tg_dwarf_queue(&dt, pd.type);
			}
		}
		{
			tgDwarfProcChunk *c = &chunk;
			tgDwarfScopes *sc = &c->sc;
			tg_array_free(&c->info);
			tg_array_free(&c->addrs);
			tg_array_free(&c->ranges_refs);
			tg_array_free(&c->ranges);
			tg_array_free(&c->ranges_addrs);
			tg_array_free(&c->types);
			tg_array_free(&c->expr);
			tg_array_free(&sc->eff); tg_array_free(&sc->kid); tg_array_free(&sc->sib);
			tg_array_free(&sc->head); tg_array_free(&sc->tail); tg_array_free(&sc->pool);
			tg_array_free(&sc->var_head); tg_array_free(&sc->var_tail); tg_array_free(&sc->var_next);
			tg_array_free(&sc->site);
		}
		tg_strmap_free(&abstract_dies);

		// types, written after their first use
		for (isize i = 0; i < dt.queue.count; i++) {
			tg_dwarf_write_type(&dt, dt.queue.data[i]);
		}
		for (isize i = 0; i < dt.pending.count; i++) {
			tgDwarfPendingType const *pd = &dt.pending.data[i];
			i64 *off = tg_map_get(&dt.offsets, (u64)(uintptr_t)pd->type);
			TG_ASSERT(off != NULL);
			tgb_patch_u32(b, pd->at, (u32)(*off - start));
		}
		tg_map_free(&dt.offsets);
		tg_map_free(&dt.queued);
		tg_array_free(&dt.pending);
		tg_array_free(&dt.queue);

		tgb_u8(b, 0); // end of compile unit children
		tgb_patch_u32(b, start, (u32)(b->count - start - 4));
	}
}
