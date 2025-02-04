/*
 * A fairly generic DMA-API to IOMMU-API glue layer.
 *
 * Copyright (C) 2014-2015 ARM Ltd.
 *
 * based in part on arch/arm/mm/dma-mapping.c:
 * Copyright (C) 2000-2004 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/device.h>
#include <linux/dma-iommu.h>
#include <linux/gfp.h>
#include <linux/huge_mm.h>
#include <linux/iommu.h>
#include <linux/iova.h>
#include <linux/irq.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <linux/scatterlist.h>
#include <linux/vmalloc.h>

struct iommu_dma_msi_page {
	struct list_head	list;
	dma_addr_t		iova;
	phys_addr_t		phys;
};

enum iommu_dma_cookie_type {
	IOMMU_DMA_IOVA_COOKIE,
	IOMMU_DMA_MSI_COOKIE,
};

struct iommu_dma_cookie {
	enum iommu_dma_cookie_type	type;
	union {
		/* Full allocator for IOMMU_DMA_IOVA_COOKIE */
		struct iova_domain	iovad;
		/* Trivial linear page allocator for IOMMU_DMA_MSI_COOKIE */
		dma_addr_t		msi_iova;
	};
	struct list_head		msi_page_list;
	spinlock_t			msi_lock;
};

static inline size_t cookie_msi_granule(struct iommu_dma_cookie *cookie)
{
	if (cookie->type == IOMMU_DMA_IOVA_COOKIE)
		return cookie->iovad.granule;
	return PAGE_SIZE;
}

static inline struct iova_domain *cookie_iovad(struct iommu_domain *domain)
{
	struct iommu_dma_cookie *cookie = domain->iova_cookie;

	if (cookie->type == IOMMU_DMA_IOVA_COOKIE)
		return &cookie->iovad;
	return NULL;
}

static struct iommu_dma_cookie *cookie_alloc(enum iommu_dma_cookie_type type)
{
	struct iommu_dma_cookie *cookie;

	cookie = kzalloc(sizeof(*cookie), GFP_KERNEL);
	if (cookie) {
		spin_lock_init(&cookie->msi_lock);
		INIT_LIST_HEAD(&cookie->msi_page_list);
		cookie->type = type;
	}
	return cookie;
}

int iommu_dma_init(void)
{
	return iova_cache_get();
}

/**
 * iommu_get_dma_cookie - Acquire DMA-API resources for a domain
 * @domain: IOMMU domain to prepare for DMA-API usage
 *
 * IOMMU drivers should normally call this from their domain_alloc
 * callback when domain->type == IOMMU_DOMAIN_DMA.
 */
int iommu_get_dma_cookie(struct iommu_domain *domain)
{
	if (domain->iova_cookie)
		return -EEXIST;

	domain->iova_cookie = cookie_alloc(IOMMU_DMA_IOVA_COOKIE);
	if (!domain->iova_cookie)
		return -ENOMEM;

	return 0;
}
EXPORT_SYMBOL(iommu_get_dma_cookie);

/**
 * iommu_get_msi_cookie - Acquire just MSI remapping resources
 * @domain: IOMMU domain to prepare
 * @base: Start address of IOVA region for MSI mappings
 *
 * Users who manage their own IOVA allocation and do not want DMA API support,
 * but would still like to take advantage of automatic MSI remapping, can use
 * this to initialise their own domain appropriately. Users should reserve a
 * contiguous IOVA region, starting at @base, large enough to accommodate the
 * number of PAGE_SIZE mappings necessary to cover every MSI doorbell address
 * used by the devices attached to @domain.
 */
int iommu_get_msi_cookie(struct iommu_domain *domain, dma_addr_t base)
{
	struct iommu_dma_cookie *cookie;

	if (domain->type != IOMMU_DOMAIN_UNMANAGED)
		return -EINVAL;

	if (domain->iova_cookie)
		return -EEXIST;

	cookie = cookie_alloc(IOMMU_DMA_MSI_COOKIE);
	if (!cookie)
		return -ENOMEM;

	cookie->msi_iova = base;
	domain->iova_cookie = cookie;
	return 0;
}
EXPORT_SYMBOL(iommu_get_msi_cookie);

/**
 * iommu_put_dma_cookie - Release a domain's DMA mapping resources
 * @domain: IOMMU domain previously prepared by iommu_get_dma_cookie() or
 *          iommu_get_msi_cookie()
 *
 * IOMMU drivers should normally call this from their domain_free callback.
 */
void iommu_put_dma_cookie(struct iommu_domain *domain)
{
	struct iommu_dma_cookie *cookie = domain->iova_cookie;
	struct iommu_dma_msi_page *msi, *tmp;

	if (!cookie)
		return;

	if (cookie->type == IOMMU_DMA_IOVA_COOKIE && cookie->iovad.granule)
		put_iova_domain(&cookie->iovad);

	list_for_each_entry_safe(msi, tmp, &cookie->msi_page_list, list) {
		list_del(&msi->list);
		kfree(msi);
	}
	kfree(cookie);
	domain->iova_cookie = NULL;
}
EXPORT_SYMBOL(iommu_put_dma_cookie);

static void iova_reserve_pci_windows(struct pci_dev *dev,
		struct iova_domain *iovad)
{
	struct pci_host_bridge *bridge = pci_find_host_bridge(dev->bus);
	struct resource_entry *window;
	unsigned long lo, hi;

	resource_list_for_each_entry(window, &bridge->windows) {
		if (resource_type(window->res) != IORESOURCE_MEM &&
		    resource_type(window->res) != IORESOURCE_IO)
			continue;

		lo = iova_pfn(iovad, window->res->start - window->offset);
		hi = iova_pfn(iovad, window->res->end - window->offset);
		reserve_iova(iovad, lo, hi);
	}
}

/**
 * iommu_dma_init_domain - Initialise a DMA mapping domain
 * @domain: IOMMU domain previously prepared by iommu_get_dma_cookie()
 * @base: IOVA at which the mappable address space starts
 * @size: Size of IOVA space
 * @dev: Device the domain is being initialised for
 *
 * @base and @size should be exact multiples of IOMMU page granularity to
 * avoid rounding surprises. If necessary, we reserve the page at address 0
 * to ensure it is an invalid IOVA. It is safe to reinitialise a domain, but
 * any change which could make prior IOVAs invalid will fail.
 */
int iommu_dma_init_domain(struct iommu_domain *domain, dma_addr_t base,
		u64 size, struct device *dev)
{
	struct iommu_dma_cookie *cookie = domain->iova_cookie;
	struct iova_domain *iovad = &cookie->iovad;
	unsigned long order, base_pfn, end_pfn;

	if (!cookie || cookie->type != IOMMU_DMA_IOVA_COOKIE)
		return -EINVAL;

	/* Use the smallest supported page size for IOVA granularity */
	order = __ffs(domain->ops->pgsize_bitmap);
	base_pfn = max_t(unsigned long, 1, base >> order);
	end_pfn = (base + size - 1) >> order;

	/* Check the domain allows at least some access to the device... */
	if (domain->geometry.force_aperture) {
		if (base > domain->geometry.aperture_end ||
		    base + size <= domain->geometry.aperture_start) {
			pr_warn("specified DMA range outside IOMMU capability\n");
			return -EFAULT;
		}
		/* ...then finally give it a kicking to make sure it fits */
		base_pfn = max_t(unsigned long, base_pfn,
				domain->geometry.aperture_start >> order);
		end_pfn = min_t(unsigned long, end_pfn,
				domain->geometry.aperture_end >> order);
	}

	/* All we can safely do with an existing domain is enlarge it */
	if (iovad->start_pfn) {
		if (1UL << order != iovad->granule ||
		    base_pfn != iovad->start_pfn ||
		    end_pfn < iovad->dma_32bit_pfn) {
			pr_warn("Incompatible range for DMA domain\n");
			return -EFAULT;
		}
		iovad->dma_32bit_pfn = end_pfn;
	} else {
		init_iova_domain(iovad, 1UL << order, base_pfn, end_pfn);
		if (dev && dev_is_pci(dev))
			iova_reserve_pci_windows(to_pci_dev(dev), iovad);
	}
	return 0;
}
EXPORT_SYMBOL(iommu_dma_init_domain);

/**
 * dma_direction_to_prot - Translate DMA API directions to IOMMU API page flags
 * @dir: Direction of DMA transfer
 * @coherent: Is the DMA master cache-coherent?
 *
 * Return: corresponding IOMMU API page protection flags
 */
int dma_direction_to_prot(enum dma_data_direction dir, bool coherent)
{
	int prot = coherent ? IOMMU_CACHE : 0;

	switch (dir) {
	case DMA_BIDIRECTIONAL:
		return prot | IOMMU_READ | IOMMU_WRITE;
	case DMA_TO_DEVICE:
		return prot | IOMMU_READ;
	case DMA_FROM_DEVICE:
		return prot | IOMMU_WRITE;
	default:
		return 0;
	}
}

static struct iova *__alloc_iova(struct iommu_domain *domain, size_t size,
		dma_addr_t dma_limit)
{
	struct iova_domain *iovad = cookie_iovad(domain);
	unsigned long shift = iova_shift(iovad);
	unsigned long length = iova_align(iovad, size) >> shift;

	if (domain->geometry.force_aperture)
		dma_limit = min(dma_limit, domain->geometry.aperture_end);
	/*
	 * Enforce size-alignment to be safe - there could perhaps be an
	 * attribute to control this per-device, or at least per-domain...
	 */
	return alloc_iova(iovad, length, dma_limit >> shift, true);
}

/* The IOVA allocator knows what we mapped, so just unmap whatever that was */
static void __iommu_dma_unmap(struct iommu_domain *domain, dma_addr_t dma_addr)
{
	struct iova_domain *iovad = cookie_iovad(domain);
	unsigned long shift = iova_shift(iovad);
	unsigned long pfn = dma_addr >> shift;
	struct iova *iova = find_iova(iovad, pfn);
	size_t size;

	if (WARN_ON(!iova))
		return;

	size = iova_size(iova) << shift;
	size -= iommu_unmap(domain, pfn << shift, size);
	/* ...and if we can't, then something is horribly, horribly wrong */
	WARN_ON(size > 0);
	__free_iova(iovad, iova);
}

static void __iommu_dma_free_pages(struct page **pages, int count)
{
	while (count--)
		__free_page(pages[count]);
	kvfree(pages);
}

static struct page **__iommu_dma_alloc_pages(unsigned int count, gfp_t gfp)
{
	struct page **pages;
	unsigned int i = 0, array_size = count * sizeof(*pages);
	unsigned int order = MAX_ORDER;

	if (array_size <= PAGE_SIZE)
		pages = kzalloc(array_size, GFP_KERNEL);
	else
		pages = vzalloc(array_size);
	if (!pages)
		return NULL;

	/* IOMMU can map any pages, so himem can also be used here */
	gfp |= __GFP_NOWARN | __GFP_HIGHMEM;

	while (count) {
		struct page *page = NULL;
		int j;

		/*
		 * Higher-order allocations are a convenience rather
		 * than a necessity, hence using __GFP_NORETRY until
		 * falling back to single-page allocations.
		 */
		for (order = min_t(unsigned int, order, __fls(count));
		     order > 0; order--) {
			page = alloc_pages(gfp | __GFP_NORETRY, order);
			if (!page)
				continue;
			if (PageCompound(page)) {
				if (!split_huge_page(page))
					break;
				__free_pages(page, order);
			} else {
				split_page(page, order);
				break;
			}
		}
		if (!page)
			page = alloc_page(gfp);
		if (!page) {
			__iommu_dma_free_pages(pages, i);
			return NULL;
		}
		j = 1 << order;
		count -= j;
		while (j--)
			pages[i++] = page++;
	}
	return pages;
}

/**
 * iommu_dma_free - Free a buffer allocated by iommu_dma_alloc()
 * @dev: Device which owns this buffer
 * @pages: Array of buffer pages as returned by iommu_dma_alloc()
 * @size: Size of buffer in bytes
 * @handle: DMA address of buffer
 *
 * Frees both the pages associated with the buffer, and the array
 * describing them
 */
void iommu_dma_free(struct device *dev, struct page **pages, size_t size,
		dma_addr_t *handle)
{
	__iommu_dma_unmap(iommu_get_domain_for_dev(dev), *handle);
	__iommu_dma_free_pages(pages, PAGE_ALIGN(size) >> PAGE_SHIFT);
	*handle = DMA_ERROR_CODE;
}

/**
 * iommu_dma_alloc - Allocate and map a buffer contiguous in IOVA space
 * @dev: Device to allocate memory for. Must be a real device
 *	 attached to an iommu_dma_domain
 * @size: Size of buffer in bytes
 * @gfp: Allocation flags
 * @prot: IOMMU mapping flags
 * @handle: Out argument for allocated DMA handle
 * @flush_page: Arch callback which must ensure PAGE_SIZE bytes from the
 *		given VA/PA are visible to the given non-coherent device.
 *
 * If @size is less than PAGE_SIZE, then a full CPU page will be allocated,
 * but an IOMMU which supports smaller pages might not map the whole thing.
 *
 * Return: Array of struct page pointers describing the buffer,
 *	   or NULL on failure.
 */
struct page **iommu_dma_alloc(struct device *dev, size_t size,
		gfp_t gfp, int prot, dma_addr_t *handle,
		void (*flush_page)(struct device *, const void *, phys_addr_t))
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	struct iova_domain *iovad = cookie_iovad(domain);
	struct iova *iova;
	struct page **pages;
	struct sg_table sgt;
	dma_addr_t dma_addr;
	unsigned int count = PAGE_ALIGN(size) >> PAGE_SHIFT;

	*handle = DMA_ERROR_CODE;

	pages = __iommu_dma_alloc_pages(count, gfp);
	if (!pages)
		return NULL;

	iova = __alloc_iova(domain, size, dev->coherent_dma_mask);
	if (!iova)
		goto out_free_pages;

	size = iova_align(iovad, size);
	if (sg_alloc_table_from_pages(&sgt, pages, count, 0, size, GFP_KERNEL))
		goto out_free_iova;

	if (!(prot & IOMMU_CACHE)) {
		struct sg_mapping_iter miter;
		/*
		 * The CPU-centric flushing implied by SG_MITER_TO_SG isn't
		 * sufficient here, so skip it by using the "wrong" direction.
		 */
		sg_miter_start(&miter, sgt.sgl, sgt.orig_nents, SG_MITER_FROM_SG);
		while (sg_miter_next(&miter))
			flush_page(dev, miter.addr, page_to_phys(miter.page));
		sg_miter_stop(&miter);
	}

	dma_addr = iova_dma_addr(iovad, iova);
	if (iommu_map_sg(domain, dma_addr, sgt.sgl, sgt.orig_nents, prot)
			< size)
		goto out_free_sg;

	*handle = dma_addr;
	sg_free_table(&sgt);
	return pages;

out_free_sg:
	sg_free_table(&sgt);
out_free_iova:
	__free_iova(iovad, iova);
out_free_pages:
	__iommu_dma_free_pages(pages, count);
	return NULL;
}

/**
 * iommu_dma_mmap - Map a buffer into provided user VMA
 * @pages: Array representing buffer from iommu_dma_alloc()
 * @size: Size of buffer in bytes
 * @vma: VMA describing requested userspace mapping
 *
 * Maps the pages of the buffer in @pages into @vma. The caller is responsible
 * for verifying the correct size and protection of @vma beforehand.
 */

int iommu_dma_mmap(struct page **pages, size_t size, struct vm_area_struct *vma)
{
	unsigned long uaddr = vma->vm_start;
	unsigned int i, count = PAGE_ALIGN(size) >> PAGE_SHIFT;
	int ret = -ENXIO;

	for (i = vma->vm_pgoff; i < count && uaddr < vma->vm_end; i++) {
		ret = vm_insert_page(vma, uaddr, pages[i]);
		if (ret)
			break;
		uaddr += PAGE_SIZE;
	}
	return ret;
}

dma_addr_t iommu_dma_map_page(struct device *dev, struct page *page,
		unsigned long offset, size_t size, int prot)
{
	dma_addr_t dma_addr;
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	struct iova_domain *iovad = cookie_iovad(domain);
	phys_addr_t phys = page_to_phys(page) + offset;
	size_t iova_off = iova_offset(iovad, phys);
	size_t len = iova_align(iovad, size + iova_off);
	struct iova *iova = __alloc_iova(domain, len, dma_get_mask(dev));

	if (!iova)
		return DMA_ERROR_CODE;

	dma_addr = iova_dma_addr(iovad, iova);
	if (iommu_map(domain, dma_addr, phys - iova_off, len, prot)) {
		__free_iova(iovad, iova);
		return DMA_ERROR_CODE;
	}
	return dma_addr + iova_off;
}

void iommu_dma_unmap_page(struct device *dev, dma_addr_t handle, size_t size,
		enum dma_data_direction dir, struct dma_attrs *attrs)
{
	__iommu_dma_unmap(iommu_get_domain_for_dev(dev), handle);
}

/*
 * Prepare a successfully-mapped scatterlist to give back to the caller.
 * Handling IOVA concatenation can come later, if needed
 */
static int __finalise_sg(struct device *dev, struct scatterlist *sg, int nents,
		dma_addr_t dma_addr)
{
	struct scatterlist *s;
	int i;

	for_each_sg(sg, s, nents, i) {
		/* Un-swizzling the fields here, hence the naming mismatch */
		unsigned int s_offset = sg_dma_address(s);
		unsigned int s_length = sg_dma_len(s);
		unsigned int s_dma_len = s->length;

		s->offset += s_offset;
		s->length = s_length;
		sg_dma_address(s) = dma_addr + s_offset;
		dma_addr += s_dma_len;
	}
	return i;
}

/*
 * If mapping failed, then just restore the original list,
 * but making sure the DMA fields are invalidated.
 */
static void __invalidate_sg(struct scatterlist *sg, int nents)
{
	struct scatterlist *s;
	int i;

	for_each_sg(sg, s, nents, i) {
		if (sg_dma_address(s) != DMA_ERROR_CODE)
			s->offset += sg_dma_address(s);
		if (sg_dma_len(s))
			s->length = sg_dma_len(s);
		sg_dma_address(s) = DMA_ERROR_CODE;
		sg_dma_len(s) = 0;
	}
}

/*
 * The DMA API client is passing in a scatterlist which could describe
 * any old buffer layout, but the IOMMU API requires everything to be
 * aligned to IOMMU pages. Hence the need for this complicated bit of
 * impedance-matching, to be able to hand off a suitably-aligned list,
 * but still preserve the original offsets and sizes for the caller.
 */
int iommu_dma_map_sg(struct device *dev, struct scatterlist *sg,
		int nents, int prot)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	struct iova_domain *iovad = cookie_iovad(domain);
	struct iova *iova;
	struct scatterlist *s, *prev = NULL;
	dma_addr_t dma_addr;
	size_t iova_len = 0;
	int i;

	/*
	 * Work out how much IOVA space we need, and align the segments to
	 * IOVA granules for the IOMMU driver to handle. With some clever
	 * trickery we can modify the list in-place, but reversibly, by
	 * hiding the original data in the as-yet-unused DMA fields.
	 */
	for_each_sg(sg, s, nents, i) {
		size_t s_offset = iova_offset(iovad, s->offset);
		size_t s_length = s->length;

		sg_dma_address(s) = s_offset;
		sg_dma_len(s) = s_length;
		s->offset -= s_offset;
		s_length = iova_align(iovad, s_length + s_offset);
		s->length = s_length;

		/*
		 * The simple way to avoid the rare case of a segment
		 * crossing the boundary mask is to pad the previous one
		 * to end at a naturally-aligned IOVA for this one's size,
		 * at the cost of potentially over-allocating a little.
		 */
		if (prev) {
			size_t pad_len = roundup_pow_of_two(s_length);

			pad_len = (pad_len - iova_len) & (pad_len - 1);
			prev->length += pad_len;
			iova_len += pad_len;
		}

		iova_len += s_length;
		prev = s;
	}

	iova = __alloc_iova(domain, iova_len, dma_get_mask(dev));
	if (!iova)
		goto out_restore_sg;

	/*
	 * We'll leave any physical concatenation to the IOMMU driver's
	 * implementation - it knows better than we do.
	 */
	dma_addr = iova_dma_addr(iovad, iova);
	if (iommu_map_sg(domain, dma_addr, sg, nents, prot) < iova_len)
		goto out_free_iova;

	return __finalise_sg(dev, sg, nents, dma_addr);

out_free_iova:
	__free_iova(iovad, iova);
out_restore_sg:
	__invalidate_sg(sg, nents);
	return 0;
}

void iommu_dma_unmap_sg(struct device *dev, struct scatterlist *sg, int nents,
		enum dma_data_direction dir, struct dma_attrs *attrs)
{
	/*
	 * The scatterlist segments are mapped into a single
	 * contiguous IOVA allocation, so this is incredibly easy.
	 */
	__iommu_dma_unmap(iommu_get_domain_for_dev(dev), sg_dma_address(sg));
}

int iommu_dma_supported(struct device *dev, u64 mask)
{
	/*
	 * 'Special' IOMMUs which don't have the same addressing capability
	 * as the CPU will have to wait until we have some way to query that
	 * before they'll be able to use this framework.
	 */
	return 1;
}

int iommu_dma_mapping_error(struct device *dev, dma_addr_t dma_addr)
{
	return dma_addr == DMA_ERROR_CODE;
}

static struct iommu_dma_msi_page *iommu_dma_get_msi_page(struct device *dev,
		phys_addr_t msi_addr, struct iommu_domain *domain)
{
	struct iommu_dma_cookie *cookie = domain->iova_cookie;
	struct iommu_dma_msi_page *msi_page;
	struct iova_domain *iovad = cookie_iovad(domain);
	struct iova *iova;
	int prot = IOMMU_WRITE | IOMMU_NOEXEC | IOMMU_MMIO;
	size_t size = cookie_msi_granule(cookie);

	msi_addr &= ~(phys_addr_t)(size - 1);
	list_for_each_entry(msi_page, &cookie->msi_page_list, list)
		if (msi_page->phys == msi_addr)
			return msi_page;

	msi_page = kzalloc(sizeof(*msi_page), GFP_ATOMIC);
	if (!msi_page)
		return NULL;

	msi_page->phys = msi_addr;
	if (iovad) {
		iova = __alloc_iova(domain, size, dma_get_mask(dev));
		if (!iova)
			goto out_free_page;
		msi_page->iova = iova_dma_addr(iovad, iova);
	} else {
		msi_page->iova = cookie->msi_iova;
		cookie->msi_iova += size;
	}

	if (iommu_map(domain, msi_page->iova, msi_addr, size, prot))
		goto out_free_iova;

	INIT_LIST_HEAD(&msi_page->list);
	list_add(&msi_page->list, &cookie->msi_page_list);
	return msi_page;

out_free_iova:
	if (iovad)
		__free_iova(iovad, iova);
	else
		cookie->msi_iova -= size;
out_free_page:
	kfree(msi_page);
	return NULL;
}

void iommu_dma_map_msi_msg(int irq, struct msi_msg *msg)
{
	struct device *dev = msi_desc_to_dev(irq_get_msi_desc(irq));
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	struct iommu_dma_cookie *cookie;
	struct iommu_dma_msi_page *msi_page;
	phys_addr_t msi_addr = (u64)msg->address_hi << 32 | msg->address_lo;
	unsigned long flags;

	if (!domain || !domain->iova_cookie)
		return;

	cookie = domain->iova_cookie;

	/*
	 * We disable IRQs to rule out a possible inversion against
	 * irq_desc_lock if, say, someone tries to retarget the affinity
	 * of an MSI from within an IPI handler.
	 */
	spin_lock_irqsave(&cookie->msi_lock, flags);
	msi_page = iommu_dma_get_msi_page(dev, msi_addr, domain);
	spin_unlock_irqrestore(&cookie->msi_lock, flags);

	if (WARN_ON(!msi_page)) {
		/*
		 * We're called from a void callback, so the best we can do is
		 * 'fail' by filling the message with obviously bogus values.
		 * Since we got this far due to an IOMMU being present, it's
		 * not like the existing address would have worked anyway...
		 */
		msg->address_hi = ~0U;
		msg->address_lo = ~0U;
		msg->data = ~0U;
	} else {
		msg->address_hi = upper_32_bits(msi_page->iova);
		msg->address_lo &= cookie_msi_granule(cookie) - 1;
		msg->address_lo += lower_32_bits(msi_page->iova);
	}
}
