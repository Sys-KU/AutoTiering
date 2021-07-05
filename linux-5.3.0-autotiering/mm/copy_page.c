/*
 * Parallel page copy routine.
 * Use DMA engine to copy page data
 *
 * Zi Yan <zi.yan@cs.rutgers.edu>
 *
 */

#include <linux/sysctl.h>
#include <linux/highmem.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/freezer.h>


unsigned int limit_mt_num = 4;

/* ======================== multi-threaded copy page ======================== */

struct copy_item {
	char *to;
	char *from;
	unsigned long chunk_size;
};

struct copy_page_info {
	struct work_struct copy_page_work;
	unsigned long num_items;
	struct copy_item item_list[0];
};

static void copy_page_routine(char *vto, char *vfrom,
	unsigned long chunk_size)
{
	memcpy(vto, vfrom, chunk_size);
}

static void copy_page_work_queue_thread(struct work_struct *work)
{
	struct copy_page_info *my_work = (struct copy_page_info *)work;
	int i;

	for (i = 0; i < my_work->num_items; ++i)
		copy_page_routine(my_work->item_list[i].to,
						  my_work->item_list[i].from,
						  my_work->item_list[i].chunk_size);
}

int copy_page_multithread(struct page *to, struct page *from, int nr_pages)
{
	unsigned int total_mt_num = limit_mt_num;
#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	int to_node = page_to_nid(to);
#else
	int to_node = numa_node_id();
#endif
	int i;
	struct copy_page_info *work_items[32] = {0};
	char *vto, *vfrom;
	unsigned long chunk_size;
	const struct cpumask *per_node_cpumask = cpumask_of_node(to_node);
	int cpu_id_list[32] = {0};
	int cpu;
	int err = 0;

	total_mt_num = min_t(unsigned int, total_mt_num,
						 cpumask_weight(per_node_cpumask));
	if (total_mt_num > 1)
		total_mt_num = (total_mt_num / 2) * 2;

	if (total_mt_num > 32 || total_mt_num < 1)
		return -ENODEV;

	for (cpu = 0; cpu < total_mt_num; ++cpu) {
		work_items[cpu] = kzalloc(sizeof(struct copy_page_info)
						+ sizeof(struct copy_item), GFP_KERNEL);
		if (!work_items[cpu]) {
			err = -ENOMEM;
			goto free_work_items;
		}
	}

	i = 0;
	for_each_cpu(cpu, per_node_cpumask) {
		if (i >= total_mt_num)
			break;
		cpu_id_list[i] = cpu;
		++i;
	}

	vfrom = kmap(from);
	vto = kmap(to);
	chunk_size = PAGE_SIZE*nr_pages / total_mt_num;

	for (i = 0; i < total_mt_num; ++i) {
		INIT_WORK((struct work_struct *)work_items[i],
				  copy_page_work_queue_thread);

		work_items[i]->num_items = 1;
		work_items[i]->item_list[0].to = vto + i * chunk_size;
		work_items[i]->item_list[0].from = vfrom + i * chunk_size;
		work_items[i]->item_list[0].chunk_size = chunk_size;

		queue_work_on(cpu_id_list[i],
					  system_highpri_wq,
					  (struct work_struct *)work_items[i]);
	}

	/* Wait until it finishes  */
	for (i = 0; i < total_mt_num; ++i)
		flush_work((struct work_struct *)work_items[i]);

	kunmap(to);
	kunmap(from);

free_work_items:
	for (cpu = 0; cpu < total_mt_num; ++cpu)
		if (work_items[cpu])
			kfree(work_items[cpu]);

	return err;
}

int copy_page_lists_mt(struct page **to, struct page **from, int nr_items)
{
	int err = 0;
	unsigned int total_mt_num = limit_mt_num;
#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	int to_node = page_to_nid(*to);
#else
	int to_node = numa_node_id();
#endif
	int i;
	struct copy_page_info *work_items[32] = {0};
	const struct cpumask *per_node_cpumask = cpumask_of_node(to_node);
	int cpu_id_list[32] = {0};
	int cpu;
	int max_items_per_thread;
	int item_idx;

	total_mt_num = min_t(unsigned int, total_mt_num,
						 cpumask_weight(per_node_cpumask));


	if (total_mt_num > 32)
		return -ENODEV;

	/* Each threads get part of each page, if nr_items < totla_mt_num */
	if (nr_items < total_mt_num)
		max_items_per_thread = nr_items;
	else
		max_items_per_thread = (nr_items / total_mt_num) +
				((nr_items % total_mt_num)?1:0);


	for (cpu = 0; cpu < total_mt_num; ++cpu) {
		work_items[cpu] = kzalloc(sizeof(struct copy_page_info) +
					sizeof(struct copy_item)*max_items_per_thread, GFP_KERNEL);
		if (!work_items[cpu]) {
			err = -ENOMEM;
			goto free_work_items;
		}
	}

	i = 0;
	for_each_cpu(cpu, per_node_cpumask) {
		if (i >= total_mt_num)
			break;
		cpu_id_list[i] = cpu;
		++i;
	}

	if (nr_items < total_mt_num) {
		for (cpu = 0; cpu < total_mt_num; ++cpu) {
			INIT_WORK((struct work_struct *)work_items[cpu],
					  copy_page_work_queue_thread);
			work_items[cpu]->num_items = max_items_per_thread;
		}

		for (item_idx = 0; item_idx < nr_items; ++item_idx) {
			unsigned long chunk_size = PAGE_SIZE * hpage_nr_pages(from[item_idx]) / total_mt_num;
			char *vfrom = kmap(from[item_idx]);
			char *vto = kmap(to[item_idx]);
			VM_BUG_ON(PAGE_SIZE * hpage_nr_pages(from[item_idx]) % total_mt_num);
			BUG_ON(hpage_nr_pages(to[item_idx]) !=
				   hpage_nr_pages(from[item_idx]));

			for (cpu = 0; cpu < total_mt_num; ++cpu) {
				work_items[cpu]->item_list[item_idx].to = vto + chunk_size * cpu;
				work_items[cpu]->item_list[item_idx].from = vfrom + chunk_size * cpu;
				work_items[cpu]->item_list[item_idx].chunk_size =
					chunk_size;
			}
		}

		for (cpu = 0; cpu < total_mt_num; ++cpu)
			queue_work_on(cpu_id_list[cpu],
						  system_highpri_wq,
						  (struct work_struct *)work_items[cpu]);
	} else {
		item_idx = 0;
		for (cpu = 0; cpu < total_mt_num; ++cpu) {
			int num_xfer_per_thread = nr_items / total_mt_num;
			int per_cpu_item_idx;

			if (cpu < (nr_items % total_mt_num))
				num_xfer_per_thread += 1;

			INIT_WORK((struct work_struct *)work_items[cpu],
					  copy_page_work_queue_thread);

			work_items[cpu]->num_items = num_xfer_per_thread;
			for (per_cpu_item_idx = 0; per_cpu_item_idx < work_items[cpu]->num_items;
				 ++per_cpu_item_idx, ++item_idx) {
				work_items[cpu]->item_list[per_cpu_item_idx].to = kmap(to[item_idx]);
				work_items[cpu]->item_list[per_cpu_item_idx].from =
					kmap(from[item_idx]);
				work_items[cpu]->item_list[per_cpu_item_idx].chunk_size =
					PAGE_SIZE * hpage_nr_pages(from[item_idx]);

				BUG_ON(hpage_nr_pages(to[item_idx]) !=
					   hpage_nr_pages(from[item_idx]));
			}

			queue_work_on(cpu_id_list[cpu],
						  system_highpri_wq,
						  (struct work_struct *)work_items[cpu]);
		}
		if (item_idx != nr_items)
			pr_err("%s: only %d out of %d pages are transferred\n", __func__,
				item_idx - 1, nr_items);
	}

	/* Wait until it finishes  */
	for (i = 0; i < total_mt_num; ++i)
		flush_work((struct work_struct *)work_items[i]);

	for (i = 0; i < nr_items; ++i) {
			kunmap(to[i]);
			kunmap(from[i]);
	}

free_work_items:
	for (cpu = 0; cpu < total_mt_num; ++cpu)
		if (work_items[cpu])
			kfree(work_items[cpu]);

	return err;
}
