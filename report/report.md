# Lab 5 用户进程实验报告

第 6 组 · 2026-11-22 · `lab5` · `D:\os-lab-lab5`

## 一、依据与范围

源码取自 2026 年课程发布页的 `lab5.zip`，解压目录为 `lab5_new`，SHA-256 为 `4dd863d539fc70eb6df24eaab2b45cda7cbf9c7d47817fbee20bda4926b42ae2`。指导书取自同一站点的 2026 版，按下表验收。浏览工具无法访问站点，Windows 的 `curl.exe -fLsS` 直接获取了发布页、ZIP 和指导书章节。 [源码发布](http://oslab.mobisys.top/lab2026/labcode.html) · [官方 ZIP](http://oslab.mobisys.top/lab2026/lab5.zip) · [Lab 5 练习](http://oslab.mobisys.top/lab2026/_book/lab5/lab5_2_1_exercises.html)

| 类型 | 内容 | 结果 |
| --- | --- | --- |
| 必做代码 | ① 用户态系统调用 ② 创建加载第一个用户进程 ③ 复制用户进程 ④ 退出、让出和终止 | 四个模块已贯通，课程评分 130/130 |
| 调试与问答 | 双层 GDB 观察 `ecall`、`sret` 和 QEMU TCG，说明用户程序预加载时机 | 主客体 GDB 均退出 0，分析见第三节 |
| 报告 | 整体流程、函数理解、规格和提示词、AI 辅助调试、OS 原理对照 | 本文及随附记录 |
| 扩展挑战 | COW、状态转换、测试及竞态分析 | 已实现页共享、写时分离和异常回收，见第七节 |

课程评分由官方 `make grade` 注入三个内核检查，再逐个构建并运行 13 个用户程序。每个程序的行为检查 7 分，公共启动和注入检查 3 分，共 130 分。单个用户程序故意触发异常时按其原有输出规则评分。 [指导书报告要求](http://oslab.mobisys.top/lab2026/_book/lab5/lab5_5_lab_requirement.html) · [课程评分脚本](../code/tools/grade.sh)

先读取小组 README、Lab 4 集成报告、代码以及 30/30 的集成与专项记录。主 worktree 的 `lab3` 有未提交内容，另一个 worktree 的 `lab4` 干净。Windows 上的 `git fetch origin` 连接 GitHub 443 超时，改用 `wsl -- git -C /mnt/d/os-lab fetch origin` 成功。远程有 `lab1` 至 `lab4` 和 `main`，因此在 `abcde63` 上创建本地 `lab5` 独立 worktree。DTB 内存探测、页映射更新、进程上下文构建、寄存器切换和调度扫描取自 Lab 4。Lab 5 框架使用 First-Fit 的自检和 `default_pmm_manager`，因此迁移同仓库 `lab2` 分支的 First-Fit 实现。实验新增的模块标记使用仓库 README 所列组长学号 2411870，迁移代码保留原有学号。

## 二、启动到用户进程回收

`kern_init` 初始化 PMM、Trap 和 VMM。 `proc_init` 建立 idle 和 init。调度切入 init 后创建 `user_main`，它从内核镜像选择用户 ELF，调用 `kernel_execve`。新地址空间与 Trap frame 准备好后，`forkrets → __trapret → sret` 进入用户 ELF 的 `_start`。之后用户程序以 `ecall` 请求服务，调度配合 `fork`、`yield`、`kill` 和 `wait` 推进所有待运行进程。最后由 init 回收孤儿及僵尸进程，校验进程表和内存回到基线。

| 模块 | 规格与实现位置 | 验收路径 |
| --- | --- | --- |
| 1 · 系统调用 | [trap.c](../code/kern/trap/trap.c) 处理 U 态 `ecall` 并将 `epc` 加 4。[内核 syscall.c](../code/kern/syscall/syscall.c) 从 `a0` 读取完整宽度的调用号，从 `a1–a5` 取参数，检查分发表上下界和空槽，返回值写入 `a0`。[用户 syscall.c](../code/user/libs/syscall.c) 用固定寄存器约束传参。[trapentry.S](../code/kern/trap/trapentry.S) 保存通用寄存器及 CSR，从 U 态陷入时换入内核栈，返回 U 态前将内核栈顶写回 `sscratch`，返回 S 态前清零。 | 注入的 `getpid` 和非法调用号断言、`hello`、`pgdir`、双层 GDB |
| 2 · 首次装载 | [proc.c](../code/kern/process/proc.c) 的 `alloc_proc` 初始化 PCB，`kernel_thread` 复用 `do_fork`，`init_main → user_main → kernel_execve` 启动默认 `exit` 或指定测试。 `load_icode` 检查 ELF 头、段和入口，按 `PT_LOAD` 权限映射，复制文件内容并清零 BSS 和新栈。初始帧设置 `sp=USTACKTOP`、`sepc=e_entry`、`SPP=0` 和 `SPIE=1`。 `do_execve` 先在旧根页表仍有效时完成新 ELF 装载，成功后切换 `satp`、刷新 TLB 和指令流，再释放旧 mm。 | 默认 QEMU、`testbss`、`badsegment`、`faultreadkernel`、临时副本的截断与有效 ELF |
| 3 · 复制进程 | [proc.c](../code/kern/process/proc.c) 的 `do_fork → copy_mm → copy_thread` 分配 PCB、内核栈和 Trap frame。普通 fork 在 [vmm.c](../code/kern/mm/vmm.c) 复制 VMA，在 [pmm.c](../code/kern/mm/pmm.c) 建立共享叶页映射，并以 COW 权限保护原可写用户页。 `CLONE_VM` 共用 mm 并增加引用。子进程的 `a0=0`，父进程得到子 PID。创建完整后才写 PID 哈希和亲属链并唤醒。 | `forktest`、`badarg`、`exit`、临时副本的父子数据隔离和逐点分配失败 |
| 4 · 进程控制 | [proc.c](../code/kern/process/proc.c) 的 `do_exit` 先切到启动根页表，递减或释放 mm，记录退出码后进入僵尸态，唤醒等待者并把子进程交给 init。 `do_wait` 支持指定子 PID 或任意子进程，检查写回地址，睡醒后重新查找，最后摘除关系与哈希并释放内核栈和 PCB。 `do_yield` 设置重调度标志，`do_kill` 设置 `PF_EXITING` 并唤醒可中断的等待者。用户态返回前处理退出标志和计时器触发的重调度。 | `yield`、`spin`、`exit`、`forktest` 和补充的等待与收养验证 |

页表权限以 ELF 段标志为准。RISC-V 的可写叶页带 `R`，用户栈只读写，内核映射保留 supervisor 权限。VMA 中目前只预映射栈顶四页，未映射的栈区会在用户访问时产生页故障。传给 `waitpid` 和 `exec` 的缓冲区必须通过区间、VMA 和实际 PTE 权限检查，包含跨页情况。

创建子地址空间时，VMA 先登记，出错时便能撤销已建映射和引用。全部映射准备成功后才提交父页表的 COW 权限。 `exit_mmap` 先撤销叶映射，`exit_range` 释放子页表后检查整个父页表，避免稀疏区间留下空表，或一个 VMA 提前回收另一个区间的父表。完全空闲的 SLOB 页归还 PMM，`kallocated` 统计 SLOB 活跃分配单元。init 同时核对物理空闲页和 SLOB 占用。 [pmm.c](../code/kern/mm/pmm.c) · [kmalloc.c](../code/kern/mm/kmalloc.c)

## 三、讲义问答与双层 GDB

### 用户程序何时被预加载

构建时，每个 `user/*.c` 与用户库先静态链接为 `obj/__user_*.out`，然后通过内核链接器的 `--format=binary` 嵌入镜像。QEMU 的 loader 启动时将整个镜像放到物理地址 `0x80200000`。此时 ELF 字节已经在 RAM。实际的用户页映射发生在进程运行到 `kernel_execve → load_icode` 时，入口由 ELF 决定，`exit` 的链接脚本入口是 `0x800020`。通常 Linux 从文件系统读取 ELF，结合虚拟内存文件映射、按需缺页和动态链接器装入程序。此处内嵌 ELF 让实验在文件系统和通用缺页机制就绪前即可验证进程机制。 [Makefile](../code/Makefile) · [用户链接脚本](../code/tools/user.ld) · [指导书练习](http://oslab.mobisys.top/lab2026/_book/lab5/lab5_2_1_exercises.html)

### ECALL 与 SRET 的执行证据

| 观察点 | 实测 |
| --- | --- |
| 客体用户指令 | `sys_putc` 的 `ecall=0x800136`，`a0=30`，`a1=73`，用户 `sp=0x7ffffee0` |
| 宿主异常委托 | 触发时 `priv=0`，`medeleg=0xb109`；S 态分支保存 `scause=8` 和 `sepc=0x800136`，以 `stvec=0xffffffffc0200dec` 为入口 |
| 客体内核分发 | `current->tf` 保存原用户 SP，工作栈为内核地址，`epc=0x80013a`，`SPP=0` |
| 返回 | `sret=0xffffffffc0200eba`，`sepc=0x80013a`，`sscratch=0xffffffffc04bd000`。宿主从 `priv=1` 切回 `priv=0`，客体恢复原 SP 和返回值 `a0=0` |

宿主 GDB 15.1 启动本机 QEMU 4.1.1 的带符号构建，Build ID 与安装版相同。交叉 GDB 10.1 通过远程桩加载 `bin/kernel`，再用 `add-symbol-file obj/__user_exit.out` 载入用户符号。客体单步落在 `__alltraps` 的第一条指令之后，因此脚本校验入口范围和 CSR。宿主优化使内联的模式切换函数在部分断点位置丢失变量，改在异常委托分支的源码行观察 CSR，再以 `helper_sret` 检查模式变化。 [调试任务](http://oslab.mobisys.top/lab2026/_book/lab5/lab5_4_gdb_ecall.html) · [宿主日志](tests/gdb-host.log) · [客体日志](tests/gdb-guest.log) · [退出码](tests/gdb-status.log)

QEMU 4.1.1 的 `target/riscv/insn_trans/trans_privileged.inc.c` 中，`trans_ecall` 调用 `generate_exception` 生成异常 helper 并结束当前翻译块。`cpu_helper.c` 的 `riscv_cpu_do_interrupt` 按当前特权级确定 ecall 的异常号，按 `medeleg` 写入目标模式的 CSR 并改变 PC。 `trans_sret` 生成 `helper_sret` 调用并结束翻译块，`op_helper.c` 的运行期 helper 检查特权约束，按 `SPP/SPIE` 恢复状态，使用 `sepc` 作为下一条 PC。

TCG 把一段客体指令翻译为中间操作并生成宿主代码。已缓存的块可再次执行，所以译码断点的命中次数与运行期 helper 的调用次数不同。同一条内核 `sret` 在启动内核线程时已经被译过。Lab 2 的双层调试也运行在 TCG 下，地址翻译观察点是运行期的 `riscv_cpu_tlb_fill` 和页表遍历，触发条件来自 TLB 未命中。这两类断点分别观察指令译码生成和执行时状态转换。

## 四、规格和实际提示词

[原始请求](prompt.md)明确限定使用本届框架和 Lab 4 基线、保留官方断言、从正常入口完成四个模块、验证失败回滚，并保留真实日志。实现时将指导书中的函数列表转成第二节的输入、状态变更和可观测结果，再用官方测试及临时故障注入对照。课程规格中要求 `ecall` 完成双向特权切换，因此复用了 Lab 4 的寄存器恢复并补上用户返回的 `sscratch` 语义。exec 装载顺序依据用户提供的 ELF 缓冲区寿命确定，先构建新镜像便可在装载失败时保留原进程继续执行。

| 实际发现 | 修复与复验 |
| --- | --- |
| 干净构建报告 `static declaration of 'free_area' follows non-static declaration` | Lab 2 定义为内部符号，Lab 5 头声明为 `extern`。迁移后按 Lab 5 保持外部定义，两个课程分配器自检源码原样保留。 |
| 链接错误 `undefined reference to print_pgdir` | 框架声明了该入口但源码缺失。实现当前用户页表连续有效区间及权限输出，课程 `pgdir` 通过。 |
| 用户封装固定读取五个变参，且用 `sd` 写入 `int ret` | 改为五个显式参数及 `a0–a5` 约束，由 C 从 `a0` 转为 `int`。用课程程序和双层 GDB 核对。 |
| 稀疏页表清理、SLOB 占用和原有 VMA 检查无法完整证明 OOM 回滚 | 按三级页表、VMA 和 SLOB 的所有权修复清理与检查。在临时源码副本里逐点返回分配失败，校验物理页、堆活跃单元和父子链接。 |
| 交叉 GDB 的单步地址与预期入口首地址差四字节，宿主条件断点遇到内联变量优化 | 根据实际反汇编与调试行表调整断点。由主客体日志核对同一个用户 `sepc`，两边最终均返回 0。 |

早期 PowerShell 到 WSL 的内联 shell 引号使含空格的 PATH 被展开，改用 LF 脚本调用。课程构建保留了 `testbss.c` 故意越界访问的 GCC 诊断。交付的 `code/Makefile`、`code/tools/grade.sh`、注入器、断言和用户测试与官方版本一致，逐文件核验了 96 个框架路径、评分相关文件和 15 个用户测试源码。报告目录的评分复现脚本只通过 Make 的 `SH` 变量在评分脚本返回后拷贝串口日志，保存完成后官方目标照常清理临时树。

### 建议分工与对应 Prompt

以下分工供小组审阅和复查实验时使用。Prompt 是按已经实现的模块整理的复现示例，实际会话记录见 [prompt.md](prompt.md)。

| 组员 | 学号 | 建议审阅和复现的范围 |
| --- | --- | --- |
| 董朝阳（组长） | 2411870 | 启动路径、ELF 装载、进程退出与回收，统整报告及课程评分 |
| 张旭 | 2411292 | 系统调用和 Trap 入口、用户返回，复现双层 GDB |
| 王炳環 | 2412219 | fork 地址空间、页表权限和 COW，复现故障注入与内存回收 |

**董朝阳 · 首次运行与进程生命周期**

> 从 2026 年 Lab 5 框架和小组 Lab 4 基线出发，梳理 `proc_init → init_main → user_main → kernel_execve → load_icode`。说明如何检查 ELF 段范围、初始化 BSS 和用户栈，以及怎样经过 `sret` 到达入口。核对 `exit/wait/kill` 的状态变化和资源归属，用默认 `exit`、`waitkill` 和原版 `make grade` 验证。给出命令、退出码、串口标记和得分。

**张旭 · 系统调用与双层 GDB**

> 对照用户库和陷入汇编核对 `a0–a5` 的调用约定。说明 U 态进入时如何通过 `sscratch` 切换内核栈，返回前如何恢复保存的寄存器。用同一次 `sys_putc` 关联客体 `ecall`、内核分发、宿主 QEMU 的异常委托和 `sret`，记录两侧的 EPC、SP、特权级和返回值。

**王炳環 · fork 与 COW**

> 检查 fork 中 VMA、页表和物理页引用各自的所有权。列出原可写页、共享 COW 页、单引用 COW 页和真只读页的状态转换。用隔离的临时副本检查父子共享的 PPN、父子首次写入、跨页 `copy_to_user`、分配失败回滚和退出后的页数。给出每项断言及运行结果。

## 五、复现与结果

环境为 WSL2 Ubuntu 24.04.5、SiFive RV64 GCC 10.2.0 / Binutils 2.35、GNU Make 4.3、QEMU 4.1.1、OpenSBI v0.4，QEMU virt 单核 128 MiB。仓库已有相同工具链下的 Lab 4 兼容记录。以下命令从 Lab 5 worktree 根目录在 WSL 执行。

```sh
cd /mnt/d/os-lab-lab5
set -o pipefail
bash report/tests/grade.sh
bash report/tests/build-boot.sh 2>&1 | tee report/tests/build.log
bash report/tests/dual-gdb.sh
bash report/tests/edge.sh 2>&1 | tee report/tests/edge.log
bash report/tests/lifecycle.sh 2>&1 | tee report/tests/lifecycle.log
bash report/tests/cow.sh 2>&1 | tee report/tests/cow.log
```

`dual-gdb.sh` 默认使用 `~/src/qemu-4.1.1/riscv64-softmmu/qemu-system-riscv64`，可用 `QEMU_DEBUG` 指定同版本带调试信息的构建。脚本根据本次 ELF 的反汇编计算断点地址。

| 项目 | 实测结果 | 证据 |
| --- | --- | --- |
| `make clean`、`make -j4` | 均返回 0 | [构建和工具版本](tests/build.log) |
| `timeout 25s make qemu` | 返回 0。默认 `exit` 测试完成，init 内存自检通过，SBI 关机 | [启动串口](tests/qemu.log) |
| 官方 `make grade` | 返回 0，130/130。13 项的行为检查与公共检查均为 OK | [评分输出](tests/grade.log) · [13 份串口](tests/grade-serial/) |
| 主客体双层 GDB | 两个 GDB 均返回 0，证明 `ecall → S 态 → sret → U 态` | [宿主日志](tests/gdb-host.log) · [客体日志](tests/gdb-guest.log) · [串口](tests/gdb-serial.log) |
| 临时边界用例 | QEMU 返回 0。4 个堆分配和 6 个页分配失败点均恢复资源和链接。父子地址空间隔离，未映射指针、跨页指针和代码页写回被拒绝，截断 ELF 拒绝后可继续执行，有效 ELF 替换后返回退出码 77 | [编译和退出码](tests/edge.log) · [串口](tests/edge-serial.log) · [测试源码](tests/edge.c) |
| 额外生命周期用例 | `waitkill`、`forktree` 均返回 0。前者验证等待中的父进程被终止及后续子进程回收，后者运行到四层树并通过 init 内存自检 | [命令与退出码](tests/lifecycle.log) · [waitkill 串口](tests/waitkill.log) · [forktree 串口](tests/forktree.log) |
| COW 专项 | QEMU 返回 0。检查共享 PPN、多代引用、父子写隔离、真只读页、fork 与写时分离的 OOM、跨页内核写回、单引用原地恢复及最终回收 | [命令和退出码](tests/cow.log) · [串口](tests/cow-serial.log) |

评分场景通过 `DEBUG_GRADE` 在回收完毕后进入课程 `readline` 结束点，普通构建在同一内存自检后经 SBI 关机。串口按字符由 `SYS_putc` 输出，计时器触发调度时父子输出可能相交。

## 六、OS 原理对照

| 知识点 | 实验中的体现 | 范围 |
| --- | --- | --- |
| 特权转换 | `ecall` 陷入、`sscratch` 换栈、Trap frame 保存与 `sret` 恢复 | 使用 RISC-V U/S 和 SBI 环境，系统调用 ABI 以 `a0` 放调用号 |
| 地址空间与保护 | Sv39、VMA、`satp`、用户叶页权限、PTE 引用计数和 TLB 刷新 | 用户 ELF 静态装载，普通 fork 共享叶页，首次写入时分离 |
| 进程生命周期 | 子进程 PID 和 Trap frame、阻塞等待、退出码、僵尸回收、孤儿由 init 收养 | 单核，关中断维护进程表原子性，使用线性扫描调度 |
| 内核内存与回滚 | PMM 物理页、SLOB 对象、进程持有的 mm 和内核栈按所有权释放 | 用分配失败注入验证完整回滚 |

本实验范围外的 OS 知识包括文件系统与文件描述符继承、动态链接、交换与通用按需分页、完整信号语义、抢占式多核同步和调度策略。

## 七、选做挑战 · Copy on Write

### 状态与所有权

`PTE_COW` 占用 Sv39 留给软件的一位。它只标识曾经可写、当前暂时写保护的用户页。VMA 保留原逻辑访问权限。 `copy_range` 先为子进程安装映射并通过 `page_insert` 增加页引用，对原可写页清除 `PTE_W`、设置 `PTE_COW`。整个地址空间准备成功后，`dup_mmap` 才对父页表提交同样的权限并失效该虚拟地址的 TLB。父进程在这段操作期间处于内核临界区，子进程尚未发布。任意分配失败都可以通过 `exit_mmap` 回滚子进程已经安装的映射，父进程保有原 PTE。 `CLONE_VM` 继续共享整套地址空间。 [mmu.h](../code/kern/mm/mmu.h) · [pmm.c](../code/kern/mm/pmm.c) · [vmm.c](../code/kern/mm/vmm.c)

| 起始状态 | 事件 | 结果 |
| --- | --- | --- |
| 私有可写，`W=1, COW=0` | fork 成功 | 父子指向同一物理页，两个 PTE 都切换到 `W=0, COW=1`，引用增加 1 |
| 共享 COW，`W=0, COW=1, ref>1` | 再次 fork | 新进程继续引用原页，保留各自的只读 COW 映射 |
| 共享 COW，`ref>1` | 实际写入 | 分配新页并复制旧页，只更新写入进程的 PTE 为 `W=1, COW=0`，旧页引用减 1 |
| 单引用 COW，`ref=1` | 实际写入 | 同一物理页原地恢复可写，清除 COW 标志 |
| 真只读，`W=0, COW=0` | fork 或写入 | fork 可共享物理页。写入仍按访问错误处理 |
| 任意已映射页 | 退出或 exec 替换 | 撤销该映射并递减引用，最后一个引用撤销时归还 PMM |
| 共享 COW，`ref>1` | 写时分离分配失败 | 页内容、原 PTE 和引用不变。用户指令路径终止进程并返回内存不足的退出码。内核写回返回 `-E_NO_MEM` |

### 写故障和系统调用写回

U 态写故障由 `exception_handler → cow_resolve` 处理。内核重新检查当前 VMA 写权限及实际 PTE 的有效、用户、可读和 COW 位，只有符合条件才恢复或更换映射。成功时保留 `sepc`，CPU 重试原 store。 `copy_to_user` 也复用该检查。在真正复制用户数据前准备整个输出区间，跨页处理的后半段如果分配失败，先前已经私有化的页仍保留原字节，`waitpid` 也保留待收割的子进程以供重试。 [trap.c](../code/kern/trap/trap.c) · [vmm.c](../code/kern/mm/vmm.c) · [proc.c](../code/kern/process/proc.c)

### Dirty COW 的防护分析

Dirty COW（CVE-2016-5195）是 Linux 中 COW 写权限处理的竞态，影响了私有只读映射。本实验检查对应的权限约束：VMA 的可写资格和当前 PTE 在实际写入前均须成立，临界区覆盖权限验证、分离、映射更新、TLB 失效和内核写回。单核 uCore 的这条路径没有 Linux GUP 的页面固定和文件后备映射机制。专项只做防御回归，用真实只读用户页和跨页写回失败验证权限与隔离。 [Ubuntu 安全公告](https://ubuntu.com/security/CVE-2016-5195)

内核用例核对父、子、孙的物理页身份和引用数，并分别在 fork 建表、写时分离及跨页写回注入 OOM。用户用例经正常 fork 路径让父子写同一虚拟地址，并校验真正只读页的写入会触发异常。测试镜像来自临时副本，最终评分仍从交付目录运行。 [内核用例](tests/cow-kernel.c) · [用户用例](tests/cow-user.c) · [复现脚本](tests/cow.sh) · [实测串口](tests/cow-serial.log)
