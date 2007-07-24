/*----------------------------------------------------------------------------*/
/* Hobbit RRD handler module.                                                 */
/*                                                                            */
/* Copyright (C) 2004-2006 Henrik Storner <henrik@hswn.dk>                    */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

static char citrix_rcsid[] = "$Id: do_citrix.c,v 1.14 2007-07-24 08:45:01 henrik Exp $";

int do_citrix_rrd(char *hostname, char *testname, char *msg, time_t tstamp)
{
	static char *citrix_params[] = { "DS:users:GAUGE:600:0:U", NULL };
	static char *citrix_tpl      = NULL;

	char *p;
	int users;

	if (citrix_tpl == NULL) citrix_tpl = setup_template(citrix_params);

	p = strstr(msg, " users active\n");
	while (p && (p > msg) && (*p != '\n')) p--;
	if (p && (sscanf(p+1, "\n%d users active\n", &users) == 1)) {
		setupfn("%s", "citrix.rrd");
		sprintf(rrdvalues, "%d:%d", (int)tstamp, users);
		return create_and_update_rrd(hostname, testname, citrix_params, citrix_tpl);
	}

	return 0;
}

