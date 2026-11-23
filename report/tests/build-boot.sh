#!/usr/bin/env bash
set -u
export PATH=/opt/riscv/riscv64-unknown-elf-toolchain-10.2.0-2020.12.8-x86_64-linux-ubuntu14/bin:/opt/qemu-4.1.1/bin:/usr/bin:/bin
cd "$(dirname "$(realpath "$0")")/../../code"
set -x
uname -r
cat /etc/os-release
riscv64-unknown-elf-gcc --version
riscv64-unknown-elf-ld --version | head -n 1
riscv64-unknown-elf-gdb --version | head -n 1
qemu-system-riscv64 --version
make --version | head -n 1
make clean
clean_status=$?
printf 'clean_exit=%s\n' "$clean_status"
if [ "$clean_status" -ne 0 ]; then exit "$clean_status"; fi
make -j4
build_status=$?
printf 'build_exit=%s\n' "$build_status"
if [ "$build_status" -ne 0 ]; then exit "$build_status"; fi
timeout 25s make qemu > ../report/tests/qemu.log 2>&1
boot_status=$?
printf 'qemu_exit=%s\n' "$boot_status"
exit "$boot_status"
