/*----------------------------------------------------------------------------*/
/* Hobbit monitor library.                                                    */
/*                                                                            */
/* Copyright (C) 2002-2006 Henrik Storner <henrik@storner.dk>                 */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

#ifndef __MATCHING_H__
#define __MATCHING_H__

/* The clients probably dont have the pcre headers */
#if defined(LOCALCLIENT) || !defined(CLIENTONLY)
#include <pcre.h>

extern pcre *compileregex(const char *pattern);
extern pcre *multilineregex(const char *pattern);
extern int matchregex(char *needle, pcre *pcrecode);
extern void freeregex(pcre *pcrecode);
extern int namematch(char *needle, char *haystack, pcre *pcrecode);
extern int timematch(char *tspec);
#endif

#endif
