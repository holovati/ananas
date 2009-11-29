#ifndef __AMD64_MACRO_H__
#define __AMD64_MACRO_H__

/*
 * Sets up a GDT entry, either for code or data. Note that these entries only
 * take up 8 bytes.
 */
#define GDT_SET_ENTRY64(ptr, offs, dpl, iscode) do { \
	uint8_t* p = ((uint8_t*)ptr + offs); \
	/* Segment Limit 0:15 (ignored) */ \
	p[ 0] = 0; p[1] = 0; \
	/* Base Address 0:15 (ignored) */ \
	p[ 2] = 0; p[3] = 0; \
	/* Base Address 16:23 (ignored) */ \
	p[ 4] = 0; \
	/* Writable 9 [1], Conforming 10, Code 11, Must be set 12, DPL 13:14 (ignored), Present 15 */ \
	p[ 5] = (((iscode) ? 0 : 1) << 1) | ((iscode) << 3) | (1 << 4) | ((dpl) << 5) | (1 << 7); \
	/* Segment limit 16:19, AVL 20, Long 21, D/B 22, Granulatity 23 (all ignored) */ \
	p[ 6] = (1 << 5); \
	/* Base address 24:31 (ignored) */ \
	p[ 7] = 0; \
} while(0)

/*
 * [1] The AMD64 Architecture Programmers Manual Volume 2: System Programming
 *     says the Writable-bit is ignored for 64-bit segments. However, at least
 *     Bochs 2.4.1 will not allow the descriptor to be used for %ss unless its
 *     set...
 */
#define GDT_SET_CODE64(ptr, offs, dpl) GDT_SET_ENTRY64(ptr, offs, dpl, 1)
#define GDT_SET_DATA64(ptr, offs, dpl) GDT_SET_ENTRY64(ptr, offs, dpl, 0)

/*
 * Sets up a GDT entry for a TSS. Note that this entry takes up 16 bytes.
 */
#define GDT_SET_TSS64(ptr, offs, dpl, base, size) do { \
	uint8_t* p = ((uint8_t*)ptr + offs); \
	/* Segment Limit 0:15 */ \
	p[ 0] = (size) & 0xff; p[1] = ((size) >> 8) & 0xff; \
	/* Base Address 0:15 */ \
	p[ 2] = (base) & 0xff; p[3] = ((base) >> 8) & 0xff; \
	/* Base Address 16:23 */ \
	p[ 4] = ((base) >> 16) & 0xff; \
	/* Type 8:11, DPL 13:14, Present 15 */ \
	p[ 5] = 9 | (dpl << 5) | (1 << 7); \
	/* Segment Limit 16:19, Available 20, Granularity 23 */ \
	p[ 6] = ((size) >> 16) & 0x7; \
	/* Base Address 24:31 */ \
	p[ 7] = ((base) >> 24) & 0xff; \
	/* Base Address 32:63 */ \
	p[ 8] = ((base) >> 32) & 0xff; p[ 9] = ((base) >> 40) & 0xff; \
	p[10] = ((base) >> 48) & 0xff; p[11] = ((base) >> 56) & 0xff; \
	/* Reserved */ \
	p[12] = 0; p[13] = 0; p[14] = 0; p[15] = 0; \
} while (0)

/*
 * Convenience macro to create a XXXr 6-byte 'limit, base' variable
 * which are needed for LGDT and LIDT.
 */
#define MAKE_RREGISTER(name, x, size) \
	uint8_t name[10]; \
	do { \
		name[0] = (size) & 0xff; \
		name[1] = (size) >> 8; \
		name[2] = ((addr_t)(x)      ) & 0xff; \
		name[3] = ((addr_t)(x) >>  8) & 0xff; \
		name[4] = ((addr_t)(x) >> 16) & 0xff; \
		name[5] = ((addr_t)(x) >> 24) & 0xff; \
		name[6] = ((addr_t)(x) >> 32) & 0xff; \
		name[7] = ((addr_t)(x) >> 40) & 0xff; \
		name[8] = ((addr_t)(x) >> 48) & 0xff; \
		name[9] = ((addr_t)(x) >> 56) & 0xff; \
	} while (0);

#define IDT_SET_ENTRY(ptr, num, dpl, sel, handler) do { \
	extern void* handler; \
	uint8_t* p = ((uint8_t*)ptr + num * 16); \
	/* Target Offset 0:15 */ \
	p[ 0] = (((addr_t)&handler)     ) & 0xff; \
	p[ 1] = (((addr_t)&handler) >> 8) & 0xff; \
	/* Target selector */ \
	p[ 2] = (sel) & 0xff; p[3] = ((sel) >> 8) & 0xff; \
	/* IST 0:2 */ \
	p[ 4] = 0; \
	/* Type 8:11, DPL 13:14, Present 15 */ \
	p[5] = 0xe | ((dpl) << 5) | (1 << 7); \
	/* Target offset 16:31 */ \
	p[ 6] = (((addr_t)&handler) >> 16) & 0xff; \
	p[ 7] = (((addr_t)&handler) >> 24) & 0xff; \
	/* Target offset 32:63 */ \
	p[ 8] = (((addr_t)&handler) >> 32) & 0xff; \
	p[ 9] = (((addr_t)&handler) >> 40) & 0xff; \
	p[10] = (((addr_t)&handler) >> 48) & 0xff; \
	p[11] = (((addr_t)&handler) >> 56) & 0xff; \
	/* Reserved */ \
	p[12] = 0; p[13] = 0; p[14] = 0; p[15] = 0; \
} while (0)

#endif /* __AMD64_MACRO_H__ */