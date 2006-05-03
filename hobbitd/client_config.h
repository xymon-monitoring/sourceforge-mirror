/*----------------------------------------------------------------------------*/
/* Hobbit message daemon.                                                     */
/*                                                                            */
/* Copyright (C) 2004-2006 Henrik Storner <henrik@hswn.dk>                    */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

#ifndef __CLIENT_CONFIG_H__
#define __CLIENT_CONFIG_H__

#include "libbbgen.h"

extern int load_client_config(char *configfn);
extern void dump_client_config(void);

extern int get_cpu_thresholds(namelist_t *hinfo, char *classname, 
			      float *loadyellow, float *loadred, int *recentlimit, int *ancientlimit);
extern int get_disk_thresholds(namelist_t *hhinfo, char *classname, 
				char *fsname, unsigned long *warnlevel, unsigned long *paniclevel, int *absolutes);
extern void get_memory_thresholds(namelist_t *hhinfo, char *classname,
				  int *physyellow, int *physred, 
				  int *swapyellow, int *swapred, 
				  int *actyellow, int *actred);

extern int scan_log(namelist_t *hinfo, char *classname, 
		    char *logname, char *logdata, char *section, strbuffer_t *summarybuf);
extern int check_file(namelist_t *hinfo, char *classname, 
		      char *filename, char *filedata, char *section, strbuffer_t *summarybuf, off_t *sz, int *trackit, int *anyrules);
extern int check_dir(namelist_t *hinfo, char *classname, 
		     char *filename, char *filedata, char *section, strbuffer_t *summarybuf, unsigned long *sz, int *trackit);

extern int clear_process_counts(namelist_t *hinfo, char *classname);
extern void add_process_count(char *pname);
extern char *check_process_count(int *pcount, int *lowlim, int *uplim, int *pcolor, char **id, int *trackit);

extern int clear_disk_counts(namelist_t *hinfo, char *classname);
extern void add_disk_count(char *dname);
extern char *check_disk_count(int *dcount, int *lowlim, int *uplim, int *dcolor);

extern int clear_port_counts(namelist_t *hinfo, char *classname);
extern void add_port_count(char *spname, char *tpname, char *stname);
extern char *check_port_count(int *pcount, int *lowlim, int *uplim, int *pcolor, char **id, int *trackit);

#endif

