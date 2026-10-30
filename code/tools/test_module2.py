#!/usr/bin/env python3
"""Run module-two regression in a temporary copy (requires the lab toolchain)."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
LOGS = ROOT.parent / 'report' / 'tests'
LOGS.mkdir(parents=True, exist_ok=True)
QEMU = os.environ.get('QEMU', 'qemu-system-riscv64')
TEST = r'''
    /* Real traps must return to the very next instruction for both lengths. */
    asm volatile(".2byte 0x0000" ::: "memory");
    cprintf("[module2] illegal16 returned\n");
    asm volatile(".4byte 0x0000007b" ::: "memory");
    cprintf("[module2] illegal32 returned\n");
    asm volatile(".2byte 0x9002" ::: "memory");
    cprintf("[module2] breakpoint16 returned\n");
    asm volatile(".4byte 0x00100073" ::: "memory");
    cprintf("[module2] breakpoint32 returned\n");
    {
        extern void interrupt_handler(struct trapframe *);
        extern void exception_handler(struct trapframe *);
        struct trapframe tf;
        const uintptr_t causes[] = {0, 1, 3, 4, 7, 8, 9, 11, 15};
        memset(&tf, 0, sizeof(tf));
        tf.epc = 0x1234;
        for (unsigned i = 0; i < sizeof(causes) / sizeof(causes[0]); i++) {
            tf.cause = causes[i] | ((uintptr_t)1 << 63);
            interrupt_handler(&tf);
            if (tf.epc != 0x1234) {
                panic("non-timer interrupt changed epc");
            }
        }
        tf.cause = 20;
        exception_handler(&tf);
        if (tf.epc != 0x1234) {
            panic("fallback exception changed epc");
        }
        cprintf("[module2] dispatch fallbacks returned\n");
    }
'''
with tempfile.TemporaryDirectory(prefix='lab3-module2-') as tmp:
    tree = Path(tmp) / 'code'
    shutil.copytree(ROOT, tree, ignore=shutil.ignore_patterns('bin', 'obj', '.git', '.qemu*'))
    init = tree / 'kern/init/init.c'
    source = init.read_text()
    position = source.rindex('    clock_init();')
    source = source[:position] + TEST + '\n' + source[position:]
    source = '#include <assert.h>\n' + source
    init.write_text(source)
    build = subprocess.run(['make', '-j2'], cwd=tree, capture_output=True, text=True)
    (LOGS / 'module2-build.log').write_text(build.stdout + build.stderr)
    if build.returncode:
        raise SystemExit('FAIL: build; see report/tests/module2-build.log')
    try:
        run = subprocess.run([QEMU, '-machine', 'virt', '-nographic', '-bios', 'default',
                              '-device', 'loader,file=bin/ucore.img,addr=0x80200000',
                              '-monitor', 'none', '-no-reboot'],
                             cwd=tree, capture_output=True, text=True, timeout=25)
    except subprocess.TimeoutExpired as exc:
        (LOGS / 'module2-qemu.log').write_bytes((exc.stdout or b'') + (exc.stderr or b''))
        raise SystemExit('FAIL: QEMU did not shut down in 25 seconds')
    output = run.stdout + run.stderr
    (LOGS / 'module2-qemu.log').write_text(output)
    checks = ['illegal16 returned', 'illegal32 returned', 'breakpoint16 returned',
              'breakpoint32 returned', 'dispatch fallbacks returned',
              'User software interrupt', 'Supervisor software interrupt',
              'Machine software interrupt', 'User timer interrupt', 'Machine timer interrupt',
              'User external interrupt', 'Supervisor external interrupt',
              'Machine external interrupt', 'Unknown interrupt: cause 15']
    for check in checks:
        assert check in output, 'Missing: ' + check
    assert run.returncode == 0, 'QEMU exit code: ' + str(run.returncode)
    assert output.count('100 ticks\n') == 10, 'Expected exactly ten timer reports'
    assert output.count('Illegal instruction at') == 2, 'Illegal instruction loop or missing trap'
    assert output.count('Breakpoint at') == 2, 'Breakpoint loop or missing trap'
    assert output.count('trapframe at') == 2, 'Expected two fallback frame dumps'
    assert 'panic' not in output.lower(), 'Kernel panic'
    print('PASS: illegal16/32, breakpoint16/32, all non-timer IRQ branches, fallback frames')
    print('PASS: 1000 real timer interrupts, exactly 10 reports, automatic SBI shutdown')