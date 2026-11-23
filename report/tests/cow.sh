#!/usr/bin/env bash
set -euo pipefail
export PATH=/opt/riscv/riscv64-unknown-elf-toolchain-10.2.0-2020.12.8-x86_64-linux-ubuntu14/bin:/opt/qemu-4.1.1/bin:/usr/bin:/bin
logs=$(dirname "$(realpath "$0")")
scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
cp -a "$logs/../../code/." "$scratch"
cd "$scratch"
make clean
cp "$logs/cow-kernel.c" kern/mm/cow_check.c
cp "$logs/cow-user.c" user/cowcheck.c
python3 - <<'PY'
from pathlib import Path

p = Path("kern/mm/pmm.c")
s = p.read_text()
anchor = "struct Page *alloc_pages(size_t n)\n{"
assert s.count(anchor) == 1
p.write_text(s.replace(anchor, "int cow_fail_page_after = -1;\n" + anchor + """
    if (cow_fail_page_after >= 0 && cow_fail_page_after-- == 0)
    {
        return NULL;
    }"""))

p = Path("kern/process/proc.c")
s = p.read_text()
anchor = "static int\ninit_main(void *arg)\n{"
assert s.count(anchor) == 1
p.write_text(s.replace(anchor, "void cow_check(void);\n\n" + anchor + "\n    cow_check();"))
PY
set -x
make -j4 build-cowcheck
timeout 25s make qemu > "$logs/cow-serial.log" 2>&1 && status=0 || status=$?
cat "$logs/cow-serial.log"
printf 'cow_qemu_exit=%s\n' "$status"
[ "$status" -eq 0 ]
grep -q 'COW: shared PPNs' "$logs/cow-serial.log"
grep -q 'COW: store OOM' "$logs/cow-serial.log"
grep -q 'COW: parent and child stores isolated' "$logs/cow-serial.log"
grep -q 'init check memory pass.' "$logs/cow-serial.log"
