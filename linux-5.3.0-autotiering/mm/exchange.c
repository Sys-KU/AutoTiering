/*
 * Exchange two in-use pages. Page flags and page->mapping are exchanged
 * as well. Only anonymous pages are supported.
 *
 * Copyright (C) 2016 NVIDIA, Zi Yan <ziy@nvidia.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
 */

#include <linux/syscalls.h>
#include <linux/migrate.h>
#include <linux/exchange.h>
#include <linux/security.h>
#include <linux/cpuset.h>
#include <linux/mm_inline.h>
#include <linux/page_idle.h>
#include <linux/page-flags.h>
#include <linux/ksm.h>
#include <linux/memcontrol.h>
#include <linux/balloon_compaction.h>
#include <linux/buffer_head.h>
#include <linux/fs.h> /* buffer_migrate_page  */
#include <linux/backing-dev.h>
#include <linux/sched/mm.h>
#include <linux/sched/sysctl.h>
#include <linux/page_balancing.h>


#include "internal.h"

// #define DEBUG

#ifdef DEBUG
#define START_TIME(ts) ts = rdtsc()
#define END_TIME(msg, ts) trace_printk("%s: %llu\n", msg, rdtsc() - ts)
#else
#define START_TIME(ts) {}
#define END_TIME(msg, ts) {}
#endif

unsigned int exchange_concur = 1;
unsigned int exchange_mt = 1;


/*
 * Move a list of individual pages
 */
struct pages_to_node {
	unsigned long from_addr;
	int from_status;

	unsigned long to_addr;
	int to_status;
};

struct page_flags {
	unsigned int page_error :1;
	unsigned int page_referenced:1;
	unsigned int page_uptodate:1;
	unsigned int page_active:1;
	unsigned int page_unevictable:1;
	unsigned int page_checked:1;
	unsigned int page_mappedtodisk:1;
	unsigned int page_dirty:1;
	unsigned int page_is_young:1;
	unsigned int page_is_idle:1;
	unsigned int page_swapcache:1;
	unsigned int page_writeback:1;
	unsigned int page_private:1;
	unsigned int page_doublemap:1;
	unsigned int _pad:2;
};

static void pr_dump_page(struct page *page, const char *msg)
{
	pr_debug("dump:%s page(%p):0x%lx,"
		"count:%d,mapcount:%d,mapping:%p,index:%#lx,flags:%#lx(%pGp),%s,order:%d"
		",%s"
		",%s,page_nid:%d"
		"\n",
		msg,
		page,
		page_to_pfn(page),
		page_ref_count(page),
		PageSlab(page)?0:page_mapcount(page),
		page->mapping, page_to_pgoff(page),
		page->flags, &page->flags,
		PageCompound(page)?"compound_page":"single_page",
		compound_order(page),
		PageDirty(page)?"dirty":"clean",
		PageDeferred(page)?"deferred":"nondeferred",
		page_to_nid(page)
		);
}

#define UNROLL2(x)   x x
#define UNROLL4(x)   UNROLL2(x)   UNROLL2(x)
#define UNROLL8(x)   UNROLL4(x)   UNROLL4(x)
#define UNROLL16(x)  UNROLL8(x)   UNROLL8(x)
#define UNROLL32(x)  UNROLL16(x)  UNROLL16(x)
#define UNROLL64(x)  UNROLL32(x)  UNROLL32(x)
#define UNROLL128(x) UNROLL64(x)  UNROLL64(x)
#define UNROLL256(x) UNROLL128(x) UNROLL128(x)
#define UNROLL512(x) UNROLL256(x) UNROLL256(x)

#define SWAP_PAGE(from, to, tmp, index, chunk) \
		tmp = *((u64*)(from + index)); \
		*((u64*)(from + index)) = *((u64*)(to + index)); \
		*((u64*)(to + index)) = tmp; \
		index = index + chunk;

static void exchange_page(char *to, char *from)
{
	u64 tmp;
	int chunk_size = sizeof(tmp);
	int i = 0;

	// for (i = 0; i < PAGE_SIZE; i += chunck_size * 4) {
	// 	tmp = *((u64*)(from + i));
	// 	*((u64*)(from + i)) = *((u64*)(to + i));
	// 	*((u64*)(to + i)) = tmp;
	// }
	while (i < PAGE_SIZE) {
		UNROLL512(SWAP_PAGE(from, to, tmp, i, chunk_size))
	}
}

static inline void exchange_highpage(struct page *to, struct page *from)
{
	char *vfrom, *vto;

	vfrom = kmap_atomic(from);
	vto = kmap_atomic(to);
	exchange_page(vto, vfrom);
	kunmap_atomic(vto);
	kunmap_atomic(vfrom);
}

static void exchange_huge_page(struct page *dst, struct page *src)
{
	int i;
	int nr_pages;

	if (PageHuge(src)) {
		/* hugetlbfs page */
		struct hstate *h = page_hstate(src);
		nr_pages = pages_per_huge_page(h);

		if (unlikely(nr_pages > MAX_ORDER_NR_PAGES)) {
			//__exchange_gigantic_page(dst, src, nr_pages);
			return;
		}
	} else {
		/* thp page */
		BUG_ON(!PageTransHuge(src));
		nr_pages = hpage_nr_pages(src);
	}

	for (i = 0; i < nr_pages; i++) {
		exchange_highpage(dst + i, src + i);
	}
}

/*
 * Copy the page to its new location without polluting cache
 */
static void exchange_page_flags(struct page *to_page, struct page *from_page)
{
	int from_cpupid, to_cpupid;
	struct page_flags from_page_flags = {0}, to_page_flags = {0};
	struct mem_cgroup *to_memcg = page_memcg(to_page),
					  *from_memcg = page_memcg(from_page);

	from_cpupid = page_cpupid_xchg_last(from_page, -1);

	from_page_flags.page_error = PageError(from_page);
	if (from_page_flags.page_error)
		ClearPageError(from_page);
	from_page_flags.page_referenced = TestClearPageReferenced(from_page);
	from_page_flags.page_uptodate = PageUptodate(from_page);
	ClearPageUptodate(from_page);
	from_page_flags.page_active = TestClearPageActive(from_page);
	from_page_flags.page_unevictable = TestClearPageUnevictable(from_page);
	from_page_flags.page_checked = PageChecked(from_page);
	if (from_page_flags.page_checked)
		ClearPageChecked(from_page);
	from_page_flags.page_mappedtodisk = PageMappedToDisk(from_page);
	ClearPageMappedToDisk(from_page);
	from_page_flags.page_dirty = PageDirty(from_page);
	ClearPageDirty(from_page);
	from_page_flags.page_is_young = test_and_clear_page_young(from_page);
	from_page_flags.page_is_idle = page_is_idle(from_page);
	clear_page_idle(from_page);
	from_page_flags.page_swapcache = PageSwapCache(from_page);
	/*from_page_flags.page_private = PagePrivate(from_page);*/
	/*ClearPagePrivate(from_page);*/
	from_page_flags.page_writeback = test_clear_page_writeback(from_page);
	from_page_flags.page_doublemap = PageDoubleMap(from_page);


	to_cpupid = page_cpupid_xchg_last(to_page, -1);

	to_page_flags.page_error = PageError(to_page);
	if (to_page_flags.page_error)
		ClearPageError(to_page);
	to_page_flags.page_referenced = TestClearPageReferenced(to_page);
	to_page_flags.page_uptodate = PageUptodate(to_page);
	ClearPageUptodate(to_page);
	to_page_flags.page_active = TestClearPageActive(to_page);
	to_page_flags.page_unevictable = TestClearPageUnevictable(to_page);
	to_page_flags.page_checked = PageChecked(to_page);
	if (to_page_flags.page_checked)
		ClearPageChecked(to_page);
	to_page_flags.page_mappedtodisk = PageMappedToDisk(to_page);
	ClearPageMappedToDisk(to_page);
	to_page_flags.page_dirty = PageDirty(to_page);
	ClearPageDirty(to_page);
	to_page_flags.page_is_young = test_and_clear_page_young(to_page);
	to_page_flags.page_is_idle = page_is_idle(to_page);
	clear_page_idle(to_page);
	to_page_flags.page_swapcache = PageSwapCache(to_page);
	/*to_page_flags.page_private = PagePrivate(to_page);*/
	/*ClearPagePrivate(to_page);*/
	to_page_flags.page_writeback = test_clear_page_writeback(to_page);
	to_page_flags.page_doublemap = PageDoubleMap(to_page);

	/* set to_page */
	if (from_page_flags.page_error)
		SetPageError(to_page);
	if (from_page_flags.page_referenced)
		SetPageReferenced(to_page);
	if (from_page_flags.page_uptodate)
		SetPageUptodate(to_page);
	if (from_page_flags.page_active) {
		VM_BUG_ON_PAGE(from_page_flags.page_unevictable, from_page);
		SetPageActive(to_page);
	} else if (from_page_flags.page_unevictable)
		SetPageUnevictable(to_page);
	if (from_page_flags.page_checked)
		SetPageChecked(to_page);
	if (from_page_flags.page_mappedtodisk)
		SetPageMappedToDisk(to_page);

	/* Move dirty on pages not done by migrate_page_move_mapping() */
	if (from_page_flags.page_dirty)
		SetPageDirty(to_page);

	if (from_page_flags.page_is_young)
		set_page_young(to_page);
	if (from_page_flags.page_is_idle)
		set_page_idle(to_page);
	if (from_page_flags.page_doublemap)
		SetPageDoubleMap(to_page);

	/* set from_page */
	if (to_page_flags.page_error)
		SetPageError(from_page);
	if (to_page_flags.page_referenced)
		SetPageReferenced(from_page);
	if (to_page_flags.page_uptodate)
		SetPageUptodate(from_page);
	if (to_page_flags.page_active) {
		VM_BUG_ON_PAGE(to_page_flags.page_unevictable, from_page);
		SetPageActive(from_page);
	} else if (to_page_flags.page_unevictable)
		SetPageUnevictable(from_page);
	if (to_page_flags.page_checked)
		SetPageChecked(from_page);
	if (to_page_flags.page_mappedtodisk)
		SetPageMappedToDisk(from_page);

	/* Move dirty on pages not done by migrate_page_move_mapping() */
	if (to_page_flags.page_dirty)
		SetPageDirty(from_page);

	if (to_page_flags.page_is_young)
		set_page_young(from_page);
	if (to_page_flags.page_is_idle)
		set_page_idle(from_page);
	if (to_page_flags.page_doublemap)
		SetPageDoubleMap(from_page);

	/*
	 * Copy NUMA information to the new page, to prevent over-eager
	 * future migrations of this same page.
	 */
	page_cpupid_xchg_last(to_page, from_cpupid);
	page_cpupid_xchg_last(from_page, to_cpupid);

	ksm_exchange_page(to_page, from_page);
	/*
	 * Please do not reorder this without considering how mm/ksm.c's
	 * get_ksm_page() depends upon ksm_migrate_page() and PageSwapCache().
	 */
	ClearPageSwapCache(to_page);
	ClearPageSwapCache(from_page);
	if (from_page_flags.page_swapcache)
		SetPageSwapCache(to_page);
	if (to_page_flags.page_swapcache)
		SetPageSwapCache(from_page);


#ifdef CONFIG_PAGE_OWNER
	/* exchange page owner  */
	BUG();
#endif
	/* exchange mem cgroup  */
	to_page->mem_cgroup = from_memcg;
	from_page->mem_cgroup = to_memcg;

	/* exchange page info */
	exchange_page_info(from_page, to_page);

}

/*
 * Replace the page in the mapping.
 *
 * The number of remaining references must be:
 * 1 for anonymous pages without a mapping
 * 2 for pages with a mapping
 * 3 for pages with a mapping and PagePrivate/PagePrivate2 set.
 */

static int exchange_page_move_mapping(struct address_space *to_mapping,
			struct address_space *from_mapping,
			struct page *to_page, struct page *from_page,
			struct buffer_head *to_head, struct buffer_head *from_head,
			enum migrate_mode mode,
			int to_extra_count, int from_extra_count)
{
	int to_expected_count = 1 + to_extra_count,
		from_expected_count = 1 + from_extra_count;
	unsigned long from_page_index = from_page->index;
	unsigned long to_page_index = to_page->index;
	int to_swapbacked = PageSwapBacked(to_page),
		from_swapbacked = PageSwapBacked(from_page);
	struct address_space *to_mapping_value = to_page->mapping,
						 *from_mapping_value = from_page->mapping;

	VM_BUG_ON_PAGE(to_mapping != page_mapping(to_page), to_page);
	VM_BUG_ON_PAGE(from_mapping != page_mapping(from_page), from_page);
	VM_BUG_ON(PageCompound(from_page) != PageCompound(to_page));

	if (!to_mapping) {
		/* Anonymous page without mapping */
		if (page_count(to_page) != to_expected_count)
			return -EAGAIN;
	}

	if (!from_mapping) {
		/* Anonymous page without mapping */
		if (page_count(from_page) != from_expected_count)
			return -EAGAIN;
	}

	/* both are anonymous pages  */
	if (!from_mapping && !to_mapping) {
		/* from_page  */
		from_page->index = to_page_index;
		from_page->mapping = to_mapping_value;

		ClearPageSwapBacked(from_page);
		if (to_swapbacked)
			SetPageSwapBacked(from_page);


		/* to_page  */
		to_page->index = from_page_index;
		to_page->mapping = from_mapping_value;

		ClearPageSwapBacked(to_page);
		if (from_swapbacked)
			SetPageSwapBacked(to_page);
	} else if (!from_mapping && to_mapping) { /* from is anonymous, to is file-backed  */
		struct zone *from_zone, *to_zone;
		void **to_pslot;
		int dirty;

		from_zone = page_zone(from_page);
		to_zone = page_zone(to_page);

		spin_lock_irq(&to_mapping->i_pages.xa_lock);

		to_pslot = radix_tree_lookup_slot(&to_mapping->i_pages, page_index(to_page));

		to_expected_count += 1 + page_has_private(to_page);
		if (page_count(to_page) != to_expected_count ||
			radix_tree_deref_slot_protected(to_pslot, &to_mapping->i_pages.xa_lock)
			!= to_page) {
			spin_unlock_irq(&to_mapping->i_pages.xa_lock);
			return -EAGAIN;
		}

		if (!page_ref_freeze(to_page, to_expected_count)) {
			spin_unlock_irq(&to_mapping->i_pages.xa_lock);
			pr_debug("cannot freeze page count\n");
			return -EAGAIN;
		}

		if (((mode & MIGRATETYPE_MASK) == MIGRATE_ASYNC) && to_head &&
				!buffer_migrate_lock_buffers(to_head, mode)) {
			page_ref_unfreeze(to_page, to_expected_count);
			spin_unlock_irq(&to_mapping->i_pages.xa_lock);

			pr_debug("cannot lock buffer head\n");
			return -EAGAIN;
		}

		/*
		 * Now we know that no one else is looking at the page:
		 * no turning back from here.
		 */
		ClearPageSwapBacked(from_page);
		ClearPageSwapBacked(to_page);

		/* from_page  */
		from_page->index = to_page_index;
		from_page->mapping = to_mapping_value;
		/* to_page  */
		to_page->index = from_page_index;
		to_page->mapping = from_mapping_value;

		get_page(from_page); /* add cache reference  */
		if (to_swapbacked)
			__SetPageSwapBacked(from_page);
		else
			VM_BUG_ON_PAGE(PageSwapCache(to_page), to_page);

		if (from_swapbacked)
			__SetPageSwapBacked(to_page);
		else
			VM_BUG_ON_PAGE(PageSwapCache(from_page), from_page);

		dirty = PageDirty(to_page);

		radix_tree_replace_slot(&to_mapping->i_pages, to_pslot, from_page);

		/* drop cache reference */
		page_ref_unfreeze(to_page, to_expected_count - 1);

		spin_unlock(&to_mapping->i_pages.xa_lock);

		/*
		 * If moved to a different zone then also account
		 * the page for that zone. Other VM counters will be
		 * taken care of when we establish references to the
		 * new page and drop references to the old page.
		 *
		 * Note that anonymous pages are accounted for
		 * via NR_FILE_PAGES and NR_ANON_MAPPED if they
		 * are mapped to swap space.
		 */
		if (to_zone != from_zone) {
			__dec_node_state(to_zone->zone_pgdat, NR_FILE_PAGES);
			__inc_node_state(from_zone->zone_pgdat, NR_FILE_PAGES);
			if (PageSwapBacked(to_page) && !PageSwapCache(to_page)) {
				__dec_node_state(to_zone->zone_pgdat, NR_SHMEM);
				__inc_node_state(from_zone->zone_pgdat, NR_SHMEM);
			}
			if (dirty && mapping_cap_account_dirty(to_mapping)) {
				__dec_node_state(to_zone->zone_pgdat, NR_FILE_DIRTY);
				__dec_zone_state(to_zone, NR_ZONE_WRITE_PENDING);
				__inc_node_state(from_zone->zone_pgdat, NR_FILE_DIRTY);
				__inc_zone_state(from_zone, NR_ZONE_WRITE_PENDING);
			}
		}
		local_irq_enable();

	} else {
		/* from is file-backed to is anonymous: fold this to the case above */
		/* both are file-backed  */
		BUG();
	}
	return MIGRATEPAGE_SUCCESS;
}

static int exchange_from_to_pages(struct page *to_page, struct page *from_page,
				enum migrate_mode mode)
{
	int rc = -EBUSY;
	struct address_space *to_page_mapping, *from_page_mapping;
	struct buffer_head *to_head = NULL, *to_bh = NULL;
#ifdef DEBUG
	u64 timestamp;
#endif

	VM_BUG_ON_PAGE(!PageLocked(from_page), from_page);
	VM_BUG_ON_PAGE(!PageLocked(to_page), to_page);

	/* copy page->mapping not use page_mapping()  */
	to_page_mapping = page_mapping(to_page);
	from_page_mapping = page_mapping(from_page);

	/* from_page has to be anonymous page  */
	BUG_ON(from_page_mapping);
	BUG_ON(PageWriteback(from_page));
	/* writeback has to finish */
	BUG_ON(PageWriteback(to_page));

	pr_dump_page(from_page, "exchange anonymous page: from ");

	/* to_page is anonymous  */
	if (!to_page_mapping) {
		pr_dump_page(to_page, "exchange anonymous page: to ");
exchange_mappings:
		START_TIME(timestamp);
		/* actual page mapping exchange */
		rc = exchange_page_move_mapping(to_page_mapping, from_page_mapping,
							to_page, from_page, NULL, NULL, mode, 0, 0);
		END_TIME("exchange_page_move_mapping", timestamp);
	} else {
		if (to_page_mapping->a_ops->migratepage == buffer_migrate_page) {

			pr_dump_page(to_page, "exchange has migratepage: to ");

			if (!page_has_buffers(to_page))
				goto exchange_mappings;

			to_head = page_buffers(to_page);

			rc = exchange_page_move_mapping(to_page_mapping,
					from_page_mapping, to_page, from_page,
					to_head, NULL, mode, 0, 0);

			if (rc != MIGRATEPAGE_SUCCESS)
				return rc;

			/*
			 * In the async case, migrate_page_move_mapping locked the buffers
			 * with an IRQ-safe spinlock held. In the sync case, the buffers
			 * need to be locked now
			 */
			if ((mode & MIGRATETYPE_MASK) != MIGRATE_ASYNC)
				BUG_ON(!buffer_migrate_lock_buffers(to_head, mode));

			ClearPagePrivate(to_page);
			set_page_private(from_page, page_private(to_page));
			set_page_private(to_page, 0);
			/* transfer private page count  */
			put_page(to_page);
			get_page(from_page);

			to_bh = to_head;
			do {
				set_bh_page(to_bh, from_page, bh_offset(to_bh));
				to_bh = to_bh->b_this_page;

			} while (to_bh != to_head);

			SetPagePrivate(from_page);

			to_bh = to_head;
		} else if (!to_page_mapping->a_ops->migratepage) {
			/* fallback_migrate_page  */
			pr_dump_page(to_page, "exchange no migratepage: to ");

			if (PageDirty(to_page)) {
				if ((mode & MIGRATETYPE_MASK) != MIGRATE_SYNC)
					return -EBUSY;
				return writeout(to_page_mapping, to_page);
			}
			if (page_has_private(to_page) &&
				!try_to_release_page(to_page, GFP_KERNEL))
				return -EAGAIN;

			goto exchange_mappings;
		}
	}


	/* actual page data exchange  */
	if (rc != MIGRATEPAGE_SUCCESS)
		return rc;

	rc = -EFAULT;

	START_TIME(timestamp);
	if (mode & MIGRATE_MT)
		rc = exchange_page_mthread(to_page, from_page,
				hpage_nr_pages(from_page));
	if (rc) {
		if (PageHuge(from_page) || PageTransHuge(from_page))
			exchange_huge_page(to_page, from_page);
		else
			exchange_highpage(to_page, from_page);
		rc = 0;
	}
	END_TIME("exchange_highpage", timestamp);

	/*
	 * 1. buffer_migrate_page:
	 *   private flag should be transferred from to_page to from_page
	 *
	 * 2. anon<->anon, fallback_migrate_page:
	 *   both have none private flags or to_page's is cleared.
	 * */
	VM_BUG_ON(!((page_has_private(from_page) && !page_has_private(to_page)) ||
				(!page_has_private(from_page) && !page_has_private(to_page))));

	// START_TIME(timestamp);
	exchange_page_flags(to_page, from_page);
	// END_TIME("exchange_page_flags", timestamp);


	pr_dump_page(from_page, "after exchange: from ");
	pr_dump_page(to_page, "after exchange: to ");

	if (to_bh) {
		VM_BUG_ON(to_bh != to_head);
		do {
			unlock_buffer(to_bh);
			put_bh(to_bh);
			to_bh = to_bh->b_this_page;

		} while (to_bh != to_head);
	}

	return rc;
}

static int unmap_and_exchange(struct page *from_page,
		struct page *to_page, enum migrate_mode mode)
{
	int rc = -EAGAIN;
	struct anon_vma *from_anon_vma = NULL;
	struct anon_vma *to_anon_vma = NULL;
	/*bool is_from_lru = !__PageMovable(from_page);*/
	/*bool is_to_lru = !__PageMovable(to_page);*/
	int from_page_was_mapped = 0;
	int to_page_was_mapped = 0;
	int from_page_count = 0, to_page_count = 0;
	int from_map_count = 0, to_map_count = 0;
	unsigned long from_flags, to_flags;
	pgoff_t from_index, to_index;
	struct address_space *from_mapping, *to_mapping;
#ifdef DEBUG
	u64 timestamp;
#endif

	//START_TIME(timestamp);
	if (!trylock_page(from_page)) {
		if ((mode & MIGRATETYPE_MASK) == MIGRATE_ASYNC)
			goto out;
		lock_page(from_page);
	}

	if (!trylock_page(to_page)) {
		if ((mode & MIGRATETYPE_MASK) == MIGRATE_ASYNC)
			goto out;
		lock_page(to_page);
	}
	//END_TIME("trylock_page*2", timestamp);


	/* from_page is supposed to be an anonymous page */
	VM_BUG_ON_PAGE(PageWriteback(from_page), from_page);

	if (PageWriteback(to_page)) {
		/*
		 * Only in the case of a full synchronous migration is it
		 * necessary to wait for PageWriteback. In the async case,
		 * the retry loop is too short and in the sync-light case,
		 * the overhead of stalling is too much
		 */
		if ((mode & MIGRATETYPE_MASK) != MIGRATE_SYNC) {
			rc = -EBUSY;
			goto out_unlock;
		}
		wait_on_page_writeback(to_page);
	}

	/*
	 * By try_to_unmap(), page->mapcount goes down to 0 here. In this case,
	 * we cannot notice that anon_vma is freed while we migrates a page.
	 * This get_anon_vma() delays freeing anon_vma pointer until the end
	 * of migration. File cache pages are no problem because of page_lock()
	 * File Caches may use write_page() or lock_page() in migration, then,
	 * just care Anon page here.
	 *
	 * Only page_get_anon_vma() understands the subtleties of
	 * getting a hold on an anon_vma from outside one of its mms.
	 * But if we cannot get anon_vma, then we won't need it anyway,
	 * because that implies that the anon page is no longer mapped
	 * (and cannot be remapped so long as we hold the page lock).
	 */
	//START_TIME(timestamp);
	if (PageAnon(from_page) && !PageKsm(from_page))
		from_anon_vma = page_get_anon_vma(from_page);

	if (PageAnon(to_page) && !PageKsm(to_page))
		to_anon_vma = page_get_anon_vma(to_page);
	//END_TIME("page_get_anon_vma*2", timestamp);

	/*if (unlikely(!is_from_lru)) {*/
		/*VM_BUG_ON_PAGE(1, from_page);*/
		/*goto out_unlock_both;*/
	/*}*/

	/*if (unlikely(!is_to_lru)) {*/
		/*pr_debug("exchange non-lru to_page\n");*/
		/*goto out_unlock_both;*/
	/*}*/

	from_page_count = page_count(from_page);
	from_map_count = page_mapcount(from_page);
	to_page_count = page_count(to_page);
	to_map_count = page_mapcount(to_page);
	from_flags = from_page->flags;
	to_flags = to_page->flags;
	from_mapping = from_page->mapping;
	to_mapping = to_page->mapping;
	from_index = from_page->index;
	to_index = to_page->index;

	START_TIME(timestamp);
	/*
	 * Corner case handling:
	 * 1. When a new swap-cache page is read into, it is added to the LRU
	 * and treated as swapcache but it has no rmap yet.
	 * Calling try_to_unmap() against a page->mapping==NULL page will
	 * trigger a BUG.  So handle it here.
	 * 2. An orphaned page (see truncate_complete_page) might have
	 * fs-private metadata. The page can be picked up due to memory
	 * offlining.  Everywhere else except page reclaim, the page is
	 * invisible to the vm, so the page can not be migrated.  So try to
	 * free the metadata, so the page can be freed.
	 */
	if (!from_page->mapping) {
		VM_BUG_ON_PAGE(PageAnon(from_page), from_page);
		if (page_has_private(from_page)) {
			try_to_free_buffers(from_page);
			goto out_unlock_both;
		}
	} else if (page_mapped(from_page)) {
		/* Establish migration ptes */
		VM_BUG_ON_PAGE(PageAnon(from_page) && !PageKsm(from_page) &&
					   !from_anon_vma, from_page);
		try_to_unmap(from_page,
			TTU_MIGRATION|TTU_IGNORE_MLOCK|TTU_IGNORE_ACCESS);
		from_page_was_mapped = 1;
	}
	END_TIME("try_to_unmap from_page", timestamp);

	START_TIME(timestamp);
	if (!to_page->mapping) {
		VM_BUG_ON_PAGE(PageAnon(to_page), to_page);
		if (page_has_private(to_page)) {
			try_to_free_buffers(to_page);
			goto out_unlock_both_remove_from_migration_pte;
		}
	} else if (page_mapped(to_page)) {
		/* Establish migration ptes */
		VM_BUG_ON_PAGE(PageAnon(to_page) && !PageKsm(to_page) &&
						!to_anon_vma, to_page);
		try_to_unmap(to_page,
			TTU_MIGRATION|TTU_IGNORE_MLOCK|TTU_IGNORE_ACCESS);
		to_page_was_mapped = 1;
	}
	END_TIME("try_to_unmap to_page", timestamp);



	//START_TIME(timestamp);
	if (!page_mapped(from_page) && !page_mapped(to_page)) {
		rc = exchange_from_to_pages(to_page, from_page, mode);
		pr_debug("exchange_from_to_pages from: %lx, to %lx: %d\n", page_to_pfn(from_page), page_to_pfn(to_page), rc);
	}
	//END_TIME("exchange_from_to_pages",timestamp);


	START_TIME(timestamp);
	/* In remove_migration_ptes(), page_walk_vma() assumes
	 * from_page and to_page have the same index.
	 * Thus, we restore old_page->index here.
	 * Here to_page is the old_page.
	 */
	if (to_page_was_mapped) {
		if (rc == MIGRATEPAGE_SUCCESS)
			swap(to_page->index, to_index);

		remove_migration_ptes(to_page,
			rc == MIGRATEPAGE_SUCCESS ? from_page : to_page, false);

		if (rc == MIGRATEPAGE_SUCCESS)
			swap(to_page->index, to_index);
	}

out_unlock_both_remove_from_migration_pte:
	if (from_page_was_mapped) {
		if (rc == MIGRATEPAGE_SUCCESS)
			swap(from_page->index, from_index);

		remove_migration_ptes(from_page,
			rc == MIGRATEPAGE_SUCCESS ? to_page : from_page, false);

		if (rc == MIGRATEPAGE_SUCCESS)
			swap(from_page->index, from_index);
	}
	END_TIME("remove_migration_ptes*2", timestamp);



	if (rc == MIGRATEPAGE_SUCCESS) {
		if (from_page_count != page_count(to_page) ||
			to_page_count != page_count(from_page) ||
			from_map_count != page_mapcount(to_page) ||
			to_map_count != page_mapcount(from_page)) {

			if (page_mapping(from_page) &&
				!page_mapping(from_page)->a_ops->migratepage &&
				to_page_count == page_count(from_page) + 1 &&
				to_map_count == page_mapcount(from_page) &&
				from_page_count == page_count(to_page) &&
				from_map_count == page_mapcount(to_page)) {

			} else if ((PageWaiters(from_page)?
				to_page_count < page_count(from_page):
				to_page_count == page_count(from_page))&&
				to_map_count == page_mapcount(from_page) &&

				(PageWaiters(to_page)?
				from_page_count < page_count(to_page):
				from_page_count == page_count(to_page) )&&
				from_map_count == page_mapcount(to_page)) {
			} else {


			pr_debug("anon<->file: from_page_was_mapped: %d, to_page_was_mapped: %d\n",
				from_page_was_mapped, to_page_was_mapped);
			pr_debug("before: from_page_count: %d, from_map_count: %d, from_flags: %#lx(%pGp), from_mapping: %p, "
				"to_page_count: %d, to_map_count: %d, to_flags: %#lx(%pGp), to_mapping: %p\n",
				from_page_count, from_map_count, from_flags, &from_flags, from_mapping,
				to_page_count, to_map_count, to_flags, &to_flags, to_mapping);


			pr_dump_page(from_page, "after exchange: from");
			pr_dump_page(to_page, "after exchange: to");
			}
		}
	} else {
		if (from_page_count != page_count(from_page) ||
			to_page_count != page_count(to_page) ||
			from_map_count != page_mapcount(from_page) ||
			to_map_count != page_mapcount(to_page)) {

			if (page_mapping(to_page) &&
				!page_mapping(to_page)->a_ops->migratepage &&
				to_page_count == page_count(to_page) + 1 &&
				to_map_count == page_mapcount(to_page) &&
				from_page_count == page_count(from_page) &&
				from_map_count == page_mapcount(from_page)) {

			} else if ((PageWaiters(to_page)?
				to_page_count < page_count(to_page):
				to_page_count == page_count(to_page) )&&
				to_map_count == page_mapcount(to_page) &&

				(PageWaiters(from_page)?
				from_page_count < page_count(from_page):
				from_page_count == page_count(from_page) )&&
				from_map_count == page_mapcount(from_page)) {
			} else {
			pr_debug("anon<->file: from_page_was_mapped: %d, to_page_was_mapped: %d, rc: %d\n",
				from_page_was_mapped, to_page_was_mapped, rc);
			pr_debug("before: from_page_count: %d, from_map_count: %d, from_flags: %#lx(%pGp), from_mapping: %p, "
				"to_page_count: %d, to_map_count: %d, to_flags: %#lx(%pGp), to_mapping: %p\n",
				from_page_count, from_map_count, from_flags, &from_flags, from_mapping,
				to_page_count, to_map_count, to_flags, &to_flags, to_mapping);


			pr_dump_page(from_page, "exchange failed: from");
			pr_dump_page(to_page, "exchange failed: to");
			}
		}
	}
out_unlock_both:
	//START_TIME(timestamp);
	if (to_anon_vma)
		put_anon_vma(to_anon_vma);
	unlock_page(to_page);
	//END_TIME("put_anon_vma(to_anon_vma)", timestamp);
out_unlock:
	//START_TIME(timestamp);
	/* Drop an anon_vma reference if we took one */
	if (from_anon_vma)
		put_anon_vma(from_anon_vma);
	unlock_page(from_page);
	//END_TIME("put_anon_vma(from_anon_vma)", timestamp);
out:
	return rc;
}

static bool can_be_exchanged(struct page *from, struct page *to)
{
	if (PageCompound(from) != PageCompound(to))
		return false;

	if (PageHuge(from) != PageHuge(to))
		return false;

	if (PageHuge(from) || PageHuge(to))
		return false;

	if (compound_order(from) != compound_order(to))
		return false;

	return true;
}

/*
 * Exchange pages in the exchange_list
 *
 * Caller should release the exchange_list resource.
 *
 * */
int exchange_pages(struct list_head *exchange_list,
			enum migrate_mode mode)
{
	struct exchange_page_info *one_pair, *one_pair2;
	int failed = 0;

	list_for_each_entry_safe(one_pair, one_pair2, exchange_list, list) {
		struct page *from_page = one_pair->from_page;
		struct page *to_page = one_pair->to_page;
		int rc = -EBUSY;
		int retry = 0;
#ifdef DEBUG
		u64 timestamp;
#endif

again:
		if (page_count(from_page) == 1) {
			/* page was freed from under us. So we are done  */
			ClearPageActive(from_page);
			ClearPageUnevictable(from_page);

			put_page(from_page);
			dec_node_page_state(from_page, NR_ISOLATED_ANON +
					page_is_file_cache(from_page));

			if (page_count(to_page) == 1) {
				ClearPageActive(to_page);
				ClearPageUnevictable(to_page);
				put_page(to_page);
			} else
				goto putback_to_page;

			continue;
		}

		if (page_count(to_page) == 1) {
			/* page was freed from under us. So we are done  */
			ClearPageActive(to_page);
			ClearPageUnevictable(to_page);

			put_page(to_page);
			dec_node_page_state(to_page, NR_ISOLATED_ANON +
					page_is_file_cache(to_page));

			continue;
		}

		/* TODO: compound page not supported */
		if (!can_be_exchanged(from_page, to_page) ||
			page_mapping(from_page)
			/* allow to_page to be file-backed page  */
			/*|| page_mapping(to_page)*/
			) {
			++failed;
			goto putback;
		}

		//START_TIME(timestamp);
		rc = unmap_and_exchange(from_page, to_page, mode);
		//END_TIME("unmap_and_exchange", timestamp);

		if (rc == -EAGAIN && retry < 3) {
			++retry;
			goto again;
		}

		if (rc != MIGRATEPAGE_SUCCESS) {
			++failed;
			trace_dump_page(from_page, "from_page");
			trace_dump_page(to_page, "to_page");
			count_vm_event(PGEXCHANGE_FAIL);
		}

		if (rc == MIGRATEPAGE_SUCCESS) {
			count_vm_events(PGEXCHANGE_SUCCESS, hpage_nr_pages(from_page));
			count_vm_events(PGEXCHANGE_SUCCESS, hpage_nr_pages(to_page));
		}

		START_TIME(timestamp);
putback:
		dec_node_page_state(from_page, NR_ISOLATED_ANON +
				page_is_file_cache(from_page));

		putback_lru_page(from_page);

putback_to_page:
		/*if (!__PageMovable(to_page)) {*/
		dec_node_page_state(to_page, NR_ISOLATED_ANON +
				page_is_file_cache(to_page));

		putback_lru_page(to_page);
		/*} else {*/
			/*putback_movable_page(to_page);*/
		/*}*/
		END_TIME("putback_lru_page*2", timestamp);
	}
	return failed;
}


/* page1's ref_cout: 2, page2's ref_cout: 1 */
int exchange_two_pages(struct page *page1, struct page *page2,
				enum migrate_mode mode)
{
	struct exchange_page_info page_info;
	LIST_HEAD(exchange_list);
	int err = -EFAULT;
	int pagevec_flushed = 0;
	int ret;
#ifdef DEBUG
	u64 last_timestamp;
#endif
	int migrate_concur =  mode & MIGRATE_CONCUR;

	if (!(PageLRU(page1) && PageLRU(page2)))
		return -EBUSY;

	if (page_count(page1) != 2 || page_count(page2) != 1)
		return -EBUSY;

retry_isolate1:
	if (!get_page_unless_zero(page1))
		return -EBUSY;
	err = isolate_lru_page(page1);
	if (err) {
		if (!pagevec_flushed) {
			migrate_prep();
			pagevec_flushed = 1;
			goto retry_isolate1;
		}
		put_page(page1); // get_page_unless_zero()
		return err;
	} else {
		put_page(page1); // get_page_unless_zero()
		put_page(page1); // isolate_lru_page()
	}
	inc_node_page_state(page1,
			NR_ISOLATED_ANON + page_is_file_cache(page1));

retry_isolate2:
	if (!get_page_unless_zero(page2)) {
		dec_node_page_state(page1,
				NR_ISOLATED_ANON + page_is_file_cache(page1));
		get_page(page1);
		putback_lru_page(page1);
		return -EBUSY;
	}
	err = isolate_lru_page(page2);
	if (err) {
		if (!pagevec_flushed) {
			migrate_prep();
			pagevec_flushed = 1;
			goto retry_isolate2;
		}
		dec_node_page_state(page1,
				NR_ISOLATED_ANON + page_is_file_cache(page1));
		get_page(page1);
		putback_lru_page(page1);

		put_page(page2);
		return err;
	} else {
		put_page(page2);
	}
	inc_node_page_state(page2,
			NR_ISOLATED_ANON + page_is_file_cache(page2));

	VM_BUG_ON_PAGE(PageTail(page1), page1);
	VM_BUG_ON_PAGE(PageTail(page2), page2);

	page_info.from_page = page1;
	page_info.to_page = page2;
	INIT_LIST_HEAD(&page_info.list);
	list_add(&page_info.list, &exchange_list);

	//START_TIME(last_timestamp);
	if (migrate_concur)
		ret = exchange_pages_concur(&exchange_list, mode);
	else
		ret = exchange_pages(&exchange_list, mode);

	//END_TIME("exchange_pages", last_timestamp);
	return ret;
}

static int unmap_pair_pages_concur(struct exchange_page_info *one_pair,
				int force, enum migrate_mode mode)
{
	int rc = -EAGAIN;
	struct anon_vma *anon_vma_from_page = NULL, *anon_vma_to_page = NULL;
	struct page *from_page = one_pair->from_page;
	struct page *to_page = one_pair->to_page;
#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	u64 timestamp;
#endif

	one_pair->from_index = from_page->index;
	one_pair->to_index = to_page->index;
	/* from_page lock down  */
	if (!trylock_page(from_page)) {
		if (!force || ((mode & MIGRATE_MODE_MASK) == MIGRATE_ASYNC))
			goto out;

		lock_page(from_page);
	}

	BUG_ON(PageWriteback(from_page));

	/*
	 * By try_to_unmap(), page->mapcount goes down to 0 here. In this case,
	 * we cannot notice that anon_vma is freed while we migrates a page.
	 * This get_anon_vma() delays freeing anon_vma pointer until the end
	 * of migration. File cache pages are no problem because of page_lock()
	 * File Caches may use write_page() or lock_page() in migration, then,
	 * just care Anon page here.
	 *
	 * Only page_get_anon_vma() understands the subtleties of
	 * getting a hold on an anon_vma from outside one of its mms.
	 * But if we cannot get anon_vma, then we won't need it anyway,
	 * because that implies that the anon page is no longer mapped
	 * (and cannot be remapped so long as we hold the page lock).
	 */
	if (PageAnon(from_page) && !PageKsm(from_page))
		one_pair->from_anon_vma = anon_vma_from_page
					= page_get_anon_vma(from_page);

	/* to_page lock down  */
	if (!trylock_page(to_page)) {
		if (!force || ((mode & MIGRATE_MODE_MASK) == MIGRATE_ASYNC))
			goto out_unlock;

		lock_page(to_page);
	}

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	timestamp = rdtsc();
	current->move_pages_breakdown.lock_page_cycles += timestamp -
		current->move_pages_breakdown.last_timestamp;
	current->move_pages_breakdown.last_timestamp = timestamp;
#endif

	BUG_ON(PageWriteback(to_page));

	/*
	 * By try_to_unmap(), page->mapcount goes down to 0 here. In this case,
	 * we cannot notice that anon_vma is freed while we migrates a page.
	 * This get_anon_vma() delays freeing anon_vma pointer until the end
	 * of migration. File cache pages are no problem because of page_lock()
	 * File Caches may use write_page() or lock_page() in migration, then,
	 * just care Anon page here.
	 *
	 * Only page_get_anon_vma() understands the subtleties of
	 * getting a hold on an anon_vma from outside one of its mms.
	 * But if we cannot get anon_vma, then we won't need it anyway,
	 * because that implies that the anon page is no longer mapped
	 * (and cannot be remapped so long as we hold the page lock).
	 */
	if (PageAnon(to_page) && !PageKsm(to_page))
		one_pair->to_anon_vma = anon_vma_to_page = page_get_anon_vma(to_page);

	/*
	 * Corner case handling:
	 * 1. When a new swap-cache page is read into, it is added to the LRU
	 * and treated as swapcache but it has no rmap yet.
	 * Calling try_to_unmap() against a page->mapping==NULL page will
	 * trigger a BUG.  So handle it here.
	 * 2. An orphaned page (see truncate_complete_page) might have
	 * fs-private metadata. The page can be picked up due to memory
	 * offlining.  Everywhere else except page reclaim, the page is
	 * invisible to the vm, so the page can not be migrated.  So try to
	 * free the metadata, so the page can be freed.
	 */
	if (!from_page->mapping) {
		VM_BUG_ON_PAGE(PageAnon(from_page), from_page);
		if (page_has_private(from_page)) {
			try_to_free_buffers(from_page);
			goto out_unlock_both;
		}
	} else if (page_mapped(from_page)) {
		/* Establish migration ptes */
		VM_BUG_ON_PAGE(PageAnon(from_page) && !PageKsm(from_page) &&
					   !anon_vma_from_page, from_page);
		try_to_unmap(from_page,
			TTU_MIGRATION|TTU_IGNORE_MLOCK|TTU_IGNORE_ACCESS);

		one_pair->from_page_was_mapped = 1;
	}

	if (!to_page->mapping) {
		VM_BUG_ON_PAGE(PageAnon(to_page), to_page);
		if (page_has_private(to_page)) {
			try_to_free_buffers(to_page);
			goto out_unlock_both;
		}
	} else if (page_mapped(to_page)) {
		/* Establish migration ptes */
		VM_BUG_ON_PAGE(PageAnon(to_page) && !PageKsm(to_page) &&
					   !anon_vma_to_page, to_page);
		try_to_unmap(to_page,
			TTU_MIGRATION|TTU_IGNORE_MLOCK|TTU_IGNORE_ACCESS);

		one_pair->to_page_was_mapped = 1;
	}

	return MIGRATEPAGE_SUCCESS;

out_unlock_both:
	if (anon_vma_to_page)
		put_anon_vma(anon_vma_to_page);
	unlock_page(to_page);
out_unlock:
	/* Drop an anon_vma reference if we took one */
	if (anon_vma_from_page)
		put_anon_vma(anon_vma_from_page);
	unlock_page(from_page);
out:

	return rc;
}

static int exchange_page_mapping_concur(struct list_head *unmapped_list_ptr,
					   struct list_head *exchange_list_ptr,
						enum migrate_mode mode)
{
	int nr_failed = 0;
	struct address_space *to_page_mapping, *from_page_mapping;
	struct exchange_page_info *one_pair, *one_pair2;

	list_for_each_entry_safe(one_pair, one_pair2, unmapped_list_ptr, list) {
		struct page *from_page = one_pair->from_page;
		struct page *to_page = one_pair->to_page;
		int rc = -EBUSY;

		VM_BUG_ON_PAGE(!PageLocked(from_page), from_page);
		VM_BUG_ON_PAGE(!PageLocked(to_page), to_page);

		/* copy page->mapping not use page_mapping()  */
		to_page_mapping = page_mapping(to_page);
		from_page_mapping = page_mapping(from_page);

		BUG_ON(from_page_mapping);
		BUG_ON(to_page_mapping);

		BUG_ON(PageWriteback(from_page));
		BUG_ON(PageWriteback(to_page));

		/* actual page mapping exchange */
		if (!page_mapped(from_page) && !page_mapped(to_page))
			rc = exchange_page_move_mapping(to_page_mapping, from_page_mapping,
								to_page, from_page, NULL, NULL, mode, 0, 0);

		if (rc) {
			if (one_pair->from_page_was_mapped)
				remove_migration_ptes(from_page, from_page, false);
			if (one_pair->to_page_was_mapped)
				remove_migration_ptes(to_page, to_page, false);

			if (one_pair->from_anon_vma)
				put_anon_vma(one_pair->from_anon_vma);
			unlock_page(from_page);

			if (one_pair->to_anon_vma)
				put_anon_vma(one_pair->to_anon_vma);
			unlock_page(to_page);

			mod_node_page_state(page_pgdat(from_page), NR_ISOLATED_ANON +
					page_is_file_cache(from_page), -hpage_nr_pages(from_page));
			putback_lru_page(from_page);

			mod_node_page_state(page_pgdat(to_page), NR_ISOLATED_ANON +
					page_is_file_cache(to_page), -hpage_nr_pages(to_page));
			putback_lru_page(to_page);

			one_pair->from_page = NULL;
			one_pair->to_page = NULL;

			list_del(&one_pair->list);
			++nr_failed;
		}
	}

	return nr_failed;
}

static int exchange_page_data_concur(struct list_head *unmapped_list_ptr,
									enum migrate_mode mode)
{
	struct exchange_page_info *one_pair;
	int num_pages = 0, idx = 0;
	struct page **src_page_list = NULL, **dst_page_list = NULL;
	unsigned long size = 0;
	int rc = -EFAULT;
#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	u64 timestamp;
#endif

	if (list_empty(unmapped_list_ptr))
		return 0;

	/* form page list  */
	list_for_each_entry(one_pair, unmapped_list_ptr, list) {
		++num_pages;
		size += PAGE_SIZE * hpage_nr_pages(one_pair->from_page);
	}

	src_page_list = kzalloc(sizeof(struct page *)*num_pages, GFP_KERNEL);
	if (!src_page_list)
		return -ENOMEM;
	dst_page_list = kzalloc(sizeof(struct page *)*num_pages, GFP_KERNEL);
	if (!dst_page_list)
		return -ENOMEM;

	list_for_each_entry(one_pair, unmapped_list_ptr, list) {
		src_page_list[idx] = one_pair->from_page;
		dst_page_list[idx] = one_pair->to_page;
		++idx;
	}

	BUG_ON(idx != num_pages);

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	timestamp = rdtsc();
	current->move_pages_breakdown.change_page_mapping_cycles += timestamp -
		current->move_pages_breakdown.last_timestamp;
	current->move_pages_breakdown.last_timestamp = timestamp;
#endif

	if (mode & MIGRATE_MT)
		rc = exchange_page_lists_mthread(dst_page_list, src_page_list,
				num_pages);

	if (rc) {
		list_for_each_entry(one_pair, unmapped_list_ptr, list) {
			if (PageHuge(one_pair->from_page) ||
				PageTransHuge(one_pair->from_page)) {
				exchange_huge_page(one_pair->to_page, one_pair->from_page);
			} else {
				exchange_highpage(one_pair->to_page, one_pair->from_page);
			}
		}
	}

	list_for_each_entry(one_pair, unmapped_list_ptr, list) {
		exchange_page_flags(one_pair->to_page, one_pair->from_page);
	}

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	timestamp = rdtsc();
	current->move_pages_breakdown.copy_page_cycles += timestamp -
		current->move_pages_breakdown.last_timestamp;
	current->move_pages_breakdown.last_timestamp = timestamp;
#endif

	kfree(src_page_list);
	kfree(dst_page_list);


	return rc;
}

static int remove_migration_ptes_concur(struct list_head *unmapped_list_ptr)
{
	struct exchange_page_info *iterator;
	LIST_HEAD(putback_list);
#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	u64 timestamp;
#endif
#ifdef DEBUG
	u64 timestamp, last_timestamp, list_timestamp;
	list_timestamp = last_timestamp = timestamp = rdtsc();
#endif

	list_for_each_entry(iterator, unmapped_list_ptr, list) {
		struct page *from_page = iterator->from_page;
		struct page *to_page = iterator->to_page;

		swap(from_page->index, iterator->from_index);
		if (iterator->from_page_was_mapped)
			remove_migration_ptes(iterator->from_page, iterator->to_page, false);
		swap(from_page->index, iterator->from_index);

		swap(to_page->index, iterator->to_index);
		if (iterator->to_page_was_mapped)
			remove_migration_ptes(iterator->to_page, iterator->from_page, false);
		swap(to_page->index, iterator->to_index);
#ifdef DEBUG
		timestamp = rdtsc();
		trace_printk("remove_migration_ptes timestamp: %llu\n", timestamp - last_timestamp);
		last_timestamp = timestamp;
#endif

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
		timestamp = rdtsc();
		current->move_pages_breakdown.remove_migration_ptes_cycles += timestamp -
			current->move_pages_breakdown.last_timestamp;
		current->move_pages_breakdown.last_timestamp = timestamp;
#endif
		dec_node_page_state(iterator->from_page, NR_ISOLATED_ANON +
				page_is_file_cache(iterator->from_page));
		count_vm_events(PGEXCHANGE_SUCCESS, hpage_nr_pages(iterator->from_page));


		dec_node_page_state(iterator->to_page, NR_ISOLATED_ANON +
				page_is_file_cache(iterator->to_page));
		count_vm_events(PGEXCHANGE_SUCCESS, hpage_nr_pages(iterator->to_page));


		if (iterator->from_anon_vma)
			put_anon_vma(iterator->from_anon_vma);
		unlock_page(iterator->from_page);
		if (iterator->to_anon_vma)
			put_anon_vma(iterator->to_anon_vma);
		unlock_page(iterator->to_page);

		putback_lru_page(iterator->from_page);
		iterator->from_page = NULL;



#ifdef CONFIG_PAGE_MIGRATION_PROFILE
		timestamp = rdtsc();
		current->move_pages_breakdown.putback_old_page_cycles += timestamp -
			current->move_pages_breakdown.last_timestamp;
		current->move_pages_breakdown.last_timestamp = timestamp;
#endif
		putback_lru_page(iterator->to_page);
		iterator->to_page = NULL;

#ifdef DEBUG
		timestamp = rdtsc();
		trace_printk("swap page timestamp: %llu\n", timestamp - last_timestamp);
		last_timestamp = timestamp;
#endif
#ifdef CONFIG_PAGE_MIGRATION_PROFILE
		timestamp = rdtsc();
		current->move_pages_breakdown.putback_new_page_cycles += timestamp -
			current->move_pages_breakdown.last_timestamp;
		current->move_pages_breakdown.last_timestamp = timestamp;
#endif
	}

	return 0;
}

int exchange_pages_concur(struct list_head *exchange_list,
		enum migrate_mode mode)
{
	struct exchange_page_info *one_pair, *one_pair2;
	int pass = 0;
	int retry = 1;
	int nr_failed = 0;
	int nr_succeeded = 0;
	int rc = 0;
	LIST_HEAD(serialized_list);
	LIST_HEAD(unmapped_list);
#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	u64 timestamp;

	timestamp = rdtsc();
	current->move_pages_breakdown.enter_unmap_and_move_cycles += timestamp -
		current->move_pages_breakdown.last_timestamp;
	current->move_pages_breakdown.last_timestamp = timestamp;
#endif

	for(pass = 0; pass < 1 && retry; pass++) {
		retry = 0;

		/* unmap and get new page for page_mapping(page) == NULL */
		list_for_each_entry_safe(one_pair, one_pair2, exchange_list, list) {
			struct page *from_page = one_pair->from_page;
			struct page *to_page = one_pair->to_page;
			cond_resched();

			if (page_count(from_page) == 1) {
				/* page was freed from under us. So we are done  */
				ClearPageActive(from_page);
				ClearPageUnevictable(from_page);

				put_page(from_page);
				dec_node_page_state(from_page, NR_ISOLATED_ANON +
						page_is_file_cache(from_page));

				if (page_count(to_page) == 1) {
					ClearPageActive(to_page);
					ClearPageUnevictable(to_page);
					put_page(to_page);
				} else {
					mod_node_page_state(page_pgdat(to_page), NR_ISOLATED_ANON +
							page_is_file_cache(to_page), -hpage_nr_pages(to_page));
					putback_lru_page(to_page);
				}
				list_del(&one_pair->list);

				continue;
			}

			if (page_count(to_page) == 1) {
				/* page was freed from under us. So we are done  */
				ClearPageActive(to_page);
				ClearPageUnevictable(to_page);

				put_page(to_page);

				dec_node_page_state(to_page, NR_ISOLATED_ANON +
						page_is_file_cache(to_page));

				mod_node_page_state(page_pgdat(from_page), NR_ISOLATED_ANON +
						page_is_file_cache(from_page), -hpage_nr_pages(from_page));
				putback_lru_page(from_page);

				list_del(&one_pair->list);
				continue;
			}
		/* We do not exchange huge pages and file-backed pages concurrently */
			if (PageHuge(one_pair->from_page) || PageHuge(one_pair->to_page)) {
				rc = -ENODEV;
			}
			else if ((page_mapping(one_pair->from_page) != NULL) ||
					 (page_mapping(one_pair->from_page) != NULL)) {
				rc = -ENODEV;
			}
			else
				rc = unmap_pair_pages_concur(one_pair, 1, mode);

			switch(rc) {
			case -ENODEV:
				list_move(&one_pair->list, &serialized_list);
				break;
			case -ENOMEM:
				goto out;
			case -EAGAIN:
				retry++;
				break;
			case MIGRATEPAGE_SUCCESS:
				list_move(&one_pair->list, &unmapped_list);
				nr_succeeded++;
				break;
			default:
				/*
				 * Permanent failure (-EBUSY, -ENOSYS, etc.):
				 * unlike -EAGAIN case, the failed page is
				 * removed from migration page list and not
				 * retried in the next outer loop.
				 */
				list_move(&one_pair->list, &serialized_list);
				nr_failed++;
				break;
			}
		}

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
		timestamp = rdtsc();
		current->move_pages_breakdown.unmap_page_cycles += timestamp -
			current->move_pages_breakdown.last_timestamp;
		current->move_pages_breakdown.last_timestamp = timestamp;
#endif

		/* move page->mapping to new page, only -EAGAIN could happen  */
		exchange_page_mapping_concur(&unmapped_list, exchange_list, mode);

		/* copy pages in unmapped_list */
		exchange_page_data_concur(&unmapped_list, mode);

		/* remove migration pte, if old_page is NULL?, unlock old and new
		 * pages, put anon_vma, put old and new pages */
		remove_migration_ptes_concur(&unmapped_list);
	}

	nr_failed += retry;
	rc = nr_failed;

	exchange_pages(&serialized_list, mode);
out:
	list_splice(&unmapped_list, exchange_list);
	list_splice(&serialized_list, exchange_list);

	return nr_failed?-EFAULT:0;
}

int try_exchange_page(struct page *page, int dst_nid)
{
	struct pglist_data *pgdat = NODE_DATA(dst_nid);
	struct page *dst_page = NULL;
	struct page_info *pi, *pi2;
	struct page_ext *page_ext;
	int src_nid;
	int last_nid = NUMA_NO_NODE;
	int scan, nr_to_scan = 16;
#ifdef DEBUG
	u64 timestamp;
#endif
	enum migrate_mode mode = MIGRATE_SYNC;

	src_nid = page_to_nid(page);
	scan = 0;

	if (!(sysctl_numa_balancing_extended_mode & NUMA_BALANCING_EXCHANGE))
		return false;

	spin_lock_irq(&pgdat->lru_lock);

	/* Check that whether page is now exchanging or not*/
	if (list_empty(&pgdat->deferred_list)) {
		count_vm_event(PGEXCHANGE_LIST_EMPTY_FAIL);
		spin_unlock_irq(&pgdat->lru_lock);
		return false;
	}

	if (!trylock_busy(page)) {
		spin_unlock_irq(&pgdat->lru_lock);
		return false;
	}

	list_for_each_entry_safe(pi, pi2, &pgdat->deferred_list, list) {

		if (unlikely(scan > nr_to_scan))
			break;

		dst_page = get_page_from_page_info(pi);
		if (!dst_page) {
			count_vm_event(PGEXCHANGE_NO_PAGE_FAIL);
			trace_printk("exch: pfn:%lu,last_cpu:%d\n", pi->pfn, pi->last_cpu);
			page_ext = get_page_ext(pi);
			clear_bit(PAGE_EXT_DEFERRED, &page_ext->flags);
			clear_bit(PAGE_EXT_BUSY_LOCK, &page_ext->flags);
			__mod_node_page_state(pgdat, NR_DEFERRED, -1);
			list_del(&pi->list);
			goto skip_page;
		}

		last_nid = cpu_to_node(pi->last_cpu);

		if (last_nid == src_nid) {
			if ((PageTransHuge(page) ^ PageTransHuge(dst_page)))
				goto skip_page;

			if (unlikely(!PageLRU(dst_page)))
				goto skip_page;

			if (unlikely(!trylock_busy(dst_page))) {
				count_vm_event(PGEXCHANGE_BUSY_FAIL);
				goto skip_page;
			}

			del_page_from_deferred_list(dst_page);
			break;
		}

		if (last_nid != src_nid) {
			list_move_tail(&pi->list, &pgdat->deferred_list);
			count_vm_event(PGEXCHANGE_NODE_UNMATCH_FAIL);
		}
skip_page:
		count_vm_event(NR_PAGE_SKIPPED);
		scan++;
	}

	spin_unlock_irq(&pgdat->lru_lock);

	if (unlikely(scan > nr_to_scan)) {
		count_vm_event(PGEXCHANGE_SCAN_FAIL);
		goto unlock_src_fail;
	}

	if (PageTransHuge(page) && PageTransHuge(dst_page)) {
		// mode |= MIGRATE_CONCUR;
		mode |= MIGRATE_MT;
		unlock_page(page);
	}

	//put_page(page);
	if (exchange_two_pages(page, dst_page, mode)) {

		if (PageTransHuge(page))
			lock_page(page);

		//get_page(page);
		goto unlock_dst_fail;
	}

	unlock_busy(page);
	unlock_busy(dst_page);
	return true;

unlock_dst_fail:
	unlock_busy(dst_page);

unlock_src_fail:
	unlock_busy(page);
	return false;
}

#ifdef CONFIG_SYSFS
static ssize_t exchange_mt_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", exchange_mt);
}
static ssize_t exchange_mt_store(struct kobject *kobj,
		struct kobj_attribute *attr,
		const char *buf, size_t count)
{
	unsigned long mt;
	int err;

	err = kstrtoul(buf, 10, &mt);
	if (err || mt > 1 || mt < 0)
		return -EINVAL;

	exchange_mt = mt;

	return count;
}
static struct kobj_attribute exchange_mt_attr =
__ATTR(exchange_mt, 0644, exchange_mt_show,
		exchange_mt_store);

static ssize_t exchange_concur_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", exchange_concur);
}
static ssize_t exchange_concur_store(struct kobject *kobj,
		struct kobj_attribute *attr,
		const char *buf, size_t count)
{
	unsigned long concur;
	int err;

	err = kstrtoul(buf, 10, &concur);
	if (err || concur > 1 || concur < 0)
		return -EINVAL;

	exchange_concur = concur;

	return count;
}
static struct kobj_attribute exchange_concur_attr =
__ATTR(exchange_concur, 0644, exchange_concur_show,
		exchange_concur_store);

static struct attribute *numa_exchange_attr[] = {
	&exchange_concur_attr.attr,
	&exchange_mt_attr.attr,
	NULL,
};

static struct attribute_group numa_exchange_attr_group = {
	.attrs = numa_exchange_attr,
};
static int __init numa_exchange_init_sysfs(struct kobject **exchange_kobj) {
	int err;

	*exchange_kobj = kobject_create_and_add("exchange", mm_kobj);
	if (unlikely(!*exchange_kobj)) {
		pr_err("failed to create exchange kobject\n");
		return -ENOMEM;
	}

	err = sysfs_create_group(*exchange_kobj, &numa_exchange_attr_group);
	if (err) {
		pr_err("failed to register exchange group\n");
		goto delete_obj;
	}

	return 0;

	sysfs_remove_group(*exchange_kobj, &numa_exchange_attr_group);
delete_obj:
	kobject_put(*exchange_kobj);
	return err;
}

static void __init numa_exchange_exit_sysfs(struct kobject *exchange_kobj)
{
	sysfs_remove_group(exchange_kobj, &numa_exchange_attr_group);
	kobject_put(exchange_kobj);
}
#else
static inline int numa_exchange_init_sysfs(struct kobject **exchange_kobj)
{
	return 0;
}
static inline void numa_exchange_exit_sysfs(struct kobject *exchange_kobj)
{
}
#endif

static int __init numa_exchange_init(void)
{
	int err;
	struct kobject *sysfs_exchange_kobj;

	err = numa_exchange_init_sysfs(&sysfs_exchange_kobj);
	if (err) {
		pr_err("failed start numa_exchange_init becasue sysfs\n");
		goto err_sysfs;
	}
	return 0;

	numa_exchange_exit_sysfs(sysfs_exchange_kobj);
err_sysfs:
	return err;
}
subsys_initcall(numa_exchange_init);
