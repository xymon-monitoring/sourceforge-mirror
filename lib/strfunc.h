/*----------------------------------------------------------------------------*/
/* Hobbit monitor library.                                                    */
/*                                                                            */
/* Copyright (C) 2002-2005 Henrik Storner <henrik@storner.dk>                 */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

#ifndef __STRFUNC_H__
#define __STRFUNC_H__

extern strbuffer_t *newstrbuffer(int initialsize);
extern void addtobuffer(strbuffer_t *buf, char *newtext);
extern void addtostrbuffer(strbuffer_t *buf, strbuffer_t *newtext);
extern void clearstrbuffer(strbuffer_t *buf);
extern void freestrbuffer(strbuffer_t *buf);
extern char *grabstrbuffer(strbuffer_t *buf);
extern strbuffer_t *dupstrbuffer(char *src);
extern void strbufferchop(strbuffer_t *buf, int count);
extern void strbufferrecalc(strbuffer_t *buf);

#endif

