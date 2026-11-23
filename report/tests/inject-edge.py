from pathlib import Path

def inject(path, anchor, addition):
    p = Path(path)
    data = p.read_text()
    assert data.count(anchor) == 1
    p.write_text(data.replace(anchor, addition))

inject("kern/mm/pmm.c", "struct Page *alloc_pages(size_t n)\n{",
       """int edge_fail_page_after = -1;
struct Page *alloc_pages(size_t n)
{
    if (edge_fail_page_after >= 0 && edge_fail_page_after-- == 0)
        return NULL;""")

inject("kern/mm/kmalloc.c", "kmalloc(size_t size)\n{",
       """kmalloc(size_t size)
{
    extern int edge_fail_heap_after;
    if (edge_fail_heap_after >= 0 && edge_fail_heap_after-- == 0) {
        return NULL;
    }""")

inject("kern/syscall/syscall.c", "static int sys_fork(uint64_t arg[])\n{",
       """int edge_fail_heap_after = -1;
static int sys_fork(uint64_t arg[])
{
    extern int edge_fail_page_after;
    static bool tested;
    if (!tested) {
        tested = 1;
        size_t pages = nr_free_pages(), heap = kallocated();
        struct proc_struct *child = current->cptr;
        for (int i = 0; i < 4; i++) {
            edge_fail_heap_after = i;
            int ret = do_fork(0, current->tf->gpr.sp, current->tf);
            edge_fail_heap_after = -1;
            assert(ret == -E_NO_MEM);
            assert(nr_free_pages() == pages && kallocated() == heap);
            assert(current->cptr == child);
        }
        for (int i = 0; i < 32; i++) {
            edge_fail_page_after = i;
            int ret = do_fork(0, current->tf->gpr.sp, current->tf);
            edge_fail_page_after = -1;
            if (ret > 0) {
                assert(i >= 6);
                cprintf("EDGE: 4 heap and %d page failure points restored resources and process links.\\n", i);
                return ret;
            }
            assert(ret == -E_NO_MEM);
            assert(nr_free_pages() == pages && kallocated() == heap);
            assert(current->cptr == child);
        }
        panic("all injected fork attempts failed");
    }""")

p = Path("kern/syscall/syscall.c")
p.write_text(p.read_text().replace("#include <error.h>", "#include <error.h>\n#include <assert.h>\n#include <kmalloc.h>"))
