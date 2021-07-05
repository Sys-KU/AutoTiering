/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_EXCHANGE_H
#define _LINUX_EXCHANGE_H

#include <linux/migrate.h>
#include <linux/list.h>

struct exchange_page_info {
	struct page *from_page;
	struct page *to_page;

	struct anon_vma *from_anon_vma;
	struct anon_vma *to_anon_vma;

	int from_page_was_mapped;
	int to_page_was_mapped;

	pgoff_t from_index, to_index;

	struct list_head list;
};

int exchange_two_pages(struct page *, struct page *, enum migrate_mode mode);
int try_exchange_page(struct page *page, int dst_nid);
int exchange_pages(struct list_head *, enum migrate_mode mode);
int exchange_pages_concur(struct list_head *exchange_list,
		enum migrate_mode mode);
int exchange_pages_between_nodes_batch(const int from_nid, const int to_nid);
int exchange_page_lists_mthread(struct page **to, struct page **from, int nr_pages);
int exchange_page_mthread(struct page *to, struct page *from, int nr_pages);
void wakeup_kexchanged(int node);

#endif /* _LINUX_EXCHANGE_H */
