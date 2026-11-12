#!/bin/bash
set -uo pipefail
d=/mnt/d/os-lab-lab4-validation-0543e09d/report-materials-20261112
export PATH="$(echo /opt/riscv/*/bin):/opt/qemu-4.1.1/bin:$PATH"
cd "$d/integrated-code" || exit
{
  set -x
  cat /etc/os-release
  riscv64-unknown-elf-gcc --version
  qemu-system-riscv64 --version
  riscv64-unknown-elf-gdb --version
  make --version
  make -j4
  rc=$?
  echo build_exit=$rc
  if [ "$rc" -ne 0 ]; then exit "$rc"; fi
  python3 tools/inject.py "$PWD"
  rc=$?
  echo inject_exit=$rc
  if [ "$rc" -ne 0 ]; then exit "$rc"; fi
  timeout 45s sh -x tools/grade.sh -v
  rc=$?
  echo grade_exit=$rc
  if [ -f .qemu.out ]; then cat .qemu.out; fi
  exit "$rc"
} 2>&1 | tee "$d/integrated-validation.log"
exit "${PIPESTATUS[0]}"

