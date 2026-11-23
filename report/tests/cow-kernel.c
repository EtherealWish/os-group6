#include <assert.h>
#include <error.h>
#include <kmalloc.h>
#include <pmm.h>
#include <riscv.h>
#include <stdio.h>
#include <string.h>
#include <sync.h>
#include <vmm.h>

extern int cow_fail_page_after;

static struct mm_struct *new_mm(void)
{
    struct mm_struct *mm = mm_create();
    struct Page *root = alloc_page();
    assert(mm != NULL && root != NULL);
    mm->pgdir = page2kva(root);
    memcpy(mm->pgdir, boot_pgdir_va, PGSIZE);
    return mm;
}

static void free_mm(struct mm_struct *mm)
{
    exit_mmap(mm);
    free_page(kva2page(mm->pgdir));
    mm_destroy(mm);
}

static struct Page *mapped_page(struct mm_struct *mm, uintptr_t va, int flags)
{
    pte_t *pte;
    struct Page *page = get_page(mm->pgdir, va, &pte);
    assert(page != NULL);
    assert((*pte & (PTE_R | PTE_W | PTE_X | PTE_U | PTE_COW)) == flags);
    return page;
}

void cow_check(void)
{
    const uintptr_t text = USERBASE;
    const uintptr_t data = USERBASE + PTSIZE;
    const uint32_t ro = PTE_U | PTE_R | PTE_X;
    const uint32_t rw = PTE_U | PTE_R | PTE_W;
    const uint32_t cow = PTE_U | PTE_R | PTE_COW;
    size_t free_before = nr_free_pages(), heap_before = kallocated();
    struct mm_struct *parent = new_mm();
    struct mm_struct *child = new_mm();
    struct mm_struct *last = new_mm();

    assert(mm_map(parent, text, PGSIZE, VM_READ | VM_EXEC, NULL) == 0);
    assert(mm_map(parent, data, 2 * PGSIZE, VM_READ | VM_WRITE, NULL) == 0);
    struct Page *code = pgdir_alloc_page(parent->pgdir, text, ro);
    struct Page *first = pgdir_alloc_page(parent->pgdir, data, rw);
    struct Page *second = pgdir_alloc_page(parent->pgdir, data + PGSIZE, rw);
    assert(code != NULL && first != NULL && second != NULL);
    memset(page2kva(first), 0x31, PGSIZE);
    memset(page2kva(second), 0x32, PGSIZE);

    // The first VMA is staged before the next leaf-table allocation fails.
    cow_fail_page_after = 2;
    assert(dup_mmap(child, parent) == -E_NO_MEM);
    cow_fail_page_after = -1;
    assert(mapped_page(child, data, cow) == first && page_ref(first) == 2);
    assert(mapped_page(parent, data, rw) == first);
    free_mm(child);
    assert(page_ref(first) == 1 && page_ref(second) == 1);

    child = new_mm();
    assert(dup_mmap(child, parent) == 0 && dup_mmap(last, child) == 0);
    assert(mapped_page(parent, data, cow) == first);
    assert(mapped_page(child, data, cow) == first);
    assert(mapped_page(last, data, cow) == first && page_ref(first) == 3);
    assert(mapped_page(last, text, ro) == code && page_ref(code) == 3);
    assert(cow_resolve(last, text) == -E_INVAL);
    assert(cow_resolve(last, data + 2 * PGSIZE) == -E_INVAL);
    assert(!user_mem_check(last, text, 1, 1));
    cprintf("COW: shared PPNs, nested fork, immutable text and fork rollback passed.\n");

    cow_fail_page_after = 0;
    assert(cow_resolve(parent, data) == -E_NO_MEM);
    cow_fail_page_after = -1;
    assert(mapped_page(parent, data, cow) == first && page_ref(first) == 3);
    assert(cow_resolve(parent, data) == 0);
    struct Page *private = mapped_page(parent, data, rw);
    assert(private != first && page_ref(private) == 1 && page_ref(first) == 2);
    assert(memcmp(page2kva(private), page2kva(first), PGSIZE) == 0);

    bool intr_flag;
    local_intr_save(intr_flag);
    uintptr_t saved_satp = read_csr(satp);
    lsatp(PADDR(child->pgdir));
    flush_tlb();
    const char bytes[] = {'a', 'b', 'c', 'd'};
    void *dest = (void *)(data + PGSIZE - 2);
    cow_fail_page_after = 1;
    assert(copy_to_user(child, dest, bytes, sizeof(bytes)) == -E_NO_MEM);
    cow_fail_page_after = -1;
    assert(memcmp(dest, "1122", sizeof(bytes)) == 0);
    assert(mapped_page(child, data + PGSIZE, cow) == second);
    assert(copy_to_user(child, dest, bytes, sizeof(bytes)) == 0);
    assert(memcmp(dest, bytes, sizeof(bytes)) == 0);
    write_csr(satp, saved_satp);
    flush_tlb();
    local_intr_restore(intr_flag);
    assert(((char *)page2kva(first))[PGSIZE - 1] == '1');
    assert(((char *)page2kva(second))[0] == '2');

    free_mm(child);
    free_mm(parent);
    assert(page_ref(first) == 1 && page_ref(second) == 1);
    size_t free_now = nr_free_pages();
    cow_fail_page_after = 0;
    assert(cow_resolve(last, data + 7) == 0);
    assert(cow_fail_page_after == 0);
    cow_fail_page_after = -1;
    assert(mapped_page(last, data, rw) == first && nr_free_pages() == free_now);
    free_mm(last);
    assert(nr_free_pages() == free_before && kallocated() == heap_before);
    cprintf("COW: store OOM, cross-page copyout, singleton promotion and reclaim passed.\n");
}
