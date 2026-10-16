// Mach-O relocatable object writer for macOS, arm64 and x86-64.
//
// Ported from xb_macho.cpp, which wrote the object to a path; tinygen writes it into a
// byte buffer, which api.c writes to the file.
//
// All sections live in one unnamed segment, as in the objects clang writes. A section's
// file offset mirrors its address, so `offset - fileoff == addr`.
//
// The module's symbol names are C names: the writer adds the leading underscore of Mach-O
// C symbols to every one of them, defined, undefined or local.
//
// Thread locals follow the TLV scheme: the symbol names a descriptor in __thread_vars,
// which points at `__tlv_bootstrap` and at the initial value, the `$tlv$init` symbol.

enum {
	MACHO_CPU_TYPE_ARM64    = 0x0100000c,
	MACHO_CPU_TYPE_X86_64   = 0x01000007,
	MACHO_CPU_SUBTYPE_X86_64_ALL = 3,
	MACHO_MH_OBJECT         = 1,
	MACHO_MH_SUBSECTIONS_VIA_SYMBOLS = 0x2000,

	MACHO_LC_SYMTAB         = 0x2,
	MACHO_LC_DYSYMTAB       = 0xb,
	MACHO_LC_SEGMENT_64     = 0x19,
	MACHO_LC_BUILD_VERSION  = 0x32,
	MACHO_PLATFORM_MACOS    = 1,

	MACHO_S_REGULAR                    = 0x0,
	MACHO_S_ZEROFILL                   = 0x1,
	MACHO_S_THREAD_LOCAL_REGULAR       = 0x11,
	MACHO_S_THREAD_LOCAL_ZEROFILL      = 0x12,
	MACHO_S_THREAD_LOCAL_VARIABLES     = 0x13,
	MACHO_S_ATTR_SOME_INSTRUCTIONS     = 0x00000400,
	MACHO_S_ATTR_DEBUG                 = 0x02000000,

	MACHO_RELOC_UNSIGNED                  = 0, // the same number on both
	MACHO_ARM64_RELOC_BRANCH26            = 2,
	MACHO_ARM64_RELOC_PAGE21              = 3,
	MACHO_ARM64_RELOC_PAGEOFF12           = 4,
	MACHO_ARM64_RELOC_GOT_LOAD_PAGE21     = 5,
	MACHO_ARM64_RELOC_GOT_LOAD_PAGEOFF12  = 6,
	MACHO_ARM64_RELOC_TLVP_LOAD_PAGE21    = 8,
	MACHO_ARM64_RELOC_TLVP_LOAD_PAGEOFF12 = 9,

	MACHO_X86_64_RELOC_SIGNED   = 1,
	MACHO_X86_64_RELOC_BRANCH   = 2,
	MACHO_X86_64_RELOC_GOT_LOAD = 3,
	MACHO_X86_64_RELOC_SIGNED_1 = 6, // SIGNED_n: n immediate bytes follow the field
	MACHO_X86_64_RELOC_SIGNED_2 = 7,
	MACHO_X86_64_RELOC_SIGNED_4 = 8,
	MACHO_X86_64_RELOC_TLV      = 9,

	MACHO_UNWIND_ARM64_MODE_FRAME  = 0x04000000,
	MACHO_UNWIND_X86_64_MODE_RBP_FRAME = 0x01000000,
	MACHO_UNWIND_X86_64_MODE_STACK_IMMD = 0x02000000,
};

// these do not fit a C enum
#define MACHO_MH_MAGIC_64 0xfeedfacfu
#define MACHO_S_ATTR_PURE_INSTRUCTIONS 0x80000000u

enum {
	MACHO_N_UNDF = 0x0,
	MACHO_N_EXT  = 0x01,
	MACHO_N_SECT = 0x0e,
	MACHO_N_PEXT = 0x10,
};

enum {
	MACHO_N_WEAK_REF = 0x40,
	MACHO_N_WEAK_DEF = 0x80,
};

// The minimum macOS versions the objects ask for, as the original's default minimum_os_version_string:
// 11.0 is the first macOS on arm64; 10.13 is the oldest x86-64 macOS current toolchains target.
// clang itself writes the host's version.
enum {
	MACHO_MIN_MACOS_ARM64  = (11 << 16),
	MACHO_MIN_MACOS_X86_64 = (10 << 16) | (13 << 8),
};

typedef struct machoHeader {
	u32 magic;
	u32 cputype;
	u32 cpusubtype;
	u32 filetype;
	u32 ncmds;
	u32 sizeofcmds;
	u32 flags;
	u32 reserved;
} machoHeader;

typedef struct machoSegmentCommand {
	u32  cmd;
	u32  cmdsize;
	char segname[16];
	u64  vmaddr;
	u64  vmsize;
	u64  fileoff;
	u64  filesize;
	u32  maxprot;
	u32  initprot;
	u32  nsects;
	u32  flags;
} machoSegmentCommand;

typedef struct machoSection {
	char sectname[16];
	char segname[16];
	u64  addr;
	u64  size;
	u32  offset;
	u32  align;
	u32  reloff;
	u32  nreloc;
	u32  flags;
	u32  reserved1;
	u32  reserved2;
	u32  reserved3;
} machoSection;

typedef struct machoBuildVersion {
	u32 cmd;
	u32 cmdsize;
	u32 platform;
	u32 minos;
	u32 sdk;
	u32 ntools;
} machoBuildVersion;

typedef struct machoSymtabCommand {
	u32 cmd;
	u32 cmdsize;
	u32 symoff;
	u32 nsyms;
	u32 stroff;
	u32 strsize;
} machoSymtabCommand;

typedef struct machoDysymtabCommand {
	u32 cmd;
	u32 cmdsize;
	u32 ilocalsym;
	u32 nlocalsym;
	u32 iextdefsym;
	u32 nextdefsym;
	u32 iundefsym;
	u32 nundefsym;
	u32 rest[12];
} machoDysymtabCommand;

// packed like the file's nlist_64: 16 bytes, n_value at 8
typedef struct machoNlist {
	u32 n_strx;
	u8  n_type;
	u8  n_sect;
	u16 n_desc;
	u64 n_value;
} machoNlist;

typedef struct machoReloc {
	i32 r_address;
	u32 r_info; // symbolnum:24, pcrel:1, length:2, extern:1, type:4
} machoReloc;

// A relocation before the symbol table is ordered: `sym` indexes the output symbols,
// or the output sections when `by_section`.
typedef struct machoPendingReloc {
	i32  address;
	i32  sym;
	u32  type;
	bool pcrel;
	u32  length; // log2 of the size
	bool by_section;
} machoPendingReloc;

TG_ARRAY(machoPendingRelocArray, machoPendingReloc);

typedef i32 machoOutSection;
enum {
	machoOut_Text,
	machoOut_Const,      // read only data without relocations
	machoOut_DataConst,  // read only data with relocations
	machoOut_Data,
	machoOut_ThreadVars,
	machoOut_ThreadData,
	machoOut_DebugAbbrev,
	machoOut_DebugInfo,
	machoOut_DebugLine,
	machoOut_DebugRanges,
	machoOut_CompactUnwind,
	machoOut_Bss,        // the zerofill sections come last
	machoOut_ThreadBss,
	machoOut_COUNT,
};

// A symbol of the output: one of the module's, or one this writer adds.
typedef struct machoOutSym {
	tgString name;   // with the leading underscore, on the heap
	u8       n_type;
	u16      n_desc;
	i32      out_section; // -1 if undefined
	i64      offset;      // within the output section
	u32      index;       // in the symbol table
} machoOutSym;

TG_ARRAY(machoOutSymArray, machoOutSym);

// An address in the DWARF sections, relocated against its section, or an undefined symbol.
typedef struct machoDwarfAddr { i32 sect; i64 offset; i32 target; i64 target_offset; i32 ext_sym; } machoDwarfAddr;
TG_ARRAY(machoDwarfAddrArray, machoDwarfAddr);

TG_INTERNAL void macho_set_name16(char *dst, char const *src) {
	memset(dst, 0, 16);
	isize n = (isize)strlen(src);
	memmove(dst, src, (usize)tg_min(n, (isize)16));
}

typedef struct machoSortKey {
	tgString name;
	i32      index;
} machoSortKey;

// equal names keep their order
TG_INTERNAL int macho_sort_key_cmp(void const *a, void const *b) {
	machoSortKey const *x = (machoSortKey const *)a;
	machoSortKey const *y = (machoSortKey const *)b;
	isize n = tg_min(x->name.len, y->name.len);
	int c = n > 0 ? memcmp(x->name.text, y->name.text, (usize)n) : 0;
	if (c == 0) c = x->name.len < y->name.len ? -1 : x->name.len > y->name.len ? 1 : 0;
	if (c != 0) return c;
	return x->index < y->index ? -1 : x->index > y->index ? 1 : 0;
}

// What the lambdas of xb_write_macho captured.
typedef struct machoWriter {
	tgModule *        m;
	machoOutSection   rodata_out;
	machoOutSymArray  out_syms;
} machoWriter;

TG_INTERNAL i32 macho_out_section_of(machoWriter *w, tgSection s) {
	switch (s) {
	case tgSection_Text:   return machoOut_Text;
	case tgSection_Rodata: return w->rodata_out;
	case tgSection_Data:   return machoOut_Data;
	case tgSection_Bss:    return machoOut_Bss;
	case tgSection_TData:  return machoOut_ThreadData;
	case tgSection_TBss:   return machoOut_ThreadBss;
	}
	return -1;
}

TG_INTERNAL i32 macho_add_sym(machoWriter *w, tgString name, char const *prefix, char const *suffix) {
	machoOutSym os;
	memset(&os, 0, sizeof(os));
	isize pl = (isize)strlen(prefix), sl = (isize)strlen(suffix);
	isize len = pl + name.len + sl;
	char *s = (char *)malloc((usize)len + 1);
	TG_ASSERT(s != NULL);
	memcpy(s, prefix, (usize)pl);
	if (name.len) memcpy(s + pl, name.text, (usize)name.len);
	memcpy(s + pl + name.len, suffix, (usize)sl);
	s[len] = 0;
	os.name = tg_str_make(s, len);
	os.out_section = -1;
	tg_array_add(&w->out_syms, os);
	return (i32)(w->out_syms.count - 1);
}

TG_INTERNAL machoPendingReloc macho_pending_reloc(i32 address, i32 sym, u32 type, bool pcrel, u32 length, bool by_section) {
	machoPendingReloc r;
	r.address = address;
	r.sym = sym;
	r.type = type;
	r.pcrel = pcrel;
	r.length = length;
	r.by_section = by_section;
	return r;
}

// Writes the module as a Mach-O relocatable object into `out`.
TG_INTERNAL bool tg_write_macho(tgModule *m, tgBytes *out) {
	tgBytes sec[machoOut_COUNT];
	i64 sec_size[machoOut_COUNT];
	i64 sec_align[machoOut_COUNT];
	machoPendingRelocArray sec_relocs[machoOut_COUNT];
	memset(sec, 0, sizeof(sec));
	memset(sec_size, 0, sizeof(sec_size));
	memset(sec_relocs, 0, sizeof(sec_relocs));
	for (isize i = 0; i < machoOut_COUNT; i++) sec_align[i] = 1;

	bool arm64 = tg_is_arm64();
	machoWriter w;
	memset(&w, 0, sizeof(w));
	w.m = m;

	bool rodata_has_relocs = false;
	for (isize i = 0; i < m->relocs.count; i++) {
		if (m->relocs.data[i].section == tgSection_Rodata) rodata_has_relocs = true;
	}
	w.rodata_out = rodata_has_relocs ? machoOut_DataConst : machoOut_Const;

	{
		tgSection const take[4] = {tgSection_Text, tgSection_Rodata, tgSection_Data, tgSection_TData};
		for (isize k = 0; k < 4; k++) {
			tgSection from = take[k];
			i32 to = macho_out_section_of(&w, from);
			tgb_bytes(&sec[to], m->sections[from].data, m->sections[from].count);
			sec_size[to] = m->sections[from].count;
			sec_align[to] = tg_max(m->section_align[from], (i64)1);
		}
	}
	sec_align[machoOut_Text] = tg_max(sec_align[machoOut_Text], (i64)4);
	sec_size[machoOut_Bss] = m->nobits_size[tgSection_Bss];
	sec_align[machoOut_Bss] = tg_max(m->section_align[tgSection_Bss], (i64)1);
	sec_size[machoOut_ThreadBss] = m->nobits_size[tgSection_TBss];
	sec_align[machoOut_ThreadBss] = tg_max(m->section_align[tgSection_TBss], (i64)1);

	// symbols
	tg_array_reserve(&w.out_syms, m->symbols.count + 16);
	isize nsym = tg_max(m->symbols.count, (isize)1);
	i32 *sym_out = (i32 *)calloc((usize)nsym, sizeof(i32)); // module symbol -> out_syms index

	// only undefined symbols that something refers to, the linker reports every other one
	bool *referenced = (bool *)calloc((usize)nsym, sizeof(bool));
	TG_ASSERT(sym_out != NULL && referenced != NULL);
	for (isize i = 0; i < m->relocs.count; i++) {
		referenced[m->relocs.data[i].sym] = true;
	}

	i32 tlv_bootstrap = -1;
	for (isize i = 0; i < m->symbols.count; i++) {
		tgSymbol const *s = &m->symbols.data[i];
		sym_out[i] = -1;
		bool global = (s->flags & tgSymbolFlag_Global) != 0;
		if (s->section == tgSection_Undef) {
			if (!referenced[i]) continue;
			i32 o = macho_add_sym(&w, s->name, "_", "");
			w.out_syms.data[o].n_type = MACHO_N_UNDF | MACHO_N_EXT;
			if (s->flags & tgSymbolFlag_Weak) w.out_syms.data[o].n_desc |= MACHO_N_WEAK_REF;
			sym_out[i] = o;
			continue;
		}

		u8 n_type = MACHO_N_SECT;
		u16 n_desc = 0;
		if (global) {
			n_type |= MACHO_N_EXT;
			if ((s->flags & tgSymbolFlag_Hidden) && (s->flags & tgSymbolFlag_Export) == 0) {
				n_type |= MACHO_N_PEXT;
			}
			if (s->flags & tgSymbolFlag_Weak) {
				n_desc |= MACHO_N_WEAK_DEF;
			}
		}

		if (s->flags & tgSymbolFlag_TLS) {
			// the initial value, then the descriptor that carries the symbol's own name
			i32 init = macho_add_sym(&w, s->name, "_", "$tlv$init");
			w.out_syms.data[init].n_type = MACHO_N_SECT;
			w.out_syms.data[init].out_section = macho_out_section_of(&w, s->section);
			w.out_syms.data[init].offset = s->offset;

			if (tlv_bootstrap < 0) {
				tlv_bootstrap = macho_add_sym(&w, tg_str_lit("_tlv_bootstrap"), "_", "");
				w.out_syms.data[tlv_bootstrap].n_type = MACHO_N_UNDF | MACHO_N_EXT;
			}
			tgBytes *tv = &sec[machoOut_ThreadVars];
			i64 at = tv->count;
			tg_array_resize(tv, at + 24); // zeroed
			sec_size[machoOut_ThreadVars] = tv->count;
			sec_align[machoOut_ThreadVars] = 8;

			i32 o = macho_add_sym(&w, s->name, "_", "");
			w.out_syms.data[o].n_type = n_type;
			w.out_syms.data[o].n_desc = n_desc;
			w.out_syms.data[o].out_section = machoOut_ThreadVars;
			w.out_syms.data[o].offset = at;
			sym_out[i] = o;

			// symbol numbers are patched in once the table is ordered
			tg_array_add(&sec_relocs[machoOut_ThreadVars], macho_pending_reloc((i32)at, tlv_bootstrap, MACHO_RELOC_UNSIGNED, false, 3, false));
			tg_array_add(&sec_relocs[machoOut_ThreadVars], macho_pending_reloc((i32)(at + 16), init, MACHO_RELOC_UNSIGNED, false, 3, false));
			continue;
		}

		i32 o = macho_add_sym(&w, s->name, "_", "");
		w.out_syms.data[o].n_type = n_type;
		w.out_syms.data[o].n_desc = n_desc;
		w.out_syms.data[o].out_section = macho_out_section_of(&w, s->section);
		w.out_syms.data[o].offset = s->offset;
		sym_out[i] = o;
	}

	for (isize i = 0; i < m->relocs.count; i++) {
		tgReloc const *r = &m->relocs.data[i];
		i32 to = macho_out_section_of(&w, r->section);
		TG_ASSERT(to >= 0);
		TG_ASSERT_MSG(sym_out[r->sym] >= 0, m->symbols.data[r->sym].name.text);
		machoPendingReloc ar = macho_pending_reloc((i32)r->offset, sym_out[r->sym], 0, false, 2, false);
		// x86-64 stores the addend in place, counted from the end of the field like COFF
		i32 rel_addend = (i32)(r->addend + 4);
		switch (r->kind) {
		case tgReloc_Abs64:
			memmove(sec[to].data + r->offset, &r->addend, 8);
			ar.type = MACHO_RELOC_UNSIGNED;
			ar.length = 3;
			break;
		case tgReloc_PC32:
			memmove(sec[to].data + r->offset, &rel_addend, 4);
			ar.pcrel = true;
			switch (r->tail) {
			case 0: ar.type = MACHO_X86_64_RELOC_SIGNED;   break;
			case 1: ar.type = MACHO_X86_64_RELOC_SIGNED_1; break;
			case 2: ar.type = MACHO_X86_64_RELOC_SIGNED_2; break;
			case 4: ar.type = MACHO_X86_64_RELOC_SIGNED_4; break;
			default: TG_PANIC("rip relative field followed by an odd number of bytes");
			}
			break;
		case tgReloc_PLT32:
		case tgReloc_REX_GOTPCRELX:
		case tgReloc_TLV:
			// a call, a mov from the GOT, a mov of the descriptor's address: no addend
			TG_ASSERT(rel_addend == 0);
			ar.pcrel = true;
			ar.type = r->kind == tgReloc_PLT32 ? MACHO_X86_64_RELOC_BRANCH :
			          r->kind == tgReloc_TLV   ? MACHO_X86_64_RELOC_TLV : MACHO_X86_64_RELOC_GOT_LOAD;
			break;
		// the code relocations carry no addend, the instruction fields stay zero
		case tgReloc_A64_Branch26:     ar.type = MACHO_ARM64_RELOC_BRANCH26;            ar.pcrel = true; break;
		case tgReloc_A64_Page21:       ar.type = MACHO_ARM64_RELOC_PAGE21;              ar.pcrel = true; break;
		case tgReloc_A64_PageOff12:    ar.type = MACHO_ARM64_RELOC_PAGEOFF12;           break;
		case tgReloc_A64_GotPage21:    ar.type = MACHO_ARM64_RELOC_GOT_LOAD_PAGE21;     ar.pcrel = true; break;
		case tgReloc_A64_GotPageOff12: ar.type = MACHO_ARM64_RELOC_GOT_LOAD_PAGEOFF12;  break;
		case tgReloc_A64_TlvPage21:    ar.type = MACHO_ARM64_RELOC_TLVP_LOAD_PAGE21;    ar.pcrel = true; break;
		case tgReloc_A64_TlvPageOff12: ar.type = MACHO_ARM64_RELOC_TLVP_LOAD_PAGEOFF12; break;
		default:
			TG_PANIC("relocation kind has no Mach-O form");
		}
		TG_ASSERT(r->addend == 0 || r->kind == tgReloc_Abs64 || !arm64);
		tg_array_add(&sec_relocs[to], ar);
	}

	// One compact unwind entry per procedure. Every frame has the standard frame record:
	// on arm64 the callee saved pairs sit right below it, on x86-64 the saved registers
	// take the slots right below rbp.
	for (isize pi = 0; pi < m->proc_debug.count; pi++) {
		tgProcDebug const *pd = &m->proc_debug.data[pi];
		tgBytes *cu = &sec[machoOut_CompactUnwind];
		i64 at = cu->count;
		tg_array_resize(cu, at + 32); // zeroed
		u32 length = (u32)(pd->end - pd->start);
		u32 encoding = 0;
		if (arm64) {
			encoding = MACHO_UNWIND_ARM64_MODE_FRAME;
			for (isize si = 0; si < pd->saved_regs.count; si++) {
				tgSavedReg const *s = &pd->saved_regs.data[si];
				// dwarf x19-x28, then d8-d15 as 72-79
				if (s->dwarf_reg >= 19 && s->dwarf_reg <= 28) encoding |= 1u << ((s->dwarf_reg - 19) / 2);
				if (s->dwarf_reg >= 72 && s->dwarf_reg <= 79) encoding |= 0x100u << ((s->dwarf_reg - 72) / 2);
			}
		} else if (pd->naked) {
			// no frame, the stack holds only the return address
			encoding = MACHO_UNWIND_X86_64_MODE_STACK_IMMD | (1u << 16);
		} else {
			// the slot count below rbp, then a 3 bit register number per slot, the lowest slot first
			i32 slots = 0;
			for (isize si = 0; si < pd->saved_regs.count; si++) slots = tg_max(slots, (i32)(-pd->saved_regs.data[si].frame_offset / 8));
			TG_ASSERT(slots <= 5);
			encoding = MACHO_UNWIND_X86_64_MODE_RBP_FRAME | ((u32)slots << 16);
			for (isize si = 0; si < pd->saved_regs.count; si++) {
				tgSavedReg const *s = &pd->saved_regs.data[si];
				// dwarf rbx 3, r12-r15 12-15; compact unwind rbx 1, r12-r15 2-5
				u32 reg = s->dwarf_reg == 3 ? 1 : (u32)(s->dwarf_reg - 10);
				TG_ASSERT(reg >= 1 && reg <= 5);
				i32 slot = slots - (i32)(-s->frame_offset / 8);
				encoding |= reg << (3*slot);
			}
		}
		memmove(cu->data + at + 8, &length, 4);
		memmove(cu->data + at + 12, &encoding, 4);
		TG_ASSERT(sym_out[pd->sym] >= 0);
		tg_array_add(&sec_relocs[machoOut_CompactUnwind], macho_pending_reloc((i32)at, sym_out[pd->sym], MACHO_RELOC_UNSIGNED, false, 3, false));
	}
	sec_size[machoOut_CompactUnwind] = sec[machoOut_CompactUnwind].count;
	sec_align[machoOut_CompactUnwind] = 8;

	// DWARF stays in the object: dsymutil and lldb read it there through the executable's debug map.
	// Its addresses are object addresses with a relocation against their section, as clang writes them.
	machoDwarfAddrArray dwarf_addrs;
	memset(&dwarf_addrs, 0, sizeof(dwarf_addrs));
	if (tg_debug_info()) {
		tgDwarf d;
		tg_dwarf_build(m, &d);
		sec[machoOut_DebugAbbrev] = d.abbrev;
		sec[machoOut_DebugInfo] = d.info;
		sec[machoOut_DebugLine] = d.line;
		sec[machoOut_DebugRanges] = d.ranges;
		for (i32 i = machoOut_DebugAbbrev; i <= machoOut_DebugRanges; i++) {
			sec_size[i] = sec[i].count;
		}
		for (i32 pass = 0; pass < 2; pass++) {
			i32 sect = pass == 0 ? machoOut_DebugInfo : machoOut_DebugLine;
			tgDwarfAddrArray const *addrs = pass == 0 ? &d.info_addrs : &d.line_addrs;
			for (isize i = 0; i < addrs->count; i++) {
				tgDwarfAddr const *a = &addrs->data[i];
				machoDwarfAddr da;
				da.sect = sect;
				da.offset = (i64)a->offset;
				da.target = machoOut_Text;
				da.target_offset = a->addend;
				da.ext_sym = -1;
				if (a->sym >= 0) {
					i32 o = sym_out[a->sym];
					if (o < 0) continue; // an unused undefined symbol, the address stays 0
					da.target = w.out_syms.data[o].out_section;
					da.target_offset = w.out_syms.data[o].offset;
					if (da.target < 0) da.ext_sym = o;
				}
				tg_array_add(&dwarf_addrs, da);
			}
		}
		// the section buffers moved to sec[]
		memset(&d.abbrev, 0, sizeof(d.abbrev));
		memset(&d.info, 0, sizeof(d.info));
		memset(&d.line, 0, sizeof(d.line));
		memset(&d.ranges, 0, sizeof(d.ranges));
		tg_dwarf_free(&d);
	}

	// the table is ordered: locals, defined externals, undefined externals
	tgI32Array order;
	memset(&order, 0, sizeof(order));
	tg_array_reserve(&order, w.out_syms.count);
	u32 nlocal = 0, nextdef = 0, nundef = 0;
	for (i32 pass = 0; pass < 3; pass++) {
		isize first = order.count;
		for (isize i = 0; i < w.out_syms.count; i++) {
			machoOutSym const *os = &w.out_syms.data[i];
			bool ext = (os->n_type & MACHO_N_EXT) != 0;
			bool undef = os->out_section < 0;
			i32 kind = !ext ? 0 : undef ? 2 : 1;
			if (kind == pass) tg_array_add(&order, (i32)i);
		}
		isize n = order.count - first;
		if (pass > 0 && n > 0) {
			// sorted by name, like the objects of other tools
			machoSortKey *keys = (machoSortKey *)calloc((usize)n, sizeof(machoSortKey));
			TG_ASSERT(keys != NULL);
			for (isize k = 0; k < n; k++) {
				keys[k].name = w.out_syms.data[order.data[first+k]].name;
				keys[k].index = order.data[first+k];
			}
			qsort(keys, (usize)n, sizeof(machoSortKey), macho_sort_key_cmp);
			for (isize k = 0; k < n; k++) {
				order.data[first+k] = keys[k].index;
			}
			free(keys);
		}
		if (pass == 0) nlocal = (u32)n;
		if (pass == 1) nextdef = (u32)n;
		if (pass == 2) nundef = (u32)n;
	}
	for (isize i = 0; i < order.count; i++) {
		w.out_syms.data[order.data[i]].index = (u32)i;
	}

	// sections: the ones with contents, the zerofill ones last
	typedef struct SectInfo { char const *sect; char const *seg; u32 flags; bool zerofill; } SectInfo;
	SectInfo const infos[machoOut_COUNT] = {
		{"__text",        "__TEXT", MACHO_S_REGULAR | MACHO_S_ATTR_PURE_INSTRUCTIONS | MACHO_S_ATTR_SOME_INSTRUCTIONS, false},
		{"__const",       "__TEXT", MACHO_S_REGULAR, false},
		{"__const",       "__DATA", MACHO_S_REGULAR, false},
		{"__data",        "__DATA", MACHO_S_REGULAR, false},
		{"__thread_vars", "__DATA", MACHO_S_THREAD_LOCAL_VARIABLES, false},
		{"__thread_data", "__DATA", MACHO_S_THREAD_LOCAL_REGULAR, false},
		{"__debug_abbrev", "__DWARF", MACHO_S_REGULAR | MACHO_S_ATTR_DEBUG, false},
		{"__debug_info",   "__DWARF", MACHO_S_REGULAR | MACHO_S_ATTR_DEBUG, false},
		{"__debug_line",   "__DWARF", MACHO_S_REGULAR | MACHO_S_ATTR_DEBUG, false},
		{"__debug_ranges", "__DWARF", MACHO_S_REGULAR | MACHO_S_ATTR_DEBUG, false},
		{"__compact_unwind", "__LD", MACHO_S_REGULAR | MACHO_S_ATTR_DEBUG, false},
		{"__bss",         "__DATA", MACHO_S_ZEROFILL, true},
		{"__thread_bss",  "__DATA", MACHO_S_THREAD_LOCAL_ZEROFILL, true},
	};
	i32 sect_number[machoOut_COUNT]; // 1 based, 0 if not written
	i64 sect_addr[machoOut_COUNT];
	memset(sect_number, 0, sizeof(sect_number));
	memset(sect_addr, 0, sizeof(sect_addr));
	i32 nsects = 0;
	i64 addr = 0;
	i64 file_end_addr = 0;
	for (isize i = 0; i < machoOut_COUNT; i++) {
		if (sec_size[i] == 0) continue;
		addr = tg_align_up(addr, sec_align[i]);
		sect_addr[i] = addr;
		sect_number[i] = ++nsects;
		addr += sec_size[i];
		if (!infos[i].zerofill) file_end_addr = addr;
	}
	i64 vmsize = addr;

	u32 sizeofcmds = (u32)(sizeof(machoSegmentCommand) + (usize)nsects*sizeof(machoSection) +
	                       sizeof(machoBuildVersion) + sizeof(machoSymtabCommand) + sizeof(machoDysymtabCommand));
	i64 data_start = tg_align_up((i64)(sizeof(machoHeader) + sizeofcmds), 16);
	i64 max_align = 16;
	for (isize i = 0; i < machoOut_COUNT; i++) {
		if (sect_number[i]) max_align = tg_max(max_align, sec_align[i]);
	}
	// section addresses only line up with file offsets when the data starts that aligned
	data_start = tg_align_up(data_start, max_align);

	for (isize i = 0; i < dwarf_addrs.count; i++) {
		machoDwarfAddr const *da = &dwarf_addrs.data[i];
		machoPendingReloc r = macho_pending_reloc((i32)da->offset, da->ext_sym, MACHO_RELOC_UNSIGNED, false, 3, false);
		u64 value = 0;
		if (da->ext_sym < 0) {
			TG_ASSERT(sect_number[da->target] != 0);
			value = (u64)(sect_addr[da->target] + da->target_offset);
			r.sym = da->target;
			r.by_section = true;
		}
		memmove(sec[da->sect].data + da->offset, &value, 8);
		tg_array_add(&sec_relocs[da->sect], r);
	}

	tg_array_clear(out);
	tg_array_reserve(out, data_start + file_end_addr + 4096);
	tg_array_resize(out, data_start + file_end_addr); // zeroed
	memset(out->data, 0, (usize)out->count);
	for (isize i = 0; i < machoOut_COUNT; i++) {
		if (!sect_number[i] || infos[i].zerofill) continue;
		if (sec[i].count) memmove(out->data + data_start + sect_addr[i], sec[i].data, (usize)sec[i].count);
	}

	// relocations, with the final symbol numbers
	i64 reloff[machoOut_COUNT];
	memset(reloff, 0, sizeof(reloff));
	tgb_align(out, 8, 0);
	for (isize i = 0; i < machoOut_COUNT; i++) {
		if (!sect_number[i]) continue;
		reloff[i] = out->count;
		for (isize k = 0; k < sec_relocs[i].count; k++) {
			machoPendingReloc const *pr = &sec_relocs[i].data[k];
			machoReloc r;
			r.r_address = pr->address;
			if (pr->by_section) {
				r.r_info = (u32)sect_number[pr->sym] | ((pr->pcrel ? 1u : 0u) << 24) | (pr->length << 25) | (pr->type << 28);
			} else {
				r.r_info = w.out_syms.data[pr->sym].index | ((pr->pcrel ? 1u : 0u) << 24) | (pr->length << 25) | (1u << 27) | (pr->type << 28);
			}
			tgb_bytes(out, &r, (isize)sizeof(r));
		}
	}

	// the symbol and string tables
	tgBytes strtab;
	memset(&strtab, 0, sizeof(strtab));
	tg_array_reserve(&strtab, 4096);
	tgb_u8(&strtab, ' ');
	tgb_u8(&strtab, 0);
	tgb_align(out, 8, 0);
	i64 symoff = out->count;
	for (isize i = 0; i < order.count; i++) {
		machoOutSym const *os = &w.out_syms.data[order.data[i]];
		machoNlist nl;
		memset(&nl, 0, sizeof(nl));
		nl.n_strx = (u32)strtab.count;
		tgb_str(&strtab, os->name);
		nl.n_type = os->n_type;
		nl.n_desc = os->n_desc;
		if (os->out_section >= 0) {
			TG_ASSERT(sect_number[os->out_section] != 0);
			nl.n_sect = (u8)sect_number[os->out_section];
			nl.n_value = (u64)(sect_addr[os->out_section] + os->offset);
		}
		tgb_bytes(out, &nl, (isize)sizeof(nl));
	}
	tgb_align(&strtab, 8, 0);
	i64 stroff = out->count;
	tgb_bytes(out, strtab.data, strtab.count);

	// header and load commands
	tgBytes cmds;
	memset(&cmds, 0, sizeof(cmds));
	tg_array_reserve(&cmds, sizeofcmds);
	{
		machoSegmentCommand sc;
		memset(&sc, 0, sizeof(sc));
		sc.cmd = MACHO_LC_SEGMENT_64;
		sc.cmdsize = (u32)(sizeof(machoSegmentCommand) + (usize)nsects*sizeof(machoSection));
		sc.vmaddr = 0;
		sc.vmsize = (u64)vmsize;
		sc.fileoff = (u64)data_start;
		sc.filesize = (u64)file_end_addr;
		sc.maxprot = 7;
		sc.initprot = 7;
		sc.nsects = (u32)nsects;
		tgb_bytes(&cmds, &sc, (isize)sizeof(sc));
		for (isize i = 0; i < machoOut_COUNT; i++) {
			if (!sect_number[i]) continue;
			machoSection s;
			memset(&s, 0, sizeof(s));
			macho_set_name16(s.sectname, infos[i].sect);
			macho_set_name16(s.segname, infos[i].seg);
			s.addr = (u64)sect_addr[i];
			s.size = (u64)sec_size[i];
			s.offset = infos[i].zerofill ? 0 : (u32)(data_start + sect_addr[i]);
			u32 align_log2 = 0;
			while (((i64)1 << align_log2) < sec_align[i]) align_log2 += 1;
			s.align = align_log2;
			s.nreloc = (u32)sec_relocs[i].count;
			s.reloff = s.nreloc ? (u32)reloff[i] : 0;
			s.flags = infos[i].flags;
			tgb_bytes(&cmds, &s, (isize)sizeof(s));
		}
	}
	{
		machoBuildVersion bv;
		memset(&bv, 0, sizeof(bv));
		bv.cmd = MACHO_LC_BUILD_VERSION;
		bv.cmdsize = (u32)sizeof(bv);
		bv.platform = MACHO_PLATFORM_MACOS;
		bv.minos = arm64 ? MACHO_MIN_MACOS_ARM64 : MACHO_MIN_MACOS_X86_64;
		tgb_bytes(&cmds, &bv, (isize)sizeof(bv));
	}
	{
		machoSymtabCommand st;
		memset(&st, 0, sizeof(st));
		st.cmd = MACHO_LC_SYMTAB;
		st.cmdsize = (u32)sizeof(st);
		st.symoff = (u32)symoff;
		st.nsyms = (u32)order.count;
		st.stroff = (u32)stroff;
		st.strsize = (u32)strtab.count;
		tgb_bytes(&cmds, &st, (isize)sizeof(st));
	}
	{
		machoDysymtabCommand dt;
		memset(&dt, 0, sizeof(dt));
		dt.cmd = MACHO_LC_DYSYMTAB;
		dt.cmdsize = (u32)sizeof(dt);
		dt.ilocalsym = 0;
		dt.nlocalsym = nlocal;
		dt.iextdefsym = nlocal;
		dt.nextdefsym = nextdef;
		dt.iundefsym = nlocal + nextdef;
		dt.nundefsym = nundef;
		tgb_bytes(&cmds, &dt, (isize)sizeof(dt));
	}
	TG_ASSERT(cmds.count == (isize)sizeofcmds);

	machoHeader h;
	memset(&h, 0, sizeof(h));
	h.magic = MACHO_MH_MAGIC_64;
	h.cputype = arm64 ? MACHO_CPU_TYPE_ARM64 : MACHO_CPU_TYPE_X86_64;
	h.cpusubtype = arm64 ? 0 : MACHO_CPU_SUBTYPE_X86_64_ALL;
	h.filetype = MACHO_MH_OBJECT;
	h.ncmds = 4;
	h.sizeofcmds = sizeofcmds;
	// the linker splits sections at symbols, so a weak duplicate drops only its own bytes
	h.flags = MACHO_MH_SUBSECTIONS_VIA_SYMBOLS;
	memmove(out->data, &h, sizeof(h));
	memmove(out->data + sizeof(h), cmds.data, (usize)cmds.count);

	for (isize i = 0; i < machoOut_COUNT; i++) {
		tg_array_free(&sec[i]);
		tg_array_free(&sec_relocs[i]);
	}
	for (isize i = 0; i < w.out_syms.count; i++) free((void *)w.out_syms.data[i].name.text);
	tg_array_free(&w.out_syms);
	tg_array_free(&dwarf_addrs);
	tg_array_free(&order);
	tg_array_free(&strtab);
	tg_array_free(&cmds);
	free(sym_out);
	free(referenced);
	return true;
}
