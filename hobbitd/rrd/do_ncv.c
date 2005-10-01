/*----------------------------------------------------------------------------*/
/* Hobbit RRD handler module.                                                 */
/*                                                                            */
/* This module handles any message with data in the form                      */
/*     NAME: VALUE                                                            */
/*                                                                            */
/* Copyright (C) 2004-2005 Henrik Storner <henrik@hswn.dk>                    */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

static char ncv_rcsid[] = "$Id: do_ncv.c,v 1.6 2005-10-01 06:22:22 henrik Exp $";

int do_ncv_rrd(char *hostname, char *testname, char *msg, time_t tstamp) 
{ 
	char **params = NULL;
	int paridx;
	char dsdef[1024];
	char *l, *name, *val;
	char *envnam;
	char *dstypes = NULL;

	sprintf(rrdfn, "%s.rrd", testname);
	sprintf(rrdvalues, "%d", (int)tstamp);

	params = (char **)calloc(8, sizeof(char *));
	params[0] = "rrdcreate";
	params[1] = rrdfn;
	paridx = 1;

	envnam = (char *)malloc(4 + strlen(testname) + 1); sprintf(envnam, "NCV_%s", testname);
	l = getenv(envnam);
	if (l) {
		dstypes = (char *)malloc(strlen(l)+3);
		sprintf(dstypes, ",%s,", l);
	}
	xfree(envnam);

	l = strchr(msg, '\n'); if (l) l++;
	while (l && *l && strncmp(l, "@@\n", 3)) {
		name = val = NULL;

		l += strspn(l, " \t\n");
		if (*l) { 
			name = l; 
			l += strcspn(l, ":="); 
			if( *l ) { *l = '\0'; l++; }
			else break;
		}
		if (name) { 
			val = l + strspn(l, " \t"); 
			l = val + strspn(val, "0123456789."); 
			if( *l ) { *l = '\0'; l++; }
			else break;
		}

		if (name && val && *val) {
			char *endptr;

			strtod(val, &endptr);
			if (isspace((int)*endptr) || (*endptr == '\0')) {
				char dsname[20];
				char dskey[22];
				char *dstype = NULL;
				char *inp;
				int outidx;

				/* val contains a valid number */
				for (inp=name,outidx=0; (*inp && (outidx < 19)); inp++) {
					if ( ((*inp >= 'A') && (*inp <= 'Z')) ||
					     ((*inp >= 'a') && (*inp <= 'z')) ||
					     ((*inp >= '0') && (*inp <= '9')) ) {
						dsname[outidx++] = *inp;
					}
				}
				dsname[outidx] = '\0';
				sprintf(dskey, ",%s:", dsname);

				if (dstypes) dstype = strstr(dstypes, dskey);
				if (dstype) {
					char *p;

					dstype += strlen(dskey);
					p = strchr(dstype, ','); if (p) *p = '\0';
					sprintf(dsdef, "DS:%s:%s:600:0:U", dsname, dstype);
					if (p) *p = ',';
				}
				else {
					sprintf(dsdef, "DS:%s:DERIVE:600:0:U", dsname);
				}

				if (!dstype || (strncasecmp(dstype, "NONE", 4) != 0)) {
					paridx++;
					params = (char **)realloc(params, (7 + paridx)*sizeof(char *));
					params[paridx] = strdup(dsdef);
					params[paridx+1] = NULL;

					sprintf(rrdvalues+strlen(rrdvalues), ":%s", val);
				}
			}
		}
	}

	if (paridx > 1) {
		params[++paridx] = strdup(rra1);
		params[++paridx] = strdup(rra2);
		params[++paridx] = strdup(rra3);
		params[++paridx] = strdup(rra4);
		params[++paridx] = NULL;

		create_and_update_rrd(hostname, rrdfn, params, NULL);

		for (paridx=2; (params[paridx] != NULL); paridx++)
		xfree(params[paridx]);
	}

	xfree(params);
	if (dstypes) xfree(dstypes);

	return 0;
}

