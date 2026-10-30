# tinygen

A tiny, fast code generator for debug builds, as a C99 library.

A front end builds procedures in a small IR through `include/tinygen.h`.
tinygen lowers them to machine code and writes an object file with debug
info. The object follows the platform's C ABI, so it links with objects from
any C compiler. tinygen exists to make debug builds fast. It only optimizes
where that is cheap and local: it folds constants, reuses repeated loads,
forwards copies, pairs loads and removes dead values.

tinygen started as a fast debug backend for a compiler, and is now a
standalone C99 library. Any host writes objects for every target.

## Platforms

|         | x86-64 | arm64 |
|---------|:------:|:-----:|
| Linux   |   ✅   |  ✅   |
| macOS   |   ✅   |  ✅   |
| Windows |   ✅   |  ❌   |

- 🔀 Any host writes objects for every target
- 🐞 Debuggers: gdb, lldb, Visual Studio, WinDbg, RAD

### Debug info formats

`tyg_target.debug_info` picks the format. The default is DWARF, and CodeView
on Windows for the Microsoft debuggers. `TYG_DEBUG_DWARF` writes DWARF on
Windows too, in the COFF object, for gdb, lldb and the MinGW toolchain. Link
such an object with GNU ld, or with `lld-link /debug:dwarf`, which keep the
DWARF in the executable.

The two formats differ in what the debuggers show:

- lldb reads CodeView, but on Windows it shows variables in xmm registers and
  in the low part of a register wrongly. It does the same with clang's objects.
- A CodeView variable has the type that a typedef names, not the typedef. So
  every debugger shows `Link *` where DWARF shows the typedef `Link`.

## Build

tinygen is one translation unit:

    cc -std=c99 -c src/tinygen.c -o tinygen.o

With MSVC, which takes C99 code in its C11 mode (Visual Studio 2019 16.8 or later):

    cl /std:c11 /W3 /c src\tinygen.c

## Hello world

tinygen is not a C compiler: it takes procedures described through its API
and writes an object file. So a hello world is a C program that asks tinygen
to generate `main`, and the object it writes is linked into the executable.
Save this as `hello.c`:
```c
#include "tinygen.h"
#include <stdio.h>

int main(void) {
    tyg_module *module = tyg_module_new(tyg_host_target(), "hello.c");

    static const char msg[] = "hello, world";
    tyg_sym msg_sym = tyg_symbol(module, "msg", 0);
    tyg_data(module, msg_sym, TYG_SECTION_RODATA, msg, sizeof(msg), 1);

    /* int puts(const char *); defined in libc */
    const tyg_type *i32 = tyg_type_scalar(module, TYG_I32);
    const tyg_type *ptr = tyg_type_ptr(module);
    tyg_sig *puts_sig = tyg_sig_new(module, i32, &ptr, 1, 0);
    tyg_sym puts_sym = tyg_symbol(module, "puts", TYG_SYM_FUNC | TYG_SYM_GLOBAL | TYG_SYM_FOREIGN);

    /* int main(void) { puts(msg); return 0; } */
    tyg_sig *main_sig = tyg_sig_new(module, i32, NULL, 0, 0);
    tyg_proc *proc = tyg_proc_begin(module, tyg_symbol(module, "main", TYG_SYM_FUNC | TYG_SYM_GLOBAL), main_sig);
    tyg_arg arg = {0};
    arg.val = tyg_addr_of(proc, tyg_at_sym(msg_sym, 0));
    tyg_call(proc, puts_sig, tyg_direct(puts_sym), &arg, 1, NULL, NULL);
    tyg_ret(proc, tyg_iconst(proc, TYG_I32, 0));

    if (tyg_proc_end(proc) != 0 || tyg_module_write(module, "hello.o") != 0) {
        fprintf(stderr, "%s\n", tyg_error(module));
        return 1;
    }
    tyg_module_free(module);
    return 0;
}
```
Build it together with tinygen, run it to get `hello.o`, then link that with
the C compiler, which brings in libc for `puts`:

    cc -std=c99 -Iinclude hello.c src/tinygen.c -o hello_gen
    ./hello_gen
    cc hello.o -o hello
    ./hello

It prints `hello, world`. The pieces, in order:

- `tyg_module_new` starts an object for the host; `"hello.c"` is the source
  name the debug info records.
- `tyg_symbol` plus `tyg_data` define the string in the read-only data
  section. The flags are 0, so `msg` stays local to the object.
- `puts` is only declared: `TYG_SYM_FOREIGN` marks it as defined elsewhere,
  and `tyg_sig_new` gives its C signature so the call follows the ABI.
- `tyg_proc_begin` opens `main` with its entry block current. `tyg_addr_of`
  takes the string's address, `tyg_call` calls `puts` with it, and `tyg_ret`
  returns the constant 0.
- `tyg_proc_end` finishes the procedure and `tyg_module_write` writes the
  object. Both report failures through `tyg_error`.

## Use

A front end describes each procedure and writes the object; `include/tinygen.h`
documents every call. This one writes `long long add(long long a, long long b)`
for the host:
```c
#include "tinygen.h"
#include <stdio.h>

int main(void) {
    tyg_module *m = tyg_module_new(tyg_host_target(), "add.c");
    const tyg_type *i64 = tyg_type_scalar(m, TYG_I64);
    const tyg_type *params[2] = {i64, i64};
    tyg_sig *sig = tyg_sig_new(m, i64, params, 2, 0);

    tyg_proc *p = tyg_proc_begin(m, tyg_symbol(m, "add", TYG_SYM_FUNC | TYG_SYM_GLOBAL), sig);
    tyg_ret(p, tyg_bin(p, TYG_ADD, tyg_param(p, 0), tyg_param(p, 1)));
    if (tyg_proc_end(p) != 0 || tyg_module_write(m, "add.o") != 0) {
        fprintf(stderr, "%s\n", tyg_error(m));
        return 1;
    }
    tyg_module_free(m);
    return 0;
}
```
`add.o` links into a C program that declares `long long add(long long, long long);`.
For another target, fill a `tyg_target` instead of calling `tyg_host_target`.
`test/smoke.c` shows the rest: control flow, memory, calls, varargs, thread
locals and debug info.

## C output

tinygen can write C source instead of an object. Set `output` in the target:
```c
tyg_target t = tyg_host_target();
t.output = TYG_OUTPUT_C;
tyg_module *m = tyg_module_new(t, "add.c");
/* the same calls as above */
tyg_module_write(m, "add.tg.c");
```
The same front end then has both: a debug build straight from tinygen in
milliseconds, and an optimized build by handing the C to any C compiler with
`-O2`. The C is one self-contained C99 translation unit for the target's arch
and os: a prelude of macros with gcc/clang and MSVC branches, every data symbol,
and one C function per procedure, with `tyg_loc` as `#line`, so compiler
messages and the debugger point at the front end's source. Locals and parameters
that the debug info names are declared with their names and C types built from
the debug types, so an optimized build shows the source variables in a debugger.

The C reads like hand-written C rather than a dump of the IR:
- An IR value used once in its block is printed inline in that use, and
  constants everywhere, so statements are expressions. A load does not move
  past a store, a call or an assignment. Values that nothing reads are dropped,
  along with their declarations.
- After `name = v;`, the rest of the block reads `v` as `name`, so the C reads
  the variable the way the source does (`v = v ^ (v << 13)`). This applies to a
  local whose address is never taken and that has the value's size. A local
  with no source name that is assigned once and read once in the same block is
  replaced by its expression, so a value passed through temporaries reads as
  one expression.
- `tyg_local_typed(p, type)` declares a local with its C type. A load or store
  of the whole local is a plain assignment.
- The structured builders `tyg_if`/`tyg_if_not`/`tyg_else`/`tyg_endif`,
  `tyg_loop`/`tyg_break`/`tyg_continue`/`tyg_endloop` and
  `tyg_switch_begin`/`tyg_case`/`tyg_default`/`tyg_endswitch` build the same
  blocks and branches as hand-built ones, and the C prints them as `if`/`else`,
  `for (;;)`, `break`, `continue` and a `switch` with its cases inside. An `if`
  on a condition known at build time prints only the part that runs. A
  construct that does not nest, or a break out of an outer loop, falls back to
  labels and `goto`. A goto to the label right after it is dropped, and so is a
  label nothing names. An entry block that is filled last is still printed
  first.
- `tyg_type_name(m, record, name, field_names)` gives a record a C name and
  field names (`struct Point`, `->x`). A name already taken gets a suffix
  (`Point_2`).

These are hints only. The IR's meaning never depends on them, and the native
backends ignore them.

The C keeps the IR's meaning under an optimizing compiler: integer arithmetic is
done in unsigned types so it wraps, shift counts are masked as the lowerings mask
them, scalar loads and stores go through `memcpy` unless volatile (the
`TYG_LD`/`TYG_ST` macros; TinyCC gets a direct access instead, it has no
aliasing rules and calls memcpy), aggregates move through `memcpy` helpers, and a procedure that calls a `TYG_SIG_RETURNS_TWICE`
function makes its accesses volatile. The IR has no pointer type, but the C
declares the values that hold pointers as `char *`, inferred per procedure: a
Lea, a pointer parameter or call result, and the values used as an address that
come from a load, a copy, `p + i`, `p - i` or a select of two pointers. Pointer
arithmetic and loads keep the provenance an optimizing compiler's alias analysis
needs (an integer cast back to a pointer loses it, and with it the hoisting and
vectorizing of loops); where a pointer meets an integer it is cast. The memcpy is
`__builtin_memcpy` on gcc and clang, which `_FORTIFY_SOURCE` does not turn into a
`__memcpy_chk` call. What the IR leaves undefined stays
undefined in C too: division by zero, `INT_MIN / -1`, and a float to integer
conversion out of range.

A byte copy may alias every other access, which costs an optimizing compiler
the hoisting of loads out of loops. A front end that knows the type of the
memory tags the address with it: `tyg_at_type(a, t)` for a scalar, a pointer or
a whole record (a `tyg_copy` between two places of that type becomes a struct
assignment), `tyg_at_field(a, record, k)` for one field and
`tyg_at_elem(a, array, i)` for one element, which also add the offset. Applied
to an address already typed as that record or array they extend the path, so
the C accesses `((struct tyg_r3 *)p)->f1.f0` (or the names `tyg_type_name` gave), and the C compiler may assume, as
C's rules let it, that accesses of different types do not alias: two paths
alias only where one's outermost type contains the other's. So a global reached
through a record of its own stays apart from the same type inside an array,
which lets a loop over the array keep the global in a register, while a write
through a pointer to the inner type still aliases it. The front end tags memory
it always accesses as that type; memory it reinterprets stays untagged, or is
tagged through one record whose fields overlap, which the C prints as a union.
The native backends ignore the tags.

Not in the C output: `TYG_STACK_POINTER` and `TYG_STACK_RESTORE` (C cannot move
the stack pointer; the procedure fails), 4 byte data relocations (C has no
constant expression for a truncated address), and `tyg_data_reloc` into
nothing but 8 byte slots. Named sections, aliases, common symbols, weak and the
visibilities become attributes the C compiler decides on. A symbol whose name
is not a C identifier gets an `asm` label on gcc and clang and a linker
alternatename pragma on MSVC.
A host that wants its own headers in the same translation unit calls
`tyg_c_header(m, text)`: the text is printed verbatim after the prelude and before
everything tinygen declares, several calls in order (typically
`#include "runtime.h"`). The symbols that text declares get `TYG_SYM_DECLARED`:
the C then has no prototype, extern declaration or asm label for them and uses
their names as they are, so a name that is not a C identifier, or that tinygen
reserves (keywords, libc and math names, `tyg_*`), makes `tyg_symbol` fail, and
defining such a symbol in the module fails too. A call passes the arguments
converted to the types of its `tyg_sig` (a pointer as `void *`, which C converts to
the header's typed parameters) and casts the result; that the header and the
`tyg_sig` agree is the host's job. Object output ignores both.

TinyCC takes the gcc branch of the prelude, with fallbacks for what it lacks:
`abort()` for the traps, byte swaps in C, `<stdatomic.h>` for the atomics, and
the `asm` label prefixed from `__leading_underscore`. Thread-local data still
needs a TinyCC target that supports it (not its in-memory `-run`).

On this Mac the bench module of 2000 procedures takes 43 ms to write as C
against 40 ms as a native object; clang then needs 1.3 s at `-O0` and 2.7 s at
`-O2` for the 300k lines, which is the trade the two outputs are for.

`sh test/run.sh` writes `test/cgen.c`, smoke, abi and bench as C, compiles them
with the system compiler at `-O0` and `-O2` and under UBSan, and runs the same
mains against the C and the native object. The generated files are left in
`build/test/obj/*-c.c`.

## Tests

One command builds the generators, emits the objects of every target, and runs
the tests the host can run. It lists the targets it skips:

    test/run.sh

The generators are built with `TG_CHECK_IR`, which checks the IR after each
pass in `tyg_proc_end`. `--docker` also runs the Linux targets in Docker. The objects are deterministic,
so `--save DIR` before a change and `--compare DIR` after it check that the
change leaves every object byte-identical.

To run one test by hand: `test/smoke.c` and `test/abi.c` build objects through the public API for a target:
`linux-x64` (the default), `linux-arm64`, `macos-arm64`, `macos-x64` or `windows`.
`test/smoke.c` also takes `windows-dwarf` (DWARF instead of CodeView, for gdb),
and `test/abi.c` takes `nopic` after the target. Their `_main.c` halves call the
functions and check the results; link them on a machine of the target.

    mkdir -p build
    cc -std=c99 -Iinclude test/smoke.c src/tinygen.c -o build/smoke
    build/smoke build/smoke.o macos-arm64
    cc test/smoke_main.c build/smoke.o -o build/smoke_run && build/smoke_run

    cc -std=c99 -Iinclude test/abi.c src/tinygen.c -o build/abi
    build/abi build/abi.o linux-arm64
    cc test/abi_main.c build/abi.o -o build/abi_run -lpthread -lm && build/abi_run   # on arm64 Linux

Each prints `all passed`. On macOS, `cc -arch x86_64` links a `macos-x64`
object and runs it under Rosetta. On Windows, `test/WINDOWS.md` has the
commands for MSVC, clang-cl and MinGW, and the debugger checks.

## Benchmark

`test/bench.c` builds many small procedures with loops, calls, float math,
aggregate copies, scopes and debug info, and times each phase. It takes the
output path, the target, the number of procedures and the number of data symbols.

    cc -std=c99 -O2 -Iinclude test/bench.c src/tinygen.c -o build/bench
    build/bench build/bench.o macos-arm64 40000 50000

or on Windows:

    cl /std:c11 /O2 /Iinclude test\bench.c src\tinygen.c /Fobuild\ /Fe:build\bench.exe
    build\bench.exe build\bench.obj windows 40000 50000

Its object is deterministic: a change that should not alter the output can be
checked by comparing the objects before and after it. Write both to the same
path, since the object records the path it was written to.

## License

tinygen is under the zlib license; see `LICENSE`. You may use it for any
purpose, including in closed-source and commercial products, and you need not
mention it in binaries you ship.
