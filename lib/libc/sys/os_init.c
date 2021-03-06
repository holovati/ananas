#include <ananas/threadinfo.h>
#include <_posix/init.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct THREADINFO* libc_threadinfo;
int    libc_argc = 0;
char** libc_argv = NULL;
char** environ = NULL;

extern void _init();
extern void _fini();

static void
libc_initialize_arg(const char* arg, char*** dest, int* count)
{
	/* Count the number of arguments */
	int num_args = 0;
	const char* cur_ptr = arg;
	while (1) {
		const char* ptr = strchr(cur_ptr, '\0');
		if (ptr == cur_ptr)
			break;
		cur_ptr = ptr + 1;
		num_args++;
	}

	/* Copy the argument pointers in place */
	*dest = malloc(sizeof(const char*) * (num_args + 1 /* terminating NULL */));
	int cur_arg = 0;
	cur_ptr = arg;
	while (1) {
		(*dest)[cur_arg] = (char*)cur_ptr;
		const char* ptr = strchr(cur_ptr, '\0');
		if (ptr == cur_ptr)
			break;
		cur_ptr = ptr + 1;
		cur_arg++;
	}
	(*dest)[cur_arg] = NULL;
	assert(cur_arg == num_args);
	if (count != NULL)
		*count = num_args;
}

void
libc_reinit_environ()
{
	if (environ != NULL)
		free(environ);
	libc_initialize_arg(libc_threadinfo->ti_env, &environ, NULL);
}

void
libc_init(struct THREADINFO* ti)
{
	libc_threadinfo = ti;

	/* Initialize argument and environment variables */
	libc_initialize_arg(ti->ti_args, &libc_argv, &libc_argc);
	libc_reinit_environ();

	/* Run .init functions as emitted by the compiler */
	_init();

	/* And schedule the destructor stuff at the end */
	atexit(_fini);
}

/* vim:set ts=2 sw=2: */
