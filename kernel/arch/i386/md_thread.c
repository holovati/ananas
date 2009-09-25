#include "i386/thread.h"
#include "i386/vm.h"
#include "i386/macro.h"
#include "lib.h"
#include "mm.h"
#include "param.h"
#include "thread.h"

extern void *gdt;
extern struct CONTEXT* currentctx; /* XXX make me per-cpu */

void md_restore_ctx(struct CONTEXT* ctx);

static void
thread_code()
{
	while (1) {
		kprintf("hi, this is %x\n", currentctx);
		/* force a switch! */
		__asm("int $0x90");
	}
}

int
md_thread_init(thread_t thread)
{
	/* Note that this function relies on thread->md being zero-filled before calling */
	struct MD_THREAD* md = (struct MD_THREAD*)thread->md;

	/* Allocate a stack */
	md->stack = kmalloc(THREAD_STACK_SIZE);

	/* Create a pagedirectory and map the kernel pages in there */
	md->pagedir = kmalloc(PAGE_SIZE);
	memset(md->pagedir, 0, PAGE_SIZE);
	vm_map_kernel_addr(md->pagedir);

	memcpy(md->pagedir, pagedir, PAGE_SIZE); /* HACK */

	/* XXX Debugging! */
	md->ctx.eax = 0x12345679;
	md->ctx.ebx = 0x0f00c0de;
	md->ctx.ecx = 0xdeadbabe;
	md->ctx.edx = 0xcafeb00b;
	md->ctx.esi = 0xfaabbeef;
	md->ctx.edi = 0x87654321;
	md->ctx.esp = (addr_t)md->stack;
	md->ctx.eip = (addr_t)&thread_code;

	/* Fill out the Task State Segment */
	md->ctx.cs = GDT_IDX_KERNEL_CODE * 8;
	md->ctx.ds = GDT_IDX_KERNEL_DATA * 8;
	md->ctx.es = GDT_IDX_KERNEL_DATA * 8;
	md->ctx.ss = GDT_IDX_KERNEL_DATA * 8;
	md->ctx.cr3 = (addr_t)md->pagedir;

	return 1;
}

size_t
md_thread_get_privdata_length()
{
	return sizeof(struct MD_THREAD);
}

void
md_thread_destroy(thread_t thread)
{
	struct MD_THREAD* md = (struct MD_THREAD*)thread->md;

	kfree(md->pagedir);
	kfree(md->stack);
}

void
md_thread_switch(thread_t thread)
{
	struct MD_THREAD* md = (struct MD_THREAD*)thread->md;

	currentctx = (struct CONTEXT*)&md->ctx;
	md_restore_ctx(currentctx);
}

/* vim:set ts=2 sw=2: */