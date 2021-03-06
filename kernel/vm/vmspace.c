#include <ananas/types.h>
#include <machine/param.h> /* for THREAD_INITIAL_MAPPING_ADDR */
#include <machine/vm.h> /* for md_{,un}map_pages() */
#include <ananas/kmem.h>
#include <ananas/lib.h>
#include <ananas/mm.h>
#include <ananas/error.h>
#include <ananas/trace.h>
#include <ananas/vm.h>
#include <ananas/vmspace.h>

TRACE_SETUP;

#define BYTES_TO_PAGES(len) ((len + PAGE_SIZE - 1) / PAGE_SIZE)

errorcode_t
vmspace_create(vmspace_t** vmspace)
{
	vmspace_t* vs = kmalloc(sizeof(*vs));
	memset(vs, 0, sizeof(*vs));
	DQUEUE_INIT(&vs->vs_pages);
	vs->vs_next_mapping = THREAD_INITIAL_MAPPING_ADDR;

	errorcode_t err = md_vmspace_init(vs);
	ANANAS_ERROR_RETURN(err);
	*vmspace = vs;
	return err;
}

void
vmspace_cleanup(vmspace_t* vs)
{
	/* Cleanup only removes all mapped areas */
	while(!DQUEUE_EMPTY(&vs->vs_areas)) {
		vmarea_t* va = DQUEUE_HEAD(&vs->vs_areas);
		vmspace_area_free(vs, va);
	}
}

void
vmspace_destroy(vmspace_t* vs)
{
	/* Ensure all mapped areas are gone (can't hurt if this is already done) */
	vmspace_cleanup(vs);

	/* Remove the vmspace-specific mappings - these are generally MD */
	DQUEUE_FOREACH_SAFE(&vs->vs_pages, p, struct PAGE) {
		page_free(p);
	}
	md_vmspace_destroy(vs);
	kfree(vs);
}

static int
vmspace_is_inuse(vmspace_t* vs, addr_t virt, size_t len)
{
	if (!DQUEUE_EMPTY(&vs->vs_areas)) {
		DQUEUE_FOREACH(&vs->vs_areas, va, vmarea_t) {
			if ((virt >= va->va_virt && (virt + len) <= (va->va_virt + va->va_len)) ||
			    (va->va_virt >= virt && (va->va_virt + va->va_len) <= (virt + len)))
				return 1;
		}
	}

	return 0;
}

errorcode_t
vmspace_mapto(vmspace_t* vs, addr_t virt, addr_t phys, size_t len /* bytes */, uint32_t flags, vmarea_t** va_out)
{
	/* First, ensure the range isn't used and the length is sane */
	if(vmspace_is_inuse(vs, virt, len))
		return ANANAS_ERROR(NO_SPACE);
	if (len == 0)
		return ANANAS_ERROR(BAD_LENGTH);

	vmarea_t* va = kmalloc(sizeof(*va));
	memset(va, 0, sizeof(*va));

	/*
	 * XXX We should ask the VM for some kind of reservation of the
	 * THREAD_MAP_ALLOC flag is set; now we'll just assume that the
	 * memory is there...
	 */
	DQUEUE_INIT(&va->va_pages);
	va->va_virt = virt;
	va->va_len = len;
	va->va_flags = flags;
	DQUEUE_ADD_TAIL(&vs->vs_areas, va);
	TRACE(VM, INFO, "vmspace_mapto(): vs=%p, va=%p, phys=%p, virt=%p, flags=0x%x", vs, va, phys, virt, flags);
	*va_out = va;

	/* Provide a mapping for the pages */
	md_map_pages(vs, va->va_virt, phys, BYTES_TO_PAGES(len), (flags & (VM_FLAG_LAZY | VM_FLAG_ALLOC)) ? 0 : flags);
	return ANANAS_ERROR_OK;
}

errorcode_t
vmspace_map(vmspace_t* vs, addr_t phys, size_t len /* bytes */, uint32_t flags, vmarea_t** va_out)
{
	/*
	 * Locate a new address to map to; we currently never re-use old addresses.
	 */
	addr_t virt = vs->vs_next_mapping;
	vs->vs_next_mapping += len;
	if ((vs->vs_next_mapping & (PAGE_SIZE - 1)) > 0)
		vs->vs_next_mapping += PAGE_SIZE - (vs->vs_next_mapping & (PAGE_SIZE - 1));
	return vmspace_mapto(vs, virt, phys, len, flags, va_out);
}

errorcode_t
vmspace_area_resize(vmspace_t* vs, vmarea_t* va, size_t new_length /* in bytes */)
{
	/* XXX we should lock the mapping here! */
	if (new_length == 0)
		return ANANAS_ERROR(BAD_LENGTH);

	/* If we're mapping a large piece, the new virtual space must not be in use */
	if(new_length > va->va_len) {
		addr_t new_virt = va->va_virt + new_length - va->va_len;
		size_t grow_length = new_length - va->va_len;
		if(vmspace_is_inuse(vs, new_virt, grow_length))
			return ANANAS_ERROR(NO_SPACE);

		/* XXX If this isn't a ALLOC mapping, reject - we don't know the physical address anymore */
		if ((va->va_flags & VM_FLAG_ALLOC) == 0)
			return ANANAS_ERROR(BAD_FLAG);

		/* Extend the mapping */
		md_map_pages(vs, new_virt, 0, BYTES_TO_PAGES(grow_length), (va->va_flags & (VM_FLAG_LAZY | VM_FLAG_ALLOC)) ? 0 : va->va_flags);
	}

	/* If we're shrinking the mapping, free all pages that are no longer in use */
	if (new_length < va->va_len) {
		addr_t free_virt_begin = va->va_virt + new_length;
		addr_t free_virt_end = va->va_virt + va->va_len;
		DQUEUE_FOREACH_SAFE(&va->va_pages, p, struct PAGE) {
			if (p->p_addr < free_virt_begin || p->p_addr >= free_virt_end)
				continue;
			DQUEUE_REMOVE(&va->va_pages, p);
			page_free(p);
		}

		/* Shrink the mapping */
		md_unmap_pages(vs, free_virt_begin, free_virt_end - free_virt_begin);
	}

	va->va_len = new_length;
	return ANANAS_ERROR_OK;
}

errorcode_t
vmspace_handle_fault(vmspace_t* vs, addr_t virt, int flags)
{
	TRACE(VM, INFO, "vmspace_handle_fault(): vs=%p, virt=%p, flags=0x%x", vs, virt, flags);

	/* Walk through the areas one by one */
	if (!DQUEUE_EMPTY(&vs->vs_areas)) {
		DQUEUE_FOREACH(&vs->vs_areas, va, vmarea_t) {
			if (!(virt >= va->va_virt && (virt < (va->va_virt + va->va_len))))
				continue;

			/* We should only get faults for lazy areas (filled by a function) or when we have to dynamically allocate things */
			KASSERT((va->va_flags & (VM_FLAG_ALLOC | VM_FLAG_LAZY)) != 0, "unexpected pagefault in area %p, virt=%p, len=%d, flags 0x%x", va, va->va_virt, va->va_len, va->va_flags);

			/* Allocate a new page; this will be used to handle the fault */
			struct PAGE* p = page_alloc_single();
			if (p == NULL)
				return ANANAS_ERROR(OUT_OF_MEMORY);
			DQUEUE_ADD_TAIL(&va->va_pages, p);
			p->p_addr = virt & ~(PAGE_SIZE - 1);

			/* Map the page */
			md_map_pages(vs, p->p_addr, page_get_paddr(p), 1, va->va_flags);
			errorcode_t err = ANANAS_ERROR_OK;
			if (va->va_fault != NULL) {
				/* Invoke the mapping-specific fault handler */
				err = va->va_fault(vs, va, virt);
				if (err != ANANAS_ERROR_NONE) {
					/* Mapping failed; throw the thread mapping away and nuke the page */
					md_unmap_pages(vs, p->p_addr, 1);
					DQUEUE_REMOVE(&va->va_pages, p);
					page_free(p);
				}
			}
			return err;
		}
	}

	return ANANAS_ERROR(BAD_ADDRESS);
}

errorcode_t
vmspace_clone(vmspace_t* vs_source, vmspace_t* vs_dest)
{
	TRACE(VM, INFO, "vmspace_clone(): source=%p dest=%p", vs_source, vs_dest);

	if(DQUEUE_EMPTY(&vs_source->vs_areas))
		return ANANAS_ERROR_OK;

	DQUEUE_FOREACH(&vs_source->vs_areas, va_src, vmarea_t) {
		if (va_src->va_flags & VM_FLAG_PRIVATE)
			continue;

		vmarea_t* va_dst;
		errorcode_t err = vmspace_mapto(vs_dest, va_src->va_virt, 0, va_src->va_len, VM_FLAG_ALLOC | va_src->va_flags, &va_dst);
		ANANAS_ERROR_RETURN(err);

		/* Copy the mapping-specific parts */
		va_dst->va_privdata = NULL; /* to be filled out by clone */
		va_dst->va_fault = va_src->va_fault;
		va_dst->va_destroy = va_src->va_destroy;
		va_dst->va_clone = va_src->va_clone;
		if (va_src->va_clone != NULL) {
			err = va_src->va_clone(vs_source, va_src, vs_dest, va_dst);
			ANANAS_ERROR_RETURN(err);
		}

		/*
		 * Copy the area page-wise - XXX we should use a copy-on-write mechanism.
		 *
		 * This loop assumes that our current vmspace is the source!
		 */
		if (!DQUEUE_EMPTY(&va_src->va_pages)) {
			DQUEUE_FOREACH(&va_src->va_pages, p, struct PAGE) {
				struct PAGE* new_page = page_alloc_order(p->p_order);
				if (new_page == NULL)
					return ANANAS_ERROR(OUT_OF_MEMORY);
				DQUEUE_ADD_TAIL(&va_dst->va_pages, new_page);
				new_page->p_addr = p->p_addr;
				int num_pages = 1 << p->p_order;

				/* XXX make a temporary mapping to copy the data. We should do a copy-on-write */
				void* ktmp = kmem_map(page_get_paddr(new_page), num_pages * PAGE_SIZE, VM_FLAG_READ | VM_FLAG_WRITE | VM_FLAG_KERNEL);
				memcpy(ktmp, (void*)p->p_addr, num_pages * PAGE_SIZE);
				kmem_unmap(ktmp, num_pages * PAGE_SIZE);
				
				/* Mark the page as present in the cloned process */
				md_map_pages(vs_dest, p->p_addr, page_get_paddr(new_page), num_pages, va_dst->va_flags);
			}
		}
	}

	return ANANAS_ERROR_OK;
}

void
vmspace_area_free(vmspace_t* vs, vmarea_t* va)
{
	DQUEUE_REMOVE(&vs->vs_areas, va);
	if (va->va_destroy != NULL)
		va->va_destroy(vs, va);

	/* If the pages were allocated, we need to free them one by one */
	if (!DQUEUE_EMPTY(&va->va_pages))
		DQUEUE_FOREACH_SAFE(&va->va_pages, p, struct PAGE) {
			page_free(p);
		}
	kfree(va);
}
