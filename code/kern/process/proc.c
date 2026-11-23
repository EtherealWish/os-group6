#include <proc.h>
#include <kmalloc.h>
#include <string.h>
#include <sync.h>
#include <pmm.h>
#include <error.h>
#include <sched.h>
#include <elf.h>
#include <vmm.h>
#include <trap.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <sbi.h>
#include <kmonitor.h>

/* ------------- process/thread mechanism design&implementation -------------
(an simplified Linux process/thread mechanism )
introduction:
  ucore implements a simple process/thread mechanism. process contains the independent memory sapce, at least one threads
for execution, the kernel data(for management), processor state (for context switch), files(in lab6), etc. ucore needs to
manage all these details efficiently. In ucore, a thread is just a special kind of process(share process's memory).
------------------------------
process state       :     meaning               -- reason
    PROC_UNINIT     :   uninitialized           -- alloc_proc
    PROC_SLEEPING   :   sleeping                -- try_free_pages, do_wait, do_sleep
    PROC_RUNNABLE   :   runnable(maybe running) -- proc_init, wakeup_proc,
    PROC_ZOMBIE     :   almost dead             -- do_exit

-----------------------------
process state changing:

  alloc_proc                                 RUNNING
      +                                   +--<----<--+
      +                                   + proc_run +
      V                                   +-->---->--+
PROC_UNINIT -- proc_init/wakeup_proc --> PROC_RUNNABLE -- try_free_pages/do_wait/do_sleep --> PROC_SLEEPING --
                                           A      +                                                           +
                                           |      +--- do_exit --> PROC_ZOMBIE                                +
                                           +                                                                  +
                                           -----------------------wakeup_proc----------------------------------
-----------------------------
process relations
parent:           proc->parent  (proc is children)
children:         proc->cptr    (proc is parent)
older sibling:    proc->optr    (proc is younger sibling)
younger sibling:  proc->yptr    (proc is older sibling)
-----------------------------
related syscall for process:
SYS_exit        : process exit,                           -->do_exit
SYS_fork        : create child process, dup mm            -->do_fork-->wakeup_proc
SYS_wait        : wait process                            -->do_wait
SYS_exec        : after fork, process execute a program   -->load a program and refresh the mm
SYS_clone       : create child thread                     -->do_fork-->wakeup_proc
SYS_yield       : process flag itself need resecheduling, -- proc->need_sched=1, then scheduler will rescheule this process
SYS_sleep       : process sleep                           -->do_sleep
SYS_kill        : kill process                            -->do_kill-->proc->flags |= PF_EXITING
                                                                 -->wakeup_proc-->do_wait-->do_exit
SYS_getpid      : get the process's pid

*/

// the process set's list
list_entry_t proc_list;

#define HASH_SHIFT 10
#define HASH_LIST_SIZE (1 << HASH_SHIFT)
#define pid_hashfn(x) (hash32(x, HASH_SHIFT))

// has list for process set based on pid
static list_entry_t hash_list[HASH_LIST_SIZE];

// idle proc
struct proc_struct *idleproc = NULL;
// init proc
struct proc_struct *initproc = NULL;
// current proc
struct proc_struct *current = NULL;

static int nr_process = 0;

void kernel_thread_entry(void);
void forkrets(struct trapframe *tf);
void switch_to(struct context *from, struct context *to);

// lab5 function2 2411870
static struct proc_struct *
alloc_proc(void)
{
    struct proc_struct *proc = kmalloc(sizeof(struct proc_struct));
    if (proc != NULL)
    {
        // 新 PCB 在资源就绪前不可调度，PID 在登记时分配。
        memset(proc, 0, sizeof(*proc));
        proc->state = PROC_UNINIT;
        proc->pid = -1;
        proc->pgdir = boot_pgdir_pa;
        list_init(&proc->list_link);
        list_init(&proc->hash_link);
    }
    return proc;
}


// set_proc_name - set the name of proc
char *
set_proc_name(struct proc_struct *proc, const char *name)
{
    memset(proc->name, 0, sizeof(proc->name));
    return memcpy(proc->name, name, strnlen(name, PROC_NAME_LEN));
}

// get_proc_name - get the name of proc
char *
get_proc_name(struct proc_struct *proc)
{
    static char name[PROC_NAME_LEN + 1];
    memset(name, 0, sizeof(name));
    return memcpy(name, proc->name, PROC_NAME_LEN);
}

// set_links - set the relation links of process
static void
set_links(struct proc_struct *proc)
{
    list_add(&proc_list, &(proc->list_link));
    proc->yptr = NULL;
    if ((proc->optr = proc->parent->cptr) != NULL)
    {
        proc->optr->yptr = proc;
    }
    proc->parent->cptr = proc;
    nr_process++;
}

// remove_links - clean the relation links of process
static void
remove_links(struct proc_struct *proc)
{
    list_del(&(proc->list_link));
    if (proc->optr != NULL)
    {
        proc->optr->yptr = proc->yptr;
    }
    if (proc->yptr != NULL)
    {
        proc->yptr->optr = proc->optr;
    }
    else
    {
        proc->parent->cptr = proc->optr;
    }
    nr_process--;
}

// get_pid - alloc a unique pid for process
static int
get_pid(void)
{
    static_assert(MAX_PID > MAX_PROCESS);
    struct proc_struct *proc;
    list_entry_t *list = &proc_list, *le;
    static int next_safe = MAX_PID, last_pid = MAX_PID;
    if (++last_pid >= MAX_PID)
    {
        last_pid = 1;
        goto inside;
    }
    if (last_pid >= next_safe)
    {
    inside:
        next_safe = MAX_PID;
    repeat:
        le = list;
        while ((le = list_next(le)) != list)
        {
            proc = le2proc(le, list_link);
            if (proc->pid == last_pid)
            {
                if (++last_pid >= next_safe)
                {
                    if (last_pid >= MAX_PID)
                    {
                        last_pid = 1;
                    }
                    next_safe = MAX_PID;
                    goto repeat;
                }
            }
            else if (proc->pid > last_pid && next_safe > proc->pid)
            {
                next_safe = proc->pid;
            }
        }
    }
    return last_pid;
}






// forkret -- the first kernel entry point of a new thread/process
// NOTE: the addr of forkret is setted in copy_thread function
//       after switch_to, the current proc will execute here.
static void
forkret(void)
{
    forkrets(current->tf);
}

// hash_proc - add proc into proc hash_list
static void
hash_proc(struct proc_struct *proc)
{
    list_add(hash_list + pid_hashfn(proc->pid), &(proc->hash_link));
}

// unhash_proc - delete proc from proc hash_list
static void
unhash_proc(struct proc_struct *proc)
{
    list_del(&(proc->hash_link));
}

// find_proc - find proc frome proc hash_list according to pid
struct proc_struct *
find_proc(int pid)
{
    if (0 < pid && pid < MAX_PID)
    {
        list_entry_t *list = hash_list + pid_hashfn(pid), *le = list;
        while ((le = list_next(le)) != list)
        {
            struct proc_struct *proc = le2proc(le, hash_link);
            if (proc->pid == pid)
            {
                return proc;
            }
        }
    }
    return NULL;
}

// lab5 function2 2411870
int
kernel_thread(int (*fn)(void *), void *arg, uint32_t clone_flags)
{
    struct trapframe tf;
    memset(&tf, 0, sizeof(tf));
    // kernel_thread_entry 从 s0 取函数入口，从 s1 取参数。
    tf.gpr.s0 = (uintptr_t)fn;
    tf.gpr.s1 = (uintptr_t)arg;
    tf.status = (read_csr(sstatus) | SSTATUS_SPP | SSTATUS_SPIE) & ~SSTATUS_SIE;
    tf.epc = (uintptr_t)kernel_thread_entry;
    return do_fork(clone_flags | CLONE_VM, 0, &tf);
}


// setup_kstack - alloc pages with size KSTACKPAGE as process kernel stack
static int
setup_kstack(struct proc_struct *proc)
{
    struct Page *page = alloc_pages(KSTACKPAGE);
    if (page != NULL)
    {
        proc->kstack = (uintptr_t)page2kva(page);
        return 0;
    }
    return -E_NO_MEM;
}

// put_kstack - free the memory space of process kernel stack
static void
put_kstack(struct proc_struct *proc)
{
    free_pages(kva2page((void *)(proc->kstack)), KSTACKPAGE);
}

// setup_pgdir - alloc one page as PDT
static int
setup_pgdir(struct mm_struct *mm)
{
    struct Page *page;
    if ((page = alloc_page()) == NULL)
    {
        return -E_NO_MEM;
    }
    pde_t *pgdir = page2kva(page);
    memcpy(pgdir, boot_pgdir_va, PGSIZE);

    mm->pgdir = pgdir;
    return 0;
}

// put_pgdir - free the memory space of PDT
static void
put_pgdir(struct mm_struct *mm)
{
    free_page(kva2page(mm->pgdir));
}

static void
release_mm(struct mm_struct *mm)
{
    exit_mmap(mm);
    put_pgdir(mm);
    mm_destroy(mm);
}

static void
switch_pgdir(uintptr_t pgdir)
{
    write_csr(satp, ((uintptr_t)SATP_MODE_SV39 << 60) | (pgdir >> PGSHIFT));
    flush_tlb();
}

// lab5 function3 2411870
static int
copy_mm(uint32_t clone_flags, struct proc_struct *proc)
{
    struct mm_struct *source = current->mm;
    if (source == NULL)
    {
        return 0;
    }

    struct mm_struct *mm = source;
    if (!(clone_flags & CLONE_VM))
    {
        mm = mm_create();
        if (mm == NULL)
        {
            return -E_NO_MEM;
        }
        int ret = setup_pgdir(mm);
        if (ret != 0)
        {
            mm_destroy(mm);
            return ret;
        }

        lock_mm(source);
        ret = dup_mmap(mm, source);
        unlock_mm(source);
        if (ret != 0)
        {
            release_mm(mm);
            return ret;
        }
    }

    mm_count_inc(mm);
    proc->mm = mm;
    proc->pgdir = PADDR(mm->pgdir);
    return 0;
}

static void
copy_thread(struct proc_struct *proc, uintptr_t esp, struct trapframe *tf)
{
    uintptr_t stack_top = proc->kstack + KSTACKSIZE;
    proc->tf = (struct trapframe *)stack_top - 1;
    *proc->tf = *tf;
    proc->tf->gpr.a0 = 0;
    // A new kernel thread leaves the frame above its working stack.
    proc->tf->gpr.sp = (tf->status & SSTATUS_SPP) && esp == 0
                        ? (uintptr_t)proc->tf : esp;

    // 首次切换进入 forkret，由 forkrets 使用新栈上的帧执行 Trap 返回。
    proc->context.ra = (uintptr_t)forkret;
    proc->context.sp = (uintptr_t)proc->tf;
}

// lab5 function3 2411870
int
do_fork(uint32_t clone_flags, uintptr_t stack, struct trapframe *tf)
{
    struct proc_struct *proc;
    bool intr_flag;
    int ret = -E_NO_FREE_PROC;

    // 本实验是单核，关中断使容量检查与进程登记处于同一临界区。
    local_intr_save(intr_flag);
    if (nr_process >= MAX_PROCESS)
    {
        goto out;
    }

    ret = -E_NO_MEM;
    if ((proc = alloc_proc()) == NULL)
    {
        goto out;
    }
    proc->parent = current;
    if ((ret = setup_kstack(proc)) != 0)
    {
        goto free_proc;
    }
    if ((ret = copy_mm(clone_flags, proc)) != 0)
    {
        goto free_stack;
    }
    copy_thread(proc, stack, tf);

    // 完成资源初始化后才发布到查找表和调度队列，最后置为就绪。
    proc->pid = get_pid();
    hash_proc(proc);
    set_links(proc);
    wakeup_proc(proc);
    ret = proc->pid;
    goto out;

free_stack:
    put_kstack(proc);
free_proc:
    kfree(proc);
out:
    local_intr_restore(intr_flag);
    return ret;
}



// lab5 function2 2411870
static int
load_icode(const unsigned char *binary, size_t size, struct mm_struct **mm_store,
           struct trapframe *tf)
{
    struct elfhdr elf;
    if (size < sizeof(elf))
    {
        return -E_INVAL_ELF;
    }
    memcpy(&elf, binary, sizeof(elf));
    // The loader accepts statically linked, little-endian RV64 executables.
    if (elf.e_magic != ELF_MAGIC || elf.e_elf[0] != 2 || elf.e_elf[1] != 1 ||
        elf.e_type != 2 || elf.e_machine != 243 ||
        elf.e_phentsize != sizeof(struct proghdr) ||
        elf.e_phoff > size || elf.e_phnum > (size - elf.e_phoff) / sizeof(struct proghdr))
    {
        return -E_INVAL_ELF;
    }

    struct mm_struct *mm = mm_create();
    if (mm == NULL)
    {
        return -E_NO_MEM;
    }
    int ret = setup_pgdir(mm);
    if (ret != 0)
    {
        mm_destroy(mm);
        return ret;
    }

    bool executable_entry = 0;
    for (size_t i = 0; i < elf.e_phnum; i++)
    {
        struct proghdr ph;
        memcpy(&ph, binary + elf.e_phoff + i * sizeof(ph), sizeof(ph));
        if (ph.p_type != ELF_PT_LOAD)
        {
            continue;
        }
        if (ph.p_filesz > ph.p_memsz || ph.p_offset > size ||
            ph.p_filesz > size - ph.p_offset)
        {
            ret = -E_INVAL_ELF;
            goto failed;
        }
        if (ph.p_memsz == 0)
        {
            continue;
        }

        uint32_t vm_flags = 0, perm = PTE_U;
        if (ph.p_flags & (ELF_PF_R | ELF_PF_W))
        {
            vm_flags |= VM_READ;
            perm |= PTE_R;
        }
        if (ph.p_flags & ELF_PF_W)
        {
            vm_flags |= VM_WRITE;
            perm |= PTE_W;
        }
        if (ph.p_flags & ELF_PF_X)
        {
            vm_flags |= VM_EXEC;
            perm |= PTE_X;
        }
        if (vm_flags == 0)
        {
            ret = -E_INVAL_ELF;
            goto failed;
        }
        ret = mm_map(mm, ph.p_va, ph.p_memsz, vm_flags, NULL);
        if (ret != 0)
        {
            goto failed;
        }
        if ((vm_flags & VM_EXEC) && ph.p_va <= elf.e_entry &&
            elf.e_entry < ph.p_va + ph.p_memsz)
        {
            executable_entry = 1;
        }

        uintptr_t end = ph.p_va + ph.p_memsz;
        uintptr_t file_end = ph.p_va + ph.p_filesz;
        for (uintptr_t va = ROUNDDOWN(ph.p_va, PGSIZE); va < end; va += PGSIZE)
        {
            struct Page *page = pgdir_alloc_page(mm->pgdir, va, perm);
            if (page == NULL)
            {
                ret = -E_NO_MEM;
                goto failed;
            }
            unsigned char *dst = page2kva(page);
            memset(dst, 0, PGSIZE);
            uintptr_t first = va < ph.p_va ? ph.p_va : va;
            uintptr_t last = va + PGSIZE < file_end ? va + PGSIZE : file_end;
            if (first < last)
            {
                memcpy(dst + (first - va), binary + ph.p_offset + (first - ph.p_va),
                       last - first);
            }
        }
    }

    if (!executable_entry)
    {
        ret = -E_INVAL_ELF;
        goto failed;
    }

    ret = mm_map(mm, USTACKTOP - USTACKSIZE, USTACKSIZE,
                 VM_READ | VM_WRITE | VM_STACK, NULL);
    if (ret != 0)
    {
        goto failed;
    }
    // The four pages below USTACKTOP are initially present. The guard stays unmapped.
    for (uintptr_t va = USTACKTOP - 4 * PGSIZE; va < USTACKTOP; va += PGSIZE)
    {
        struct Page *page = pgdir_alloc_page(mm->pgdir, va, PTE_U | PTE_R | PTE_W);
        if (page == NULL)
        {
            ret = -E_NO_MEM;
            goto failed;
        }
        memset(page2kva(page), 0, PGSIZE);
    }

    memset(tf, 0, sizeof(*tf));
    tf->gpr.sp = USTACKTOP;
    tf->epc = elf.e_entry;
    tf->status = SSTATUS_SPIE | SSTATUS_SUM;
    *mm_store = mm;
    return 0;

failed:
    release_mm(mm);
    return ret;
}

int do_execve(const char *name, size_t len, unsigned char *binary, size_t size)
{
    struct mm_struct *old = current->mm;
    char local_name[PROC_NAME_LEN + 1] = {0};
    if (len > PROC_NAME_LEN)
    {
        len = PROC_NAME_LEN;
    }
    if (size == 0 || !user_mem_check(old, (uintptr_t)binary, size, 0) ||
        (len != 0 && !copy_from_user(old, local_name, name, len, 0)))
    {
        return -E_INVAL;
    }

    struct trapframe user_frame;
    struct mm_struct *mm;
    int ret = load_icode(binary, size, &mm, &user_frame);
    if (ret != 0)
    {
        return ret;
    }

    // Keep the source address space active until all ELF bytes have been read.
    mm_count_inc(mm);
    current->mm = mm;
    current->pgdir = PADDR(mm->pgdir);
    switch_pgdir(current->pgdir);
    if (old != NULL && mm_count_dec(old) == 0)
    {
        release_mm(old);
    }
    asm volatile("fence.i" ::: "memory");
    *current->tf = user_frame;
    set_proc_name(current, local_name);
    return 0;
}

int do_wait(int pid, int *code_store)
{
    if (code_store != NULL && !user_mem_check(current->mm, (uintptr_t)code_store,
                                               sizeof(*code_store), 1))
    {
        return -E_INVAL;
    }

    bool intr_flag;
    local_intr_save(intr_flag);
    for (;;)
    {
        if (current->flags & PF_EXITING)
        {
            do_exit(-E_KILLED);
        }

        struct proc_struct *child;
        if (pid != 0)
        {
            child = find_proc(pid);
            if (child == NULL || child->parent != current)
            {
                break;
            }
        }
        else
        {
            child = current->cptr;
            if (child == NULL)
            {
                break;
            }
            while (child != NULL && child->state != PROC_ZOMBIE)
            {
                child = child->optr;
            }
        }

        if (child != NULL && child->state == PROC_ZOMBIE)
        {
            if (code_store != NULL)
            {
                int ret = copy_to_user(current->mm, code_store, &child->exit_code,
                                       sizeof(*code_store));
                if (ret != 0)
                {
                    local_intr_restore(intr_flag);
                    return ret;
                }
            }
            unhash_proc(child);
            remove_links(child);
            put_kstack(child);
            kfree(child);
            local_intr_restore(intr_flag);
            return 0;
        }

        current->state = PROC_SLEEPING;
        current->wait_state = WT_CHILD;
        schedule();
    }
    local_intr_restore(intr_flag);
    return -E_BAD_PROC;
}


// lab5 function4 2411870
int do_exit(int error_code)
{
    if (current == idleproc || current == initproc)
    {
        panic("idle or init cannot exit");
    }

    bool intr_flag;
    local_intr_save(intr_flag);
    struct mm_struct *mm = current->mm;
    if (mm != NULL)
    {
        current->pgdir = boot_pgdir_pa;
        switch_pgdir(boot_pgdir_pa);
        current->mm = NULL;
        if (mm_count_dec(mm) == 0)
        {
            release_mm(mm);
        }
    }

    current->state = PROC_ZOMBIE;
    current->exit_code = error_code;
    struct proc_struct *parent = current->parent;
    if (parent->wait_state == WT_CHILD)
    {
        wakeup_proc(parent);
    }

    while (current->cptr != NULL)
    {
        struct proc_struct *child = current->cptr;
        current->cptr = child->optr;
        child->parent = initproc;
        child->yptr = NULL;
        child->optr = initproc->cptr;
        if (initproc->cptr != NULL)
        {
            initproc->cptr->yptr = child;
        }
        initproc->cptr = child;
        if (child->state == PROC_ZOMBIE && initproc->wait_state == WT_CHILD)
        {
            wakeup_proc(initproc);
        }
    }

    schedule();
    local_intr_restore(intr_flag);
    panic("zombie process resumed");
}

int do_yield(void)
{
    current->need_resched = 1;
    return 0;
}

int do_kill(int pid)
{
    bool intr_flag;
    local_intr_save(intr_flag);
    struct proc_struct *proc = find_proc(pid);
    int ret = -E_INVAL;
    if (proc != NULL && proc != initproc && proc->state != PROC_ZOMBIE)
    {
        ret = -E_KILLED;
        if (!(proc->flags & PF_EXITING))
        {
            proc->flags |= PF_EXITING;
            if (proc->wait_state & WT_INTERRUPTED)
            {
                wakeup_proc(proc);
            }
            ret = 0;
        }
    }
    local_intr_restore(intr_flag);
    return ret;
}


#define __KERNEL_EXECVE(name, binary, size) ({           \
    cprintf("kernel_execve: pid = %d, name = \"%s\".\n", \
            current->pid, name);                         \
    kernel_execve(name, binary, (size_t)(size));         \
})

#define KERNEL_EXECVE(x) ({                                    \
    extern unsigned char _binary_obj___user_##x##_out_start[], \
        _binary_obj___user_##x##_out_size[];                   \
    __KERNEL_EXECVE(#x, _binary_obj___user_##x##_out_start,    \
                    _binary_obj___user_##x##_out_size);        \
})

#define __KERNEL_EXECVE2(x, xstart, xsize) ({   \
    extern unsigned char xstart[], xsize[];     \
    __KERNEL_EXECVE(#x, xstart, (size_t)xsize); \
})

#define KERNEL_EXECVE2(x, xstart, xsize) __KERNEL_EXECVE2(x, xstart, xsize)


// lab5 function2 2411870
static int
kernel_execve(const char *name, unsigned char *binary, size_t size)
{
    bool intr_flag;
    local_intr_save(intr_flag);
    int ret = do_execve(name, strlen(name), binary, size);
    if (ret == 0)
    {
        // Enter the normal trap-return path using the frame reserved by copy_thread.
        forkrets(current->tf);
        panic("forkrets returned from exec");
    }
    local_intr_restore(intr_flag);
    return ret;
}

static int
user_main(void *arg)
{
#ifdef TEST
    return KERNEL_EXECVE2(TEST, TESTSTART, TESTSIZE);
#else
    return KERNEL_EXECVE(exit);
#endif
}

static int
init_main(void *arg)
{
    size_t free_pages_before = nr_free_pages();
    size_t allocated_before = kallocated();
    int pid = kernel_thread(user_main, NULL, 0);
    if (pid < 0)
    {
        panic("cannot start user process: %e", pid);
    }

    while (do_wait(0, NULL) == 0)
    {
        schedule();
    }
    cprintf("all user-mode processes have quit.\n");
    assert(current == initproc && current->cptr == NULL);
    assert(nr_process == 2);
    assert(list_next(&proc_list) == &initproc->list_link);
    assert(list_next(&initproc->list_link) == &proc_list);
    assert(nr_free_pages() == free_pages_before);
    assert(kallocated() == allocated_before);
    cprintf("init check memory pass.\n");
#ifdef DEBUG_GRADE
    // The course grader stops at readline after collecting the serial output.
    kmonitor(NULL);
#endif
    sbi_shutdown();
    for (;;)
    {
        asm volatile("wfi");
    }
}

void
proc_init(void)
{
    list_init(&proc_list);
    for (int i = 0; i < HASH_LIST_SIZE; i++)
    {
        list_init(&hash_list[i]);
    }

    if ((idleproc = alloc_proc()) == NULL)
    {
        panic("cannot allocate idleproc.\n");
    }
    // idle 沿用启动栈，作为调度器的保底线程，不进入普通进程队列。
    idleproc->pid = 0;
    idleproc->state = PROC_RUNNABLE;
    idleproc->kstack = (uintptr_t)bootstack;
    idleproc->need_resched = 1;
    set_proc_name(idleproc, "idle");
    nr_process++;
    current = idleproc;

    int pid = kernel_thread(init_main, NULL, 0);
    if (pid < 0)
    {
        panic("cannot create initproc: %e.\n", pid);
    }
    initproc = find_proc(pid);
    assert(initproc != NULL);
    set_proc_name(initproc, "init");
}

//LAB4 function3 2411292

void proc_run(struct proc_struct *proc)
{
    if (proc != current) {
        bool intr_flag;
        local_intr_save(intr_flag);
        struct proc_struct *prev = current;
        current = proc;
        // pgdir 是根页表物理地址；RV64 MODE 位于 [63:60]，PPN 不含页内偏移。
        switch_pgdir(proc->pgdir);
        switch_to(&prev->context, &proc->context);
        // 旧线程再次被选中才执行这里；嵌套 schedule 时由外层恢复原 SIE。
        // 新线程首次运行则进入 forkret，由预置 Trap frame 经 sret 恢复状态。
        local_intr_restore(intr_flag);
    }
}

void cpu_idle(void)
{
    // 被切回 idle 后继续检查请求；没有请求时不改动进程状态。
    while (1) {
        if (current->need_resched) {
            schedule();
        }
    }
}
