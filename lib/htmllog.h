/*----------------------------------------------------------------------------*/
/* Hobbit monitor library.                                                    */
/*                                                                            */
/* Copyright (C) 2002-2005 Henrik Storner <henrik@storner.dk>                 */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

#ifndef __HTMLLOG_H__
#define __HTMLLOG_H__

#include <stdio.h>

enum histbutton_t { HIST_TOP, HIST_BOTTOM, HIST_NONE };

extern enum histbutton_t histlocation;

extern void generate_html_log(char *hostname, char *displayname, char *service, char *ip, 
		       int color, char *sender, char *flags, 
		       time_t logtime, char *timesincechange, 
		       char *firstline, char *restofmsg, 
		       time_t acktime, char *ackmsg, 
		       time_t disabletime, char *dismsg,
		       int is_history, int wantserviceid, int htmlfmt, int hobbitd,
		       char *multigraphs,
		       char *linktoclient,
		       char *nkprio, char *nkresolver, char *nkttgroup, char *nkttextra,
		       FILE *output);
extern char *alttag(char *columnname, int color, int acked, int propagate, char *age);

#endif
