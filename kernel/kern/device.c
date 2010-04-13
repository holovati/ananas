#include <sys/console.h>
#include <sys/device.h>
#include <sys/lib.h>
#include <sys/mm.h>
#include <sys/tty.h>
#include <sys/vm.h>
#include "console.inc"

/*
 * devprobe.inc will be generated by config and just lists all drivers we have
 * to probe.
 */
#include "devprobe.inc"

/*
 * devhints.inc will be generated by config and lists all hints we have.
 */
#include "hints.inc"

static void device_print_attachment(device_t dev);

static device_t corebus = NULL;

/* Note: drv = NULL will be used if the driver isn't yet known! */
device_t
device_alloc(device_t bus, driver_t drv)
{
	device_t dev = kmalloc(sizeof(struct DEVICE));
	memset(dev, 0, sizeof(struct DEVICE));
	dev->driver = drv;
	dev->parent = bus;
	dev->unit = 0; /* XXX */
	dev->biodev = dev; /* by default every driver performs its own I/O */
	if (drv != NULL)
		strcpy(dev->name, drv->name);
	/* hook the device up to the chain */
	if (corebus != NULL) {
		dev->next = corebus->next;
		corebus->next = dev;
	}
	return dev;
}

void
device_free(device_t dev)
{
	kfree(dev);
	/* XXX unhook */
}

int
device_attach_single(device_t dev)
{
	int result;
	driver_t driver = dev->driver;

	if (driver->drv_probe != NULL) {
		/*
		 * This device has a probe function; we must call it to figure out
	 	 * whether the device actually exists or we're about to attach
		 * something out of thin air here...
		 */
		result = driver->drv_probe(dev);
		if (result)
			return result;
	}
	/*
	 * XXX This is a kludge: it prevents us from displaying attach information for drivers
	 * that will be initialize outside the probe tree (such as the console which will be
	 * initialized as soon as possible.
	 */
	if (dev->parent != NULL)
		device_print_attachment(dev);
	if (driver->drv_attach != NULL) {
		result = driver->drv_attach(dev);
		if (result)
			return result;
	}

	return 0;
}

static int
device_resolve_type(char** value)
{
	if (!memcmp(*value, "mem=", 4)) {
		*value += 4;
		return RESTYPE_MEMORY;
	}
	if (!memcmp(*value, "io=", 3)) {
		*value += 3;
		return RESTYPE_IO;
	}
	if (!memcmp(*value, "irq=", 4)) {
		*value += 4;
		return RESTYPE_IRQ;
	}
	return RESTYPE_UNUSED;
}

int
device_add_resource(device_t dev, resource_type_t type, unsigned int base, unsigned int len)
{
	unsigned int curhint;

	for (curhint = 0; curhint < DEVICE_MAX_RESOURCES; curhint++) {
		if (dev->resource[curhint].type != RESTYPE_UNUSED)
			continue;
		dev->resource[curhint].type = type;
		dev->resource[curhint].base = base;
		dev->resource[curhint].length = len;
		return 1;
	}

	return 0;
}

/*
 * Handles retrieving the resources for a device based on a given hint.
 */
int
device_get_resources_byhint(device_t dev, const char* hint, const char** hints)
{
	const char** curhint;

	/* Clear out any current device resources */
	memset(dev->resource, 0, sizeof(struct RESOURCE) * DEVICE_MAX_RESOURCES);
	for (curhint = hints; *curhint != NULL; curhint++) {
		if (strlen(*curhint) < strlen(hint))
			continue;
		if (memcmp(*curhint, hint, strlen(hint)))
			continue;

		/*
		 * We got a resource match; need to figure out the type.
		 */
		char* value = (char*)(*curhint + strlen(hint));
		int type = device_resolve_type(&value);
		if (type == RESTYPE_UNUSED) {
			kprintf("%s: ignoring unparsable resource '%s'\n", dev->name, *curhint);
			continue;
		}
		unsigned long v = strtoul(value, NULL, 0);
		if (!device_add_resource(dev, type, v, 0)) {
			kprintf("%s: skipping resource type 0x%x, too many specified\n", dev->name, type);
			continue;
		}
	}

	return 0;
}

/*
 * Handles retrieving the resources for a bus.device.unit from
 * the hints.
 */
int
device_get_resources(device_t dev, const char** hints)
{
	char tmphint[32 /* XXX */];

	if (dev->parent != NULL)
		sprintf(tmphint, "%s.%s.%u.", dev->parent->name, dev->name, dev->unit);
	else
		sprintf(tmphint, "%s.%u.", dev->name, dev->unit);

	return device_get_resources_byhint(dev, tmphint, hints);
}

static void
device_print_attachment(device_t dev)
{
	KASSERT(dev->parent != NULL, "can't print device which doesn't have a parent bus");
	kprintf("%s%u on %s%u ", dev->name, dev->unit, dev->parent->name, dev->parent->unit);
	int i;
	for (i = 0; i < DEVICE_MAX_RESOURCES; i++) {
		int hex = 0;
		switch (dev->resource[i].type) {
			case RESTYPE_MEMORY: kprintf("memory "); hex = 1; break;
			case RESTYPE_IO: kprintf("io "); hex = 1; break;
			case RESTYPE_IRQ: kprintf("irq "); break;
			case RESTYPE_CHILDNUM: kprintf("child "); break;
			case RESTYPE_PCI_BUS: kprintf("bus "); break;
			case RESTYPE_PCI_DEVICE: kprintf("dev "); break;
			case RESTYPE_PCI_FUNCTION: kprintf("func "); break;
			case RESTYPE_PCI_VENDORID: kprintf("vendor "); hex = 1; break;
			case RESTYPE_PCI_DEVICEID: kprintf("device "); hex = 1; break;
			case RESTYPE_PCI_CLASS: kprintf("class "); break;
			default: continue;
		}
		kprintf(hex ? "0x%x" : "%u", dev->resource[i].base);
		if (dev->resource[i].length > 0)
			kprintf(hex ? "-0x%x" : "-%u", dev->resource[i].base + dev->resource[i].length);
		kprintf(" ");
	}
	kprintf("\n");
}

struct RESOURCE*
device_get_resource(device_t dev, resource_type_t type, int index)
{
	int i;
	for (i = 0; i < DEVICE_MAX_RESOURCES; i++) {
		if (dev->resource[i].type != type)
			continue;
		if (index-- > 0)
			continue;
		return &dev->resource[i];
	}

	return NULL;
}

void*
device_alloc_resource(device_t dev, resource_type_t type, size_t len)
{
	struct RESOURCE* res = device_get_resource(dev, type, 0);
	if (res == NULL)
		/* No such resource! */
		return NULL;

	res->length = len;

	switch(type) {
		case RESTYPE_MEMORY:
			return (void*)vm_map_device(res->base, len);
		case RESTYPE_IO:
		case RESTYPE_CHILDNUM:
		case RESTYPE_IRQ: /* XXX should allocate, not just return */
			return (void*)(uintptr_t)res->base;
		default:
			panic("%s: resource type %u exists, but can't allocate", dev->name, type);
	}

	/* NOTREACHED */
	return NULL;
}

/*
 * Handles attaching devices to a bus; may recursively call itself.
 */
void
device_attach_bus(device_t bus)
{
	for (struct PROBE** p = devprobe; *p != NULL; p++) {
		/* See if the device exists on this bus */
		int exists = 0;
		for (const char** curbus = (*p)->bus; *curbus != NULL; curbus++) {
			if (strcmp(*curbus, bus->name) == 0) {
				exists = 1;
				break;
			}
		}

		if (!exists)
			continue;

		/*
		 * OK, the device may be present on this bus; allocate a device for it.
		 */
		driver_t driver = (*p)->driver;
		KASSERT(driver != NULL, "matched a probe device without a driver!");

		/*
		 * If an input- or output driver was specified, we need to skip attaching
		 * it again; we cheat and claim we attached it, yet we already did a long
		 * time ago ;-)
		 * 
		 */
#ifdef CONSOLE_INPUT_DRIVER
		extern struct DRIVER CONSOLE_INPUT_DRIVER;
		if (driver == &CONSOLE_INPUT_DRIVER) {
			device_t input_dev = tty_get_inputdev(console_tty);
			input_dev->parent = bus;
			device_print_attachment(input_dev);
			continue;
		}
#endif
#ifdef CONSOLE_OUTPUT_DRIVER
		extern struct DRIVER CONSOLE_OUTPUT_DRIVER;
		if (driver == &CONSOLE_OUTPUT_DRIVER) {
			device_t output_dev = tty_get_outputdev(console_tty);
			output_dev->parent = bus;
			device_print_attachment(output_dev);
			continue;
		}
#endif

		device_t dev = device_alloc(bus, driver);
		device_get_resources(dev, config_hints);

		int result = 0;
		if (driver->drv_probe != NULL) {
			/*
			 * This device has a probe function; we must call it to figure out
			 * whether the device actually exists or we're about to attach
			 * something out of thin air here...
			 */
			result = driver->drv_probe(dev);
		}
		if (result == 0) {
			device_print_attachment(dev);
			if (driver->drv_attach != NULL)
				result = driver->drv_attach(dev);
		}
		if (result != 0) {
			device_free(dev);
			continue;
		}

		/* We have a device; attach any children on this bus if needed */
		if (driver->drv_attach_children != NULL)
			driver->drv_attach_children(dev);
		device_attach_bus(dev);
	}
}

void
device_init()
{
	/*
	 * First of all, create the core bus; this is as bare to the metal as it
	 * gets.
	 */
	corebus = (device_t)kmalloc(sizeof(struct DEVICE));
	memset(corebus, 0, sizeof(struct DEVICE));
	strcpy(corebus->name, "corebus");
	device_attach_bus(corebus);
}

void
device_set_biodev(device_t dev, device_t biodev)
{
	KASSERT(biodev->driver != NULL, "block device without a driver");
	KASSERT(biodev->driver->drv_start != NULL, "block device without drv_start");

	dev->biodev = biodev;
}

struct DEVICE*
device_find(const char* name)
{
	char* ptr = (char*)name;
	while (*ptr != '\0' && (*ptr < '0' || *ptr > '9')) ptr++;
	int unit = (*ptr != '\0') ? strtoul(ptr, NULL, 10) : 0;

	for (struct DEVICE* dev = corebus; dev != NULL; dev = dev->next) {
		if (!strncmp(dev->name, name, ptr - name) && dev->unit == unit)
			return dev;
	}
	return NULL;
}

ssize_t
device_write(device_t dev, const char* buf, size_t len)
{
	KASSERT(dev->driver != NULL, "device_write() without a driver");
	KASSERT(dev->driver->drv_write != NULL, "device_write() without drv_write");

	return dev->driver->drv_write(dev, buf, len);
}

ssize_t
device_read(device_t dev, char* buf, size_t len)
{
	KASSERT(dev->driver != NULL, "device_read() without a driver");
	KASSERT(dev->driver->drv_read != NULL, "device_read() without drv_read");

	return dev->driver->drv_read(dev, buf, len);
}

void
device_printf(device_t dev, const char* fmt, ...)
{
	va_list va;

	/* XXX will interleave printf's in SMP */
	kprintf("%s%u: ", dev->name, dev->unit);
	va_start(va, fmt);
	vaprintf(fmt, va);
	va_end(va);
	kprintf("\n");
}

/* vim:set ts=2 sw=2: */
