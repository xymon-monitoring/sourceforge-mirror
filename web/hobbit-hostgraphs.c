/*----------------------------------------------------------------------------*/
/* Hobbit webpage generator tool.                                             */
/*                                                                            */
/* This tool creates an overview page of several graphs.                      */
/*                                                                            */
/* Copyright (C) 2006 Henrik Storner <henrik@storner.dk>                      */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

static char rcsid[] = "$Id: hobbit-hostgraphs.c,v 1.5 2006-06-21 05:56:03 henrik Exp $";

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>

#include "libbbgen.h"

enum { A_SELECT, A_GENERATE } action = A_SELECT;

char *hostpattern = NULL;
char *pagepattern = NULL;
char *ippattern = NULL;
char **hosts = NULL;
char **tests = NULL;
time_t starttime = 0;
time_t endtime = 0;

static void errormsg(char *msg)
{
	printf("Content-type: %s\n\n", xgetenv("HTMLCONTENTTYPE"));
	printf("<html><head><title>Invalid request</title></head>\n");
	printf("<body>%s</body></html>\n", msg);
	exit(1);
}

void parse_query(void)
{
	cgidata_t *cgidata, *cwalk;
	int sday = 0, smon = 0, syear = 0, eday = 0, emon = 0, eyear = 0;
	int hostcount = 0, testcount = 0, alltests = 0;

	cgidata = cgi_request();
	if (cgidata == NULL) return;

	// start-mon=6&start-day=1&start-yr=2006&end-mon=6&end-day=13&end-yr=2006&hostname=bascop01&testname=ALL&DoReport=Generate+Report

	cwalk = cgidata;
	while (cwalk) {
		/*
		 * cwalk->name points to the name of the setting.
		 * cwakl->value points to the value (may be an empty string).
		 */

		if ((strcmp(cwalk->name, "hostpattern") == 0) && cwalk->value && strlen(cwalk->value)) {
			hostpattern = strdup(cwalk->value);
		}
		else if ((strcmp(cwalk->name, "pagepattern") == 0) && cwalk->value && strlen(cwalk->value)) {
			pagepattern = strdup(cwalk->value);
		}
		else if ((strcmp(cwalk->name, "ippattern") == 0)   && cwalk->value && strlen(cwalk->value)) {
			ippattern = strdup(cwalk->value);
		}
		else if (strcmp(cwalk->name, "DoReport") == 0) {
			action = A_GENERATE;
		}
		else if ((strcmp(cwalk->name, "hostname") == 0)   && cwalk->value && strlen(cwalk->value)) {
			if (!hosts) hosts = (char **) malloc(sizeof(char *));

			hosts = (char **)realloc(hosts, (hostcount+2) * sizeof(char *));
			hosts[hostcount] = strdup(cwalk->value); hostcount++;

			hosts[hostcount] = NULL;
		}
		else if ((strcmp(cwalk->name, "testname") == 0)   && cwalk->value && strlen(cwalk->value)) {
			if (!tests) tests = (char **) malloc(sizeof(char *));

			if (strcmp(cwalk->value, "ALL") == 0) {
				alltests = 1;
			}
			else {
				tests = (char **)realloc(tests, (testcount+2) * sizeof(char *));
				tests[testcount] = strdup(cwalk->value); testcount++;
			}

			tests[testcount] = NULL;
		}
		else if ((strcmp(cwalk->name, "start-day") == 0)   && cwalk->value && strlen(cwalk->value)) {
			sday = atoi(cwalk->value);
		}
		else if ((strcmp(cwalk->name, "start-mon") == 0)   && cwalk->value && strlen(cwalk->value)) {
			smon = atoi(cwalk->value);
		}
		else if ((strcmp(cwalk->name, "start-yr") == 0)   && cwalk->value && strlen(cwalk->value)) {
			syear = atoi(cwalk->value);
		}
		else if ((strcmp(cwalk->name, "end-day") == 0)   && cwalk->value && strlen(cwalk->value)) {
			eday = atoi(cwalk->value);
		}
		else if ((strcmp(cwalk->name, "end-mon") == 0)   && cwalk->value && strlen(cwalk->value)) {
			emon = atoi(cwalk->value);
		}
		else if ((strcmp(cwalk->name, "end-yr") == 0)   && cwalk->value && strlen(cwalk->value)) {
			eyear = atoi(cwalk->value);
		}

		cwalk = cwalk->next;
	}

	if (action == A_GENERATE) {
		struct tm tm;

		memset(&tm, 0, sizeof(tm));
		tm.tm_mday = sday;
		tm.tm_mon = smon - 1;
		tm.tm_year = syear - 1900;
		tm.tm_hour = 0;
		tm.tm_min  = 0;
		tm.tm_sec  = 0;
		tm.tm_isdst = -1;
		starttime = mktime(&tm);

		memset(&tm, 0, sizeof(tm));
		tm.tm_mday = eday;
		tm.tm_mon = emon - 1;
		tm.tm_year = eyear - 1900;
		tm.tm_hour = 23;
		tm.tm_min  = 59;
		tm.tm_sec  = 59;
		tm.tm_isdst = -1;
		endtime = mktime(&tm);
	}

	if (alltests) {
		if (tests) xfree(tests); testcount = 0;
		tests = (char **) malloc(5 * sizeof(char *));

		if (hostcount == 1) {
			tests[testcount] = strdup("cpu"); testcount++;
			tests[testcount] = strdup("disk"); testcount++;
			tests[testcount] = strdup("memory"); testcount++;
			tests[testcount] = strdup("conn"); testcount++;
		}
		else {
			tests[testcount] = strdup("cpu"); testcount++;
			tests[testcount] = strdup("mem"); testcount++;
			tests[testcount] = strdup("swap"); testcount++;
			tests[testcount] = strdup("conn-multi"); testcount++;
		}

		tests[testcount] = NULL;
	}

	if (hostcount > 1) {
		int i;

		for (i = 0; (i < testcount); i++) {
			if (strcmp(tests[i], "conn") == 0) tests[i] = strdup("conn-multi");
		}
	}
}

int main(int argc, char *argv[])
{
	int argi;
	char *envarea = NULL;
	char *hffile = "boilerplate";

	for (argi = 1; (argi < argc); argi++) {
		if (argnmatch(argv[argi], "--env=")) {
			char *p = strchr(argv[argi], '=');
			loadenv(p+1, envarea);
		}
		else if (argnmatch(argv[argi], "--area=")) {
			char *p = strchr(argv[argi], '=');
			envarea = strdup(p+1);
		}
		else if (strcmp(argv[argi], "--debug") == 0) {
			debug = 1;
		}
		else if (argnmatch(argv[argi], "--hffile=")) {
			char *p = strchr(argv[argi], '=');
			hffile = strdup(p+1);
		}
	}

	parse_query();

	fprintf(stdout, "Content-type: %s\n\n", xgetenv("HTMLCONTENTTYPE"));

	if (action == A_SELECT) {
                char *cookie, *p;

		cookie = getenv("HTTP_COOKIE");
		if (cookie && !pagepattern && ((p = strstr(cookie, "pagepath=")) != NULL)) {
			/* Match ONLY the exact pagename by using start/end of line markers */

			p += strlen("pagepath=");
			pagepattern = (char *)malloc(strlen(p) + 3);
			sprintf(pagepattern, "^%s", p);
			p = strchr(pagepattern, ';'); if (p) *p = '\0';

			if (strlen(pagepattern) == 0) { 
				xfree(pagepattern); 
				pagepattern = NULL;
			}
			else {
				strcat(pagepattern, "$");
			}
		}

		if (hostpattern || pagepattern || ippattern)
			sethostenv_filter(hostpattern, pagepattern, ippattern);
		showform(stdout, "hostgraphs", "hostgraphs_form", COL_BLUE, getcurrenttime(NULL), NULL);
	}
	else if ((action == A_GENERATE) && hosts && hosts[0] && tests && tests[0]) {
		int hosti, testi;

		headfoot(stdout, "hostgraphs", "", "header", COL_GREEN);
		fprintf(stdout, "<table align=\"center\" summary=\"Graphs\">\n");


		for (testi=0; (tests[testi]); testi++) {
			fprintf(stdout, "<tr><td><img src=\"%s/hobbitgraph.sh?host=%s",
				xgetenv("CGIBINURL"), hosts[0]);

			for (hosti=1; (hosts[hosti]); hosti++) fprintf(stdout, ",%s", hosts[hosti]);

			fprintf(stdout, "&amp;service=%s&amp;graph_start=%ld&amp;graph_end=%ld&graph=custom&amp;action=view\"></td></tr>\n",
				tests[testi], (long int)starttime, (long int)endtime);
		}

	  	fprintf(stdout, "</table><br><br>\n");
		headfoot(stdout, "hostgraphs", "", "footer", COL_GREEN);
	}

	return 0;
}

