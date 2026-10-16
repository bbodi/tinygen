# Provenance

tinygen started as the debug backend of a compiler, written in C++. This page
records where each part came from and what changed in the port. The comments in
the source describe the current code only.

## Files

| tinygen         | Ported from                                                                   |
|-----------------|-------------------------------------------------------------------------------|
| `build.c`       | `xb_gen.cpp` (the IR helpers), `xb_gen_stmt.cpp` (calls, returns, parameters, debug scopes), `xb_backend.cpp` (`xb_new_proc`, `xb_lower_proc`) |
| `module.c`      | the module parts of `xb_gen.cpp`, `xb_globals.cpp` (`xb_section_reserve`), `xb_dwarf.cpp` (`xb_file_id`), `xb_backend.cpp` (`xb_append_lowered`, `xb_lower_start`) |
| `x64_abi.c`     | `x64_abi.cpp`                                                                 |
| `x64_win64.c`   | `x64_win64.cpp`                                                               |
| `a64_abi.c`     | `a64_abi.cpp`                                                                 |
| `a64_lower.c`   | `a64_lower.cpp`                                                               |
| `dwarf.c`       | `xb_dwarf.cpp`                                                                |
| `codeview.c`    | `x64_codeview.cpp`                                                            |
| `elf.c`         | `x64_elf.cpp`                                                                 |
| `macho.c`       | `xb_macho.cpp`                                                                |
| `coff.c`        | `x64_coff.cpp`                                                                |
| `types.c`       | new: the original took its types from its front end                           |

## What changed

- **Types.** The original ABI code classified LLVM-lowered types
  (`lbAbiAmd64SysV`, `lbAbiArm64`). tinygen classifies `tyg_type` directly,
  following clang's `X86_64ABIInfo`, `AArch64ABIInfo` and `WinX86_64ABIInfo`
  for C. The debug info writers take `tyg_dtype` and write C types the way
  clang and MSVC do. The register assignment machinery is the original's.
- **Failure.** The original left a procedure with a `longjmp`. The public API
  cannot, because the caller's code is not inside a `setjmp`. A failure now
  records `p->fail_reason`, and `tyg_proc_end` reports it.
- **Objects.** The writers fill a byte buffer instead of writing to a path.
- **Removed.** The shadow modules that replayed module writes, the thread
  pool, the RAD Debugger section (`.raddbg`) of COFF objects, the checks for
  x86-only operations in the arm64 lowering (`cpuid`, `xgetbv`, valgrind
  requests), and the inline assembly templates (`x64_asm.cpp`), which the
  public API never exposed. The IR op `tgOp_Asm`, its lowering and the
  register allocator's pin callback went with them.
- **Thread locals.** The original chose the TLS model from the build mode.
  tinygen does not know the build mode, so it takes position independent
  code as the sign of a shared library.
- **Closures.** Several C++ lambdas became helper functions with a state
  struct: `x64_helper`, `tg_cleanup_proc`, `tg_alloc_regs`, `tg_alloc_slots`,
  the Mach-O writer, and `a64_for_each_pair`, `jump_if` and `skip_if` in the
  arm64 lowering.
