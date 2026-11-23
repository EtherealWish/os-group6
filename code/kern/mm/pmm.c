#include <default_pmm.h>
#include <defs.h>
#include <error.h>
#include <kmalloc.h>
#include <memlayout.h>
#include <mmu.h>
#include <pmm.h>
#include <proc.h>
#include <sbi.h>
#include <dtb.h>
#include <stdio.h>
#include <string.h>
#include <sync.h>
#include <vmm.h>
#include <riscv.h>

// virtual address of physical page array
struct Page *pages;
// amount of physical memory (in pages)
size_t npage = 0;
// The kernel image is mapped at VA=KERNBASE and PA=info.base
uint_t va_pa_offset;
// memory starts at 0x80000000 in RISC-V
const size_t nbase = DRAM_BASE / PGSIZE;

// virtual address of boot-time page directory
pde_t *boot_pgdir_va = NULL;
// physical address of boot-time page directory
uintptr_t boot_pgdir_pa;

// physical memory management
const struct pmm_manager *pmm_manager;

static void check_alloc_page(void);
static void check_pgdir(void);
static void check_boot_pgdir(void);

// init_pmm_manager - initialize a pmm_manager instance
static void init_pmm_manager(void)
{
    pmm_manager = &default_pmm_manager;
    cprintf("memory management: %s\n", pmm_manager->name);
    pmm_manager->init();
}

// init_memmap - call pmm->init_memmap to build Page struct for free memory
static void init_memmap(struct Page *base, size_t n)
{
    pmm_manager->init_memmap(base, n);
}

// alloc_pages - call pmm->alloc_pages to allocate a continuous n*PAGESIZE
// memory
struct Page *alloc_pages(size_t n)
{
    struct Page *page = NULL;
    bool intr_flag;
    local_intr_save(intr_flag);
    {
        page = pmm_manager->alloc_pages(n);
    }
    local_intr_restore(intr_flag);
    return page;
}

// free_pages - call pmm->free_pages to free a continuous n*PAGESIZE memory
void free_pages(struct Page *base, size_t n)
{
    bool intr_flag;
    local_intr_save(intr_flag);
    {
        pmm_manager->free_pages(base, n);
    }
    local_intr_restore(intr_flag);
}

// nr_free_pages - call pmm->nr_free_pages to get the size (nr*PAGESIZE)
// of current free memory
size_t nr_free_pages(void)
{
    size_t ret;
    bool intr_flag;
    local_intr_save(intr_flag);
    {
        ret = pmm_manager->nr_free_pages();
    }
    local_intr_restore(intr_flag);
    return ret;
}

//lab2 function1 2411870
/* page_init - detect physical memory from DTB, build the Page array right
 * after the kernel image, and hand the remaining whole pages to pmm_manager */
static void page_init(void) {
    extern char end[];

    uint64_t mem_begin = get_memory_base();
    uint64_t mem_size = get_memory_size();
    uint64_t mem_end = mem_begin + mem_size;
    if (mem_begin == 0 || mem_size == 0 || mem_end < mem_begin) {
        panic("page_init: invalid memory info from DTB (base 0x%016lx, size 0x%016lx)",
              mem_begin, mem_size);
    }

    va_pa_offset = PHYSICAL_MEMORY_OFFSET;

    cprintf("physical memory map:\n");
    cprintf("  memory: 0x%016lx, [0x%016lx, 0x%016lx].\n", mem_size, mem_begin,
            mem_end - 1);

    // the kernel can only manage physical memory below KERNTOP's physical address
    uint64_t maxpa = mem_end;
    if (maxpa > KERNTOP - va_pa_offset) {
        maxpa = KERNTOP - va_pa_offset;
    }
    if (maxpa <= DRAM_BASE) {
        panic("page_init: no manageable memory above DRAM_BASE");
    }

    npage = maxpa / PGSIZE;
    pages = (struct Page *)ROUNDUP((uintptr_t)end, PGSIZE);

    // pages[0, npage - nbase) describes physical pages [nbase, npage)
    uintptr_t freemem = PADDR((uintptr_t)(pages + (npage - nbase)));
    if (freemem > maxpa) {
        panic("page_init: not enough memory for the Page array");
    }

    // the array lies beyond BSS and is not zeroed: give every field a known
    // value, and keep every page reserved until the free area is registered
    for (size_t i = 0; i < npage - nbase; i++) {
        pages[i].ref = 0;
        pages[i].flags = 0;
        pages[i].property = 0;
        SetPageReserved(pages + i);
    }

    uintptr_t free_begin = ROUNDUP(freemem, PGSIZE);
    uintptr_t free_end = ROUNDDOWN(maxpa, PGSIZE);
    if (free_begin < free_end) {
        init_memmap(pa2page(free_begin), (free_end - free_begin) / PGSIZE);
    }
}

// boot_map_segment - setup&enable the paging mechanism
// parameters
//  la:   linear address of this memory need to map (after x86 segment map)
//  size: memory size
//  pa:   physical address of this memory
//  perm: permission of this memory
static void boot_map_segment(pde_t *pgdir, uintptr_t la, size_t size,
                             uintptr_t pa, uint32_t perm)
{
    assert(PGOFF(la) == PGOFF(pa));
    size_t n = ROUNDUP(size + PGOFF(la), PGSIZE) / PGSIZE;
    la = ROUNDDOWN(la, PGSIZE);
    pa = ROUNDDOWN(pa, PGSIZE);
    for (; n > 0; n--, la += PGSIZE, pa += PGSIZE)
    {
        pte_t *ptep = get_pte(pgdir, la, 1);
        assert(ptep != NULL);
        *ptep = pte_create(pa >> PGSHIFT, PTE_V | perm);
    }
}

// boot_alloc_page - allocate one page using pmm->alloc_pages(1)
// return value: the kernel virtual address of this allocated page
// note: this function is used to get the memory for PDT(Page Directory
// Table)&PT(Page Table)
static void *boot_alloc_page(void)
{
    struct Page *p = alloc_page();
    if (p == NULL)
    {
        panic("boot_alloc_page failed.\n");
    }
    return page2kva(p);
}

// pmm_init - setup a pmm to manage physical memory, build PDT&PT to setup
// paging mechanism
//         - check the correctness of pmm & paging mechanism, print PDT&PT
void pmm_init(void)
{
    // We need to alloc/free the physical memory (granularity is 4KB or other
    // size).
    // So a framework of physical memory manager (struct pmm_manager)is defined
    // in pmm.h
    // First we should init a physical memory manager(pmm) based on the
    // framework.
    // Then pmm can alloc/free the physical memory.
    // Now the first_fit/best_fit/worst_fit/buddy_system pmm are available.
    init_pmm_manager();

    // detect physical memory space, reserve already used memory,
    // then use pmm->init_memmap to create free page list
    page_init();

    // use pmm->check to verify the correctness of the alloc/free function in a
    // pmm
    check_alloc_page();

    // create boot_pgdir, an initial page directory(Page Directory Table, PDT)
    extern char boot_page_table_sv39[];
    boot_pgdir_va = (pte_t *)boot_page_table_sv39;
    boot_pgdir_pa = PADDR(boot_pgdir_va);

    check_pgdir();

    static_assert(KERNBASE % PTSIZE == 0 && KERNTOP % PTSIZE == 0);

    // now the basic virtual memory map(see memalyout.h) is established.
    // check the correctness of the basic virtual memory map.
    check_boot_pgdir();

    kmalloc_init();
}

// get_pte - get pte and return the kernel virtual address of this pte for la
//        - if the PT contians this pte didn't exist, alloc a page for PT
// parameter:
//  pgdir:  the kernel virtual base address of PDT
//  la:     the linear address need to map
//  create: a logical value to decide if alloc a page for PT
// return vaule: the kernel virtual address of this pte
pte_t *get_pte(pde_t *pgdir, uintptr_t la, bool create)
{
    pde_t *pdep1 = &pgdir[PDX1(la)];
    if (!(*pdep1 & PTE_V))
    {
        struct Page *page;
        if (!create || (page = alloc_page()) == NULL)
        {
            return NULL;
        }
        set_page_ref(page, 1);
        uintptr_t pa = page2pa(page);
        memset(KADDR(pa), 0, PGSIZE);
        *pdep1 = pte_create(page2ppn(page), PTE_U | PTE_V);
    }

    pde_t *pdep0 = &((pde_t *)KADDR(PDE_ADDR(*pdep1)))[PDX0(la)];
    if (!(*pdep0 & PTE_V))
    {
        struct Page *page;
        if (!create || (page = alloc_page()) == NULL)
        {
            return NULL;
        }
        set_page_ref(page, 1);
        uintptr_t pa = page2pa(page);
        memset(KADDR(pa), 0, PGSIZE);
        *pdep0 = pte_create(page2ppn(page), PTE_U | PTE_V);
    }
    return &((pte_t *)KADDR(PDE_ADDR(*pdep0)))[PTX(la)];
}

// get_page - get related Page struct for linear address la using PDT pgdir
struct Page *get_page(pde_t *pgdir, uintptr_t la, pte_t **ptep_store)
{
    pte_t *ptep = get_pte(pgdir, la, 0);
    if (ptep_store != NULL)
    {
        *ptep_store = ptep;
    }
    if (ptep != NULL && *ptep & PTE_V)
    {
        return pte2page(*ptep);
    }
    return NULL;
}

void unmap_range(pde_t *pgdir, uintptr_t start, uintptr_t end)
{
    assert(start % PGSIZE == 0 && end % PGSIZE == 0);
    assert(USER_ACCESS(start, end));

    do
    {
        pte_t *ptep = get_pte(pgdir, start, 0);
        if (ptep == NULL)
        {
            start = ROUNDDOWN(start + PTSIZE, PTSIZE);
            continue;
        }
        if (*ptep != 0)
        {
            page_remove(pgdir, start);
        }
        start += PGSIZE;
    } while (start != 0 && start < end);
}

static bool
page_table_empty(const pte_t *table)
{
    for (size_t i = 0; i < NPTEENTRY; i++)
    {
        if (table[i] & PTE_V)
        {
            return 0;
        }
    }
    return 1;
}

void exit_range(pde_t *pgdir, uintptr_t start, uintptr_t end)
{
    assert(start % PGSIZE == 0 && end % PGSIZE == 0);
    assert(USER_ACCESS(start, end));

    for (uintptr_t addr = ROUNDDOWN(start, PDSIZE); addr < end; addr += PDSIZE)
    {
        pde_t *root = &pgdir[PDX1(addr)];
        if (!(*root & PTE_V))
        {
            continue;
        }

        struct Page *directory = pde2page(*root);
        pde_t *entries = page2kva(directory);
        uintptr_t lower = start > addr ? ROUNDDOWN(start, PTSIZE) : addr;
        uintptr_t upper = end < addr + PDSIZE ? end : addr + PDSIZE;
        for (uintptr_t region = lower; region < upper; region += PTSIZE)
        {
            pde_t *entry = &entries[PDX0(region)];
            if (*entry & PTE_V)
            {
                struct Page *table = pde2page(*entry);
                if (page_table_empty(page2kva(table)))
                {
                    *entry = 0;
                    free_page(table);
                }
            }
        }
        // Other VMAs can still own leaf tables under this directory.
        if (page_table_empty(entries))
        {
            *root = 0;
            free_page(directory);
        }
    }
}

// lab5 function3 2411870
// Stage child mappings. The parent permissions are committed after all VMAs
// have been copied successfully and before the child is published.
int copy_range(pde_t *to, pde_t *from, uintptr_t start, uintptr_t end)
{
    assert(start % PGSIZE == 0 && end % PGSIZE == 0);
    assert(USER_ACCESS(start, end));

    while (start < end)
    {
        pte_t *pte = get_pte(from, start, 0);
        if (pte == NULL)
        {
            start = ROUNDDOWN(start + PTSIZE, PTSIZE);
            continue;
        }
        if ((*pte & (PTE_V | PTE_U)) == (PTE_V | PTE_U))
        {
            uint32_t perm = *pte & (PTE_USER | PTE_COW);
            if (perm & PTE_W)
            {
                perm = (perm & ~PTE_W) | PTE_COW;
            }
            int ret = page_insert(to, pte2page(*pte), start, perm);
            if (ret != 0)
            {
                return ret;
            }
        }
        start += PGSIZE;
    }
    return 0;
}

void cow_protect_range(pde_t *pgdir, uintptr_t start, uintptr_t end)
{
    while (start < end)
    {
        pte_t *pte = get_pte(pgdir, start, 0);
        if (pte == NULL)
        {
            start = ROUNDDOWN(start + PTSIZE, PTSIZE);
            continue;
        }
        if ((*pte & (PTE_V | PTE_U | PTE_W)) == (PTE_V | PTE_U | PTE_W))
        {
            *pte = (*pte & ~PTE_W) | PTE_COW;
            tlb_invalidate(pgdir, start);
        }
        start += PGSIZE;
    }
}

//LAB4 function1 2412219
void page_remove(pde_t *pgdir, uintptr_t la)
{
    // 查找现有页表项。
    pte_t *ptep = get_pte(pgdir, la, 0);
    if (ptep == NULL || (*ptep & PTE_V) == 0)
    {
        return;
    }

    struct Page *page = pte2page(*ptep);

    // 撤销映射，使对应地址的旧 TLB 缓存失效。
    *ptep = 0;
    tlb_invalidate(pgdir, la);

    // 最后一个映射被撤销时，释放物理页。
    if (page_ref_dec(page) == 0)
    {
        free_page(page);
    }
}

int page_insert(pde_t *pgdir, struct Page *page,
                uintptr_t la, uint32_t perm)
{
    // 中间级页表缺失时，允许分配页表页。
    pte_t *ptep = get_pte(pgdir, la, 1);
    if (ptep == NULL)
    {
        return -E_NO_MEM;
    }

    if ((*ptep & PTE_V) == 0)
    {
        page_ref_inc(page);
    }
    else if (pte2page(*ptep) != page)
    {
        page_remove(pgdir, la);
        page_ref_inc(page);
    }

    // 同页重复映射时引用数不变，权限仍按本次参数更新。
    *ptep = pte_create(page2ppn(page), perm);
    tlb_invalidate(pgdir, la);
    return 0;
}


// Invalidate this VA in the active hart’s TLB.
void tlb_invalidate(pde_t *pgdir, uintptr_t la)
{
    asm volatile("sfence.vma %0" : : "r"(la) : "memory");
}

// pgdir_alloc_page - call alloc_page & page_insert functions to
//                  - allocate a page size memory & setup an addr map
//                  - pa<->la with linear address la and the PDT pgdir
struct Page *pgdir_alloc_page(pde_t *pgdir, uintptr_t la, uint32_t perm)
{
    struct Page *page = alloc_page();
    if (page != NULL)
    {
        if (page_insert(pgdir, page, la, perm) != 0)
        {
            free_page(page);
            return NULL;
        }
        // swap_map_swappable(check_mm_struct, la, page, 0);
        page->pra_vaddr = la;
        assert(page_ref(page) == 1);
        // cprintf("get No. %d  page: pra_vaddr %x, pra_link.prev %x,
        // pra_link_next %x in pgdir_alloc_page\n", (page-pages),
        // page->pra_vaddr,page->pra_page_link.prev,
        // page->pra_page_link.next);
    }

    return page;
}

static void check_alloc_page(void)
{
    pmm_manager->check();
    cprintf("check_alloc_page() succeeded!\n");
}

static void check_pgdir(void)
{
    // assert(npage <= KMEMSIZE / PGSIZE);
    // The memory starts at 2GB in RISC-V
    // so npage is always larger than KMEMSIZE / PGSIZE
    size_t nr_free_store;

    nr_free_store = nr_free_pages();

    assert(npage <= KERNTOP / PGSIZE);
    assert(boot_pgdir_va != NULL && (uint32_t)PGOFF(boot_pgdir_va) == 0);
    assert(get_page(boot_pgdir_va, 0x0, NULL) == NULL);

    struct Page *p1, *p2;
    p1 = alloc_page();
    assert(page_insert(boot_pgdir_va, p1, 0x0, 0) == 0);

    pte_t *ptep;
    assert((ptep = get_pte(boot_pgdir_va, 0x0, 0)) != NULL);
    assert(pte2page(*ptep) == p1);
    assert(page_ref(p1) == 1);

    ptep = (pte_t *)KADDR(PDE_ADDR(boot_pgdir_va[0]));
    ptep = (pte_t *)KADDR(PDE_ADDR(ptep[0])) + 1;
    assert(get_pte(boot_pgdir_va, PGSIZE, 0) == ptep);

    p2 = alloc_page();
    assert(page_insert(boot_pgdir_va, p2, PGSIZE, PTE_U | PTE_W) == 0);
    assert((ptep = get_pte(boot_pgdir_va, PGSIZE, 0)) != NULL);
    assert(*ptep & PTE_U);
    assert(*ptep & PTE_W);
    assert(boot_pgdir_va[0] & PTE_U);
    assert(page_ref(p2) == 1);

    assert(page_insert(boot_pgdir_va, p1, PGSIZE, 0) == 0);
    assert(page_ref(p1) == 2);
    assert(page_ref(p2) == 0);
    assert((ptep = get_pte(boot_pgdir_va, PGSIZE, 0)) != NULL);
    assert(pte2page(*ptep) == p1);
    assert((*ptep & PTE_U) == 0);

    page_remove(boot_pgdir_va, 0x0);
    assert(page_ref(p1) == 1);
    assert(page_ref(p2) == 0);

    page_remove(boot_pgdir_va, PGSIZE);
    assert(page_ref(p1) == 0);
    assert(page_ref(p2) == 0);

    assert(page_ref(pde2page(boot_pgdir_va[0])) == 1);

    pde_t *pd1 = boot_pgdir_va, *pd0 = page2kva(pde2page(boot_pgdir_va[0]));
    free_page(pde2page(pd0[0]));
    free_page(pde2page(pd1[0]));
    boot_pgdir_va[0] = 0;
    flush_tlb();

    assert(nr_free_store == nr_free_pages());

    cprintf("check_pgdir() succeeded!\n");
}

static void check_boot_pgdir(void)
{
    size_t nr_free_store;
    pte_t *ptep;
    int i;

    nr_free_store = nr_free_pages();

    for (i = ROUNDDOWN(KERNBASE, PGSIZE); i < npage * PGSIZE; i += PGSIZE)
    {
        assert((ptep = get_pte(boot_pgdir_va, (uintptr_t)KADDR(i), 0)) != NULL);
        assert(PTE_ADDR(*ptep) == i);
    }

    assert(boot_pgdir_va[0] == 0);

    struct Page *p;
    p = alloc_page();
    assert(page_insert(boot_pgdir_va, p, 0x100, PTE_W | PTE_R) == 0);
    assert(page_ref(p) == 1);
    assert(page_insert(boot_pgdir_va, p, 0x100 + PGSIZE, PTE_W | PTE_R) == 0);
    assert(page_ref(p) == 2);

    const char *str = "ucore: Hello world!!";
    strcpy((void *)0x100, str);
    assert(strcmp((void *)0x100, (void *)(0x100 + PGSIZE)) == 0);

    *(char *)(page2kva(p) + 0x100) = '\0';
    assert(strlen((const char *)0x100) == 0);

    pde_t *pd1 = boot_pgdir_va, *pd0 = page2kva(pde2page(boot_pgdir_va[0]));
    free_page(p);
    free_page(pde2page(pd0[0]));
    free_page(pde2page(pd1[0]));
    boot_pgdir_va[0] = 0;
    flush_tlb();

    assert(nr_free_store == nr_free_pages());

    cprintf("check_boot_pgdir() succeeded!\n");
}

void print_pgdir(void)
{
    if (current == NULL || current->mm == NULL)
    {
        return;
    }

    cprintf("user virtual ranges:\n");
    pde_t *pgdir = current->mm->pgdir;
    uintptr_t start = USERBASE;
    while (start < USERTOP)
    {
        pte_t *pte = get_pte(pgdir, start, 0);
        if (pte == NULL)
        {
            start = ROUNDDOWN(start + PTSIZE, PTSIZE);
            continue;
        }
        if (!(*pte & PTE_V))
        {
            start += PGSIZE;
            continue;
        }

        uint32_t perm = *pte & PTE_USER;
        uintptr_t end = start;
        do
        {
            end += PGSIZE;
            pte = end < USERTOP ? get_pte(pgdir, end, 0) : NULL;
        } while (pte != NULL && (*pte & PTE_USER) == perm);

        cprintf("  %p-%p %c%c%c%c\n", (void *)start, (void *)end,
                perm & PTE_U ? 'u' : '-', perm & PTE_R ? 'r' : '-',
                perm & PTE_W ? 'w' : '-', perm & PTE_X ? 'x' : '-');
        start = end;
    }
}
