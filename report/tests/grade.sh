#!/usr/bin/env bash
set -u -o pipefail
export PATH=/opt/riscv/riscv64-unknown-elf-toolchain-10.2.0-2020.12.8-x86_64-linux-ubuntu14/bin:/opt/qemu-4.1.1/bin:/usr/bin:/bin
cd "$(dirname "$(realpath "$0")")/../../code"
export LAB5_GRADE_LOGS="$(realpath ../report/tests)"
mkdir -p "$LAB5_GRADE_LOGS/grade-serial"
scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
cat > "$scratch/capture" <<'CAPTURE'
#!/bin/sh
sh "$@"
status=$?
for serial in .*.log; do
    [ -s "$serial" ] || continue
    cp "$serial" "$LAB5_GRADE_LOGS/grade-serial/${serial#.}" || exit 1
done
exit "$status"
CAPTURE
chmod +x "$scratch/capture"
{
    set -x
    timeout 170s make grade GRADE_TMP="$scratch/source" SH="$scratch/capture"
    status=$?
    printf 'grade_exit=%s\n' "$status"
    exit "$status"
} 2>&1 | tee "$LAB5_GRADE_LOGS/grade.log"
