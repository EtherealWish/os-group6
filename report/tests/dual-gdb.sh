#!/usr/bin/env bash
set -euo pipefail
export PATH=/opt/riscv/riscv64-unknown-elf-toolchain-10.2.0-2020.12.8-x86_64-linux-ubuntu14/bin:/opt/qemu-4.1.1/bin:/usr/bin:/bin
cd "$(dirname "$(realpath "$0")")/../../code"
logs=$(realpath ../report/tests)
qemu_debug=${QEMU_DEBUG:-"$HOME/src/qemu-4.1.1/riscv64-softmmu/qemu-system-riscv64"}
port=${LAB5_GDB_PORT:-12345}
scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
make -j4
ecall=$(riscv64-unknown-elf-objdump -d --disassemble=sys_putc obj/__user_exit.out | awk '$NF == "ecall" && !found {sub(":", "", $1); print "0x" $1; found=1}')
sret=$(riscv64-unknown-elf-objdump -d bin/kernel | awk '$NF == "sret" && !found {sub(":", "", $1); print "0x" $1; found=1}')
dispatch=$(riscv64-unknown-elf-nm bin/kernel | awk '$3 == "syscall" {print "0x" $1}')
resume=$(printf '0x%x' "$((ecall + 4))")
test -n "$ecall" && test -n "$sret"
cat > "$scratch/host.gdb" <<HOST
set pagination off
set debuginfod enabled off
set confirm off
break generate_exception if excp == 8 && ctx->base.pc_next == $ecall
commands
  silent
  printf "HOST TCG ecall: pc=%#lx exception=%d\n", ctx->base.pc_next, excp
  bt 4
  disable 1
  continue
end
break riscv_cpu_do_interrupt if cs->exception_index == 8 && ((RISCVCPU *)cs)->env.pc == $ecall
commands
  silent
  set \$envp = &((RISCVCPU *)cs)->env
  printf "HOST ecall runtime: pc=%#lx priv=%d medeleg=%#lx\n", \$envp->pc, \$envp->priv, \$envp->medeleg
  disable 2
  continue
end
break target/riscv/cpu_helper.c:564 if env->sepc == $ecall && env->scause == 8
commands
  silent
  printf "HOST delegated to S: scause=%lu sepc=%#lx stvec=%#lx prior_pc=%#lx\n", env->scause, env->sepc, env->stvec, env->pc
  disable 3
  continue
end
break trans_sret if ctx->base.pc_next == $sret
commands
  silent
  printf "HOST TCG sret: pc=%#lx\n", ctx->base.pc_next
  bt 3
  disable 4
  continue
end
break helper_sret if env->sepc == $resume
commands
  silent
  printf "HOST sret runtime: priv=%d SPP=%lu SPIE=%lu sepc=%#lx\n", env->priv, (env->mstatus >> 8) & 1, (env->mstatus >> 5) & 1, env->sepc
  disable 5
  continue
end
break target/riscv/op_helper.c:101 if env->sepc == $resume
commands
  silent
  printf "HOST sret completed: priv=%d sepc=%#lx\n", env->priv, env->sepc
  disable 6
  continue
end
run
HOST
cat > "$scratch/guest.gdb" <<GUEST
set pagination off
set confirm off
set remotetimeout 8
file bin/kernel
target remote localhost:$port
add-symbol-file obj/__user_exit.out
hbreak *$ecall
continue
printf "GUEST ecall: pc=%#lx syscall=%lu arg=%lu sp=%#lx\n", \$pc, \$a0, \$a1, \$sp
x/i \$pc
set \$user_sp = \$sp
delete
si
if (unsigned long)\$pc < (unsigned long)&__alltraps || (unsigned long)\$pc >= (unsigned long)&__trapret
    echo Unexpected ecall target\n
    quit 1
end
printf "GUEST trap entry: pc=%#lx scause=%lu sepc=%#lx\n", \$pc, \$scause, \$sepc
hbreak *$dispatch
continue
set \$tf = current->tf
printf "GUEST syscall: kernel_sp=%#lx saved_user_sp=%#lx EPC=%#lx SPP=%lu\n", \$sp, \$tf->gpr.sp, \$tf->epc, (\$tf->status >> 8) & 1
if \$tf->epc != $resume || \$tf->gpr.sp != \$user_sp || (\$tf->status & 0x100) != 0
    quit 2
end
delete
hbreak *$sret if \$sepc == $resume
continue
printf "GUEST sret: pc=%#lx sepc=%#lx sscratch=%#lx a0=%ld\n", \$pc, \$sepc, \$sscratch, \$a0
x/i \$pc
if \$a0 != 0 || \$sp != \$user_sp
    quit 3
end
delete
si
printf "GUEST resumed: pc=%#lx a0=%ld sp=%#lx\n", \$pc, \$a0, \$sp
if \$pc != $resume || \$a0 != 0 || \$sp != \$user_sp
    quit 4
end
detach
GUEST
printf 'ecall=%s sret=%s resume=%s\n' "$ecall" "$sret" "$resume"
set +e
timeout 40s gdb -nx -batch -x "$scratch/host.gdb" --args "$qemu_debug" \
    -machine virt -nographic -bios default -device loader,file=bin/ucore.img,addr=0x80200000 \
    -serial "file:$logs/gdb-serial.log" -monitor none -gdb "tcp:127.0.0.1:$port" -S \
    > "$logs/gdb-host.log" 2>&1 &
host=$!
timeout 30s riscv64-unknown-elf-gdb -nx -batch -x "$scratch/guest.gdb" > "$logs/gdb-guest.log" 2>&1
guest_status=$?
wait "$host"
host_status=$?
printf 'guest_gdb_exit=%s host_gdb_exit=%s\n' "$guest_status" "$host_status" | tee "$logs/gdb-status.log"
cat "$logs/gdb-host.log" "$logs/gdb-guest.log"
[ "$guest_status" -eq 0 ] && [ "$host_status" -eq 0 ] &&
    grep -q 'HOST TCG ecall' "$logs/gdb-host.log" &&
    grep -q 'HOST delegated to S' "$logs/gdb-host.log" &&
    grep -q 'HOST TCG sret' "$logs/gdb-host.log" &&
    grep -q 'HOST sret completed: priv=0' "$logs/gdb-host.log" &&
    grep -q 'GUEST resumed' "$logs/gdb-guest.log"
