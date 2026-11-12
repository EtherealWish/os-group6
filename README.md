# 操作系统课程实验仓库

## 仓库结构约定

- **每次实验对应一个分支**，分支名为 `labX`（如 `lab1`、`lab2`）
- 每个实验分支下固定包含两个文件夹：

```
labX/
├── code/                 # 本次实验实现后的完整代码
└── report/
    ├── report.md         # 实验报告
    ├── prompt.md         # 本次实验用到的全部 prompt 汇总
    ├── images/           # 可选：报告截图
    └── tests/            # 可选：测试日志
```

`main` 分支仅保存本说明与公共配置，不放实验内容。

## 分支一览

| 分支 | 实验内容 | 状态 |
|------|----------|------|
| lab1 | 比麻雀更小的麻雀（最小可执行内核）：启动流程分析、练习 1 程序入口、练习 2 GDB 验证启动流程 | 已完成 |
| lab2 | 物理内存和页表：物理内存初始化（`page_init`）、First-Fit / Best-Fit / Buddy System 分配器、SLUB 对象分配、GDB 调试页表查询 | 已完成 |
| lab3 | Trap 入口与上下文恢复、中断与异常处理 | 模块二已验证；现有评分 100/100 |
| lab4 | 页映射维护、进程创建及初始化、进程调度 | 三模块集成，2026-11-12 复测 30/30 |

## 小组成员

第 6 组

| 姓名 | 学号 | 分工 |
|------|------|------|
| 董朝阳（组长） | 2411870 | 实验环境搭建、知识点与 OS 原理对照、报告统稿 |
| 张旭 | 2411292 | 项目结构与启动流程分析、练习 1、实验整体逻辑分析 |
| 王炳環 | 2412219 | 核心函数与功能模块分析、练习 2（GDB 调试）、测试截图 |

## 实验环境

- WSL2 · Ubuntu 24.04 LTS
- 交叉编译器：`riscv64-unknown-elf-gcc` 10.2.0（SiFive freedom-tools v2020.12）
- 模拟器：`qemu-system-riscv64` 8.2.2（内置 OpenSBI v1.3）；lab2 起另装 QEMU 4.1.1（`/opt/qemu-4.1.1`），原因见 lab2 报告 2.2 节

> **lab2 起请使用 QEMU 4.1.1。** 新版 QEMU 下，内核在 `entry.S` 开启分页后会立即取指缺页并卡死，没有任何输出。运行方式：`make qemu QEMU=/opt/qemu-4.1.1/bin/qemu-system-riscv64`。`make grade` 的评分脚本直接调用 `qemu-system-riscv64`，需将 QEMU 4.1.1 的目录放到 PATH 最前。

## 实验四文件与测试

- `code/` 包含三模块集成实现，当前验证基线为 `9435ded`。
- [小组实验报告](report/report.md)整合实现、规格、测试、Challenge 和截图。
- [小组 Prompt 汇总](report/prompt.md)按成员保存实际请求、最终规格和两轮测试提示词。
- [本次集成日志](report/tests/lab4-integrated-20261112/)记录 2026-11-12 的编译和课程评分，结果为 30/30。
- [模块二阶段记录](report/lab4-module2.md)保留失败路径、容量边界和首次运行的历史验证。
- [Lab3 历史报告](report/lab3-report.md)和 [Lab3 Prompt](report/lab3-prompt.md)保留前一实验内容。

构建及评分在 WSL 中执行，使用 QEMU 4.1.1。课程测试和评分沿用当前版本，线程退出仍进入框架的 panic/监控器并关机。更多验证边界见实验报告。

```sh
export PATH=/opt/riscv/riscv64-unknown-elf-toolchain-10.2.0-2020.12.8-x86_64-linux-ubuntu14/bin:/opt/qemu-4.1.1/bin:$PATH
cd code
make
make grade
```
