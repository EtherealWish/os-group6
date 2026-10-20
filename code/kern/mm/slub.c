#include <slub.h>
#include <pmm.h>
#include <list.h>
#include <string.h>
#include <stdio.h>
#include <buddy_pmm.h>
//lab2 function challenge 2411292
/* Metadata is stored at the beginning of pages owned by SLUB. Page descriptor
 * fields and the page allocator's lists are never used for object bookkeeping.
 * This is a serialized, single-core teaching allocator, not Linux SLUB. */
#define SLUB_ALIGNMENT 16
#define SLUB_MIN_SIZE 16
#define SLUB_CLASS_COUNT 7
#define SLUB_MAX_SMALL 1024
#define SLUB_BITMAP_WORDS ((PGSIZE / SLUB_MIN_SIZE + 63) / 64)

struct slub_region {
    list_entry_t all_link;       // every currently owned region, including full pages
    list_entry_t available_link; // small-object pages with at least one free slot
    struct Page *page;
    size_t npages;               // exact power-of-two request sent to alloc_pages
    size_t class_index;          // SLUB_CLASS_COUNT denotes a large allocation
    size_t slot_size;
    size_t capacity;
    size_t inuse;
    uint64_t used[SLUB_BITMAP_WORDS];
};

#define SLUB_DATA_OFFSET \
    ((sizeof(struct slub_region) + SLUB_ALIGNMENT - 1) & \
     ~((size_t)SLUB_ALIGNMENT - 1))

static const size_t slub_sizes[SLUB_CLASS_COUNT] = {
    16, 32, 64, 128, 256, 512, 1024,
};
static list_entry_t slub_regions;
static list_entry_t slub_available[SLUB_CLASS_COUNT];
static bool slub_ready;

static void slub_check(void);

static struct slub_region *
slub_from_available(list_entry_t *le) {
    return to_struct(le, struct slub_region, available_link);
}

static bool
slub_slot_used(struct slub_region *region, size_t slot) {
    assert(slot < region->capacity);
    return (region->used[slot / 64] >> (slot % 64)) & 1;
}

static void
slub_assert_small(struct slub_region *region) {
    assert(region->class_index < SLUB_CLASS_COUNT);
    assert(region->npages == 1);
    assert(region->slot_size == slub_sizes[region->class_index]);
    assert(region->capacity == (PGSIZE - SLUB_DATA_OFFSET) / region->slot_size);
    assert(region->capacity > 1);
    assert(region->capacity <= SLUB_BITMAP_WORDS * 64);
    assert(region->inuse <= region->capacity);
}

/* Only convert pages returned by the initialized public page allocator.
 * KADDR is commented out in this tree: use the existing direct-map offset,
 * checking both descriptor bounds and the complete mapped byte range. */
static struct slub_region *
slub_new_region(size_t npages) {
    assert(npages > 0 && (npages & (npages - 1)) == 0);
    if (npages > (size_t)-1 / PGSIZE || npages > nr_free_pages()) {
        return NULL;
    }
    struct Page *page = alloc_pages(npages);
    if (page == NULL) {
        return NULL;
    }
    size_t ppn = page2ppn(page);
    assert(ppn >= nbase && ppn < npage && npages <= npage - ppn);
    uintptr_t pa = page2pa(page);
    assert(pa <= (uintptr_t)-1 - va_pa_offset);
    uintptr_t kva = pa + va_pa_offset;
    assert(kva >= KERNBASE && kva < KERNTOP);
    assert(npages * PGSIZE <= KERNTOP - kva);
    assert(kva % PGSIZE == 0);

    struct slub_region *region = (struct slub_region *)kva;
    memset(region, 0, sizeof(*region));
    list_init(&region->all_link);
    list_init(&region->available_link);
    region->page = page;
    region->npages = npages;
    return region;
}

void
slub_init(void) {
    assert(!slub_ready);
    assert(pmm_manager != NULL && pages != NULL && npage > nbase);
    assert(va_pa_offset == PHYSICAL_MEMORY_OFFSET);
    static_assert(SLUB_ALIGNMENT >= sizeof(void *));
    static_assert(PGSIZE % SLUB_ALIGNMENT == 0);
    static_assert(SLUB_DATA_OFFSET + 2 * SLUB_MAX_SMALL <= PGSIZE);
    static_assert(SLUB_BITMAP_WORDS * 64 >= PGSIZE / SLUB_MIN_SIZE);
    list_init(&slub_regions);
    size_t i;
    for (i = 0; i < SLUB_CLASS_COUNT; i++) {
        list_init(&slub_available[i]);
    }
    slub_ready = 1;
    slub_check();
}

static void *
slub_alloc_small(size_t class_index) {
    list_entry_t *available = &slub_available[class_index];
    struct slub_region *region;
    if (list_empty(available)) {
        region = slub_new_region(1);
        if (region == NULL) {
            return NULL;
        }
        region->class_index = class_index;
        region->slot_size = slub_sizes[class_index];
        region->capacity = (PGSIZE - SLUB_DATA_OFFSET) / region->slot_size;
        slub_assert_small(region);
        list_add(&slub_regions, &region->all_link);
        list_add(available, &region->available_link);
    } else {
        region = slub_from_available(list_next(available));
        slub_assert_small(region);
    }

    assert(region->inuse < region->capacity);
    size_t slot;
    for (slot = 0; slot < region->capacity; slot++) {
        if (!slub_slot_used(region, slot)) {
            break;
        }
    }
    assert(slot < region->capacity);
    region->used[slot / 64] |= 1ULL << (slot % 64);
    region->inuse++;
    if (region->inuse == region->capacity) {
        list_del_init(&region->available_link);
    }
    return (char *)region + SLUB_DATA_OFFSET + slot * region->slot_size;
}

static void *
slub_alloc_large(size_t size) {
    if (size > (size_t)-1 - SLUB_DATA_OFFSET) {
        return NULL;
    }
    size_t bytes = size + SLUB_DATA_OFFSET;
    /* Division/remainder avoids overflow from bytes + PGSIZE - 1. */
    size_t needed = bytes / PGSIZE + (bytes % PGSIZE != 0);
    size_t npages = 1;
    while (npages < needed) {
        if (npages > (size_t)-1 / 2) {
            return NULL;
        }
        npages *= 2;
    }
    struct slub_region *region = slub_new_region(npages);
    if (region == NULL) {
        return NULL;
    }
    region->class_index = SLUB_CLASS_COUNT;
    region->slot_size = size;
    region->capacity = region->inuse = 1;
    list_add(&slub_regions, &region->all_link);
    return (char *)region + SLUB_DATA_OFFSET;
}

void *
slub_alloc(size_t size) {
    assert(slub_ready);
    if (size == 0) {
        return NULL;
    }
    if (size > SLUB_MAX_SMALL) {
        return slub_alloc_large(size);
    }
    size_t class_index = 0;
    while (slub_sizes[class_index] < size) {
        class_index++;
    }
    return slub_alloc_small(class_index);
}

/* Unlink metadata BEFORE freeing its backing pages. No access to the header
 * is permitted after free_pages(), which may merge/reuse the whole block. */
static void
slub_return_region(struct slub_region *region) {
    struct Page *page = region->page;
    size_t npages = region->npages;
    assert(region->inuse == 0);
    list_del_init(&region->all_link);
    if (!list_empty(&region->available_link)) {
        list_del_init(&region->available_link);
    }
    free_pages(page, npages);
}

void
slub_free(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    assert(slub_ready);
    uintptr_t addr = (uintptr_t)ptr;
    list_entry_t *le;
    for (le = list_next(&slub_regions); le != &slub_regions; le = list_next(le)) {
        struct slub_region *region = to_struct(le, struct slub_region, all_link);
        uintptr_t start = (uintptr_t)region;
        /* Never dereference ptr or a header obtained by rounding ptr down.
         * Only registered, still-owned regions may supply metadata. */
        if (addr < start || addr - start >= region->npages * PGSIZE) {
            continue;
        }
        size_t offset = addr - start;
        assert(offset >= SLUB_DATA_OFFSET);
        offset -= SLUB_DATA_OFFSET;
        if (region->class_index == SLUB_CLASS_COUNT) {
            assert(offset == 0 && region->inuse == 1);
            region->inuse = 0;
            slub_return_region(region);
            return;
        }

        slub_assert_small(region);
        assert(offset % region->slot_size == 0);
        size_t slot = offset / region->slot_size;
        assert(slot < region->capacity && slub_slot_used(region, slot));
        assert(region->inuse > 0);
        bool was_full = region->inuse == region->capacity;
        region->used[slot / 64] &= ~(1ULL << (slot % 64));
        region->inuse--;
        if (region->inuse == 0) {
            slub_return_region(region);
        } else if (was_full) {
            list_add(&slub_available[region->class_index], &region->available_link);
        }
        return;
    }
    panic("slub_free: address is not a live SLUB allocation");
}

/* SLUB专项测试：all helpers below are read-only except for allocations,
 * frees and writes strictly inside the requested object payloads. */
struct slub_test_stats {
    size_t regions, pages, objects;
};

static bool
slub_test_has_region(uintptr_t address) {
    list_entry_t *le;
    for (le = slub_regions.next; le != &slub_regions; le = le->next) {
        if ((uintptr_t)to_struct(le, struct slub_region, all_link) == address) {
            return 1;
        }
    }
    return 0;
}

/* Independent oracle: count bits directly, not via slub_slot_used(). */
static struct slub_test_stats
slub_test_state(void) {
    struct slub_test_stats stats = {0, 0, 0};
    size_t limit = npage - nbase, c;
    list_entry_t *le, *other;
    assert(slub_regions.next->prev == &slub_regions);
    assert(slub_regions.prev->next == &slub_regions);
    for (le = slub_regions.next; le != &slub_regions; le = le->next) {
        assert(++stats.regions <= limit);
        assert(le->next->prev == le && le->prev->next == le);
        struct slub_region *r = to_struct(le, struct slub_region, all_link);
        uintptr_t start = (uintptr_t)r;
        assert(start >= KERNBASE && start < KERNTOP && start % PGSIZE == 0);
        size_t ppn = PPN(PADDR(start));
        assert(ppn >= nbase && ppn < npage);
        assert(r->page == pages + (ppn - nbase));
        assert(r->npages > 0 && (r->npages & (r->npages - 1)) == 0);
        assert(r->npages <= npage - ppn && ppn % r->npages == 0);
        assert(r->npages * PGSIZE <= KERNTOP - start);
        assert(r->page->property == r->npages); /* Buddy allocated head */
        size_t i, bits = 0;
        for (i = 0; i < r->npages; i++) {
            assert(!PageReserved(r->page + i) && !PageProperty(r->page + i));
            assert(page_ref(r->page + i) == 0);
            assert(i == 0 || r->page[i].property == 0);
        }
        for (i = 0; i < SLUB_BITMAP_WORDS * 64; i++) {
            bool used = (r->used[i / 64] >> (i % 64)) & 1;
            if (r->class_index < SLUB_CLASS_COUNT && i < r->capacity) {
                bits += used;
            } else {
                assert(!used);
            }
        }
        assert(r->inuse > 0 && r->inuse <= r->capacity);
        if (r->class_index < SLUB_CLASS_COUNT) {
            assert(r->npages == 1 && r->slot_size == slub_sizes[r->class_index]);
            assert(r->capacity == (PGSIZE - SLUB_DATA_OFFSET) / r->slot_size);
            assert(r->capacity <= SLUB_BITMAP_WORDS * 64 && bits == r->inuse);
        } else {
            assert(r->class_index == SLUB_CLASS_COUNT);
            assert(r->capacity == 1 && r->inuse == 1 && r->slot_size > 1024);
            assert(r->slot_size <= r->npages * PGSIZE - SLUB_DATA_OFFSET);
        }
        stats.pages += r->npages;
        stats.objects += r->inuse;
        for (other = le->next; other != &slub_regions; other = other->next) {
            struct slub_region *q = to_struct(other, struct slub_region, all_link);
            assert(start + r->npages * PGSIZE <= (uintptr_t)q ||
                   (uintptr_t)q + q->npages * PGSIZE <= start);
        }
    }
    /* Every available entry must be a registered region of the right class.
     * Counts and reciprocal links also detect duplicates/cycles. */
    for (c = 0; c < SLUB_CLASS_COUNT; c++) {
        list_entry_t *head = &slub_available[c];
        size_t count = 0;
        assert(head->next->prev == head && head->prev->next == head);
        for (le = head->next; le != head; le = le->next) {
            assert(++count <= stats.regions);
            assert(le->next->prev == le && le->prev->next == le);
            struct slub_region *r = to_struct(le, struct slub_region, available_link);
            assert(slub_test_has_region((uintptr_t)r));
            assert(r->class_index == c && r->inuse < r->capacity);
        }
    }
    for (le = slub_regions.next; le != &slub_regions; le = le->next) {
        struct slub_region *r = to_struct(le, struct slub_region, all_link);
        size_t found = 0;
        for (c = 0; c < SLUB_CLASS_COUNT; c++) {
            list_entry_t *head = &slub_available[c];
            for (other = head->next; other != head; other = other->next) {
                if (other == &r->available_link) {
                    assert(c == r->class_index);
                    found++;
                }
            }
        }
        bool partial = r->class_index < SLUB_CLASS_COUNT && r->inuse < r->capacity;
        assert(found == (partial ? 1 : 0));
        if (!partial) {
            assert(list_empty(&r->available_link));
        }
    }
    /* Buddy's free heads are publicly marked in Page; inspect them without
     * touching its private list. A free block may not span any owned region. */
    size_t free_total = 0, i, free_end = nbase;
    for (i = 0; i < limit; i++) {
        struct Page *p = pages + i;
        if (!PageProperty(p)) {
            continue;
        }
        size_t ppn = nbase + i, count = p->property;
        assert(!PageReserved(p) && count > 0 && (count & (count - 1)) == 0);
        assert(ppn % count == 0 && count <= npage - ppn && ppn >= free_end);
        free_end = ppn + count;
        free_total += count;
        assert(p->page_link.next->prev == &p->page_link);
        assert(p->page_link.prev->next == &p->page_link);
        for (le = slub_regions.next; le != &slub_regions; le = le->next) {
            struct slub_region *r = to_struct(le, struct slub_region, all_link);
            size_t owned = page2ppn(r->page);
            assert(ppn + count <= owned || owned + r->npages <= ppn);
        }
    }
    assert(free_total == nr_free_pages());
    return stats;
}

static void
slub_test_expect(size_t initial, size_t regions, size_t objects, size_t held_pages) {
    struct slub_test_stats s = slub_test_state();
    assert(s.regions == regions && s.objects == objects && s.pages == held_pages);
    assert(held_pages <= initial && nr_free_pages() == initial - held_pages);
}

static struct slub_region *
slub_test_owner(void *ptr) {
    assert(ptr != NULL && (uintptr_t)ptr % 16 == 0);
    list_entry_t *le;
    for (le = slub_regions.next; le != &slub_regions; le = le->next) {
        struct slub_region *r = to_struct(le, struct slub_region, all_link);
        if ((uintptr_t)ptr >= (uintptr_t)r + SLUB_DATA_OFFSET &&
            (uintptr_t)ptr - (uintptr_t)r < r->npages * PGSIZE) {
            return r;
        }
    }
    panic("slub test: no owner for allocation");
}

/* Uses the actual free block distribution as a precondition, not merely the
 * total page count. Valid for the selected Buddy power-of-two blocks. */
static bool
slub_test_can_allocate(size_t block_pages, size_t blocks) {
    size_t i;
    for (i = 0; i < npage - nbase; i++) {
        if (PageProperty(pages + i) && pages[i].property >= block_pages) {
            size_t available = pages[i].property / block_pages;
            if (available >= blocks) {
                return 1;
            }
            blocks -= available;
        }
    }
    return 0;
}

static uint8_t
slub_test_byte(size_t offset, size_t tag) {
    return (uint8_t)(tag + offset * 37 + (offset >> 8));
}

static void
slub_test_write(void *ptr, size_t size, size_t tag) {
    uint8_t *p = ptr;
    size_t i;
    for (i = 0; i < size; i++) {
        p[i] = slub_test_byte(i, tag);
    }
}

static void
slub_test_read(void *ptr, size_t size, size_t tag) {
    const volatile uint8_t *p = ptr;
    size_t i;
    for (i = 0; i < size; i++) {
        assert(p[i] == slub_test_byte(i, tag));
    }
}

static void
slub_test_classes(void) {
    const size_t requests[7][3] = {
        {1, 15, 16}, {17, 31, 32}, {33, 63, 64}, {65, 127, 128},
        {129, 255, 256}, {257, 511, 512}, {513, 1023, 1024},
    };
    const size_t expected[] = {16, 32, 64, 128, 256, 512, 1024};
    size_t initial = nr_free_pages(), c, j;
    if (!slub_test_can_allocate(1, 1)) {
        cprintf("slub test: 21 class boundaries SKIP (no free page)\n");
        return;
    }
    for (c = 0; c < 7; c++) {
        for (j = 0; j < 3; j++) {
            size_t request = requests[c][j];
            void *p = slub_alloc(request);
            struct slub_region *r = slub_test_owner(p);
            assert(r->class_index == c && r->slot_size == expected[c]);
            assert(request <= r->slot_size && r->inuse == 1 && r->npages == 1);
            assert((uintptr_t)p == (uintptr_t)r + SLUB_DATA_OFFSET);
            assert(r->used[0] == 1);
            slub_test_write(p, request, request);
            slub_test_read(p, request, request);
            slub_test_expect(initial, 1, 1, 1);
            uintptr_t old_region = (uintptr_t)r;
            slub_free(p);
            assert(!slub_test_has_region(old_region));
            slub_test_expect(initial, 0, 0, 0);
        }
        cprintf("slub test: class %lu boundary requests PASS (free %lu -> %lu -> %lu)\n",
                expected[c], initial, initial - 1, nr_free_pages());
    }
}

static void
slub_test_sharing_reuse(void) {
    size_t initial = nr_free_pages(), i, j;
    if (!slub_test_can_allocate(1, 1)) {
        cprintf("slub test: sharing/reuse SKIP (no free page)\n");
        return;
    }
    void *p[4];
    struct slub_region *r = NULL;
    for (i = 0; i < 4; i++) {
        p[i] = slub_alloc(63);
        struct slub_region *owner = slub_test_owner(p[i]);
        if (i == 0) {
            r = owner;
        }
        assert(owner == r && r->slot_size == 64);
        assert((uintptr_t)p[i] == (uintptr_t)p[0] + i * 64);
        for (j = 0; j < i; j++) {
            assert((uintptr_t)p[j] + 63 <= (uintptr_t)p[i]);
        }
        slub_test_write(p[i], 63, 20 + i);
        slub_test_expect(initial, 1, i + 1, 1);
    }
    for (i = 0; i < 4; i++) {
        slub_test_read(p[i], 63, 20 + i);
    }
    uintptr_t old_slot = (uintptr_t)p[1], old_region = (uintptr_t)r;
    slub_free(p[1]);
    assert((r->used[0] & (1ULL << 1)) == 0 && r->inuse == 3);
    slub_test_expect(initial, 1, 3, 1);
    for (i = 0; i < 4; i++) {
        if (i != 1) {
            slub_test_read(p[i], 63, 20 + i);
        }
    }
    p[1] = slub_alloc(63);
    assert((uintptr_t)p[1] == old_slot && slub_test_owner(p[1]) == r);
    assert((r->used[0] & (1ULL << 1)) != 0 && r->inuse == 4);
    slub_test_write(p[1], 63, 21);
    slub_test_expect(initial, 1, 4, 1);
    for (i = 0; i < 4; i++) {
        slub_test_read(p[i], 63, 20 + i);
    }
    for (i = 0; i < 4; i++) {
        slub_free(p[i]);
        slub_test_expect(initial, i == 3 ? 0 : 1, 3 - i, i == 3 ? 0 : 1);
        for (j = i + 1; j < 4; j++) {
            slub_test_read(p[j], 63, 20 + j);
        }
    }
    assert(!slub_test_has_region(old_region)); /* do not dereference r now */
    p[0] = slub_alloc(63); /* class still works after an entire page was returned */
    slub_test_owner(p[0]);
    slub_test_expect(initial, 1, 1, 1);
    slub_free(p[0]);
    slub_test_expect(initial, 0, 0, 0);
    cprintf("slub test: sharing/data independence/bitmap reuse/last free PASS\n");
}

static void
slub_test_full_page(void) {
    /* Bounded BSS storage, never a capacity-sized array on the startup stack. */
    static void *objects[PGSIZE / SLUB_MIN_SIZE + 1];
    size_t initial = nr_free_pages(), capacity = (PGSIZE - SLUB_DATA_OFFSET) / 16;
    if (!slub_test_can_allocate(1, 2)) {
        cprintf("slub test: full page/expansion SKIP (need two pages)\n");
        return;
    }
    assert(capacity + 1 <= sizeof(objects) / sizeof(objects[0]));
    struct slub_region *first = NULL;
    size_t i;
    for (i = 0; i < capacity; i++) {
        objects[i] = slub_alloc(16);
        struct slub_region *r = slub_test_owner(objects[i]);
        if (i == 0) {
            first = r;
        }
        assert(r == first && r->capacity == capacity);
        assert((uintptr_t)objects[i] == (uintptr_t)objects[0] + i * 16);
        slub_test_write(objects[i], 16, i);
        slub_test_expect(initial, 1, i + 1, 1);
    }
    assert(first->inuse == capacity && list_empty(&first->available_link));
    objects[capacity] = slub_alloc(16);
    assert(slub_test_owner(objects[capacity]) != first);
    slub_test_write(objects[capacity], 16, capacity);
    slub_test_expect(initial, 2, capacity + 1, 2);
    for (i = 0; i <= capacity; i++) {
        slub_test_read(objects[i], 16, i);
    }
    size_t hole = capacity / 2;
    uintptr_t freed_slot = (uintptr_t)objects[hole], first_address = (uintptr_t)first;
    slub_free(objects[hole]);
    assert(first->inuse == capacity - 1 && !list_empty(&first->available_link));
    assert(((first->used[hole / 64] >> (hole % 64)) & 1) == 0);
    slub_test_expect(initial, 2, capacity, 2);
    objects[hole] = slub_alloc(16);
    assert((uintptr_t)objects[hole] == freed_slot);
    assert(first->inuse == capacity && list_empty(&first->available_link));
    slub_test_write(objects[hole], 16, hole);
    slub_test_expect(initial, 2, capacity + 1, 2);
    for (i = 0; i <= capacity; i++) {
        slub_test_read(objects[i], 16, i);
    }
    for (i = 0; i < capacity; i++) {
        slub_free(objects[i]);
        objects[i] = NULL;
        slub_test_expect(initial, i + 1 == capacity ? 1 : 2,
                         capacity - i, i + 1 == capacity ? 1 : 2);
        slub_test_read(objects[capacity], 16, capacity);
    }
    assert(!slub_test_has_region(first_address));
    slub_free(objects[capacity]);
    objects[capacity] = NULL;
    slub_test_expect(initial, 0, 0, 0);
    cprintf("slub test: full page %lu slots/bitmap words/second page/reentry PASS\n", capacity);
}

static void
slub_test_large(void) {
    /* These explicit pairs independently check exact-fit and rounded cases. */
    const size_t requests[] = {1025, PGSIZE - SLUB_DATA_OFFSET,
        PGSIZE - SLUB_DATA_OFFSET + 1, PGSIZE, 2 * PGSIZE + 1};
    const size_t expected_pages[] = {1, 1, 2, 2, 4};
    size_t initial = nr_free_pages(), i, j;
    for (i = 0; i < sizeof(requests) / sizeof(requests[0]); i++) {
        size_t request = requests[i], count = expected_pages[i];
        if (!slub_test_can_allocate(count, 2)) {
            cprintf("slub test: large %lu SKIP (need two %lu-page blocks)\n", request, count);
            continue;
        }
        void *p[2];
        uintptr_t region_address[2];
        for (j = 0; j < 2; j++) {
            p[j] = slub_alloc(request);
            struct slub_region *r = slub_test_owner(p[j]);
            region_address[j] = (uintptr_t)r;
            assert(r->class_index == SLUB_CLASS_COUNT && r->npages == count);
            assert(r->slot_size == request && r->capacity == 1 && r->inuse == 1);
            assert(page2ppn(r->page) % count == 0);
            assert(request + SLUB_DATA_OFFSET <= count * PGSIZE);
            assert((uintptr_t)p[j] == (uintptr_t)r + SLUB_DATA_OFFSET);
            slub_test_write(p[j], request, 71 + j);
            slub_test_expect(initial, j + 1, j + 1, (j + 1) * count);
        }
        assert((uintptr_t)p[0] + request <= (uintptr_t)p[1] ||
               (uintptr_t)p[1] + request <= (uintptr_t)p[0]);
        slub_test_read(p[0], request, 71);
        slub_test_read(p[1], request, 72);
        slub_free(p[0]);
        assert(!slub_test_has_region(region_address[0]));
        slub_test_expect(initial, 1, 1, count);
        slub_test_read(p[1], request, 72);
        slub_free(p[1]);
        assert(!slub_test_has_region(region_address[1]));
        slub_test_expect(initial, 0, 0, 0);
        cprintf("slub test: large %lu bytes -> %lu pages each PASS (free %lu -> %lu -> %lu)\n",
                request, count, initial, initial - 2 * count, nr_free_pages());
    }
}

/* Hash registered metadata and list heads, without changing production state. */
static uint64_t
slub_test_digest(void) {
    uint64_t hash = 14695981039346656037ULL;
    list_entry_t *le;
    for (le = slub_regions.next; le != &slub_regions; le = le->next) {
        const uint8_t *p = (const uint8_t *)to_struct(le, struct slub_region, all_link);
        size_t i;
        for (i = 0; i < sizeof(struct slub_region); i++) {
            hash = (hash ^ p[i]) * 1099511628211ULL;
        }
    }
    hash = (hash ^ (uintptr_t)slub_regions.next) * 1099511628211ULL;
    hash = (hash ^ (uintptr_t)slub_regions.prev) * 1099511628211ULL;
    size_t c;
    for (c = 0; c < SLUB_CLASS_COUNT; c++) {
        hash = (hash ^ (uintptr_t)slub_available[c].next) * 1099511628211ULL;
        hash = (hash ^ (uintptr_t)slub_available[c].prev) * 1099511628211ULL;
    }
    return hash;
}

static void
slub_test_failures(void) {
    size_t initial = nr_free_pages();
    void *anchor = NULL;
    if (slub_test_can_allocate(1, 1)) {
        anchor = slub_alloc(33);
        slub_test_owner(anchor);
        slub_test_write(anchor, 33, 99);
    }
    size_t held = anchor != NULL ? 1 : 0;
    slub_test_expect(initial, held, held, held);
    size_t free_before = nr_free_pages();
    assert(free_before < (size_t)-1 / PGSIZE);
    const size_t requests[] = {0, (size_t)-1, (size_t)-1 - SLUB_DATA_OFFSET,
                              (free_before + 1) * PGSIZE};
    const char *names[] = {"zero size", "size overflow", "page rounding overflow",
                           "insufficient pages"};
    size_t i;
    for (i = 0; i < sizeof(requests) / sizeof(requests[0]); i++) {
        uint64_t before = slub_test_digest();
        assert(slub_alloc(requests[i]) == NULL);
        assert(nr_free_pages() == free_before && slub_test_digest() == before);
        slub_test_expect(initial, held, held, held);
        if (anchor != NULL) {
            slub_test_read(anchor, 33, 99);
        }
        cprintf("slub test: %s/state preservation PASS\n", names[i]);
    }
    uint64_t before = slub_test_digest();
    slub_free(NULL);
    assert(slub_test_digest() == before);
    slub_test_expect(initial, held, held, held);
    slub_free(anchor);
    slub_test_expect(initial, 0, 0, 0);
    if (held != 0) {
        void *retry = slub_alloc(33);
        slub_test_owner(retry);
        slub_test_write(retry, 33, 100);
        slub_test_read(retry, 33, 100);
        slub_test_expect(initial, 1, 1, 1);
        slub_free(retry);
        slub_test_expect(initial, 0, 0, 0);
        cprintf("slub test: allocation after failure PASS\n");
    } else {
        cprintf("slub test: allocation after failure SKIP (no free page)\n");
    }
    cprintf("slub test: free NULL PASS\n");
}

static void
slub_check(void) {
    assert(slub_ready);

if (pmm_manager != &buddy_pmm_manager) {
    cprintf("slub test: SKIP (Buddy-specific tests)\n");
    return;
}
    size_t initial = nr_free_pages();
    slub_test_expect(initial, 0, 0, 0);
    cprintf("slub test: begin (%lu Buddy free pages)\n", initial);
    slub_test_classes();
    slub_test_sharing_reuse();
    slub_test_full_page();
    slub_test_large();
    slub_test_failures();
    slub_test_expect(initial, 0, 0, 0);
    cprintf("slub test: invalid/interior/double free SKIP (fatal assertions)\n");
    cprintf("slub test: completed, 0 regions/0 objects, Buddy free pages restored to %lu\n", initial);
}
