# Lab4 实验报告：进程调度

**姓名与学号：** 张旭，2411292

**负责内容：** 功能模块三（进程调度）、Challenge 思考题

**AI 工具与模型：** Codex CLI，GPT-6.1 Sol（xhigh）

## 一、实验目的与分工

Lab4 在内存管理和 Trap 处理的基础上实现内核线程的创建与调度。我负责功能模块三，包括 `cpu_idle()`、`schedule()`、`proc_run()` 和 `switch_to()`，使创建好的线程能够获得 CPU，并在切换后继续执行。
## 二、实验环境

| 项目 | 配置 |
|---|---|
| 运行环境 | WSL，Bash |
| 目标平台 | RISC-V RV64，Sv39 |
| 代码目录 | `os-group6-lab4/code` |
| 开发分支 | `lab4`，跟踪 `origin/lab4` |
| 起始提交 | `04f1d9478c5f2704636cc7a9eca7380ad10ee332` |
| 编译工具 | GNU Make、GCC 10.2.0 |
| 模拟器 | QEMU 4.1.1，virt |
| 调试器 | riscv64-unknown-elf-gdb 10.1 |

## 三、实验整体逻辑

本实验先准备地址空间，再创建线程，最后完成调度。模块一维护页映射，模块二分配 PCB 和内核栈、设置初始现场，模块三从可运行线程中选择目标并切换过去。

`proc_init()` 把启动时的执行流作为 idle，令 `current = idleproc`，再创建 init。此时 CPU 仍在执行 idle。进入 `cpu_idle()` 后，idle 根据 `need_resched` 发起第一次调度，init 才真正开始运行。

```text
kern_init
  → pmm_init、pic_init、idt_init、vmm_init
  → proc_init：建立 idle，创建 init
  → clock_init、intr_enable
  → cpu_idle：检查调度请求
  → schedule：选择可运行线程
  → proc_run：更新 current，切换页表
  → switch_to：切换寄存器上下文
```

新线程的第一次运行用到了 Lab3 的 Trap 返回代码。模块二预先准备好 context 和 Trap frame，模块三恢复这些状态，执行路径如下：

```text
switch_to 恢复 context.ra
  → forkret
  → forkrets(current->tf)
  → __trapret
  → 恢复 Trap frame，执行 sret
  → kernel_thread_entry
  → 调用线程函数
```

本实验以 idle 和线程中显式调用 `schedule()` 的位置作为调度点，时钟中断负责设置下一次事件和计数。

## 四、进程调度的实现

### 1. PCB 和上下文

`struct proc_struct` 中与调度直接相关的字段如下：

| 字段 | 用途 |
|---|---|
| `state` | 判断线程是否处于 `PROC_RUNNABLE` |
| `need_resched` | 表示需要重新调度 |
| `runs` | 记录线程被调度器选中的次数 |
| `context` | 保存切换时的寄存器 |
| `pgdir` | 根页表的物理地址 |
| `list_link` | 普通进程链表中的节点 |
| `tf` | 指向线程的 Trap frame |

`PROC_RUNNABLE` 包括就绪和正在运行两种情况，`current` 指明当前占用 CPU 的线程。idle 由调度器单独处理，不在普通进程链表中。

`struct context` 依次保存 `ra`、`sp`、`s0` 至 `s11`，共 14 项。RV64 每项 8 字节，整个结构为 112 字节。

| 寄存器 | 槽位 | 字节偏移 |
|---|---:|---:|
| `ra` | 0 | 0 |
| `sp` | 1 | 8 |
| `s0` 至 `s11` | 2 至 13 | 16 至 104 |

线程切换位于普通函数调用路径中，调用者按 ABI 保护跨调用仍需使用的临时值，`switch_to()` 保存返回地址、栈指针和被调用者保存寄存器。Lab3 的 Trap 可以发生在任意指令处，因此要保存更完整的现场，这也是 context 和 Trap frame 布局不同的原因。

### 2. cpu_idle()

`cpu_idle()` 循环检查 `current->need_resched`，请求非零时调用 `schedule()`。模块二在初始化 idle 时已将该标记设为 1，因此可以触发第一次调度。idle 再次获得 CPU 后，继续执行这个循环。

### 3. schedule()

调度开始时保存中断状态并关闭中断，清除当前线程的调度请求。若当前线程是 idle，从普通链表的第一个节点查找；否则从当前节点的后继开始循环查找，跳过头结点和非 `PROC_RUNNABLE` 线程。

找到目标后，将其 `runs` 增加一次，再调用 `proc_run()`。若链表为空，或所有普通线程都不可运行，则选择 idle。一轮后只剩当前线程可运行时，也可以再次选中自己。

这里采用指导书给出的链表顺序查找方法。头结点用于连接链表，遍历时先判断节点身份，再通过 `le2proc()` 取得 PCB。

### 4. proc_run()

目标就是当前线程时直接返回。切换到其他线程时，先保存旧 PCB，更新 `current`，再装载目标页表、刷新 TLB，最后调用 `switch_to()`。

Sv39 页表装载代码为：

```c
write_csr(satp, ((uintptr_t)SATP_MODE_SV39 << 60) |
               (proc->pgdir >> PGSHIFT));
flush_tlb();
```

`SATP_MODE_SV39` 的值为 8，位于 `satp` 的最高四位。`pgdir` 保存根页表的物理字节地址，右移 12 位得到 PPN。原 `lsatp()` 使用 Sv32 编码，这里按 RV64 的格式直接构造 `satp`。

切换期间，`current`、页表和寄存器现场需要保持一致，因此 `schedule()` 和 `proc_run()` 都保护各自的临界区。内层保存到的中断状态为关闭，返回后仍保持关闭，外层再恢复调用前的状态。

旧线程被切走时，调用栈随线程保留。它再次获得 CPU 后，从 `switch_to()` 后面的语句继续，恢复保存在自己栈上的中断状态。

### 5. switch_to()

参数 `a0` 指向旧线程的 context，`a1` 指向新线程的 context。汇编先将旧线程的 14 个寄存器存入保存区，再从目标保存区加载对应值，最后执行 `ret`。

恢复的 `sp` 决定使用哪个线程的栈，恢复的 `ra` 决定继续执行的位置。运行过的线程回到上一次切换调用之后；新线程使用预置的 `forkret` 地址，进入首次运行路径。

## 五、规格与提示词设计

按照 Lab0.5 的要求，我将四个函数放在同一份实现提示词中，因为线程选择、页表切换和寄存器恢复需要共同完成一次调度。

| 部分 | 编写内容 |
|---|---|
| `[PROMPT]` | 指定模块三、目标文件和两阶段安排 |
| `[RELY]` | 提供 PCB 字段、context 布局、链表接口、Sv39 和中断操作 |
| `[GUARANTEE]` | 列出四个函数签名和修改位置 |
| `[SPECIFICATION]` | 描述调用前状态、执行后状态及各个分支 |

四个函数的主要规格如下：

| 函数 | 前置条件 | 后置条件 |
|---|---|---|
| `cpu_idle()` | idle 和调度依赖已初始化 | 持续检查请求，需要时调用调度器，切回后继续循环 |
| `schedule()` | current、idle 和进程链表有效 | 清除请求，选中可运行线程或 idle，增加目标运行次数 |
| `proc_run()` | 目标 PCB、内核栈、页表和上下文已准备好 | current、活动页表及执行现场对应目标，旧线程可以恢复执行 |
| `switch_to()` | from、to 指向合法 context，调用者已保护切换过程 | 保存旧寄存器，恢复目标寄存器，使用目标栈和返回地址继续执行 |

分支规格覆盖空表、没有可运行线程、重新选中自身、嵌套中断保护和新线程首次运行。对 `proc_run()`，还单独要求目标为当前线程时保持现有状态。

第一份提示词用于实现与编译，第二份用于运行测试。测试提示词将课程检查、调度边界、多线程往返和 GDB 观察分开安排，并要求记录每次切换前后的现场。这样可以先检查接口，再检查线程真正运行时的状态变化。

## 六、编译与运行结果

### 1. 编译检查

实现阶段将 `(uintptr_t)8 << 60` 改为 `(uintptr_t)SATP_MODE_SV39 << 60`，并补充遍历边界、中断恢复和上下文布局的注释。在 `code/` 目录编译三个目标文件，再构建整个内核：

```bash
make V= obj/kern/process/proc.o obj/kern/schedule/sched.o obj/kern/process/switch.o
make V=
```

两次编译均成功，生成 `bin/kernel` 和 `bin/ucore.img`，`git diff --check` 无输出。

### 2. 课程测试

课程测试在当前代码的副本中执行，PMM、Process Create、Schedule 三项各得 10 分，总分 30/30。串口输出如下：

```text
[Test PMM] PASS
[Test Process Create] PASS
[Test Schedule] PASS
this initproc, pid = 1, name = "init"
To U: "Hello world!!".
To U: "en.., Bye, Bye. :)"
Total Score: 30/30
```

### 3. 调度检查

在独立副本中加入测试线程，检查四个函数在不同状态下的行为。被测函数保持不变，主要结果如下：

| 检查内容 | 运行结果 |
|---|---|
| 空表和状态筛选 | 空表时选中 idle；睡眠、未初始化和僵尸线程均被跳过 |
| 调度请求与自身切换 | 请求为 0 时 idle 不调用调度器；schedule 再次选中自身时清除请求、增加 runs；proc_run(current) 保持原状态 |
| 多线程轮换 | 按实际链表顺序 1 → 3 → 2 轮换，两个工作线程各完成 6 轮 |
| 嵌套中断保护 | SIE 为 0、1 时各完成 9 次往返，内层保持关闭，外层恢复原状态 |
| 寄存器与栈 | s0 至 s11 的测试值经过 18 次往返保持一致，栈内数据完整 |
| 页表和资源 | MODE=8、PPN=0x8020a，与当前 PCB 一致；反复切换后空闲页数保持 31714 |

部分原始输出如下：

```text
[M3 roundtrip] PASS visits init=7 worker1=6 worker2=6 init_runs=3->9 worker_runs=6,6
[M3 interrupts] PASS actual_roundtrip SIE0=9 SIE1=9
[M3 registers] PASS s0-s11 sentinels survived 18 roundtrips; stack canaries intact
[M3 resources] PASS before=31724 after_create=31714 after_switches=31714 created=5 retained_until_shutdown=5
```

五个测试线程共占用 10 页，切换前后占用量不变。测试结束时线程的 PCB 和内核栈保留至关机。

### 4. GDB 观察

GDB 检查了 `switch_to()` 的 14 项寄存器保存和恢复，并跟踪首次运行与旧线程继续执行，共通过 76 项断言。

第一次切换时，目标 `context.ra` 为 `forkret=0xffffffffc020311e`，`context.sp` 为 `0xffffffffc0413ee0`。执行 `ret` 后经过 `forkret → forkrets → __trapret → sret → kernel_thread_entry → init_main`，线程函数接收到 `Hello world!!`。

init 再次被选中后，恢复到原来的 PC `0xffffffffc02036aa` 和 SP `0xffffffffc0413d50`，继续执行 `switch_to()` 后面的代码。此时内层 `proc_run()` 返回后 SIE 仍为 0，外层 `schedule()` 将其恢复为 1，与调用前状态一致。

## 七、调试问题与处理

### 1. 课程 readline 断点未命中

测试提示词最初按“在监控器断点处终止 QEMU”安排结束检查，但课程 GDB 输出 `Remote connection closed` 并返回 1，`readline` 断点没有命中，该项检查为 FAIL。

检查 `kmonitor()` 发现，它在 `readline()` 前调用了 `sbi_shutdown()`。调试得到调用路径 `kernel_thread_entry → do_exit → __panic → kmonitor`，并确认在 `0xffffffffc020034c` 执行 SBI 服务号 8，QEMU 随后返回 0。

评分脚本按此前的串口标记得到 30/30，而结束断点未命中。根据这次结果，我修正了提示词对结束方式的假设，将课程得分、断点命中和关机结果分别记录。当前 `do_exit()` 通过 panic 进入监控器，尚未实现进程资源回收。

### 2. satp 的有符号右移

首次 GDB 检查用 `$satp >> 60` 取得 MODE。由于最高位为 1，GDB 按有符号值右移，导致断言误报。改为先转换成 64 位无符号数：

```text
(((unsigned long long)$satp) >> 60) == 8
```

修正调试表达式后，MODE 为 8，最终 76 项断言通过。

## 八、与操作系统原理的联系

### 1. 本实验中的对应关系

| 原理知识点 | 实验中的体现 |
|---|---|
| 进程状态与调度 | state 决定线程是否可被选中，current 标识当前运行者 |
| 调度策略与切换机制 | schedule 选择目标，proc_run 和 switch_to 完成切换 |
| 执行上下文 | context 保存恢复栈和寄存器，使线程从原位置继续 |
| 虚拟地址空间 | pgdir 和 satp 决定当前使用的页表，切换后刷新 TLB |
| 临界区与原子性 | 保存并关闭本 CPU 中断，保护 current、页表和上下文的交接 |
| 内核态控制转移 | 首次线程运行借助预置 Trap frame 和 sret 进入入口函数 |

本实验将就绪和运行合在 `PROC_RUNNABLE` 中，运行者由 `current` 区分。线程切换利用普通函数调用约定保存必要寄存器，Trap 保存则需要覆盖被打断指令的现场。关闭本 CPU 中断可以保护单核临界区，多核共享数据还需要锁等同步手段。

### 2. 教材中其他相关内容

教材还讨论了抢占式调度、独立用户地址空间、进程退出回收和多核同步。本实验的调度点是显式调用 `schedule()`；内核线程共用启动页表，`copy_mm()` 保留地址空间处理接口；`do_exit()` 当前进入监控器。这些接口为后面的进程管理功能留下了位置。

本次线程均共享启动页表，因此地址空间检查只涉及 MODE、PPN 与 current 的对应关系，没有覆盖不同根页表之间的切换和 TLB 陈旧项检查；完整退出回收也未执行。

## 九、扩展练习 Challenge

### 1. local_intr_save 和 local_intr_restore 如何控制中断

这两个宏定义在 `kern/sync/sync.h` 中。`local_intr_save(intr_flag)` 调用 `__intr_save()`，读取 `sstatus` 的 SIE 位。若 SIE 为 1，就调用 `intr_disable()` 清除该位，并把 `intr_flag` 记为 1；若原本为 0，则记录 0。这样既关闭了中断，也留下了进入临界区前的状态。

`intr_disable()` 通过 `clear_csr(sstatus, SSTATUS_SIE)` 执行 `csrrc` 清位，`intr_enable()` 通过 `set_csr()` 执行 `csrrs` 置位。`SSTATUS_SIE` 的值是 `0x2`，对应 `sstatus` 的第 1 位。`local_intr_restore(intr_flag)` 只在标记为 1 时重新开启中断；标记为 0 时不执行开中断操作。

在模块三中，外层 `schedule()` 保存原状态后关中断，内层 `proc_run()` 读到的 SIE 已经是 0，所以它返回时继续保持关闭。旧线程切回后，由外层恢复最初的中断状态。这使嵌套调用能够共用一段关中断区间。

### 2. get_pte 中的两段代码为什么相像

多级页表的每一层都采用相同的组织方式：根据虚拟地址中的索引找到一个表项，再由表项找到下一级页表。分页模式改变的是层数和索引宽度，这个查找过程基本相同。以 4 KiB 基本页为例：

| 分页模式 | 地址划分 | 页表层数 | 每级表项数 | 每个表项大小 |
|---|---|---:|---:|---:|
| Sv32 | 10 位索引 + 10 位索引 + 12 位偏移 | 2 | 1024 | 4 字节 |
| Sv39 | 9 位索引 + 9 位索引 + 9 位索引 + 12 位偏移 | 3 | 512 | 8 字节 |
| Sv48 | 4 组 9 位索引 + 12 位偏移 | 4 | 512 | 8 字节 |

Sv32 用于 RV32，Sv39 和 Sv48 用于 RV64。三种模式的一张页表都占 4 KiB，页内偏移也都是 12 位。Sv39 和 Sv48 的表项大小、每级索引宽度相同，Sv48 比 Sv39 多一级页表，用于覆盖更大的虚拟地址范围。

当前 `kern/mm/pmm.c` 中的 `get_pte()` 按 Sv39 的三级结构查找。第一段通过 `PDX1(la)` 定位根级表项 `pdep1`，确保下一层页表存在；第二段通过 `PDX0(la)` 定位中间级表项 `pdep0`，确保末级页表存在。两段都要检查 `PTE_V`，在允许创建且页表缺失时申请一页、设置引用计数、清零，并将新页表的 PPN 写入上一级表项，所以代码形式几乎一样。

沿表项向下查找时，`PDE_ADDR()` 取出下一级页表的物理地址，`KADDR()` 转为内核可访问的虚拟地址。两次处理完成后，再用 `PTX(la)` 定位末级表项并返回它的地址。Sv32 少一级处理，Sv48 则多一级处理，都可以沿用这种逐层查找的方法。

### 3. 页表项查找和页表页分配是否应该拆开

我认为当前写法适合本实验。`create` 参数区分了两种用途：`get_page()` 和 `page_remove()` 传入 0，只查已有页表；`page_insert()` 传入 1，在建立映射时按需补齐中间级页表。调用者共用一套遍历逻辑，代码比较直接。

不过，`create=1` 会改变页表和内存占用。例如第一段成功创建下一层页表，第二段申请失败，函数会返回 `NULL`，但前面新建的页表仍然挂在原页表上。随着功能增加，把纯查找和建表分开，能让这类状态变化更明确，也便于在建表路径中统一处理分配失败和回收。

因此，本实验可以保留现有接口；扩展时可以让查找接口只返回已有表项，由建表接口负责申请和连接页表页，内部共用逐级遍历代码。还要注意，`get_pte()` 返回的是末级表项的地址，表项本身可能仍然无效；把数据页映射到该地址的工作由 `page_insert()` 完成。

## 十、实验小结

进程调度需要同时处理线程选择、页表和执行现场。`schedule()` 决定运行目标，`proc_run()` 更新当前线程和页表，`switch_to()` 恢复目标栈与返回地址。GDB 中观察到 init 切走后又回到原 PC/SP，使代码中的保存、恢复与实际执行过程对应起来。

课程评分为 30/30，核心专项和独立 GDB 检查通过。课程 `readline` 结束流程的问题定位到了监控器提前关机。
