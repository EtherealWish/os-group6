# 操作系统课程实验仓库

第 6 组 · Lab 5 用户进程

| 组员 | 学号 |
| --- | --- |
| 董朝阳（组长） | 2411870 |
| 张旭 | 2411292 |
| 王炳環 | 2412219 |

- [code/](code/) 保存 2026 年 Lab 5 框架、四个必做模块和 COW 选做实现。
- [实验报告](report/report.md) 对应四个模块、讲义问答、双层 GDB 和验证结果。
- [实际提示词](report/prompt.md) 和 [测试记录](report/tests/) 随报告保存。

每次实验使用同名分支 `labX`，在分支根目录放置 `code/` 和 `report/`。Lab 4 的实现及报告见 `lab4` 分支。

在 WSL2 内，从此 worktree 的根目录复现构建、QEMU 启动和课程评分。

```sh
bash report/tests/build-boot.sh
bash report/tests/grade.sh
bash report/tests/cow.sh
```

实验使用 SiFive RV64 GCC 10.2.0、QEMU 4.1.1 和 OpenSBI v0.4。双层 GDB 的 QEMU 带符号版本和测试脚本见实验报告。
