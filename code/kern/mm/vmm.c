#include <vmm.h>
#include <sync.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include <error.h>
#include <pmm.h>
#include <riscv.h>
#include <kmalloc.h>

/*
  vmm design include two parts: mm_struct (mm) & vma_struct (vma)
  mm is the memory manager for the set of continuous virtual memory
  area which have the same PDT. vma is a continuous virtual memory area.
  There a linear link list for vma & a redblack link list for vma in mm.
---------------
  mm related functions:
   golbal functions
     struct mm_struct * mm_create(void)
     void mm_destroy(struct mm_struct *mm)
     int do_pgfault(struct mm_struct *mm, uint32_t error_code, uintptr_t addr)
--------------
  vma related functions:
   global functions
     struct vma_struct * vma_create (uintptr_t vm_start, uintptr_t vm_end,...)
     void insert_vma_struct(struct mm_struct *mm, struct vma_struct *vma)
     struct vma_struct * find_vma(struct mm_struct *mm, uintptr_t addr)
   local functions
     inline void check_vma_overlap(struct vma_struct *prev, struct vma_struct *next)
---------------
   check correctness functions
     void check_vmm(void);
     void check_vma_struct(void);
     void check_pgfault(void);
*/

static void check_vmm(void);
static void check_vma_struct(void);

// mm_create -  alloc a mm_struct & initialize it.
struct mm_struct *
mm_create(void)
{
    struct mm_struct *mm = kmalloc(sizeof(struct mm_struct));

    if (mm != NULL)
    {
        list_init(&(mm->mmap_list));
        mm->mmap_cache = NULL;
        mm->pgdir = NULL;
        mm->map_count = 0;

        mm->sm_priv = NULL;

        set_mm_count(mm, 0);
        lock_init(&(mm->mm_lock));
    }
    return mm;
}

// vma_create - alloc a vma_struct & initialize it. (addr range: vm_start~vm_end)
struct vma_struct *
vma_create(uintptr_t vm_start, uintptr_t vm_end, uint32_t vm_flags)
{
    struct vma_struct *vma = kmalloc(sizeof(struct vma_struct));

    if (vma != NULL)
    {
        vma->vm_start = vm_start;
        vma->vm_end = vm_end;
        vma->vm_flags = vm_flags;
    }
    return vma;
}

// find_vma - find a vma  (vma->vm_start <= addr <= vma_vm_end)
struct vma_struct *
find_vma(struct mm_struct *mm, uintptr_t addr)
{
    struct vma_struct *vma = NULL;
    if (mm != NULL)
    {
        vma = mm->mmap_cache;
        if (!(vma != NULL && vma->vm_start <= addr && vma->vm_end > addr))
        {
            bool found = 0;
            list_entry_t *list = &(mm->mmap_list), *le = list;
            while ((le = list_next(le)) != list)
            {
                vma = le2vma(le, list_link);
                if (vma->vm_start <= addr && addr < vma->vm_end)
                {
                    found = 1;
                    break;
                }
            }
            if (!found)
            {
                vma = NULL;
            }
        }
        if (vma != NULL)
        {
            mm->mmap_cache = vma;
        }
    }
    return vma;
}

// check_vma_overlap - check if vma1 overlaps vma2 ?
static inline void
check_vma_overlap(struct vma_struct *prev, struct vma_struct *next)
{
    assert(prev->vm_start < prev->vm_end);
    assert(prev->vm_end <= next->vm_start);
    assert(next->vm_start < next->vm_end);
}

// insert_vma_struct -insert vma in mm's list link
void insert_vma_struct(struct mm_struct *mm, struct vma_struct *vma)
{
    assert(vma->vm_start < vma->vm_end);
    list_entry_t *list = &(mm->mmap_list);
    list_entry_t *le_prev = list, *le_next;

    list_entry_t *le = list;
    while ((le = list_next(le)) != list)
    {
        struct vma_struct *mmap_prev = le2vma(le, list_link);
        if (mmap_prev->vm_start > vma->vm_start)
        {
            break;
        }
        le_prev = le;
    }

    le_next = list_next(le_prev);

    /* check overlap */
    if (le_prev != list)
    {
        check_vma_overlap(le2vma(le_prev, list_link), vma);
    }
    if (le_next != list)
    {
        check_vma_overlap(vma, le2vma(le_next, list_link));
    }

    vma->vm_mm = mm;
    list_add_after(le_prev, &(vma->list_link));

    mm->map_count++;
}

// mm_destroy - free mm and mm internal fields
void mm_destroy(struct mm_struct *mm)
{
    assert(mm_count(mm) == 0);

    list_entry_t *list = &(mm->mmap_list), *le;
    while ((le = list_next(list)) != list)
    {
        list_del(le);
        kfree(le2vma(le, list_link)); // kfree vma
    }
    kfree(mm); // kfree mm
    mm = NULL;
}

int mm_map(struct mm_struct *mm, uintptr_t addr, size_t len, uint32_t vm_flags,
           struct vma_struct **vma_store)
{
    if (len == 0 || addr >= USERTOP || len > USERTOP - addr)
    {
        return -E_INVAL;
    }
    uintptr_t start = ROUNDDOWN(addr, PGSIZE), end = ROUNDUP(addr + len, PGSIZE);
    if (!USER_ACCESS(start, end))
    {
        return -E_INVAL;
    }

    assert(mm != NULL);

    int ret = -E_INVAL;

    list_entry_t *list = &mm->mmap_list, *entry = list;
    while ((entry = list_next(entry)) != list)
    {
        struct vma_struct *existing = le2vma(entry, list_link);
        if (start < existing->vm_end && existing->vm_start < end)
        {
            return -E_INVAL;
        }
        if (existing->vm_start >= end)
        {
            break;
        }
    }
    struct vma_struct *vma;
    ret = -E_NO_MEM;

    if ((vma = vma_create(start, end, vm_flags)) == NULL)
    {
        goto out;
    }
    insert_vma_struct(mm, vma);
    if (vma_store != NULL)
    {
        *vma_store = vma;
    }
    ret = 0;

out:
    return ret;
}

// lab5 function3 2411870
int dup_mmap(struct mm_struct *to, struct mm_struct *from)
{
    list_entry_t *list = &from->mmap_list, *entry = list;
    while ((entry = list_prev(entry)) != list)
    {
        struct vma_struct *source = le2vma(entry, list_link);
        struct vma_struct *copy = vma_create(source->vm_start, source->vm_end,
                                             source->vm_flags);
        if (copy == NULL)
        {
            return -E_NO_MEM;
        }
        // Make this range visible to exit_mmap before allocating any pages.
        insert_vma_struct(to, copy);
        int ret = copy_range(to->pgdir, from->pgdir, copy->vm_start, copy->vm_end);
        if (ret != 0)
        {
            return ret;
        }
    }
    // No allocation can fail in this pass. The child is still unpublished.
    while ((entry = list_next(entry)) != list)
    {
        struct vma_struct *vma = le2vma(entry, list_link);
        if (vma->vm_flags & VM_WRITE)
        {
            cow_protect_range(from->pgdir, vma->vm_start, vma->vm_end);
        }
    }
    return 0;
}



void exit_mmap(struct mm_struct *mm)
{
    assert(mm != NULL && mm_count(mm) == 0);
    pde_t *pgdir = mm->pgdir;
    list_entry_t *list = &(mm->mmap_list), *le = list;
    while ((le = list_next(le)) != list)
    {
        struct vma_struct *vma = le2vma(le, list_link);
        unmap_range(pgdir, vma->vm_start, vma->vm_end);
    }
    while ((le = list_next(le)) != list)
    {
        struct vma_struct *vma = le2vma(le, list_link);
        exit_range(pgdir, vma->vm_start, vma->vm_end);
    }
}

bool copy_from_user(struct mm_struct *mm, void *dst, const void *src, size_t len, bool writable)
{
    if (!user_mem_check(mm, (uintptr_t)src, len, writable))
    {
        return 0;
    }
    memcpy(dst, src, len);
    return 1;
}

int cow_resolve(struct mm_struct *mm, uintptr_t addr)
{
    bool intr_flag;
    local_intr_save(intr_flag);
    int ret = -E_INVAL;
    struct vma_struct *vma = find_vma(mm, addr);
    if (vma == NULL || !(vma->vm_flags & VM_WRITE))
    {
        goto out;
    }

    uintptr_t va = ROUNDDOWN(addr, PGSIZE);
    pte_t *pte = get_pte(mm->pgdir, va, 0);
    const pte_t required = PTE_V | PTE_U | PTE_R | PTE_COW;
    if (pte == NULL || (*pte & (required | PTE_W)) != required)
    {
        goto out;
    }

    struct Page *old = pte2page(*pte);
    if (page_ref(old) == 1)
    {
        *pte = (*pte | PTE_W) & ~PTE_COW;
        tlb_invalidate(mm->pgdir, va);
        ret = 0;
        goto out;
    }

    struct Page *page = alloc_page();
    if (page == NULL)
    {
        ret = -E_NO_MEM;
        goto out;
    }
    memcpy(page2kva(page), page2kva(old), PGSIZE);
    ret = page_insert(mm->pgdir, page, va, (*pte & PTE_USER) | PTE_W);
    if (ret != 0)
    {
        free_page(page);
    }

out:
    local_intr_restore(intr_flag);
    return ret;
}

int copy_to_user(struct mm_struct *mm, void *dst, const void *src, size_t len)
{
    bool intr_flag;
    local_intr_save(intr_flag);
    uintptr_t addr = (uintptr_t)dst;
    int ret = -E_INVAL;
    if (!user_mem_check(mm, addr, len, 1))
    {
        goto out;
    }
    if (mm != NULL)
    {
        // Resolve the whole buffer before writing even its first byte.
        for (uintptr_t va = ROUNDDOWN(addr, PGSIZE); va < addr + len; va += PGSIZE)
        {
            pte_t *pte = get_pte(mm->pgdir, va, 0);
            if (*pte & PTE_COW)
            {
                ret = cow_resolve(mm, va);
                if (ret != 0)
                {
                    goto out;
                }
            }
        }
    }
    memcpy(dst, src, len);
    ret = 0;

out:
    local_intr_restore(intr_flag);
    return ret;
}

// vmm_init - initialize virtual memory management
//          - now just call check_vmm to check correctness of vmm
void vmm_init(void)
{
    check_vmm();
}

// check_vmm - check correctness of vmm
static void
check_vmm(void)
{
    // size_t nr_free_pages_store = nr_free_pages();

    check_vma_struct();
    // check_pgfault();

    cprintf("check_vmm() succeeded.\n");
}

static void
check_vma_struct(void)
{
    // size_t nr_free_pages_store = nr_free_pages();

    struct mm_struct *mm = mm_create();
    assert(mm != NULL);

    int step1 = 10, step2 = step1 * 10;

    int i;
    for (i = step1; i >= 1; i--)
    {
        struct vma_struct *vma = vma_create(i * 5, i * 5 + 2, 0);
        assert(vma != NULL);
        insert_vma_struct(mm, vma);
    }

    for (i = step1 + 1; i <= step2; i++)
    {
        struct vma_struct *vma = vma_create(i * 5, i * 5 + 2, 0);
        assert(vma != NULL);
        insert_vma_struct(mm, vma);
    }

    list_entry_t *le = list_next(&(mm->mmap_list));

    for (i = 1; i <= step2; i++)
    {
        assert(le != &(mm->mmap_list));
        struct vma_struct *mmap = le2vma(le, list_link);
        assert(mmap->vm_start == i * 5 && mmap->vm_end == i * 5 + 2);
        le = list_next(le);
    }

    for (i = 5; i <= 5 * step2; i += 5)
    {
        struct vma_struct *vma1 = find_vma(mm, i);
        assert(vma1 != NULL);
        struct vma_struct *vma2 = find_vma(mm, i + 1);
        assert(vma2 != NULL);
        struct vma_struct *vma3 = find_vma(mm, i + 2);
        assert(vma3 == NULL);
        struct vma_struct *vma4 = find_vma(mm, i + 3);
        assert(vma4 == NULL);
        struct vma_struct *vma5 = find_vma(mm, i + 4);
        assert(vma5 == NULL);

        assert(vma1->vm_start == i && vma1->vm_end == i + 2);
        assert(vma2->vm_start == i && vma2->vm_end == i + 2);
    }

    for (i = 4; i >= 0; i--)
    {
        struct vma_struct *vma_below_5 = find_vma(mm, i);
        if (vma_below_5 != NULL)
        {
            cprintf("vma_below_5: i %x, start %x, end %x\n", i, vma_below_5->vm_start, vma_below_5->vm_end);
        }
        assert(vma_below_5 == NULL);
    }

    mm_destroy(mm);

    cprintf("check_vma_struct() succeeded!\n");
}
bool user_mem_check(struct mm_struct *mm, uintptr_t addr, size_t len, bool write)
{
    uintptr_t end = addr + len;
    if (mm == NULL)
    {
        return KERN_ACCESS(addr, end);
    }
    if (!USER_ACCESS(addr, end))
    {
        return 0;
    }

    uintptr_t start = addr;
    while (start < end)
    {
        struct vma_struct *vma = find_vma(mm, start);
        if (vma == NULL || !(vma->vm_flags & (write ? VM_WRITE : VM_READ)))
        {
            return 0;
        }
        uintptr_t limit = end < vma->vm_end ? end : vma->vm_end;
        pte_t required = PTE_V | PTE_U | PTE_R;
        for (uintptr_t page = ROUNDDOWN(start, PGSIZE); page < limit; page += PGSIZE)
        {
            pte_t *pte = get_pte(mm->pgdir, page, 0);
            if (pte == NULL || (*pte & required) != required ||
                (write && !(*pte & (PTE_W | PTE_COW))))
            {
                return 0;
            }
        }
        start = limit;
    }
    return 1;
}
