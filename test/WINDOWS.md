# Testing the Windows x64 target

`test/smoke.c` builds the smoke functions into a COFF object with CodeView
debug info; `test/smoke_main.c` calls them and checks the results. These steps
build both on a Windows machine, run the checks, and look at the debug info in
a debugger.

Run every command from the repository root, in an **x64 Native Tools Command
Prompt for VS** (so `cl`, `link` and `dumpbin` are on the path and target x64).
Use either MSVC (`cl` + `link`) or LLVM (`clang-cl` + `lld-link`).

## 1. Build the generator and emit the object

MSVC:

    mkdir build
    cl /nologo /std:c11 /W3 /D_CRT_SECURE_NO_WARNINGS /Iinclude test\smoke.c src\tinygen.c /Fobuild\ /Fe:build\smoke.exe
    build\smoke.exe build\smoke.obj windows

clang-cl:

    mkdir build
    clang-cl /nologo /std:c11 /W3 /D_CRT_SECURE_NO_WARNINGS /Iinclude test\smoke.c src\tinygen.c /Fobuild\ /Fe:build\smoke.exe
    build\smoke.exe build\smoke.obj windows

`/std:c11` needs Visual Studio 2019 16.8 or later; tinygen is C99, which MSVC
only accepts in its C11 mode. Both compilers build it without warnings at `/W3`.
`/W3` rather than `/W4`: every internal function is `static`, and MSVC's `/W4`
warns about each one a translation unit leaves unused (clang-cl does not: the
functions are marked unused).

The second argument picks the target: `windows` writes x86-64 COFF + CodeView,
`windows-dwarf` COFF + DWARF, `linux` (the default) x86-64 ELF + DWARF,
whatever the host is.

## 2. Link and run the checks

MSVC:

    cl /nologo /Zi /Od test\smoke_main.c build\smoke.obj /Fobuild\ /Fdbuild\ /Fe:build\smoke_run.exe /link /DEBUG:FULL
    build\smoke_run.exe

clang-cl + lld-link:

    clang-cl /nologo /Z7 /Od test\smoke_main.c build\smoke.obj /Fobuild\ /Fe:build\smoke_run.exe -fuse-ld=lld /link /DEBUG
    build\smoke_run.exe

MinGW (gcc + GNU ld, from any shell with MinGW-w64 on the path). The
`windows-dwarf` target writes DWARF instead of CodeView, for gdb:

    gcc -std=c99 -Iinclude test\smoke.c src\tinygen.c -o build\smoke.exe
    build\smoke.exe build\smoke.obj windows-dwarf
    gcc -g test\smoke_main.c build\smoke.obj -o build\smoke_run.exe
    build\smoke_run.exe

Expected output:

    hello 42 1.50
    all passed

and exit code 0 (`echo %ERRORLEVEL%`). A failing check prints `FAIL smoke_main.c:<line>: ...`.

Expected linker messages:

- **LNK4217** (`locally defined symbol tg_ext_value imported`): on purpose.
  `tg_read_ext` reads `tg_ext_value` as foreign data, through
  `__imp_tg_ext_value`, as it would from a DLL; the linker makes that pointer
  itself since `smoke_main.c` defines the variable. GNU ld does not, so under
  MinGW `smoke_main.c` defines `__imp_tg_ext_value` itself, and there is no warning.
- `build\smoke_run.lib` and `.exp` appear: `tg_add` and the data `tg_origin` are
  exported (`-export:` in the object's `.drectve`), which turns the import
  library on. `dumpbin /exports build\smoke_run.exe` lists both.

If `printf` is an unresolved external with an old toolset, add
`legacy_stdio_definitions.lib` after `/link`.

What the checks cover on Windows: the Win64 ABI (4 positional register slots,
the shadow area, stack arguments, structs of 1/2/4/8 bytes in registers and all
others by pointer to a copy, sret, floats in xmm), C varargs both ways (floats
also in the integer registers, `va_list` as a pointer into the home slots,
aggregate varargs), callee saved xmm6+, the stack probes of a frame over a page
and of an `alloca` of 256 KB (without them the program dies with an access violation),
thread locals through `_tls_index` and `gs:[0x58]`, `__imp_` data, a weak
definition, and `longjmp` out of a callback through a tinygen frame, which
unwinds that frame with its `.pdata`/`.xdata` (with broken unwind info the
program crashes there).

### The ABI test

`test/abi.c` checks the calling convention and the lowering more thoroughly
against C: float aggregates, 16 byte aligned structs, results in memory, a call
with 27 arguments, char and short, varargs of every kind both ways, `snprintf`,
thread locals in a second thread, atomics, `alloca`, bit operations,
conversions, `long double` and a far branch. The C half needs C11 (`_Alignas`, `_Thread_local`):

    cl /nologo /std:c11 /W3 /D_CRT_SECURE_NO_WARNINGS /Iinclude test\abi.c src\tinygen.c /Fobuild\ /Fe:build\abi.exe
    build\abi.exe build\abi.obj windows
    cl /nologo /std:c11 /Zi /Od test\abi_main.c build\abi.obj /Fobuild\ /Fdbuild\ /Fe:build\abi_run.exe /link /DEBUG:FULL
    build\abi_run.exe

with clang-cl as in step 1, linking with `-fuse-ld=lld`; or with MinGW:

    gcc -std=c11 -mlong-double-64 -g test\abi_main.c build\abi.obj -o build\abi_run.exe

It prints `all passed`. `link` and lld give LNK4217 for `c_small_ptr`, which
tinygen reads as foreign data, like `tg_ext_value` above. GCC's MinGW port
has only emulated thread locals, which cannot reach tinygen's native ones, so
built by gcc the test checks those through tinygen's functions only.

tinygen's `long double` on Windows is MSVC's: a `double`, passed like one.
MinGW's gcc makes it the x87 type in 16 bytes, passed by reference, unless it
builds with `-mlong-double-64`; without the flag `abi_main.c` stops with an
`#error`.

## 3. Look at the object (optional)

    dumpbin /headers /symbols /relocations build\smoke.obj
    dumpbin /unwindinfo build\smoke.obj
    dumpbin /disasm build\smoke.obj

With LLVM: `llvm-readobj --codeview --unwind build\smoke.obj` and
`llvm-pdbutil dump -types -symbols -l build\smoke.obj`.

## 4. Check the debug info in a debugger

The line tables name the source file `smoke.tg`. `test\smoke.tg` is a
pseudo-source with the functions on the lines the smoke test gives them; when
the debugger asks for `smoke.tg`, point it at `test\smoke.tg` (or add `test`
to the source path).

### Visual Studio

    devenv /debugexe build\smoke_run.exe

1. Debug > New Breakpoint > Function Breakpoint: `tg_sum`. F5.
2. It stops in `tg_sum`, line 5 of `smoke.tg`. The Locals window shows the
   parameter `n` (100) and the variables `s` and `i`, all `__int64`.
3. F10 steps through lines 6 and 7 of the loop; `s` and `i` change as it goes
   (`i` counts up, `s` sums). Shift+F11 returns to `main`.
4. Function breakpoint `tg_node_sum`, F5. Locals: `n` (a `Node *`: expand it to
   `next`, `name`, `vals` (3 doubles), `color` = `GREEN`, `flags` with the
   bitfields `a`, `b` and `c` = 100, and the anonymous union's `i` and `f`),
   `p` (a `Point`, x = 1, y = 2) and `t`. Step to line 53: the block's `inner`
   appears, and is gone again after line 54. Watch `tg_origin`: x = 10, y = 20.
5. Function breakpoint `twice` (in `smoke_main.c`), F5: the Call Stack shows
   `twice` <- `tg_call_cb` <- `main`, walked with tinygen's unwind info.

### WinDbg

    windbg build\smoke_run.exe

    .srcpath+ <repo>\test
    l+t
    bp smoke_run!tg_sum
    bp smoke_run!twice
    bp smoke_run!tg_node_sum
    g
    dv /i /t /v
    p
    dv /i /t /v
    p
    p
    dv /i /t /v
    k
    g
    k
    g
    p
    dv /i /t /v
    ?? *n
    ?? n->flags
    p
    dv /i /t /v
    dt smoke_run!Node
    ?? tg_origin
    .fnent smoke_run!tg_fkeep

`l+t` makes `p` step by source line. A breakpoint on a name stops on the first
instruction, before the prologue: there `dv` shows the register variables as
`<value unavailable>` (and the frame ones at an address off a stale rbp); the
first `p` goes to line 6 with `n` = 100. `dv /i /t /v` lists the variables with
their kinds (`param`/`local`), types and locations: `n`, `s` and `i` in
`tg_sum`, where `i` counts up and `s` sums as `p` goes round the loop; `k`
shows `tg_sum` <- `main`. At `twice`, `k` shows `twice` <- `tg_call_cb` <-
`main`, walked with tinygen's unwind info. In `tg_node_sum`: `n`, `p` and `t`,
`?? *n` with `color` = 5 (`GREEN`), and `n->flags` with the bitfields `a`, `b`
and `c` = 100; after a step to line 53 `inner` appears, and it is gone again at
line 56. `.fnent` shows `tg_fkeep`'s unwind info: the pushes of rbp, rsi and
rdi, the frame register rbp, and the save of xmm6.

`tg_big_sum` takes `struct Big` by pointer to a copy: `bp smoke_run!tg_big_sum`,
`g`, step the prologue with `l-t` and `p`, then `dv /i /t /v` shows the
parameter as `struct Big * b` and `?? b` as {1, 20, 300}, just as WinDbg shows
such a parameter of MSVC's.

### gdb (MinGW, the `windows-dwarf` object)

    gdb build\smoke_run.exe

    directory test
    break tg_sum
    break tg_big_sum
    break twice
    break tg_node_sum
    run
    info args
    info locals
    next
    next
    next
    info locals
    continue
    print b
    continue
    bt
    continue
    print *n
    next
    next
    info locals
    print tg_origin
    ptype struct Node

The breakpoint in `tg_sum` stops on line 5 with `n` = 100, `s` and `i` 0;
`next` goes round the loop and `i` counts up. `b` in `tg_big_sum` is {a = 1,
b = 20, c = 300}. At `twice`, `bt` shows `twice` <- `tg_call_cb` (smoke.tg:40)
<- `main`. In `tg_node_sum`: `*n` with `color = GREEN`, `flags = {a = 0, b =
0, c = 100}` and the anonymous union; at line 53 `inner` is a local; `ptype
struct Node` prints the whole struct. gdb reads no CodeView, so with the
`windows` object it only names the procedures and unwinds through them.

### lldb (the `windows-dwarf` object)

lldb on Windows needs Python's `python3.dll` on the path. Give it the
`windows-dwarf` object, linked by GNU ld as for gdb, or by `lld-link
/debug:dwarf`:

    lldb build\smoke_run.exe

    b tg_sum
    b tg_big_sum
    b smoke.tg:54
    run
    next
    next
    next
    frame variable
    continue
    frame variable b
    continue
    frame variable inner t

`tg_sum` steps by line with `n` = 100 and `i` counting up, `b` is {a = 1, b =
20, c = 300}, and at line 54 `inner` is 100. Use DWARF for lldb: with the
CodeView object it shows a variable in an xmm register as "unable to convert
register", and one in the low part of a register (`r8d`, `edi`, `dil`) as 0
or garbage, just as it does for clang's CodeView. A typedef's name, such as an
optional's, survives only in DWARF.

### RAD Debugger

    cd test
    raddbg ..\build\smoke_run.exe

Started from `test`, it finds `smoke.tg` itself. Add a function breakpoint on
`tg_sum` (F1, "Add Function Breakpoint", or type the name into the breakpoints
view) and run with F5: after one F10 the Locals view shows `n` = 100, `s` and
`i`, which change as F10 goes round the loop. In `tg_node_sum`: `n` with
`color` = `GREEN` and `flags.c` = 100, and `inner` at line 53. At `twice` the
call stack is `twice` <- `tg_call_cb` <- `main`.

## What was checked

On Windows 11 x64: steps 1 and 2 with VS 2022 Build Tools 17.14 (`cl` 19.44,
`link`) and with LLVM 23.1 (`clang-cl`, `lld-link`), both without compiler
warnings; the generator built by either writes the same object, and is clean
under `/fsanitize=address` for every target. `dumpbin /unwindinfo` and
`llvm-pdbutil` read the object and the PDBs. The WinDbg steps above pass on the
executables of both linkers. With WinLibs GCC 16.2 and binutils 2.47: the
smoke test passes linked by GNU ld, the generator built by gcc writes the same
object, and a strong definition overrides the weak `tg_weak_answer` with all
three linkers. The gdb steps above pass on the `windows-dwarf` object linked
by GNU ld. The Visual Studio steps above pass in Visual Studio 2022 17.14
Community, and the RAD Debugger steps in the RAD Debugger 0.9.27, both on the
executable `link` makes.

Before that, on macOS: the object parsed with `llvm-readobj --codeview
--unwind` and `llvm-pdbutil dump`, linked with `lld-link` (a DLL against a stub,
and a PDB built from it), and the smoke test passed when linked by lld in MinGW
mode and run under Wine.
