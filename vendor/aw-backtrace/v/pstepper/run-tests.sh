#!/bin/sh
#
# Ad-hoc / stress pstepper test runner. For the normal build+test cycle use
# `ninja check` instead -- see testrunner.rb. This script stays for running
# one test (or a tight -n loop of one) without going through ninja.
#
# Builds (./genbuild.rb ninja), then runs each guest test program under
# qemu-x86_64 with the plugin. Each program self-checks and exits nonzero on
# failure, so `set -e` is the whole harness.
#
#   ./run-tests.sh                              build + run each once
#   ./run-tests.sh pstepper_thread_test         just one
#   ./run-tests.sh -n 20 pstepper_thread_test   run it 20x (stress)
#
# qemu-x86_64: $QEMU_X86_64, else $QEMU_SRC/build/qemu-x86_64, else
# ../qemu/build/qemu-x86_64.
set -eu

runs=1
if [ "${1:-}" = "-n" ]; then
    runs=$2
    shift 2
fi
tests=${*:-"pstepper_test pstepper_signal_test pstepper_thread_test pstepper_xsave_test"}

qemu=${QEMU_X86_64:-${QEMU_SRC:-../qemu}/build/qemu-x86_64}
plugin=./pstepper_plugin.so

./genbuild.rb ninja

if [ ! -x "$qemu" ]; then
    echo "run-tests.sh: no qemu-x86_64 at $qemu (set QEMU_X86_64)" >&2
    exit 1
fi

fail=0
for t in $tests; do
    i=1
    while [ "$i" -le "$runs" ]; do
        printf '=== %s (%d/%d) ===\n' "$t" "$i" "$runs"
        if ! "$qemu" -plugin "$plugin" "./$t"; then
            fail=1
            break
        fi
        i=$((i + 1))
    done
done

[ "$fail" -eq 0 ] && echo "OK" || echo "FAILED"
exit "$fail"
