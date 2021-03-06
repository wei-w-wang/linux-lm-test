/*
 * Virtio balloon implementation, inspired by Dor Laor and Marcelo
 * Tosatti's implementations.
 *
 *  Copyright 2008 Rusty Russell IBM Corporation
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <linux/virtio.h>
#include <linux/virtio_balloon.h>
#include <linux/swap.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/balloon_compaction.h>
#include <linux/oom.h>
#include <linux/wait.h>
#include <linux/mm.h>
#include <linux/mount.h>

/*
 * Balloon device works in 4K page units.  So each page is pointed to by
 * multiple balloon pages.  All memory counters in this driver are in balloon
 * page units.
 */
#define VIRTIO_BALLOON_PAGES_PER_PAGE (unsigned)(PAGE_SIZE >> VIRTIO_BALLOON_PFN_SHIFT)
#define VIRTIO_BALLOON_ARRAY_PFNS_MAX 256
#define OOM_VBALLOON_DEFAULT_PAGES 256
#define VIRTBALLOON_OOM_NOTIFY_PRIORITY 80

#define PAGE_BMAP_SIZE		(8 * PAGE_SIZE)
#define PFNS_PER_PAGE_BMAP	(PAGE_BMAP_SIZE * BITS_PER_BYTE)
#define PAGE_BMAP_COUNT_MAX	32

static int oom_pages = OOM_VBALLOON_DEFAULT_PAGES;
module_param(oom_pages, int, S_IRUSR | S_IWUSR);
MODULE_PARM_DESC(oom_pages, "pages to free on OOM");

#ifdef CONFIG_BALLOON_COMPACTION
static struct vfsmount *balloon_mnt;
#endif

/* Types of pages to chunk */
#define PAGE_CHUNK_TYPE_BALLOON 0
#define PAGE_CHUNK_TYPE_UNUSED 1

#define MAX_PAGE_CHUNKS 4096
struct virtio_balloon {
	struct virtio_device *vdev;
	struct virtqueue *inflate_vq, *deflate_vq, *stats_vq, *miscq;

	/* The balloon servicing is delegated to a freezable workqueue. */
	struct work_struct update_balloon_stats_work;
	struct work_struct update_balloon_size_work;

	/* Prevent updating balloon when it is being canceled. */
	spinlock_t stop_update_lock;
	bool stop_update;

	/* Waiting for host to ack the pages we released. */
	wait_queue_head_t acked;

	/* Number of balloon pages we've told the Host we're not using. */
	unsigned int num_pages;
	/*
	 * The pages we've told the Host we're not using are enqueued
	 * at vb_dev_info->pages list.
	 * Each page on this list adds VIRTIO_BALLOON_PAGES_PER_PAGE
	 * to num_pages above.
	 */
	struct balloon_dev_info vb_dev_info;

	/* Synchronize access/update to this struct virtio_balloon elements */
	struct mutex balloon_lock;

	/*
	 * Buffer for PAGE_CHUNK_TYPE_BALLOON:
	 * virtio_balloon_page_chunk_hdr +
	 * virtio_balloon_page_chunk * MAX_PAGE_CHUNKS
	 */
	struct virtio_balloon_page_chunk_hdr *balloon_page_chunk_hdr;
	struct virtio_balloon_page_chunk *balloon_page_chunk;

	/*
	 * Buffer for PAGE_CHUNK_TYPE_UNUSED:
	 * virtio_balloon_miscq_hdr +
	 * virtio_balloon_page_chunk_hdr +
	 * virtio_balloon_page_chunk * MAX_PAGE_CHUNKS
	 */
	struct virtio_balloon_miscq_hdr *miscq_out_hdr;
	struct virtio_balloon_page_chunk_hdr *unused_page_chunk_hdr;
	struct virtio_balloon_page_chunk *unused_page_chunk;

	/* Buffer for host to send cmd to miscq */
	struct virtio_balloon_miscq_hdr *miscq_in_hdr;

	/* Bitmap used to record pages */
	unsigned long *page_bmap[PAGE_BMAP_COUNT_MAX];
	/* Number of the allocated page_bmap */
	unsigned int page_bmaps;

	/*
	 * The allocated page_bmap size may be smaller than the pfn range of
	 * the ballooned pages. In this case, we need to use the page_bmap
	 * multiple times to cover the entire pfn range. It's like using a
	 * short ruler several times to finish measuring a long object.
	 * The start location of the ruler in the next measurement is the end
	 * location of the ruler in the previous measurement.
	 *
	 * pfn_max & pfn_min: forms the pfn range of the ballooned pages
	 * pfn_start & pfn_stop: records the start and stop pfn in each cover
	 */
	unsigned long pfn_min, pfn_max, pfn_start, pfn_stop;

	/* The array of pfns we tell the Host about. */
	unsigned int num_pfns;
	__virtio32 pfns[VIRTIO_BALLOON_ARRAY_PFNS_MAX];

	/* Memory statistics */
	struct virtio_balloon_stat stats[VIRTIO_BALLOON_S_NR];

	/* To register callback in oom notifier call chain */
	struct notifier_block nb;
};

static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_BALLOON, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static u32 page_to_balloon_pfn(struct page *page)
{
	unsigned long pfn = page_to_pfn(page);

	BUILD_BUG_ON(PAGE_SHIFT < VIRTIO_BALLOON_PFN_SHIFT);
	/* Convert pfn from Linux page size to balloon page size. */
	return pfn * VIRTIO_BALLOON_PAGES_PER_PAGE;
}

static void balloon_ack(struct virtqueue *vq)
{
	struct virtio_balloon *vb = vq->vdev->priv;

	wake_up(&vb->acked);
}

static inline void init_page_bmap_range(struct virtio_balloon *vb)
{
	vb->pfn_min = ULONG_MAX;
	vb->pfn_max = 0;
}

static inline void update_page_bmap_range(struct virtio_balloon *vb,
					  struct page *page)
{
	unsigned long balloon_pfn = page_to_balloon_pfn(page);

	vb->pfn_min = min(balloon_pfn, vb->pfn_min);
	vb->pfn_max = max(balloon_pfn, vb->pfn_max);
}

/* The page_bmap size is extended by adding more number of page_bmap */
static void extend_page_bmap_size(struct virtio_balloon *vb,
				  unsigned long pfns)
{
	int i, bmaps;
	unsigned long bmap_len;

	bmap_len = ALIGN(pfns, BITS_PER_LONG) / BITS_PER_BYTE;
	bmap_len = ALIGN(bmap_len, PAGE_BMAP_SIZE);
	bmaps = min((int)(bmap_len / PAGE_BMAP_SIZE),
		    PAGE_BMAP_COUNT_MAX);

	for (i = 1; i < bmaps; i++) {
		vb->page_bmap[i] = kmalloc(PAGE_BMAP_SIZE, GFP_KERNEL);
		if (vb->page_bmap[i])
			vb->page_bmaps++;
		else
			break;
	}
}

static void free_extended_page_bmap(struct virtio_balloon *vb)
{
	int i, bmaps = vb->page_bmaps;

	for (i = 1; i < bmaps; i++) {
		kfree(vb->page_bmap[i]);
		vb->page_bmap[i] = NULL;
		vb->page_bmaps--;
	}
}

static void free_page_bmap(struct virtio_balloon *vb)
{
	int i;

	for (i = 0; i < vb->page_bmaps; i++)
		kfree(vb->page_bmap[i]);
}

static void clear_page_bmap(struct virtio_balloon *vb)
{
	int i;

	for (i = 0; i < vb->page_bmaps; i++)
		memset(vb->page_bmap[i], 0, PAGE_BMAP_SIZE);
}

static void send_page_chunks(struct virtio_balloon *vb, struct virtqueue *vq,
			     int type, bool busy_wait)
{
	struct scatterlist sg;
	struct virtio_balloon_page_chunk_hdr *hdr;
	void *buf;
	unsigned int len;

	switch (type) {
	case PAGE_CHUNK_TYPE_BALLOON:
		hdr = vb->balloon_page_chunk_hdr;
		len = 0;
		break;
	case PAGE_CHUNK_TYPE_UNUSED:
		hdr = vb->unused_page_chunk_hdr;
		len = sizeof(struct virtio_balloon_miscq_hdr);
		break;
	default:
		dev_warn(&vb->vdev->dev, "%s: chunk %d of unknown pages\n",
			 __func__, type);
		return;
	}

	buf = (void *)hdr - len;
	len += sizeof(struct virtio_balloon_page_chunk_hdr);
	len += hdr->chunks * sizeof(struct virtio_balloon_page_chunk);
	sg_init_table(&sg, 1);
	sg_set_buf(&sg, buf, len);
	if (!virtqueue_add_outbuf(vq, &sg, 1, vb, GFP_KERNEL)) {
		virtqueue_kick(vq);
		if (busy_wait)
			while (!virtqueue_get_buf(vq, &len) &&
			       !virtqueue_is_broken(vq))
				cpu_relax();
		else
			wait_event(vb->acked, virtqueue_get_buf(vq, &len));
		hdr->chunks = 0;
	}
}

static void add_one_chunk(struct virtio_balloon *vb, struct virtqueue *vq,
			  int type, u64 base, u64 size)
{
	struct virtio_balloon_page_chunk_hdr *hdr;
	struct virtio_balloon_page_chunk *chunk;

	switch (type) {
	case PAGE_CHUNK_TYPE_BALLOON:
		hdr = vb->balloon_page_chunk_hdr;
		chunk = vb->balloon_page_chunk;
		break;
	case PAGE_CHUNK_TYPE_UNUSED:
		hdr = vb->unused_page_chunk_hdr;
		chunk = vb->unused_page_chunk;
		break;
	default:
		dev_warn(&vb->vdev->dev, "%s: chunk %d of unknown pages\n",
			 __func__, type);
		return;
	}
	chunk = chunk + hdr->chunks;
	chunk->base = cpu_to_le64(base << VIRTIO_BALLOON_CHUNK_BASE_SHIFT);
	chunk->size = cpu_to_le64(size << VIRTIO_BALLOON_CHUNK_SIZE_SHIFT);
	hdr->chunks++;
	if (hdr->chunks == MAX_PAGE_CHUNKS)
		send_page_chunks(vb, vq, type, false);
}

static void chunking_pages_from_bmap(struct virtio_balloon *vb,
				     struct virtqueue *vq,
				     unsigned long pfn_start,
				     unsigned long *bmap,
				     unsigned long len)
{
	unsigned long pos = 0, end = len * BITS_PER_BYTE;

	while (pos < end) {
		unsigned long one = find_next_bit(bmap, end, pos);

		if (one < end) {
			unsigned long chunk_size, zero;

			zero = find_next_zero_bit(bmap, end, one + 1);
			if (zero >= end)
				chunk_size = end - one;
			else
				chunk_size = zero - one;

			if (chunk_size)
				add_one_chunk(vb, vq, PAGE_CHUNK_TYPE_BALLOON,
					      pfn_start + one, chunk_size);
			pos = one + chunk_size;
		} else
			break;
	}
}

static void tell_host(struct virtio_balloon *vb, struct virtqueue *vq)
{
	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_BALLOON_CHUNKS)) {
		int pfns, page_bmaps, i;
		unsigned long pfn_start, pfns_len;

		pfn_start = vb->pfn_start;
		pfns = vb->pfn_stop - pfn_start + 1;
		pfns = roundup(roundup(pfns, BITS_PER_LONG),
			       PFNS_PER_PAGE_BMAP);
		page_bmaps = pfns / PFNS_PER_PAGE_BMAP;
		pfns_len = pfns / BITS_PER_BYTE;

		for (i = 0; i < page_bmaps; i++) {
			unsigned int bmap_len = PAGE_BMAP_SIZE;

			/* The last one takes the leftover only */
			if (i + 1 == page_bmaps)
				bmap_len = pfns_len - PAGE_BMAP_SIZE * i;

			chunking_pages_from_bmap(vb, vq, pfn_start +
						 i * PFNS_PER_PAGE_BMAP,
						 vb->page_bmap[i], bmap_len);
		}
		if (vb->balloon_page_chunk_hdr->chunks > 0)
			send_page_chunks(vb, vq, PAGE_CHUNK_TYPE_BALLOON,
					 false);
	} else {
		struct scatterlist sg;
		unsigned int len;

		sg_init_one(&sg, vb->pfns, sizeof(vb->pfns[0]) * vb->num_pfns);

		/*
		 * We should always be able to add one buffer to an empty
		 * queue.
		 */
		virtqueue_add_outbuf(vq, &sg, 1, vb, GFP_KERNEL);
		virtqueue_kick(vq);

		/* When host has read buffer, this completes via balloon_ack */
		wait_event(vb->acked, virtqueue_get_buf(vq, &len));
	}
}

static void set_page_pfns(struct virtio_balloon *vb,
			  __virtio32 pfns[], struct page *page)
{
	unsigned int i;

	/*
	 * Set balloon pfns pointing at this page.
	 * Note that the first pfn points at start of the page.
	 */
	for (i = 0; i < VIRTIO_BALLOON_PAGES_PER_PAGE; i++)
		pfns[i] = cpu_to_virtio32(vb->vdev,
					  page_to_balloon_pfn(page) + i);
}

static void set_page_bmap(struct virtio_balloon *vb,
			  struct list_head *pages, struct virtqueue *vq)
{
	unsigned long pfn_start, pfn_stop;
	struct page *page;
	bool found;

	vb->pfn_min = rounddown(vb->pfn_min, BITS_PER_LONG);
	vb->pfn_max = roundup(vb->pfn_max, BITS_PER_LONG);

	extend_page_bmap_size(vb, vb->pfn_max - vb->pfn_min + 1);
	pfn_start = vb->pfn_min;

	while (pfn_start < vb->pfn_max) {
		pfn_stop = pfn_start + PFNS_PER_PAGE_BMAP * vb->page_bmaps;
		pfn_stop = pfn_stop < vb->pfn_max ? pfn_stop : vb->pfn_max;

		vb->pfn_start = pfn_start;
		clear_page_bmap(vb);
		found = false;

		list_for_each_entry(page, pages, lru) {
			unsigned long bmap_idx, bmap_pos, balloon_pfn;

			balloon_pfn = page_to_balloon_pfn(page);
			if (balloon_pfn < pfn_start || balloon_pfn > pfn_stop)
				continue;
			bmap_idx = (balloon_pfn - pfn_start) /
				   PFNS_PER_PAGE_BMAP;
			bmap_pos = (balloon_pfn - pfn_start) %
				   PFNS_PER_PAGE_BMAP;
			set_bit(bmap_pos, vb->page_bmap[bmap_idx]);

			found = true;
		}
		if (found) {
			vb->pfn_stop = pfn_stop;
			tell_host(vb, vq);
		}
		pfn_start = pfn_stop;
	}
	free_extended_page_bmap(vb);
}

static unsigned fill_balloon(struct virtio_balloon *vb, size_t num)
{
	struct balloon_dev_info *vb_dev_info = &vb->vb_dev_info;
	unsigned num_allocated_pages;
	bool chunking = virtio_has_feature(vb->vdev,
					   VIRTIO_BALLOON_F_BALLOON_CHUNKS);

	/* We can only do one array worth at a time. */
	if (chunking) {
		init_page_bmap_range(vb);
	} else {
		/* We can only do one array worth at a time. */
		num = min(num, ARRAY_SIZE(vb->pfns));
	}

	mutex_lock(&vb->balloon_lock);
	for (vb->num_pfns = 0; vb->num_pfns < num;
	     vb->num_pfns += VIRTIO_BALLOON_PAGES_PER_PAGE) {
		struct page *page = balloon_page_enqueue(vb_dev_info);

		if (!page) {
			dev_info_ratelimited(&vb->vdev->dev,
					     "Out of puff! Can't get %u pages\n",
					     VIRTIO_BALLOON_PAGES_PER_PAGE);
			/* Sleep for at least 1/5 of a second before retry. */
			msleep(200);
			break;
		}
		if (chunking)
			update_page_bmap_range(vb, page);
		else
			set_page_pfns(vb, vb->pfns + vb->num_pfns, page);
		vb->num_pages += VIRTIO_BALLOON_PAGES_PER_PAGE;
		if (!virtio_has_feature(vb->vdev,
					VIRTIO_BALLOON_F_DEFLATE_ON_OOM))
			adjust_managed_page_count(page, -1);
	}

	num_allocated_pages = vb->num_pfns;
	/* Did we get any? */
	if (vb->num_pfns != 0) {
		if (chunking)
			set_page_bmap(vb, &vb_dev_info->pages,
					vb->inflate_vq);
		else
			tell_host(vb, vb->inflate_vq);
	}
	mutex_unlock(&vb->balloon_lock);

	return num_allocated_pages;
}

static void release_pages_balloon(struct virtio_balloon *vb,
				 struct list_head *pages)
{
	struct page *page, *next;

	list_for_each_entry_safe(page, next, pages, lru) {
		if (!virtio_has_feature(vb->vdev,
					VIRTIO_BALLOON_F_DEFLATE_ON_OOM))
			adjust_managed_page_count(page, 1);
		list_del(&page->lru);
		put_page(page); /* balloon reference */
	}
}

static unsigned leak_balloon(struct virtio_balloon *vb, size_t num)
{
	unsigned num_freed_pages;
	struct page *page;
	struct balloon_dev_info *vb_dev_info = &vb->vb_dev_info;
	LIST_HEAD(pages);
	bool chunking = virtio_has_feature(vb->vdev,
					   VIRTIO_BALLOON_F_BALLOON_CHUNKS);
	if (chunking)
		init_page_bmap_range(vb);
	else
		/* We can only do one array worth at a time. */
		num = min(num, ARRAY_SIZE(vb->pfns));

	/* We can only do one array worth at a time. */
	num = min(num, ARRAY_SIZE(vb->pfns));

	mutex_lock(&vb->balloon_lock);
	/* We can't release more pages than taken */
	num = min(num, (size_t)vb->num_pages);
	for (vb->num_pfns = 0; vb->num_pfns < num;
	     vb->num_pfns += VIRTIO_BALLOON_PAGES_PER_PAGE) {
		page = balloon_page_dequeue(vb_dev_info);
		if (!page)
			break;
		set_page_pfns(vb, vb->pfns + vb->num_pfns, page);
		if (chunking)
			update_page_bmap_range(vb, page);
		else
			set_page_pfns(vb, vb->pfns + vb->num_pfns, page);
		list_add(&page->lru, &pages);
		vb->num_pages -= VIRTIO_BALLOON_PAGES_PER_PAGE;
	}

	num_freed_pages = vb->num_pfns;
	/*
	 * Note that if
	 * virtio_has_feature(vdev, VIRTIO_BALLOON_F_MUST_TELL_HOST);
	 * is true, we *have* to do it in this order
	 */
	if (vb->num_pfns != 0) {
		if (chunking)
			set_page_bmap(vb, &pages, vb->deflate_vq);
		else
			tell_host(vb, vb->deflate_vq);
	}
	release_pages_balloon(vb, &pages);
	mutex_unlock(&vb->balloon_lock);
	return num_freed_pages;
}

static inline void update_stat(struct virtio_balloon *vb, int idx,
			       u16 tag, u64 val)
{
	BUG_ON(idx >= VIRTIO_BALLOON_S_NR);
	vb->stats[idx].tag = cpu_to_virtio16(vb->vdev, tag);
	vb->stats[idx].val = cpu_to_virtio64(vb->vdev, val);
}

#define pages_to_bytes(x) ((u64)(x) << PAGE_SHIFT)

static void update_balloon_stats(struct virtio_balloon *vb)
{
	unsigned long events[NR_VM_EVENT_ITEMS];
	struct sysinfo i;
	int idx = 0;
	long available;

	all_vm_events(events);
	si_meminfo(&i);

	available = si_mem_available();

	update_stat(vb, idx++, VIRTIO_BALLOON_S_SWAP_IN,
				pages_to_bytes(events[PSWPIN]));
	update_stat(vb, idx++, VIRTIO_BALLOON_S_SWAP_OUT,
				pages_to_bytes(events[PSWPOUT]));
	update_stat(vb, idx++, VIRTIO_BALLOON_S_MAJFLT, events[PGMAJFAULT]);
	update_stat(vb, idx++, VIRTIO_BALLOON_S_MINFLT, events[PGFAULT]);
	update_stat(vb, idx++, VIRTIO_BALLOON_S_MEMFREE,
				pages_to_bytes(i.freeram));
	update_stat(vb, idx++, VIRTIO_BALLOON_S_MEMTOT,
				pages_to_bytes(i.totalram));
	update_stat(vb, idx++, VIRTIO_BALLOON_S_AVAIL,
				pages_to_bytes(available));
}

/*
 * While most virtqueues communicate guest-initiated requests to the hypervisor,
 * the stats queue operates in reverse.  The driver initializes the virtqueue
 * with a single buffer.  From that point forward, all conversations consist of
 * a hypervisor request (a call to this function) which directs us to refill
 * the virtqueue with a fresh stats buffer.  Since stats collection can sleep,
 * we delegate the job to a freezable workqueue that will do the actual work via
 * stats_handle_request().
 */
static void stats_request(struct virtqueue *vq)
{
	struct virtio_balloon *vb = vq->vdev->priv;

	spin_lock(&vb->stop_update_lock);
	if (!vb->stop_update)
		queue_work(system_freezable_wq, &vb->update_balloon_stats_work);
	spin_unlock(&vb->stop_update_lock);
}

static void stats_handle_request(struct virtio_balloon *vb)
{
	struct virtqueue *vq;
	struct scatterlist sg;
	unsigned int len;

	update_balloon_stats(vb);

	vq = vb->stats_vq;
	if (!virtqueue_get_buf(vq, &len))
		return;
	sg_init_one(&sg, vb->stats, sizeof(vb->stats));
	virtqueue_add_outbuf(vq, &sg, 1, vb, GFP_KERNEL);
	virtqueue_kick(vq);
}

static void virtballoon_changed(struct virtio_device *vdev)
{
	struct virtio_balloon *vb = vdev->priv;
	unsigned long flags;

	spin_lock_irqsave(&vb->stop_update_lock, flags);
	if (!vb->stop_update)
		queue_work(system_freezable_wq, &vb->update_balloon_size_work);
	spin_unlock_irqrestore(&vb->stop_update_lock, flags);
}

static inline s64 towards_target(struct virtio_balloon *vb)
{
	s64 target;
	u32 num_pages;

	virtio_cread(vb->vdev, struct virtio_balloon_config, num_pages,
		     &num_pages);

	/* Legacy balloon config space is LE, unlike all other devices. */
	if (!virtio_has_feature(vb->vdev, VIRTIO_F_VERSION_1))
		num_pages = le32_to_cpu((__force __le32)num_pages);

	target = num_pages;
	return target - vb->num_pages;
}

static void update_balloon_size(struct virtio_balloon *vb)
{
	u32 actual = vb->num_pages;

	/* Legacy balloon config space is LE, unlike all other devices. */
	if (!virtio_has_feature(vb->vdev, VIRTIO_F_VERSION_1))
		actual = (__force u32)cpu_to_le32(actual);

	virtio_cwrite(vb->vdev, struct virtio_balloon_config, actual,
		      &actual);
}

/*
 * virtballoon_oom_notify - release pages when system is under severe
 *			    memory pressure (called from out_of_memory())
 * @self : notifier block struct
 * @dummy: not used
 * @parm : returned - number of freed pages
 *
 * The balancing of memory by use of the virtio balloon should not cause
 * the termination of processes while there are pages in the balloon.
 * If virtio balloon manages to release some memory, it will make the
 * system return and retry the allocation that forced the OOM killer
 * to run.
 */
static int virtballoon_oom_notify(struct notifier_block *self,
				  unsigned long dummy, void *parm)
{
	struct virtio_balloon *vb;
	unsigned long *freed;
	unsigned num_freed_pages;

	vb = container_of(self, struct virtio_balloon, nb);
	if (!virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_DEFLATE_ON_OOM))
		return NOTIFY_OK;

	freed = parm;
	num_freed_pages = leak_balloon(vb, oom_pages);
	update_balloon_size(vb);
	*freed += num_freed_pages;

	return NOTIFY_OK;
}

static void update_balloon_stats_func(struct work_struct *work)
{
	struct virtio_balloon *vb;

	vb = container_of(work, struct virtio_balloon,
			  update_balloon_stats_work);
	stats_handle_request(vb);
}

static void update_balloon_size_func(struct work_struct *work)
{
	struct virtio_balloon *vb;
	s64 diff;

	vb = container_of(work, struct virtio_balloon,
			  update_balloon_size_work);
	diff = towards_target(vb);

	if (diff > 0)
		diff -= fill_balloon(vb, diff);
	else if (diff < 0)
		diff += leak_balloon(vb, -diff);
	update_balloon_size(vb);

	if (diff)
		queue_work(system_freezable_wq, work);
}

static void miscq_in_hdr_add(struct virtio_balloon *vb)
{
	struct scatterlist sg_in;

	sg_init_one(&sg_in, vb->miscq_in_hdr,
		    sizeof(struct virtio_balloon_miscq_hdr));
	if (virtqueue_add_inbuf(vb->miscq, &sg_in, 1, vb->miscq_in_hdr,
	    GFP_KERNEL) < 0) {
		__virtio_clear_bit(vb->vdev,
				   VIRTIO_BALLOON_F_MISC_VQ);
		dev_warn(&vb->vdev->dev, "%s: add miscq_in_hdr err\n",
			 __func__);
		return;
	}
	virtqueue_kick(vb->miscq);
}

static void miscq_send_unused_pages(struct virtio_balloon *vb)
{
	struct virtio_balloon_miscq_hdr *miscq_out_hdr = vb->miscq_out_hdr;
	struct virtqueue *vq = vb->miscq;
	int ret = 0;
	unsigned int order = 0, migratetype = 0;
	struct zone *zone = NULL;
	struct page *page = NULL;
	u64 pfn;

	miscq_out_hdr->cmd =  VIRTIO_BALLOON_MISCQ_INQUIRE_UNUSED_PAGES;
	miscq_out_hdr->flags = 0;

	for_each_populated_zone(zone) {
		for (order = MAX_ORDER - 1; order > 0; order--) {
			for (migratetype = 0; migratetype < MIGRATE_TYPES;
			     migratetype++) {
				do {
					ret = inquire_unused_page_block(zone,
						order, migratetype, &page);
					if (!ret) {
						pfn = (u64)page_to_pfn(page);
						add_one_chunk(vb, vq,
							PAGE_CHUNK_TYPE_UNUSED,
							pfn,
							(u64)(1 << order));
					}
				} while (!ret);
			}
		}
	}
	miscq_out_hdr->flags |= VIRTIO_BALLOON_MISCQ_F_COMPLETE;
	send_page_chunks(vb, vq, PAGE_CHUNK_TYPE_UNUSED, true);
}

static void miscq_handle(struct virtqueue *vq)
{
	struct virtio_balloon *vb = vq->vdev->priv;
	struct virtio_balloon_miscq_hdr *hdr;
	unsigned int len;

	hdr = virtqueue_get_buf(vb->miscq, &len);
	if (!hdr || len != sizeof(struct virtio_balloon_miscq_hdr)) {
		dev_warn(&vb->vdev->dev, "%s: invalid miscq hdr len\n",
			 __func__);
		miscq_in_hdr_add(vb);
		return;
	}
	switch (hdr->cmd) {
	case VIRTIO_BALLOON_MISCQ_INQUIRE_UNUSED_PAGES:
		miscq_send_unused_pages(vb);
		break;
	default:
		dev_warn(&vb->vdev->dev, "%s: miscq cmd %d not supported\n",
			 __func__, hdr->cmd);
	}
	miscq_in_hdr_add(vb);
}

static int init_vqs(struct virtio_balloon *vb)
{
	struct virtqueue **vqs;
	vq_callback_t **callbacks;
	const char **names;
	int err = -ENOMEM;
	int i, nvqs;

	 /* Inflateq and deflateq are used unconditionally */
	nvqs = 2;

	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_STATS_VQ))
		nvqs++;
	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_MISC_VQ))
		nvqs++;

	/* Allocate space for find_vqs parameters */
	vqs = kcalloc(nvqs, sizeof(*vqs), GFP_KERNEL);
	if (!vqs)
		goto err_vq;
	callbacks = kmalloc_array(nvqs, sizeof(*callbacks), GFP_KERNEL);
	if (!callbacks)
		goto err_callback;
	names = kmalloc_array(nvqs, sizeof(*names), GFP_KERNEL);
	if (!names)
		goto err_names;

	callbacks[0] = balloon_ack;
	names[0] = "inflate";
	callbacks[1] = balloon_ack;
	names[1] = "deflate";

	i = 2;
	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_STATS_VQ)) {
		callbacks[i] = stats_request;
		names[i] = "stats";
		i++;
	}

	if (virtio_has_feature(vb->vdev,
				      VIRTIO_BALLOON_F_MISC_VQ)) {
		callbacks[i] = miscq_handle;
		names[i] = "miscq";
	}

	err = vb->vdev->config->find_vqs(vb->vdev, nvqs, vqs, callbacks,
					 names);
	if (err)
		goto err_find;

	vb->inflate_vq = vqs[0];
	vb->deflate_vq = vqs[1];
	i = 2;
	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_STATS_VQ)) {
		struct scatterlist sg;

		vb->stats_vq = vqs[i++];
		/*
		 * Prime this virtqueue with one buffer so the hypervisor can
		 * use it to signal us later (it can't be broken yet!).
		 */
		sg_init_one(&sg, vb->stats, sizeof vb->stats);
		if (virtqueue_add_outbuf(vb->stats_vq, &sg, 1, vb, GFP_KERNEL)
		    < 0)
			BUG();
		virtqueue_kick(vb->stats_vq);
	}

	if (virtio_has_feature(vb->vdev, VIRTIO_BALLOON_F_MISC_VQ)) {
		vb->miscq = vqs[i];
		miscq_in_hdr_add(vb);
	}

	kfree(names);
	kfree(callbacks);
	kfree(vqs);
	return 0;

err_find:
	kfree(names);
err_names:
	kfree(callbacks);
err_callback:
	kfree(vqs);
err_vq:
	return err;
}

#ifdef CONFIG_BALLOON_COMPACTION

static void tell_host_one_page(struct virtio_balloon *vb,
			       struct virtqueue *vq, struct page *page)
{
	add_one_chunk(vb, vq, PAGE_CHUNK_TYPE_BALLOON, page_to_pfn(page), 1);
}

/*
 * virtballoon_migratepage - perform the balloon page migration on behalf of
 *			     a compation thread.     (called under page lock)
 * @vb_dev_info: the balloon device
 * @newpage: page that will replace the isolated page after migration finishes.
 * @page   : the isolated (old) page that is about to be migrated to newpage.
 * @mode   : compaction mode -- not used for balloon page migration.
 *
 * After a ballooned page gets isolated by compaction procedures, this is the
 * function that performs the page migration on behalf of a compaction thread
 * The page migration for virtio balloon is done in a simple swap fashion which
 * follows these two macro steps:
 *  1) insert newpage into vb->pages list and update the host about it;
 *  2) update the host about the old page removed from vb->pages list;
 *
 * This function preforms the balloon page migration task.
 * Called through balloon_mapping->a_ops->migratepage
 */
static int virtballoon_migratepage(struct balloon_dev_info *vb_dev_info,
		struct page *newpage, struct page *page, enum migrate_mode mode)
{
	struct virtio_balloon *vb = container_of(vb_dev_info,
			struct virtio_balloon, vb_dev_info);
	bool chunking = virtio_has_feature(vb->vdev,
					   VIRTIO_BALLOON_F_BALLOON_CHUNKS);
	unsigned long flags;

	/*
	 * In order to avoid lock contention while migrating pages concurrently
	 * to leak_balloon() or fill_balloon() we just give up the balloon_lock
	 * this turn, as it is easier to retry the page migration later.
	 * This also prevents fill_balloon() getting stuck into a mutex
	 * recursion in the case it ends up triggering memory compaction
	 * while it is attempting to inflate the ballon.
	 */
	if (!mutex_trylock(&vb->balloon_lock))
		return -EAGAIN;

	get_page(newpage); /* balloon reference */

	/* balloon's page migration 1st step  -- inflate "newpage" */
	spin_lock_irqsave(&vb_dev_info->pages_lock, flags);
	balloon_page_insert(vb_dev_info, newpage);
	vb_dev_info->isolated_pages--;
	__count_vm_event(BALLOON_MIGRATE);
	spin_unlock_irqrestore(&vb_dev_info->pages_lock, flags);
	if (chunking) {
		tell_host_one_page(vb, vb->inflate_vq, newpage);
	} else {
		vb->num_pfns = VIRTIO_BALLOON_PAGES_PER_PAGE;
		set_page_pfns(vb, vb->pfns, newpage);
		tell_host(vb, vb->inflate_vq);
	}
	/* balloon's page migration 2nd step -- deflate "page" */
	balloon_page_delete(page);
	if (chunking) {
		tell_host_one_page(vb, vb->deflate_vq, page);
	} else {
		vb->num_pfns = VIRTIO_BALLOON_PAGES_PER_PAGE;
		set_page_pfns(vb, vb->pfns, page);
		tell_host(vb, vb->deflate_vq);
	}
	mutex_unlock(&vb->balloon_lock);

	put_page(page); /* balloon reference */

	return MIGRATEPAGE_SUCCESS;
}

static struct dentry *balloon_mount(struct file_system_type *fs_type,
		int flags, const char *dev_name, void *data)
{
	static const struct dentry_operations ops = {
		.d_dname = simple_dname,
	};

	return mount_pseudo(fs_type, "balloon-kvm:", NULL, &ops,
				BALLOON_KVM_MAGIC);
}

static struct file_system_type balloon_fs = {
	.name           = "balloon-kvm",
	.mount          = balloon_mount,
	.kill_sb        = kill_anon_super,
};

#endif /* CONFIG_BALLOON_COMPACTION */

static void balloon_page_chunk_init(struct virtio_balloon *vb)
{
	void *buf;

	/*
	 * By default, we allocate page_bmap[0] only. More page_bmap will be
	 * allocated on demand.
	 */
	vb->page_bmap[0] = kmalloc(PAGE_BMAP_SIZE, GFP_KERNEL);
	buf = kmalloc(sizeof(struct virtio_balloon_page_chunk_hdr) +
		      sizeof(struct virtio_balloon_page_chunk) *
		      MAX_PAGE_CHUNKS, GFP_KERNEL);
	if (!vb->page_bmap[0] || !buf) {
		__virtio_clear_bit(vb->vdev, VIRTIO_BALLOON_F_BALLOON_CHUNKS);
		kfree(vb->page_bmap[0]);
		kfree(vb->balloon_page_chunk_hdr);
		dev_warn(&vb->vdev->dev, "%s: failed\n", __func__);
	} else {
		vb->page_bmaps = 1;
		vb->balloon_page_chunk_hdr = buf;
		vb->balloon_page_chunk_hdr->chunks = 0;
		vb->balloon_page_chunk = buf +
				sizeof(struct virtio_balloon_page_chunk_hdr);
	}
}

static void miscq_init(struct virtio_balloon *vb)
{
	void *buf;

	vb->miscq_in_hdr = kmalloc(sizeof(struct virtio_balloon_miscq_hdr),
				   GFP_KERNEL);
	buf = kmalloc(sizeof(struct virtio_balloon_miscq_hdr) +
		      sizeof(struct virtio_balloon_page_chunk_hdr) +
		      sizeof(struct virtio_balloon_page_chunk) *
		      MAX_PAGE_CHUNKS, GFP_KERNEL);
	if (!vb->miscq_in_hdr || !buf) {
		kfree(buf);
		kfree(vb->miscq_in_hdr);
		__virtio_clear_bit(vb->vdev, VIRTIO_BALLOON_F_MISC_VQ);
		dev_warn(&vb->vdev->dev, "%s: failed\n", __func__);
	} else {
		vb->miscq_out_hdr = buf;
		vb->unused_page_chunk_hdr = buf +
				sizeof(struct virtio_balloon_miscq_hdr);
		vb->unused_page_chunk_hdr->chunks = 0;
		vb->unused_page_chunk = buf +
				sizeof(struct virtio_balloon_miscq_hdr) +
				sizeof(struct virtio_balloon_page_chunk_hdr);
	}
}

static int virtballoon_probe(struct virtio_device *vdev)
{
	struct virtio_balloon *vb;
	int err;

	if (!vdev->config->get) {
		dev_err(&vdev->dev, "%s failure: config access disabled\n",
			__func__);
		return -EINVAL;
	}

	vdev->priv = vb = kmalloc(sizeof(*vb), GFP_KERNEL);
	if (!vb) {
		err = -ENOMEM;
		goto out;
	}

	INIT_WORK(&vb->update_balloon_stats_work, update_balloon_stats_func);
	INIT_WORK(&vb->update_balloon_size_work, update_balloon_size_func);
	spin_lock_init(&vb->stop_update_lock);
	vb->stop_update = false;
	vb->num_pages = 0;

	if (virtio_has_feature(vdev, VIRTIO_BALLOON_F_BALLOON_CHUNKS))
		balloon_page_chunk_init(vb);

	if (virtio_has_feature(vdev, VIRTIO_BALLOON_F_MISC_VQ))
		miscq_init(vb);

	mutex_init(&vb->balloon_lock);
	init_waitqueue_head(&vb->acked);
	vb->vdev = vdev;

	balloon_devinfo_init(&vb->vb_dev_info);

	err = init_vqs(vb);
	if (err)
		goto out_free_vb;

	vb->nb.notifier_call = virtballoon_oom_notify;
	vb->nb.priority = VIRTBALLOON_OOM_NOTIFY_PRIORITY;
	err = register_oom_notifier(&vb->nb);
	if (err < 0)
		goto out_del_vqs;

#ifdef CONFIG_BALLOON_COMPACTION
	balloon_mnt = kern_mount(&balloon_fs);
	if (IS_ERR(balloon_mnt)) {
		err = PTR_ERR(balloon_mnt);
		unregister_oom_notifier(&vb->nb);
		goto out_del_vqs;
	}

	vb->vb_dev_info.migratepage = virtballoon_migratepage;
	vb->vb_dev_info.inode = alloc_anon_inode(balloon_mnt->mnt_sb);
	if (IS_ERR(vb->vb_dev_info.inode)) {
		err = PTR_ERR(vb->vb_dev_info.inode);
		kern_unmount(balloon_mnt);
		unregister_oom_notifier(&vb->nb);
		vb->vb_dev_info.inode = NULL;
		goto out_del_vqs;
	}
	vb->vb_dev_info.inode->i_mapping->a_ops = &balloon_aops;
#endif

	virtio_device_ready(vdev);

	if (towards_target(vb))
		virtballoon_changed(vdev);
	return 0;

out_del_vqs:
	vdev->config->del_vqs(vdev);
out_free_vb:
	kfree(vb);
out:
	return err;
}

static void remove_common(struct virtio_balloon *vb)
{
	/* There might be pages left in the balloon: free them. */
	while (vb->num_pages)
		leak_balloon(vb, vb->num_pages);
	update_balloon_size(vb);

	/* Now we reset the device so we can clean up the queues. */
	vb->vdev->config->reset(vb->vdev);

	vb->vdev->config->del_vqs(vb->vdev);
}

static void virtballoon_remove(struct virtio_device *vdev)
{
	struct virtio_balloon *vb = vdev->priv;

	unregister_oom_notifier(&vb->nb);

	spin_lock_irq(&vb->stop_update_lock);
	vb->stop_update = true;
	spin_unlock_irq(&vb->stop_update_lock);
	cancel_work_sync(&vb->update_balloon_size_work);
	cancel_work_sync(&vb->update_balloon_stats_work);

	remove_common(vb);
	free_page_bmap(vb);
	kfree(vb->miscq_out_hdr);
	kfree(vb->miscq_in_hdr);
	if (vb->vb_dev_info.inode)
		iput(vb->vb_dev_info.inode);
	kfree(vb);
}

#ifdef CONFIG_PM_SLEEP
static int virtballoon_freeze(struct virtio_device *vdev)
{
	struct virtio_balloon *vb = vdev->priv;

	/*
	 * The workqueue is already frozen by the PM core before this
	 * function is called.
	 */
	remove_common(vb);
	return 0;
}

static int virtballoon_restore(struct virtio_device *vdev)
{
	struct virtio_balloon *vb = vdev->priv;
	int ret;

	ret = init_vqs(vdev->priv);
	if (ret)
		return ret;

	virtio_device_ready(vdev);

	if (towards_target(vb))
		virtballoon_changed(vdev);
	update_balloon_size(vb);
	return 0;
}
#endif

static unsigned int features[] = {
	VIRTIO_BALLOON_F_MUST_TELL_HOST,
	VIRTIO_BALLOON_F_STATS_VQ,
	VIRTIO_BALLOON_F_DEFLATE_ON_OOM,
	VIRTIO_BALLOON_F_BALLOON_CHUNKS,
	VIRTIO_BALLOON_F_MISC_VQ,
};

static struct virtio_driver virtio_balloon_driver = {
	.feature_table = features,
	.feature_table_size = ARRAY_SIZE(features),
	.driver.name =	KBUILD_MODNAME,
	.driver.owner =	THIS_MODULE,
	.id_table =	id_table,
	.probe =	virtballoon_probe,
	.remove =	virtballoon_remove,
	.config_changed = virtballoon_changed,
#ifdef CONFIG_PM_SLEEP
	.freeze	=	virtballoon_freeze,
	.restore =	virtballoon_restore,
#endif
};

module_virtio_driver(virtio_balloon_driver);
MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio balloon driver");
MODULE_LICENSE("GPL");
