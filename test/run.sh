#!/bin/sh
# Builds the test generators, emits the objects of every target, and runs the tests the
# host can run: the native target, and macos-x64 under Rosetta on arm64 macOS. It reports
# the targets it skips.
#
#   test/run.sh                  run the tests
#   test/run.sh --docker         also run the Linux targets in Docker (gcc image)
#   test/run.sh --save DIR       also copy every emitted object into DIR
#   test/run.sh --compare DIR    also check that every emitted object matches DIR
#
# The objects are deterministic, so --save before a change and --compare after it check
# that the change leaves the output byte-identical. Run it from the repository root.
set -eu

docker=0
save=""
compare=""
while [ $# -gt 0 ]; do
	case "$1" in
	--docker) docker=1 ;;
	--save) save="$2"; shift ;;
	--compare) compare="$2"; shift ;;
	*) echo "usage: test/run.sh [--docker] [--save DIR] [--compare DIR]" >&2; exit 2 ;;
	esac
	shift
done

out=${TG_TEST_OUT:-build/test}
mkdir -p "$out/obj"
cc=${CC:-cc}
failed=0
ran=""
skipped=""

fail() {
	echo "FAILED: $1"
	failed=1
}

os=$(uname -s)
arch=$(uname -m)
case "$os-$arch" in
Darwin-arm64) host=macos-arm64 ;;
Darwin-x86_64) host=macos-x64 ;;
Linux-x86_64) host=linux-x64 ;;
Linux-aarch64) host=linux-arm64 ;;
*) host="" ;;
esac

# TG_CHECK_IR checks the IR after each pass; it does not change the objects.
echo "building the generators"
$cc -std=c99 -Wall -DTG_CHECK_IR -Iinclude test/smoke.c src/tinygen.c -o "$out/smoke"
$cc -std=c99 -Wall -DTG_CHECK_IR -Iinclude test/abi.c src/tinygen.c -o "$out/abi"
$cc -std=c99 -O2 -Wall -DTG_CHECK_IR -Iinclude test/bench.c src/tinygen.c -o "$out/bench"
$cc -std=c99 -Wall -DTG_CHECK_IR -Iinclude test/cgen.c src/tinygen.c -o "$out/cgen"

# Every object, written to a fixed path, since an object records the path it was written to.
echo "emitting the objects"
emit() { # name, generator and its arguments
	name=$1
	shift
	"$@" > "$out/obj/$name.log" 2>&1 || { cat "$out/obj/$name.log"; fail "emit $name"; return 0; }
}
for t in linux-x64 linux-arm64 macos-arm64 macos-x64 windows windows-dwarf; do
	emit "smoke-$t" "$out/smoke" "$out/obj/smoke-$t.o" "$t"
done
for t in linux-x64 linux-arm64 macos-arm64 macos-x64 windows; do
	emit "abi-$t" "$out/abi" "$out/obj/abi-$t.o" "$t"
	emit "abi-$t-nopic" "$out/abi" "$out/obj/abi-$t-nopic.o" "$t" nopic
done
for t in linux-x64 linux-arm64 macos-arm64 macos-x64 windows; do
	emit "bench-$t" "$out/bench" "$out/obj/bench-$t.o" "$t" 2000 2000
done
# the C output is text, not an object: --save and --compare leave it out. A generator
# exits non-zero when C mode refuses a procedure.
rm -f "$out"/obj/*-c.c "$out"/obj/cgen-c-*.c "$out/obj/cgen-$host.o"
emit cgen-c "$out/cgen" "$out/obj/cgen-c.c" c
[ -z "$host" ] || emit "cgen-$host" "$out/cgen" "$out/obj/cgen-$host.o" "$host"
emit smoke-c "$out/smoke" "$out/obj/smoke-c.c" c
emit abi-c "$out/abi" "$out/obj/abi-c.c" c
emit bench-c "$out/bench" "$out/obj/bench-c.c" c 2000 2000
# x86-64 C, for its x87 long double, when arm64 macOS can run it under Rosetta
if [ "$host" = macos-arm64 ] && arch -x86_64 /usr/bin/true 2> /dev/null; then
	emit cgen-c-macos-x64 "$out/cgen" "$out/obj/cgen-c-macos-x64.c" c-macos-x64
fi

if [ -n "$save" ]; then
	mkdir -p "$save"
	cp "$out"/obj/*.o "$save"/
	echo "saved the objects in $save"
fi
if [ -n "$compare" ]; then
	differ=0
	for o in "$out"/obj/*.o; do
		b=$(basename "$o")
		if [ ! -f "$compare/$b" ]; then
			echo "missing from $compare: $b"
			differ=1
		elif ! cmp -s "$o" "$compare/$b"; then
			echo "differs from $compare: $b"
			differ=1
		fi
	done
	if [ $differ -eq 0 ]; then echo "every object matches $compare"; else fail "compare with $compare"; fi
fi

# run_native target arch_flags libs: links each test against its object, at -O0 and -O2
run_native() {
	t=$1
	flags=$2
	for test in smoke abi; do
		for o in -O0 -O2; do
			bin="$out/$test-$t$o"
			# shellcheck disable=SC2086
			if ! $cc $o $flags "test/${test}_main.c" "$out/obj/$test-$t.o" -o "$bin" -lpthread -lm > "$bin.log" 2>&1; then
				cat "$bin.log"
				fail "link $test $t $o"
				continue
			fi
			"$bin" > "$bin.log" 2>&1 || { cat "$bin.log"; fail "run $test $t $o"; }
		done
	done
	ran="$ran $t"
}

case "$os-$arch" in
Darwin-arm64)
	run_native macos-arm64 "-arch arm64"
	if arch -x86_64 /usr/bin/true 2> /dev/null; then
		run_native macos-x64 "-arch x86_64"
	else
		skipped="$skipped macos-x64(no Rosetta)"
	fi
	;;
Darwin-x86_64) run_native macos-x64 "" ;;
Linux-x86_64) run_native linux-x64 "" ;;
Linux-aarch64) run_native linux-arm64 "" ;;
*) echo "no native target for $os-$arch" ;;
esac

# run_cgen name flags input: compiles cgen_main.c with the C output or the host object and
# runs it. A warning about the C output fails it too.
run_cgen() {
	bin="$out/cgen-$1"
	# shellcheck disable=SC2086
	if ! $cc -std=c99 -Wall -Wextra $2 "$3" test/cgen_main.c -o "$bin" -lm > "$bin.log" 2>&1 ||
		{ case "$3" in *.c) [ -s "$bin.log" ] ;; *) false ;; esac; }; then
		cat "$bin.log"
		fail "compile cgen $1"
		return 0
	fi
	if "$bin" > "$bin.log" 2>&1; then echo "cgen $1: $(tail -n 1 "$bin.log")"; else cat "$bin.log"; fail "run cgen $1"; fi
}
# check_c_names file: the debug variables of cgen.c's build_debug are C variables of their
# debug types, the bitfield and packed records stay byte arrays, and the C compiles with -g.
check_c_names() {
	for decl in '^	\(TYG_UNUSED \)\{0,1\}int64_t total;$' '^	\(TYG_UNUSED \)\{0,1\}int32_t int_1;$' '^	\(TYG_UNUSED \)\{0,1\}int64_t v1_1;$' '^	\(TYG_UNUSED \)\{0,1\}int64_t total_1;$' \
		'^	\(TYG_UNUSED \)\{0,1\}_Bool flag;$' '^	\(TYG_UNUSED \)\{0,1\}i16 small;$' '^int64_t dbg_scalars(int64_t a, int32_t b) {$' '^	\(TYG_UNUSED \)\{0,1\}int64_t a_1;$' \
		'^int64_t dbg_point(struct tyg_r[0-9]* pt) {$' '^	\(TYG_UNUSED \)\{0,1\}struct Node node;$' '^	\(TYG_UNUSED \)\{0,1\}struct Node \*next;$' \
		'^	\(TYG_UNUSED \)\{0,1\}enum Color color;$' '^	\(TYG_UNUSED \)\{0,1\}int32_t arr\[3\];$' '^	\(TYG_UNUSED \)\{0,1\}Count count;$' '^typedef int64_t Count;$'; do
		grep -q "$decl" "$1" || fail "cgen C names: no line matches $decl"
	done
	# a pointer to struct Bits declares it, incomplete
	for decl in 'struct Bits {' 'struct Bits bits' 'struct Packed'; do
		! grep -q "$decl" "$1" || fail "cgen C names: '$decl', yet it has no C form"
	done
	# typed memory: tagged accesses are typed lvalues, a tagged copy a struct assignment (operands
	# may be folded expressions, see tg_c_fold)
	for decl in '= (float)((struct tyg_r[0-9]* \*)(.*))->f1;$' '^	((struct tyg_r[0-9]* \*)(.*))->f3 = (int64_t)' \
		'^	(\*(struct tyg_r[0-9]* \*)(.*)) = (\*(struct tyg_r[0-9]* \*)(.*));$' '^	((struct tyg_r[0-9]* \*)(.*))->u.l2.f2 = (int64_t)' \
		'^	((struct tyg_r[0-9]* \*)(.*&tg_t_glob.*))->f0.f0 = (int32_t)' \
		'^	((struct tyg_r[0-9]* \*)(.*))->f1.f3 = (int64_t)' '^	((struct tyg_r[0-9]* \*)(.* - 32))->f2\[0\] = ((struct tyg_r[0-9]* \*)(.* - 8))->f1;$'; do
		grep -q "$decl" "$1" || fail "cgen typed memory: no line matches $decl"
	done
	# typed locals (tyg_local_typed): declared with their C types, read and written whole plainly
	for decl in '^	TYG_UNUSED TYG_ALIGNED(8) int64_t l[0-9]*;$' '^	TYG_UNUSED TYG_ALIGNED(8) void \*l[0-9]*;$' '^	TYG_UNUSED TYG_ALIGNED(8) double l[0-9]*;$' \
		'^	TYG_UNUSED TYG_ALIGNED(4) int32_t l[0-9]*;$' '^	l[0-9]* = (int64_t)v[0-9]*;$' '(uint64_t)l[0-9]*' '^	l[0-9]* = (void \*)'; do
		grep -q "$decl" "$1" || fail "cgen typed locals: no line matches $decl"
	done
	# expressions (tg_c_fold): tg_typed_locals returns one expression of its locals and constants
	grep -q '^	return (int64_t).*l[0-9]*) \* UINT64_C(0x3e8).*(double)l[0-9]*.*(uint32_t)l[0-9]*)*;$' "$1" ||
		fail "cgen expressions: tg_typed_locals does not return one expression"
	# structured control flow: tg_struct prints as for (;;), if/else, break and continue; the break
	# out of the outer loop is the one goto
	body=$(sed -n '/^int64_t tg_struct(int64_t p0) {$/,/^}$/p' "$1")
	for decl in '	for (;;) {' '	} else {' '	break;' '	continue;' '	if (1) {' '	if (!'; do
		printf '%s\n' "$body" | grep -qF "$decl" || fail "cgen structured: tg_struct has no '$decl'"
	done
	[ "$(printf '%s\n' "$body" | grep -c 'goto ')" = 1 ] || fail "cgen structured: tg_struct has other gotos than the outer break"
	# an entry block filled last prints first (tg_c_entry_first)
	! sed -n '/^int64_t tg_entry_last(int64_t p0) {$/,/^}$/p' "$1" | grep -q 'goto ' ||
		fail "cgen entry: tg_entry_last still jumps to its entry block"
	# no goto to the label right after it
	! awk '/^\t*goto B[0-9]+;$/ { g = $1 " " $2; sub(/^.*goto /, "", g); sub(/;$/, "", g); next_is = g ":;"; next }
		next_is != "" && $0 == next_is { found = 1 } { next_is = "" } END { exit !found }' "$1" ||
		fail "cgen labels: a goto to the label right after it"
	# tyg_switch: one C switch, its cases and default
	grep -q '^	switch (.*) { case 0ULL: goto B[0-9]*; case 5ULL: goto B[0-9]*; case 18446744073709551613ULL: goto B[0-9]*; case 9223372036854775808ULL: goto B[0-9]*; default: goto B[0-9]*; }$' "$1" ||
		fail "cgen switch: no C switch for tg_switch"
	if $cc -std=c99 -Wall -Wextra -g -O2 -c "$1" -o "$out/cgen-c-g.o" > "$out/cgen-c-g.log" 2>&1 && [ ! -s "$out/cgen-c-g.log" ]; then
		echo "cgen C names: checked, compiles with -g -O2"
	else
		cat "$out/cgen-c-g.log"
		fail "compile cgen C with -g -O2"
	fi
}
c_start=$(date +%s)
if [ -f "$out/obj/cgen-c.c" ]; then
	echo "checking the C output"
	run_cgen c-O0 -O0 "$out/obj/cgen-c.c"
	run_cgen c-O2 -O2 "$out/obj/cgen-c.c"
	ubsan="-O1 -fsanitize=undefined -fno-sanitize-recover=undefined"
	echo "int main(void) { return 0; }" > "$out/ubsan-probe.c"
	# shellcheck disable=SC2086
	if $cc $ubsan "$out/ubsan-probe.c" -o "$out/ubsan-probe" > /dev/null 2>&1; then
		run_cgen c-ubsan "$ubsan" "$out/obj/cgen-c.c"
	else
		echo "cgen c-ubsan: skipped, $cc does not accept $ubsan"
		skipped="$skipped cgen-ubsan"
	fi
	[ ! -f "$out/obj/cgen-$host.o" ] || run_cgen "$host-O0" -O0 "$out/obj/cgen-$host.o"
	check_c_names "$out/obj/cgen-c.c"
	if [ -f "$out/obj/cgen-c-macos-x64.c" ]; then
		run_cgen c-macos-x64-O0 "-arch x86_64 -O0" "$out/obj/cgen-c-macos-x64.c"
		run_cgen c-macos-x64-O2 "-arch x86_64 -O2" "$out/obj/cgen-c-macos-x64.c"
	fi
fi

# run_c_suite name main extra_flags: compiles the generator's C output, where a warning fails
# it, at -O0 and -O2, links it with its main like run_native does, and runs it. bench has no
# main: its C is compiled only.
run_c_suite() {
	src="$out/obj/$1-c.c"
	[ -f "$src" ] || return 0
	for o in -O0 -O2; do
		bin="$out/$1-c$o"
		# shellcheck disable=SC2086
		if ! $cc -std=c99 -Wall -Wextra $o -c "$src" -o "$bin.o" > "$bin.log" 2>&1 || [ -s "$bin.log" ]; then
			cat "$bin.log"
			fail "compile $1 c$o"
			continue
		fi
		if [ -z "$2" ]; then echo "$1 c$o: compiled"; continue; fi
		# shellcheck disable=SC2086
		if ! $cc $o $3 "$2" "$bin.o" -o "$bin" -lpthread -lm > "$bin.log" 2>&1; then
			cat "$bin.log"
			fail "link $1 c$o"
			continue
		fi
		if "$bin" > "$bin.log" 2>&1; then echo "$1 c$o: $(tail -n 1 "$bin.log")"; else cat "$bin.log"; fail "run $1 c$o"; fi
	done
}
if [ -f "$out/obj/smoke-c.c" ]; then
	echo "checking smoke, abi and bench as C"
	run_c_suite smoke test/smoke_main.c ""
	run_c_suite abi test/abi_main.c -DTG_FAR_COUNT=2500 # see build_far in abi.c
	run_c_suite bench "" ""
fi
echo "the C checks took $(($(date +%s) - c_start)) s"

if [ $docker -eq 1 ]; then
	for t in linux-arm64 linux-x64; do
		case "$t" in linux-arm64) plat=linux/arm64 ;; *) plat=linux/amd64 ;; esac
		case "$ran" in *" $t"*) continue ;; esac
		echo "running $t in Docker"
		if docker run --rm --platform "$plat" -v "$PWD:/src" -w /src -e CC=gcc -e "TG_TEST_OUT=$out/docker-$t" \
			gcc:14 sh test/run.sh > "$out/docker-$t.log" 2>&1; then
			ran="$ran $t(docker)"
		else
			cat "$out/docker-$t.log"
			fail "docker $t"
		fi
	done
fi
for t in linux-x64 linux-arm64; do
	case "$ran" in *" $t"*) ;; *) skipped="$skipped $t" ;; esac
done
skipped="$skipped windows(see test/WINDOWS.md)"

echo "ran:    $ran"
echo "skipped:$skipped"
if [ $failed -ne 0 ]; then
	echo "some tests FAILED"
	exit 1
fi
echo "all passed"
