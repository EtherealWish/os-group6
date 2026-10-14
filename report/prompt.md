# Lab2 Prompt 汇总

> 本文件汇总本次实验中使用的**全部** prompt，按模块与迭代顺序记录。
> 报告正文（`report.md`）中只放每个模块**最终成功**的提示词，本文件保留完整迭代轨迹。
> 提示词结构参考指导书 lab0.5「提示词结构」的四段式框架（`[PROMPT]` / `[RELY]` / `[GUARANTEE]` / `[SPECIFICATION]`）。

---

## 模块：功能模块一 —— 物理内存初始化（`page_init()`）

**负责人：** 2411870-董朝阳
**AI 工具 / 模型：** Claude Code（桌面应用 Code 标签页） / Claude Opus 5.5

本模块的工作方式是：组员用自然语言给 Claude Code 下达任务，Claude Code 自行阅读指导书和框架代码，**先写出四段式提示词，再严格按该提示词实现代码**，随后编译测试。下面先记录组员的原始指令，再给出生成并使用的四段式提示词。

### 组员的原始指令（按时间顺序）

| 顺序 | 指令原文 | 作用 |
|---|---|---|
| 1 | `你做一下lab2吧，给我完成模块一` | 下达任务。Claude Code 读取 lab2 指导书各章节，确认模块一为实现 `page_init()` 及 Challenge 思考题；本机未找到代码包，请组员提供 |
| 2 | `C:\Users\26762\Desktop\study\大三上\操作系统  在这个路径去编辑` | 指定代码包 `lab2.zip` 所在目录及工作位置 |
| 3 | `可以换qemu版本` | 内核在 QEMU 8.2.2 上启动即陷入死循环，组员同意改用 QEMU 4.1.1 而不是修改框架的 `entry.S` |
| 4 | `你监控一下，随时跟进` | 要求持续跟进 QEMU 4.1.1 的编译与后续测试 |
| 5 | `你去截一下图吧` | 模块二完成前工程无法直接编译，Claude Code 编写 `run_lab2_module1.sh`（复制到临时目录并补占位后运行），截取 `make qemu` 与 `make grade` 的终端窗口作为报告图 1、图 2 |
| 6 | `你看看还有其他问题吗，没有就退到仓库把` | 检查交付内容（无编译残留、仅 `pmm.c` 被修改、模块二的 `YOUR CODE` 标记保持原样）后，创建 `lab2` 分支并推送 |

### 第 1 次迭代

**目的：** 实现 `kern/mm/pmm.c` 中的 `page_init()`，完成物理内存探测与 Page 描述符初始化。

**编写提示词前的准备：** Claude Code 先阅读了指导书「物理内存探测」一节、lab0.5「提示词结构」一节，以及框架中的 `pmm.c`、`pmm.h`、`memlayout.h`、`mmu.h`、`dtb.c`、`default_pmm.c`、`entry.S`，并阅读了评分脚本注入的测试 `tools/test_lab2.c`，从中确认了测试对 `va_pa_offset`、`npage`、`pages` 对齐方式和保留页的具体要求。

**使用的提示词：**

````markdown
[PROMPT]
**任务**：修改文件 `kern/mm/pmm.c`，在 `//lab2 function1 YOUR CODE` 标记处实现 `page_init()`，完成物理内存探测与 Page 描述符初始化：根据设备树探测到的物理内存范围建立 Page 描述符数组，把除 OpenSBI、内核镜像和描述符数组之外的完整空闲页交给当前的 pmm_manager 管理。

**操作要求**：你必须在项目中修改实际文件，而不是仅展示代码。保留文件中的现有代码和 `lab2` 注释，把标记中的 `YOUR CODE` 替换为学号 `2411870`。只修改 `kern/mm/pmm.c`；不要修改 `dtb.c`、`pmm.h`、`memlayout.h`、`default_pmm.c`、`tools/` 下的测试文件，也不要切换 `init_pmm_manager()` 中的物理内存管理器。

**输出要求**：直接进行文件操作，使用下面 [RELY]、[GUARANTEE]、[SPECIFICATION] 中的信息，以工程真实代码为准，不要引入无关功能。完成后说明改动了哪些代码。

[RELY]
// 内存布局（kern/mm/memlayout.h、kern/mm/mmu.h、libs/riscv.h）
#define KERNBASE               0xFFFFFFFFC0200000
#define KMEMSIZE               0x7E00000               // 内核可管理的最大物理内存
#define KERNTOP                (KERNBASE + KMEMSIZE)   // 可管理上界对应的虚拟地址
#define PHYSICAL_MEMORY_OFFSET 0xFFFFFFFF40000000      // 虚拟地址 = 物理地址 + 该偏移
#define DRAM_BASE              0x80000000
#define PGSIZE                 4096
#define PGSHIFT                12
#define PPN(la)                (((uintptr_t)(la)) >> PGSHIFT)
// entry.S 的启动页表把 [0xFFFFFFFFC0000000, +1GiB) 映射到物理 [0x80000000, +1GiB)

// libs/defs.h
#define ROUNDDOWN(a, n)  // 向下对齐到 n 的倍数，结果类型与 a 相同
#define ROUNDUP(a, n)    // 向上对齐到 n 的倍数，结果类型与 a 相同

// 数据结构（memlayout.h）
struct Page {
    int ref;                 // 引用计数
    uint64_t flags;          // 状态位
    unsigned int property;   // 空闲块大小（仅块首页有效）
    list_entry_t page_link;  // 空闲链表节点
};
SetPageReserved(page) / ClearPageReserved(page) / PageReserved(page)   // PG_reserved 位
SetPageProperty(page) / ClearPageProperty(page) / PageProperty(page)   // PG_property 位

// 全局变量（kern/mm/pmm.c）
struct Page *pages;           // Page 描述符数组的【虚拟地址】，pages[i] 描述物理页号 nbase+i
size_t npage;                 // 可管理物理页号的【上界】（不是页数），合法页号范围 [nbase, npage)
const size_t nbase = DRAM_BASE / PGSIZE;
uint64_t va_pa_offset;        // 虚拟地址与物理地址之差，PADDR 依赖它

// 可用函数
uint64_t get_memory_base(void);  // 设备树报告的 DRAM 起始物理地址，未探测到时为 0
uint64_t get_memory_size(void);  // 设备树报告的 DRAM 大小（字节），未探测到时为 0
static void init_memmap(struct Page *base, size_t n);  // 转调 pmm_manager->init_memmap；
                                                       // default 实现会 assert 这 n 个页都处于 Reserved 状态
static inline struct Page *pa2page(uintptr_t pa);      // 物理地址 → 描述符，PPN(pa) >= npage 时 panic
static inline uintptr_t page2pa(struct Page *page);
#define PADDR(kva)                                     // 内核虚拟地址 → 物理地址（kva 必须 >= KERNBASE）
void panic(const char *fmt, ...);
int cprintf(const char *fmt, ...);
extern char end[];                                     // 链接脚本定义：内核镜像结尾的【虚拟地址】

[GUARANTEE]
必须实现的接口：
```c
static void page_init(void);   // 由 pmm_init() 在 init_pmm_manager() 之后调用
```
你可以根据需要添加：
- 辅助函数（必须为 static）
- 内部使用的宏定义

[SPECIFICATION]
## page_init
**Pre-Condition**:
- 分页已由 entry.S 开启，内核运行在高地址虚拟空间；`dtb_init()` 已执行；`init_pmm_manager()` 已选好并初始化了管理器。
- `pages`、`npage`、`va_pa_offset` 尚未设置。

**Post-Condition**:
- `va_pa_offset` 等于虚实映射偏移；`npage` 等于可管理物理内存末端的页号；`pages` 指向内核 `end` 之后、按页对齐的位置。
- 描述符数组覆盖物理页号 `[nbase, npage)`，每个描述符都已初始化；不进入空闲区的页面（OpenSBI、内核镜像、描述符数组本身、不足一页的尾部）保持 Reserved。
- 描述符数组之后的完整页面全部登记到当前 pmm_manager，空闲页数等于这段区间的页数。
- 输出探测到的物理内存范围，格式为：
  `physical memory map:` 换行，`  memory: 0x<大小16位十六进制>, [0x<起始16位>, 0x<结束-1 的16位>].`

  **Case 1（正常）**：
  - 设备树给出了有效的内存范围，且描述符数组之后至少还有一个完整页：按上述布局建立描述符，并调用一次 `init_memmap()` 注册这段连续的空闲页。

  **Case 2（设备树信息无效）**：
  - 起始地址或大小为 0、范围溢出、或可管理的末端不高于 DRAM 起点：调用 `panic()` 终止初始化，不修改其他全局状态。

  **Case 3（内存不足以放下描述符数组）**：
  - 描述符数组的末端超出可管理的物理内存末端：调用 `panic()`，不得越界写入描述符。

  **Case 4（没有剩余完整页）**：
  - 描述符数组之后按页对齐的起点不小于按页对齐的末端：不调用 `init_memmap()`，但其余初始化照常完成。

**Requirements**:
- 严格区分虚拟地址、物理地址、物理页号、Page 描述符地址四种量，不得混用；比较或运算前先统一到同一种。
- 可管理上界取设备树报告的内存末端与内核可管理上界（`KERNTOP` 对应的物理地址）中较小者。
- 描述符数组所在内存不在 BSS 中，内容未被清零，初始化时必须把每个描述符的所有字段显式置为确定值，再标记为 Reserved。
````

**结果：** 按提示词生成的 `page_init()` 一次满足全部规格。

**问题与调整：**

1. 整个工程编译失败，报错位于 `best_fit_pmm.c`（`'nr_free' undeclared`）。该文件属于功能模块二，框架只给了测试函数。处理方式：在 WSL 临时副本中补一个最小占位使其能编译，源文件不改。
2. 在 QEMU 8.2.2 上内核无任何输出。通过 GDB 和 `qemu -d int` 定位为：`entry.S` 写 `satp` 后，下一条指令按未映射的低地址取指，发生取指缺页并在 `stvec = 0x80200000` 处死循环，属于模拟器版本差异。经组员同意改用 QEMU 4.1.1（指令 3），问题消失。
3. 以上两个问题都与提示词无关，提示词未修改。

**验证：** `make grade` 中 `check page init: OK`；GDB 读出 `npage = 0x88000`、`pages = 0xffffffffc0207000`、空闲页 31929 页，与手算布局一致；`-m 64M` 和 `-m 256M` 下均能正确初始化。

---

### ✅ 最终提示词（已同步到 report.md）

与第 1 次迭代使用的提示词相同，未经修改。

---

## 模块：Challenge —— 硬件的可用物理内存范围的获取方法（思考题）

**负责人：** 2411870-董朝阳
**AI 工具 / 模型：** Claude Code（桌面应用 Code 标签页） / Claude Opus 5.5

本题为思考题，无单独提示词，包含在指令 1「给我完成模块一」中。Claude Code 结合指导书中关于 OpenSBI 和设备树的说明作答，答案见 `report.md` 的 Challenge 一节。

---

## 其他模块

> 功能模块二（First-Fit 思考题、Best-Fit、Buddy System）及 SLUB Challenge 的提示词由对应负责人补充。
