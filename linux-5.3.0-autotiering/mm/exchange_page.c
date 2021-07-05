/*
 * This implements parallel page copy function through multi threaded
 * work queues.
 *
 * Zi Yan <ziy@nvidia.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
 */
#include <linux/highmem.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/freezer.h>

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

/*
 * nr_copythreads can be the highest number of threads for given node
 * on any architecture. The actual number of copy threads will be
 * limited by the cpumask weight of the target node.
 */
extern unsigned int limit_mt_num;

struct copy_page_info {
	struct work_struct copy_page_work;
	char *to;
	char *from;
	unsigned long chunk_size;
};

static void exchange_page_routine(char *to, char *from, unsigned long chunk_size)
{
	u64 tmp;
	int i = 0;
	int pieces = sizeof(tmp);

	while (i < chunk_size) {
		UNROLL512(SWAP_PAGE(from, to, tmp, i, pieces))
	}
}

static void exchange_page_work_queue_thread(struct work_struct *work)
{
	struct copy_page_info *my_work = (struct copy_page_info*)work;

	exchange_page_routine(my_work->to,
							  my_work->from,
							  my_work->chunk_size);
}

int exchange_page_mthread(struct page *to, struct page *from, int nr_pages)
{
	int total_mt_num = limit_mt_num;
#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	int to_node = page_to_nid(to);
#else
	int to_node = numa_node_id();
#endif
	int i;
	struct copy_page_info *work_items;
	char *vto, *vfrom;
	unsigned long chunk_size;
	const struct cpumask *per_node_cpumask = cpumask_of_node(to_node);
	int cpu_id_list[32] = {0};
	int cpu;

	total_mt_num = min_t(unsigned int, total_mt_num,
						 cpumask_weight(per_node_cpumask));

	if (total_mt_num > 1)
		total_mt_num = (total_mt_num / 2) * 2;

	if (total_mt_num > 32 || total_mt_num < 1)
		return -ENODEV;

	work_items = kvzalloc(sizeof(struct copy_page_info)*total_mt_num,
						 GFP_KERNEL);
	if (!work_items)
		return -ENOMEM;

	i = 0;
	for_each_cpu(cpu, per_node_cpumask) {
		if (i >= total_mt_num)
			break;
		cpu_id_list[i] = cpu;
		++i;
	}

	/* XXX: assume no highmem  */
	vfrom = kmap(from);
	vto = kmap(to);
	chunk_size = PAGE_SIZE*nr_pages / total_mt_num;

	for (i = 0; i < total_mt_num; ++i) {
		INIT_WORK((struct work_struct *)&work_items[i],
				exchange_page_work_queue_thread);

		work_items[i].to = vto + i * chunk_size;
		work_items[i].from = vfrom + i * chunk_size;
		work_items[i].chunk_size = chunk_size;

		queue_work_on(cpu_id_list[i],
					  system_highpri_wq,
					  (struct work_struct *)&work_items[i]);
	}

	/* Wait until it finishes  */
	flush_workqueue(system_highpri_wq);

	kunmap(to);
	kunmap(from);

	kvfree(work_items);

	return 0;
}

int exchange_page_lists_mthread(struct page **to, struct page **from, int nr_pages) 
{
	int err = 0;
	int total_mt_num = limit_mt_num;
#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	int to_node = page_to_nid(*to);
#else
	int to_node = numa_node_id();
#endif
	int i;
	struct copy_page_info *work_items;
	const struct cpumask *per_node_cpumask = cpumask_of_node(to_node);
	int cpu_id_list[32] = {0};
	int cpu;
	int item_idx;


	total_mt_num = min_t(unsigned int, total_mt_num,
						 cpumask_weight(per_node_cpumask));

	if (total_mt_num > 32 || total_mt_num < 1)
		return -ENODEV;

	if (nr_pages < total_mt_num) {
		int residual_nr_pages = nr_pages - rounddown_pow_of_two(nr_pages);

		if (residual_nr_pages) {
			for (i = 0; i < residual_nr_pages; ++i) {
				BUG_ON(hpage_nr_pages(to[i]) != hpage_nr_pages(from[i]));
				err = exchange_page_mthread(to[i], from[i], hpage_nr_pages(to[i]));
				VM_BUG_ON(err);
			}
			nr_pages = rounddown_pow_of_two(nr_pages);
			to = &to[residual_nr_pages];
			from = &from[residual_nr_pages];
		}

		work_items = kvzalloc(sizeof(struct copy_page_info)*total_mt_num,
							 GFP_KERNEL);
	} else
		work_items = kvzalloc(sizeof(struct copy_page_info)*nr_pages,
							 GFP_KERNEL);
	if (!work_items)
		return -ENOMEM;

	i = 0;
	for_each_cpu(cpu, per_node_cpumask) {
		if (i >= total_mt_num)
			break;
		cpu_id_list[i] = cpu;
		++i;
	}

	if (nr_pages < total_mt_num) {
		for (cpu = 0; cpu < total_mt_num; ++cpu)
			INIT_WORK((struct work_struct *)&work_items[cpu],
					  exchange_page_work_queue_thread);
		cpu = 0;
		for (item_idx = 0; item_idx < nr_pages; ++item_idx) {
			unsigned long chunk_size = nr_pages * PAGE_SIZE * hpage_nr_pages(from[item_idx]) / total_mt_num;
			char *vfrom = kmap(from[item_idx]);
			char *vto = kmap(to[item_idx]);
			VM_BUG_ON(PAGE_SIZE * hpage_nr_pages(from[item_idx]) % total_mt_num);
			VM_BUG_ON(total_mt_num % nr_pages);
			BUG_ON(hpage_nr_pages(to[item_idx]) !=
				   hpage_nr_pages(from[item_idx]));

			for (i = 0; i < (total_mt_num/nr_pages); ++cpu, ++i) {
				work_items[cpu].to = vto + chunk_size * i;
				work_items[cpu].from = vfrom + chunk_size * i;
				work_items[cpu].chunk_size = chunk_size;
			}
		}
		if (cpu != total_mt_num)
			pr_err("%s: only %d out of %d pages are transferred\n", __func__,
				cpu - 1, total_mt_num);

		for (cpu = 0; cpu < total_mt_num; ++cpu)
			queue_work_on(cpu_id_list[cpu],
						  system_highpri_wq,
						  (struct work_struct *)&work_items[cpu]);
	} else {
		for (i = 0; i < nr_pages; ++i) {
			int thread_idx = i % total_mt_num;

			INIT_WORK((struct work_struct *)&work_items[i], exchange_page_work_queue_thread);

			/* XXX: assume no highmem  */
			work_items[i].to = kmap(to[i]);
			work_items[i].from = kmap(from[i]);
			work_items[i].chunk_size = PAGE_SIZE * hpage_nr_pages(from[i]);

			BUG_ON(hpage_nr_pages(to[i]) != hpage_nr_pages(from[i]));

			queue_work_on(cpu_id_list[thread_idx], system_highpri_wq, (struct work_struct *)&work_items[i]);
		}
	}

	/* Wait until it finishes  */
	flush_workqueue(system_highpri_wq);

	for (i = 0; i < nr_pages; ++i) {
			kunmap(to[i]);
			kunmap(from[i]);
	}

	kvfree(work_items);

	return err;
}

