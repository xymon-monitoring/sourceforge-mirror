/*----------------------------------------------------------------------------*/
/* Hobbit monitor library.                                                    */
/*                                                                            */
/* Copyright (C) 2002-2006 Henrik Storner <henrik@storner.dk>                 */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

#ifndef __EVENTLOG_H_
#define __EVENTLOG_H_

/* Format of records in the $BBHIST/allevents file */
typedef struct event_t {
	struct namelist_t *host;
	struct htnames_t *service;
	time_t	eventtime;
	time_t	changetime;
	time_t	duration;
	int	newcolor;	/* stored as "re", "ye", "gr" etc. */
	int	oldcolor;
	int	state;		/* 2=escalated, 1=recovered, 0=no change */
	struct event_t *next;
} event_t;


extern char *eventignorecolumns;
extern int havedoneeventlog;

extern void do_eventlog(FILE *output, int maxcount, int maxminutes, char *fromtime, char *totime, 
			char *pagematch, char *hostmatch, char *testmatch, char *colormatch,
			int ignoredialups);

#endif
