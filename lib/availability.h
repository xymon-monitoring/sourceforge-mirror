/*----------------------------------------------------------------------------*/
/* Hobbit overview webpage generator tool.                                    */
/*                                                                            */
/* Copyright (C) 2002-2005 Henrik Storner <henrik@storner.dk>                 */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

#ifndef __REPORTDATA_H__
#define __REPORTDATA_H__

extern replog_t *reploghead;

extern char *durationstr(time_t duration);
extern int parse_historyfile(FILE *fd, reportinfo_t *repinfo, char *hostname, char *servicename, 
				time_t fromtime, time_t totime, int for_history,
				double warnlevel, double greenlevel, char *reporttime);
extern replog_t *save_replogs(void);
extern void restore_replogs(replog_t *head);
extern int history_color(FILE *fd, time_t snapshot, time_t *starttime, char **histlogname);

#endif

