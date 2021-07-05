// SPDX-License-Identifier: GPL-2.0
/*
 * Balance pages in tiered memory system. This scheme includes page promotion,
 * demotion, and exchange across NUMA nodes.
 *
 * Author: Jonghyeon Kim <tome01@ajou.ac.kr>
 */

#include <linux/debugfs.h>
#include <linux/mm.h>
#include <linux/huge_mm.h>
#include <linux/uaccess.h>
#include <linux/jump_label.h>
#include <linux/exchange.h>
#include <linux/memcontrol.h>
#include <linux/node.h>

#include <linux/page_balancing.h>

#include "internal.h"

unsigned int background_demotion = 0;
unsigned int batch_demotion = 0;
unsigned int thp_mt_copy = 0;
unsigned int skip_lower_tier = 1;

static bool need_page_balancing(void)
{
	return true;
}

static inline struct page_info *get_page_info(struct page_ext *page_ext)
{
	return (void *)page_ext + page_info_ops.offset;
}

struct page_ext *get_page_ext(struct page_info *page_info)
{
	return (void *)page_info - page_info_ops.offset;
}

struct page *get_page_from_page_info(struct page_info *page_info)
{
	if (page_info->pfn)
		return pfn_to_page(page_info->pfn);
	else
		return NULL;
}

struct page_info *get_page_info_from_page(struct page *page)
{
	struct page_ext *page_ext = lookup_page_ext(page);
	if (!page_ext)
		return NULL;
	return get_page_info(page_ext);
}

void set_page_to_page_info(struct page *page,
				struct page_info *page_info)
{
	page_info->pfn = page_to_pfn(page);
}

unsigned int __get_page_access_lv(struct page_info *pi)
{
	int i;
	unsigned int lv = 0;
	u8 bitmap = pi->access_bitmap;

	for (i = 0; i < ACCESS_HISTORY_SIZE; i++) {
		lv = lv + (bitmap & 1);
		bitmap = bitmap >> 1;
	}
	return lv;
}

static inline unsigned int __PageTracked(struct page_ext *page_ext)
{
	return test_bit(PAGE_EXT_TRACKED, &page_ext->flags);
}

static inline void __SetPageTracked(struct page_ext *page_ext)
{
	set_bit(PAGE_EXT_TRACKED, &page_ext->flags);
}

static inline void __ClearPageTracked(struct page_ext *page_ext)
{
	clear_bit(PAGE_EXT_TRACKED, &page_ext->flags);
}

static inline unsigned int __PageDeferred(struct page_ext *page_ext)
{
	return test_bit(PAGE_EXT_DEFERRED, &page_ext->flags);
}

static inline void __SetPageDeferred(struct page_ext *page_ext)
{
	set_bit(PAGE_EXT_DEFERRED, &page_ext->flags);
}

static inline void __ClearPageDeferred(struct page_ext *page_ext)
{
	clear_bit(PAGE_EXT_DEFERRED, &page_ext->flags);
}


static inline unsigned int __PageDemoted(struct page_ext *page_ext)
{
	return test_bit(PAGE_EXT_DEMOTED, &page_ext->flags);
}

static inline void __SetPageDemoted(struct page_ext *page_ext)
{
	set_bit(PAGE_EXT_DEMOTED, &page_ext->flags);
}

static inline void __ClearPageDemoted(struct page_ext *page_ext)
{
	clear_bit(PAGE_EXT_DEMOTED, &page_ext->flags);
}

static inline void __del_page_from_deferred_list(struct page_ext *page_ext,
				struct page *page)
{
	struct page_info *pi = get_page_info(page_ext);
	struct pglist_data *pgdat = page_pgdat(page);

	if (__PageTracked(page_ext)) {
		unsigned int lv = __get_page_access_lv(pi);
		if (--(pgdat->lap_area[lv].nr_free) < 0)
			pgdat->lap_area[lv].nr_free = 0;

		__ClearPageTracked(page_ext);
		__mod_lruvec_page_state(page, NR_TRACKED, -hpage_nr_pages(page));
		list_del(&pi->list);
	} else if (__PageDeferred(page_ext)) {
		__ClearPageDeferred(page_ext);
		__mod_lruvec_page_state(page, NR_DEFERRED, -hpage_nr_pages(page));
		list_del(&pi->list);
	}
}

static inline unsigned int __PageBusyLock(struct page_ext *page_ext)
{
	return test_bit(PAGE_EXT_BUSY_LOCK, &page_ext->flags);
}

static inline void __lock_busy(struct page_ext *page_ext)
{
	set_bit(PAGE_EXT_BUSY_LOCK, &page_ext->flags);
}

static inline unsigned int __trylock_busy(struct page_ext *page_ext)
{
	if (test_bit(PAGE_EXT_BUSY_LOCK, &page_ext->flags))
		return 0;
	else {
		__lock_busy(page_ext);
		return 1;
	}
}

static inline void __unlock_busy(struct page_ext *page_ext)
{
	clear_bit(PAGE_EXT_BUSY_LOCK, &page_ext->flags);
}

static inline void __clear_page_info(struct page_ext *page_ext)
{
	struct page_info *pi = get_page_info(page_ext);
	pi->pfn = 0;
	pi->access_bitmap = 0;
}

unsigned int PageTracked(struct page *page)
{
	struct page_ext *page_ext;

	if (!(sysctl_numa_balancing_extended_mode & NUMA_BALANCING_OPM))
		return 0;

	page_ext = lookup_page_ext(page);
	if (unlikely(!page_ext))
		return 0;

	return __PageTracked(page_ext);
}

void ClearPageTracked(struct page *page)
{
	struct page_ext *page_ext;

	if (!(sysctl_numa_balancing_extended_mode & NUMA_BALANCING_OPM))
		return;

	page_ext = lookup_page_ext(page);
	if (unlikely(!page_ext))
		return;

	if (!__PageTracked(page_ext))
		return;

	VM_BUG_ON(!__PageTracked(page_ext));

	__ClearPageTracked(page_ext);
	__mod_lruvec_page_state(page, NR_TRACKED, -hpage_nr_pages(page));
}

unsigned int PageDeferred(struct page *page)
{
	struct page_ext *page_ext;

	if (!(sysctl_numa_balancing_extended_mode & NUMA_BALANCING_EXCHANGE))
		return 0;

	page_ext = lookup_page_ext(page);
	if (unlikely(!page_ext))
		return 0;

	return __PageDeferred(page_ext);
}


void ClearPageDeferred(struct page *page)
{
	struct page_ext *page_ext;

	if (!(sysctl_numa_balancing_extended_mode & NUMA_BALANCING_EXCHANGE))
		return;

	page_ext = lookup_page_ext(page);
	if (unlikely(!page_ext))
		return;

	if (!__PageDeferred(page_ext))
		return;

	VM_BUG_ON(!__PageDeferred(page_ext));

	__ClearPageDeferred(page_ext);
	__mod_lruvec_page_state(page, NR_DEFERRED, -hpage_nr_pages(page));
}

unsigned int PageDemoted(struct page *page)
{
	struct page_ext *page_ext = lookup_page_ext(page);
	if (unlikely(!page_ext))
		return 0;
	return __PageDemoted(page_ext);
}


void ClearPageDemoted(struct page *page)
{
	struct page_ext *page_ext;

	page_ext = lookup_page_ext(page);
	if (unlikely(!page_ext))
		return;

	if (!__PageDemoted(page_ext))
		return;

	__ClearPageDemoted(page_ext);
}

#ifdef CONFIG_PAGE_BALANCING_DEBUG
void trace_dump_page(struct page *page, const char *msg)
{
	trace_printk("dump:%s page(%p):0x%lx,"
		"refcount:%d,mapcount:%d,mapping:%p,index:%#lx,flags:%#lx(%pGp),"
		"%s,%s,%s,%s,page_nid:%d\n",
		msg,
		page,
		page_to_pfn(page),
		page_ref_count(page),
		PageSlab(page)?0:page_mapcount(page),
		page->mapping, page_to_pgoff(page),
		page->flags, &page->flags,
		PageCompound(page)?"compound_page":"single_page",
		PageDirty(page)?"dirty":"clean",
		PageDeferred(page)?"deferred":"nondeferred",
		PageTracked(page)?"tracked":"nontracked",
		page_to_nid(page)
		);
}

static void print_access_history(const char *msg, struct page *page,
		struct page_info *pi)
{
	char buf[10];
	unsigned int i, node_id = page_to_nid(page);
	unsigned long pfn = page_to_pfn(page);
	char hi, lo;
	u8 bitmap = pi->access_bitmap;

	for (i = 0; i < ACCESS_HISTORY_SIZE; i++) {
		if (bitmap & 1)
			buf[i] = '1';
		else
			buf[i] = '0';

		bitmap = bitmap >> 1;
	}

	buf[ACCESS_HISTORY_SIZE] = '\0';

	trace_printk("%s pfn:[%6lx],access:[%8s],lv:[%u],node:[%u],last_cpu[%d]\n",
			msg, pfn, buf, __get_page_access_lv(pi), node_id, pi->last_cpu);
}
#else
static inline void print_access_history(const char *msg, struct page *page,
		struct page_info *pi)
{
}
#endif /* CONFIG_PAGE_BALANCING_DEBUG */

void SetPageTracked(struct page *page)
{
	struct page_ext *page_ext;

	if (!(sysctl_numa_balancing_extended_mode & NUMA_BALANCING_OPM))
		return;

	page_ext = lookup_page_ext(page);
	if (unlikely(!page_ext))
		return;

	__SetPageTracked(page_ext);
	__mod_lruvec_page_state(page, NR_TRACKED, hpage_nr_pages(page));
}

void SetPageDeferred(struct page *page)
{
	struct page_ext *page_ext;

	if (!(sysctl_numa_balancing_extended_mode & NUMA_BALANCING_EXCHANGE))
		return;

	page_ext = lookup_page_ext(page);
	if (unlikely(!page_ext))
		return;

	__SetPageDeferred(page_ext);
	__mod_lruvec_page_state(page, NR_DEFERRED, hpage_nr_pages(page));
}

void SetPageDemoted(struct page *page)
{
	struct page_ext *page_ext;

	if (!(sysctl_numa_balancing_extended_mode & NUMA_BALANCING_OPM))
		return;
	page_ext = lookup_page_ext(page);
	if (unlikely(!page_ext))
		return;

	__SetPageDemoted(page_ext);
}

void clear_page_info(struct page *page)
{
	struct page_ext *page_ext = lookup_page_ext(page);
	if (unlikely(!page_ext))
		return;
	__clear_page_info(page_ext);
}

/* page should be locked from pgdat->lru_lock */
void del_page_from_deferred_list(struct page *page)
{
	struct page_ext *page_ext;
	int mode = sysctl_numa_balancing_extended_mode;
	mode = mode & (NUMA_BALANCING_EXCHANGE | NUMA_BALANCING_OPM);

	if (!mode)
		return;

	page_ext = lookup_page_ext(page);
	__del_page_from_deferred_list(page_ext, page);
}

/* page, lap_list should be locked from pgdat->lru_lock */
void del_page_from_lap_list(struct page *page)
{
	del_page_from_deferred_list(page);
}

void copy_page_info(struct page *oldpage, struct page *newpage)
{
	struct page_ext *old_ext, *new_ext;
	struct page_info *old_pi, *new_pi;
	int mode = sysctl_numa_balancing_extended_mode;
	mode = mode & (NUMA_BALANCING_CPM | NUMA_BALANCING_OPM);

	if (!mode)
		return;

	old_ext = lookup_page_ext(oldpage);
	new_ext = lookup_page_ext(newpage);
	if (unlikely(!old_ext || !new_ext))
		return;

	old_pi = get_page_info(old_ext);
	new_pi = get_page_info(new_ext);

	if (mode & NUMA_BALANCING_OPM) {
		new_pi->access_bitmap = old_pi->access_bitmap;

		print_access_history("migrate-old", oldpage, old_pi);
		print_access_history("migrate-new", newpage, new_pi);
	}

}

void exchange_page_info(struct page *from_page, struct page *to_page)
{
	struct page_ext *from_ext = lookup_page_ext(from_page);
	struct page_ext *to_ext = lookup_page_ext(to_page);
	struct page_info *from_pi, *to_pi;
	struct page_info tmp_pi;

	if (unlikely(!from_ext || !to_ext))
		return;
	from_pi = get_page_info(from_ext);
	to_pi = get_page_info(to_ext);

	tmp_pi.pfn = 0;
	tmp_pi.access_bitmap = 0;

	if (sysctl_numa_balancing_extended_mode & NUMA_BALANCING_OPM) {
		tmp_pi.access_bitmap = to_pi->access_bitmap;
		to_pi->access_bitmap = from_pi->access_bitmap;
		from_pi->access_bitmap = tmp_pi.access_bitmap;

		print_access_history("exchange-from", from_page, from_pi);
		print_access_history("exchange-  to", to_page, to_pi);
	}

}

int get_page_last_cpu(struct page *page)
{
	struct page_ext *page_ext = lookup_page_ext(page);
	struct page_info *pi;
	if (unlikely(!page_ext))
		return NUMA_NO_NODE;
	pi = get_page_info(page_ext);
	return pi->last_cpu;
}

void set_page_last_cpu(struct page *page, int cpu)
{
	struct page_ext *page_ext = lookup_page_ext(page);
	struct page_info *pi;
	if (unlikely(!page_ext))
		return;
	pi = get_page_info(page_ext);
	pi->last_cpu = cpu;
}

unsigned int PageBusyLock(struct page *page)
{
	struct page_ext *page_ext = lookup_page_ext(page);
	if (unlikely(!page_ext))
		return 0;
	return __PageBusyLock(page_ext);
}

void lock_busy(struct page *page)
{
	struct page_ext *page_ext = lookup_page_ext(page);
	if (unlikely(!page_ext))
		return;
	__lock_busy(page_ext);
}

unsigned int trylock_busy(struct page *page)
{
	struct page_ext *page_ext = lookup_page_ext(page);
	if (unlikely(!page_ext))
		return 0;
	return __trylock_busy(page_ext);
}

void unlock_busy(struct page *page)
{
	struct page_ext *page_ext = lookup_page_ext(page);
	if (unlikely(!page_ext))
		return;
	__unlock_busy(page_ext);
}

void add_page_for_tracking(struct page *page, unsigned int prev_lv)
{
	struct page_ext *page_ext;
	struct page_info *pi;
	struct pglist_data *pgdat;
	unsigned int lv;
	int recent;
	int thp_enabled = transparent_hugepage_flags & (1 << TRANSPARENT_HUGEPAGE_FLAG);
	int mode = sysctl_numa_balancing_extended_mode;
	mode = mode & NUMA_BALANCING_OPM;

	if (!mode)
		return;

	if (skip_lower_tier) {
		/*
		 * The lowest tier memory node does not need to mark cold page.
		 * So, we skip followed process that add page to cold page list.
		 */
		if (is_bottom_node(page_to_nid(page)))
			return;
	}

	/* Skip tail pages */
	if (PageTail(page))
		return;

	/* If THP is enabled, only allow tracking THP pages */
	if (thp_enabled && !PageTransHuge(page))
		return;

	if (page_count(page) > 1)
		return;

	page_ext = lookup_page_ext(page);
	if (unlikely(!page_ext))
		return;

	pgdat = page_pgdat(page);

	spin_lock_irq(&pgdat->lru_lock);

	pi = get_page_info(page_ext);
	lv = __get_page_access_lv(pi);

	if(__PageBusyLock(page_ext)
		|| __PageDeferred(page_ext)
		|| !PageLRU(page))
	{
		spin_unlock_irq(&pgdat->lru_lock);
		return;
	}

	VM_BUG_ON_PAGE(!PageLRU(page), page);
	VM_BUG_ON_PAGE(__PageBusyLock(page_ext), page);
	VM_BUG_ON_PAGE(__PageDeferred(page_ext), page);

	set_page_to_page_info(page, pi);

	/* Other lv page move to lap_list with changed lv */
	if (__PageTracked(page_ext)) {
		if (lv != prev_lv) {
			if (--(pgdat->lap_area[prev_lv].nr_free) < 0)
				pgdat->lap_area[prev_lv].nr_free = 0;
			(pgdat->lap_area[lv].nr_free)++;
		}

		recent = pi->access_bitmap & 0x1;
		// Recently accessd
		if (recent == 1) {
			list_move_tail(&pi->list, &pgdat->lap_area[lv].lap_list);
			print_access_history("    accessed", page, pi);
		} else { // Recently not accessed
			list_move(&pi->list, &pgdat->lap_area[lv].lap_list);
			print_access_history("not_accessed", page, pi);
		}

	/* Add page to lap_list */
	} else {
		__SetPageTracked(page_ext);

		recent = pi->access_bitmap & 0x1;
		// Recently accessd
		if (recent == 1) {
			list_add_tail(&pi->list, &pgdat->lap_area[lv].lap_list);
			print_access_history("    accessed", page, pi);
		} else { // Recently not accessed
			list_add(&pi->list, &pgdat->lap_area[lv].lap_list);
			print_access_history("not_accessed", page, pi);
		}

		__mod_lruvec_page_state(page, NR_TRACKED, hpage_nr_pages(page));
		(pgdat->lap_area[lv].nr_free)++;
	}

	spin_unlock_irq(&pgdat->lru_lock);
}

void add_page_for_exchange(struct page *page, int node)
{
	struct page_ext *page_ext;
	struct page_info *pi;
	struct pglist_data *pgdat;

	if (!(sysctl_numa_balancing_extended_mode & NUMA_BALANCING_EXCHANGE))
		return;

	page_ext = lookup_page_ext(page);
	if (unlikely(!page_ext))
		return;

	pgdat = page_pgdat(page);

	spin_lock_irq(&pgdat->lru_lock);

	//FIXME: defererd page move to head of list
	if (__PageDeferred(page_ext)
		|| __PageBusyLock(page_ext)
		|| !PageLRU(page)) {
		spin_unlock_irq(&pgdat->lru_lock);
		return;
	}

	VM_BUG_ON_PAGE(!PageLRU(page), page);
	VM_BUG_ON_PAGE(__PageDeferred(page_ext), page);
	VM_BUG_ON_PAGE(__PageBusyLock(page_ext), page);

	pi = get_page_info(page_ext);
	set_page_to_page_info(page, pi);

	if (__PageTracked(page_ext)) {
		__ClearPageTracked(page_ext);
		__mod_lruvec_page_state(page, NR_TRACKED, -hpage_nr_pages(page));
		__SetPageDeferred(page_ext);
		list_move(&pi->list, &pgdat->deferred_list);
	} else {
		__SetPageDeferred(page_ext);
		list_add(&pi->list, &pgdat->deferred_list);
	}

	spin_unlock_irq(&pgdat->lru_lock);

	VM_BUG_ON_PAGE(__PageTracked(page_ext), page);

	mod_lruvec_page_state(page, NR_DEFERRED, hpage_nr_pages(page));
}

unsigned int mod_page_access_lv(struct page *page, unsigned int accessed)
{
	struct page_ext *page_ext = lookup_page_ext(page);
	struct page_info *pi = get_page_info(page_ext);
	unsigned int prev_lv = __get_page_access_lv(pi);

	// Shfit Left, Recently accessed bit is LSB
	pi->access_bitmap = ((pi->access_bitmap) << 1);
	if (accessed)
		pi->access_bitmap |= 0x1;
	else
		pi->access_bitmap &= 0xfe;
	return prev_lv;
}

unsigned int get_page_access_lv(struct page *page)
{
	struct page_ext *page_ext;
	struct page_info *pi;

	if (!(sysctl_numa_balancing_extended_mode & NUMA_BALANCING_OPM))
		return -1;
	page_ext = lookup_page_ext(page);
	pi = get_page_info(page_ext);

	return __get_page_access_lv(pi);
}

void reset_page_access_lv(struct page *page)
{
	struct page_ext *page_ext;
	struct page_info *pi;

	if (!(sysctl_numa_balancing_extended_mode & NUMA_BALANCING_OPM))
		return;
	page_ext = lookup_page_ext(page);
	pi = get_page_info(page_ext);

	pi->access_bitmap = (u8) ~(1 << ACCESS_HISTORY_SIZE);
}

/* Traverse migratable nodes from start_nid to all same-tier memory nodes */
static int traverse_migratable_nodes(int start_nid, int order, int hold)
{
	int temp_nid = next_migration_node(start_nid);
	int dst_nid = NUMA_NO_NODE;

	/* Hold start_nid on temp_nid */
	if (hold)
		temp_nid = start_nid;

	if (start_nid == NUMA_NO_NODE || temp_nid == NUMA_NO_NODE)
		return dst_nid;

	do {
		if (migrate_balanced_pgdat(NODE_DATA(temp_nid), order)) {
			dst_nid = temp_nid;
			break;
		}
		temp_nid = next_migration_node(temp_nid);

		/* migration path fail */
		if (temp_nid == NUMA_NO_NODE) {
			dst_nid = NUMA_NO_NODE;
			break;
		}
	} while (temp_nid != start_nid);

	return dst_nid;
}

int find_best_demotion_node(struct page *page)
{
	int order = compound_order(page);
	int page_nid = page_to_nid(page);

	int last_cpu = get_page_last_cpu(page);
	int last_nid;

	int dst_nid = NUMA_NO_NODE;
	int sub_nid;

	if (last_cpu < 0)
		last_nid = page_nid;
	else
		last_nid = cpu_to_node(last_cpu);

	if (!is_top_node(page_nid) || !is_top_node(last_nid))
		return NUMA_NO_NODE;

	sub_nid = next_demotion_node(last_nid);
	dst_nid = traverse_migratable_nodes(sub_nid, order, 1);

	return dst_nid;
}
EXPORT_SYMBOL(find_best_demotion_node);

/* Find best node for migration */
int find_best_migration_node(struct page *page, int target_nid)
{
	int order = compound_order(page);
	int page_nid = page_to_nid(page);
	int first_nid = next_promotion_node(page_nid);

	int dst_nid;

	dst_nid = traverse_migratable_nodes(target_nid, order, 1);
#if 0 /* Verbose version */
	/* Migration between same-tier memory nodes */
	if (is_top_node(page_nid)) {
	} else { /* Promotion */
		if (first_nid == target_nid) {
		} /* Remote Promotion */
		else {
			/* Find migratable lower-tier node*/
			if (dst_nid == NUMA_NO_NODE)
				dst_nid = traverse_migratable_nodes(page_nid, order, 0);
		}
	}
#else /* Simple version */
	/* Find migratable lower-tier node*/
	if (dst_nid == NUMA_NO_NODE // Fail first try
			&& !is_top_node(page_nid) // DCPMM page
			&& first_nid != target_nid) // Remote Promotion
		dst_nid = traverse_migratable_nodes(page_nid, order, 0);
#endif

	return dst_nid;
}
EXPORT_SYMBOL(find_best_migration_node);

static void init_pages_in_zone(pg_data_t *pgdat, struct zone *zone)
{
	unsigned long pfn = zone->zone_start_pfn;
	unsigned long end_pfn = zone_end_pfn(zone);
	unsigned long count = 0;

	/*
	 * Walk the zone in pageblock_nr_pages steps. If a page block spans
	 * a zone boundary, it will be double counted between zones. This does
	 * not matter as the mixed block count will still be correct
	 */
	for (; pfn < end_pfn; ) {
		unsigned long block_end_pfn;

		if (!pfn_valid(pfn)) {
			pfn = ALIGN(pfn + 1, MAX_ORDER_NR_PAGES);
			continue;
		}

		block_end_pfn = ALIGN(pfn + 1, pageblock_nr_pages);
		block_end_pfn = min(block_end_pfn, end_pfn);

		for (; pfn < block_end_pfn; pfn++) {
			struct page *page;
			struct page_ext *page_ext;
			struct page_info *pi;

			if (!pfn_valid_within(pfn))
				continue;

			page = pfn_to_page(pfn);

			if (page_zone(page) != zone)
				continue;

			/*
			 * To avoid having to grab zone->lock, be a little
			 * careful when reading buddy page order. The only
			 * danger is that we skip too much and potentially miss
			 * some early allocated pages, which is better than
			 * heavy lock contention.
			 */
			if (PageBuddy(page)) {
				unsigned long order = page_order_unsafe(page);

				if (order > 0 && order < MAX_ORDER)
					pfn += (1UL << order) - 1;
				continue;
			}

			if (PageReserved(page))
				continue;

			page_ext = lookup_page_ext(page);
			if (unlikely(!page_ext))
				continue;

			/* Maybe overlapping zone */
			if (test_bit(PAGE_EXT_BALALCING, &page_ext->flags))
				continue;

			pi = get_page_info(page_ext);

			/* Found early allocated page */
			__set_bit(PAGE_EXT_BALALCING, &page_ext->flags);
			__ClearPageTracked(page_ext);
			__ClearPageDeferred(page_ext);
			__ClearPageDemoted(page_ext);
			pi->pfn = 0;
			pi->last_cpu = -1;
			pi->access_bitmap = (u8) ~(1 << ACCESS_HISTORY_SIZE);

			count++;
		}
		cond_resched();
	}

	printk("Node %d, zone %8s: page info found early allocated %lu pages\n",
		pgdat->node_id, zone->name, count);
}

static void init_zones_in_node(pg_data_t *pgdat)
{
	struct zone *zone;
	struct zone *node_zones = pgdat->node_zones;

	for (zone = node_zones; zone - node_zones < MAX_NR_ZONES; ++zone) {
		if (!populated_zone(zone))
			continue;

		init_pages_in_zone(pgdat, zone);
	}
}

static void init_early_allocated_pages(void)
{
	pg_data_t *pgdat;

	for_each_online_pgdat(pgdat)
		init_zones_in_node(pgdat);
}

static void init_page_balancing(void)
{
	init_early_allocated_pages();
}

struct page_ext_operations page_info_ops = {
	.size = sizeof(struct page_info),
	.need = need_page_balancing,
	.init = init_page_balancing,
};

#ifdef CONFIG_SYSFS
static ssize_t background_demotion_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	switch (background_demotion) {
	case 0:
		return sprintf(buf, "%u - Disabled.\n",
				background_demotion);
	case 1:
		return sprintf(buf, "%u - Enabled background page demotion\n",
				background_demotion);
	default:
		return sprintf(buf, "error\n");
	}
}

static ssize_t background_demotion_store(struct kobject *kobj,
		struct kobj_attribute *attr,
		const char *buf, size_t count)
{
	unsigned long enable;
	int err;

	err = kstrtoul(buf, 10, &enable);
	if (err || enable < 0 || enable > 1)
		return -EINVAL;

	background_demotion = enable;

	return count;
}

static struct kobj_attribute background_demotion_attr =
__ATTR(background_demotion, 0644, background_demotion_show,
		background_demotion_store);

static ssize_t nr_reserved_pages_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	int cpu;
	int size = 0;
	char buf_nr_free[10];
	const struct cpumask *cpumasks = cpu_online_mask;
	enum page_type type = BASEPAGE;

	/* maybe not accurate */
	for (type = BASEPAGE; type < NR_PAGE_TYPE; type++) {
		for_each_cpu(cpu, cpumasks) {
			size += sprintf(buf_nr_free, "%u ",
					promote_area[type][cpu].nr_free);
			strcat(buf, buf_nr_free);
		}
		strcat(buf, "\n");
		size++;
	}

	return size;
}

static struct kobj_attribute nr_reserved_pages_attr =
__ATTR(nr_reserved_pages, 0644, nr_reserved_pages_show, NULL);

static ssize_t batch_demotion_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	switch (batch_demotion) {
	case 0:
		return sprintf(buf, "%u - Disabled. batch size is 1\n",
				batch_demotion);
	case 1:
		return sprintf(buf, "%u - Enabled. batch size is defined by current free reserved pages\n",
				batch_demotion);
	default:
		return sprintf(buf, "error\n");
	}
}

static ssize_t batch_demotion_store(struct kobject *kobj,
		struct kobj_attribute *attr,
		const char *buf, size_t count)
{
	unsigned long enable;
	int err;

	err = kstrtoul(buf, 10, &enable);
	if (err || enable < 0 || enable > 1)
		return -EINVAL;

	batch_demotion = enable;

	return count;
}

static struct kobj_attribute batch_demotion_attr =
__ATTR(batch_demotion, 0644, batch_demotion_show,
		batch_demotion_store);

static ssize_t thp_mt_copy_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	switch (thp_mt_copy) {
	case 0:
		return sprintf(buf, "%u - Disabled. single-thread copy\n",
				thp_mt_copy);
	case 1:
		return sprintf(buf, "%u - Enabled. multi-thread(4) copys\n",
				thp_mt_copy);
	default:
		return sprintf(buf, "error\n");
	}
}

static ssize_t thp_mt_copy_store(struct kobject *kobj,
		struct kobj_attribute *attr,
		const char *buf, size_t count)
{
	unsigned long enable;
	int err;

	err = kstrtoul(buf, 10, &enable);
	if (err || enable < 0 || enable > 1)
		return -EINVAL;

	thp_mt_copy = enable;

	return count;
}

static struct kobj_attribute thp_mt_copy_attr =
__ATTR(thp_mt_copy, 0644, thp_mt_copy_show,
		thp_mt_copy_store);

static ssize_t skip_lower_tier_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	switch (skip_lower_tier) {
	case 0:
		return sprintf(buf, "%u - Disabled. tracking all pages\n",
				skip_lower_tier);
	case 1:
		return sprintf(buf, "%u - Enabled. skip tracking lower-tier pages\n",
				skip_lower_tier);
	default:
		return sprintf(buf, "error\n");
	}
}

static ssize_t skip_lower_tier_store(struct kobject *kobj,
		struct kobj_attribute *attr,
		const char *buf, size_t count)
{
	unsigned long enable;
	int err;

	err = kstrtoul(buf, 10, &enable);
	if (err || enable < 0 || enable > 1)
		return -EINVAL;

	skip_lower_tier = enable;

	return count;
}

static struct kobj_attribute skip_lower_tier_attr =
__ATTR(skip_lower_tier, 0644, skip_lower_tier_show,
		skip_lower_tier_store);

static struct attribute *page_balancing_attr[] = {
	&background_demotion_attr.attr,
	&batch_demotion_attr.attr,
	&thp_mt_copy_attr.attr,
	&skip_lower_tier_attr.attr,
	&nr_reserved_pages_attr.attr,
	NULL,
};

static struct attribute_group page_balancing_attr_group = {
	.attrs = page_balancing_attr,
};

static void __init page_balancing_exit_sysfs(struct kobject *page_balancing_kobj)
{
	sysfs_remove_group(page_balancing_kobj, &page_balancing_attr_group);
	kobject_put(page_balancing_kobj);
}

static int __init page_balancing_init_sysfs(struct kobject **page_balancing_kobj) {
	int err;

	*page_balancing_kobj = kobject_create_and_add("page_balancing", mm_kobj);
	if (unlikely(!*page_balancing_kobj)) {
		pr_err("failed to create page_balancing kobject\n");
		return -ENOMEM;
	}

	err = sysfs_create_group(*page_balancing_kobj, &page_balancing_attr_group);
	if (err) {
		pr_err("failed to register page_balancing group\n");
		goto delete_obj;
	}

	return 0;

delete_obj:
	page_balancing_exit_sysfs(*page_balancing_kobj);
	return err;
}

#else
static inline int page_balancing_init_sysfs(struct kobject **page_balancing_kobj)
{
	return 0;
}
static inline void page_balancing_exit_sysfs(struct kobject *page_balancing_kobj)
{
}
#endif

static int __init page_balancing_init(void)
{
	int err;
	struct kobject *sysfs_page_balancing_kobj;

	err = page_balancing_init_sysfs(&sysfs_page_balancing_kobj);
	if (err) {
		pr_err("failed start page_balancing_init becasue sysfs\n");
		goto err_sysfs;
	}
	return 0;

err_sysfs:
	return err;
}
subsys_initcall(page_balancing_init);
