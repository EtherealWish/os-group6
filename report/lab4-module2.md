# Lab4 功能模块二 · 进程创建及初始化

> 本文件保存 2026-11-10 模块二阶段的实现和验证记录。正式依赖已在后续合入，2026-11-12 对集成代码 9435ded 的复测为 30/30，见 [小组实验报告](report.md)。

实验日期为 2026-11-10。小组为第 6 组，模块实现标记为 2411870。

## 基线与验收范围

初始工作区在 `lab3` 的 `f9dc83b`，跟踪 `origin/lab3`。已有未跟踪内容为 `output/pdf/` 下的 PDF 和 `tmp/pdfs/build.py`。未发现适用于当前目录的 `AGENTS.md`。Windows Git 远程查询遇到连接重置及超时，随后 GitHub API 和 WSL `git ls-remote` 均确认远程只有 `main`、`lab1`、`lab2` 和 `lab3`。因此先在原工作区的独立目录 `lab4/code/` 中以课程发布的 `lab4.zip` 为基线实施，随后按仓库分支约定交付到 `lab4` 分支的 `code/`。解包根目录为 `lab4_new`，SHA-256 为 `7a92f243f88ca9a4ec78b52e554a14d78a41bbf6a79a1275f6cc929aaebceba2`。[课程源码发布页](http://oslab.mobisys.top/lab2026/labcode.html) · [远程查询和校验值](tests/lab4-module2/baseline.log)

课程按虚拟内存页操作、进程创建及初始化、进程调度划分三个模块。模块二负责创建和登记可调度的内核线程，实现 `alloc_proc`、`proc_init`、`kernel_thread`、`do_fork`、`copy_thread` 和 `copy_mm`。课程测试在 `proc_init` 末尾断言 idle 和 init 的状态、栈、父子关系与初始上下文，在 `init_main` 内创建两个测试线程并检查调度执行，最后核对 init 的串口输出。[Lab4 练习](http://oslab.mobisys.top/lab2026/_book/lab4/lab4_2_1_exercises.html) · [实验报告要求](http://oslab.mobisys.top/lab2026/_book/lab4/lab4_5_labs_requirement.html)

## 实现与接口

交付源码相对课程包只修改了 `kern/process/proc.c`。

| 函数 | 前提及实现 |
| --- | --- |
| `alloc_proc` | 内核堆已初始化。清零 PCB，设置未分配 PID、`PROC_UNINIT`、启动页表物理地址及空链表节点。内存不足返回 `NULL`。 |
| `proc_init` | 初始化进程链表和 PID 哈希桶。让 PID 0 的 idle 复用启动栈，设为 `current` 并请求首次调度。用 `kernel_thread` 创建 PID 1，再按 PID 找到并命名 init。idle 由调度器单独处理。 |
| `kernel_thread` | 将函数与参数写入临时帧的 `s0`、`s1`，`epc` 指向 `kernel_thread_entry`，设置 `SPP`、`SPIE` 并清除 `SIE`，将带有 `CLONE_VM` 的标志传给 `do_fork`。 |
| `do_fork` | 输入为当前进程、有效 Trap frame 和可选的启动栈指针。分配 PCB 与独立内核栈，准备地址空间和执行现场，随后依次分配 PID、挂入哈希表与链表、增加计数并唤醒。成功返回正 PID，超容量返回 `-E_NO_FREE_PROC`，内存不足返回 `-E_NO_MEM`。失败时按取得资源的逆序释放。 |
| `copy_thread` | 将 Trap frame 复制到子线程内核栈顶预留区，子线程 `a0` 置零。显式栈指针非零时写入保存的 `sp`，否则使用内核栈顶。将切换上下文的 `ra` 设为 `forkret`，`sp` 设为帧地址。 |
| `copy_mm` | 保留框架接口。当前执行实体是共享内核页表的内核线程，断言父线程没有用户地址空间。新 PCB 的 `mm` 为 `NULL`，`pgdir` 已由 `alloc_proc` 设置。 |

现有 `set_proc_name` 固定从源地址读取 15 字节，给短字符串 `"idle"` 和 `"init"` 命名时会越过源字符串。保留名称缓冲区清零和返回约定，将拷贝长度改为 `strnlen(name, PROC_NAME_LEN)`。

### 原理与调用链

```text
kern_init → pmm_init → vmm_init → proc_init
  → idleproc / current
  → kernel_thread(init_main, ...) → do_fork → copy_thread → 登记并唤醒 initproc
cpu_idle → schedule → proc_run → switch_to
  → forkret → forkrets → __trapret → sret → kernel_thread_entry → init_main
```

PCB 保存进程身份、生命周期状态和资源引用。上下文切换按 RISC-V 调用约定保存 `ra`、`sp`、`s0` 至 `s11`。首次运行还需要一份 Trap frame，由 `sret` 建立线程的 PC、栈和特权级，再由入口汇编把 `s1` 传入 `a0`。这使新线程能从普通 C 函数开始执行。[课程线程首次调度说明](http://oslab.mobisys.top/lab2026/_book/lab4/lab4_4_2_3_sched_run_kthread.html)

分配与登记处于单核关中断临界区，调用结束恢复原先的中断状态。发布前完成资源初始化，可以使查找表、调度队列和进程计数保持一致。实验覆盖内核线程，用户进程地址空间复制、写时复制、多核同步和退出回收属于后续范围。

## 实际构建与验证

在 WSL2 Ubuntu 24.04.5 LTS 上使用 SiFive RISC-V GCC 10.2.0、GNU Make 4.3 和交叉 GDB 10.1。两版 QEMU 用同一临时依赖镜像、同一 `virt` 和默认 BIOS 参数运行。4.1.1 使用 OpenSBI v0.4，能完成内核初始化并执行 init。系统版 8.2.2 使用 OpenSBI v1.3，只观测到固件输出，5 秒超时退出 124。因此评分及补充验证采用 `/opt/qemu-4.1.1/bin/qemu-system-riscv64`。[4.1.1 启动日志](tests/lab4-module2/boot-4.1.1.log) · [8.2.2 启动日志](tests/lab4-module2/boot-8.2.2.log)

| 副本 | 实际命令或检查 | 结果 |
| --- | --- | --- |
| 交付源码副本 | `make obj/kern/process/proc.o` | 退出 0，模块单文件编译通过。 |
| 交付源码副本 | `make -j4`，`make grade GRADE_TMP=…/isolated-grade` | 均退出 2。原框架缺 `best_fit_pmm.h`，另有 Lab2 `page_init`、Lab3 Trap 和 Lab4 模块一、三待接入。脚本打印的 0/30 来自构建失败后无串口数据。 |
| 临时依赖副本 | `make -j4`，`timeout 75s make grade GRADE_TMP=…/course-grade` | 均退出 0。原课程测试给出 PMM、Process Create、Schedule 三项 OK，总分 30/30。 |
| 独立评分副本 | `python3 tools/inject.py "$PWD"`，`timeout 45s sh tools/grade.sh -v` | 注入成功，评分退出 0，再次得到 30/30。保留了完整串口。 |
| 临时边界测试副本 | `make -j4`，`timeout 30s qemu-system-riscv64 -machine virt -nographic -bios default -device loader,file=bin/ucore.img,addr=0x80200000 -no-reboot` | 均退出 0。出现 `[Module2] ALL EDGE CHECKS PASS`，由测试调用 SBI 关机。 |
| 临时依赖副本 | `timeout 20s riscv64-unknown-elf-gdb -nx -batch -x …/first-entry.gdb bin/kernel` | 退出 0。实际命中 `kernel_thread_entry`，`current == initproc`、父线程为 idle，`sp` 在自身栈顶且 16 字节对齐，`s0` 为 `init_main`，`s1` 指向 `Hello world!!`。 |

临时副本移植了仓库已有的 Lab2 页初始化、Best Fit 实现和 Lab3 Trap 处理，保留课程 PMM 检查，并补入用于联调的页表映射与调度实现。课程测试、注入器和评分脚本保持原样。上述 30/30 是临时依赖组合的结果，小组正式依赖的整体验收仍待完成。原评分测试将 `void (*)(void *)` 传入要求 `int (*)(void *)` 的接口，产生编译警告。框架的 `do_exit` 在 init 输出完成后进入 panic/监控器，串口如实保留该输出。[评分构建与结果](tests/lab4-module2/course-grade.log) · [详细评分](tests/lab4-module2/grade-verbose.log) · [完整串口](tests/lab4-module2/grade-serial.log)

边界测试检查了 PCB 默认值，分别注入 PCB 和内核栈分配失败，核对返回码、PCB 回收、空闲页数、链表、PID 和 SIE 恢复。随后验证失败后可创建新线程、两种启动栈指针取值及 Trap frame 寄存器，并实际创建至 `MAX_PROCESS = 4096`，确认下一次调用返回 `-E_NO_FREE_PROC` 且未发布新进程。[补充测试日志](tests/lab4-module2/edge.log)

首次 GDB 调试前，`make grade` 清理了镜像，重新编译后成功命中线程入口。第二次检查中 GDB 将 `$sp` 视作指针，位运算出错。将其转换为 `unsigned long` 后复跑，全部断言通过。[首次执行检查](tests/lab4-module2/first-entry.log)

## 交付状态

待小组接入正式 Lab2/3 前置实现及 Lab4 模块一、三后，需在合并后的源码上再运行课程评分。`lab4` 分支的 `code/` 只包含课程框架和模块二源码。编译产物、临时依赖及注入的测试代码单独保存在仓库外的验证目录 `D:\os-lab-lab4-validation-0543e09d`。本次用户请求原文追加于 [prompt.md](prompt.md)。
