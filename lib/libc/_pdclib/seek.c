/* $Id: seek.c 393 2010-03-12 11:08:14Z solar $ */

/* int64_t _PDCLIB_seek( FILE *, int64_t, int )

   This file is part of the Public Domain C Library (PDCLib).
   Permission is granted to use, modify, and / or redistribute at will.
*/

#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#ifndef _PDCLIB_GLUE_H
#define _PDCLIB_GLUE_H
#include <_PDCLIB/_PDCLIB_glue.h>
#endif

_PDCLIB_int64_t _PDCLIB_seek( struct _PDCLIB_file_t * stream, _PDCLIB_int64_t offset, int whence )
{
    switch ( whence )
    {
        case SEEK_CUR:
	    /*
	     * Since streams are buffered, we must not blindly hand our offset
	     * to the kernel - we have to take our own position into account
	     * because the kernel will not know how much of the file we have
	     * read within our application.
 	     */
	    whence = SEEK_SET;
	    offset += ftell(stream);
	    break;
        case SEEK_SET:
        case SEEK_END:
	    /*
	     * Our position does not matter since the file end and start are
	     * not influenced by it
	     */
	    break;
        default:
            _PDCLIB_errno = EINVAL;
            return EOF;
            break;
    }
    /* note that lseek() sets errno, so we won't have to */
    _PDCLIB_int64_t rc = lseek( stream->handle, offset, whence );
    if ( rc >= 0 )
    {
        stream->ungetidx = 0;
        stream->bufidx = 0;
        stream->bufend = 0;
        stream->pos.offset = rc;
        return rc;
    }
    return EOF;
}

#ifdef TEST
#include <_PDCLIB/_PDCLIB_test.h>

int main()
{
    /* Testing covered by ftell.c */
    return TEST_RESULTS;
}

#endif

