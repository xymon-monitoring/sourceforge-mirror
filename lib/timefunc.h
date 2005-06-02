/*----------------------------------------------------------------------------*/
/* Hobbit monitor library.                                                    */
/*                                                                            */
/* Copyright (C) 2002-2005 Henrik Storner <henrik@storner.dk>                 */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

#ifndef __TIMEFUNC_H__
#define __TIMEFUNC_H__

extern time_t fakestarttime;
extern char *timestamp;

extern time_t getcurrenttime(time_t *retparm);
#define time(X) getcurrenttime(X)

extern void init_timestamp(void);
extern char *weekday_text(char *dayspec);
extern char *time_text(char *timespec);
extern char *timespec_text(char *spec);
extern struct timeval *tvdiff(struct timeval *tstart, struct timeval *tend, struct timeval *result);
extern int within_sla(char *l, int defresult);
extern int periodcoversnow(char *tag);
extern char *histlogtime(time_t histtime);
extern int durationvalue(char *dur);
extern char *durationstring(time_t secs);

#endif

