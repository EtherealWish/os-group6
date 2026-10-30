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

## 实验三文件与测试

- `code/kern/trap/`：Trap 入口、寄存器恢复及中断与异常处理。
- `code/tools/grade.sh`、`inject.py`、`test_lab3*`：原有评分测试。
- `code/tools/test_module2.py`：模块二补充回归，覆盖 16/32 位异常返回、分派与自动关机。
- [完整实验报告](report/report.md)：含三位成员的 Prompt 附录、实现分析和测试截图。
- `report/prompt.md`：Prompt 原文查阅备份。
- `report/tests/`：评分、编译和 QEMU 测试记录。

在 WSL 中设置工具链路径后运行（路径按实际安装位置调整）：

```sh
export PATH=/opt/riscv/riscv64-unknown-elf-toolchain-10.2.0-2020.12.8-x86_64-linux-ubuntu14/bin:/opt/qemu-4.1.1/bin:/usr/bin:/bin
cd /mnt/d/os-lab/code
make grade
python3 tools/test_module2.py
```

测试在临时源码副本中注入用例。`bin/`、`obj/` 及临时调试输出不纳入版本控制；`report/tests/` 保存验收证据。