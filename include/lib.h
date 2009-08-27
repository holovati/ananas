#include "types.h"
#include "stdarg.h"

#ifndef __LIBKERN_H__
#define __LIBKERN_H__

void* memset(void* b, int c, size_t len);
void kprintf(const char* fmt, ...);

#endif /* __LIBKERN_H__ */
