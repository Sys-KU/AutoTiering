// SPDX-License-Identifier: GPL-2.0-only
/* net/core/xdp.c
 *
 * Copyright (c) 2017 Jesper Dangaard Brouer, Red Hat Inc.
 */
#include <linux/bpf.h>
#include <linux/filter.h>
#include <linux/types.h>
#include <linux/mm.h>
#include <linux/netdevice.h>
#include <linux/slab.h>
#include <linux/idr.h>
#include <linux/rhashtable.h>
#include <net/page_pool.h>

#include <net/xdp.h>
#include <net/xdp_priv.h> /* struct xdp_mem_allocator */
#include <trace/events/xdp.h>

#define REG_STATE_NEW		0x0
#define REG_STATE_REGISTERED	0x1
#define REG_STATE_UNREGISTERED	0x2
#define REG_STATE_UNUSED	0x3

static DEFINE_IDA(mem_id_pool);
static DEFINE_MUTEX(mem_id_lock);
#define MEM_ID_MAX 0xFFFE
#define MEM_ID_MIN 1
static int mem_id_next = MEM_ID_MIN;

static bool mem_id_init; /* false */
static struct rhashtable *mem_id_ht;

static u32 xdp_mem_id_hashfn(const void *data, u32 len, u32 seed)
{
	const u32 *k = data;
	const u32 key = *k;

	BUILD_BUG_ON(FIELD_SIZEOF(struct xdp_mem_allocator, mem.id)
		     != sizeof(u32));

	/* Use cyclic increasing ID as direct hash key */
	return key;
}

static int xdp_mem_id_cmp(struct rhashtable_compare_arg *arg,
			  const void *ptr)
{
	const struct xdp_mem_allocator *xa = ptr;
	u32 mem_id = *(u32 *)arg->key;

	return xa->mem.id != mem_id;
}

static const struct rhashtable_params mem_id_rht_params = {
	.nelem_hint = 64,
	.head_offset = offsetof(struct xdp_mem_allocator, node),
	.key_offset  = offsetof(struct xdp_mem_allocator, mem.id),
	.key_len = FIELD_SIZEOF(struct xdp_mem_allocator, mem.id),
	.max_size = MEM_ID_MAX,
	.min_size = 8,
	.automatic_shrinking = true,
	.hashfn    = xdp_mem_id_hashfn,
	.obj_cmpfn = xdp_mem_id_cmp,
};

static void __xdp_mem_allocator_rcu_free(struct rcu_head *rcu)
{
	struct xdp_mem_allocator *xa;

	xa = container_of(rcu, struct xdp_mem_allocator, rcu);

	/* Allocator have indicated safe to remove before this is called */
	if (xa->mem.type == MEM_TYPE_PAGE_POOL)
		page_pool_free(xa->page_pool);

	/* Allow this ID to be reused */
	ida_simple_remove(&mem_id_pool, xa->mem.id);

	/* Poison memory */
	xa->mem.id = 0xFFFF;
	xa->mem.type = 0xF0F0;
	xa->allocator = (void *)0xDEAD9001;

	kfree(xa);
}

static bool __mem_id_disconnect(int id, bool force)
{
	struct xdp_mem_allocator *xa;
	bool safe_to_remove = true;

	mutex_lock(&mem_id_lock);

	xa = rhashtable_lookup_fast(mem_id_ht, &id, mem_id_rht_params);
	if (!xa) {
		mutex_unlock(&mem_id_lock);
		WARN(1, "Request remove non-existing id(%d), driver bug?", id);
		return true;
	}
	xa->disconnect_cnt++;

	/* Detects in-flight packet-pages for page_pool */
	if (xa->mem.type == MEM_TYPE_PAGE_POOL)
		safe_to_remove = page_pool_request_shutdown(xa->page_pool);

	trace_mem_disconnect(xa, safe_to_remove, force);

	if ((safe_to_remove || force) &&
	    !rhashtable_remove_fast(mem_id_ht, &xa->node, mem_id_rht_params))
		call_rcu(&xa->rcu, __xdp_mem_allocator_rcu_free);

	mutex_unlock(&mem_id_lock);
	return (safe_to_remove|force);
}

#define DEFER_TIME (msecs_to_jiffies(1000))
#define DEFER_WARN_INTERVAL (30 * HZ)
#define DEFER_MAX_RETRIES 120

static void mem_id_disconnect_defer_retry(struct work_struct *wq)
{
	struct delayed_work *dwq = to_delayed_work(wq);
	struct xdp_mem_allocator *xa = container_of(dwq, typeof(*xa), defer_wq);
	bool force = false;

	if (xa->disconnect_cnt > DEFER_MAX_RETRIES)
		force = true;

	if (__mem_id_disconnect(xa->mem.id, force))
		return;

	/* Periodic warning */
	if (time_after_eq(jiffies, xa->defer_warn)) {
		int sec = (s32)((u32)jiffies - (u32)xa->defer_start) / HZ;

		pr_warn("%s() stalled mem.id=%u shutdown %d attempts %d sec\n",
			__func__, xa->mem.id, xa->disconnect_cnt, sec);
		xa->defer_warn = jiffies + DEFER_WARN_INTERVAL;
	}

	/* Still not ready to be disconnected, retry later */
	schedule_delayed_work(&xa->defer_wq, DEFER_TIME);
}

void xdp_rxq_info_unreg_mem_model(struct xdp_rxq_info *xdp_rxq)
{
	struct xdp_mem_allocator *xa;
	int id = xdp_rxq->mem.id;

	if (xdp_rxq->reg_state != REG_STATE_REGISTERED) {
		WARN(1, "Missing register, driver bug");
		return;
	}

	if (xdp_rxq->mem.type != MEM_TYPE_PAGE_POOL &&
	    xdp_rxq->mem.type != MEM_TYPE_ZERO_COPY) {
		return;
	}

	if (id == 0)
		return;

	if (__mem_id_disconnect(id, false))
		return;

	/* Could not disconnect, defer new disconnect attempt to later */
	mutex_lock(&mem_id_lock);

	xa = rhashtable_lookup_fast(mem_id_ht, &id, mem_id_rht_params);
	if (!xa) {
		mutex_unlock(&mem_id_lock);
		return;
	}
	xa->defer_start = jiffies;
	xa->defer_warn  = jiffies + DEFER_WARN_INTERVAL;

	INIT_DELAYED_WORK(&xa->defer_wq, mem_id_disconnect_defer_retry);
	mutex_unlock(&mem_id_lock);
	schedule_delayed_work(&xa->defer_wq, DEFER_TIME);
}
EXPORT_SYMBOL_GPL(xdp_rxq_info_unreg_mem_model);

/* This unregister operation will also cleanup and destroy the
 * allocator. The page_pool_free() operation is first called when it's
 * safe to remove, possibly deferred to a workqueue.
 */
void xdp_rxq_info_unreg(struct xdp_rxq_info *xdp_rxq)
{
	/* Simplify driver cleanup code paths, allow unreg "unused" */
	if (xdp_rxq->reg_state == REG_STATE_UNUSED)
		return;

	WARN(!(xdp_rxq->reg_state == REG_STATE_REGISTERED), "Driver BUG");

	xdp_rxq_info_unreg_mem_model(xdp_rxq);

	xdp_rxq->reg_state = REG_STATE_UNREGISTERED;
	xdp_rxq->dev = NULL;

	/* Reset mem info to defaults */
	xdp_rxq->mem.id = 0;
	xdp_rxq->mem.type = 0;
}
EXPORT_SYMBOL_GPL(xdp_rxq_info_unreg);

static void xdp_rxq_info_init(struct xdp_rxq_info *xdp_rxq)
{
	memset(xdp_rxq, 0, sizeof(*xdp_rxq));
}

/* Returns 0 on success, negative on failure */
int xdp_rxq_info_reg(struct xdp_rxq_info *xdp_rxq,
		     struct net_device *dev, u32 queue_index)
{
	if (xdp_rxq->reg_state == REG_STATE_UNUSED) {
		WARN(1, "Driver promised not to register this");
		return -EINVAL;
	}

	if (xdp_rxq->reg_state == REG_STATE_REGISTERED) {
		WARN(1, "Missing unregister, handled but fix driver");
		xdp_rxq_info_unreg(xdp_rxq);
	}

	if (!dev) {
		WARN(1, "Missing net_device from driver");
		return -ENODEV;
	}

	/* State either UNREGISTERED or NEW */
	xdp_rxq_info_init(xdp_rxq);
	xdp_rxq->dev = dev;
	xdp_rxq->queue_index = queue_index;

	xdp_rxq->reg_state = REG_STATE_REGISTERED;
	return 0;
}
EXPORT_SYMBOL_GPL(xdp_rxq_info_reg);

void xdp_rxq_info_unused(struct xdp_rxq_info *xdp_rxq)
{
	xdp_rxq->reg_state = REG_STATE_UNUSED;
}
EXPORT_SYMBOL_GPL(xdp_rxq_info_unused);

bool xdp_rxq_info_is_reg(struct xdp_rxq_info *xdp_rxq)
{
	return (xdp_rxq->reg_state == REG_STATE_REGISTERED);
}
EXPORT_SYMBOL_GPL(xdp_rxq_info_is_reg);

static int __mem_id_init_hash_table(void)
{
	struct rhashtable *rht;
	int ret;

	if (unlikely(mem_id_init))
		return 0;

	rht = kzalloc(sizeof(*rht), GFP_KERNEL);
	if (!rht)
		return -ENOMEM;

	ret = rhashtable_init(rht, &mem_id_rht_params);
	if (ret < 0) {
		kfree(rht);
		return ret;
	}
	mem_id_ht = rht;
	smp_mb(); /* mutex lock should provide enough pairing */
	mem_id_init = true;

	return 0;
}

/* Allocate a cyclic ID that maps to allocator pointer.
 * See: https://www.kernel.org/doc/html/latest/core-api/idr.html
 *
 * Caller must lock mem_id_lock.
 */
static int __mem_id_cyclic_get(gfp_t gfp)
{
	int retries = 1;
	int id;

again:
	id = ida_simple_get(&mem_id_pool, mem_id_next, MEM_ID_MAX, gfp);
	if (id < 0) {
		if (id == -ENOSPC) {
			/* Cyclic allocator, reset next id */
			if (retries--) {
				mem_id_next = MEM_ID_MIN;
				goto again;
			}
		}
		return id; /* errno */
	}
	mem_id_next = id + 1;

	return id;
}

static bool __is_supported_mem_type(enum xdp_mem_type type)
{
	if (type == MEM_TYPE_PAGE_POOL)
		return is_page_pool_compiled_in();

	if (type >= MEM_TYPE_MAX)
		return false;

	return true;
}

int xdp_rxq_info_reg_mem_model(struct xdp_rxq_info *xdp_rxq,
			       enum xdp_mem_type type, void *allocator)
{
	struct xdp_mem_allocator *xdp_alloc;
	gfp_t gfp = GFP_KERNEL;
	int id, errno, ret;
	void *ptr;

	if (xdp_rxq->reg_state != REG_STATE_REGISTERED) {
		WARN(1, "Missing register, driver bug");
		return -EFAULT;
	}

	if (!__is_supported_mem_type(type))
		return -EOPNOTSUPP;

	xdp_rxq->mem.type = type;

	if (!allocator) {
		if (type == MEM_TYPE_PAGE_POOL || type == MEM_TYPE_ZERO_COPY)
			return -EINVAL; /* Setup time check page_pool req */
		return 0;
	}

	/* Delay init of rhashtable to save memory if feature isn't used */
	if (!mem_id_init) {
		mutex_lock(&mem_id_lock);
		ret = __mem_id_init_hash_table();
		mutex_unlock(&mem_id_lock);
		if (ret < 0) {
			WARN_ON(1);
			return ret;
		}
	}

	xdp_alloc = kzalloc(sizeof(*xdp_alloc), gfp);
	if (!xdp_alloc)
		return -ENOMEM;

	mutex_lock(&mem_id_lock);
	id = __mem_id_cyclic_get(gfp);
	if (id < 0) {
		errno = id;
		goto err;
	}
	xdp_rxq->mem.id = id;
	xdp_alloc->mem  = xdp_rxq->mem;
	xdp_alloc->allocator = allocator;

	/* Insert allocator into ID lookup table */
	ptr = rhashtable_insert_slow(mem_id_ht, &id, &xdp_alloc->node);
	if (IS_ERR(ptr)) {
		ida_simple_remove(&mem_id_pool, xdp_rxq->mem.id);
		xdp_rxq->mem.id = 0;
		errno = PTR_ERR(ptr);
		goto err;
	}

	if (type == MEM_TYPE_PAGE_POOL)
		page_pool_get(xdp_alloc->page_pool);

	mutex_unlock(&mem_id_lock);

	trace_mem_connect(xdp_alloc, xdp_rxq);
	return 0;
err:
	mutex_unlock(&mem_id_lock);
	kfree(xdp_alloc);
	return errno;
}
EXPORT_SYMBOL_GPL(xdp_rxq_info_reg_mem_model);

/* XDP RX runs under NAPI protection, and in different delivery error
 * scenarios (e.g. queue full), it is possible to return the xdp_frame
 * while still leveraging this protection.  The @napi_direct boolian
 * is used for those calls sites.  Thus, allowing for faster recycling
 * of xdp_frames/pages in those cases.
 */
static void __xdp_return(void *data, struct xdp_mem_info *mem, bool napi_direct,
			 unsigned long handle)
{
	struct xdp_mem_allocator *xa;
	struct page *page;

	switch (mem->type) {
	case MEM_TYPE_PAGE_POOL:
		rcu_read_lock();
		/* mem->id is valid, checked in xdp_rxq_info_reg_mem_model() */
		xa = rhashtable_lookup(mem_id_ht, &mem->id, mem_id_rht_params);
		page = virt_to_head_page(data);
		if (likely(xa)) {
			napi_direct &= !xdp_return_frame_no_direct();
			page_pool_put_page(xa->page_pool, page, napi_direct);
		} else {
			/* Hopefully stack show who to blame for late return */
			WARN_ONCE(1, "page_pool gone mem.id=%d", mem->id);
			trace_mem_return_failed(mem, page);
			put_page(page);
		}
		rcu_read_unlock();
		break;
	case MEM_TYPE_PAGE_SHARED:
		page_frag_free(data);
		break;
	case MEM_TYPE_PAGE_ORDER0:
		page = virt_to_page(data); /* Assumes order0 page*/
		put_page(page);
		break;
	case MEM_TYPE_ZERO_COPY:
		/* NB! Only valid from an xdp_buff! */
		rcu_read_lock();
		/* mem->id is valid, checked in xdp_rxq_info_reg_mem_model() */
		xa = rhashtable_lookup(mem_id_ht, &mem->id, mem_id_rht_params);
		xa->zc_alloc->free(xa->zc_alloc, handle);
		rcu_read_unlock();
	default:
		/* Not possible, checked in xdp_rxq_info_reg_mem_model() */
		break;
	}
}

void xdp_return_frame(struct xdp_frame *xdpf)
{
	__xdp_return(xdpf->data, &xdpf->mem, false, 0);
}
EXPORT_SYMBOL_GPL(xdp_return_frame);

void xdp_return_frame_rx_napi(struct xdp_frame *xdpf)
{
	__xdp_return(xdpf->data, &xdpf->mem, true, 0);
}
EXPORT_SYMBOL_GPL(xdp_return_frame_rx_napi);

void xdp_return_buff(struct xdp_buff *xdp)
{
	__xdp_return(xdp->data, &xdp->rxq->mem, true, xdp->handle);
}
EXPORT_SYMBOL_GPL(xdp_return_buff);

/* Only called for MEM_TYPE_PAGE_POOL see xdp.h */
void __xdp_release_frame(void *data, struct xdp_mem_info *mem)
{
	struct xdp_mem_allocator *xa;
	struct page *page;

	rcu_read_lock();
	xa = rhashtable_lookup(mem_id_ht, &mem->id, mem_id_rht_params);
	page = virt_to_head_page(data);
	if (xa)
		page_pool_release_page(xa->page_pool, page);
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(__xdp_release_frame);

int xdp_attachment_query(struct xdp_attachment_info *info,
			 struct netdev_bpf *bpf)
{
	bpf->prog_id = info->prog ? info->prog->aux->id : 0;
	bpf->prog_flags = info->prog ? info->flags : 0;
	return 0;
}
EXPORT_SYMBOL_GPL(xdp_attachment_query);

bool xdp_attachment_flags_ok(struct xdp_attachment_info *info,
			     struct netdev_bpf *bpf)
{
	if (info->prog && (bpf->flags ^ info->flags) & XDP_FLAGS_MODES) {
		NL_SET_ERR_MSG(bpf->extack,
			       "program loaded with different flags");
		return false;
	}
	return true;
}
EXPORT_SYMBOL_GPL(xdp_attachment_flags_ok);

void xdp_attachment_setup(struct xdp_attachment_info *info,
			  struct netdev_bpf *bpf)
{
	if (info->prog)
		bpf_prog_put(info->prog);
	info->prog = bpf->prog;
	info->flags = bpf->flags;
}
EXPORT_SYMBOL_GPL(xdp_attachment_setup);

struct xdp_frame *xdp_convert_zc_to_xdp_frame(struct xdp_buff *xdp)
{
	unsigned int metasize, totsize;
	void *addr, *data_to_copy;
	struct xdp_frame *xdpf;
	struct page *page;

	/* Clone into a MEM_TYPE_PAGE_ORDER0 xdp_frame. */
	metasize = xdp_data_meta_unsupported(xdp) ? 0 :
		   xdp->data - xdp->data_meta;
	totsize = xdp->data_end - xdp->data + metasize;

	if (sizeof(*xdpf) + totsize > PAGE_SIZE)
		return NULL;

	page = dev_alloc_page();
	if (!page)
		return NULL;

	addr = page_to_virt(page);
	xdpf = addr;
	memset(xdpf, 0, sizeof(*xdpf));

	addr += sizeof(*xdpf);
	data_to_copy = metasize ? xdp->data_meta : xdp->data;
	memcpy(addr, data_to_copy, totsize);

	xdpf->data = addr + metasize;
	xdpf->len = totsize - metasize;
	xdpf->headroom = 0;
	xdpf->metasize = metasize;
	xdpf->mem.type = MEM_TYPE_PAGE_ORDER0;

	xdp_return_buff(xdp);
	return xdpf;
}
EXPORT_SYMBOL_GPL(xdp_convert_zc_to_xdp_frame);
