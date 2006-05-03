/*----------------------------------------------------------------------------*/
/* Hobbit RRD handler module.                                                 */
/*                                                                            */
/* Copyright (C) 2004-2006 Henrik Storner <henrik@hswn.dk>                    */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

static char bbtest_rcsid[] = "$Id: do_bbtest.c,v 1.11 2006-05-03 21:19:24 henrik Exp $";

int do_bbtest_rrd(char *hostname, char *testname, char *msg, time_t tstamp)
{ 
	static char *bbtest_params[] = { "rrdcreate", rrdfn, "DS:runtime:GAUGE:600:0:U", rra1, rra2, rra3, rra4, NULL };
	static char *bbtest_tpl      = NULL;

	char	*p;
	float	runtime;

	if (bbtest_tpl == NULL) bbtest_tpl = setup_template(bbtest_params);

	p = strstr(msg, "TIME TOTAL");
	if (p && (sscanf(p, "TIME TOTAL %f", &runtime) == 1)) {
		if (strcmp("bbtest", testname) != 0) {
			sprintf(rrdfn, "bbtest.%s.rrd", testname);
		}
		else {
			strcpy(rrdfn, "bbtest.rrd");
		}
		sprintf(rrdvalues, "%d:%.2f", (int) tstamp, runtime);
		return create_and_update_rrd(hostname, rrdfn, bbtest_params, bbtest_tpl);
	}

	return 0;
}
