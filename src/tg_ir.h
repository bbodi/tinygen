// The IR, the ABI description, procedures and the module.
//
// Pipeline: the builder API (build.c) -> tyg IR per procedure -> analysis and
// register allocation -> machine code (x64_lower.c, a64_lower.c) -> the module's
// sections -> an ELF, COFF or Mach-O object with DWARF or CodeView.
#ifndef TG_IR_H
#define TG_IR_H

#include "tg_base.h"
#include "../include/tinygen.h"

// The internal names of the public tyg_module and tyg_proc.
typedef struct tyg_module tgModule;
typedef struct tyg_proc   tgProc;

////////////////////////////////////////////////////////////////
// Target
//
// The target of the module being built or written on this thread. Lowering asks
// these instead of taking the module.
////////////////////////////////////////////////////////////////

static TG_THREAD_LOCAL tyg_target tg_cur_target;

TG_INTERNAL bool tg_is_arm64(void)   { return tg_cur_target.arch == TYG_ARCH_ARM64; }
TG_INTERNAL bool tg_is_darwin(void)  { return tg_cur_target.os == TYG_OS_DARWIN; }
TG_INTERNAL bool tg_is_windows(void) { return tg_cur_target.os == TYG_OS_WINDOWS; }
TG_INTERNAL bool tg_is_win64(void)   { return tg_cur_target.os == TYG_OS_WINDOWS && tg_cur_target.arch == TYG_ARCH_X64; }
TG_INTERNAL bool tg_is_pic(void)     { return tg_cur_target.pic != 0; }
TG_INTERNAL bool tg_debug_info(void) { return tg_cur_target.debug_info != 0; }
// on Windows, CodeView unless DWARF was asked for
TG_INTERNAL bool tg_debug_codeview(void) { return tg_is_windows() && tg_debug_info() && tg_cur_target.debug_info != TYG_DEBUG_DWARF; }

////////////////////////////////////////////////////////////////
// Scalar types
////////////////////////////////////////////////////////////////

typedef u8 tgType;
enum {
	tgType_None,
	tgType_I8,
	tgType_I16,
	tgType_I32,
	tgType_I64,
	tgType_F32,
	tgType_F64,
	tgType_V128, // only used for ABI pieces: a whole vector register, xmm, ymm or zmm by the piece size
	tgType_COUNT,
};

static i32 const tg_type_sizes[tgType_COUNT] = {0, 1, 2, 4, 8, 4, 8, 16};

TG_INTERNAL i32  tg_type_size(tgType t)     { return tg_type_sizes[t]; }
TG_INTERNAL bool tg_type_is_float(tgType t) { return t == tgType_F32 || t == tgType_F64; }
TG_INTERNAL bool tg_type_is_int(tgType t)   { return t >= tgType_I8 && t <= tgType_I64; }

TG_ARRAY(tgTypeArray, tgType);

////////////////////////////////////////////////////////////////
// Memory operands
////////////////////////////////////////////////////////////////

typedef u8 tgMemKind;
enum {
	tgMem_None,
	tgMem_Reg,      // [vreg + offset], the vreg holds a pointer
	tgMem_Local,    // [frame local + offset]
	tgMem_Sym,      // [symbol + offset]
	tgMem_Incoming, // [incoming stack arguments + offset]
};

typedef struct tgMem {
	tgMemKind kind;
	u8         align;  // known alignment when it may be below the type's (packed fields), 0 if natural
	u32        base;
	i32        offset;
} tgMem;

TG_INTERNAL tgMem tg_mem(tgMemKind kind, u32 base, i32 offset) {
	tgMem m;
	memset(&m, 0, sizeof(m));
	m.kind = kind;
	m.base = base;
	m.offset = offset;
	return m;
}

TG_INTERNAL tgMem tg_mem_offset(tgMem m, i64 offset) {
	m.offset += (i32)offset;
	if (m.align != 0 && offset != 0) {
		// the largest power of two dividing both
		i64 o = offset < 0 ? -offset : offset;
		i64 a = m.align;
		while (a > 1 && (o % a) != 0) a /= 2;
		m.align = (u8)a;
	}
	return m;
}

////////////////////////////////////////////////////////////////
// Instructions
////////////////////////////////////////////////////////////////

typedef u8 tgOp;
enum {
	tgOp_Nop,        // imm = 1: a machine nop; imm = 2: the end of the prologue for the debugger, see tyg_debug_param
	tgOp_Loc,        // source line marker: imm = line, a = file id, b = column
	tgOp_Scope,      // debug scope marker: the code after it belongs to scope imm

	tgOp_IConst,     // dst = imm
	tgOp_FConst,     // dst = bits(imm)
	tgOp_Lea,        // dst = &mem
	tgOp_Load,       // dst = [mem]
	tgOp_Store,      // [mem] = a
	tgOp_Copy,       // dst = a

	tgOp_Add,
	tgOp_Sub,
	tgOp_Mul,
	tgOp_SDiv,
	tgOp_UDiv,
	tgOp_SRem,
	tgOp_URem,
	tgOp_And,
	tgOp_Or,
	tgOp_Xor,
	tgOp_Shl,        // raw x86 shifts, the count is masked by the cpu
	tgOp_LShr,
	tgOp_AShr,

	tgOp_FAdd,
	tgOp_FSub,
	tgOp_FMul,
	tgOp_FDiv,

	tgOp_Neg,
	tgOp_Not,
	tgOp_FNeg,

	tgOp_ICmp,       // dst(i8) = a <aux> b
	tgOp_FCmp,       // dst(i8) = a <aux> b

	tgOp_Zext,       // dst(type) = zext a(aux)
	tgOp_Sext,
	tgOp_Trunc,
	tgOp_SIToF,      // dst(float type) = a(aux int type)
	tgOp_UIToF,
	tgOp_FToSI,      // dst(int type) = a(aux float type)
	tgOp_FToUI,
	tgOp_FExt,
	tgOp_FTrunc,
	tgOp_Bitcast,    // int <-> float of the same size, aux = source type

	tgOp_Select,     // dst = a ? b : c

	tgOp_MemCopy,    // copy imm bytes: [a] <- [b], a and b are pointer vregs
	tgOp_MemMove,    // like MemCopy but the ranges may overlap
	tgOp_MemZero,    // zero imm bytes at mem
	tgOp_MemCopyDyn, // copy c bytes, c is a vreg
	tgOp_MemMoveDyn,
	tgOp_MemSetDyn,  // set c bytes at [a] to the byte b

	tgOp_Call,       // imm = index into proc->calls

	tgOp_Jump,       // imm = block
	tgOp_Branch,     // if a != 0 goto imm else goto imm2 (stored in c)
	tgOp_Ret,
	tgOp_Unreachable,
	tgOp_Trap,
	tgOp_DebugTrap,

	tgOp_AtomicFence,
	tgOp_AtomicLoad,  // dst = [mem]
	tgOp_AtomicStore, // [mem] = a, with a full fence
	tgOp_AtomicRmw,   // dst = old [mem]; [mem] = old <aux> a
	tgOp_AtomicCas,   // dst = old [mem]; if old == a { [mem] = b }; dst2 (c) = success

	tgOp_ReadCycleCounter, // imm = 1: its frequency, arm64 only
	tgOp_CpuRelax,
	tgOp_Prefetch,
	tgOp_StackPointer, // dst = rsp
	tgOp_FrameAddress, // dst = rbp
	tgOp_ReturnAddress,
	tgOp_Sqrt,        // dst = sqrt(a)
	tgOp_Fma,         // dst = a*b + c rounded once, only with the fma target feature
	tgOp_Bswap,
	tgOp_Popcount,
	tgOp_Ctz,         // count trailing zeros, defined for zero
	tgOp_Clz,         // count leading zeros, defined for zero
	tgOp_Syscall,     // imm = index into proc->calls (args only)
	tgOp_MulOvf,      // dst = a * b, c = overflowed; aux = 1 if signed
	tgOp_MulHiU,      // dst = high 64 bits of the unsigned product a * b
	tgOp_Cpuid,       // [mem] = eax, ebx, ecx, edx of cpuid(eax = a, ecx = b)
	tgOp_Xgetbv,      // [mem] = eax, edx of xgetbv(ecx = a)
	tgOp_Vec128,      // [mem] = x86 vector instruction tg_vec_intrinsics[aux] on [a], [b], [c]; a, b, c are pointers, imm = immediate
	tgOp_Valgrind,    // dst = valgrind client request(default = a, args = [b])
	tgOp_Alloca,      // dst = a bytes of fresh stack memory aligned to imm
	tgOp_TlsAddr,     // dst = this thread's address of the thread local symbol imm
	tgOp_FToHalf,     // dst(i16) = f16 bits of a(aux float type), rounded by the cpu; arm64 only
	tgOp_HalfToF,     // dst(f32) = the f16 bits a; arm64 only
	tgOp_Asm,         // inline asm template: imm = index into proc->asms
	tgOp_StackRestore, // rsp = a, a value of tgOp_StackPointer: frees the allocas since
	tgOp_X87,         // x86-64 long double on the x87 unit, aux = tgX87Op, see there

	tgOp_COUNT,
};

typedef u8 tgCond;
enum {
	tgCond_EQ,
	tgCond_NE,
	tgCond_SLT,
	tgCond_SLE,
	tgCond_SGT,
	tgCond_SGE,
	tgCond_ULT,
	tgCond_ULE,
	tgCond_UGT,
	tgCond_UGE,
	// floating point: ordered unless stated
	tgCond_FEQ,
	tgCond_FNE,  // unordered or not equal
	tgCond_FLT,
	tgCond_FLE,
	tgCond_FGT,
	tgCond_FGE,
};

// The forms of tgOp_X87. a, b and c are pointer vregs to 80-bit values in memory, except
// where a scalar vreg is named. `type` is the scalar's type.
typedef u8 tgX87Op;
enum {
	tgX87_Add,     // [a] = [b] + [c]
	tgX87_Sub,     // [a] = [b] - [c]
	tgX87_Mul,     // [a] = [b] * [c]
	tgX87_Div,     // [a] = [b] / [c]
	tgX87_Neg,     // [a] = -[b]
	tgX87_Cmp,     // dst(i8) = [b] <imm, a float tgCond> [c]
	tgX87_FromSI,  // [a] = the signed int c (i32 or i64)
	tgX87_FromUI,  // [a] = the unsigned i64 c
	tgX87_FromF,   // [a] = the float c (f32 or f64)
	tgX87_ToSI,    // dst = [b] as a signed int, truncated (i32 or i64)
	tgX87_ToUI,    // dst(i64) = [b] as an unsigned int, truncated
	tgX87_ToF,     // dst = [b] rounded to a float (f32 or f64)
};

typedef u8 tgRmwOp;
enum {
	tgRmw_Xchg,
	tgRmw_Add,
	tgRmw_Sub,
	tgRmw_And,
	tgRmw_Or,
	tgRmw_Xor,
	tgRmw_Nand,
};

typedef struct tgInstr {
	tgOp   op;
	tgType type;
	u8      aux;   // tgCond, source tgType, or tgRmwOp
	u8      flags;
	u32     dst;
	u32     a;
	u32     b;
	u32     c;
	tgMem  mem;
	i64     imm;
} tgInstr;

enum {
	tgInstrFlag_Volatile = 1<<0,
};

TG_ARRAY(tgInstrArray, tgInstr);

TG_INTERNAL tgInstr tg_instr(tgOp op) {
	tgInstr i;
	memset(&i, 0, sizeof(i));
	i.op = op;
	return i;
}

////////////////////////////////////////////////////////////////
// Types for the ABI (the public tyg_type) and signatures
////////////////////////////////////////////////////////////////

typedef u8 tgTKind;
enum {
	tgT_Void,
	tgT_Int,    // bits: 8, 16, 32, 64
	tgT_F32,
	tgT_F64,
	tgT_Ptr,
	tgT_Array,
	tgT_Record, // fields at offsets, which may overlap
	tgT_F80,    // long double: x87 extended on x86-64 SysV, a double on arm64 macOS and Windows,
	            // unsupported on arm64 Linux (see types.c)
};

struct tyg_type {
	tgTKind         kind;
	bool             is_signed;
	i32              bits;     // Int
	i64              size;
	i64              align;
	tyg_type const * elem;     // Array
	i64              count;    // Array
	tyg_field *      fields;   // Record
	i32              field_count;
};

TG_INTERNAL bool tg_t_is_scalar(tyg_type const *t) {
	return t->kind == tgT_Int || t->kind == tgT_F32 || t->kind == tgT_F64 || t->kind == tgT_Ptr;
}

// The IR scalar type of a scalar tyg_type, else tgType_None.
TG_INTERNAL tgType tg_t_scalar(tyg_type const *t) {
	switch (t->kind) {
	case tgT_Int:
		switch (t->bits) {
		case 8:  return tgType_I8;
		case 16: return tgType_I16;
		case 32: return tgType_I32;
		case 64: return tgType_I64;
		}
		return tgType_None;
	case tgT_F32: return tgType_F32;
	case tgT_F64: return tgType_F64;
	case tgT_Ptr: return tgType_I64;
	}
	return tgType_None;
}

typedef struct tgAbiFunc tgAbiFunc;

struct tyg_sig {
	tgModule *        m;       // the module whose arena holds the sig and its abi
	tyg_type const *  ret;
	tyg_type const ** params;
	i32               count;
	u32               flags;   // TYG_SIG_*
	tgAbiFunc *      abi;     // computed on first use
	char const *      abi_reason; // why there is no abi
	bool              abi_done;
};

////////////////////////////////////////////////////////////////
// ABI
////////////////////////////////////////////////////////////////

typedef u8 tgArgKind;
enum {
	tgArg_Direct,   // passed in registers (or their stack spill slots), split into pieces
	tgArg_Indirect, // a pointer to the value is passed
	tgArg_ByVal,    // the value is copied onto the stack
	tgArg_Ignore,   // zero-sized, not passed at all
};

typedef u8 tgLocKind;
enum {
	tgLoc_Gpr,
	tgLoc_Xmm,
	tgLoc_Stack,
	tgLoc_X87,   // a result in st(0): the 10 bytes of a long double, stored as 16
};

typedef u8 tgExtKind;
enum {
	tgExt_None,
	tgExt_Zero,
	tgExt_Sign,
};

// One register-sized part of an argument or result.
typedef struct tgAbiPiece {
	tgType    type;
	i32        size;       // bytes of the value this piece carries
	i32        src_offset; // where those bytes live within the value
	tgLocKind loc;
	u8         reg;
	tgExtKind ext;
	i32        stack_offset;
} tgAbiPiece;

typedef struct tgAbiArg {
	tgArgKind       kind;
	tyg_type const * type;
	i32              piece_index;
	i32              piece_count;
	// ByVal: where on the stack the copy lives
	i32              stack_offset;
	i32              byval_size;
	i32              byval_align;
	bool             copy;      // Indirect: always pass a copy, the callee may write to it
	i32              copy_part; // Indirect: the copy is split into parts this big and aligned, one pointer piece each
} tgAbiArg;

TG_ARRAY(tgAbiArgArray, tgAbiArg);
TG_ARRAY(tgAbiPieceArray, tgAbiPiece);

struct tgAbiFunc {
	bool             c_vararg;
	bool             has_sret;   // first param is a pointer to the result
	tyg_type const * ret_type;
	tgAbiArg        ret;
	tgAbiArg        sret;       // the hidden sret pointer, valid if has_sret
	tgAbiArgArray   params;     // one per fixed parameter
	tgAbiPieceArray pieces;
	i32              stack_size; // bytes of stack arguments
	i32              gpr_count;  // registers used by the fixed params
	i32              xmm_count;
};

////////////////////////////////////////////////////////////////
// Calls
////////////////////////////////////////////////////////////////

typedef u8 tgCallArgKind;
enum {
	tgCallArg_Gpr,        // load vreg into gpr
	tgCallArg_Xmm,        // load vreg into xmm
	tgCallArg_GprMem,     // load `size` bytes at mem into gpr
	tgCallArg_XmmMem,     // load `size` bytes at mem into xmm
	tgCallArg_Stack,      // store vreg into [rsp + stack_offset]
	tgCallArg_StackMem,   // copy `size` bytes at mem to [rsp + stack_offset]
	tgCallArg_X87Mem,     // a returned long double: load the 80-bit value at mem into st(0)
};

typedef struct tgCallArg {
	tgCallArgKind kind;
	tgType        type;
	u8             reg;
	tgExtKind     ext;
	u32            vreg;
	tgMem         mem;
	i32            size;
	i32            stack_offset;
} tgCallArg;

typedef struct tgCallRet {
	tgLocKind loc; // Gpr, Xmm, or X87: the call's st(0) is stored to dst
	u8         reg;
	tgType    type;
	i32        size;
	tgMem     dst; // stored to memory
} tgCallRet;

TG_ARRAY(tgCallArgArray, tgCallArg);
TG_ARRAY(tgCallRetArray, tgCallRet);
TG_SLICE(tgCallArgSlice, tgCallArg);
TG_SLICE(tgCallRetSlice, tgCallRet);

typedef struct tgCall {
	u32             target_vreg; // 0 if direct
	i32             target_sym;  // -1 if indirect
	tgCallArgSlice args;
	tgCallRetSlice rets;
	i32             stack_size;
	i32             sse_count;   // for C varargs: number of xmm registers used, else -1
	u32             result_vreg; // for syscalls
	bool            returns_twice; // setjmp: nothing stays in a register across the call
} tgCall;

TG_ARRAY(tgCallArray, tgCall);

// One argument of a call, converted from the public tyg_arg by build.c.
typedef struct tgArgValue {
	u32              val;  // a scalar, or 0
	tgMem            mem;  // the aggregate, when val is 0
	tyg_type const * type;
} tgArgValue;

////////////////////////////////////////////////////////////////
// Inline asm
////////////////////////////////////////////////////////////////

typedef u8 tgAsmIoKind;
enum {
	tgAsmIo_Gpr,    // vreg <-> gpr
	tgAsmIo_Gpr8H,  // u8 vreg <-> ah, ch, dh or bh (`reg` 4..7)
	tgAsmIo_Xmm,    // float vreg <-> xmm
	tgAsmIo_XmmMem, // `size` bytes at the pointer vreg <-> xmm/ymm/zmm
	tgAsmIo_Flag,   // output only: the condition `reg` (a tgCC) as 0 or 1
};

typedef struct tgAsmIo {
	tgAsmIoKind kind;
	u8           reg;
	u8           size;
	bool         sign;
	u32          vreg;
} tgAsmIo;

typedef u8 tgAsmItemKind;
enum { tgAsmItem_Bytes, tgAsmItem_Label, tgAsmItem_Align, tgAsmItem_Branch };

// One piece of a template's code: plain bytes, a label, an alignment, or a label
// reference. A branch starts short and grows to its rel32 form when the target is out
// of range; a rip relative memory operand only has the rel32 form.
typedef struct tgAsmItem {
	tgAsmItemKind kind;
	bool is_long;
	i32  label;                             // the label, or the alignment of an Align
	i32  start,      len,      rel_at;      // in the byte pool; the short form of a branch
	i32  long_start, long_len, long_rel_at; // the rel32 form, len 0 if none
} tgAsmItem;

TG_SLICE(tgAsmItemSlice, tgAsmItem);
TG_SLICE(tgAsmIoSlice, tgAsmIo);

// An asm template call: the operands live in fixed registers around the encoded bytes,
// which are laid out where they land, for #align.
typedef struct tgAsmBlock {
	tgAsmItemSlice items;
	tgU8Slice      pool;
	i32             label_count;
	i32             align;      // the largest #align, 0 if none
	tgAsmIoSlice   inputs;
	tgAsmIoSlice   outputs;
	u16             save_regs;  // callee saved gprs the template touches, bit = register number
	i32             save_local; // frame local they are saved in, or -1
	u16             save_xmms;  // Win64: callee saved xmm registers the template touches
	i32             xmm_save_local;
	i32             rbp_local;  // frame local rbp is saved in when the template writes it, or -1
} tgAsmBlock;

TG_ARRAY(tgAsmBlockArray, tgAsmBlock);

////////////////////////////////////////////////////////////////
// Procedures
////////////////////////////////////////////////////////////////

typedef struct tgBlock {
	i32           index;
	bool          placed;
	bool          cold;  // placed after the rest of the procedure
	i32           debug_scope; // the lexical scope the block starts in
	tgInstrArray instrs;
	i32           code_offset;
} tgBlock;

TG_ARRAY(tgBlockPtrArray, tgBlock *);

typedef struct tgLocal {
	i64 size;
	i64 align;
	i32 frame_offset;
	i64 over_align; // > 16: frame_offset holds a pointer into an aligned spot of the raw area
	i32 raw_offset;
} tgLocal;

TG_ARRAY(tgLocalArray, tgLocal);

typedef struct tgParamIn {
	tgLocKind loc;
	u8         reg;
	tgType    type;
	i32        size;
	i32        stack_offset;
	tgMem     dst;
} tgParamIn;

TG_ARRAY(tgParamInArray, tgParamIn);

typedef struct tgLineEntry {
	i32 code_offset;
	i32 file_id;
	i32 line;
	i32 column;
} tgLineEntry;

TG_ARRAY(tgLineEntryArray, tgLineEntry);

typedef struct tgDebugVar {
	tgString         name;
	tyg_dtype const * type;
	i32               local;      // frame local holding the variable, or -1
	i32               frame_offset_fixup; // resolved after layout
	bool              is_param;
	bool              by_ref;     // the local holds a pointer to the value
	i32               line;
	i32               file_id;
	i32               sym;        // static storage: the symbol, used when local < 0
	bool              in_reg;     // the variable, or with by_ref its address, is in dwarf_reg wherever a debugger shows it
	u8                dwarf_reg;
	i32               scope;      // lexical scope, 0 is the procedure
	i32               offset;     // within the local or the symbol
} tgDebugVar;

TG_ARRAY(tgDebugVarArray, tgDebugVar);

// From code_offset on, until the next one, the code belongs to a lexical scope.
typedef struct tgScopeMark {
	i32  code_offset;
	i32  scope;
	bool cold;
} tgScopeMark;

TG_ARRAY(tgScopeMarkArray, tgScopeMark);

// A global variable or constant, for the debug info.
typedef struct tgGlobalDebug {
	tgString         name;
	tyg_dtype const * type;
	i32               sym;   // -1 for a constant, which has no file and line
	i32               file_id;
	i32               line;
	i64               value; // for a constant
} tgGlobalDebug;

TG_ARRAY(tgGlobalDebugArray, tgGlobalDebug);

// A body inlined into its caller, whose code is the lexical scope `scope`.
typedef struct tgInlineSite {
	i32       scope;
	tgString name;
	i32       decl_file;
	i32       decl_line;
	i32       call_file;
	i32       call_line;
	i32       call_column;
} tgInlineSite;

TG_ARRAY(tgInlineSiteArray, tgInlineSite);

typedef struct tgSavedReg { i32 dwarf_reg; i32 frame_offset; } tgSavedReg;
TG_ARRAY(tgSavedRegArray, tgSavedReg);

typedef struct tgProcDebug {
	tgString          name;       // the source name
	tgString          link_name;
	i32                sym;
	u8                 section;   // the tgSection of the code: .text, or a named one
	i64                start;
	i64                end;
	i32                file_id;
	i32                line;
	i32                line_entry_start;
	i32                line_entry_count;
	i32                prologue_end; // code offset of the nop that ends the prologue on the declaration's line, 0: none
	tgDebugVarArray   vars;
	tgI32Array        scope_parent; // per lexical scope, scope 0 is the procedure's
	tgInlineSiteArray inline_sites;
	tgScopeMarkArray  scope_marks;
	tyg_dtype const *  type;
	bool               naked; // no frame, the cfa stays rsp+8
	// callee saved registers spilled by the prologue, for the unwinder
	tgSavedRegArray   saved_regs;
	i32                saved_at; // code offset right after the spills
	// Win64 prologue, for the unwind info: pushes, an optional `sub rsp, pad + xmm saves`,
	// `mov rbp, rsp`, `sub rsp, N`, then `movaps [rbp + 16*i], xmm`
	u8                 win_push_reg[8];
	u8                 win_push_at[8];
	i32                win_push_count;
	u8                 win_pad_at;     // 0: no pad
	i32                win_pad_size;   // the pad and the xmm save area
	u8                 win_xmm_reg[10];
	u8                 win_xmm_at[10];
	i32                win_xmm_count;
	u8                 win_setfp_at;
	u8                 win_alloc_at;   // 0: no allocation
	i32                win_alloc_size;
} tgProcDebug;

TG_ARRAY(tgProcDebugArray, tgProcDebug);

// An empty debug record whose arrays grow in `arena`.
TG_INTERNAL void tg_proc_debug_init(tgProcDebug *d, tgArena *arena) {
	memset(d, 0, sizeof(*d));
	d->vars.arena = arena;
	d->scope_parent.arena = arena;
	d->inline_sites.arena = arena;
	d->scope_marks.arena = arena;
	d->saved_regs.arena = arena;
}

// What the builder knows of a vreg's value. Every vreg has one definition.
typedef struct tgVregInfo {
	i64  k;     // the constant, sign extended from the type's width
	u64  umax;  // the largest value the vreg can hold, zero extended
	bool is_k;
	u32  kbase; // when not 0: the vreg is kbase + kadd, kadd a constant
	u32  suma, sumb; // when not 0: the i64 vreg is suma + sumb
	i64  kadd;
} tgVregInfo;

TG_ARRAY(tgVregInfoArray, tgVregInfo);

struct tyg_proc {
	tgModule *        m;
	tyg_sig const *    sig;
	tgString          name;    // the link name
	i32                sym;
	tgAbiFunc *       abi;
	bool               naked;   // no prologue, epilogue or stack slots
	u8                 code_section; // tyg_proc_section: the tgSection of the code, 0 for .text

	tgBlockPtrArray   blocks;
	tgBlockPtrArray   order;
	tgBlock *         curr;
	tgTypeArray       vregs;   // vreg 0 is no value
	tgVregInfoArray   vinfo;
	tgLocalArray      locals;
	tgCallArray       calls;
	tgAsmBlockArray   asms;
	tgParamInArray    params_in;

	tgU32Array        param_vals;  // per parameter: its vreg, or a pointer vreg to its memory
	i32                sret_local;  // the local holding the hidden result pointer, or -1
	i32                va_save_local; // variadic: the register save area, or -1
	i32                call_align;    // the alignment the calls' stack arguments need past 16, or 0

	i32                file_id;
	i32                last_line;
	i32                last_column;

	tgString          debug_name; // the source name, or empty for the link name
	i32                decl_file;
	i32                decl_line;
	tyg_dtype const *  debug_type;
	tgDebugVarArray   debug_vars;
	tgI32Array        debug_scope_parent;
	tgInlineSiteArray inline_sites;
	i32                debug_scope;
	isize              params_ready; // 1 + the index of the entry block's tgOp_Nop with imm 2, or 0

	char const *       fail_reason; // the first failure, see tg_fail in build.c
};

TG_INTERNAL i32 tg_vreg_count(tgProc *p) { return (i32)p->vregs.count; }

////////////////////////////////////////////////////////////////
// Module
////////////////////////////////////////////////////////////////

typedef u8 tgSection;
enum {
	tgSection_Undef,
	tgSection_Text,
	tgSection_Rodata,
	tgSection_Data,
	tgSection_Bss,
	tgSection_TData,
	tgSection_TBss,
	tgSection_COUNT,
};

enum {
	tgSymbolFlag_Func    = 1<<0,
	tgSymbolFlag_Global  = 1<<1,
	tgSymbolFlag_Weak    = 1<<2,
	tgSymbolFlag_Hidden  = 1<<3,
	tgSymbolFlag_Foreign = 1<<4, // may be defined outside the executable, reach data through the GOT
	tgSymbolFlag_TLS     = 1<<5,
	tgSymbolFlag_Export  = 1<<6, // default visibility, reached through the GOT by a shared object; dllexport on Windows
	tgSymbolFlag_Undef   = 1<<7, // only from tg_lower_sym_flags: the symbol has no section yet
	tgSymbolFlag_Protected = 1<<8, // ELF STV_PROTECTED; not in tg_lower_sym_flags
};

typedef struct tgSymbol {
	tgString  name;
	tgSection section;
	u16        flags;
	i64        offset;
	i64        size;
	i64        realign;  // a thread local whose accesses round its address up to this
	i32        elf_index;
	i32        alias_of;     // tyg_alias: the target + 1, resolved when the object is written
	i64        common_align; // tyg_common: the symbol is a common symbol of `size` bytes
} tgSymbol;

TG_ARRAY(tgSymbolArray, tgSymbol);

typedef u8 tgRelocKind;
enum {
	tgReloc_PC32,       // S + A - P
	tgReloc_PLT32,
	tgReloc_GOTPCRELX,
	tgReloc_REX_GOTPCRELX,
	tgReloc_Abs64,      // S + A
	tgReloc_Abs32,
	tgReloc_TPOFF32,
	tgReloc_GOTTPOFF,
	tgReloc_TLSGD,
	tgReloc_SecRel32,   // COFF: offset from the start of the symbol's section
	tgReloc_TLV,        // x86-64 Mach-O: the address of a thread local's descriptor
	// arm64, the immediate fields are filled in by the linker
	tgReloc_A64_Branch26,     // bl
	tgReloc_A64_Page21,       // adrp of the symbol's page
	tgReloc_A64_PageOff12,    // add of the low 12 bits
	tgReloc_A64_GotPage21,    // adrp of the page of the symbol's GOT entry
	tgReloc_A64_GotPageOff12, // ldr of the GOT entry
	tgReloc_A64_TlvPage21,    // Mach-O: adrp of the page of the thread local's descriptor
	tgReloc_A64_TlvPageOff12, // Mach-O: ldr of the descriptor's address
	tgReloc_A64_TlsIePage21,  // ELF: adrp of the page of the GOT entry with the thread local's offset
	tgReloc_A64_TlsIeLo12,    // ELF: ldr of that GOT entry
	tgReloc_A64_TlsDescPage21,// ELF: adrp of the page of the thread local's TLS descriptor
	tgReloc_A64_TlsDescLd,    // ELF: ldr of the descriptor's resolver
	tgReloc_A64_TlsDescAdd,   // ELF: add of the descriptor's low 12 bits
	tgReloc_A64_TlsDescCall,  // ELF: the blr of the resolver
};

typedef struct tgReloc {
	tgSection   section;
	tgRelocKind kind;
	u8           tail;   // rip relative: the immediate bytes after the field, which x86-64 Mach-O encodes
	i64          offset;
	i32          sym;
	i64          addend;
} tgReloc;

TG_ARRAY(tgRelocArray, tgReloc);

// The symbols for the start of each section, for relocations in debug info
enum {
	tgSectionSym_Text,
	tgSectionSym_DebugLine,
	tgSectionSym_DebugAbbrev,
	tgSectionSym_DebugStr,
	tgSectionSym_COUNT,
};

// A symbol that lowering needs and that may not exist yet. Lowering may run on worker
// threads and must not add symbols, so it refers to these as TG_PENDING_SYM + index
// until its code is appended.
typedef struct tgPendingSym {
	tgString name;
	u32       flags;
	i8        helper;    // x86-64: 1 the memmove helper, 2 the memset helper, which the append emits
	i32       import_of; // Windows: symbol + 1 whose __imp_ symbol this is, named by the append
} tgPendingSym;

TG_ARRAY(tgPendingSymArray, tgPendingSym);

enum { TG_PENDING_SYM = 1<<30 };

// What lowering one procedure produces, with code offsets from the procedure's start.
// The module appends it in a fixed order, so the object does not depend on threads.
typedef struct tgLowerOut {
	tgBytes           text;
	tgRelocArray      relocs;
	tgLineEntryArray  lines;
	tgPendingSymArray pending;
	tgProcDebug       dbg;
	i64                align;
	tgSection         section; // where the code goes: .text, or a named section
} tgLowerOut;

////////////////////////////////////////////////////////////////
// Debug types (the public tyg_dtype)
////////////////////////////////////////////////////////////////

typedef u8 tgDtKind;
enum {
	tgDt_Void,
	tgDt_Base,
	tgDt_Pointer,
	tgDt_Const,
	tgDt_Volatile,
	tgDt_Typedef,
	tgDt_Array,
	tgDt_Func,
	tgDt_Record,
	tgDt_Enum,
};

typedef struct tgDtMember {
	tgString         name;      // empty for an anonymous member
	tyg_dtype const * type;
	i64               offset;    // bytes; for a bitfield, bit_offset holds the position
	i64               bit_offset;
	i32               bit_size;  // 0 if not a bitfield
} tgDtMember;

typedef struct tgDtEnumerator {
	tgString name;
	i64       value;
} tgDtEnumerator;

TG_ARRAY(tgDtMemberArray, tgDtMember);
TG_ARRAY(tgDtEnumeratorArray, tgDtEnumerator);

struct tyg_dtype {
	tgDtKind            kind;
	u8                   encoding;    // Base: tyg_dt_encoding
	u8                   record_kind; // Record: tyg_dt_record_kind
	bool                 complete;    // Record
	bool                 variadic;    // Func
	tgString            name;
	i64                  size;
	i64                  align;
	tyg_dtype const *    target;      // Pointer, Const, Volatile, Typedef, Array element, Func result, Enum underlying
	i64                  count;       // Array, < 0 if unknown
	tyg_dtype const **   params;      // Func
	i32                  param_count;
	tgDtMemberArray     members;     // Record
	tgDtEnumeratorArray enumerators; // Enum
	i32                  file_id;
	i32                  line;
	i32                  index;       // creation order, for stable output
};

TG_ARRAY(tgDtypePtrArray, tyg_dtype *);

// A data section the front end names, see tyg_data_named. Its tgSection is
// tgSection_COUNT + its index.
typedef struct tgNamedSection {
	tgString name;
	u32      flags;       // TYG_SECF_*
	tgBytes  bytes;       // empty with TYG_SECF_ZERO
	i64      nobits_size; // with TYG_SECF_ZERO
	i64      align;
} tgNamedSection;

TG_ARRAY(tgNamedSectionArray, tgNamedSection);

////////////////////////////////////////////////////////////////
// Module
////////////////////////////////////////////////////////////////

struct tyg_module {
	tyg_target          target;
	tgString           source_name;
	tgString           comp_dir;
	u16                language;   // DW_LANG_*, see tyg_module_language
	tgString           object_path; // the path tyg_module_write writes to, empty for tyg_module_emit

	tgArena            arena;   // types, signatures and names: everything that lives as long as the module

	tgSymbolArray      symbols;
	tgStrMap           symbol_map;

	tgBytes            sections[tgSection_COUNT];
	i64                 nobits_size[tgSection_COUNT]; // .bss and .tbss hold no bytes
	i64                 section_align[tgSection_COUNT];
	tgNamedSectionArray named_sections;
	tgRelocArray       relocs;

	// debug info
	tgProcDebugArray   proc_debug;
	tgGlobalDebugArray global_debug;
	tgLineEntryArray   lines;
	tgStringArray      files;
	tgStrMap           file_ids;
	tgDtypePtrArray    dtypes;

	// x86-64: the module's memmove and memset for sizes known at run time, symbol + 1, 0 until emitted
	i32                 x64_move_helper;
	i32                 x64_set_helper;

	tgProc *           proc;      // the procedure being built, or NULL
	tgArena            proc_arena;

	tgBytes            output;    // the object, after tg_module_emit
	char                error[512];
	bool                has_error;
};

// A symbol's flags for lowering, with tgSymbolFlag_Undef while it has no section. Lowering
// adds no symbols and changes none, so it sees them as they were when it started.
TG_INTERNAL u8 tg_lower_sym_flags(tgModule *m, i64 sym) {
	TG_ASSERT(0 <= sym && sym < m->symbols.count);
	tgSymbol const *s = &m->symbols.data[sym];
	return (u8)(s->flags | (s->section == tgSection_Undef ? tgSymbolFlag_Undef : 0));
}

#endif
