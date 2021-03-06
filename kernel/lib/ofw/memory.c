/* Implements commands as described in 6.3.2.4: memory */
#include <ananas/lib.h>
#include <ofw.h>

void*
ofw_claim(ofw_cell_t virt, ofw_cell_t size, ofw_cell_t align)
{
	struct {
		ofw_cell_t	service;
		ofw_cell_t	n_args;
		ofw_cell_t	n_returns;
		ofw_cell_t	virt;
		ofw_cell_t	size;
		ofw_cell_t	align;
		ofw_cell_t	baseaddr;
	} args = {
		.service = (ofw_cell_t)"claim",
		.n_args    = 3,
		.n_returns = 1,
	};

	args.virt = virt;
	args.size = size;
	args.align = align;
	if (ofw_call(&args) == -1)
		return (void*)-1;
	return (void*)args.baseaddr;
}

void
ofw_release(ofw_cell_t virt, ofw_cell_t size)
{
	struct {
		ofw_cell_t	service;
		ofw_cell_t	n_args;
		ofw_cell_t	n_returns;
		ofw_cell_t	virt;
		ofw_cell_t	size;
	} args = {
		.service = (ofw_cell_t)"claim",
		.n_args    = 2,
		.n_returns = 0,
	};

	args.virt = virt;
	args.size = size;
	ofw_call(&args);
	
}

/* vim:set ts=2 sw=2: */
