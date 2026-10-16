// The code buffer both encoders write into, with its relocations, and the pending symbols
// both lowerings name.

typedef struct tgAsm {
	tgModule *m;
	tgBytes *code;
	tgRelocArray *relocs;
} tgAsm;

TG_INTERNAL i64 tg_pos(tgAsm *a) {
	return a->code->count;
}

TG_INTERNAL void tg_b(tgAsm *a, u8 b) {
	tg_array_add(a->code, b);
}

TG_INTERNAL void tg_bytes(tgAsm *a, void const *data, isize n) {
	tg_array_add_elems(a->code, (u8 const *)data, n);
}

TG_INTERNAL void tg_u16(tgAsm *a, u16 v) { tg_bytes(a, &v, 2); }
TG_INTERNAL void tg_u32(tgAsm *a, u32 v) { tg_bytes(a, &v, 4); }
TG_INTERNAL void tg_u64(tgAsm *a, u64 v) { tg_bytes(a, &v, 8); }

TG_INTERNAL void tg_patch_u32(tgAsm *a, i64 at, u32 v) {
	memmove(a->code->data + at, &v, 4);
}

// A relocation in a data section.
TG_INTERNAL void tg_module_reloc(tgModule *m, tgReloc r) {
	tg_array_add(&m->relocs, r);
}

TG_INTERNAL void tg_add_reloc(tgModule *m, tgSection section, tgRelocKind kind, i64 offset, i32 sym, i64 addend) {
	tgReloc r;
	memset(&r, 0, sizeof(r));
	r.section = section;
	r.kind = kind;
	r.offset = offset;
	r.sym = sym;
	r.addend = addend;
	tg_module_reloc(m, r);
}

// A relocation in the code being assembled, at an offset in a->code.
TG_INTERNAL void tg_asm_reloc(tgAsm *a, tgRelocKind kind, i64 offset, i32 sym, i64 addend) {
	tgReloc r;
	memset(&r, 0, sizeof(r));
	r.section = tgSection_Text;
	r.kind = kind;
	r.offset = offset;
	r.sym = sym;
	r.addend = addend;
	tg_array_add(a->relocs, r);
}

// A pending symbol for `name`, which tg_append_lowered looks up or adds when it appends the
// code. `helper` is 0, or 1 for the memmove and 2 for the memset helper of x64_helper.
TG_INTERNAL i32 tg_lower_symbol(tgModule *m, tgLowerOut *out, tgString name, u32 flags, i8 helper) {
	(void)m;
	for (isize i = 0; i < out->pending.count; i++) {
		if (tg_str_eq(out->pending.data[i].name, name)) return TG_PENDING_SYM + (i32)i;
	}
	tgPendingSym ps;
	memset(&ps, 0, sizeof(ps));
	ps.name = name;
	ps.flags = flags;
	ps.helper = helper;
	tg_array_add(&out->pending, ps);
	return TG_PENDING_SYM + (i32)(out->pending.count-1);
}
