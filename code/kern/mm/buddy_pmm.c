#include <buddy_pmm.h>
#include <list.h>
#include <stdio.h>
//lab2 function2-3 2411292
/* One address-ordered list of free block heads; nr_free counts pages.
 * A free head has PageProperty and property == block size in pages.
 * An allocated head retains its size but has no PageProperty; all interior
 * pages have property == 0. These conventions are private to Buddy.
 * Only free heads are linked. No extra Page fields or dynamic metadata. */
static free_area_t buddy_free_area;

#define free_list (buddy_free_area.free_list)
#define nr_free (buddy_free_area.nr_free)
#define BUDDY_MAX_PAGES ((size_t)((unsigned int)-1))

/* Validate a descriptor address before doing Page pointer arithmetic. */
static size_t
buddy_page_index(struct Page *page) {
    assert(pages != NULL && npage > nbase);
    assert((uintptr_t)page >= (uintptr_t)pages);
    uintptr_t offset = (uintptr_t)page - (uintptr_t)pages;
    assert(offset % sizeof(struct Page) == 0);
    size_t index = offset / sizeof(struct Page);
    assert(index < npage - nbase);
    return index;
}

static void
buddy_assert_block(struct Page *base, size_t size) {
    size_t index = buddy_page_index(base);
    assert(size > 0 && (size & (size - 1)) == 0);
    assert(size <= BUDDY_MAX_PAGES);
    assert(size <= npage - nbase - index);
    assert(page2ppn(base) % size == 0);
}

/* Zero means the rounded size cannot be represented by Page.property.
 * Check before doubling, so even a size_t-sized request cannot overflow. */
static size_t
buddy_round_pages(size_t n) {
    assert(n > 0);
    size_t size = 1;
    while (size < n) {
        if (size > BUDDY_MAX_PAGES / 2) {
            return 0;
        }
        size *= 2;
    }
    return size;
}

static void
buddy_insert(struct Page *base, size_t size) {
    buddy_assert_block(base, size);
    assert(!PageReserved(base) && !PageProperty(base));
    list_entry_t *le = list_next(&free_list);
    while (le != &free_list && le2page(le, page_link) < base) {
        le = list_next(le);
    }
    base->property = size;
    SetPageProperty(base);
    list_add_before(le, &base->page_link);
}

/* Membership, not just a stale property value, establishes that a buddy
 * is free. Check both PFN bounds before constructing its descriptor. */
static struct Page *
buddy_find_free(size_t ppn, size_t size) {
    if (ppn < nbase || ppn >= npage || size > npage - ppn) {
        return NULL;
    }
    struct Page *target = pages + (ppn - nbase);
    list_entry_t *le;
    for (le = list_next(&free_list); le != &free_list; le = list_next(le)) {
        struct Page *p = le2page(le, page_link);
        if (p > target) {
            break;
        }
        if (p == target) {
            assert(PageProperty(p) && !PageReserved(p));
            return p->property == size ? p : NULL;
        }
    }
    return NULL;
}

/* The incoming block is not linked. Merging changes no page counts.
 * Adjacent registered ranges may merge if they form true buddies; holes
 * and reserved ranges cannot merge because they have no free-list entry. */
static void
buddy_release_block(struct Page *base, size_t size) {
    buddy_assert_block(base, size);
    base->property = 0;
    while (size <= BUDDY_MAX_PAGES / 2) {
        size_t ppn = page2ppn(base);
        struct Page *other = buddy_find_free(ppn ^ size, size);
        if (other == NULL) {
            break;
        }
        list_del_init(&other->page_link);
        ClearPageProperty(other);
        other->property = 0;
        if (other < base) {
            base = other;
        }
        size *= 2;
    }
    buddy_insert(base, size);
}

static void
buddy_init(void) {
    list_init(&free_list);
    nr_free = 0;
}

static void
buddy_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);
    size_t index = buddy_page_index(base);
    assert(n <= npage - nbase - index);
    assert(n <= BUDDY_MAX_PAGES - nr_free);

    /* Validate the whole input before changing it. Previously registered
     * pages are no longer Reserved, so overlapping registration is rejected. */
    size_t i;
    for (i = 0; i < n; i++) {
        assert(PageReserved(base + i));
    }
    for (i = 0; i < n; i++) {
        struct Page *p = base + i;
        p->flags = p->property = 0;
        set_page_ref(p, 0);
        list_init(&p->page_link);
    }

    nr_free += n;
    while (n > 0) {
        size_t size = 1, ppn = page2ppn(base);
        while (size <= n / 2 && size <= BUDDY_MAX_PAGES / 2 &&
               ppn % (size * 2) == 0) {
            size *= 2;
        }
        buddy_release_block(base, size);
        base += size;
        n -= size;
    }
}

static struct Page *
buddy_alloc_pages(size_t n) {
    size_t size = buddy_round_pages(n);
    if (size == 0 || size > nr_free) {
        return NULL;
    }

    /* Prefer the smallest fitting block, keeping larger blocks available. */
    struct Page *base = NULL;
    list_entry_t *le;
    for (le = list_next(&free_list); le != &free_list; le = list_next(le)) {
        struct Page *p = le2page(le, page_link);
        if (p->property >= size &&
            (base == NULL || p->property < base->property)) {
            base = p;
            if (p->property == size) {
                break;
            }
        }
    }
    if (base == NULL) {
        return NULL;
    }

    size_t block_size = base->property;
    buddy_assert_block(base, block_size);
    list_del_init(&base->page_link);
    ClearPageProperty(base);
    while (block_size > size) {
        block_size /= 2;
        buddy_insert(base + block_size, block_size);
    }
    base->property = size;
    nr_free -= size;
    return base;
}

static void
buddy_free_pages(struct Page *base, size_t n) {
    size_t size = buddy_round_pages(n);
    assert(size != 0);
    buddy_assert_block(base, size);
    assert(!PageReserved(base) && !PageProperty(base));
    assert(base->property == size);
    assert(list_empty(&base->page_link));
    assert(size <= BUDDY_MAX_PAGES - nr_free);

    /* Reject partial/interior/double frees and reserved pages before any
     * mutation. Allocated interiors have no block-size marker. */
    size_t i;
    for (i = 0; i < size; i++) {
        struct Page *p = base + i;
        assert(!PageReserved(p) && !PageProperty(p));
        assert(i == 0 || p->property == 0);
        assert(page_ref(p) == 0);
    }
    nr_free += size;
    buddy_release_block(base, size);
}

static size_t
buddy_nr_free_pages(void) {
    return nr_free;
}

/* Read-only structural check, also usable when no free pages remain. */
static void
buddy_check_state(void) {
    size_t total = 0, end_index = 0, blocks = 0;
    list_entry_t *le;
    assert(free_list.next->prev == &free_list);
    assert(free_list.prev->next == &free_list);
    for (le = list_next(&free_list); le != &free_list; le = list_next(le)) {
        assert(++blocks <= nr_free);
        assert(le->next->prev == le && le->prev->next == le);
        struct Page *base = le2page(le, page_link);
        size_t index = buddy_page_index(base), size = base->property;
        buddy_assert_block(base, size);
        assert(PageProperty(base));
        assert(index >= end_index);
        end_index = index + size;
        assert(total <= nr_free && size <= nr_free - total);
        total += size;
        size_t i;
        for (i = 0; i < size; i++) {
            struct Page *p = base + i;
            assert(!PageReserved(p) && page_ref(p) == 0);
            assert(i == 0 || (!PageProperty(p) && p->property == 0));
        }
        if (size <= BUDDY_MAX_PAGES / 2) {
            assert(buddy_find_free(page2ppn(base) ^ size, size) == NULL);
        }
    }
    assert(total == nr_free);
}

/* Test helpers below operate through the real public allocator. They never
 * reset its state or register memory. Expected sizes are supplied explicitly,
 * rather than calculated by buddy_round_pages(), which is under test. */
static struct Page *
buddy_test_smallest(void) {
    struct Page *best = NULL;
    list_entry_t *le;
    for (le = list_next(&free_list); le != &free_list; le = list_next(le)) {
        struct Page *p = le2page(le, page_link);
        if (best == NULL || p->property < best->property) {
            best = p;
        }
    }
    return best;
}

/* Independent membership oracle; do not reuse the allocator's buddy lookup. */
static void
buddy_test_free_block(struct Page *base, size_t size) {
    size_t found = 0;
    list_entry_t *le;
    for (le = list_next(&free_list); le != &free_list; le = list_next(le)) {
        if (le2page(le, page_link) == base) {
            assert(PageProperty(base) && base->property == size);
            found++;
        }
    }
    assert(found == 1);
}

static void
buddy_test_allocated(struct Page *base, size_t size) {
    assert(base != NULL);
    buddy_assert_block(base, size);
    assert(base->property == size && list_empty(&base->page_link));
    size_t i;
    for (i = 0; i < size; i++) {
        struct Page *p = base + i;
        assert(!PageReserved(p) && !PageProperty(p) && page_ref(p) == 0);
        assert(i == 0 || p->property == 0);
    }
    list_entry_t *le;
    for (le = list_next(&free_list); le != &free_list; le = list_next(le)) {
        struct Page *p = le2page(le, page_link);
        assert(base + size <= p || p + p->property <= base);
    }
}

/* The digest includes free descriptors and links, supplementing structural
 * checks and the exact page count when checking that a failure made no change. */
static uint64_t
buddy_test_free_digest(void) {
    uint64_t digest = 14695981039346656037ULL;
    list_entry_t *le;
    for (le = list_next(&free_list); le != &free_list; le = list_next(le)) {
        struct Page *base = le2page(le, page_link);
        size_t i;
        for (i = 0; i < base->property; i++) {
            struct Page *p = base + i;
            digest = (digest ^ (uintptr_t)p) * 1099511628211ULL;
            digest = (digest ^ p->flags) * 1099511628211ULL;
            digest = (digest ^ p->property) * 1099511628211ULL;
            digest = (digest ^ (unsigned int)p->ref) * 1099511628211ULL;
            digest = (digest ^ (uintptr_t)p->page_link.next) * 1099511628211ULL;
            digest = (digest ^ (uintptr_t)p->page_link.prev) * 1099511628211ULL;
        }
    }
    digest = (digest ^ (uintptr_t)free_list.next) * 1099511628211ULL;
    return (digest ^ (uintptr_t)free_list.prev) * 1099511628211ULL;
}

static void
buddy_test_failure(size_t request) {
    buddy_check_state();
    size_t before = nr_free_pages();
    uint64_t digest = buddy_test_free_digest();
    assert(alloc_pages(request) == NULL);
    assert(nr_free_pages() == before);
    buddy_check_state();
    assert(buddy_test_free_digest() == digest);
}

/* Pre: at least one fitting block. Post: both legal free-size forms restore
 * the initial count. A fragmented/low-memory state is explicitly skipped. */
static void
buddy_test_basic(void) {
    const size_t requests[] = {1, 2, 3, 4};
    const size_t sizes[] = {1, 2, 4, 4};
    size_t initial = nr_free_pages(), i;
    for (i = 0; i < sizeof(requests) / sizeof(requests[0]); i++) {
        list_entry_t *le;
        bool available = 0;
        for (le = list_next(&free_list); le != &free_list; le = list_next(le)) {
            if (le2page(le, page_link)->property >= sizes[i]) {
                available = 1;
                break;
            }
        }
        if (!available) {
            cprintf("buddy test: request %lu SKIP (no fitting block)\n", requests[i]);
            continue;
        }
        size_t mode;
        for (mode = 0; mode < 2; mode++) {
            struct Page *p = alloc_pages(requests[i]);
            buddy_test_allocated(p, sizes[i]);
            assert(nr_free_pages() == initial - sizes[i]);
            buddy_check_state();
            free_pages(p, mode == 0 ? requests[i] : sizes[i]);
            assert(nr_free_pages() == initial);
            buddy_check_state();
        }
        cprintf("buddy test: request %lu -> %lu pages PASS\n", requests[i], sizes[i]);
    }
    assert(nr_free_pages() == initial);
}

/* Bookkeeping lives in the payload of pages owned by the test, never in
 * allocator metadata or a fixed-size array. The kernel's existing direct
 * mapping provides access. Save next/page/size before returning each block. */
struct buddy_test_hold {
    struct buddy_test_hold *next;
    struct Page *page;
    size_t size;
};

static void
buddy_test_hold_block(struct buddy_test_hold **held, size_t size) {
    size_t before = nr_free_pages();
    struct Page *p = alloc_pages(size);
    buddy_test_allocated(p, size);
    assert(nr_free_pages() == before - size);
    uintptr_t kva = page2pa(p) + va_pa_offset;
    assert(kva >= KERNBASE && PADDR(kva) == page2pa(p));
    struct buddy_test_hold *entry = (struct buddy_test_hold *)kva;
    entry->next = *held;
    entry->page = p;
    entry->size = size;
    *held = entry;
    buddy_check_state();
}

static void
buddy_test_return_held(struct buddy_test_hold *held) {
    while (held != NULL) {
        struct buddy_test_hold *next = held->next;
        struct Page *p = held->page;
        size_t size = held->size, before = nr_free_pages();
        buddy_test_allocated(p, size);
        free_pages(p, size);
        assert(nr_free_pages() == before + size);
        buddy_check_state();
        held = next;
    }
}

/* Pre: a real free block of >=4 pages. Temporarily allocate all smaller
 * blocks so the first one-page request MUST split the chosen source block.
 * No list detachment or synthetic arena is used. Startup is single-threaded. */
static void
buddy_test_split_merge(void) {
    size_t initial = nr_free_pages();
    struct buddy_test_hold *held = NULL;
    struct Page *source;
    while ((source = buddy_test_smallest()) != NULL && source->property < 4) {
        buddy_test_hold_block(&held, source->property);
    }
    if (source == NULL) {
        buddy_test_return_held(held);
        assert(nr_free_pages() == initial);
        buddy_check_state();
        cprintf("buddy test: split/merge SKIP (no block of >=4 pages)\n");
        return;
    }

    size_t source_size = source->property, before = nr_free_pages();
    struct Page *leaf[4];
    leaf[0] = alloc_pages(1);
    assert(leaf[0] == source);
    buddy_test_allocated(leaf[0], 1);
    assert(nr_free_pages() == before - 1);
    size_t size;
    /* Each expected split remainder must be linked exactly once. */
    for (size = 1; size < source_size; size *= 2) {
        buddy_test_free_block(source + size, size);
    }
    buddy_check_state();
    size_t i;
    for (i = 1; i < 4; i++) {
        leaf[i] = alloc_pages(1);
        assert(leaf[i] == source + i);
        buddy_test_allocated(leaf[i], 1);
        assert(nr_free_pages() == before - i - 1);
        buddy_check_state();
    }
    cprintf("buddy test: split %lu-page block into allocated leaves PASS\n", source_size);

    /* Reserve every remaining free block (all >=4 pages). Only the four
     * leaves stay outside the held list. This creates a genuine fragmented
     * failure with two free pages, rather than faking nr_free or list state. */
    while (!list_empty(&free_list)) {
        struct Page *p = le2page(list_next(&free_list), page_link);
        assert(p->property >= 4);
        buddy_test_hold_block(&held, p->property);
    }
    assert(nr_free_pages() == 0);
    buddy_check_state();
    buddy_test_failure(1);

    free_pages(leaf[1], 1);
    assert(nr_free_pages() == 1);
    buddy_test_free_block(leaf[1], 1);
    buddy_test_allocated(leaf[0], 1);
    buddy_check_state();
    free_pages(leaf[2], 1);
    assert(nr_free_pages() == 2);
    assert(page2ppn(leaf[2]) == page2ppn(leaf[1]) + 1);
    assert((page2ppn(leaf[1]) ^ 1) != page2ppn(leaf[2]));
    buddy_test_free_block(leaf[1], 1);
    buddy_test_free_block(leaf[2], 1);
    buddy_test_allocated(leaf[0], 1);
    buddy_test_allocated(leaf[3], 1);
    buddy_check_state();
    buddy_test_failure(2);  /* total==2, but these adjacent pages are not buddies */
    struct Page *retry = alloc_pages(1);
    assert(retry == leaf[1]);
    buddy_test_allocated(retry, 1);
    assert(nr_free_pages() == 1);
    buddy_check_state();
    free_pages(retry, 1);
    assert(nr_free_pages() == 2);
    buddy_check_state();
    cprintf("buddy test: exhaustion, adjacent non-buddies, fragmented failure PASS\n");

    free_pages(leaf[0], 1);
    assert(nr_free_pages() == 3);
    buddy_test_free_block(source, 2);
    buddy_test_free_block(leaf[2], 1);
    buddy_test_allocated(leaf[3], 1);
    buddy_check_state();
    free_pages(leaf[3], 1);
    assert(nr_free_pages() == 4);
    buddy_test_free_block(source, 4);  /* 1+1 -> 2, then 2+2 -> 4 */
    buddy_check_state();
    buddy_test_return_held(held);
    assert(nr_free_pages() == initial);
    buddy_test_free_block(source, source_size);
    buddy_check_state();
    cprintf("buddy test: true buddies and recursive merge PASS\n");
}

static void
buddy_check(void) {
    assert(pmm_manager == &buddy_pmm_manager);
    buddy_check_state();
    size_t initial = nr_free_pages();
    cprintf("buddy test: begin (%lu free pages)\n", initial);
    buddy_test_basic();
    buddy_test_split_merge();
    /* nr_free uses unsigned int, so initial+1 fits in the current size_t. */
    buddy_test_failure(initial + 1);
    buddy_test_failure((size_t)-1);
    buddy_test_basic();  /* legal allocation/free still works after failures */
    assert(nr_free_pages() == initial);
    buddy_check_state();
    cprintf("buddy test: init_memmap input variations SKIP (live memory)\n");
    cprintf("buddy test: invalid free/zero request SKIP (fatal assertions)\n");
    cprintf("buddy test: completed, free pages restored to %lu\n", initial);
}

const struct pmm_manager buddy_pmm_manager = {
    .name = "buddy_pmm_manager",
    .init = buddy_init,
    .init_memmap = buddy_init_memmap,
    .alloc_pages = buddy_alloc_pages,
    .free_pages = buddy_free_pages,
    .nr_free_pages = buddy_nr_free_pages,
    .check = buddy_check,
};
