#ifndef __KERN_MM_BUDDY_PMM_H__
#define __KERN_MM_BUDDY_PMM_H__

#include <pmm.h>

/* Requests are rounded up to a power of two pages. Free only a whole
 * allocation, using either the original request or the actual block size.
 * No partial frees are supported. */
extern const struct pmm_manager buddy_pmm_manager;

#endif /* !__KERN_MM_BUDDY_PMM_H__ */
