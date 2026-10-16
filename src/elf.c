// ELF64 relocatable object writer for x86-64 and arm64, with .eh_frame. The DWARF sections
// come from dwarf.c.
//
// Ported from x64_elf.cpp, which wrote the object to a path; tinygen writes it into a
// byte buffer, which api.c writes to the file. The debug sections and their relocations are
// only in the object with debug info.

enum {
	TG_R_X86_64_64            = 1,
	TG_R_X86_64_PC32          = 2,
	TG_R_X86_64_PLT32         = 4,
	TG_R_X86_64_GOTPCREL      = 9,
	TG_R_X86_64_32            = 10,
	TG_R_X86_64_DTPOFF64      = 17,
	TG_R_X86_64_TLSGD         = 19,
	TG_R_X86_64_GOTTPOFF      = 22,
	TG_R_X86_64_TPOFF32       = 23,
	TG_R_X86_64_GOTPCRELX     = 41,
	TG_R_X86_64_REX_GOTPCRELX = 42,

	TG_R_AARCH64_ABS64                       = 257,
	TG_R_AARCH64_ABS32                       = 258,
	TG_R_AARCH64_PREL32                      = 261,
	TG_R_AARCH64_ADR_PREL_PG_HI21            = 275,
	TG_R_AARCH64_ADD_ABS_LO12_NC             = 277,
	TG_R_AARCH64_CALL26                      = 283,
	TG_R_AARCH64_ADR_GOT_PAGE                = 311,
	TG_R_AARCH64_LD64_GOT_LO12_NC            = 312,
	TG_R_AARCH64_TLSIE_ADR_GOTTPREL_PAGE21   = 541,
	TG_R_AARCH64_TLSIE_LD64_GOTTPREL_LO12_NC = 542,
	TG_R_AARCH64_TLSDESC_ADR_PAGE21          = 562,
	TG_R_AARCH64_TLSDESC_LD64_LO12           = 563,
	TG_R_AARCH64_TLSDESC_ADD_LO12            = 564,
	TG_R_AARCH64_TLSDESC_CALL                = 569,
};

// DW_CFA_offset of a register saved at cfa-8*n, for registers past the 6 bit short form too
TG_INTERNAL void tg_cfa_offset(tgBytes *b, u32 reg, u64 n) {
	if (reg < 64) {
		tgb_u8(b, (u8)(0x80 | reg));
	} else {
		tgb_u8(b, 0x05); // offset_extended
		tgb_uleb(b, reg);
	}
	tgb_uleb(b, n);
}

// DW_CFA_advance_loc by `delta` code alignment units
TG_INTERNAL void tg_cfa_advance(tgBytes *b, u32 delta) {
	if (delta < 64) {
		tgb_u8(b, (u8)(0x40 | delta));
	} else {
		tgb_u8(b, 0x03); tgb_u8(b, (u8)delta); tgb_u8(b, (u8)(delta >> 8)); // advance_loc2
	}
}

// relocations for the sections this file builds (debug info, eh_frame)
typedef struct tgExtraReloc {
	i64 offset;
	i32 sym_section; // a section symbol, by output section, or tgOut_COUNT + a named section's index
	u32 type;
	i64 addend;
} tgExtraReloc;

// a relocation in .debug_info against one of the module's symbols
typedef struct tgSymReloc {
	i64 offset;
	i32 sym;
	u32 type;
} tgSymReloc;

TG_ARRAY(tgExtraRelocArray, tgExtraReloc);
TG_ARRAY(tgSymRelocArray, tgSymReloc);

typedef i32 tgOutSection;
enum {
	tgOut_Null,
	tgOut_Text,
	tgOut_Rodata,
	tgOut_Data,
	tgOut_Bss,
	tgOut_TData,
	tgOut_TBss,
	tgOut_EhFrame,
	tgOut_DebugAbbrev,
	tgOut_DebugInfo,
	tgOut_DebugLine,
	tgOut_DebugRanges,
	tgOut_NoteStack,
	tgOut_RelaText,
	tgOut_RelaData,
	tgOut_RelaRodata,
	tgOut_RelaTData,
	tgOut_RelaEhFrame,
	tgOut_RelaDebugInfo,
	tgOut_RelaDebugLine,
	tgOut_RelaDebugRanges, // only with code in named sections, see tyg_proc_section
	tgOut_Symtab,
	tgOut_Strtab,
	tgOut_Shstrtab,
	tgOut_COUNT,
};

TG_INTERNAL bool tg_elf_is_debug_section(tgOutSection s) {
	return (s >= tgOut_DebugAbbrev && s <= tgOut_DebugRanges) || (s >= tgOut_RelaDebugInfo && s <= tgOut_RelaDebugRanges);
}

////////////////////////////////////////////////////////////////
// Object file
////////////////////////////////////////////////////////////////

typedef struct tgElfShdr {
	u32 sh_name;
	u32 sh_type;
	u64 sh_flags;
	u64 sh_addr;
	u64 sh_offset;
	u64 sh_size;
	u32 sh_link;
	u32 sh_info;
	u64 sh_addralign;
	u64 sh_entsize;
} tgElfShdr;

typedef struct tgElfSym {
	u32 st_name;
	u8  st_info;
	u8  st_other;
	u16 st_shndx;
	u64 st_value;
	u64 st_size;
} tgElfSym;

typedef struct tgElfRela {
	u64 r_offset;
	u64 r_info;
	i64 r_addend;
} tgElfRela;

TG_ARRAY(tgElfSymArray, tgElfSym);

TG_INTERNAL tgOutSection tg_elf_out_section_of(tgSection s) {
	switch (s) {
	case tgSection_Text:   return tgOut_Text;
	case tgSection_Rodata: return tgOut_Rodata;
	case tgSection_Data:   return tgOut_Data;
	case tgSection_Bss:    return tgOut_Bss;
	case tgSection_TData:  return tgOut_TData;
	case tgSection_TBss:   return tgOut_TBss;
	}
	return tgOut_Null;
}

TG_INTERNAL tgBytes *tg_elf_rela_for(tgBytes *sec, tgOutSection target) {
	switch (target) {
	case tgOut_Text:      return &sec[tgOut_RelaText];
	case tgOut_Data:      return &sec[tgOut_RelaData];
	case tgOut_Rodata:    return &sec[tgOut_RelaRodata];
	case tgOut_TData:     return &sec[tgOut_RelaTData];
	case tgOut_EhFrame:   return &sec[tgOut_RelaEhFrame];
	case tgOut_DebugInfo: return &sec[tgOut_RelaDebugInfo];
	case tgOut_DebugLine: return &sec[tgOut_RelaDebugLine];
	case tgOut_DebugRanges: return &sec[tgOut_RelaDebugRanges];
	}
	TG_PANIC("bad rela");
	return NULL;
}

// The sym_section of a tgExtraReloc against code in `s`, .text or a named section.
TG_INTERNAL i32 tg_elf_code_sym_section(tgSection s) {
	if (s >= tgSection_COUNT) return tgOut_COUNT + (i32)(s - tgSection_COUNT);
	TG_ASSERT(s == tgSection_Text);
	return tgOut_Text;
}

TG_INTERNAL void tg_elf_set(tgElfShdr *sh, u32 const *name_off, tgOutSection s, u32 type, u64 flags, u64 align, u64 entsize, u32 link, u32 info) {
	sh[s].sh_name = name_off[s];
	sh[s].sh_type = type;
	sh[s].sh_flags = flags;
	sh[s].sh_addralign = align;
	sh[s].sh_entsize = entsize;
	sh[s].sh_link = link;
	sh[s].sh_info = info;
}

TG_INTERNAL u64 tg_elf_align_of(tgModule *m, tgSection s) {
	return (u64)tg_max(m->section_align[s], (i64)16);
}

// A named section in the object: its section header index, its section symbol and its
// relocations.
typedef struct tgElfNamed {
	u32     shndx;
	u32     rela_shndx;
	i32     section_sym;
	bool    has_relocs;
	tgBytes rela;
} tgElfNamed;

TG_INTERNAL void tg_elf_add_extra(tgBytes *sec, i32 const *section_sym, tgElfNamed const *named, tgOutSection target, tgExtraRelocArray const *rs) {
	for (isize i = 0; i < rs->count; i++) {
		tgExtraReloc const *r = &rs->data[i];
		i32 sym = r->sym_section >= tgOut_COUNT ? named[r->sym_section - tgOut_COUNT].section_sym : section_sym[r->sym_section];
		tgElfRela er;
		memset(&er, 0, sizeof(er));
		er.r_offset = (u64)r->offset;
		er.r_info = ((u64)sym << 32) | r->type;
		er.r_addend = r->addend;
		tgb_bytes(tg_elf_rela_for(sec, target), &er, (isize)sizeof(er));
	}
}

TG_INTERNAL u32 tg_elf_shndx_of(u32 const *shndx, tgElfNamed const *named, tgSection s) {
	if (s >= tgSection_COUNT) return named[s - tgSection_COUNT].shndx;
	return shndx[tg_elf_out_section_of(s)];
}

TG_INTERNAL i32 tg_elf_section_sym_of(i32 const *section_sym, tgElfNamed const *named, tgSection s) {
	if (s >= tgSection_COUNT) return named[s - tgSection_COUNT].section_sym;
	return section_sym[tg_elf_out_section_of(s)];
}

// The ELF section type of a named section: the arrays of constructors and destructors have
// their own types, also with a ".N" priority suffix.
TG_INTERNAL u32 tg_elf_named_type(tgNamedSection const *ns) {
	char const *const arrays[3] = {".init_array", ".fini_array", ".preinit_array"};
	u32 const types[3] = {14, 15, 16}; // SHT_INIT_ARRAY, SHT_FINI_ARRAY, SHT_PREINIT_ARRAY
	if (ns->flags & TYG_SECF_ZERO) return 8; // SHT_NOBITS
	for (i32 i = 0; i < 3; i++) {
		isize n = (isize)strlen(arrays[i]);
		if (ns->name.len >= n && memcmp(ns->name.text, arrays[i], (usize)n) == 0 &&
		    (ns->name.len == n || ns->name.text[n] == '.')) {
			return types[i];
		}
	}
	if (ns->name.len >= 5 && memcmp(ns->name.text, ".note", 5) == 0) return 7; // SHT_NOTE
	return 1; // SHT_PROGBITS
}

// Writes the module as an ELF relocatable object into `out`.
TG_INTERNAL bool tg_write_elf(tgModule *m, tgBytes *out) {
	tgBytes sec[tgOut_COUNT];
	memset(sec, 0, sizeof(sec));
	tgExtraRelocArray extra_relocs;    // .eh_frame
	tgExtraRelocArray info_relocs;     // .debug_info
	tgExtraRelocArray line_relocs;     // .debug_line
	tgExtraRelocArray ranges_relocs;   // .debug_ranges
	tgSymRelocArray   info_sym_relocs; // .debug_info
	memset(&extra_relocs, 0, sizeof(extra_relocs));
	memset(&info_relocs, 0, sizeof(info_relocs));
	memset(&line_relocs, 0, sizeof(line_relocs));
	memset(&ranges_relocs, 0, sizeof(ranges_relocs));
	memset(&info_sym_relocs, 0, sizeof(info_sym_relocs));

	tgb_bytes(&sec[tgOut_Text], m->sections[tgSection_Text].data, m->sections[tgSection_Text].count);
	tgb_bytes(&sec[tgOut_Rodata], m->sections[tgSection_Rodata].data, m->sections[tgSection_Rodata].count);
	tgb_bytes(&sec[tgOut_Data], m->sections[tgSection_Data].data, m->sections[tgSection_Data].count);
	tgb_bytes(&sec[tgOut_TData], m->sections[tgSection_TData].data, m->sections[tgSection_TData].count);

	bool arm64 = tg_is_arm64();
	u32 const r_abs64 = arm64 ? TG_R_AARCH64_ABS64  : TG_R_X86_64_64;
	u32 const r_abs32 = arm64 ? TG_R_AARCH64_ABS32  : TG_R_X86_64_32;
	u32 const r_pc32  = arm64 ? TG_R_AARCH64_PREL32 : TG_R_X86_64_PC32;

	// .eh_frame
	{
		tgBytes *b = &sec[tgOut_EhFrame];
		isize cie_start = b->count;
		tgb_u32(b, 0); // length
		tgb_u32(b, 0); // CIE id
		tgb_u8(b, 1);  // version
		tgb_cstr(b, "zR");
		tgb_uleb(b, arm64 ? 4 : 1);   // code alignment
		tgb_sleb(b, -8);              // data alignment
		tgb_uleb(b, arm64 ? 30 : 16); // return address register
		tgb_uleb(b, 1);  // augmentation data length
		tgb_u8(b, 0x1b); // FDE pointers: pcrel sdata4
		if (arm64) {
			tgb_u8(b, 0x0c); tgb_uleb(b, 31); tgb_uleb(b, 0); // def_cfa sp+0, the return address stays in x30
		} else {
			tgb_u8(b, 0x0c); tgb_uleb(b, 7); tgb_uleb(b, 8); // def_cfa rsp+8
			tg_cfa_offset(b, 16, 1);                         // rip at cfa-8
		}
		tgb_align(b, 8, 0);
		tgb_patch_u32(b, cie_start, (u32)(b->count - cie_start - 4));

		for (isize pi = 0; pi < m->proc_debug.count; pi++) {
			tgProcDebug const *pd = &m->proc_debug.data[pi];
			isize fde_start = b->count;
			tgb_u32(b, 0);
			tgb_u32(b, (u32)(b->count - cie_start)); // CIE pointer
			tgExtraReloc r;
			r.offset = b->count;
			r.sym_section = tg_elf_code_sym_section(pd->section);
			r.type = r_pc32;
			r.addend = pd->start;
			tg_array_add(&extra_relocs, r);
			tgb_u32(b, 0); // pc begin
			tgb_u32(b, (u32)(pd->end - pd->start));
			tgb_uleb(b, 0); // augmentation data length
			if (pd->naked) {
				// no frame, the cie's rsp+8 holds throughout
				tgb_align(b, 8, 0);
				tgb_patch_u32(b, fde_start, (u32)(b->count - fde_start - 4));
				continue;
			}
			if (arm64) {
				tg_cfa_advance(b, 1);                   // stp x29, x30, [sp, #-16]!
				tgb_u8(b, 0x0e); tgb_uleb(b, 16);       // def_cfa_offset 16
				tg_cfa_offset(b, 29, 2);                // x29 at cfa-16
				tg_cfa_offset(b, 30, 1);                // x30 at cfa-8
				tg_cfa_advance(b, 1);                   // mov x29, sp
				tgb_u8(b, 0x0c); tgb_uleb(b, 29); tgb_uleb(b, 16); // def_cfa x29+16
			} else {
				tg_cfa_advance(b, 1);                   // push rbp
				tgb_u8(b, 0x0e); tgb_uleb(b, 16);       // def_cfa_offset 16
				tg_cfa_offset(b, 6, 2);                 // rbp at cfa-16
				tg_cfa_advance(b, 3);                   // mov rbp, rsp
				tgb_u8(b, 0x0d); tgb_uleb(b, 6);        // def_cfa_register rbp
			}
			if (pd->saved_regs.count > 0) {
				// after the saves, counted from the end of the frame setup
				tg_cfa_advance(b, arm64 ? (u32)(pd->saved_at - 8) / 4 : (u32)(pd->saved_at - 4));
				for (isize si = 0; si < pd->saved_regs.count; si++) {
					tgSavedReg const *s = &pd->saved_regs.data[si];
					// saved at fp+off, the cfa is fp+16
					tg_cfa_offset(b, (u32)s->dwarf_reg, (u64)((16 - s->frame_offset) / 8));
				}
			}
			tgb_align(b, 8, 0);
			tgb_patch_u32(b, fde_start, (u32)(b->count - fde_start - 4));
		}
	}

	bool debug = tg_debug_info();
	if (debug) {
		tgDwarf d;
		tg_dwarf_build(m, &d);
		sec[tgOut_DebugAbbrev] = d.abbrev;
		sec[tgOut_DebugInfo] = d.info;
		sec[tgOut_DebugLine] = d.line;
		sec[tgOut_DebugRanges] = d.ranges;
		tgExtraReloc ra = {0, tgOut_DebugAbbrev, 0, 0};
		tgExtraReloc rs = {0, tgOut_DebugLine, 0, 0};
		ra.offset = d.abbrev_offset_at; ra.type = r_abs32;
		rs.offset = d.stmt_list_at;     rs.type = r_abs32;
		tg_array_add(&info_relocs, ra);
		tg_array_add(&info_relocs, rs);
		for (isize i = 0; i < d.ranges_refs.count; i++) {
			isize at = d.ranges_refs.data[i];
			u32 off = 0;
			memmove(&off, d.info.data + at, 4);
			tgExtraReloc r;
			r.offset = at;
			r.sym_section = tgOut_DebugRanges;
			r.type = r_abs32;
			r.addend = off;
			tg_array_add(&info_relocs, r);
		}
		for (isize i = 0; i < d.info_addrs.count; i++) {
			tgDwarfAddr const *a = &d.info_addrs.data[i];
			if (a->sym < 0) {
				tgExtraReloc r;
				r.offset = a->offset;
				r.sym_section = tg_elf_code_sym_section(a->section);
				r.type = r_abs64;
				r.addend = a->addend;
				tg_array_add(&info_relocs, r);
			} else {
				bool tls = (m->symbols.data[a->sym].flags & tgSymbolFlag_TLS) != 0;
				TG_ASSERT(!tls || !arm64);
				tgSymReloc r;
				r.offset = a->offset;
				r.sym = a->sym;
				r.type = tls ? TG_R_X86_64_DTPOFF64 : r_abs64;
				tg_array_add(&info_sym_relocs, r);
			}
		}
		for (isize i = 0; i < d.line_addrs.count; i++) {
			tgDwarfAddr const *a = &d.line_addrs.data[i];
			tgExtraReloc r;
			r.offset = a->offset;
			r.sym_section = tg_elf_code_sym_section(a->section);
			r.type = r_abs64;
			r.addend = a->addend;
			tg_array_add(&line_relocs, r);
		}
		for (isize i = 0; i < d.ranges_addrs.count; i++) {
			tgDwarfAddr const *a = &d.ranges_addrs.data[i];
			tgExtraReloc r;
			r.offset = a->offset;
			r.sym_section = tg_elf_code_sym_section(a->section);
			r.type = r_abs64;
			r.addend = a->addend;
			tg_array_add(&ranges_relocs, r);
		}
		// the section buffers moved to sec[]
		memset(&d.abbrev, 0, sizeof(d.abbrev));
		memset(&d.info, 0, sizeof(d.info));
		memset(&d.line, 0, sizeof(d.line));
		memset(&d.ranges, 0, sizeof(d.ranges));
		tg_dwarf_free(&d);
	}

	// the sections in the object, and their ELF indices: without debug info, no debug sections
	bool present[tgOut_COUNT];
	u32 shndx[tgOut_COUNT];
	u32 shnum = 0;
	for (tgOutSection i = 0; i < tgOut_COUNT; i++) {
		present[i] = debug || !tg_elf_is_debug_section(i);
		if (i == tgOut_RelaDebugRanges) present[i] = ranges_relocs.count > 0;
		shndx[i] = present[i] ? shnum++ : 0;
	}
	// the named sections come after the fixed ones, each with its relocations when it has any
	isize named_count = m->named_sections.count;
	tgElfNamed *named = (tgElfNamed *)calloc((usize)tg_max(named_count, (isize)1), sizeof(tgElfNamed));
	TG_ASSERT(named != NULL);
	for (isize i = 0; i < m->relocs.count; i++) {
		tgSection rs = m->relocs.data[i].section;
		if (rs >= tgSection_COUNT) named[rs - tgSection_COUNT].has_relocs = true;
	}
	for (isize i = 0; i < named_count; i++) {
		named[i].shndx = shnum++;
		if (named[i].has_relocs) named[i].rela_shndx = shnum++;
	}

	// symbols: null, section symbols, then globals
	tgBytes strtab;
	memset(&strtab, 0, sizeof(strtab));
	tgb_u8(&strtab, 0);
	tgElfSymArray syms;
	memset(&syms, 0, sizeof(syms));
	tgElfSym null_sym;
	memset(&null_sym, 0, sizeof(null_sym));
	tg_array_add(&syms, null_sym);

	i32 section_sym[tgOut_COUNT];
	memset(section_sym, 0, sizeof(section_sym));
	tgOutSection const sym_sections[] = {tgOut_Text, tgOut_Rodata, tgOut_Data, tgOut_Bss, tgOut_TData, tgOut_TBss, tgOut_EhFrame, tgOut_DebugAbbrev, tgOut_DebugInfo, tgOut_DebugLine, tgOut_DebugRanges};
	for (isize i = 0; i < tg_count_of(sym_sections); i++) {
		tgOutSection s = sym_sections[i];
		if (!present[s]) continue;
		tgElfSym es;
		memset(&es, 0, sizeof(es));
		es.st_info = (0 << 4) | 3; // local, section
		es.st_shndx = (u16)shndx[s];
		section_sym[s] = (i32)syms.count;
		tg_array_add(&syms, es);
	}
	for (isize i = 0; i < named_count; i++) {
		tgElfSym es;
		memset(&es, 0, sizeof(es));
		es.st_info = (0 << 4) | 3; // local, section
		es.st_shndx = (u16)named[i].shndx;
		named[i].section_sym = (i32)syms.count;
		tg_array_add(&syms, es);
	}
	i32 *sym_index = (i32 *)calloc((usize)tg_max(m->symbols.count, (isize)1), sizeof(i32));
	TG_ASSERT(sym_index != NULL);

	// named local symbols, so profilers and debuggers see static functions and data, and
	// TLS relocations have a TLS symbol: linkers reject them against a section symbol.
	// A ".L" name is assembler-local and is referenced through its section symbol, unless
	// it is a thread local.
	for (isize i = 0; i < m->symbols.count; i++) {
		tgSymbol const *s = &m->symbols.data[i];
		sym_index[i] = -1;
		if ((s->flags & tgSymbolFlag_Global) != 0 || s->section == tgSection_Undef) continue;
		if (tg_str_has_prefix(s->name, tg_str_lit(".L")) && !(s->flags & tgSymbolFlag_TLS)) continue;
		tgElfSym es;
		memset(&es, 0, sizeof(es));
		es.st_name = (u32)strtab.count;
		tgb_str(&strtab, s->name);
		u8 type = (s->flags & tgSymbolFlag_Func) ? 2 : (s->flags & tgSymbolFlag_TLS) ? 6 : 1;
		es.st_info = (u8)((0 << 4) | type);
		es.st_shndx = (u16)tg_elf_shndx_of(shndx, named, s->section);
		es.st_value = (u64)s->offset;
		es.st_size = (u64)s->size;
		sym_index[i] = (i32)syms.count;
		tg_array_add(&syms, es);
	}
	u32 first_global = (u32)syms.count;

	for (isize i = 0; i < m->symbols.count; i++) {
		tgSymbol const *s = &m->symbols.data[i];
		bool is_local = (s->flags & tgSymbolFlag_Global) == 0;
		if (is_local) {
			continue; // a named local above, or referenced through its section symbol
		}
		tgElfSym es;
		memset(&es, 0, sizeof(es));
		es.st_name = (u32)strtab.count;
		tgb_str(&strtab, s->name);
		u8 bind = (s->flags & tgSymbolFlag_Weak) ? 2 : 1;
		u8 type = (s->flags & tgSymbolFlag_TLS) ? 6 : 0;
		if (s->section != tgSection_Undef) {
			type = (s->flags & tgSymbolFlag_Func) ? 2 : (s->flags & tgSymbolFlag_TLS) ? 6 : 1;
			es.st_shndx = (u16)tg_elf_shndx_of(shndx, named, s->section);
			es.st_value = (u64)s->offset;
			es.st_size = (u64)s->size;
		} else if (s->common_align != 0) {
			type = 1; // object
			es.st_shndx = 0xfff2; // SHN_COMMON
			es.st_value = (u64)s->common_align;
			es.st_size = (u64)s->size;
		}
		if (s->flags & tgSymbolFlag_Hidden) {
			es.st_other = 2; // STV_HIDDEN, also on a reference: the definition must be in the image
		} else if (s->flags & tgSymbolFlag_Protected) {
			es.st_other = 3; // STV_PROTECTED, also on a reference, like the assemblers' .protected
		}
		es.st_info = (u8)((bind << 4) | type);
		sym_index[i] = (i32)syms.count;
		tg_array_add(&syms, es);
	}

	// relocations
	for (isize i = 0; i < m->relocs.count; i++) {
		tgReloc const *r = &m->relocs.data[i];
		tgSymbol const *s = &m->symbols.data[r->sym];
		u32 elf_sym = 0;
		i64 addend = r->addend;
		if (sym_index[r->sym] >= 0) {
			elf_sym = (u32)sym_index[r->sym];
		} else {
			if (s->section == tgSection_Undef) {
				// a local symbol has no ELF symbol of its own, so it must be defined here
				tg_set_error(m, "the local symbol %.*s is used but not defined", (int)s->name.len, s->name.text);
				for (tgOutSection k = 0; k < tgOut_COUNT; k++) tg_array_free(&sec[k]);
				tg_array_free(&extra_relocs);
				tg_array_free(&info_relocs);
				tg_array_free(&line_relocs);
	tg_array_free(&ranges_relocs);
				tg_array_free(&info_sym_relocs);
				tg_array_free(&strtab);
				tg_array_free(&syms);
				free(sym_index);
				for (isize k = 0; k < named_count; k++) tg_array_free(&named[k].rela);
				free(named);
				return false;
			}
			elf_sym = (u32)tg_elf_section_sym_of(section_sym, named, s->section);
			addend += s->offset;
		}
		u32 type = 0;
		switch (r->kind) {
		case tgReloc_PC32:          type = TG_R_X86_64_PC32; break;
		case tgReloc_PLT32:         type = TG_R_X86_64_PLT32; break;
		case tgReloc_GOTPCRELX:     type = TG_R_X86_64_GOTPCRELX; break;
		case tgReloc_REX_GOTPCRELX: type = TG_R_X86_64_REX_GOTPCRELX; break;
		case tgReloc_Abs64:         type = TG_R_X86_64_64; break;
		case tgReloc_Abs32:         type = TG_R_X86_64_32; break;
		case tgReloc_TPOFF32:       type = TG_R_X86_64_TPOFF32; break;
		case tgReloc_GOTTPOFF:      type = TG_R_X86_64_GOTTPOFF; break;
		case tgReloc_TLSGD:         type = TG_R_X86_64_TLSGD; break;
		case tgReloc_A64_Branch26:      type = TG_R_AARCH64_CALL26; break;
		case tgReloc_A64_Page21:        type = TG_R_AARCH64_ADR_PREL_PG_HI21; break;
		case tgReloc_A64_PageOff12:     type = TG_R_AARCH64_ADD_ABS_LO12_NC; break;
		case tgReloc_A64_GotPage21:     type = TG_R_AARCH64_ADR_GOT_PAGE; break;
		case tgReloc_A64_GotPageOff12:  type = TG_R_AARCH64_LD64_GOT_LO12_NC; break;
		case tgReloc_A64_TlsIePage21:   type = TG_R_AARCH64_TLSIE_ADR_GOTTPREL_PAGE21; break;
		case tgReloc_A64_TlsIeLo12:     type = TG_R_AARCH64_TLSIE_LD64_GOTTPREL_LO12_NC; break;
		case tgReloc_A64_TlsDescPage21: type = TG_R_AARCH64_TLSDESC_ADR_PAGE21; break;
		case tgReloc_A64_TlsDescLd:     type = TG_R_AARCH64_TLSDESC_LD64_LO12; break;
		case tgReloc_A64_TlsDescAdd:    type = TG_R_AARCH64_TLSDESC_ADD_LO12; break;
		case tgReloc_A64_TlsDescCall:   type = TG_R_AARCH64_TLSDESC_CALL; break;
		}
		if (arm64 && r->kind == tgReloc_Abs64) type = TG_R_AARCH64_ABS64;
		if (arm64 && r->kind == tgReloc_Abs32) type = TG_R_AARCH64_ABS32;
		TG_ASSERT_MSG(type != 0, "relocation kind has no ELF form");
		tgElfRela er;
		memset(&er, 0, sizeof(er));
		er.r_offset = (u64)r->offset;
		er.r_info = ((u64)elf_sym << 32) | type;
		er.r_addend = addend;
		if (r->section >= tgSection_COUNT) {
			tgb_bytes(&named[r->section - tgSection_COUNT].rela, &er, (isize)sizeof(er));
		} else {
			tgb_bytes(tg_elf_rela_for(sec, tg_elf_out_section_of(r->section)), &er, (isize)sizeof(er));
		}
	}
	tg_elf_add_extra(sec, section_sym, named, tgOut_EhFrame, &extra_relocs);
	tg_elf_add_extra(sec, section_sym, named, tgOut_DebugInfo, &info_relocs);
	tg_elf_add_extra(sec, section_sym, named, tgOut_DebugLine, &line_relocs);
	tg_elf_add_extra(sec, section_sym, named, tgOut_DebugRanges, &ranges_relocs);
	for (isize i = 0; i < info_sym_relocs.count; i++) {
		tgSymReloc const *r = &info_sym_relocs.data[i];
		tgSymbol const *s = &m->symbols.data[r->sym];
		tgElfRela er;
		memset(&er, 0, sizeof(er));
		er.r_offset = (u64)r->offset;
		if (sym_index[r->sym] >= 0) {
			er.r_info = ((u64)sym_index[r->sym] << 32) | r->type;
		} else {
			er.r_info = ((u64)tg_elf_section_sym_of(section_sym, named, s->section) << 32) | r->type;
			er.r_addend = s->offset;
		}
		tgb_bytes(&sec[tgOut_RelaDebugInfo], &er, (isize)sizeof(er));
	}

	for (isize i = 0; i < syms.count; i++) {
		tgb_bytes(&sec[tgOut_Symtab], &syms.data[i], (isize)sizeof(tgElfSym));
	}
	sec[tgOut_Strtab] = strtab;

	// section headers
	static char const *const names[tgOut_COUNT] = {
		"", ".text", ".rodata", ".data", ".bss", ".tdata", ".tbss", ".eh_frame", ".debug_abbrev", ".debug_info", ".debug_line", ".debug_ranges",
		".note.GNU-stack", ".rela.text", ".rela.data", ".rela.rodata", ".rela.tdata", ".rela.eh_frame",
		".rela.debug_info", ".rela.debug_line", ".rela.debug_ranges", ".symtab", ".strtab", ".shstrtab",
	};
	u32 name_off[tgOut_COUNT];
	memset(name_off, 0, sizeof(name_off));
	tgBytes *shstr = &sec[tgOut_Shstrtab];
	for (tgOutSection i = 0; i < tgOut_COUNT; i++) {
		if (!present[i]) continue;
		name_off[i] = (u32)shstr->count;
		tgb_cstr(shstr, names[i]);
	}
	tgElfShdr *nsh = (tgElfShdr *)calloc((usize)tg_max(2*named_count, (isize)1), sizeof(tgElfShdr));
	TG_ASSERT(nsh != NULL);
	for (isize i = 0; i < named_count; i++) {
		tgNamedSection const *ns = &m->named_sections.data[i];
		tgElfShdr *d = &nsh[2*i];
		d->sh_name = (u32)shstr->count;
		tgb_str(shstr, ns->name);
		d->sh_type = tg_elf_named_type(ns);
		d->sh_flags = 2; // SHF_ALLOC
		if (ns->flags & TYG_SECF_WRITE) d->sh_flags |= 1;     // SHF_WRITE
		if (ns->flags & TYG_SECF_TLS)   d->sh_flags |= 0x400; // SHF_TLS
		if (ns->flags & TYG_SECF_EXEC)  d->sh_flags |= 4;     // SHF_EXECINSTR
		d->sh_addralign = (u64)tg_max(ns->align, (i64)1);
		d->sh_entsize = (d->sh_type >= 14 && d->sh_type <= 16) ? 8 : 0;
		if (named[i].has_relocs) {
			tgElfShdr *r = &nsh[2*i + 1];
			r->sh_name = (u32)shstr->count;
			tgb_cstr(shstr, ".rela");
			tg_array_resize(shstr, shstr->count - 1); // ".rela" then the name, one terminator
			tgb_str(shstr, ns->name);
			r->sh_type = 4; // SHT_RELA
			r->sh_flags = 0x40; // SHF_INFO_LINK
			r->sh_addralign = 8;
			r->sh_entsize = 24;
			r->sh_info = named[i].shndx;
		}
	}

	tgElfShdr sh[tgOut_COUNT];
	memset(sh, 0, sizeof(sh));
	u64 const SHF_WRITE = 1, SHF_ALLOC = 2, SHF_EXEC = 4, SHF_INFO_LINK = 0x40, SHF_TLS = 0x400;
	u32 const SHT_PROGBITS = 1, SHT_SYMTAB = 2, SHT_STRTAB = 3, SHT_RELA = 4, SHT_NOBITS = 8;
	u32 const symtab = shndx[tgOut_Symtab];
	tg_elf_set(sh, name_off, tgOut_Text,        SHT_PROGBITS, SHF_ALLOC|SHF_EXEC, tg_elf_align_of(m, tgSection_Text), 0, 0, 0);
	tg_elf_set(sh, name_off, tgOut_Rodata,      SHT_PROGBITS, SHF_ALLOC, tg_elf_align_of(m, tgSection_Rodata), 0, 0, 0);
	tg_elf_set(sh, name_off, tgOut_Data,        SHT_PROGBITS, SHF_ALLOC|SHF_WRITE, tg_elf_align_of(m, tgSection_Data), 0, 0, 0);
	tg_elf_set(sh, name_off, tgOut_Bss,         SHT_NOBITS,   SHF_ALLOC|SHF_WRITE, tg_elf_align_of(m, tgSection_Bss), 0, 0, 0);
	tg_elf_set(sh, name_off, tgOut_TData,       SHT_PROGBITS, SHF_ALLOC|SHF_WRITE|SHF_TLS, tg_elf_align_of(m, tgSection_TData), 0, 0, 0);
	tg_elf_set(sh, name_off, tgOut_TBss,        SHT_NOBITS,   SHF_ALLOC|SHF_WRITE|SHF_TLS, tg_elf_align_of(m, tgSection_TBss), 0, 0, 0);
	tg_elf_set(sh, name_off, tgOut_EhFrame,     SHT_PROGBITS, SHF_ALLOC, 8, 0, 0, 0);
	tg_elf_set(sh, name_off, tgOut_DebugAbbrev, SHT_PROGBITS, 0, 1, 0, 0, 0);
	tg_elf_set(sh, name_off, tgOut_DebugInfo,   SHT_PROGBITS, 0, 1, 0, 0, 0);
	tg_elf_set(sh, name_off, tgOut_DebugLine,   SHT_PROGBITS, 0, 1, 0, 0, 0);
	tg_elf_set(sh, name_off, tgOut_DebugRanges, SHT_PROGBITS, 0, 1, 0, 0, 0);
	tg_elf_set(sh, name_off, tgOut_NoteStack,   SHT_PROGBITS, 0, 1, 0, 0, 0);
	tg_elf_set(sh, name_off, tgOut_RelaText,      SHT_RELA, SHF_INFO_LINK, 8, 24, symtab, shndx[tgOut_Text]);
	tg_elf_set(sh, name_off, tgOut_RelaData,      SHT_RELA, SHF_INFO_LINK, 8, 24, symtab, shndx[tgOut_Data]);
	tg_elf_set(sh, name_off, tgOut_RelaRodata,    SHT_RELA, SHF_INFO_LINK, 8, 24, symtab, shndx[tgOut_Rodata]);
	tg_elf_set(sh, name_off, tgOut_RelaTData,     SHT_RELA, SHF_INFO_LINK, 8, 24, symtab, shndx[tgOut_TData]);
	tg_elf_set(sh, name_off, tgOut_RelaEhFrame,   SHT_RELA, SHF_INFO_LINK, 8, 24, symtab, shndx[tgOut_EhFrame]);
	tg_elf_set(sh, name_off, tgOut_RelaDebugInfo, SHT_RELA, SHF_INFO_LINK, 8, 24, symtab, shndx[tgOut_DebugInfo]);
	tg_elf_set(sh, name_off, tgOut_RelaDebugLine, SHT_RELA, SHF_INFO_LINK, 8, 24, symtab, shndx[tgOut_DebugLine]);
	tg_elf_set(sh, name_off, tgOut_RelaDebugRanges, SHT_RELA, SHF_INFO_LINK, 8, 24, symtab, shndx[tgOut_DebugRanges]);
	tg_elf_set(sh, name_off, tgOut_Symtab,   SHT_SYMTAB, 0, 8, 24, shndx[tgOut_Strtab], first_global);
	tg_elf_set(sh, name_off, tgOut_Strtab,   SHT_STRTAB, 0, 1, 0, 0, 0);
	tg_elf_set(sh, name_off, tgOut_Shstrtab, SHT_STRTAB, 0, 1, 0, 0, 0);

	// layout: header, section data, section headers
	tg_array_clear(out);
	tg_array_reserve(out, 64 + m->sections[tgSection_Text].count*2);
	tg_array_resize(out, 64);
	for (tgOutSection i = 1; i < tgOut_COUNT; i++) {
		if (!present[i]) continue;
		tgb_align(out, (isize)tg_max(sh[i].sh_addralign, (u64)1), 0);
		sh[i].sh_offset = (u64)out->count;
		sh[i].sh_size = (u64)sec[i].count;
		tgb_bytes(out, sec[i].data, sec[i].count);
		if (i == tgOut_Bss)  sh[i].sh_size = (u64)m->nobits_size[tgSection_Bss];
		if (i == tgOut_TBss) sh[i].sh_size = (u64)m->nobits_size[tgSection_TBss];
	}
	for (isize i = 0; i < named_count; i++) {
		tgNamedSection const *ns = &m->named_sections.data[i];
		tgElfShdr *d = &nsh[2*i];
		tgb_align(out, (isize)tg_max(d->sh_addralign, (u64)1), 0);
		d->sh_offset = (u64)out->count;
		if (ns->flags & TYG_SECF_ZERO) {
			d->sh_size = (u64)ns->nobits_size;
		} else {
			d->sh_size = (u64)ns->bytes.count;
			tgb_bytes(out, ns->bytes.data, ns->bytes.count);
		}
		if (named[i].has_relocs) {
			tgElfShdr *r = &nsh[2*i + 1];
			tgb_align(out, 8, 0);
			r->sh_offset = (u64)out->count;
			r->sh_size = (u64)named[i].rela.count;
			r->sh_link = symtab;
			tgb_bytes(out, named[i].rela.data, named[i].rela.count);
		}
	}
	tgb_align(out, 8, 0);
	u64 shoff = (u64)out->count;
	for (tgOutSection i = 0; i < tgOut_COUNT; i++) {
		if (!present[i]) continue;
		tgb_bytes(out, &sh[i], (isize)sizeof(tgElfShdr));
	}
	for (isize i = 0; i < named_count; i++) {
		tgb_bytes(out, &nsh[2*i], (isize)sizeof(tgElfShdr));
		if (named[i].has_relocs) tgb_bytes(out, &nsh[2*i + 1], (isize)sizeof(tgElfShdr));
	}
	free(nsh);

	u8 *h = out->data;
	h[0] = 0x7f; h[1] = 'E'; h[2] = 'L'; h[3] = 'F';
	h[4] = 2; // 64 bit
	h[5] = 1; // little endian
	h[6] = 1; // version
	h[7] = 0; // System V
	u16 e_type = 1;      // relocatable
	u16 e_machine = arm64 ? 183 : 62; // aarch64 or x86-64
	u32 e_version = 1;
	u64 zero = 0;
	u16 e_ehsize = 64;
	u16 e_shentsize = (u16)sizeof(tgElfShdr);
	u16 e_shnum = (u16)shnum;
	u16 e_shstrndx = (u16)shndx[tgOut_Shstrtab];
	memmove(h + 16, &e_type, 2);
	memmove(h + 18, &e_machine, 2);
	memmove(h + 20, &e_version, 4);
	memmove(h + 24, &zero, 8); // entry
	memmove(h + 32, &zero, 8); // phoff
	memmove(h + 40, &shoff, 8);
	memmove(h + 52, &e_ehsize, 2);
	memmove(h + 58, &e_shentsize, 2);
	memmove(h + 60, &e_shnum, 2);
	memmove(h + 62, &e_shstrndx, 2);

	for (tgOutSection i = 0; i < tgOut_COUNT; i++) tg_array_free(&sec[i]);
	tg_array_free(&extra_relocs);
	tg_array_free(&info_relocs);
	tg_array_free(&line_relocs);
	tg_array_free(&ranges_relocs);
	tg_array_free(&info_sym_relocs);
	tg_array_free(&syms);
	free(sym_index);
	for (isize k = 0; k < named_count; k++) tg_array_free(&named[k].rela);
	free(named);
	return true;
}
