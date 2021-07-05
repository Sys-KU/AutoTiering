/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_PAGE_BALANCING_H
#define __LINUX_PAGE_BALANCING_H
#include <linux/sched/sysctl.h>

#define ACCESS_HISTORY_SIZE 8
#define MAX_ACCESS_LEVEL ACCESS_HISTORY_SIZE
#define MEDIAN_ACCESS_LEVEL (ACCESS_HISTORY_SIZE >> 1)

#ifdef CONFIG_PAGE_BALANCING
struct page_info {
	struct list_head list;
	unsigned long pfn;
	int8_t last_cpu; // for free_promote area
	u8 access_bitmap;
};
#endif

extern struct page_ext_operations page_info_ops;

#ifdef CONFIG_PAGE_BALANCING
extern struct page *get_page_from_page_info(struct page_info *page_info);
extern struct page_info *get_page_info_from_page(struct page *page);
extern struct page_ext *get_page_ext(struct page_info *page_info);

extern void set_page_to_page_info(struct page *page, struct page_info *page_info);
extern void clear_page_info(struct page *page);
extern void del_page_from_deferred_list(struct page *page);
extern void del_page_from_lap_list(struct page *page);
extern void copy_page_info(struct page *oldpage, struct page *newpage);
extern void exchange_page_info(struct page *from_page, struct page *to_page);
extern void SetPageDeferred(struct page *page);
extern void ClearPageDeferred(struct page *page);
extern unsigned int PageDeferred(struct page *page);
extern void SetPageDemoted(struct page *page);
extern void ClearPageDemoted(struct page *page);
extern unsigned int PageDemoted(struct page *page);
extern void lock_busy(struct page *page);
extern unsigned int trylock_busy(struct page *page);
extern void unlock_busy(struct page *page);
extern void add_page_for_exchange(struct page *page, int node);
extern void add_page_for_tracking(struct page *page, unsigned int prev_lv);
extern unsigned int mod_page_access_lv(struct page *page, unsigned int accessed);
extern unsigned int get_page_access_lv(struct page *page);
extern void reset_page_access_lv(struct page *page);

extern int get_page_last_cpu(struct page *page);
extern void set_page_last_cpu(struct page *page, int cpu);

/* Finding best nodes */
extern int find_best_demotion_node(struct page *page);
extern int find_best_migration_node(struct page *page, int target_nid);

/* User-space parameters */
extern unsigned int background_demotion;
extern unsigned int batch_demotion;
extern unsigned int thp_mt_copy;

#ifdef CONFIG_PAGE_BALANCING_DEBUG
extern void trace_dump_page(struct page *page, const char *msg);
#else /* CONFIG_PAGE_BALANCING_DEBUG */
static inline void trace_dump_page(struct page *page, const char *msg) {
}
#endif /* CONFIG_PAGE_BALANCING_DEBUG */

#else /* CONFIG_PAGE_BALANCING */
static inline void *get_page_from_page_info(struct page_info *page_info)
{
	return NULL;
}
static inline void *get_page_ext(struct page_info *page_info)
{
	return NULL;
}
static inline void set_page_to_page_info(struct page *page,
				struct page_info *page_info) {
}
static inline void clear_page_info(struct page *page) {
}
static inline void del_page_from_deferred_list(struct page *page) {
}
static inline void del_page_from_lap_list(struct page *page)
}
static inline void copy_page_info(struct page *oldpage, struct page *newpage) {
}
static inline void exchange_page_info(struct page *from_page, struct page *to_page) {
}
static inline void SetPageDeferred(struct page *page) {
}
static inline void ClearPageDeferred(struct page *page) {
}
static inline unsigned int PageDeferred(struct page *page) {
	return 0;
}
static inline void SetPageDemoted(struct page *page) {
}
static inline void ClearPageDemoted(struct page *page) {
}
static inline unsigned int PageDemoted(struct page *page) {
	return 0;
}
static inline void lock_busy(struct page *page) {
}
static inline unsigned int trylock_busy(struct page *page) {
	return 0;
}
static inline void unlock_busy(struct page *page) {
}
static inline void add_page_for_exchange(struct page *page, int node) {
}
static inline void add_page_for_tracking(struct page *page, unsigned int prev_lv) {
}
static inline unsigned int mod_page_access_lv(struct page *page, unsigned int accessed) {
	/* always hot */
	return MAX_ACCESS_LEVEL;
}
static inline unsigned int get_page_access_lv(struct page *page) {
	/* always hot */
	return MAX_ACCESS_LEVEL;
}
static inline void reset_page_access_lv(struct page *page) {
}

static inline int find_best_demotion_node(struct page *page) {
	return NUMA_NO_NODE;
}
static inline int  find_best_migration_node(struct page *page, int target_nid) {
	return NUMA_NO_NODE;
}

#endif /* CONFIG_PAGE_BALANCING */
#endif /* __LINUX_PAGE_BALANCING_H */
