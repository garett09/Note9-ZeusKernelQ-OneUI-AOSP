/*
 * linux/mm/hpa.c
 *
 * Copyright (C) 2015 Samsung Electronics, Inc. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.

 * Does best efforts to allocate required high-order pages.
 */

#include <linux/list.h>
#include <linux/bootmem.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/mm_types.h>
#include <linux/mmzone.h>
#include <linux/migrate.h>
#include <linux/memcontrol.h>
#include <linux/page-isolation.h>
#include <linux/mm_inline.h>
#include <linux/swap.h>
#include <linux/scatterlist.h>
#include <linux/debugfs.h>
#include <linux/vmalloc.h>
#include <linux/device.h>
#include <linux/oom.h>

#include "internal.h"

#define MAX_SCAN_TRY		(2)

static unsigned long start_pfn, end_pfn;
static unsigned long cached_scan_pfn;

#define HPA_MIN_OOMADJ	100

static bool oom_unkillable_task(struct task_struct *p)
{
	if (is_global_init(p))
		return true;
	if (p->flags & PF_KTHREAD)
		return true;
	return false;
}

static bool oom_skip_task(struct task_struct *p, int selected_adj)
{
	if (same_thread_group(p, current))
		return true;
	if (p->signal->oom_score_adj <= HPA_MIN_OOMADJ)
		return true;
	if ((p->signal->oom_score_adj < selected_adj) &&
	    (selected_adj <= OOM_SCORE_ADJ_MAX))
		return true;
	if (test_bit(MMF_OOM_SKIP, &p->mm->flags))
		return true;
	if (in_vfork(p))
		return true;
	return false;
}

static int hpa_killer(void)
{
	struct task_struct *tsk, *p;
	struct task_struct *selected = NULL;
	unsigned long selected_tasksize = 0;
	int selected_adj = OOM_SCORE_ADJ_MAX + 1;

	rcu_read_lock();
	for_each_process(tsk) {
		int tasksize;
		int current_adj;

		if (oom_unkillable_task(tsk))
			continue;

		p = find_lock_task_mm(tsk);
		if (!p)
			continue;

		if (!oom_skip_task(p, selected_adj)) {
			task_unlock(p);
			continue;
		}

		tasksize = get_mm_rss(p->mm);
		tasksize += get_mm_counter(p->mm, MM_SWAPENTS);
		tasksize += mm_pgtables_bytes(p->mm) / PAGE_SIZE;
		current_adj = p->signal->oom_score_adj;

		task_unlock(p);

		if (selected &&
		    (current_adj == selected_adj) &&
		    (tasksize <= selected_tasksize))
			continue;

		put_task_struct(selected);
		selected = p;
		selected_tasksize = tasksize;
		selected_adj = current_adj;
		get_task_struct(selected);
	}
	rcu_read_unlock();

	if (!selected) {
		pr_info("HPA: no killable task\n");
		return -ESRCH;
	}

	pr_info("HPA: Killing '%s' (%d), adj %hd to free %lukB\n",
		selected->comm, task_pid_nr(selected), selected_adj,
		selected_tasksize * (PAGE_SIZE / SZ_1K));

	do_send_sig_info(SIGKILL, SEND_SIG_FORCED, selected, true);

	put_task_struct(selected);

	return 0;
}

static bool is_movable_chunk(unsigned long pfn, unsigned int order)
{
	struct page *page = pfn_to_page(pfn);
	struct page *page_end = pfn_to_page(pfn + (1 << order));

	while (page != page_end) {
		if (PageCompound(page) || PageReserved(page))
			return false;
		if (!PageLRU(page) && !__PageMovable(page))
			return false;

		page += PageBuddy(page) ? 1 << page_order(page) : 1;
	}

	return true;
}

static int alloc_freepages_range(struct zone *zone, unsigned int order,
				 struct page **pages, int required)

{
	unsigned int current_order;
	unsigned int mt;
	unsigned long wmark;
	unsigned long flags;
	struct free_area *area;
	struct page *page;
	int i;
	int count = 0;
	struct list_head *pos, *n;

	spin_lock_irqsave(&zone->lock, flags);

	for (current_order = order; current_order < MAX_ORDER; ++current_order) {
		area = &(zone->free_area[current_order]);
		wmark = min_wmark_pages(zone) + (1 << current_order);

		for (mt = MIGRATE_UNMOVABLE; mt < MIGRATE_PCPTYPES; ++mt) {
			list_for_each_safe(pos, n, &area->free_list[mt]) {
				if (!zone_watermark_ok(zone, current_order,
							wmark, 0, 0))
					goto wmark_fail;

				/*
				 * expanding the current free chunk is not
				 * supported here due to the complex logic of
				 * expand().
				 */
				if ((required << order) < (1 << current_order))
					break;

				page = list_entry(pos, struct page, lru);

				if ((page_to_pfn(page) >= 0xF0000) &&
						(page_to_pfn(page) <= 0xFFFFF))
					continue;
				if (page_to_pfn(page) > end_pfn)
					continue;

				list_del(&page->lru);
				__ClearPageBuddy(page);
				set_page_private(page, 0);
				set_pcppage_migratetype(page, mt);
				/*
				 * skip checking bad page state
				 * for fast allocation
				 */
				area->nr_free--;
				__mod_zone_page_state(zone, NR_FREE_PAGES,
							-(1 << current_order));

				required -= 1 << (current_order - order);

				for (i = 1 << (current_order - order); i > 0; i--) {
					post_alloc_hook(page, order, GFP_KERNEL);
					pages[count++] = page;
					page += 1 << order;
				}
			}
		}
	}

wmark_fail:
	spin_unlock_irqrestore(&zone->lock, flags);

	return count;
}

static void prep_highorder_pages(unsigned long base_pfn, int order)
{
	int nr_pages = 1 << order;
	unsigned long pfn;

	for (pfn = base_pfn + 1; pfn < base_pfn + nr_pages; pfn++)
		set_page_count(pfn_to_page(pfn), 0);
}

int alloc_pages_highorder(int order, struct page **pages, int nents)
{
	struct zone *zone;
	unsigned int nr_pages = 1 << order;
	unsigned long total_scanned = 0;
	unsigned long pfn, tmp;
	int remained = nents;
	int ret;
	int retry_count = 0;
	int allocated;

retry:
	for_each_zone(zone) {
		if (zone->spanned_pages == 0)
			continue;

		allocated = alloc_freepages_range(zone, order,
					pages + nents - remained, remained);
		remained -= allocated;

		if (remained == 0)
			return 0;
	}

	migrate_prep();

	for (pfn = ALIGN(cached_scan_pfn, nr_pages);
			(total_scanned < (end_pfn - start_pfn) * MAX_SCAN_TRY)
			&& (remained > 0);
			pfn += nr_pages, total_scanned += nr_pages) {
		int mt;

		if (pfn + nr_pages > end_pfn) {
			pfn = start_pfn;
			continue;
		}

		/* pfn validation check in the range */
		if ((pfn >= 0xF0000) && (pfn <= 0xFFFFF))
			pfn = 0x800000;

		tmp = pfn;
		do {
			if (!pfn_valid(tmp))
				break;
		} while (++tmp < (pfn + nr_pages));

		if (tmp < (pfn + nr_pages))
			continue;

		mt = get_pageblock_migratetype(pfn_to_page(pfn));
		/*
		 * CMA pages should not be reclaimed.
		 * Isolated page blocks should not be tried again because it
		 * causes isolated page block remained in isolated state
		 * forever.
		 */
		if (is_migrate_cma_rbin(mt) || is_migrate_isolate(mt)) {
			/* nr_pages is added before next iteration */
			pfn = ALIGN(pfn + 1, pageblock_nr_pages) - nr_pages;
			continue;
		}

		if (!is_movable_chunk(pfn, order))
			continue;

		ret = alloc_contig_range_fast(pfn, pfn + nr_pages, mt);
		if (ret == 0)
			prep_highorder_pages(pfn, order);
		else
			continue;

		pages[nents - remained] = pfn_to_page(pfn);
		remained--;
	}

	/* save latest scanned pfn */
	cached_scan_pfn = pfn;

	if (remained) {
		int i;

		drop_slab();
		count_vm_event(DROP_SLAB);
		ret = hpa_killer();
		if (ret == 0) {
			total_scanned = 0;
			pr_info("HPA: drop_slab and killer retry %d count\n",
				retry_count++);
			goto retry;
		}

		for (i = 0; i < (nents - remained); i++)
			__free_pages(pages[i], order);

		pr_info("%s: remained=%d / %d, not enough memory in order %d\n",
				 __func__, remained, nents, order);

		ret = -ENOMEM;
	}

	return ret;
}

int free_pages_highorder(int order, struct page **pages, int nents)
{
	int i;

	for (i = 0; i < nents; i++)
		__free_pages(pages[i], order);

	return 0;
}

static int __init init_highorder_pages_allocator(void)
{
	struct zone *zone;

	for_each_zone(zone) {
		if (zone->spanned_pages == 0)
			continue;
		if (zone_idx(zone) == ZONE_MOVABLE) {
			start_pfn = zone->zone_start_pfn;
			end_pfn = start_pfn + zone->present_pages;
		}
	}

	if (!start_pfn) {
		start_pfn = __phys_to_pfn(memblock_start_of_DRAM());
		end_pfn = max_pfn;
	}

	if (end_pfn > 0x97FFFF)
		end_pfn = 0x97FFFF;

	cached_scan_pfn = start_pfn;

	return 0;
}
late_initcall(init_highorder_pages_allocator);
