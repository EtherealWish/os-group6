#include <defs.h>
#include <list.h>
#include <memlayout.h>
#include <assert.h>
#include <kmalloc.h>
#include <sync.h>
#include <pmm.h>
#include <stdio.h>

/*
 * SLOB Allocator: Simple List Of Blocks
 *
 * Matt Mackall <mpm@selenic.com> 12/30/03
 *
 * How SLOB works:
 *
 * The core of SLOB is a traditional K&R style heap allocator, with
 * support for returning aligned objects. The granularity of this
 * allocator is 8 bytes on x86, though it's perhaps possible to reduce
 * this to 4 if it's deemed worth the effort. The slob heap is a
 * singly-linked list of pages from __get_free_page, grown on demand
 * and allocation from the heap is currently first-fit.
 *
 * Above this is an implementation of kmalloc/kfree. Blocks returned
 * from kmalloc are 8-byte aligned and prepended with a 8-byte header.
 * If kmalloc is asked for objects of PAGE_SIZE or larger, it calls
 * __get_free_pages directly so that it can return page-aligned blocks
 * and keeps a linked list of such pages and their orders. These
 * objects are detected in kfree() by their page alignment.
 *
 * SLAB is emulated on top of SLOB by simply calling constructors and
 * destructors for every SLAB allocation. Objects are returned with
 * the 8-byte alignment unless the SLAB_MUST_HWCACHE_ALIGN flag is
 * set, in which case the low-level allocator will fragment blocks to
 * create the proper alignment. Again, objects of page-size or greater
 * are allocated by calling __get_free_pages. As SLAB objects know
 * their size, no separate size bookkeeping is necessary and there is
 * essentially no allocation space overhead.
 */

// some helper
#define spin_lock_irqsave(l, f) local_intr_save(f)
#define spin_unlock_irqrestore(l, f) local_intr_restore(f)
typedef unsigned int gfp_t;
#ifndef PAGE_SIZE
#define PAGE_SIZE PGSIZE
#endif

#ifndef L1_CACHE_BYTES
#define L1_CACHE_BYTES 64
#endif

#ifndef ALIGN
#define ALIGN(addr, size) (((addr) + (size) - 1) & (~((size) - 1)))
#endif

struct slob_block
{
	int units;
	struct slob_block *next;
};
typedef struct slob_block slob_t;

#define SLOB_UNIT sizeof(slob_t)
#define SLOB_UNITS(size) (((size) + SLOB_UNIT - 1) / SLOB_UNIT)
#define SLOB_ALIGN L1_CACHE_BYTES

struct bigblock
{
	int order;
	void *pages;
	struct bigblock *next;
};
typedef struct bigblock bigblock_t;

static slob_t arena = {.next = &arena, .units = 1};
static slob_t *slobfree = &arena;
static bigblock_t *bigblocks;
static size_t slob_live_units;

static void *__slob_get_free_pages(gfp_t gfp, int order)
{
	struct Page *page = alloc_pages(1 << order);
	if (!page)
		return NULL;
	return page2kva(page);
}

#define __slob_get_free_page(gfp) __slob_get_free_pages(gfp, 0)

static inline void __slob_free_pages(unsigned long kva, int order)
{
	free_pages(kva2page((void *)kva), 1 << order);
}

static void slob_insert(slob_t *block, bool release_page);

static void *slob_alloc(size_t size, gfp_t gfp)
{
	assert((size + SLOB_UNIT) < PAGE_SIZE);

	slob_t *prev, *cur;
	int units = SLOB_UNITS(size);
	unsigned long flags;

	spin_lock_irqsave(&slob_lock, flags);
	prev = slobfree;
	for (cur = prev->next;; prev = cur, cur = cur->next)
	{
		if (cur != &arena && cur->units >= units)
		{
			if (cur->units == units)	/* exact fit? */
				prev->next = cur->next; /* unlink */
			else
			{ /* fragment */
				prev->next = cur + units;
				prev->next->units = cur->units - units;
				prev->next->next = cur->next;
				cur->units = units;
			}

			slobfree = prev;
			slob_live_units += units;
			spin_unlock_irqrestore(&slob_lock, flags);
			return cur;
		}
		if (cur == slobfree)
		{
			spin_unlock_irqrestore(&slob_lock, flags);

			cur = (slob_t *)__slob_get_free_page(gfp);
			if (!cur)
				return NULL;

			cur->units = units;
			slob_t *tail = cur + units;
			tail->units = SLOB_UNITS(PAGE_SIZE) - units;
			spin_lock_irqsave(&slob_lock, flags);
			slob_insert(tail, 0);
			slob_live_units += units;
			spin_unlock_irqrestore(&slob_lock, flags);
			return cur;
		}
	}
}

static bool same_slab(const slob_t *left, const slob_t *right)
{
	return ROUNDDOWN((uintptr_t)left, PAGE_SIZE) ==
	       ROUNDDOWN((uintptr_t)right, PAGE_SIZE);
}

// Insert a block with interrupts disabled. Keep extents page-local so a
// completely free slab can return to the physical page allocator.
static void slob_insert(slob_t *block, bool release_page)
{
	slob_t *prev = slobfree;
	for (; !(block > prev && block < prev->next); prev = prev->next)
	{
		if (prev >= prev->next && (block > prev || block < prev->next))
			break;
	}

	if (prev->next != &arena && same_slab(block, prev->next) &&
	    block + block->units == prev->next)
	{
		block->units += prev->next->units;
		block->next = prev->next->next;
	}
	else
	{
		block->next = prev->next;
	}

	if (prev != &arena && same_slab(prev, block) && prev + prev->units == block)
	{
		prev->units += block->units;
		prev->next = block->next;
		block = prev;
	}
	else
	{
		prev->next = block;
	}
	slobfree = prev;

	if (release_page && block->units == SLOB_UNITS(PAGE_SIZE))
	{
		slob_t *link = &arena;
		while (link->next != block)
			link = link->next;
		link->next = block->next;
		if (slobfree == block)
			slobfree = link;
		__slob_free_pages((unsigned long)block, 0);
	}
}

static void slob_free(void *ptr)
{
	slob_t *block = ptr;
	unsigned long flags;
	spin_lock_irqsave(&slob_lock, flags);
	slob_live_units -= block->units;
	slob_insert(block, 1);
	spin_unlock_irqrestore(&slob_lock, flags);
}

void slob_init(void)
{
	cprintf("use SLOB allocator\n");
}

inline void
kmalloc_init(void)
{
	slob_init();
	cprintf("kmalloc_init() succeeded!\n");
}

size_t
slob_allocated(void)
{
	return slob_live_units * SLOB_UNIT;
}

size_t
kallocated(void)
{
	return slob_allocated();
}

static int find_order(size_t size)
{
	int order = 0;
	for (size_t pages = (size - 1) / PAGE_SIZE; pages != 0; pages >>= 1)
		order++;
	return order;
}

static void *__kmalloc(size_t size, gfp_t gfp)
{
	slob_t *m;
	bigblock_t *bb;
	unsigned long flags;

	if (size < PAGE_SIZE - SLOB_UNIT)
	{
		m = slob_alloc(size + SLOB_UNIT, gfp);
		return m ? (void *)(m + 1) : 0;
	}

	bb = slob_alloc(sizeof(bigblock_t), gfp);
	if (!bb)
		return 0;

	bb->order = find_order(size);
	bb->pages = (void *)__slob_get_free_pages(gfp, bb->order);

	if (bb->pages)
	{
		spin_lock_irqsave(&block_lock, flags);
		bb->next = bigblocks;
		bigblocks = bb;
		spin_unlock_irqrestore(&block_lock, flags);
		return bb->pages;
	}

	slob_free(bb);
	return 0;
}

void *
kmalloc(size_t size)
{
	return __kmalloc(size, 0);
}

void kfree(void *block)
{
	bigblock_t *bb, **last = &bigblocks;
	unsigned long flags;

	if (!block)
		return;

	if (!((unsigned long)block & (PAGE_SIZE - 1)))
	{
		/* might be on the big block list */
		spin_lock_irqsave(&block_lock, flags);
		for (bb = bigblocks; bb; last = &bb->next, bb = bb->next)
		{
			if (bb->pages == block)
			{
				*last = bb->next;
				spin_unlock_irqrestore(&block_lock, flags);
				__slob_free_pages((unsigned long)block, bb->order);
				slob_free(bb);
				return;
			}
		}
		spin_unlock_irqrestore(&block_lock, flags);
	}

	slob_free((slob_t *)block - 1);
	return;
}

unsigned int ksize(const void *block)
{
	bigblock_t *bb;
	unsigned long flags;

	if (!block)
		return 0;

	if (!((unsigned long)block & (PAGE_SIZE - 1)))
	{
		spin_lock_irqsave(&block_lock, flags);
		for (bb = bigblocks; bb; bb = bb->next)
			if (bb->pages == block)
			{
				spin_unlock_irqrestore(&slob_lock, flags);
				return PAGE_SIZE << bb->order;
			}
		spin_unlock_irqrestore(&block_lock, flags);
	}

	return ((slob_t *)block - 1)->units * SLOB_UNIT;
}
