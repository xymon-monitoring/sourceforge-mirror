/*----------------------------------------------------------------------------*/
/* Hobbit webpage generator tool.                                             */
/*                                                                            */
/* Copyright (C) 2004-2005 Henrik Storner <henrik@storner.dk>                 */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

static char rcsid[] = "$Id: bb-datepage.c,v 1.9 2005-05-07 09:24:20 henrik Exp $";

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <limits.h>

#include "libbbgen.h"
#include "version.h"

static enum { FRM_NONE, FRM_MONTH, FRM_WEEK, FRM_DAY } frmtype = FRM_NONE;
static int year = -1;
static int month = -1;
static int day = -1;
static int week = -1;

static void errormsg(char *msg)
{
	printf("Content-type: text/html\n\n");
	printf("<html><head><title>Invalid request</title></head>\n");
	printf("<body>%s</body></html>\n", msg);
	exit(1);
}

static void parse_query(void)
{
	cgidata_t *cgidata = cgi_request();
	cgidata_t *cwalk;

	if (cgidata == NULL) {
		errormsg(cgi_error());
	}

	cwalk = cgidata;
	while (cwalk) {
		if (strcasecmp(cwalk->name, "YEAR") == 0) {
			year = atoi(cwalk->value);
		}
		else if (strcasecmp(cwalk->name, "MONTH") == 0) {
			month = atoi(cwalk->value);
		}
		else if (strcasecmp(cwalk->name, "DAY") == 0) {
			day = atoi(cwalk->value);
		}
		else if (strcasecmp(cwalk->name, "WEEK") == 0) {
			week = atoi(cwalk->value);
		}
		else if (strcasecmp(cwalk->name, "TYPE") == 0) {
			if (strcasecmp(cwalk->value, "month") == 0) frmtype = FRM_MONTH;
			else if (strcasecmp(cwalk->value, "week") == 0) frmtype = FRM_WEEK;
			else if (strcasecmp(cwalk->value, "day") == 0) frmtype = FRM_DAY;
			else errormsg("Bad type parameter\n");
		}

		cwalk = cwalk->next;
	}
}

int main(int argc, char *argv[])
{
	int argi;
	char *hffile = "report";
	char *urlprefix = "";
	int bgcolor = COL_BLUE;
	char *envarea = NULL;

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
		else if (argnmatch(argv[argi], "--color=")) {
			char *p = strchr(argv[argi], '=');
			bgcolor = parse_color(p+1);
		}
		else if (argnmatch(argv[argi], "--url=")) {
			char *p = strchr(argv[argi], '=');
			urlprefix = strdup(p+1);
		}
	}

	parse_query();

	if (cgi_method == CGI_POST) {
		char *cookie, *pagepath, *p;
		char *endurl;

		cookie = getenv("HTTP_COOKIE");
		if (cookie == NULL) {
			errormsg("Cookies must be enabled\n");
			return 1;
		}

		p = strstr(cookie, "pagepath="); if (p) p+= strlen("pagepath=");
		if ((p == NULL) || (strlen(p) == 0) || (*p == ';')) {
			p = strstr(cookie, "host="); if (p) p += strlen("host=");
			if ((p == NULL) || (strlen(p) == 0) || (*p == ';')) {
				pagepath = "";
			}
			else {
				char *hname;
				namelist_t *hinfo;

				hname = strdup(p);
				p = strchr(hname, ';'); if (p) *p = '\0';

				load_hostnames(xgetenv("BBHOSTS"), NULL, get_fqdn());
				hinfo = hostinfo(hname);
				if (hinfo) {
					pagepath = bbh_item(hinfo, BBH_PAGEPATH);
				}
				else {
					pagepath = "";
				}
			}
		}
		else {
			pagepath = strdup(p);
			p = strchr(pagepath, ';'); if (p) *p = '\0';
		}

		endurl = (char *)malloc(strlen(urlprefix) + strlen(pagepath) + 1024);

		switch (frmtype) {
		  case FRM_DAY:
			if ((year == -1) || (month == -1) || (day == -1)) errormsg("Invalid day-request");
			sprintf(endurl, "%s/daily/%d/%02d/%02d/%s", urlprefix, year, month, day, pagepath);
			break;

		  case FRM_WEEK:
			if ((year == -1) || (week == -1)) errormsg("Invalid week-request");
			sprintf(endurl, "%s/weekly/%d/%02d/%s", urlprefix, year, week, pagepath);
			break;

		  case FRM_MONTH:
			if ((year == -1) || (month == -1)) errormsg("Invalid month-request");
			sprintf(endurl, "%s/monthly/%d/%02d/%s", urlprefix, year, month, pagepath);
			break;

		  case FRM_NONE:
			break;
		}

		if (*pagepath) strcat(endurl, "/");

		fprintf(stdout, "Content-type: text/html\n\n");
		fprintf(stdout, "<html><head><meta http-equiv=\"refresh\" content=\"0; URL=%s\"></head></html>\n", endurl);
	}
	else if (cgi_method == CGI_GET) {
                int formfile;
                char formfn[PATH_MAX];
		time_t seltime;
		struct tm *seltm;

		seltime = time(NULL); seltm = localtime(&seltime);

                /* Present the query form */
		switch (frmtype) {
		  case FRM_DAY:
			seltm->tm_mday -= 1; seltime = mktime(seltm);
			sprintf(formfn, "%s/web/%s_form_daily", xgetenv("BBHOME"), hffile);
			break;

		  case FRM_WEEK:
			seltm->tm_mday -= 7; seltime = mktime(seltm);
			sprintf(formfn, "%s/web/%s_form_weekly", xgetenv("BBHOME"), hffile);
			break;

		  case FRM_MONTH:
			seltm->tm_mon -= 1; seltime = mktime(seltm);
			sprintf(formfn, "%s/web/%s_form_monthly", xgetenv("BBHOME"), hffile);
			break;

		  case FRM_NONE:
			errormsg("No report type defined");
		}

                formfile = open(formfn, O_RDONLY);
                if (formfile >= 0) {
                        char *inbuf;
                        struct stat st;

                        fstat(formfile, &st);
                        inbuf = (char *) malloc(st.st_size + 1);
                        read(formfile, inbuf, st.st_size);
                        inbuf[st.st_size] = '\0';
                        close(formfile);

                        printf("Content-Type: text/html\n\n");
                        sethostenv("", "", "", colorname(bgcolor));

                        headfoot(stdout, hffile, "", "header", bgcolor);
                        output_parsed(stdout, inbuf, COL_BLUE, "report", seltime);
                        headfoot(stdout, hffile, "", "footer", bgcolor);

                        xfree(inbuf);
                }
	}

	return 0;
}

