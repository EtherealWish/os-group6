**负责人：** 2411292-张旭

**AI 工具 / 模型：** Codex CLI / GPT-6.1 Sol（xhigh）

**实验内容：** Lab4 功能模块三，进程调度

## 一、Lab4 实验梳理

开始实现前，我先结合指导书和组员提交的代码，梳理模块三与线程创建、页表和 Trap 返回之间的关系。

**提出的问题：**

> 我负责 Lab4 功能模块三“进程调度”。实验指导书已下载至本地 WSL 环境，组员完成的代码也已上传至小组仓库。请结合指导书和当前源码，梳理本实验的整体流程、模块三的主要任务，以及它与线程创建、页表切换和 Trap 返回过程之间的关系。

**梳理结果：**

模块一维护页映射，模块二创建 PCB、内核栈和初始现场，模块三负责选择线程并切换执行。四个函数分别承担以下工作：

| 函数 | 作用 |
|---|---|
| `cpu_idle()` | 检查重新调度请求 |
| `schedule()` | 按链表顺序选择可运行线程，必要时选择 idle |
| `proc_run()` | 更新 current 和 Sv39 页表，调用上下文切换 |
| `switch_to()` | 保存旧线程、恢复目标线程的寄存器 |

起始版本为 `04f1d94`，组员已提交模块一、二和用于联调的调度代码。实现阶段从这份代码继续，重点检查线程选择和切换时的状态关系。

context 按 `ra`、`sp`、`s0` 至 `s11` 排列。新线程的 `ra` 预置为 `forkret`，第一次切换后通过 `forkrets`、`__trapret` 恢复 Trap frame，进入线程函数。运行过的线程则回到上次切换的位置。页表部分使用 Sv39 编码，指导书中提到的旧 `lsatp()` 采用 Sv32 格式。

## 二、第一轮：进程调度实现

第一轮检查四个函数的行为，接入本人任务标记，并编译相关目标和整个内核。

**使用的提示词：**

````text
[PROMPT]

任务：完成 Lab4 功能模块三“进程调度”，把已经创建好的内核线程交给 CPU 执行，形成从 idle 到 init 以及线程之间切换的完整过程。

工作目录为 `/home/xuzzzz123/os-course/os-group6-lab4`，内核工程位于 `code/`，本地分支为 `lab4`，跟踪 `origin/lab4`。本部分负责人为 2411292-张旭。

本任务分两轮进行。第一轮核对并完善实现，完成编译检查；第二轮单独编写提示词，执行课程测试、专项测试和 GDB 检查。这份提示词用于第一轮，请在工程中直接修改实际文件。

当前基线为 `04f1d9478c5f2704636cc7a9eca7380ad10ee332`。组员已经实现模块一、二，并在 `4e5f4b1` 中为联调补入了模块三。请先阅读这份已有实现，按下面的规格逐项核对，保留正确的逻辑，针对实际问题作必要修改。结束时说明新增、调整和沿用的内容，不要为了制造代码差异重写正确实现。

记录窗口已将模块三的三处任务标记接入 `//LAB4 function3 2411292`。请保留本人标记，以及模块一、二和 Lab2/3 中其他组员的标记。

[RELY]

先阅读以下资料与源码：

1. Lab0.5 四段式提示词规范及实践指南：`/home/xuzzzz123/os-lab-book/oslab.mobisys.top/lab2026/_book/lab0.5/3_prompt_structure.html`、`4_practice_guide.html`。
2. 同一 `_book` 下 Lab4 的 `lab4_2_1_exercises.html`、`lab4_4_1_pcb.html`、`lab4_4_2_3_sched_run_kthread.html` 和 `lab4_5_labs_requirement.html`。
3. `code/kern/process/proc.c`、`proc.h`、`switch.S`、`entry.S`：PCB、内核栈、初始上下文和线程入口。
4. `code/kern/schedule/sched.c`、`sched.h`：进程唤醒与调度。
5. `code/kern/sync/sync.h`、`code/kern/driver/intr.c`：保存、关闭和恢复本 CPU 的中断状态。
6. `code/kern/mm/pmm.c`、`pmm.h`、`mmu.h`、`code/libs/riscv.h`、`list.h`、`defs.h`：页表物理地址、Sv39、寄存器宽度及链表接口。
7. `code/kern/init/init.c`、`code/kern/trap/trapentry.S`、`trap.c`：初始化顺序、首次运行恢复链和当前中断处理。
8. `code/Makefile`、`code/tools/test_lab4.c`、`inject.py`、`grade.sh`：了解构建规则和课程检查内容，本轮不修改或运行课程测试。

当前工程约定如下：

- `proc_init()` 已建立 `idleproc`、`initproc`，并令 `current = idleproc`、`idleproc->need_resched = 1`。idle 不挂入普通的 `proc_list`。
- `proc_list` 是带头结点的双向链表；`list_link` 是 PCB 内嵌节点，使用 `list_next()`、`le2proc(le, list_link)` 访问。头结点不能解释成 PCB。
- `PROC_RUNNABLE` 同时涵盖就绪和正在运行的线程。其他状态有 `PROC_UNINIT`、`PROC_SLEEPING`、`PROC_ZOMBIE`。
- `need_resched` 表示需要重新调度，`runs` 记录调度器选中该线程的次数，`current` 指向当前线程。
- `proc->pgdir` 保存页表根节点的物理地址。`SATP_MODE_SV39 = 8`，页偏移位数为 12。RV64 的 Sv39 MODE 位于 `satp` 的最高四位，PPN 由根页表物理地址右移页偏移位数得到。
- `libs/riscv.h` 中的 `lsatp(unsigned int pgdir)` 是 Sv32 遗留接口，本任务不使用它。参考 `pmm.c` 的 `enable_paging()`，通过正确宽度构造 Sv39 的 `satp`，并使用已有 `flush_tlb()`。
- `local_intr_save(flag)` 保存旧 SIE 并关闭中断；`local_intr_restore(flag)` 按保存值恢复。嵌套调用应保持外层的关闭状态。
- `struct context` 的字段按 `ra`、`sp`、`s0` 至 `s11` 排列，共 14 个 `uintptr_t`。RV64 下每项 8 字节，共 112 字节；`STORE`、`LOAD`、`REGBYTES` 使用工程已有定义。
- `switch_to(from, to)` 的两个参数分别通过 `a0`、`a1` 传入，指向旧、新线程的 `struct context`，不是 PCB 起点，也不是 Trap frame。
- 模块二已把新线程的 `context.ra` 设置为 `forkret`，将 `context.sp` 指向线程自己的 Trap frame。首次运行经过 `forkret → forkrets → __trapret → sret → kernel_thread_entry`，最后调用线程函数。

[GUARANTEE]

必须完成并核对下列接口，签名与当前声明保持一致：

```c
void cpu_idle(void);
void schedule(void);
void proc_run(struct proc_struct *proc);
void switch_to(struct context *from, struct context *to);
```

修改范围：`code/kern/process/proc.c` 中的 `proc_run()`、`cpu_idle()` 及邻近模块三说明，`code/kern/schedule/sched.c` 中的 `schedule()`，以及 `code/kern/process/switch.S`。三处任务标记均保留 `LAB4 function3 2411292`。

保留 `alloc_proc()`、`proc_init()`、`kernel_thread()`、`do_fork()`、`copy_thread()`、`copy_mm()`、`wakeup_proc()`、`do_exit()` 和页表映射实现。保留 `struct proc_struct`、`struct context`、Trap frame 布局、汇编恢复代码、启动流程、公共链表库、Makefile、课程测试、注入及评分文件。无需新增调度队列、时间片抢占、多核锁、用户地址空间复制或完整退出回收机制。

`prompt.md`、`report.md` 由记录窗口维护。本窗口只修改上述模块三源码，回复实际差异和编译结果，不改文档，不提交或推送 Git。

[SPECIFICATION]

## 1. cpu_idle()

**Pre-Condition**

物理内存、虚拟内存和进程初始化已经完成，当前执行流属于 idle，调度依赖处于可用状态。

**Post-Condition**

持续执行空闲循环，发现当前线程提出重新调度请求时进入调度器。被切回 idle 后能够继续循环，该函数不正常返回。

**Case 1**

`need_resched` 非零时调用调度器；调度可能让其他线程接管 CPU，idle 再次运行时从自身保存的执行位置继续。

**Case 2**

没有重新调度请求时继续等待，不擅自修改进程状态、链表和统计信息。

## 2. schedule()

**Pre-Condition**

`current`、`idleproc` 有效，进程链表已初始化；普通当前线程仍有合法链表节点，idle 由调度器单独处理。

**Post-Condition**

清除当前线程的重新调度请求，选出一个合法运行目标，增加该目标的 `runs` 一次，并交给 `proc_run()`。线程以后从本次调用返回时，恢复进入调度前保存的中断状态。

**Case 1**

当前为 idle 时，从普通进程链表的第一个节点开始查找可运行线程。

**Case 2**

当前为普通线程时，从其后继节点开始，按链表顺序循环查找，跳过头结点及非 `PROC_RUNNABLE` 线程。若其他线程不可运行而当前线程仍可运行，允许重新选中当前线程。

**Case 3**

完成一轮查找仍无可运行普通线程，使用 idle 保底；空链表也应正常处理。遍历必须有限，不把头结点当作 PCB，不选中睡眠、未初始化或僵尸线程。

**Requirements**

选择目标和切换衔接在关闭本 CPU 中断的临界区内完成。沿用当前链表顺序，不另造优先级或时间片策略。

## 3. proc_run(proc)

**Pre-Condition**

`proc` 指向调度器选出的合法线程，其内核栈、上下文和页表均已准备好，旧 `current` 有效。线程页表必须保留切换代码、相关 PCB 与内核栈所需的内核映射。

**Post-Condition**

切换发生时，全局 `current`、活动页表与实际执行上下文对应同一目标线程。旧线程的现场能够用于以后继续执行；旧线程重新获得 CPU 并从本调用返回时，恢复它自己的调用前中断状态。

**Case 1**

目标就是当前线程时，不重复切换页表和寄存器，也不改变中断状态。

**Case 2**

目标不同于当前线程时，在保护区内保留旧 PCB、更新 `current`、装载目标 Sv39 页表、刷新 TLB，并完成旧、新上下文交接。页表根地址必须按物理页号编码，不能直接把虚拟地址或完整物理字节地址写进 PPN 字段。

**Requirements**

中断不能在 `current`、页表和实际上下文尚未一致时提前打开。注意 `switch_to()` 后面的 C 代码只有在线程再次被切回来时才会继续执行；新线程首次运行所用的返回状态由模块二的初始现场提供。

## 4. switch_to(from, to)

**Pre-Condition**

两个参数均指向有效且独立的上下文保存区，布局与 `struct context` 一致，调用者已完成必要的中断保护。目标栈和恢复位置有效。

**Post-Condition**

旧线程的 `ra`、`sp`、`s0` 至 `s11` 存入 `from`，随后从 `to` 恢复对应值，使用目标线程的栈和返回地址继续执行。字段偏移与 C 结构体逐项一致，恢复前不能丢失仍需使用的参数地址。

**Case 1**

目标曾运行过时，回到它上一次上下文切换调用之后，继续自己的执行流。

**Case 2**

目标是新创建线程时，利用预置的返回地址进入 `forkret`，衔接现有首次运行链路。

**Requirements**

该汇编函数只负责上下文交接，不选择线程，不修改 PCB 状态、调度计数或页表，不复制完整 Trap frame。说明为何这里使用普通函数调用约定保存必要现场，而 Trap 入口需要保存更完整的现场。

## 5. 第一轮编译与交付

先检查当前 Git 状态和已有工作区差异。修改完成后执行 `git diff --check`，用项目 Makefile 的真实参数编译 `obj/kern/process/proc.o`、`obj/kern/schedule/sched.o`、`obj/kern/process/switch.o`，再执行完整内核构建。

结束时回复：实际改动的文件与代码差异；四个函数如何衔接；调度遍历、Sv39、寄存器偏移、中断嵌套与首次运行的核对结果；执行命令、退出码及关键输出；已有逻辑和本轮修改分别是什么。若发生编译错误，定位实际原因，保留错误记录，不扩大到组员模块或课程评分文件。

本轮只完成实现与编译。课程评分、QEMU、专项测试和 GDB 留到第二轮，完成后停止。
````

**执行结果：**

三处任务标记统一为 `LAB4 function3 2411292`。`proc_run()` 将 `(uintptr_t)8 << 60` 改为 `(uintptr_t)SATP_MODE_SV39 << 60`；`schedule()` 和 `switch_to()` 补充遍历、中断恢复及上下文布局说明。原调度算法和寄存器保存、恢复指令沿用已有实现。

编译在 `code/` 目录执行：

| 命令 | 退出码 | 结果 |
|---|---:|---|
| `git diff --check` | 0 | 无输出 |
| `make V= obj/kern/process/proc.o obj/kern/schedule/sched.o obj/kern/process/switch.o` | 0 | 三个对象均实际编译 |
| `make V=` | 0 | 完成 `+ ld bin/kernel`，生成 `bin/ucore.img` |

## 三、第二轮：进程调度测试

第二轮在 QEMU 中检查调度边界、多线程往返、中断状态和寄存器恢复，再用 GDB 跟踪首次运行及旧线程切回的位置。

**使用的提示词：**

````text
[PROMPT]

任务：完成 Lab4 功能模块三“进程调度”的第二轮测试。请在真实 QEMU 内核中验证 idle 调度、线程选择、页表编码和寄存器切换，保存能够对应到本次代码的执行记录。

原工程为 `/home/xuzzzz123/os-course/os-group6-lab4`，代码位于 `code/`，分支为 `lab4`，负责人为 2411292-张旭。第一轮已完成：保留组员集成的核心调度逻辑，将 Sv39 模式值改为 `SATP_MODE_SV39`，完善说明，三项目标文件与完整内核编译成功。

本轮在当前实际 `code/` 的独立临时副本中编写测试和执行，不在原工程中注入测试或清理构建产物。请实际操作，不能只给测试示例或复述已有 30/30。结束时回复结果，由记录窗口更新个人 `prompt.md` 和 `report.md`。

[RELY]

先阅读当前 Lab4 指导书的模块三要求、PCB 与线程调度讲解，以及：

1. `code/kern/process/proc.c`、`proc.h`、`switch.S`、`entry.S`。
2. `code/kern/schedule/sched.c`、`sched.h`。
3. `code/kern/sync/sync.h`、`code/kern/driver/intr.c`、`code/libs/riscv.h`、`list.h`、`sbi.h`。
4. `code/kern/init/init.c`、`code/kern/trap/trapentry.S`、`trap.c`、`code/kern/mm/pmm.c`。
5. `code/Makefile`、`code/tools/test_lab4.c`、`inject.py`、`grade.sh`。

第一轮源码关系如下：

- `cpu_idle()` 根据 `need_resched` 调用 `schedule()`。
- `schedule()` 在关闭中断的区域内清除请求、按链表顺序选择 `PROC_RUNNABLE` 线程，找不到则选 idle，并将目标 `runs` 增加一次。
- `proc_run()` 对同线程直接返回；目标不同时更新 `current`、装载 Sv39 页表、刷新 TLB，并调用 `switch_to()`。
- `switch_to()` 使用 `a0/from`、`a1/to` 保存恢复 `ra`、`sp`、`s0` 至 `s11`。共 14 个槽位、112 字节，偏移为 0、8、16 至 104。
- 新线程第一次运行经过 `forkret → forkrets → __trapret → sret → kernel_thread_entry`，已运行线程回到自己上次切换调用之后。
- `proc_init()` 在设置 `current = idleproc` 之后才创建 init。这两步之间存在真实的空普通进程链表阶段，可作为隔离副本中的空表测试位置。
- 当前线程都使用启动页表。核对 `satp` 与 `current->pgdir` 一致，不能据此宣称已验证不同用户地址空间之间的切换。
- 当前 `do_exit()` 是进入 panic/监控器的框架占位实现，没有完整线程退出回收。

使用已安装的 QEMU 4.1.1：`/home/xuzzzz123/src/qemu-4.1.1/riscv64-softmmu/qemu-system-riscv64`。本机可找到 `riscv64-unknown-elf-gdb`；其他调试器是否可用应先实际检查。

当前 Lab4 评分脚本通过 `make print-qemu`、`make print-GDB` 获取工具，不要照搬其他实验的启动约定。需要覆盖工具时，使用 Makefile 实际支持的参数传递，并检查子 make 得到的值；仅设置环境中的 `GDB` 不一定能覆盖 Makefile 的赋值。保留现有评分兼容修改，不另改评分脚本。

[GUARANTEE]

1. 开始前记录原工程 Git 状态，以及源码、文档和已有构建产物的校验信息。用 `mktemp -d` 创建唯一目录，复制当前工作区的完整 `code/`，包括未提交修改。分别保留课程测试、专项测试与 GDB 所需副本及镜像，不仅检出 Git 提交版本。
2. 在副本中重新编译，保证运行镜像对应当前源码。可清理副本，原工程不执行构建、清理或测试注入，不修改任何原文件。
3. 课程副本使用现有注入器、测试和评分规则。专项副本可新增独立 C/汇编测试文件，并在 `proc_init()`、`init_main()` 或初始化边界插入最少的测试调用。测试调用用于驱动真实初始化和调度，不能替换其实现。
4. 四个被测函数 `cpu_idle()`、`schedule()`、`proc_run()`、`switch_to()` 在所有副本中均保持与第一轮源码一致；其他模块算法、结构体、页表启动代码、SBI 和课程文件也保持原样。
5. 使用 `kernel_thread()` 创建真实线程。允许在隔离副本、关闭中断且线程尚未运行的受控阶段，调整真实 PCB 的状态来构造调度输入，检查后恢复。不能伪造 PCB、替换 `current`、重置全局链表、跳过切换或改写寄存器来伪造结果。
6. 测试包含课程三项检查、调度边界、多线程往返、中断恢复、寄存器保存恢复及首次进入链路。每项明确前置条件、操作、断言和结果；`PASS`、`FAIL`、`SKIP` 分开记录。
7. 测试发现缺陷时保留失败断言、输入和日志，不修改调度算法或降低预期。不要故意传入非法指针等导致停机的参数作为普通用例。
8. 完成后核对原工程、结束本轮启动的进程、保留临时副本和日志。不改个人或小组文档，不提交、推送 Git，不删除测试记录。

[SPECIFICATION]

## 1. 课程测试

**Pre-Condition**

课程副本与第一轮实际源码一致，测试仅通过原有 `inject.py` 接入，工具路径已核对。

**Post-Condition**

实际运行并分别记录以下检查，每项 10 分，满分 30：

- `[Test PMM] PASS`。
- `[Test Process Create] PASS`。
- `[Test Schedule] PASS`，并有 init 的 PID、名称和原有问候输出。

**Requirements**

优先在保留的课程副本中直接注入、构建并执行原 `tools/grade.sh`，避免顶层目标自动删除所需日志。记录命令、构建、评分及各进程退出情况。评分脚本在监控器断点处终止 QEMU，应记作受控终止；不能把评分退出码当成 guest 正常关机的证明。

本框架 init 返回后进入 `do_exit()` 的 `process exit!!` panic/监控器是已知占位路径，需结合调用位置解释。额外的断言失败或其他 panic 不应忽略。

## 2. idle、空表和没有可运行线程

**Pre-Condition**

使用独立专项镜像的真实初始化过程，idle 已建立，当前为 idle，中断状态可控。

**Post-Condition**

以下情况均经过真实 `schedule()`，按断言验证：

**Case 1**

在 `current = idleproc` 已完成、init 尚未创建的合法初始化位置插入测试调用。此时普通链表自然为空。调度应选中 idle，清除请求，将 idle 的 `runs` 增加一次，保持 `current`、页表和中断状态正确，且有限返回。检查后恢复后续首次调度所需的请求状态。

**Case 2**

init 已创建但尚未运行时，在临界区内暂时使普通线程不可运行。逐项验证 `PROC_SLEEPING`、`PROC_UNINIT`、`PROC_ZOMBIE` 不会被选中，最终仍以 idle 保底。确认断言后恢复原状态和后续初始化条件。这只是状态筛选测试，不代表已验证退出或回收机制。

**Case 3**

通过 GDB 或专项驱动观察 `cpu_idle()`：请求为 0 时，在有限循环观察中不进入 `schedule()`；恢复或设置请求为 1 后，进入调度并继续正常首次切换。若通过 GDB 设置请求位，应记录它作为测试输入的变化，不能修改被观察的寄存器保存结果。

**Requirements**

不通过 `list_init(&proc_list)` 或丢弃已登记节点制造空表。原始状态、调度计数的测试增量和恢复动作分别记录；恢复动作放在断言之后，不能掩盖失败。

## 3. 同线程与多线程调度

**Pre-Condition**

PCB 和上下文有效，专项镜像使用真实线程及内核栈。

**Post-Condition**

**Case 1**

直接调用 `proc_run(current)`，检查 `current`、`satp`、中断开关、调度字段及 context 保存区保持不变。这个接口本身不增加 `runs`。

**Case 2**

只让当前普通线程保持可运行，其他普通线程安全阻塞。调用 `schedule()` 后应重新选中当前线程，清除请求并使它的 `runs` 增加一次，无须真实切换。

**Case 3**

用 `kernel_thread()` 创建至少两个工作线程，在线程之间进行多轮主动调度。记录实际链表顺序，按该顺序核对运行轨迹，不假设创建顺序等于链表顺序。验证每个线程多次运行、`current` 指向自身 PCB、入口参数正确、栈内局部数据保持、重新调度请求被处理且计数合理。

**Case 4**

在轮换中保留不可运行的真实线程，验证它们没有进入线程函数、不增加运行次数，也不会阻断可运行线程间的切换。

**Requirements**

工作线程不能通过返回进入尚未实现的退出回收。可以在完成任务后进入睡眠并调度走，由仍可运行的协调线程检查结果，最后使用已有 `sbi_shutdown()` 结束专项镜像。不能释放仍挂接的 PCB 或正在使用的内核栈。

## 4. 中断状态与 Sv39

**Pre-Condition**

至少两个线程能够真实往返，进入每个检查前记录该线程的中断状态。

**Post-Condition**

分别以 SIE 为 0 和 1 的状态调用调度，确认另一个线程实际运行后，原线程返回时恢复自己的调用前状态。验证 `schedule()` 与 `proc_run()` 嵌套关闭中断期间没有由内层提前打开。

在目标线程检查活动 `satp`：MODE 为 Sv39，PPN 对应当前 PCB 的根页表物理地址。可结合 GDB 在更新 `current`、写入页表和上下文切换边界读取状态，确认切换临界区的 SIE 为 0。

**Requirements**

比较需要保持的状态位，不要求整个 `sstatus` 在真实中断前后逐位相同。所有线程使用同一启动页表时，明确只覆盖编码和一致性，不声称覆盖不同地址空间切换及 TLB 陈旧项清除的完整行为。

## 5. 寄存器恢复与首次运行

**Pre-Condition**

使用当前 ELF 和反汇编确定机器指令位置，GDB 连接 guest 内核，选择真实的线程切换。

**Post-Condition**

1. 在 `switch_to()` 第一条机器指令处记录 `a0`、`a1`，对应旧、新 PCB 中 context 的地址，保存旧线程全部 14 个相关寄存器及目标 context 快照。
2. 旧现场保存完成后，对照快照逐项检查 `from` 的 14 个槽位。执行 `ret` 前，对照目标快照检查恢复后的 `ra`、`sp`、`s0` 至 `s11`。
3. 执行 `ret` 后，PC 对应目标返回位置，栈属于目标线程。至少跟踪一次旧线程重新被切回，确认它从自己的调用位置继续。
4. 跟踪新线程首次进入 `forkret`、`forkrets`、`__trapret` 和 `kernel_thread_entry`，核对 Trap frame、最终函数入口与参数。地址从本次镜像获取，不使用旧实验地址。
5. 新增独立汇编测试辅助例程，按 ABI 保护自身调用现场，把 `s0` 至 `s11` 设置成可区分的测试值，调用真实调度后核对。用其他线程的执行记录证明中间发生了真实切换，不能只测试自选当前线程。辅助例程结束时恢复调用者现场并保持栈对齐。

**Requirements**

不能要求 caller-saved 的 `a*`、`t*` 跨普通 C 调用保持原值。GDB 若不暴露某个 CSR，可由专项内核使用 `read_csr()` 断言和输出，说明取证方式，不伪造 GDB 读数。

先检查调试器实际支持的命令，不预设 Python 支持；RV64 输出格式按实际 GDB 能力选择。TCP 受限时，独立调试可用 QEMU GDB stdio 管道；课程脚本所需连接若无法建立，应记录阻塞，不能改评分规则获取分数。

## 6. 资源与结束状态

**Pre-Condition**

记录测试线程创建前后的空闲页数、已创建线程及其状态。

**Post-Condition**

在线程创建完成后的多轮切换中，空闲页计数保持稳定，链表链接和 PCB 状态一致，切换不会继续消耗页面。协调线程完成断言后输出专项完成标记和各项结果，正常调用关机接口结束。

**Requirements**

当前没有完整 `do_exit()`，不能要求线程创建占用的所有资源在运行中自动回收到创建前计数。说明哪些线程和内核栈存活至隔离内核关机，不通过强行释放活动资源制造“全部恢复”。结束时核对原工程文件哈希、Git 状态以及第一轮已有构建产物保持一致。

## 7. 结果与交付

回复应包含：临时目录与镜像；实际工具版本和构建命令；课程三项得分；专项各项 `PASS/FAIL/SKIP`；关键断言与真实运行值；GDB 停点、context、栈与返回位置；中断和 satp 检查；资源统计；每个命令和进程的退出情况；遇到的问题及处理；原工程核对结果；新增测试源码和完整日志位置。

同时贴出可直接写入报告的关键原始输出，不只提供本机文件链接。分清构建成功、执行过测试与断言通过。若某项无法安全构造，解释限制并标为未覆盖；出现失败则保留失败，不用修改算法或预期换取 PASS。

第二轮完成后停止，等待记录窗口整理结果。
````

**执行结果：**

三个测试副本分别用于课程评分、专项运行和 GDB 检查。课程得分为 30/30，独立 GDB 完成 76 项断言。结果共 16 项 PASS、1 项 FAIL、2 项 SKIP，其中 FAIL 为课程 readline 结束断点未命中。

| 检查 | 结果 | 实际观察 |
|---|---|---|
| 课程 PMM | PASS | 原测试标记及映射检查通过，10/10 |
| 课程 Process Create | PASS | 原进程创建检查通过，10/10 |
| 课程 Schedule | PASS | 原调度检查及 init 问候输出通过，10/10 |
| 自然空表 | PASS | idle 的 runs 从 0 到 1，请求清零，页表和 context 不变 |
| 状态筛选与 idle 保底 | PASS | 睡眠、未初始化、僵尸状态均被跳过，随后恢复原状态 |
| cpu_idle 请求 0/1 | PASS | 请求为 0 时单步 120 条指令，调度断点命中 0 次；恢复为 1 后进入调度 |
| proc_run 选中自身 | PASS | SIE 为 0/1 时均保持 PCB、context、页表和计数不变 |
| schedule 再次选中自身 | PASS | 清除请求，runs 恰好增加一次，不进行寄存器切换 |
| 真实多线程与顺序 | PASS | 按实际链表顺序轮换，两工作线程各完成 6 轮 |
| 轮换中跳过不可运行线程 | PASS | 三个受控线程未进入函数，runs 均为 0 |
| 中断恢复及嵌套保护 | PASS | SIE 为 0/1 各完成 9 次往返；内层保持关闭，外层恢复 |
| Sv39 编码和一致性 | PASS | MODE=8，PPN=0x8020a，与当前 PCB 一致 |
| 14 个上下文槽位 | PASS | 14 项保存、14 项恢复逐项匹配，共 112 字节 |
| 首次运行和旧线程恢复 | PASS | 跟踪完整首次恢复链，旧 init 回到自己的 PC/SP |
| 寄存器哨兵 | PASS | 18 次真实往返保持 s0 至 s11 的测试值和栈内数据 |
| 资源与专项关机 | PASS | 创建后及切换后的空闲页数均为 31714，专项 QEMU 主动关机 |
| 课程 readline 断点结束流程 | FAIL | 断点未命中：监控器提前关机，GDB 返回 1 |
| 不同地址空间及完整 TLB 陈旧项检查 | SKIP | 全部线程共享启动页表 |
| 完整退出回收 | SKIP | do_exit 仍是占位实现 |

课程原始输出节选：

```text
[Test PMM] PASS
[Test Process Create] PASS
[Test Schedule] PASS
this initproc, pid = 1, name = "init"
To U: "Hello world!!".
To U: "en.., Bye, Bye. :)"
Total Score: 30/30
```

专项原始输出节选：

```text
[M3 empty] PASS pid=0 runs=0->1 request=0 sie=0 satp=0x800000000008020a
[M3 list] actual pid:state order 6:3 5:0 4:1 3:2 2:2 1:2
[M3 trace] PASS 1 3 2 1 3 2 1 3 2 1 3 2 1 3 2 1 3 2 1
[M3 roundtrip] PASS visits init=7 worker1=6 worker2=6 init_runs=3->9 worker_runs=6,6
[M3 interrupts] PASS actual_roundtrip SIE0=9 SIE1=9
[M3 registers] PASS s0-s11 sentinels survived 18 roundtrips; stack canaries intact
[M3 resources] PASS before=31724 after_create=31714 after_switches=31714 created=5 retained_until_shutdown=5
[M3 final] init pid=1 state=2 runs=10; worker pids=2,3 states=1,1 runs=7,7
[M3 COMPLETE] PASS shutdown=sbi_shutdown; shared boot pgdir only
```

根据本次 ELF 的反汇编，`switch_to` 首指令为 `0xffffffffc02036ee`。首次 from/to 为 `0xffffffffc0410040`、`0xffffffffc0410140`，目标返回地址为 `forkret=0xffffffffc020311e`，初始 SP/Trap frame 为 `0xffffffffc0413ee0`。随后跟踪首次恢复链，确认参数为 `Hello world!!`，并观察旧 init 切回后的 PC/SP。

```text
[GDB old thread resumed] pc=ffffffffc02036aa sp=ffffffffc0413d50 stack=ffffffffc0412000..ffffffffc0414000 sstatus=8000000000046000
[GDB PASS] inner proc_run returned to schedule without enabling SIE
[GDB PASS] outer schedule restored caller SIE=1
[GDB COMPLETE] PASS first-entry, all 14 saved/restored slots, old-thread resume, nested interrupts
```

课程清理、注入和构建的退出码均为 0，`sh -x tools/grade.sh -v` 返回 0。课程 GDB 重放返回 1，对应 QEMU 已主动关机并返回 0；最终专项 QEMU、独立 GDB 及对应 QEMU 均返回 0。测试前后原工程 151 个文件的哈希、Git 状态和 HEAD 一致。

## 四、调试中的调整

### 1. 为什么评分 30/30，课程断点检查却失败

原测试提示词写道：

> 评分脚本在监控器断点处终止 QEMU，应记作受控终止；不能把评分退出码当成 guest 正常关机的证明。

运行结果与这段描述不同。检查源码发现，`kmonitor()` 在 `readline()` 前调用了 `sbi_shutdown()`。重放原 `.gdb.in` 时，GDB 输出 `Remote connection closed` 并返回 1，QEMU 则已主动关机，返回 0。

调用栈为 `kernel_thread_entry → do_exit → __panic → kmonitor`，课程镜像在 `0xffffffffc020034c` 执行 SBI 8。三个测试标记已在此前输出，评分按这些标记得到 30/30，结束断点检查为 FAIL。

根据这次运行，补充结束检查的规格：

```text
分别检查课程分数、断点命中和 guest 结束方式。当前 kmonitor 在 readline 前主动关机，GDB 因连接关闭返回 1，QEMU 返回 0；readline 结束检查记为 FAIL。
```

### 2. satp 的有符号右移误报

第一次 GDB 使用 `$satp >> 60` 检查 MODE，由于最高位为 1，按有符号数右移后断言失败。将表达式改为先转成 `unsigned long long` 再右移，MODE 为 8，最终 76 项断言通过。

### 3. 环境及日志问题

首次课程执行在创建 TCP 套接字时退出，串口未生成测试结果。在允许 TCP 连接的环境中重跑后得到 30/30。独立 GDB 使用 stdio 管道和原生命令。

评分 verbose 日志直接重定向时收集不完整，改用管道后取得完整 shell 跟踪。

