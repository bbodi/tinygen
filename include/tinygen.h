/*
 * tinygen: a tiny, fast code generator for debug builds.
 * Copyright (c) 2026 BB. zlib license, see LICENSE.
 *
 * A front end builds procedures in a small IR through this API. tinygen lowers
 * them to x86-64 or arm64 machine code and writes an ELF, COFF or Mach-O
 * object with DWARF or CodeView debug info. The object follows the platform's
 * C ABI, so it links with objects from any C compiler.
 *
 * tinygen does not optimize. It exists to make debug builds fast.
 *
 * Model
 * -----
 * Scalars (integers, floats, pointers) live in virtual registers, `tyg_val`.
 * Everything else lives in memory and is reached through a `tyg_addr`.
 * Pointers are TYG_I64 values.
 *
 * Every handle is owned by its module and lives until tyg_module_free.
 * A module is not thread safe: build it from one thread.
 */
#ifndef TINYGEN_H
#define TINYGEN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TYG_VERSION_MAJOR 0
#define TYG_VERSION_MINOR 1

/* -------------------------------------------------------------------------
 * Targets and modules
 * ------------------------------------------------------------------------- */

typedef enum tyg_arch { TYG_ARCH_X64, TYG_ARCH_ARM64 } tyg_arch;
typedef enum tyg_os   { TYG_OS_LINUX, TYG_OS_DARWIN, TYG_OS_WINDOWS } tyg_os;
typedef enum tyg_output { TYG_OUTPUT_OBJECT = 0, TYG_OUTPUT_C } tyg_output;

typedef struct tyg_target {
	tyg_arch arch;
	tyg_os   os;
	int      pic;        /* position independent code: data of foreign symbols through the GOT */
	int      debug_info; /* a tyg_debug_format: none, DWARF, or CodeView on Windows */
	/* a tyg_output. With TYG_OUTPUT_C, tyg_module_write writes a C99 source file for the
	 * target's arch and os, to be compiled by any C compiler for that platform, and
	 * tyg_module_emit returns the text. */
	int      output;
} tyg_target;

/* tyg_target.debug_info. DEFAULT is the platform's format: DWARF, CodeView on Windows for
 * the Microsoft debuggers. DWARF writes DWARF on Windows too, in the COFF object, for gdb,
 * lldb and the MinGW toolchain. README.md lists what each debugger shows of each format. */
typedef enum tyg_debug_format {
	TYG_DEBUG_NONE    = 0,
	TYG_DEBUG_DEFAULT = 1,
	TYG_DEBUG_DWARF   = 2,
} tyg_debug_format;

/* The target this library was compiled for. */
tyg_target tyg_host_target(void);

typedef struct tyg_module tyg_module;

/* NULL when the target is not supported: x86-64 and arm64 Linux and macOS, and x86-64
 * Windows are. macOS gets Mach-O objects, Linux ELF, Windows COFF. */
tyg_module *tyg_module_new(tyg_target target, const char *source_name);
void        tyg_module_free(tyg_module *m);

/* The source language for the debug info: a DWARF DW_LANG_* code. The default
 * is TYG_LANG_C99. A language without a code of its own may use the code of the
 * language whose rules for names and values it is closest to. */
enum {
	TYG_LANG_C89        = 0x01,
	TYG_LANG_C          = 0x02,
	TYG_LANG_C_PLUS_PLUS = 0x04,
	TYG_LANG_C99        = 0x0c,
	TYG_LANG_RUST       = 0x1c,
	TYG_LANG_C11        = 0x1d,
};
void tyg_module_language(tyg_module *m, unsigned dwarf_language);

/* Writes the object file, or the C source with TYG_OUTPUT_C. Returns 0 on success.
 * The C output does not have procedures yet: tyg_proc_begin fails. Named sections,
 * aliases, common, weak and the visibilities become attributes of the C compiler,
 * which decides whether the platform has them. */
int tyg_module_write(tyg_module *m, const char *path);
/* The object file (or C source) in memory, valid until tyg_module_free. */
int tyg_module_emit(tyg_module *m, const uint8_t **bytes, size_t *size);

/* The last error, or NULL. Every failing call sets it. */
const char *tyg_error(tyg_module *m);

/* -------------------------------------------------------------------------
 * Symbols and data
 * ------------------------------------------------------------------------- */

typedef int32_t tyg_sym;
#define TYG_NO_SYM (-1)

enum {
	TYG_SYM_FUNC    = 1 << 0,
	TYG_SYM_GLOBAL  = 1 << 1, /* visible to other objects */
	TYG_SYM_WEAK    = 1 << 2,
	TYG_SYM_HIDDEN  = 1 << 3, /* global within the linked image only */
	TYG_SYM_FOREIGN = 1 << 4, /* may be defined in a shared library */
	TYG_SYM_TLS     = 1 << 5,
	TYG_SYM_EXPORT  = 1 << 6, /* exported from a shared library or DLL */
	/* ELF: visible to other objects, but references from within the linked image bind to
	 * this definition. Mach-O has no such visibility and treats it as default, like
	 * clang; COFF ignores it. TYG_SYM_HIDDEN wins over it. */
	TYG_SYM_PROTECTED = 1 << 7,
};

typedef enum tyg_section {
	TYG_SECTION_RODATA,
	TYG_SECTION_DATA,
	TYG_SECTION_BSS,
	TYG_SECTION_TDATA,
	TYG_SECTION_TBSS,
} tyg_section;

/* Declares a symbol, or returns the existing one with the same name. Names are the C
 * names: on macOS the Mach-O writer adds the leading underscore, so "printf" refers to
 * the symbol `_printf`. */
tyg_sym tyg_symbol(tyg_module *m, const char *name, unsigned flags);
void    tyg_symbol_add_flags(tyg_module *m, tyg_sym sym, unsigned flags);

/* Defines a data symbol. `bytes` is NULL for the zero-filled sections. */
int tyg_data(tyg_module *m, tyg_sym sym, tyg_section section,
             const void *bytes, int64_t size, int64_t align);
/* Stores the address of `target` + addend at `offset` in the data of `sym`. `size` is 8,
 * or 4 except on macOS. */
int tyg_data_reloc(tyg_module *m, tyg_sym sym, int64_t offset, int size,
                   tyg_sym target, int64_t addend);

/* Named sections, aliases and common symbols work on Linux (ELF) and with C output
 * only so far; on the other targets these calls fail.
 *
 * Flags of a named section. It is always allocated (loaded). */
enum {
	TYG_SECF_WRITE = 1 << 0, /* writable */
	TYG_SECF_TLS   = 1 << 1, /* thread local */
	TYG_SECF_ZERO  = 1 << 2, /* zero filled, takes no room in the object */
	TYG_SECF_EXEC  = 1 << 3, /* executable: code, see tyg_proc_section; not with TLS or ZERO */
};

/* Defines a data symbol in the section called `section` (for example
 * ".init_array" or a name from the source), made on first use with `flags`
 * (TYG_SECF_*). Every definition in one section gives the same flags. On ELF,
 * the .init_array, .fini_array and .preinit_array names, and their ".N"
 * priority forms, get those section types. */
int tyg_data_named(tyg_module *m, tyg_sym sym, const char *section, unsigned flags,
                   const void *bytes, int64_t size, int64_t align);

/* Makes `alias` another name for `target`, which must be defined in this
 * module by the time the object is written. */
int tyg_alias(tyg_module *m, tyg_sym alias, tyg_sym target);

/* Makes `sym`, a global data symbol, a common symbol: zero filled data the
 * linker merges with the other objects' common symbols and definitions. */
int tyg_common(tyg_module *m, tyg_sym sym, int64_t size, int64_t align);

/* -------------------------------------------------------------------------
 * Types for the ABI
 *
 * Calls need the C type of each parameter, because the ABI classifies
 * aggregates by their fields. A record lists its fields at their offsets;
 * fields may overlap, which describes a union. A bitfield is described by
 * its storage unit.
 * ------------------------------------------------------------------------- */

typedef struct tyg_type tyg_type;

typedef enum tyg_scalar {
	TYG_VOID,
	TYG_I8,
	TYG_I16,
	TYG_I32,
	TYG_I64,
	TYG_F32,
	TYG_F64,
	TYG_F80, /* C's long double: only for tyg_type_scalar, see "long double" below */
} tyg_scalar;

typedef struct tyg_field {
	const tyg_type *type;
	int64_t         offset;
} tyg_field;

const tyg_type *tyg_type_scalar(tyg_module *m, tyg_scalar s);
/* A signed or unsigned integer; matters for extension of small ints in calls. */
const tyg_type *tyg_type_int(tyg_module *m, tyg_scalar s, int is_signed);
const tyg_type *tyg_type_ptr(tyg_module *m);
const tyg_type *tyg_type_array(tyg_module *m, const tyg_type *elem, int64_t count);
const tyg_type *tyg_type_record(tyg_module *m, int64_t size, int64_t align,
                                const tyg_field *fields, int count);

int64_t tyg_type_size(const tyg_type *t);
int64_t tyg_type_align(const tyg_type *t);

/* The size and alignment of the target's va_list, for the front end's layout. */
int64_t tyg_va_list_size(tyg_module *m);
int64_t tyg_va_list_align(tyg_module *m);

typedef struct tyg_sig tyg_sig;

enum {
	TYG_SIG_VARIADIC      = 1 << 0,
	TYG_SIG_RETURNS_TWICE = 1 << 1, /* setjmp and friends */
	TYG_SIG_NORETURN      = 1 << 2,
};

tyg_sig *tyg_sig_new(tyg_module *m, const tyg_type *ret,
                     const tyg_type *const *params, int count, unsigned flags);

/* -------------------------------------------------------------------------
 * Procedures
 * ------------------------------------------------------------------------- */

typedef struct tyg_proc tyg_proc;
typedef uint32_t tyg_val;   /* 0 is no value */
typedef int32_t  tyg_block;
typedef int32_t  tyg_local;

typedef enum tyg_addr_kind {
	TYG_ADDR_VAL,   /* [val + offset], val holds a pointer */
	TYG_ADDR_LOCAL, /* [local + offset] */
	TYG_ADDR_SYM,   /* [symbol + offset] */
} tyg_addr_kind;

typedef struct tyg_addr {
	tyg_addr_kind kind;
	int32_t       base;   /* a tyg_val, tyg_local or tyg_sym */
	int64_t       offset;
	int32_t       align;  /* the known alignment when it is below the type's, else 0 */
} tyg_addr;

static inline tyg_addr tyg_at_val(tyg_val v, int64_t off)     { tyg_addr a = {TYG_ADDR_VAL,   (int32_t)v, off, 0}; return a; }
static inline tyg_addr tyg_at_local(tyg_local l, int64_t off) { tyg_addr a = {TYG_ADDR_LOCAL, l, off, 0}; return a; }
static inline tyg_addr tyg_at_sym(tyg_sym s, int64_t off)     { tyg_addr a = {TYG_ADDR_SYM,   s, off, 0}; return a; }

/* Starts the definition of the function `sym`. One procedure is built at a time. */
tyg_proc *tyg_proc_begin(tyg_module *m, tyg_sym sym, const tyg_sig *sig);
/* Lowers the procedure to machine code. Returns 0 on success. On failure the
 * procedure is discarded and tyg_error says why, for example an operation
 * this target does not support yet. */
int tyg_proc_end(tyg_proc *p);
/* Abandons the procedure being built: nothing of it reaches the object. A front
 * end that finds halfway through that it cannot compile a procedure calls this
 * and then generates the procedure some other way. */
void tyg_proc_abort(tyg_proc *p);

/* Puts the procedure's code in the section called `section` instead of .text, for
 * example a name from the source. The section is made on first use with
 * TYG_SECF_EXEC, which tyg_data_named must also give for it. ELF only, like the
 * other named sections: on the other targets it fails, and the code stays in .text.
 * Returns 0 on success. */
int tyg_proc_section(tyg_proc *p, const char *section);

tyg_local tyg_local_new(tyg_proc *p, int64_t size, int64_t align);

/* A scalar parameter's value. For an aggregate parameter, the address of the
 * memory that holds it, which the procedure may write. */
tyg_val tyg_param(tyg_proc *p, int index);

/* Blocks. The entry block exists and is current after tyg_proc_begin. */
tyg_block tyg_block_new(tyg_proc *p);
void      tyg_block_set(tyg_proc *p, tyg_block b);
tyg_block tyg_block_current(tyg_proc *p);
/* Whether the current block ends in a jump, branch, return or trap. */
int       tyg_block_terminated(tyg_proc *p);

/* Constants */
tyg_val tyg_iconst(tyg_proc *p, tyg_scalar type, int64_t value);
tyg_val tyg_fconst(tyg_proc *p, tyg_scalar type, double value);
tyg_val tyg_addr_of(tyg_proc *p, tyg_addr a);

/* Memory */
enum { TYG_MEM_VOLATILE = 1 << 0 };
tyg_val tyg_load(tyg_proc *p, tyg_scalar type, tyg_addr a, unsigned flags);
void    tyg_store(tyg_proc *p, tyg_scalar type, tyg_addr a, tyg_val v, unsigned flags);
void    tyg_copy(tyg_proc *p, tyg_addr dst, tyg_addr src, int64_t size);
void    tyg_move(tyg_proc *p, tyg_addr dst, tyg_addr src, int64_t size);  /* may overlap */
void    tyg_zero(tyg_proc *p, tyg_addr dst, int64_t size);
void    tyg_copy_dyn(tyg_proc *p, tyg_val dst, tyg_val src, tyg_val size);
void    tyg_move_dyn(tyg_proc *p, tyg_val dst, tyg_val src, tyg_val size);
void    tyg_set_dyn(tyg_proc *p, tyg_val dst, tyg_val byte, tyg_val size);
tyg_val tyg_alloca(tyg_proc *p, tyg_val size, int64_t align);

/* Arithmetic. Both operands have the same type, which is the result's. */
typedef enum tyg_binop {
	TYG_ADD, TYG_SUB, TYG_MUL,
	TYG_SDIV, TYG_UDIV, TYG_SREM, TYG_UREM,
	TYG_AND, TYG_OR, TYG_XOR,
	TYG_SHL, TYG_LSHR, TYG_ASHR, /* the count is taken modulo the bit width */
	TYG_FADD, TYG_FSUB, TYG_FMUL, TYG_FDIV,
} tyg_binop;

typedef enum tyg_unop { TYG_NEG, TYG_NOT, TYG_FNEG } tyg_unop;

typedef enum tyg_cond {
	TYG_EQ, TYG_NE,
	TYG_SLT, TYG_SLE, TYG_SGT, TYG_SGE,
	TYG_ULT, TYG_ULE, TYG_UGT, TYG_UGE,
	TYG_FEQ, TYG_FNE, /* FNE is true for unordered operands */
	TYG_FLT, TYG_FLE, TYG_FGT, TYG_FGE,
} tyg_cond;

typedef enum tyg_conv {
	TYG_ZEXT, TYG_SEXT, TYG_TRUNC,
	TYG_SITOF, TYG_UITOF, TYG_FTOSI, TYG_FTOUI,
	TYG_FEXT, TYG_FTRUNC,
	TYG_BITCAST, /* between an int and a float of the same size */
} tyg_conv;

tyg_scalar tyg_val_type(tyg_proc *p, tyg_val v);

tyg_val tyg_bin(tyg_proc *p, tyg_binop op, tyg_val a, tyg_val b);
tyg_val tyg_un(tyg_proc *p, tyg_unop op, tyg_val a);
tyg_val tyg_cmp(tyg_proc *p, tyg_cond c, tyg_val a, tyg_val b);           /* TYG_I8: 0 or 1 */
tyg_val tyg_convert(tyg_proc *p, tyg_conv op, tyg_scalar to, tyg_val a);
tyg_val tyg_select(tyg_proc *p, tyg_val cond, tyg_val a, tyg_val b);

/* Control flow */
void tyg_jump(tyg_proc *p, tyg_block target);
void tyg_branch(tyg_proc *p, tyg_val cond, tyg_block if_true, tyg_block if_false);
void tyg_ret(tyg_proc *p, tyg_val v);         /* 0 for a void procedure */
void tyg_ret_mem(tyg_proc *p, tyg_addr a);    /* an aggregate result */
void tyg_trap(tyg_proc *p);
void tyg_debug_trap(tyg_proc *p);
void tyg_unreachable(tyg_proc *p);

/* Calls. A scalar argument is a value; an aggregate argument is the address of
 * its memory. Arguments past the signature's parameters are C varargs: their
 * types are given in `vararg_types`. */
typedef struct tyg_arg {
	tyg_val  val;  /* a scalar */
	tyg_addr mem;  /* an aggregate, when val is 0 */
} tyg_arg;

typedef struct tyg_callee {
	tyg_sym sym;   /* a direct call, or TYG_NO_SYM */
	tyg_val val;   /* an indirect call through a pointer */
} tyg_callee;

static inline tyg_callee tyg_direct(tyg_sym s)   { tyg_callee c = {s, 0}; return c; }
static inline tyg_callee tyg_indirect(tyg_val v) { tyg_callee c = {TYG_NO_SYM, v}; return c; }

/* Returns the scalar result, or 0. An aggregate result is stored at `result`. */
tyg_val tyg_call(tyg_proc *p, const tyg_sig *sig, tyg_callee callee,
                 const tyg_arg *args, int count,
                 const tyg_type *const *vararg_types, const tyg_addr *result);

/* C varargs, in a variadic procedure. `ap` points to a va_list. */
void tyg_va_start(tyg_proc *p, tyg_val ap);
/* The address of the next argument of type t, valid until the next va_arg. */
tyg_val tyg_va_arg(tyg_proc *p, tyg_val ap, const tyg_type *t);
void tyg_va_copy(tyg_proc *p, tyg_val dst, tyg_val src);

/* long double
 *
 * A long double value lives in memory, like an aggregate: tyg_type_scalar(m, TYG_F80) is
 * its type for signatures, varargs and records, tyg_type_size and tyg_type_align give its
 * layout, and these calls compute with values in memory. An argument or result of the type
 * is passed like an aggregate (tyg_arg.mem, tyg_call's result, tyg_ret_mem); tyg_param and
 * tyg_va_arg give its address. Copy one with tyg_copy of tyg_type_size bytes.
 *
 * The target decides what it is:
 * - x86-64 Linux and macOS: the x87 80-bit extended type, 16 bytes aligned to 16, its
 *   value in the low 10 bytes. The calls run on the x87 unit with its default control
 *   word (64-bit precision, round to nearest).
 * - arm64 macOS: a double, 8 bytes.
 * - Windows x64: MSVC's double, 8 bytes, passed like a double (MinGW's gcc has the x87
 *   type unless it builds with -mlong-double-64).
 * - arm64 Linux (IEEE binary128): not supported yet; the type exists for layouts, but a
 *   signature, call or operation that uses it fails.
 *
 * There is no constant call: the front end stores a constant's bytes in the target's
 * format, with tyg_store or as data. `dst` may be the same memory as an operand. */
void    tyg_ld_bin(tyg_proc *p, tyg_binop op, tyg_addr dst, tyg_addr a, tyg_addr b); /* TYG_FADD..TYG_FDIV */
void    tyg_ld_neg(tyg_proc *p, tyg_addr dst, tyg_addr a);
tyg_val tyg_ld_cmp(tyg_proc *p, tyg_cond c, tyg_addr a, tyg_addr b);                 /* TYG_FEQ..TYG_FGE, TYG_I8 */
/* dst = a converted: TYG_SITOF or TYG_UITOF from an integer, TYG_FEXT from TYG_F32 or TYG_F64. */
void    tyg_ld_from(tyg_proc *p, tyg_conv op, tyg_addr dst, tyg_val a);
/* a converted to `to`: TYG_FTOSI or TYG_FTOUI (truncating), TYG_FTRUNC to TYG_F32 or TYG_F64. */
tyg_val tyg_ld_to(tyg_proc *p, tyg_conv op, tyg_scalar to, tyg_addr a);

/* Atomics, sequentially consistent */
typedef enum tyg_rmw { TYG_RMW_XCHG, TYG_RMW_ADD, TYG_RMW_SUB, TYG_RMW_AND, TYG_RMW_OR, TYG_RMW_XOR, TYG_RMW_NAND } tyg_rmw;
void    tyg_fence(tyg_proc *p);
tyg_val tyg_atomic_load(tyg_proc *p, tyg_scalar type, tyg_addr a);
void    tyg_atomic_store(tyg_proc *p, tyg_scalar type, tyg_addr a, tyg_val v);
tyg_val tyg_atomic_rmw(tyg_proc *p, tyg_rmw op, tyg_addr a, tyg_val v);  /* the old value */
/* The old value; *ok is 1 when the exchange happened. */
tyg_val tyg_atomic_cas(tyg_proc *p, tyg_addr a, tyg_val expected, tyg_val desired, tyg_val *ok);

/* Intrinsics */
typedef enum tyg_intrinsic {
	TYG_SQRT,     /* a */
	TYG_BSWAP,    /* a */
	TYG_POPCOUNT, /* a */
	TYG_CTZ,      /* a, defined for 0 */
	TYG_CLZ,      /* a, defined for 0 */
	TYG_FRAME_ADDRESS,
	TYG_RETURN_ADDRESS,
	TYG_STACK_POINTER,
	TYG_CPU_RELAX,
	TYG_STACK_RESTORE, /* a: a value of TYG_STACK_POINTER from earlier in the procedure;
	                      frees the tyg_alloca memory allocated since then */
} tyg_intrinsic;
tyg_val tyg_intrin(tyg_proc *p, tyg_intrinsic op, tyg_val a);

/* -------------------------------------------------------------------------
 * Debug info
 *
 * Debug types describe the source language's types to the debugger. They are
 * separate from the ABI types and may be richer.
 * ------------------------------------------------------------------------- */

typedef int32_t tyg_file;
typedef struct tyg_dtype tyg_dtype;

tyg_file tyg_debug_file(tyg_module *m, const char *path);

/* The source position of the code that follows. */
void tyg_loc(tyg_proc *p, tyg_file file, int line, int column);

typedef enum tyg_dt_encoding {
	TYG_DT_SIGNED, TYG_DT_UNSIGNED, TYG_DT_SIGNED_CHAR, TYG_DT_UNSIGNED_CHAR,
	TYG_DT_FLOAT, TYG_DT_BOOL,
} tyg_dt_encoding;

typedef enum tyg_dt_record_kind { TYG_DT_STRUCT, TYG_DT_UNION, TYG_DT_CLASS } tyg_dt_record_kind;

const tyg_dtype *tyg_dt_void(tyg_module *m);
const tyg_dtype *tyg_dt_base(tyg_module *m, const char *name, int64_t size, tyg_dt_encoding enc);
const tyg_dtype *tyg_dt_pointer(tyg_module *m, const tyg_dtype *to);
const tyg_dtype *tyg_dt_const(tyg_module *m, const tyg_dtype *t);
const tyg_dtype *tyg_dt_volatile(tyg_module *m, const tyg_dtype *t);
const tyg_dtype *tyg_dt_typedef(tyg_module *m, const char *name, const tyg_dtype *t, tyg_file file, int line);
/* count < 0: an array of unknown size */
const tyg_dtype *tyg_dt_array(tyg_module *m, const tyg_dtype *elem, int64_t count);
const tyg_dtype *tyg_dt_func(tyg_module *m, const tyg_dtype *ret, const tyg_dtype *const *params, int count, int variadic);

/* A record is created first and completed later, so it may refer to itself.
 * One that is never completed is a declaration. name may be NULL. */
tyg_dtype *tyg_dt_record(tyg_module *m, tyg_dt_record_kind kind, const char *name, tyg_file file, int line);
void tyg_dt_member(tyg_module *m, tyg_dtype *rec, const char *name, const tyg_dtype *t, int64_t offset);
void tyg_dt_bitfield(tyg_module *m, tyg_dtype *rec, const char *name, const tyg_dtype *t, int64_t bit_offset, int bit_size);
void tyg_dt_record_complete(tyg_module *m, tyg_dtype *rec, int64_t size, int64_t align);

tyg_dtype *tyg_dt_enum(tyg_module *m, const char *name, const tyg_dtype *underlying, tyg_file file, int line);
void tyg_dt_enumerator(tyg_module *m, tyg_dtype *e, const char *name, int64_t value);

/* The procedure's own debug entry: its source name, position and type. */
void tyg_debug_proc(tyg_proc *p, const char *name, tyg_file file, int line, const tyg_dtype *type);
/* Lexical scopes nest; scope 0 is the procedure's. */
void tyg_scope_begin(tyg_proc *p);
void tyg_scope_end(tyg_proc *p);
/* A variable stored at `where` (a local or a symbol) for the rest of its scope. */
void tyg_debug_var(tyg_proc *p, const char *name, const tyg_dtype *t, tyg_addr where, tyg_file file, int line);
/* A parameter, after its value is stored at `where`. For an aggregate parameter, `where` can
 * also be tyg_at_val(tyg_param(p, i), 0), including one the ABI passes by pointer to a copy.
 * In the entry block, the prologue ends for the debugger after the last of these: a
 * breakpoint on the procedure stops there, with the parameters in place. */
void tyg_debug_param(tyg_proc *p, const char *name, const tyg_dtype *t, tyg_addr where, tyg_file file, int line);
/* A variable reached through a pointer: `where`, a local, holds the variable's address,
 * for example a closure's captured variable. The debugger shows the variable itself.
 * In the entry block it counts like a parameter: the prologue ends after it. */
void tyg_debug_var_ref(tyg_proc *p, const char *name, const tyg_dtype *t, tyg_addr where, tyg_file file, int line);

void tyg_debug_global(tyg_module *m, const char *name, const tyg_dtype *t, tyg_sym sym, tyg_file file, int line);

#ifdef __cplusplus
}
#endif
#endif
