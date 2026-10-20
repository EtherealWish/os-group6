#ifndef __KERN_MM_SLUB_H__
#define __KERN_MM_SLUB_H__

#include <defs.h>

/* Simplified Slab/SLUB-style object allocator over the current pmm_manager.
 * Call init exactly once after physical memory initialization and its checks.
 * Calls must be serialized; this lab implementation has no interrupt/SMP locks.
 * The page manager must not be switched while SLUB owns any pages.
 *
 * alloc returns uninitialized storage aligned to 16 bytes, or NULL for a zero
 * size, arithmetic overflow or insufficient contiguous physical memory.
 * free accepts NULL, otherwise it requires the exact live allocation address.
 * Bad pointers/double frees panic when detected. A stale pointer whose address
 * has been reallocated cannot be distinguished from the new live allocation.
 * Empty small-object pages and large allocations are returned immediately to
 * the page manager. Free slots in nonempty pages remain available for reuse. */
void slub_init(void);
void *slub_alloc(size_t size);
void slub_free(void *ptr);

#endif /* !__KERN_MM_SLUB_H__ */
