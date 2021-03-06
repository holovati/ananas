/* $Id: remove.c 366 2009-09-13 15:14:02Z solar $ */

/* remove( const char * )

   This file is part of the Public Domain C Library (PDCLib).
   Permission is granted to use, modify, and / or redistribute at will.
*/

/* This is an example implementation of remove() fit for use with POSIX kernels.
*/

#include <stdio.h>

#ifndef REGTEST

extern int unlink( const char * pathname );

int remove( const char * pathname )
{
    int rc;
    /* XXX ananas: do not support unlink yet */
    rc = -1;
#if 0
    if ( ( rc = unlink( pathname ) ) == -1 )
    {
        switch ( errno )
        {
            /* These are the values possible on a Linux machine. Adapt the
               values and their mapping to PDCLib errno values at will. (This
               is an example implementation, so we keep it very simple.)
            */
            case EACCES:
            case EFAULT:
            case EIO:
            case EISDIR:
            case ELOOP:
            case ENAMETOOLONG:
            case ENOENT:
            case ENOMEM:
            case ENOTDIR:
            case EPERM:
            case EROFS:
                _PDCLIB_errno = _PDCLIB_EIO;
                break;
            default:
                _PDCLIB_errno = _PDCLIB_EUNKNOWN;
                break;
        }
    }
#endif
    return rc;
}

#endif

#ifdef TEST
#include <_PDCLIB/_PDCLIB_test.h>

int main( void )
{
    /* Testing covered by ftell.c (and several others) */
    return TEST_RESULTS;
}

#endif

