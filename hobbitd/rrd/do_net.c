/*----------------------------------------------------------------------------*/
/* Big Brother message daemon.                                                */
/*                                                                            */
/* Copyright (C) 2004 Henrik Storner <henrik@hswn.dk>                         */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

static char bbnet_rcsid[] = "$Id: do_net.c,v 1.7 2005-01-15 17:38:33 henrik Exp $";

static char *bbnet_params[]       = { "rrdcreate", rrdfn, "DS:sec:GAUGE:600:0:U", rra1, rra2, rra3, rra4, NULL };

int do_net_larrd(char *hostname, char *testname, char *msg, time_t tstamp)
{
	char *p;
	float seconds;

	if (strcmp(testname, "http") == 0) {
		char *line1, *url = NULL, *eoln;

		line1 = msg;
		while ((line1 = strchr(line1, '\n')) != NULL) {
			line1++; /* Skip the newline */
			eoln = strchr(line1, '\n'); if (eoln) *eoln = '\0';

			if ( (strncmp(line1, "&green", 6) == 0) || 
			     (strncmp(line1, "&yellow", 7) == 0) ||
			     (strncmp(line1, "&red", 4) == 0) ) {
				p = strstr(line1, "http");
				if (p) {
					url = xstrdup(p);
					p = strchr(url, ' ');
					if (p) *p = '\0';
				}
			}
			else if (url && ((p = strstr(line1, "Seconds:")) != NULL) && (sscanf(p, "Seconds: %f", &seconds) == 1)) {
				char *urlfn = url;

				if (strncmp(urlfn, "http://", 7) == 0) urlfn += 7;
				p = urlfn; while ((p = strchr(p, '/')) != NULL) *p = ',';
				sprintf(rrdfn, "tcp.http.%s.rrd", urlfn);
				sprintf(rrdvalues, "%d:%.2f", (int)tstamp, seconds);
				create_and_update_rrd(hostname, rrdfn, bbnet_params, update_params);
				xfree(url); url = NULL;
			}

			if (eoln) *eoln = '\n';
		}

		if (url) xfree(url);
	}
	else if ((strcmp(testname, "conn") == 0) || (strcmp(testname, "ping") == 0) || (strcmp(testname, "fping") == 0)) {
		/*
		 * Ping-tests, possibly using fping.
		 */
		char *tmod = "ms";

		if ((p = strstr(msg, "time=")) != NULL) {
			/* Standard ping, reports ".... time=0.2 ms" */
			seconds = atof(p+5);
			tmod = p + 5; tmod += strspn(tmod, "0123456789. ");
		}
		else if ((p = strstr(msg, "alive")) != NULL) {
			/* fping, reports ".... alive (0.43 ms)" */
			seconds = atof(p+7);
			tmod = p + 7; tmod += strspn(tmod, "0123456789. ");
		}

		if (strncmp(tmod, "ms", 2) == 0) seconds = seconds / 1000.0;
		else if (strncmp(tmod, "usec", 4) == 0) seconds = seconds / 1000000.0;

		sprintf(rrdfn, "tcp.conn.rrd");
		sprintf(rrdvalues, "%d:%.6f", (int)tstamp, seconds);
		return create_and_update_rrd(hostname, rrdfn, bbnet_params, update_params);
	}
	else {
		/*
		 * Normal network tests - pick up the "Seconds:" value
		 */
		p = strstr(msg, "\nSeconds:");
		if (p && (sscanf(p+1, "Seconds: %f", &seconds) == 1)) {
			sprintf(rrdfn, "tcp.%s.rrd", testname);
			sprintf(rrdvalues, "%d:%.2f", (int)tstamp, seconds);
			return create_and_update_rrd(hostname, rrdfn, bbnet_params, update_params);
		}
	}

	return 0;
}

