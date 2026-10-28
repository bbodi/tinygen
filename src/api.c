// The public module calls: targets, modules, symbols, data, debug files and globals,
// and writing the object.

tyg_target tyg_host_target(void) {
	tyg_target t;
	memset(&t, 0, sizeof(t));
#if defined(__aarch64__) || defined(_M_ARM64)
	t.arch = TYG_ARCH_ARM64;
#else
	t.arch = TYG_ARCH_X64;
#endif
#if defined(_WIN32)
	t.os = TYG_OS_WINDOWS;
#elif defined(__APPLE__)
	t.os = TYG_OS_DARWIN;
#else
	t.os = TYG_OS_LINUX;
#endif
	t.pic = 1;
#if defined(__MINGW32__)
	t.debug_info = TYG_DEBUG_DWARF; // like gcc there, for gdb
#else
	t.debug_info = TYG_DEBUG_DEFAULT;
#endif
	return t;
}

tyg_module *tyg_module_new(tyg_target target, const char *source_name) {
	bool supported = ((target.os == TYG_OS_LINUX || target.os == TYG_OS_DARWIN) &&
	                  (target.arch == TYG_ARCH_X64 || target.arch == TYG_ARCH_ARM64)) ||
	                 (target.os == TYG_OS_WINDOWS && target.arch == TYG_ARCH_X64);
	if (!supported) {
		// Windows on arm64 has no ABI or object writer yet
		return NULL;
	}
	if (target.output != TYG_OUTPUT_OBJECT && target.output != TYG_OUTPUT_C) return NULL;
	tgModule *m = (tgModule *)calloc(1, sizeof(tgModule));
	if (m == NULL) return NULL;
	m->target = target;
	tg_cur_target = target;
	m->source_name = tg_module_string(m, tg_str(source_name ? source_name : ""));
	m->comp_dir = tg_module_string(m, tg_str(""));
	for (isize i = 0; i < tgSection_COUNT; i++) m->section_align[i] = 1;
	return m;
}

TG_INTERNAL void tg_proc_debug_free(tgProcDebug *d) {
	tg_array_free(&d->vars);
	tg_array_free(&d->scope_parent);
	tg_array_free(&d->inline_sites);
	tg_array_free(&d->scope_marks);
	tg_array_free(&d->saved_regs);
}

void tyg_module_free(tyg_module *m) {
	if (m == NULL) return;
	tg_array_free(&m->symbols);
	tg_strmap_free(&m->symbol_map);
	for (isize i = 0; i < tgSection_COUNT; i++) tg_array_free(&m->sections[i]);
	for (isize i = 0; i < m->named_sections.count; i++) tg_array_free(&m->named_sections.data[i].bytes);
	tg_array_free(&m->named_sections);
	tg_array_free(&m->relocs);
	for (isize i = 0; i < m->proc_debug.count; i++) tg_proc_debug_free(&m->proc_debug.data[i]);
	tg_array_free(&m->proc_debug);
	tg_array_free(&m->global_debug);
	tg_array_free(&m->lines);
	tg_array_free(&m->files);
	tg_strmap_free(&m->file_ids);
	for (isize i = 0; i < m->dtypes.count; i++) {
		tg_array_free(&m->dtypes.data[i]->members);
		tg_array_free(&m->dtypes.data[i]->enumerators);
	}
	tg_array_free(&m->dtypes);
	tg_array_free(&m->c_sigs);
	tg_array_free(&m->c_names);
	tg_map_free(&m->c_type_ids);
	tg_array_free(&m->c_types);
	tg_array_free(&m->c_procs);
	tg_array_free(&m->output);
	if (tg_arena_cur == &m->proc_arena) tg_arena_cur = NULL;
	tg_arena_free(&m->proc_arena);
	tg_arena_free(&m->arena);
	free(m);
}

void tyg_module_language(tyg_module *m, unsigned dwarf_language) {
	if (dwarf_language == 0 || dwarf_language > 0xffff) {
		tg_set_error(m, "tyg_module_language: bad DWARF language %u", dwarf_language);
		return;
	}
	m->language = (u16)dwarf_language;
}

const char *tyg_error(tyg_module *m) {
	return m && m->has_error ? m->error : NULL;
}

TG_INTERNAL bool tg_resolve_aliases(tgModule *m);

int tyg_module_emit(tyg_module *m, const uint8_t **bytes, size_t *size) {
	if (m->proc != NULL) {
		tg_set_error(m, "tyg_module_emit: procedure %s is still being built", m->proc->name.text);
		return 1;
	}
	tg_cur_target = m->target;
	tg_array_clear(&m->output);
	if (!tg_resolve_aliases(m)) return 1;
	bool ok = m->target.output == TYG_OUTPUT_C ? tg_write_c(m, &m->output)     :
	          m->target.os == TYG_OS_DARWIN  ? tg_write_macho(m, &m->output) :
	          m->target.os == TYG_OS_WINDOWS ? tg_write_coff(m, &m->output)  :
	                                           tg_write_elf(m, &m->output);
	if (!ok) {
		if (!m->has_error) tg_set_error(m, "writing the object failed");
		return 1;
	}
	*bytes = m->output.data;
	*size = (size_t)m->output.count;
	return 0;
}

int tyg_module_write(tyg_module *m, const char *path) {
	const uint8_t *bytes = NULL;
	size_t size = 0;
	m->object_path = tg_module_string(m, tg_str(path)); // CodeView's S_OBJNAME names the object
	int emitted = tyg_module_emit(m, &bytes, &size);
	m->object_path = tg_str_lit("");
	if (emitted != 0) return 1;
	FILE *f = fopen(path, "wb");
	if (f == NULL) {
		tg_set_error(m, "cannot open %s for writing", path);
		return 1;
	}
	size_t written = size ? fwrite(bytes, 1, size, f) : 0;
	int closed = fclose(f);
	if (written != size || closed != 0) {
		tg_set_error(m, "cannot write %s", path);
		return 1;
	}
	return 0;
}

////////////////////////////////////////////////////////////////
// Symbols and data
////////////////////////////////////////////////////////////////

TG_INTERNAL u16 tg_sym_flags_from_public(unsigned flags) {
	u16 f = 0;
	if (flags & TYG_SYM_FUNC)    f |= tgSymbolFlag_Func;
	if (flags & TYG_SYM_GLOBAL)  f |= tgSymbolFlag_Global;
	if (flags & TYG_SYM_WEAK)    f |= tgSymbolFlag_Weak;
	if (flags & TYG_SYM_HIDDEN)  f |= tgSymbolFlag_Hidden;
	if (flags & TYG_SYM_FOREIGN) f |= tgSymbolFlag_Foreign;
	if (flags & TYG_SYM_TLS)     f |= tgSymbolFlag_TLS;
	if (flags & TYG_SYM_EXPORT)  f |= tgSymbolFlag_Export;
	if (flags & TYG_SYM_PROTECTED) f |= tgSymbolFlag_Protected;
	return f;
}

tyg_sym tyg_symbol(tyg_module *m, const char *name, unsigned flags) {
	if (name == NULL || name[0] == 0) {
		tg_set_error(m, "tyg_symbol: empty name");
		return TYG_NO_SYM;
	}
	isize count = m->symbols.count;
	i32 sym = tg_symbol(m, tg_str(name));
	if (m->symbols.count != count) {
		// tg_symbol makes a new symbol global; the caller's flags decide here
		m->symbols.data[sym].flags = tg_sym_flags_from_public(flags);
	} else {
		m->symbols.data[sym].flags |= tg_sym_flags_from_public(flags);
	}
	return sym;
}

void tyg_symbol_add_flags(tyg_module *m, tyg_sym sym, unsigned flags) {
	if (sym < 0 || sym >= m->symbols.count) {
		tg_set_error(m, "tyg_symbol_add_flags: bad symbol %d", sym);
		return;
	}
	m->symbols.data[sym].flags |= tg_sym_flags_from_public(flags);
}

int tyg_data(tyg_module *m, tyg_sym sym, tyg_section section, const void *bytes, int64_t size, int64_t align) {
	if (sym < 0 || sym >= m->symbols.count) {
		tg_set_error(m, "tyg_data: bad symbol %d", sym);
		return 1;
	}
	if (m->symbols.data[sym].section != tgSection_Undef) {
		tg_set_error(m, "tyg_data: %s is defined already", m->symbols.data[sym].name.text);
		return 1;
	}
	if (size < 0 || align < 1 || !tg_is_pow2(align)) {
		tg_set_error(m, "tyg_data: bad size %lld or alignment %lld", (long long)size, (long long)align);
		return 1;
	}
	tgSection sec = tgSection_Undef;
	switch (section) {
	case TYG_SECTION_RODATA: sec = tgSection_Rodata; break;
	case TYG_SECTION_DATA:   sec = tgSection_Data;   break;
	case TYG_SECTION_BSS:    sec = tgSection_Bss;    break;
	case TYG_SECTION_TDATA:  sec = tgSection_TData;  break;
	case TYG_SECTION_TBSS:   sec = tgSection_TBss;   break;
	default:
		tg_set_error(m, "tyg_data: bad section %d", (int)section);
		return 1;
	}
	bool nobits = sec == tgSection_Bss || sec == tgSection_TBss;
	if (!nobits && bytes == NULL && size > 0) {
		tg_set_error(m, "tyg_data: %s needs bytes", m->symbols.data[sym].name.text);
		return 1;
	}
	tg_define_data(m, sym, sec, nobits ? NULL : bytes, size, align);
	return 0;
}

int tyg_data_reloc(tyg_module *m, tyg_sym sym, int64_t offset, int size, tyg_sym target, int64_t addend) {
	if (sym < 0 || sym >= m->symbols.count || target < 0 || target >= m->symbols.count) {
		tg_set_error(m, "tyg_data_reloc: bad symbol");
		return 1;
	}
	tgSymbol const *s = &m->symbols.data[sym];
	if (s->section == tgSection_Undef || tg_section_is_nobits(m, s->section)) {
		tg_set_error(m, "tyg_data_reloc: %s has no initialized data", s->name.text);
		return 1;
	}
	if ((size != 8 && size != 4) || offset < 0 || offset + size > s->size) {
		tg_set_error(m, "tyg_data_reloc: bad size %d or offset %lld in %s", size, (long long)offset, s->name.text);
		return 1;
	}
	if (size == 4 && m->target.os == TYG_OS_DARWIN) {
		// ld64 takes no 32-bit absolute address in a 64-bit image
		tg_set_error(m, "tyg_data_reloc: a 4 byte address is not supported on macOS");
		return 1;
	}
	if (size == 4 && m->target.output == TYG_OUTPUT_C) {
		tg_set_error(m, "tyg_data_reloc: C output: a 4 byte address is not supported");
		return 1;
	}
	tg_data_reloc(m, sym, offset, size, target, addend);
	return 0;
}

// A symbol tyg_data_named, tyg_alias or tyg_common may define. Only the ELF writer
// supports them so far; for C output the C compiler decides.
TG_INTERNAL bool tg_check_definable(tyg_module *m, tyg_sym sym, char const *what) {
	if (m->target.os != TYG_OS_LINUX && m->target.output != TYG_OUTPUT_C) {
		tg_set_error(m, "%s: not supported on this target yet", what);
		return false;
	}
	if (sym < 0 || sym >= m->symbols.count) {
		tg_set_error(m, "%s: bad symbol %d", what, sym);
		return false;
	}
	tgSymbol const *s = &m->symbols.data[sym];
	if (s->section != tgSection_Undef || s->alias_of != 0 || s->common_align != 0) {
		tg_set_error(m, "%s: %s is defined already", what, s->name.text);
		return false;
	}
	return true;
}

int tyg_data_named(tyg_module *m, tyg_sym sym, const char *section, unsigned flags,
                   const void *bytes, int64_t size, int64_t align) {
	if (!tg_check_definable(m, sym, "tyg_data_named")) return 1;
	if (section == NULL || section[0] == 0) {
		tg_set_error(m, "tyg_data_named: no section name");
		return 1;
	}
	if ((flags & ~(unsigned)(TYG_SECF_WRITE | TYG_SECF_TLS | TYG_SECF_ZERO | TYG_SECF_EXEC)) ||
	    ((flags & TYG_SECF_EXEC) && (flags & (TYG_SECF_TLS | TYG_SECF_ZERO)))) {
		tg_set_error(m, "tyg_data_named: bad flags 0x%x", flags);
		return 1;
	}
	if (size < 0 || align < 1 || !tg_is_pow2(align)) {
		tg_set_error(m, "tyg_data_named: bad size %lld or alignment %lld", (long long)size, (long long)align);
		return 1;
	}
	bool nobits = (flags & TYG_SECF_ZERO) != 0;
	if (!nobits && bytes == NULL && size > 0) {
		tg_set_error(m, "tyg_data_named: %s needs bytes", m->symbols.data[sym].name.text);
		return 1;
	}
	isize found = tg_named_section_get(m, section, flags, "tyg_data_named");
	if (found < 0) return 1;
	tg_define_data(m, sym, (tgSection)(tgSection_COUNT + found), nobits ? NULL : bytes, size, align);
	return 0;
}

int tyg_alias(tyg_module *m, tyg_sym alias, tyg_sym target) {
	if (!tg_check_definable(m, alias, "tyg_alias")) return 1;
	if (target < 0 || target >= m->symbols.count || target == alias) {
		tg_set_error(m, "tyg_alias: bad target %d", target);
		return 1;
	}
	m->symbols.data[alias].alias_of = target + 1;
	return 0;
}

int tyg_common(tyg_module *m, tyg_sym sym, int64_t size, int64_t align) {
	if (!tg_check_definable(m, sym, "tyg_common")) return 1;
	if (size < 0 || align < 1 || !tg_is_pow2(align)) {
		tg_set_error(m, "tyg_common: bad size %lld or alignment %lld", (long long)size, (long long)align);
		return 1;
	}
	if (!(m->symbols.data[sym].flags & tgSymbolFlag_Global)) {
		tg_set_error(m, "tyg_common: %s is not global", m->symbols.data[sym].name.text);
		return 1;
	}
	m->symbols.data[sym].size = size;
	m->symbols.data[sym].common_align = align;
	return 0;
}

// Gives every alias its target's place, before the object is written.
TG_INTERNAL bool tg_resolve_aliases(tgModule *m) {
	for (isize i = 0; i < m->symbols.count; i++) {
		tgSymbol *s = &m->symbols.data[i];
		if (s->alias_of == 0) continue;
		tgSymbol const *t = tg_alias_target(m, (i32)i);
		if (t == NULL || t->section == tgSection_Undef) {
			tg_set_error(m, "the alias %s has no defined target", s->name.text);
			return false;
		}
		s->section = t->section;
		s->offset = t->offset;
		s->size = t->size;
		s->flags |= (u16)(t->flags & (tgSymbolFlag_Func | tgSymbolFlag_TLS));
	}
	return true;
}

////////////////////////////////////////////////////////////////
// Debug files and globals
////////////////////////////////////////////////////////////////

tyg_file tyg_debug_file(tyg_module *m, const char *path) {
	if (path == NULL || path[0] == 0) return 0;
	return tg_add_file(m, tg_str(path));
}

void tyg_debug_global(tyg_module *m, const char *name, const tyg_dtype *t, tyg_sym sym, tyg_file file, int line) {
	if (sym < 0 || sym >= m->symbols.count || t == NULL) {
		tg_set_error(m, "tyg_debug_global: bad symbol or type");
		return;
	}
	tgGlobalDebug g;
	memset(&g, 0, sizeof(g));
	g.name = tg_module_string(m, tg_str(name ? name : m->symbols.data[sym].name.text));
	g.type = t;
	g.sym = sym;
	g.file_id = file;
	g.line = line;
	tg_array_add(&m->global_debug, g);
}
