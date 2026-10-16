// COFF (x86-64) relocatable object writer for Windows, with .pdata/.xdata unwind info and
// CodeView debug info (codeview.c).
//
// Ported from x64_coff.cpp, which wrote the object to a path; tinygen writes it into a
// byte buffer, which api.c writes to the file. The RAD Debugger section (.raddbg), which the original
// filled with its own type views, is dropped.

typedef u8 tgCoffSec;
enum {
	tgCoff_Text,
	tgCoff_Rdata,
	tgCoff_Data,
	tgCoff_Bss,
	tgCoff_Tls,
	tgCoff_Pdata,
	tgCoff_Xdata,
	tgCoff_Drectve,
	tgCoff_DebugS,
	tgCoff_DebugT,
	tgCoff_DebugAbbrev, // DWARF instead of CodeView, for gdb
	tgCoff_DebugInfo,
	tgCoff_DebugLine,
	tgCoff_DebugRanges,
	tgCoff_COUNT,
};

enum {
	TG_IMAGE_REL_AMD64_ADDR64   = 0x1,
	TG_IMAGE_REL_AMD64_ADDR32   = 0x2,
	TG_IMAGE_REL_AMD64_ADDR32NB = 0x3,
	TG_IMAGE_REL_AMD64_REL32    = 0x4,
	TG_IMAGE_REL_AMD64_SECTION  = 0xA,
	TG_IMAGE_REL_AMD64_SECREL   = 0xB,
};

// a relocation in one of the sections this file builds, against a section's start
typedef struct tgCoffReloc {
	u32       offset;
	i32       sym;         // module symbol, or -1 for the start of `sym_section`
	tgCoffSec sym_section;
	u16       type;
} tgCoffReloc;

TG_ARRAY(tgCoffRelocArray, tgCoffReloc);

typedef struct tgCoffWriter {
	tgModule *       m;
	tgBytes          sec[tgCoff_COUNT];
	tgCoffRelocArray relocs[tgCoff_COUNT];
	i64              bss_size;
	i64              tbss_base;   // .tbss follows .tdata in .tls$
	i64              align[tgCoff_COUNT];
} tgCoffWriter;

TG_INTERNAL tgCoffSec tg_coff_section_of(tgSection s) {
	switch (s) {
	case tgSection_Text:   return tgCoff_Text;
	case tgSection_Rodata: return tgCoff_Rdata;
	case tgSection_Data:   return tgCoff_Data;
	case tgSection_Bss:    return tgCoff_Bss;
	case tgSection_TData:  return tgCoff_Tls;
	case tgSection_TBss:   return tgCoff_Tls;
	}
	TG_PANIC("bad section");
	return tgCoff_Text;
}

// The offset of a symbol in its COFF section.
TG_INTERNAL i64 tg_coff_symbol_offset(tgCoffWriter *w, tgSymbol const *s) {
	if (s->section == tgSection_TBss) return w->tbss_base + s->offset;
	return s->offset;
}

TG_INTERNAL void tg_coff_add_reloc(tgCoffWriter *w, tgCoffSec in, u32 offset, tgCoffSec target, u16 type) {
	tgCoffReloc r;
	r.offset = offset;
	r.sym = -1;
	r.sym_section = target;
	r.type = type;
	tg_array_add(&w->relocs[in], r);
}

// Defined by codeview.c, which follows this file in the unity build.
TG_INTERNAL void tg_codeview_emit(tgCoffWriter *w);

TG_INTERNAL void tg_coff_write_addend(tgCoffWriter *w, tgCoffSec in, u32 offset, u16 type, i64 value);

// The DWARF sections, as MinGW's gcc writes them: offsets into the other debug sections are
// SECREL, addresses ADDR64, with the addend in the bytes.
TG_INTERNAL void tg_coff_dwarf(tgCoffWriter *w) {
	tgModule *m = w->m;
	tgDwarf d;
	tg_dwarf_build(m, &d);
	w->sec[tgCoff_DebugAbbrev] = d.abbrev;
	w->sec[tgCoff_DebugInfo] = d.info;
	w->sec[tgCoff_DebugLine] = d.line;
	w->sec[tgCoff_DebugRanges] = d.ranges;
	tg_coff_add_reloc(w, tgCoff_DebugInfo, (u32)d.abbrev_offset_at, tgCoff_DebugAbbrev, TG_IMAGE_REL_AMD64_SECREL);
	tg_coff_add_reloc(w, tgCoff_DebugInfo, (u32)d.stmt_list_at, tgCoff_DebugLine, TG_IMAGE_REL_AMD64_SECREL);
	for (isize i = 0; i < d.ranges_refs.count; i++) {
		tg_coff_add_reloc(w, tgCoff_DebugInfo, (u32)d.ranges_refs.data[i], tgCoff_DebugRanges, TG_IMAGE_REL_AMD64_SECREL);
	}
	// .debug_ranges needs none: its entries count from the compile unit's low_pc
	for (isize k = 0; k < 2; k++) {
		tgDwarfAddrArray const *addrs = k == 0 ? &d.info_addrs : &d.line_addrs;
		tgCoffSec in = k == 0 ? tgCoff_DebugInfo : tgCoff_DebugLine;
		for (isize i = 0; i < addrs->count; i++) {
			tgDwarfAddr const *a = &addrs->data[i];
			tgCoffReloc r;
			r.offset = (u32)a->offset;
			r.sym = -1;
			r.sym_section = tgCoff_Text;
			r.type = TG_IMAGE_REL_AMD64_ADDR64;
			i64 addend = a->addend;
			if (a->sym >= 0) {
				// a global or undefined symbol by name, a local through its section
				tgSymbol const *s = &m->symbols.data[a->sym];
				if ((s->flags & tgSymbolFlag_Global) || s->section == tgSection_Undef) {
					r.sym = a->sym;
					addend = 0;
				} else {
					r.sym_section = tg_coff_section_of(s->section);
					addend = tg_coff_symbol_offset(w, s);
				}
			}
			tg_coff_write_addend(w, in, r.offset, r.type, addend);
			tg_array_add(&w->relocs[in], r);
		}
	}
	// the section buffers moved to w->sec
	memset(&d.abbrev, 0, sizeof(d.abbrev));
	memset(&d.info, 0, sizeof(d.info));
	memset(&d.line, 0, sizeof(d.line));
	memset(&d.ranges, 0, sizeof(d.ranges));
	tg_dwarf_free(&d);
}

////////////////////////////////////////////////////////////////
// Unwind info
////////////////////////////////////////////////////////////////

enum {
	TG_UWOP_PUSH_NONVOL = 0,
	TG_UWOP_ALLOC_LARGE = 1,
	TG_UWOP_ALLOC_SMALL = 2,
	TG_UWOP_SET_FPREG   = 3,
	TG_UWOP_SAVE_XMM128 = 8,
};

typedef struct tgUnwindCodes {
	u16 codes[48];
	i32 n;
} tgUnwindCodes;

TG_INTERNAL void tg_uw_code(tgUnwindCodes *u, u8 at, u8 op, u8 info) {
	u->codes[u->n++] = (u16)(at | (op << 8) | (info << 12));
}

TG_INTERNAL void tg_uw_alloc(tgUnwindCodes *u, u8 at, u32 size) {
	if (size <= 128) {
		tg_uw_code(u, at, TG_UWOP_ALLOC_SMALL, (u8)(size/8 - 1));
	} else if (size <= 512*1024 - 8) {
		tg_uw_code(u, at, TG_UWOP_ALLOC_LARGE, 0);
		u->codes[u->n++] = (u16)(size/8);
	} else {
		tg_uw_code(u, at, TG_UWOP_ALLOC_LARGE, 1);
		u->codes[u->n++] = (u16)(size & 0xffff);
		u->codes[u->n++] = (u16)(size >> 16);
	}
}

TG_INTERNAL void tg_coff_unwind(tgCoffWriter *w) {
	tgModule *m = w->m;
	tgBytes *pdata = &w->sec[tgCoff_Pdata];
	tgBytes *xdata = &w->sec[tgCoff_Xdata];
	for (isize k = 0; k < m->proc_debug.count; k++) {
		tgProcDebug const *pd = &m->proc_debug.data[k];
		// a procedure without a frame is a leaf, which needs no unwind info
		if (pd->naked || pd->win_push_count == 0) continue;
		tgb_align(xdata, 4, 0);
		u32 info_at = (u32)xdata->count;

		// the codes undo the prologue, so they are listed last operation first
		tgUnwindCodes u;
		memset(&u, 0, sizeof(u));
		// the xmm saves are at rbp + 16*i, rbp being the frame base (frame offset 0)
		for (i32 i = pd->win_xmm_count-1; i >= 0; i--) {
			tg_uw_code(&u, pd->win_xmm_at[i], TG_UWOP_SAVE_XMM128, pd->win_xmm_reg[i]);
			u.codes[u.n++] = (u16)i;
		}
		if (pd->win_alloc_at != 0) {
			tg_uw_alloc(&u, pd->win_alloc_at, (u32)pd->win_alloc_size);
		}
		tg_uw_code(&u, pd->win_setfp_at, TG_UWOP_SET_FPREG, 0);
		if (pd->win_pad_at != 0) {
			tg_uw_alloc(&u, pd->win_pad_at, (u32)pd->win_pad_size);
		}
		for (i32 i = pd->win_push_count-1; i >= 0; i--) {
			tg_uw_code(&u, pd->win_push_at[i], TG_UWOP_PUSH_NONVOL, pd->win_push_reg[i]);
		}
		u8 prolog_size = pd->win_alloc_at != 0 ? pd->win_alloc_at : pd->win_setfp_at;
		if (pd->win_xmm_count > 0) prolog_size = pd->win_xmm_at[pd->win_xmm_count-1];

		tgb_u8(xdata, 1); // version 1, no flags
		tgb_u8(xdata, prolog_size);
		tgb_u8(xdata, (u8)u.n);
		tgb_u8(xdata, (u8)(RBP | (0 << 4))); // frame register rbp, offset 0
		for (i32 i = 0; i < u.n; i++) tgb_u16(xdata, u.codes[i]);
		if (u.n % 2) tgb_u16(xdata, 0);

		u32 at = (u32)pdata->count;
		tgb_u32(pdata, (u32)pd->start);
		tgb_u32(pdata, (u32)pd->end);
		tgb_u32(pdata, info_at);
		tg_coff_add_reloc(w, tgCoff_Pdata, at + 0, tgCoff_Text, TG_IMAGE_REL_AMD64_ADDR32NB);
		tg_coff_add_reloc(w, tgCoff_Pdata, at + 4, tgCoff_Text, TG_IMAGE_REL_AMD64_ADDR32NB);
		tg_coff_add_reloc(w, tgCoff_Pdata, at + 8, tgCoff_Xdata, TG_IMAGE_REL_AMD64_ADDR32NB);
	}
}

////////////////////////////////////////////////////////////////
// The object file
////////////////////////////////////////////////////////////////

typedef struct tgCoffSym {
	tgString name;
	u32      value;
	i16      section; // 1-based, 0 undefined
	u16      type;
	u8       storage_class;
	u8       aux_count;
	u8       aux[18];
} tgCoffSym;

TG_ARRAY(tgCoffSymArray, tgCoffSym);

typedef struct tgCoffOutReloc { u32 offset; u32 sym; u16 type; } tgCoffOutReloc;
TG_ARRAY(tgCoffOutRelocArray, tgCoffOutReloc);

TG_INTERNAL u32 tg_coff_align_flag(i64 align) {
	u32 log = 0;
	while (((i64)1 << log) < align && log < 13) log++;
	return (log + 1) << 20;
}

TG_INTERNAL bool tg_coff_needs_quotes(tgString name) {
	for (isize i = 0; i < name.len; i++) {
		u8 c = (u8)name.text[i];
		bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		             c == '_' || c == '$' || c == '@' || c == '?' || c == '.';
		if (!plain) return true;
	}
	return false;
}

TG_INTERNAL u32 tg_coff_add_sym(tgCoffSymArray *syms, u32 *sym_count, tgCoffSym const *s) {
	u32 index = *sym_count;
	tg_array_add(syms, *s);
	*sym_count += 1 + s->aux_count;
	return index;
}

// The addend of a relocation, which COFF keeps in the bytes it patches.
TG_INTERNAL void tg_coff_write_addend(tgCoffWriter *w, tgCoffSec in, u32 offset, u16 type, i64 value) {
	u8 *p = w->sec[in].data + offset;
	if (type == TG_IMAGE_REL_AMD64_ADDR64) {
		memmove(p, &value, 8);
	} else if (type == TG_IMAGE_REL_AMD64_SECTION) {
		u16 v = (u16)value;
		memmove(p, &v, 2);
	} else {
		i32 v = (i32)value;
		memmove(p, &v, 4);
	}
}

// The suffix that makes the names of the defaults of weak definitions unique, like LLVM's:
// the name of the first strong global definition of the module.
TG_INTERNAL tgString tg_coff_weak_suffix(tgModule *m) {
	for (isize i = 0; i < m->symbols.count; i++) {
		tgSymbol const *s = &m->symbols.data[i];
		if ((s->flags & tgSymbolFlag_Global) && !(s->flags & tgSymbolFlag_Weak) && s->section != tgSection_Undef) return s->name;
	}
	return m->source_name.len > 0 ? m->source_name : tg_str_lit("tinygen");
}

typedef struct tgCoffSecHdr {
	u8  name[8];
	u32 virtual_size, virtual_address, size_of_raw_data, pointer_to_raw_data;
	u32 pointer_to_relocations, pointer_to_linenumbers;
	u16 number_of_relocations, number_of_linenumbers;
	u32 characteristics;
} tgCoffSecHdr;

typedef char tg_coff_sechdr_is_40_bytes[sizeof(tgCoffSecHdr) == 40 ? 1 : -1];

TG_INTERNAL bool tg_write_coff(tgModule *m, tgBytes *out) {
	tgCoffWriter w;
	memset(&w, 0, sizeof(w));
	w.m = m;
	for (isize i = 0; i < tgCoff_COUNT; i++) w.align[i] = 1;
	{
		static tgSection const from[4] = {tgSection_Text, tgSection_Rodata, tgSection_Data, tgSection_TData};
		static tgCoffSec const to[4] = {tgCoff_Text, tgCoff_Rdata, tgCoff_Data, tgCoff_Tls};
		for (i32 i = 0; i < 4; i++) {
			tgb_bytes(&w.sec[to[i]], m->sections[from[i]].data, m->sections[from[i]].count);
			w.align[to[i]] = tg_max(w.align[to[i]], tg_max(m->section_align[from[i]], (i64)16));
		}
	}
	w.align[tgCoff_Bss] = tg_max(m->section_align[tgSection_Bss], (i64)16);
	w.bss_size = m->nobits_size[tgSection_Bss];
	{
		// .tbss has no COFF counterpart, its zeros go after .tdata
		i64 a = tg_max(m->section_align[tgSection_TBss], (i64)1);
		w.align[tgCoff_Tls] = tg_max(w.align[tgCoff_Tls], a);
		tgb_align(&w.sec[tgCoff_Tls], (isize)a, 0);
		w.tbss_base = w.sec[tgCoff_Tls].count;
		for (i64 i = 0; i < m->nobits_size[tgSection_TBss]; i++) tgb_u8(&w.sec[tgCoff_Tls], 0);
	}
	w.align[tgCoff_Pdata] = 4;
	w.align[tgCoff_Xdata] = 4;
	w.align[tgCoff_DebugS] = 4;
	w.align[tgCoff_DebugT] = 4;

	tg_coff_unwind(&w);
	if (tg_debug_codeview()) {
		tg_codeview_emit(&w);
	} else if (tg_debug_info()) {
		tg_coff_dwarf(&w);
	}

	// exports
	for (isize i = 0; i < m->symbols.count; i++) {
		tgSymbol const *s = &m->symbols.data[i];
		if ((s->flags & tgSymbolFlag_Export) == 0 || s->section == tgSection_Undef) continue;
		tgBytes *d = &w.sec[tgCoff_Drectve];
		tgb_bytes(d, " -export:", 9); // the MinGW spelling, which link and lld also take
		if (tg_coff_needs_quotes(s->name)) {
			tgb_u8(d, '"'); tgb_bytes(d, s->name.text, s->name.len); tgb_u8(d, '"');
		} else {
			tgb_bytes(d, s->name.text, s->name.len);
		}
		if ((s->flags & tgSymbolFlag_Func) == 0) tgb_bytes(d, ",data", 5);
	}

	// which sections exist, numbered from 1
	static char const *const names[tgCoff_COUNT] = {".text", ".rdata", ".data", ".bss", ".tls$", ".pdata", ".xdata", ".drectve", ".debug$S", ".debug$T",
		".debug_abbrev", ".debug_info", ".debug_line", ".debug_ranges"};
	i16 number[tgCoff_COUNT];
	i16 section_count = 0;
	memset(number, 0, sizeof(number));
	for (isize i = 0; i < tgCoff_COUNT; i++) {
		bool present = i == tgCoff_Bss ? w.bss_size > 0 : w.sec[i].count > 0;
		if (i == tgCoff_Text) present = true;
		if (present) number[i] = ++section_count;
	}

	// symbols: section symbols, then the module's
	tgCoffSymArray syms;
	memset(&syms, 0, sizeof(syms));
	tg_array_reserve(&syms, m->symbols.count + 64);
	u32 sym_count = 0; // with aux records
	u32 section_sym[tgCoff_COUNT];
	memset(section_sym, 0, sizeof(section_sym));
	for (isize i = 0; i < tgCoff_COUNT; i++) {
		if (number[i] == 0) continue;
		tgCoffSym s;
		memset(&s, 0, sizeof(s));
		s.name = tg_str(names[i]);
		s.section = number[i];
		s.storage_class = 3; // static
		s.aux_count = 1;
		u32 length = (u32)(i == tgCoff_Bss ? w.bss_size : w.sec[i].count);
		memmove(s.aux + 0, &length, 4);
		u16 num = (u16)number[i];
		memmove(s.aux + 12, &num, 2);
		section_sym[i] = tg_coff_add_sym(&syms, &sym_count, &s);
	}

	// lld-link reports every undefined symbol, so only the referenced ones go in
	bool *referenced = (bool *)calloc((usize)tg_max(m->symbols.count, (isize)1), sizeof(bool));
	i64 *sym_index = (i64 *)calloc((usize)tg_max(m->symbols.count, (isize)1), sizeof(i64));
	TG_ASSERT(referenced != NULL && sym_index != NULL);
	for (isize i = 0; i < m->relocs.count; i++) referenced[m->relocs.data[i].sym] = true;
	for (isize i = 0; i < tgCoff_COUNT; i++) {
		for (isize k = 0; k < w.relocs[i].count; k++) if (w.relocs[i].data[k].sym >= 0) referenced[w.relocs[i].data[k].sym] = true;
	}

	tgString weak_suffix = tg_coff_weak_suffix(m);
	for (isize i = 0; i < m->symbols.count; i++) {
		tgSymbol const *s = &m->symbols.data[i];
		sym_index[i] = -1;
		bool undef = s->section == tgSection_Undef;
		// a local is referenced through its section symbol; an undefined symbol is external anyway
		if ((s->flags & tgSymbolFlag_Global) == 0 && !undef) continue;
		if (undef && !referenced[i]) continue;
		tgCoffSym cs;
		memset(&cs, 0, sizeof(cs));
		cs.name = s->name;
		cs.type = (s->flags & tgSymbolFlag_Func) ? 0x20 : 0;
		cs.storage_class = 2; // external
		if (!undef) {
			cs.section = number[tg_coff_section_of(s->section)];
			cs.value = (u32)tg_coff_symbol_offset(&w, s);
		}
		if (!undef && (s->flags & tgSymbolFlag_Weak)) {
			// a weak definition: a weak external that falls back to a uniquely named default, like LLVM's
			tgCoffSym def = cs;
			isize len = 6 + s->name.len + 9 + weak_suffix.len;
			char *name = (char *)tg_arena_alloc(&m->arena, len + 1, 1);
			snprintf(name, (usize)len + 1, ".weak.%.*s.default.%.*s", (int)s->name.len, s->name.text, (int)weak_suffix.len, weak_suffix.text);
			def.name = tg_str_make(name, len);
			u32 def_index = tg_coff_add_sym(&syms, &sym_count, &def);
			tgCoffSym weak;
			memset(&weak, 0, sizeof(weak));
			weak.name = s->name;
			weak.type = cs.type;
			weak.storage_class = 105; // weak external
			weak.aux_count = 1;
			memmove(weak.aux + 0, &def_index, 4);
			u32 search_alias = 3;
			memmove(weak.aux + 4, &search_alias, 4);
			sym_index[i] = tg_coff_add_sym(&syms, &sym_count, &weak);
		} else {
			sym_index[i] = tg_coff_add_sym(&syms, &sym_count, &cs);
		}
	}

	// relocations, with the addend written into the section's bytes
	tgCoffOutRelocArray out_relocs[tgCoff_COUNT];
	memset(out_relocs, 0, sizeof(out_relocs));
	for (isize i = 0; i < m->relocs.count; i++) {
		tgReloc const *r = &m->relocs.data[i];
		tgCoffSec in = tg_coff_section_of(r->section);
		tgSymbol const *s = &m->symbols.data[r->sym];
		u32 target = 0;
		i64 base = 0;
		if (sym_index[r->sym] >= 0) {
			target = (u32)sym_index[r->sym];
		} else {
			TG_ASSERT_MSG(s->section != tgSection_Undef, "undefined local symbol");
			target = section_sym[tg_coff_section_of(s->section)];
			base = tg_coff_symbol_offset(&w, s);
		}
		u16 type = 0;
		i64 addend = r->addend + base;
		switch (r->kind) {
		case tgReloc_PC32:
		case tgReloc_PLT32:
			type = TG_IMAGE_REL_AMD64_REL32;
			addend += 4; // COFF counts from the end of the field
			break;
		case tgReloc_Abs64:    type = TG_IMAGE_REL_AMD64_ADDR64; break;
		case tgReloc_Abs32:    type = TG_IMAGE_REL_AMD64_ADDR32; break;
		case tgReloc_SecRel32: type = TG_IMAGE_REL_AMD64_SECREL; break;
		default:
			tg_set_error(m, "relocation kind %d has no COFF form", (int)r->kind);
			free(referenced);
			free(sym_index);
			for (isize k = 0; k < tgCoff_COUNT; k++) { tg_array_free(&w.sec[k]); tg_array_free(&w.relocs[k]); tg_array_free(&out_relocs[k]); }
			tg_array_free(&syms);
			return false;
		}
		u32 offset = (u32)r->offset;
		if (r->section == tgSection_TBss) offset += (u32)w.tbss_base;
		tg_coff_write_addend(&w, in, offset, type, addend);
		tgCoffOutReloc o;
		o.offset = offset;
		o.sym = target;
		o.type = type;
		tg_array_add(&out_relocs[in], o);
	}
	for (isize i = 0; i < tgCoff_COUNT; i++) {
		if (number[i] == 0) continue;
		// the section symbol's aux record carries the count
		u16 nrel = (u16)tg_min(out_relocs[i].count + w.relocs[i].count, (isize)0xffff);
		for (isize k = 0; k < syms.count; k++) {
			tgCoffSym *cs = &syms.data[k];
			if (cs->storage_class == 3 && cs->section == number[i]) memmove(cs->aux + 4, &nrel, 2);
		}
		for (isize k = 0; k < w.relocs[i].count; k++) {
			tgCoffReloc const *r = &w.relocs[i].data[k];
			u32 target = 0;
			if (r->sym >= 0) {
				TG_ASSERT(sym_index[r->sym] >= 0);
				target = (u32)sym_index[r->sym];
			} else {
				target = section_sym[r->sym_section];
			}
			tgCoffOutReloc o;
			o.offset = r->offset;
			o.sym = target;
			o.type = r->type;
			tg_array_add(&out_relocs[i], o);
		}
	}

	// layout: header, section headers, section data with relocations, symbols, strings
	tg_array_reserve(out, 1024 + w.sec[tgCoff_Text].count*2);
	isize header_size = 20 + 40*section_count;
	tg_array_resize(out, header_size);

	isize sh_at = 20;
	u32 const CNT_CODE = 0x20, CNT_INIT = 0x40, CNT_UNINIT = 0x80, LNK_INFO = 0x200, LNK_REMOVE = 0x800;
	u32 const NRELOC_OVFL = 0x01000000, DISCARDABLE = 0x02000000, MEM_EXEC = 0x20000000, MEM_READ = 0x40000000, MEM_WRITE = 0x80000000;
	tgCoffSecHdr headers[tgCoff_COUNT];
	memset(headers, 0, sizeof(headers));
	// the string table, of the section and symbol names longer than 8 bytes
	tgBytes strtab;
	memset(&strtab, 0, sizeof(strtab));
	tgb_u32(&strtab, 0);
	for (isize i = 0; i < tgCoff_COUNT; i++) {
		if (number[i] == 0) continue;
		tgCoffSecHdr *h = &headers[i];
		tgString n = tg_str(names[i]);
		if (n.len <= 8) {
			memmove(h->name, n.text, (usize)n.len);
		} else {
			// "/" and the offset in decimal, like .debug_info in MinGW's objects
			char buf[9];
			snprintf(buf, sizeof(buf), "/%u", (unsigned)strtab.count);
			memmove(h->name, buf, strlen(buf));
			tgb_bytes(&strtab, n.text, n.len);
			tgb_u8(&strtab, 0);
		}
		u32 flags = 0;
		switch (i) {
		case tgCoff_Text:    flags = CNT_CODE | MEM_EXEC | MEM_READ; break;
		case tgCoff_Rdata:   flags = CNT_INIT | MEM_READ; break;
		case tgCoff_Data:    flags = CNT_INIT | MEM_READ | MEM_WRITE; break;
		case tgCoff_Bss:     flags = CNT_UNINIT | MEM_READ | MEM_WRITE; break;
		case tgCoff_Tls:     flags = CNT_INIT | MEM_READ | MEM_WRITE; break;
		case tgCoff_Pdata:   flags = CNT_INIT | MEM_READ; break;
		case tgCoff_Xdata:   flags = CNT_INIT | MEM_READ; break;
		case tgCoff_Drectve: flags = LNK_INFO | LNK_REMOVE; break;
		case tgCoff_DebugS:  flags = CNT_INIT | MEM_READ | DISCARDABLE; break;
		case tgCoff_DebugT:  flags = CNT_INIT | MEM_READ | DISCARDABLE; break;
		default:             flags = CNT_INIT | MEM_READ | DISCARDABLE; break; // DWARF
		}
		flags |= tg_coff_align_flag(w.align[i]);
		if (i == tgCoff_Bss) {
			h->size_of_raw_data = (u32)w.bss_size;
		} else {
			tgb_align(out, 16, 0);
			h->pointer_to_raw_data = w.sec[i].count > 0 ? (u32)out->count : 0;
			h->size_of_raw_data = (u32)w.sec[i].count;
			tgb_bytes(out, w.sec[i].data, w.sec[i].count);
		}
		isize nrel = out_relocs[i].count;
		if (nrel > 0) {
			tgb_align(out, 2, 0);
			h->pointer_to_relocations = (u32)out->count;
			if (nrel >= 0xffff) {
				// the real count goes in the first entry
				flags |= NRELOC_OVFL;
				h->number_of_relocations = 0xffff;
				tgb_u32(out, (u32)(nrel + 1));
				tgb_u32(out, 0);
				tgb_u16(out, 0);
			} else {
				h->number_of_relocations = (u16)nrel;
			}
			for (isize k = 0; k < nrel; k++) {
				tgCoffOutReloc const *r = &out_relocs[i].data[k];
				tgb_u32(out, r->offset);
				tgb_u32(out, r->sym);
				tgb_u16(out, r->type);
			}
		}
		h->characteristics = flags;
	}
	for (isize i = 0; i < tgCoff_COUNT; i++) {
		if (number[i] == 0) continue;
		memmove(out->data + sh_at + 40*(number[i]-1), &headers[i], 40);
	}

	// symbols, then the string table
	tgb_align(out, 4, 0);
	u32 symtab_at = (u32)out->count;
	for (isize k = 0; k < syms.count; k++) {
		tgCoffSym const *s = &syms.data[k];
		u8 rec[18];
		memset(rec, 0, sizeof(rec));
		if (s->name.len <= 8) {
			memmove(rec, s->name.text, (usize)s->name.len);
		} else {
			u32 off = (u32)strtab.count;
			tgb_bytes(&strtab, s->name.text, s->name.len);
			tgb_u8(&strtab, 0);
			memmove(rec + 4, &off, 4);
		}
		memmove(rec + 8, &s->value, 4);
		memmove(rec + 12, &s->section, 2);
		memmove(rec + 14, &s->type, 2);
		rec[16] = s->storage_class;
		rec[17] = s->aux_count;
		tgb_bytes(out, rec, 18);
		if (s->aux_count) tgb_bytes(out, s->aux, 18);
	}
	u32 strtab_size = (u32)strtab.count;
	memmove(strtab.data, &strtab_size, 4);
	tgb_bytes(out, strtab.data, strtab.count);

	u16 machine = 0x8664;
	u16 nsec = (u16)section_count;
	u32 zero = 0;
	memmove(out->data + 0, &machine, 2);
	memmove(out->data + 2, &nsec, 2);
	memmove(out->data + 4, &zero, 4); // timestamp
	memmove(out->data + 8, &symtab_at, 4);
	memmove(out->data + 12, &sym_count, 4);

	tg_array_free(&strtab);
	tg_array_free(&syms);
	free(referenced);
	free(sym_index);
	for (isize i = 0; i < tgCoff_COUNT; i++) {
		tg_array_free(&w.sec[i]);
		tg_array_free(&w.relocs[i]);
		tg_array_free(&out_relocs[i]);
	}
	return true;
}
