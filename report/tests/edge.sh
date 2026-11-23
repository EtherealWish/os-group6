#!/usr/bin/env bash
set -euo pipefail
export PATH=/opt/riscv/riscv64-unknown-elf-toolchain-10.2.0-2020.12.8-x86_64-linux-ubuntu14/bin:/opt/qemu-4.1.1/bin:/usr/bin:/bin
logs=$(dirname "$(realpath "$0")")
scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
cp -a "$logs/../../code/." "$scratch"
cd "$scratch"
make clean
cp "$logs/edge.c" user/edge.c
python3 "$logs/inject-edge.py"
set -x
make -j4 build-edge
timeout 25s make qemu > "$logs/edge-serial.log" 2>&1 && status=0 || status=$?
cat "$logs/edge-serial.log"
printf 'edge_qemu_exit=%s\n' "$status"
[ "$status" -eq 0 ]
grep -q 'EDGE: 4 heap and' "$logs/edge-serial.log"
grep -q 'EDGE: fork isolation' "$logs/edge-serial.log"
grep -q 'init check memory pass.' "$logs/edge-serial.log"
