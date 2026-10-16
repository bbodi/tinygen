// The module: symbols, data in sections, relocations in data, debug files, and
// appending lowered procedures to .text.
//
// Every array of the module lives on the heap (arena = NULL). Strings the module
// keeps (symbol and file names) are copies in m->arena.

TG_INTERNAL i32 x64_helper(tgModule *m, bool set);

// A zero terminated copy of the string that lives as long as the module.
TG_INTERNAL tgString tg_module_string(tgModule *m, tgString s) {
	char *p = (char *)tg_arena_alloc(&m->arena, s.len + 1, 1);
	if (s.len) memcpy(p, s.text, (usize)s.len);
	return tg_str_make(p, s.len);
}

////////////////////////////////////////////////////////////////
// Symbols
////////////////////////////////////////////////////////////////

// Finds or adds the symbol by name. A new symbol is global and undefined.
TG_INTERNAL i32 tg_symbol(tgModule *m, tgString name) {
	i32 index = -1;
	u64 h = tg_strmap_hash(name);
	i64 *found = tg_strmap_get_hashed(&m->symbol_map, name, h);
	if (found != NULL) {
		index = (i32)*found;
	} else {
		tgSymbol s;
		memset(&s, 0, sizeof(s));
		s.name = tg_module_string(m, name);
		s.section = tgSection_Undef;
		s.flags = tgSymbolFlag_Global;
		index = (i32)m->symbols.count;
		tg_array_add(&m->symbols, s);
		tg_strmap_set_hashed(&m->symbol_map, s.name, h, index);
	}
	return index;
}

TG_INTERNAL void tg_sym_add_flags(tgModule *m, i32 sym, u16 flags) {
	m->symbols.data[sym].flags |= flags;
}

TG_INTERNAL void tg_sym_set_flags(tgModule *m, i32 sym, u16 flags) {
	m->symbols.data[sym].flags = flags;
}

TG_INTERNAL void tg_sym_set_realign(tgModule *m, i32 sym, i64 realign) {
	m->symbols.data[sym].realign = realign;
}

// The symbol at the end of sym's chain of aliases, sym itself when it is no alias, or NULL
// when the chain is a cycle.
TG_INTERNAL tgSymbol *tg_alias_target(tgModule *m, i32 sym) {
	tgSymbol *t = &m->symbols.data[sym];
	isize hops = 0;
	while (t->alias_of != 0 && hops++ <= m->symbols.count) t = &m->symbols.data[t->alias_of - 1];
	return t->alias_of != 0 ? NULL : t;
}

// An alias of a thread local is one too. A use of the alias takes that over from the target,
// since the code that reaches the alias depends on it before the object is written.
TG_INTERNAL void tg_alias_take_tls(tgModule *m, i32 sym) {
	tgSymbol *s = &m->symbols.data[sym];
	if (s->alias_of == 0) return;
	tgSymbol const *t = tg_alias_target(m, sym);
	if (t != NULL && (t->flags & tgSymbolFlag_TLS)) {
		s->flags |= tgSymbolFlag_TLS;
		s->realign = t->realign;
	}
}

// Places the symbol in a data section.
TG_INTERNAL void tg_sym_define(tgModule *m, i32 sym, tgSection section, i64 offset, i64 size) {
	tgSymbol *s = &m->symbols.data[sym];
	s->section = section;
	s->offset = offset;
	s->size = size;
}

////////////////////////////////////////////////////////////////
// Sections
////////////////////////////////////////////////////////////////

// A section by its tgSection: one of the fixed ones, or a named one past tgSection_COUNT.
TG_INTERNAL tgNamedSection *tg_named_section(tgModule *m, tgSection sec) {
	TG_ASSERT(sec >= tgSection_COUNT && sec - tgSection_COUNT < m->named_sections.count);
	return &m->named_sections.data[sec - tgSection_COUNT];
}

TG_INTERNAL tgBytes *tg_section_bytes(tgModule *m, tgSection sec) {
	return sec < tgSection_COUNT ? &m->sections[sec] : &tg_named_section(m, sec)->bytes;
}

TG_INTERNAL i64 *tg_section_align_ref(tgModule *m, tgSection sec) {
	return sec < tgSection_COUNT ? &m->section_align[sec] : &tg_named_section(m, sec)->align;
}

// Whether the section holds no bytes, like .bss and .tbss.
TG_INTERNAL bool tg_section_is_nobits(tgModule *m, tgSection sec) {
	if (sec < tgSection_COUNT) return sec == tgSection_Bss || sec == tgSection_TBss;
	return (tg_named_section(m, sec)->flags & TYG_SECF_ZERO) != 0;
}

TG_INTERNAL i64 *tg_section_nobits_ref(tgModule *m, tgSection sec) {
	return sec < tgSection_COUNT ? &m->nobits_size[sec] : &tg_named_section(m, sec)->nobits_size;
}

TG_INTERNAL bool tg_section_is_tls(tgModule *m, tgSection sec) {
	if (sec < tgSection_COUNT) return sec == tgSection_TData || sec == tgSection_TBss;
	return (tg_named_section(m, sec)->flags & TYG_SECF_TLS) != 0;
}

// The index of the named section, made with `flags` if it is new. -1 with the error set
// when it exists with other flags, or there are too many.
TG_INTERNAL isize tg_named_section_get(tgModule *m, const char *section, unsigned flags, char const *what) {
	tgString name = tg_str(section);
	isize found = -1;
	for (isize i = 0; i < m->named_sections.count; i++) {
		if (tg_str_eq(m->named_sections.data[i].name, name)) {
			found = i;
			break;
		}
	}
	if (found >= 0 && m->named_sections.data[found].flags != flags) {
		tg_set_error(m, "%s: section %s was made with other flags", what, section);
		return -1;
	}
	if (found < 0) {
		if (tgSection_COUNT + m->named_sections.count > 255) {
			tg_set_error(m, "%s: too many sections", what);
			return -1;
		}
		tgNamedSection ns;
		memset(&ns, 0, sizeof(ns));
		ns.name = tg_module_string(m, name);
		ns.flags = flags;
		ns.align = 1;
		found = m->named_sections.count;
		tg_array_add(&m->named_sections, ns);
	}
	return found;
}

// Reserves zeroed space in a section, aligned, and raises the section's alignment.
// .bss and .tbss hold no bytes, only their size grows. Returns the offset.
TG_INTERNAL i64 tg_section_reserve(tgModule *m, tgSection sec, i64 size, i64 align) {
	i64 *sec_align = tg_section_align_ref(m, sec);
	*sec_align = tg_max(*sec_align, align);
	i64 at = 0;
	if (tg_section_is_nobits(m, sec)) {
		i64 *nobits = tg_section_nobits_ref(m, sec);
		at = tg_align_up(*nobits, align);
		*nobits = at + size;
	} else {
		tgBytes *data = tg_section_bytes(m, sec);
		while (data->count % align != 0) tg_array_add(data, (u8)0);
		at = data->count;
		tg_array_resize(data, at + size);
	}
	return at;
}

// Appends bytes to a section with contents, after zeros up to `align`. Returns their offset.
// Unlike tg_section_reserve, it leaves the section's alignment alone.
TG_INTERNAL i64 tg_section_append(tgModule *m, tgSection section, void const *data, isize size, i64 align) {
	tgBytes *sec = tg_section_bytes(m, section);
	while (sec->count % align != 0) {
		tg_array_add(sec, (u8)0);
	}
	i64 offset = sec->count;
	if (data != NULL) {
		tg_array_add_elems(sec, (u8 const *)data, size);
	} else {
		tg_array_resize(sec, offset + size);
	}
	return offset;
}

// Overwrites bytes that are already in a section.
TG_INTERNAL void tg_section_write(tgModule *m, tgSection section, i64 offset, void const *data, isize size) {
	memmove(tg_section_bytes(m, section)->data + offset, data, (usize)size);
}

// A symbol for data at `offset` in `section`, named after the offset: `prefix` and the offset.
TG_INTERNAL i32 tg_offset_symbol(tgModule *m, char const *prefix, tgSection section, i64 offset, i64 size, u8 flags) {
	char name[96];
	snprintf(name, sizeof(name), "%s%lld", prefix, (long long)offset);
	i32 sym = tg_symbol(m, tg_str(name));
	tg_sym_define(m, sym, section, offset, size);
	tg_sym_set_flags(m, sym, flags);
	return sym;
}

// Read-only data.
TG_INTERNAL i32 tg_rodata(tgModule *m, void const *data, isize size, i64 align) {
	i64 offset = tg_section_append(m, tgSection_Rodata, data, size, align);
	return tg_offset_symbol(m, ".Ltg.ro.", tgSection_Rodata, offset, size, 0);
}

// Defines `sym` as `size` bytes in `section`, aligned. `bytes` is NULL for zeros, and must
// be for .bss and .tbss. Returns the offset in the section.
TG_INTERNAL i64 tg_define_data(tgModule *m, i32 sym, tgSection section, void const *bytes, i64 size, i64 align) {
	TG_ASSERT(tg_is_pow2(align));
	TG_ASSERT(bytes == NULL || !tg_section_is_nobits(m, section));
	i64 at = tg_section_reserve(m, section, size, align);
	if (bytes != NULL && size > 0) tg_section_write(m, section, at, bytes, (isize)size);
	tg_sym_define(m, sym, section, at, size);
	if (tg_section_is_tls(m, section)) {
		tg_sym_add_flags(m, sym, tgSymbolFlag_TLS);
	}
	return at;
}

// A relocation in a data section: the address of `target` + addend, `size` (8 or 4) bytes
// at `offset` of the section.
TG_INTERNAL void tg_section_reloc(tgModule *m, tgSection section, i64 offset, i32 size, i32 target, i64 addend) {
	TG_ASSERT(size == 8 || size == 4);
	tgReloc r;
	memset(&r, 0, sizeof(r));
	r.section = section;
	r.kind = size == 8 ? tgReloc_Abs64 : tgReloc_Abs32;
	r.offset = offset;
	r.sym = target;
	r.addend = addend;
	tg_array_add(&m->relocs, r);
}

// A relocation in the data of `sym`, at `offset` from its start.
TG_INTERNAL void tg_data_reloc(tgModule *m, i32 sym, i64 offset, i32 size, i32 target, i64 addend) {
	tgSymbol const *s = &m->symbols.data[sym];
	TG_ASSERT(s->section != tgSection_Undef && s->section != tgSection_Text);
	TG_ASSERT(!tg_section_is_nobits(m, s->section));
	TG_ASSERT(0 <= offset && offset + size <= s->size);
	tg_section_reloc(m, s->section, s->offset + offset, size, target, addend);
}

////////////////////////////////////////////////////////////////
// Debug files
////////////////////////////////////////////////////////////////

// The id of a source file for the debug info, by its path. Ids start at 1, in the order of
// the files' first use: DWARF 4 line tables number their file entries from 1, and 0 means
// no file in tgLineEntry, tgProcDebug and the other debug records.
TG_INTERNAL i32 tg_add_file(tgModule *m, tgString path) {
	i32 id = 0;
	u64 h = tg_strmap_hash(path);
	i64 *found = tg_strmap_get_hashed(&m->file_ids, path, h);
	if (found != NULL) {
		id = (i32)*found;
	} else {
		id = (i32)m->files.count + 1;
		tg_array_add(&m->files, tg_module_string(m, path));
		tg_strmap_set_hashed(&m->file_ids, path, h, id);
	}
	return id;
}

////////////////////////////////////////////////////////////////
// Lowered procedures
////////////////////////////////////////////////////////////////

// Appends one lowered procedure to its section, .text or a named one, after the symbols it
// asked for.
TG_INTERNAL void tg_append_lowered(tgModule *m, tgLowerOut *out) {
	i32 *syms = out->pending.count > 0 ? tg_alloc_items(i32, out->pending.count) : NULL;
	// the helpers go right before the first procedure that calls them, memmove's first
	for (i8 helper = 1; helper <= 2; helper++) {
		for (isize i = 0; i < out->pending.count; i++) {
			if (out->pending.data[i].helper == helper) syms[i] = x64_helper(m, helper == 2);
		}
	}
	for (isize i = 0; i < out->pending.count; i++) {
		tgPendingSym const *ps = &out->pending.data[i];
		if (ps->import_of != 0) {
			tgString target = m->symbols.data[ps->import_of - 1].name;
			isize len = 6 + target.len;
			char *name = (char *)tg_arena_alloc(&m->arena, len + 1, 1);
			memcpy(name, "__imp_", 6);
			memcpy(name + 6, target.text, (usize)target.len);
			syms[i] = tg_symbol(m, tg_str_make(name, len));
		} else if (ps->helper == 0) {
			syms[i] = tg_symbol(m, ps->name);
			m->symbols.data[syms[i]].flags |= (u16)ps->flags;
		}
	}

	tgSection section = out->section != tgSection_Undef ? out->section : tgSection_Text;
	tgBytes *text = tg_section_bytes(m, section);
	if (tg_is_arm64()) {
		u32 brk = 0xD4200020; // brk #1
		if (section != tgSection_Text) {
			i64 *align = tg_section_align_ref(m, section);
			*align = tg_max(*align, out->align);
		}
		while (text->count % out->align != 0) tg_array_add_elems(text, (u8 *)&brk, 4);
	} else {
		i64 *align = tg_section_align_ref(m, section);
		*align = tg_max(*align, out->align);
		while (text->count % out->align != 0) tg_array_add(text, (u8)0xCC);
	}
	i64 base = text->count;
	tg_array_add_elems(text, out->text.data, out->text.count);
	tgReloc *relocs = tg_array_push_zero(&m->relocs, out->relocs.count);
	for (isize i = 0; i < out->relocs.count; i++) {
		tgReloc r = out->relocs.data[i];
		r.offset += base;
		r.section = section;
		if (r.sym >= TG_PENDING_SYM) r.sym = syms[r.sym - TG_PENDING_SYM];
		relocs[i] = r;
	}
	// the arrays of dbg are in the module's arena; the names are still in the procedure's
	tgProcDebug dbg = out->dbg;
	for (isize i = 0; i < dbg.vars.count; i++) dbg.vars.data[i].name = tg_module_string(m, dbg.vars.data[i].name);
	for (isize i = 0; i < dbg.inline_sites.count; i++) dbg.inline_sites.data[i].name = tg_module_string(m, dbg.inline_sites.data[i].name);
	dbg.name = tg_module_string(m, dbg.name);
	dbg.link_name = tg_module_string(m, dbg.link_name);
	dbg.section = section;
	dbg.start += base;
	dbg.end += base;
	dbg.line_entry_start += (i32)m->lines.count;
	tg_array_add_elems(&m->lines, out->lines.data, out->lines.count);
	tgSymbol *s = &m->symbols.data[dbg.sym];
	s->section = section;
	s->offset = dbg.start;
	s->size = dbg.end - dbg.start;
	s->flags |= tgSymbolFlag_Func;
	tg_array_add(&m->proc_debug, dbg);
}
