#!/usr/bin/env bash
set -euo pipefail
export PATH=/opt/riscv/riscv64-unknown-elf-toolchain-10.2.0-2020.12.8-x86_64-linux-ubuntu14/bin:/opt/qemu-4.1.1/bin:/usr/bin:/bin
logs=$(dirname "$(realpath "$0")")
scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
cp -a "$logs/../../code/." "$scratch"
cd "$scratch"

set -x
make clean
make -j4 build-waitkill
timeout 25s make qemu > "$logs/waitkill.log" 2>&1 && status=0 || status=$?
printf 'waitkill_qemu_exit=%s\n' "$status"
[ "$status" -eq 0 ]
grep -q 'kill parent ok.' "$logs/waitkill.log"
grep -q 'kill child1 ok.' "$logs/waitkill.log"
grep -q 'init check memory pass.' "$logs/waitkill.log"

make -j4 build-forktree
timeout 25s make qemu > "$logs/forktree.log" 2>&1 && status=0 || status=$?
printf 'forktree_qemu_exit=%s\n' "$status"
[ "$status" -eq 0 ]
grep -q "I am '1111'" "$logs/forktree.log"
grep -q 'init check memory pass.' "$logs/forktree.log"
