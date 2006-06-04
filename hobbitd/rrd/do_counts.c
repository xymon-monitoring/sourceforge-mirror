/*----------------------------------------------------------------------------*/
/* Hobbit RRD handler module.                                                 */
/*                                                                            */
/* This module handles various "counts" messages.                             */
/*                                                                            */
/* Copyright (C) 2005-2006 Henrik Storner <henrik@hswn.dk>                    */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

static char counts_rcsid[] = "$Id: do_counts.c,v 1.4 2006-06-04 10:55:33 henrik Exp $";

static int do_one_counts_rrd(char *counttype, char *hostname, char *testname, char *msg, time_t tstamp, char *params[], char *tpl) 
{ 
	char *boln, *eoln;

	boln = strchr(msg, '\n'); if (boln) boln++;
	while (boln && *boln) {
		char *fn, *countstr = NULL;

		eoln = strchr(boln, '\n'); if (eoln) *eoln = '\0';

		fn = strtok(boln, ":"); if (fn) countstr = strtok(NULL, ":");
		if (fn && countstr) {
			char *p;

			for (p=strchr(fn, '/'); (p); p = strchr(p, '/')) *p = ',';
			sprintf(rrdfn, "%s.%s.rrd", counttype, fn);

			sprintf(rrdvalues, "%d:%s", (int)tstamp, countstr);
			create_and_update_rrd(hostname, rrdfn, params, tpl);
		}

		boln = (eoln ? eoln+1 : NULL);
	}

	return 0;
}

static char *counts_params[] = { "rrdcreate", rrdfn, "DS:count:GAUGE:600:0:U", rra1, rra2, rra3, rra4, NULL };
static char *counts_tpl      = NULL;
static char *derive_params[] = { "rrdcreate", rrdfn, "DS:count:DERIVE:600:0:U", rra1, rra2, rra3, rra4, NULL };
static char *derive_tpl      = NULL;

int do_counts_rrd(char *counttype, char *hostname, char *testname, char *msg, time_t tstamp) 
{
	if (counts_tpl == NULL) counts_tpl = setup_template(counts_params);

	return do_one_counts_rrd(counttype, hostname, testname, msg, tstamp, counts_params, counts_tpl);
}

int do_derives_rrd(char *counttype, char *hostname, char *testname, char *msg, time_t tstamp) 
{
	if (derive_tpl == NULL) derive_tpl = setup_template(derive_params);

	return do_one_counts_rrd(counttype, hostname, testname, msg, tstamp, derive_params, derive_tpl);
}

