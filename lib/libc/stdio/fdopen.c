/* $Id: fopen.c 366 2009-09-13 15:14:02Z solar $ */

/* fopen( const char *, const char * )

   This file is part of the Public Domain C Library (PDCLib).
   Permission is granted to use, modify, and / or redistribute at will.
*/

#include <stdio.h>
#include <stdlib.h>

#ifndef REGTEST
#include <_PDCLIB/_PDCLIB_glue.h>

extern struct _PDCLIB_file_t * _PDCLIB_filelist;

struct _PDCLIB_file_t * fdopen( _PDCLIB_fd_t fildes, const char * _PDCLIB_restrict mode )
{
    struct _PDCLIB_file_t * rc;
    size_t filename_len;
    if ( mode == NULL || fildes == _PDCLIB_NOHANDLE )
    {
        /* Mode or file descriptor invalid */
        return NULL;
    }
    /* To reduce the number of malloc calls, all data fields are concatenated:
       * the FILE structure itself,
       * ungetc buffer,
       * filename buffer,
       * data buffer.
       Data buffer comes last because it might change in size ( setvbuf() ).
    */
    filename_len = 1;
    if ( ( rc = calloc( 1, sizeof( struct _PDCLIB_file_t ) + _PDCLIB_UNGETCBUFSIZE + filename_len + BUFSIZ ) ) == NULL )
    {
        /* no memory */
        return NULL;
    }
    if ( ( rc->status = _PDCLIB_filemode( mode ) ) == 0 ) 
    {
        /* invalid mode */
        free( rc );
        return NULL;
    }
    rc->handle = fildes; /* XXX how do we error check this? */
    /* Setting pointers into the memory block allocated above */
    rc->ungetbuf = (unsigned char *)rc + sizeof( struct _PDCLIB_file_t );
    rc->filename = (char *)rc->ungetbuf + _PDCLIB_UNGETCBUFSIZE;
    rc->buffer   = rc->filename + filename_len;
    /* Clear filename in FILE structure; we have no way of knowing it */
    rc->filename[0] = '\0';
    /* Initializing the rest of the structure */
    rc->bufsize = BUFSIZ;
    rc->bufidx = 0;
    rc->ungetidx = 0;
    /* Setting buffer to _IOLBF because "when opened, a stream is fully
       buffered if and only if it can be determined not to refer to an
       interactive device."
    */
    rc->status |= _IOLBF;
    /* TODO: Setting mbstate */
    /* Adding to list of open files */
    rc->next = _PDCLIB_filelist;
    _PDCLIB_filelist = rc;
    return rc;
}
#endif
