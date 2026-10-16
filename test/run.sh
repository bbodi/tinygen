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

# TG_CHECK_IR checks the IR after each pass; it does not change the objects.
echo "building the generators"
$cc -std=c99 -Wall -DTG_CHECK_IR -Iinclude test/smoke.c src/tinygen.c -o "$out/smoke"
$cc -std=c99 -Wall -DTG_CHECK_IR -Iinclude test/abi.c src/tinygen.c -o "$out/abi"
$cc -std=c99 -O2 -Wall -DTG_CHECK_IR -Iinclude test/bench.c src/tinygen.c -o "$out/bench"

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

os=$(uname -s)
arch=$(uname -m)
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
