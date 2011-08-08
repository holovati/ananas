#include <ananas/types.h>
#include <machine/param.h>
#include <machine/vm.h>
#include <machine/interrupts.h>
#include <machine/macro.h>
#include <machine/thread.h>
#include <machine/pcpu.h>
#include <ananas/x86/io.h>
#include <ananas/x86/pic.h>
#include <ananas/x86/pit.h>
#include <ananas/x86/smap.h>
#include <ananas/bootinfo.h>
#include <ananas/handle.h>
#include <ananas/init.h>
#include <ananas/lib.h>
#include <ananas/pcpu.h>
#include <ananas/mm.h>
#include <ananas/vm.h>
#include <loader/module.h>
#include "options.h"

/* __end is defined by the linker script and indicates the end of the kernel */
extern void* __end;

/* __entry is the entry position and the first symbol in the kernel */
extern void* __entry;

/* Pointer to the kernel page directory */
uint32_t* kernel_pd;

/* Global/Interrupt descriptor tables, as preallocated by stub.s */
extern void *idt, *gdt;

/* Initial TSS used by the kernel */
struct TSS kernel_tss;

/* Boot CPU pcpu structure */
static struct PCPU bsp_pcpu;

/* Bootinfo as supplied by the loader, or NULL */
struct BOOTINFO* bootinfo = NULL;
static struct BOOTINFO _bootinfo;

/* CPU clock speed, in MHz */
int md_cpu_clock_mhz = 0;

void
md_remove_low_mappings()
{
	for (int i = (addr_t)&__entry - KERNBASE; i < (addr_t)&__end - KERNBASE; i += (1 << VM_SHIFT_PT)) {
		kernel_pd[i >> VM_SHIFT_PT] = 0;
	}
}

/*
 * i386-dependant startup code, called by stub.s.
 */
void
md_startup(struct BOOTINFO* bootinfo_ptr)
{
	/*
	 * As long as we do not have any paging, we can access any
	 * memory location we want - this is handy while we are copying
	 * and validating the loader-supplied bootinfo.
	 */	
	if (bootinfo_ptr != NULL) {
		/*
		 * Copy enough bytes to cover our bootinfo - but only activate the bootinfo
	   * if the size is correct.
		 */
		memcpy((void*)((addr_t)&_bootinfo - KERNBASE), bootinfo_ptr, sizeof(struct BOOTINFO));
		if (bootinfo_ptr->bi_size >= sizeof(struct BOOTINFO))
			*(uint32_t*)((addr_t)&bootinfo - KERNBASE) = (addr_t)&_bootinfo;
		else
			bootinfo_ptr = NULL;
	}

	/*
	 * Once this is called, we don't have any paging set up yet. This means that
	 * we can't access any variables and/or data without substracting KERNBASE.
	 *
	 * First of all, we figure out where the kernel ends and thus space is
	 * available; this information is conveniently stored by the loader. Note
	 * that we're still running unpaged, so we use bootinfo_ptr.
	 */
	addr_t availptr = (addr_t)&__end - KERNBASE;
	if (bootinfo_ptr != NULL) {
		availptr = (addr_t)((struct LOADER_MODULE*)bootinfo_ptr->bi_modules)->mod_phys_end_addr;
		availptr = (availptr & ~(PAGE_SIZE - 1)) + PAGE_SIZE;
	}

	/*
	 * Paging on i386 works by having a 'page directory' (PD), which consists of
	 * 1024 pointers to a 'page table' (PT). While resolving any 32-bit address
	 * (with bits ordered from highest 31 to 0), the top 10 bits (22-31) refer to
	 * which page directory to use, while the next 10 bits (12-21) refer to the
	 * page table to use and finally, the lower 12 bits (11-0) are the address
	 * itself.
	 *
	 * We place the PD directly after the kernel itself, and follow this by any
	 * PT's we need to map the virtual kernel addresses.
	 */
	uint32_t* pd = (uint32_t*)availptr;
	availptr += PAGE_SIZE;
	memset((void*)pd, 0, PAGE_SIZE);

	/* Allocate all kernel page tables and clear them */
	uint32_t* pt = (uint32_t*)availptr;
	availptr += VM_NUM_KERNEL_PTS * PAGE_SIZE;
	memset(pt, 0, VM_NUM_KERNEL_PTS * PAGE_SIZE);

	/* Hook the kernel page tables to the kernel page directory */
	addr_t cur_pt = (addr_t)pt;
	for (unsigned int n = 0; n < VM_NUM_KERNEL_PTS; n++, cur_pt += PAGE_SIZE) {
		pd[VM_FIRST_KERNEL_PT + n] = cur_pt | PDE_P | PTE_RW;
	}

	/*
	 * Now, walk through the memory occupied by the kernel and map the
	 * pages. Note that we'll want to map the pagetable entries as well
	 * which we've placed after the kernel, so we'll need to map up to
	 * availptr.
	 */
	for (addr_t i = (addr_t)&__entry; i < (availptr | KERNBASE); i += PAGE_SIZE) {
		/*
		 * Determine the index within the page table; this is simply bits
	 	 * 12-21, so we need to shift the lower 12 bits away and throw every
	 	 * above bit 10 away.
		 */
		uint32_t* pt = (uint32_t*)(pd[i >> VM_SHIFT_PT] & ~(PAGE_SIZE - 1));
		KASSERT(pt != NULL, "kernel pt not mapped");
		pt[(((i >> VM_SHIFT_PTE) & ((1 << 10) - 1)))] = (i - KERNBASE) | PTE_P | PTE_RW;
	}

	/*
	 * OK, the page table we have constructed should be valid for the kernel's
	 * virtual addresses. However, if we would activate this table, the
	 * system would immediately crash because the address we are executing is
	 * no longer mapped!
	 *
	 * Resolve this by duplicating the PD mappings to KERNBASE - __end to the
	 * mappings minus KERNBASE. No need to map the pagetable parts.
	 */
	for (addr_t i = (addr_t)&__entry - KERNBASE; i < (addr_t)&__end - KERNBASE; i += (1 << VM_SHIFT_PT)) {
		pd[i >> VM_SHIFT_PT] = pd[(i + KERNBASE) >> VM_SHIFT_PT];
	}

	/* It's time to... throw the switch! */
	__asm(
		/* Set CR3 to the physical address of the page directory */
		"movl	%%eax, %%cr3\n"
		"jmp	l1\n"
"l1:\n"
		/* Enable paging by flipping the PG bit in CR0 */
		"movl	%%cr0, %%eax\n"
		"orl	$0x80000000, %%eax\n"
		"movl	%%eax, %%cr0\n"
		"jmp	l2\n"
"l2:\n"
		/* Finally, jump to our virtual address */
		"movl	$l3, %%eax\n"
		"push	%%eax\n"
		"ret\n"
"l3:\n"
		/* Update ESP/EBP to use the new virtual addresses */
		"addl	%%ebx, %%esp\n"
		"addl	%%ebx, %%ebp\n"
	: : "a" (pd), "b" (KERNBASE));

	/*
	 * Paging has been setup; this means we can sensibly use kernel memory now.
	 */
	kernel_pd = (uint32_t*)((addr_t)pd | KERNBASE);

#ifndef SMP
	/*
	 * We can throw the duplicate mappings away now, since we are now using our
	 * virtual mappings...
	 *
	 * ...unless we're using SMP, because the AP code requires an identity
	 * mapping so that paging can be enabled - we must delay until the AP's are
	 * launched.
	 */
	md_remove_low_mappings();
#endif

	__asm(
		/* Reload the page directory */
		"movl	%%eax, %%cr3\n"
		"jmp	l4\n"
"l4:\n"
	: : "a" (pd));

	/*
	 * The next step is to set up the Global Descriptor Table (GDT); this is
	 * used to distinguish between kernel and userland code, as well as being able
	 * to switch between tasks.
	 */

	/*
	 * Prepare the GDT entries; this is just kernel/user code/data - protection between
	 * tasks is handeled using paging.
	 */
	memset(&gdt, 0, GDT_NUM_ENTRIES * 8);
	addr_t bsp_addr = (addr_t)&bsp_pcpu;
	GDT_SET_ENTRY32(&gdt, GDT_IDX_KERNEL_CODE,   SEG_TYPE_CODE, SEG_DPL_SUPERVISOR, 0, 0xfffff);
	GDT_SET_ENTRY32(&gdt, GDT_IDX_KERNEL_DATA,   SEG_TYPE_DATA, SEG_DPL_SUPERVISOR, 0, 0xfffff);
	GDT_SET_ENTRY32(&gdt, GDT_IDX_KERNEL_PCPU,   SEG_TYPE_DATA, SEG_DPL_SUPERVISOR, bsp_addr, sizeof(struct PCPU));
	GDT_SET_ENTRY32(&gdt, GDT_IDX_USER_CODE,     SEG_TYPE_CODE, SEG_DPL_USER,       0, 0xfffff);
	GDT_SET_ENTRY32(&gdt, GDT_IDX_USER_DATA,     SEG_TYPE_DATA, SEG_DPL_USER,       0, 0xfffff);
	GDT_SET_TSS(&gdt, GDT_IDX_KERNEL_TASK, 0, (addr_t)&kernel_tss, sizeof(struct TSS));

	MAKE_RREGISTER(gdtr, &gdt, GDT_NUM_ENTRIES);

	/* Load the GDT, and reload our registers */
	__asm(
		"lgdt (%%eax)\n"
		"mov %%bx, %%ds\n"
		"mov %%bx, %%es\n"
		"mov %%dx, %%fs\n"
		"mov %%bx, %%gs\n"
		"pushl %%ecx\n"
		"pushl $l5\n"
		"lret\n"		/* retf */
"l5:\n"
	: : "a" (&gdtr),
 	    "b" (GDT_SEL_KERNEL_DATA),
	    "c" (GDT_SEL_KERNEL_CODE),
	    "d" (GDT_SEL_KERNEL_PCPU));

	/*
	 * Remap the interrupts; by default, IRQ 0-7 are mapped to interrupts 0x08 -
	 * 0x0f and IRQ 8-15 to 0x70 - 0x77. We remap IRQ 0-15 to 0x20-0x2f (since
	 * 0..0x1f is reserved by Intel).
	 */
	x86_pic_remap();

	/*
	 * Prepare the IDT entries; this means mapping the exception interrupts to
	 * handlers. Note that trap gates (SEG_TGATE_TYPE) do not alter the interrupt
	 * flag, while interrupt gates (SEG_IGATE_TYPE) disable the interrupt flag
	 * for the duration of the fault.
	 */
	memset(&idt, 0, IDT_NUM_ENTRIES * 8);
	IDT_SET_ENTRY(0x00, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, exception0);
	IDT_SET_ENTRY(0x01, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, exception1);
	IDT_SET_ENTRY(0x02, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, exception2);
	IDT_SET_ENTRY(0x03, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, exception3);
	IDT_SET_ENTRY(0x04, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, exception4);
	IDT_SET_ENTRY(0x05, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, exception5);
	IDT_SET_ENTRY(0x06, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, exception6);
	IDT_SET_ENTRY(0x07, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, exception7);
	/* Double fault must disable interrupts as well to increase odds of it running till the end */
	IDT_SET_ENTRY(0x08, SEG_IGATE_TYPE, SEG_DPL_SUPERVISOR, exception8);
	IDT_SET_ENTRY(0x09, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, exception9);
	IDT_SET_ENTRY(0x0a, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, exception10);
	IDT_SET_ENTRY(0x0b, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, exception11);
	IDT_SET_ENTRY(0x0c, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, exception12);
	IDT_SET_ENTRY(0x0d, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, exception13);
	/*
	 * Page fault must disable interrupts to ensure %cr2 (fault address) will not
	 * be overwritten; it will re-enable the interrupt flag when it's safe to do
	 * so.
	 */
	IDT_SET_ENTRY(0x0e, SEG_IGATE_TYPE, SEG_DPL_SUPERVISOR, exception14);
	IDT_SET_ENTRY(0x10, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, exception16);
	IDT_SET_ENTRY(0x11, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, exception17);
	IDT_SET_ENTRY(0x12, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, exception18);
	IDT_SET_ENTRY(0x13, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, exception19);
	IDT_SET_ENTRY(0x20, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, scheduler_irq);
	IDT_SET_ENTRY(0x21, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, irq1);
	IDT_SET_ENTRY(0x22, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, irq2);
	IDT_SET_ENTRY(0x23, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, irq3);
	IDT_SET_ENTRY(0x24, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, irq4);
	IDT_SET_ENTRY(0x25, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, irq5);
	IDT_SET_ENTRY(0x26, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, irq6);
	IDT_SET_ENTRY(0x27, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, irq7);
	IDT_SET_ENTRY(0x28, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, irq8);
	IDT_SET_ENTRY(0x29, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, irq9);
	IDT_SET_ENTRY(0x2a, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, irq10);
	IDT_SET_ENTRY(0x2b, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, irq11);
	IDT_SET_ENTRY(0x2c, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, irq12);
	IDT_SET_ENTRY(0x2d, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, irq13);
	IDT_SET_ENTRY(0x2e, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, irq14);
	IDT_SET_ENTRY(0x2f, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, irq15);

	/*
	 * Register the system call as a trap gate; this means it can be interrupted by
	 * interrupt routines, which means our syscalls are preemptiple.
	 */
	IDT_SET_ENTRY(SYSCALL_INT, SEG_TGATE_TYPE, SEG_DPL_USER, syscall_int);

#ifdef SMP
	IDT_SET_ENTRY(0xff, SEG_TGATE_TYPE, SEG_DPL_SUPERVISOR, spurious_irq);
#endif

	MAKE_RREGISTER(idtr, &idt, IDT_NUM_ENTRIES);

	/* Load the IDT */
	__asm(
		"lidt (%%eax)\n"
	: : "a" (&idtr));

	/*
	 * Load the kernel TSS - we need this once we are going to transition between
	 * ring 0 and 3 code, as it tells the CPU where the necessary stacks are
	 * located. Note that we don't fill out the esp0 part, because this is
	 * different per-thread - md_thread_witch() will deal with it.
	 */
	memset(&kernel_tss, 0, sizeof(struct TSS));
	kernel_tss.ss0 = GDT_SEL_KERNEL_DATA;
	__asm(
		"ltr %%ax\n"
	: : "a" (GDT_SEL_KERNEL_TASK));

	/*
	 * Initialize the FPU; this consists of loading it with a known control
	 * word, and requesting exception handling using the more modern #MF
	 * exception mechanism.
	 *
 	 * XXX What does this do without a FPU?
	 */
	uint16_t cw = 0x37f;
	__asm("fldcw %0; finit" : : "m" (cw));
	__asm(
		"movl	%%cr0, %%eax\n"
		"orl	%%ebx, %%eax\n"
		"movl	%%eax, %%cr0\n"
	: : "b" (CR0_NE));

	/* Ensure the memory manager is available for action */
	mm_init();

	/*
	 * We are good to go; we now need to add the chunks of memory that are
	 * present in the system. However, we cannot obtain them as this is only
	 * possible within realmode. To prevent this mess, the loader obtains this
	 * information for us.
	 */
	if (bootinfo == NULL || bootinfo->bi_memory_map_addr == 0 ||
	    bootinfo->bi_memory_map_size == 0 ||
	    (bootinfo->bi_memory_map_size % sizeof(struct SMAP_ENTRY)) != 0) {
		/* Loader did not provide a sensible memory map - now what? */
		panic("No memory map!");
	}

	/* Present the chunks of memory to the memory manager */
	int mem_map_pages = (bootinfo->bi_memory_map_size + PAGE_SIZE - 1) / PAGE_SIZE;
	void* memory_map = vm_map_kernel((addr_t)bootinfo->bi_memory_map_addr, mem_map_pages, VM_FLAG_READ | VM_FLAG_WRITE | VM_FLAG_KERNEL);
	struct SMAP_ENTRY* smap_entry = memory_map;
	for (int i = 0; i < bootinfo->bi_memory_map_size / sizeof(struct SMAP_ENTRY); i++, smap_entry++) {
		if (smap_entry->type != SMAP_TYPE_MEMORY)
			continue;

		/* This piece of memory is available; add it */
		addr_t base = /* smap_entry->base_hi << 32 | */ smap_entry->base_lo;
		size_t len = /* smap_entry->len_hi << 32 | */ smap_entry->len_lo;
		/* Round base up to a page, and length down a page if needed */
		base  = (base + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
		len  &= ~(PAGE_SIZE - 1);
		mm_zone_add(base, len);
	}
	vm_unmap_kernel((addr_t)memory_map, mem_map_pages);

	/*
	 * Mark the pages occupied by the kernel as used; this prevents the memory
	 * allocator from handing out memory where the kernel lives.
	 *
	 * Note that we always use the first few pages directly after the kernel for
	 * our page directory (these will not be relocated by vm_init()) so be sure
	 * to include them (this is why we use avail and not __end !)
	 */
	size_t kern_pages = ((addr_t)availptr - ((addr_t)&__entry - KERNBASE)) / PAGE_SIZE;
	kmem_mark_used((void*)(addr_t)&__entry - KERNBASE, kern_pages);

	/* Initialize the handles; this is needed by the per-CPU code as it initialize threads */
	handle_init();

	/*
	 * Initialize the per-CPU thread; this needs a working memory allocator, so that is why
	 * we delay it.
	 */
	memset(&bsp_pcpu, 0, sizeof(bsp_pcpu));
	pcpu_init(&bsp_pcpu);
	bsp_pcpu.tss = (addr_t)&kernel_tss;

	/* Initialize the PIT */
	x86_pit_init();

	/* Enable debug extensions */
	__asm __volatile("movl %cr4, %eax; orl $0x8, %eax; mov %eax, %cr4");

	/*
	 * Enable interrupts. We do this right before the machine-independant code
	 * because it will allow us to rely on interrupts when probing devices etc.
	 *
	 * Note that we explicitely block the scheduler, as this only should be
	 * enabled once we are ready to run userland threads.
	 */
	__asm("sti");

	/* Find out how quick the CPU is; this requires interrupts and will be needed for delay() */
	md_cpu_clock_mhz = x86_pit_calc_cpuspeed_mhz();

	/* All done - it's up to the machine-independant code now */
	mi_startup();
}

/* vim:set ts=2 sw=2: */
