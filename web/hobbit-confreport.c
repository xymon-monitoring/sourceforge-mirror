/*----------------------------------------------------------------------------*/
/* Hobbit CGI tool to generate a report of the Hobbit configuration           */
/*                                                                            */
/* Copyright (C) 2003-2005 Henrik Storner <henrik@storner.dk>                 */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

static char rcsid[] = "$Id: hobbit-confreport.c,v 1.8 2006-03-18 07:30:33 henrik Exp $";

#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>

#include "libbbgen.h"

typedef struct hostlist_t {
	char *hostname;
	int testcount;
	htnames_t *tests;
	htnames_t *disks, *svcs, *procs;
	struct hostlist_t *next;
} hostlist_t;

typedef struct coltext_t {
	char *colname;
	char *coldescr;
	int used;
	struct coltext_t *next;
} coltext_t;

hostlist_t *hosthead = NULL;
static char *pingcolumn = "conn";
static char *pingplus = "conn=";
static char *coldelim = ";";
static coltext_t *chead = NULL;
static int ccount = 0;

void errormsg(char *msg)
{
        printf("Content-type: text/html\n\n");
        printf("<html><head><title>Invalid request</title></head>\n");
        printf("<body>%s</body></html>\n", msg);
        exit(1);
}

static int host_compare(const void *v1, const void *v2)
{
	hostlist_t **h1 = (hostlist_t **)v1;
	hostlist_t **h2 = (hostlist_t **)v2;

	return strcmp((*h1)->hostname, (*h2)->hostname);
}

static int test_compare(const void *v1, const void *v2)
{
	htnames_t **t1 = (htnames_t **)v1;
	htnames_t **t2 = (htnames_t **)v2;

	return strcmp((*t1)->name, (*t2)->name);
}


static int is_net_test(char *tname)
{
	char *miscnet[] = { pingcolumn,  "http", "dns", "dig", "rpc", "ntp", "ldap", "content", "sslcert", NULL };
	int i;

	if (find_tcp_service(tname)) return 1;
	for (i=0; (miscnet[i]); i++) if (strcmp(tname, miscnet[i]) == 0) return 1;

	return 0;
}


void use_columndoc(char *column)
{
	coltext_t *cwalk;

	for (cwalk = chead; (cwalk && strcasecmp(cwalk->colname, column)); cwalk = cwalk->next);
	if (cwalk) cwalk->used = 1;
}

typedef struct tag_t {
	char *columnname;
	char *visualdata;	/* The URL or other end-user visible test spec. */
	char *expdata;
	int b1, b2, b3;		/* "badFOO" values, if any */
	struct tag_t *next;
} tag_t;

static void print_disklist(char *hostname)
{
	/*
	 * We get the list of monitored disks/filesystems by looking at the
	 * set of disk RRD files for this host. That way we do not have to
	 * parse the disk status reports that come in many different flavours.
	 */

	char dirname[PATH_MAX];
	char fn[PATH_MAX];
	DIR *d;
	struct dirent *de;
	char *p;

	sprintf(dirname, "%s/%s", xgetenv("BBRRDS"), hostname);
	d = opendir(dirname);
	if (!d) return;

	while ((de = readdir(d)) != NULL) {
		if (strncmp(de->d_name, "disk,", 5) == 0) {
			strcpy(fn, de->d_name + 4);
			p = strstr(fn, ".rrd"); if (!p) continue;
			*p = '\0';
			p = fn; while ((p = strchr(p, ',')) != NULL) *p = '/';
			fprintf(stdout, "%s<br>\n", fn);
		}
	}

	closedir(d);
}

static void print_host(hostlist_t *host, htnames_t *testnames[], int testcount)
{
	int testi, rowcount, netcount;
	namelist_t *hinfo = hostinfo(host->hostname);
	char *dispname = NULL, *clientalias = NULL, *comment = NULL, *description = NULL, *pagepathtitle = NULL;
	char *net = NULL, *nkalerts = NULL;
	char *nktime = NULL, *downtime = NULL, *reporttime = NULL;
	char *itm;
	tag_t *taghead = NULL;
	int contidx = 0, haveping = 0;
	char contcol[1024];
	activealerts_t alert;
	char *buf = NULL; 
	int buflen = 0;

	fprintf(stdout, "<p style=\"page-break-before: always\">\n"); 
	fprintf(stdout, "<table width=\"100%%\" border=1 summary=\"%s configuration\">\n", host->hostname);

	pagepathtitle = bbh_item(hinfo, BBH_PAGEPATHTITLE);
	if (!pagepathtitle || (strlen(pagepathtitle) == 0)) pagepathtitle = "Top page";
	dispname = bbh_item(hinfo, BBH_DISPLAYNAME); 
	if (dispname && (strcmp(dispname, host->hostname) == 0)) dispname = NULL;
	clientalias = bbh_item(hinfo, BBH_CLIENTALIAS); 
	if (clientalias && (strcmp(clientalias, host->hostname) == 0)) clientalias = NULL;
	comment = bbh_item(hinfo, BBH_COMMENT);
	description = bbh_item(hinfo, BBH_DESCRIPTION); 
	net = bbh_item(hinfo, BBH_NET);
	nktime = bbh_item(hinfo, BBH_NKTIME); if (!nktime) nktime = "24x7"; else nktime = strdup(timespec_text(nktime));
	downtime = bbh_item(hinfo, BBH_DOWNTIME); if (downtime) downtime = strdup(timespec_text(downtime));
	reporttime = bbh_item(hinfo, BBH_REPORTTIME); if (!reporttime) reporttime = "24x7"; else reporttime = strdup(timespec_text(reporttime));

	rowcount = 1;
	if (pagepathtitle) rowcount++;
	if (dispname || clientalias) rowcount++;
	if (comment) rowcount++;
	if (description) rowcount++;
	if (nktime) rowcount++;
	if (downtime) rowcount++;
	if (reporttime) rowcount++;

	fprintf(stdout, "<tr>\n");
	fprintf(stdout, "<th rowspan=%d align=left width=\"25%%\" valign=top>Basics</th>\n", rowcount);
	fprintf(stdout, "<th align=center>%s (%s)</th>\n", 
		(dispname ? dispname : host->hostname), bbh_item(hinfo, BBH_IP));
	fprintf(stdout, "</tr>\n");

	if (dispname || clientalias) {
		fprintf(stdout, "<tr><td>Aliases:");
		if (dispname) fprintf(stdout, " %s", dispname);
		if (clientalias) fprintf(stdout, " %s", clientalias);
		fprintf(stdout, "</td></tr>\n");
	}
	if (pagepathtitle) fprintf(stdout, "<tr><td>Monitoring location: %s</td></tr>\n", pagepathtitle);
	if (comment) fprintf(stdout, "<tr><td>Comment: %s</td></tr>\n", comment);
	if (description) fprintf(stdout, "<tr><td>Description: %s</td></tr>\n", description);
	if (nktime) fprintf(stdout, "<tr><td>NK monitoring period: %s</td></tr>\n", nktime);
	if (downtime) fprintf(stdout, "<tr><td>Planned downtime: %s</td></tr>\n", downtime);
	if (reporttime) fprintf(stdout, "<tr><td>SLA Reporting Period: %s</td></tr>\n", reporttime);


	nkalerts = bbh_item(hinfo, BBH_NK);

	/* Build a list of the network tests */
	itm = bbh_item_walk(hinfo);
	while (itm) {
		char *visdata = NULL, *colname = NULL, *expdata = NULL;
		bburl_t bu;
		int dialuptest = 0, reversetest = 0, alwaystruetest = 0, httpextra = 0;

		if (*itm == '?') { dialuptest=1;     itm++; }
		if (*itm == '!') { reversetest=1;    itm++; }
		if (*itm == '~') { alwaystruetest=1; itm++; }

		if ( argnmatch(itm, "http")         ||
		     argnmatch(itm, "content=http") ||
		     argnmatch(itm, "cont;http")    ||
		     argnmatch(itm, "cont=")        ||
		     argnmatch(itm, "nocont;http")  ||
		     argnmatch(itm, "nocont=")      ||
		     argnmatch(itm, "post;http")    ||
		     argnmatch(itm, "post=")        ||
		     argnmatch(itm, "nopost;http")  ||
		     argnmatch(itm, "nopost=")      ||
		     argnmatch(itm, "type;http")    ||
		     argnmatch(itm, "type=")        ) {
			visdata = decode_url(itm, &bu);
			colname = bu.columnname; 
			if (!colname) {
				if (bu.expdata) {
					httpextra = 1;
					if (contidx == 0) {
						colname = "content";
						contidx++;
					}
					else {
						sprintf(contcol, "content%d", contidx);
						colname = contcol;
						contidx++;
					}
				}
				else {
					colname = "http";
				}
			}
			expdata = bu.expdata;
		}
		else if (strncmp(itm, "rpc=", 4) == 0) {
			colname = "rpc";
			visdata = strdup(itm+4);
		}
		else if (strncmp(itm, "dns=", 4) == 0) {
			colname = "dns";
			visdata = strdup(itm+4);
		}
		else if (strncmp(itm, "dig=", 4) == 0) {
			colname = "dns";
			visdata = strdup(itm+4);
		}
		else if (strncmp(itm, pingplus, strlen(pingplus)) == 0) {
			haveping = 1;
			colname = pingcolumn;
			visdata = strdup(itm+strlen(pingplus));
		}
		else if (is_net_test(itm)) {
			colname = strdup(itm);
		}


		if (colname) {
			tag_t *newitem;

addtolist:
			for (newitem = taghead; (newitem && strcmp(newitem->columnname, colname)); newitem = newitem->next);

			if (!newitem) {
				newitem = (tag_t *)calloc(1, sizeof(tag_t));
				newitem->columnname = strdup(colname);
				newitem->visualdata = (visdata ? strdup(visdata) : NULL);
				newitem->expdata = (expdata ? strdup(expdata) : NULL);
				newitem->next = taghead;
				taghead = newitem;
			}
			else {
				/* Multiple tags for one column - must be http */
				newitem->visualdata = (char *)realloc(newitem->visualdata, strlen(newitem->visualdata) + strlen(visdata) + 5);
				strcat(newitem->visualdata, "<br>");
				strcat(newitem->visualdata, visdata);
			}

			if (httpextra) {
				httpextra = 0;
				colname = "http";
				expdata = NULL;
				goto addtolist;
			}
		}

		itm = bbh_item_walk(NULL);
	}

	if (!haveping && !bbh_item(hinfo, BBH_FLAG_NOCONN)) {
		for (testi = 0; (testi < testcount); testi++) {
			if (strcmp(testnames[testi]->name, pingcolumn) == 0) {
				tag_t *newitem = (tag_t *)calloc(1, sizeof(tag_t));
				newitem->columnname = strdup(pingcolumn);
				newitem->next = taghead;
				taghead = newitem;
			}
		}
	}

	/* Add the "badFOO" settings */
	itm = bbh_item_walk(hinfo);
	while (itm) {
		if (strncmp(itm, "bad", 3) == 0) {
			char *tname, *p;
			int b1, b2, b3, n = -1;
			tag_t *tag = NULL;

			tname = itm+3; 
			p = strchr(tname, ':'); 
			if (p) {
				*p = '\0';
				n = sscanf(p+1, "%d:%d:%d", &b1, &b2, &b3);
				for (tag = taghead; (tag && strcmp(tag->columnname, tname)); tag = tag->next);
				*p = ':';
			}

			if (tag && (n == 3)) {
				tag->b1 = b1; tag->b2 = b2; tag->b3 = b3;
			}
		}

		itm = bbh_item_walk(NULL);
	}

	if (taghead) {
		fprintf(stdout, "<tr>\n");
		fprintf(stdout, "<th align=left valign=top>Network tests");
		if (net) fprintf(stdout, "<br>(from %s)", net);
		fprintf(stdout, "</th>\n");

		fprintf(stdout, "<td><table border=0 cellpadding=\"3\" cellspacing=\"5\" summary=\"%s network tests\">\n", host->hostname);
		fprintf(stdout, "<tr><th align=left valign=top>Service</th><th align=left valign=top>NK</th><th align=left valign=top>C/Y/R limits</th><th align=left valign=top>Specifics</th></tr>\n");
	}
	for (testi = 0, netcount = 0; (testi < testcount); testi++) {
		tag_t *twalk;

		for (twalk = taghead; (twalk && strcasecmp(twalk->columnname, testnames[testi]->name)); twalk = twalk->next);
		if (!twalk) continue;

		use_columndoc(testnames[testi]->name);
		fprintf(stdout, "<tr>");
		fprintf(stdout, "<td valign=top>%s</td>", testnames[testi]->name);
		fprintf(stdout, "<td valign=top>%s</td>", (checkalert(nkalerts, testnames[testi]->name) ? "Yes" : "No"));

		fprintf(stdout, "<td valign=top>");
		if (twalk->b1 || twalk->b2 || twalk->b3) {
			fprintf(stdout, "%d/%d/%d", twalk->b1, twalk->b2, twalk->b3);
		}
		else {
			fprintf(stdout, "-/-/-");
		}
		fprintf(stdout, "</td>");

		fprintf(stdout, "<td valign=top>");
		fprintf(stdout, "<i>%s</i>", (twalk->visualdata ? twalk->visualdata : "&nbsp;"));
		if (twalk->expdata) fprintf(stdout, " must return <i>'%s'</i>", twalk->expdata);
		fprintf(stdout, "</td>");

		fprintf(stdout, "</tr>");
		netcount++;
	}
	if (taghead) {
		fprintf(stdout, "</table></td>\n");
		fprintf(stdout, "</tr>\n");
	}


	if (netcount != testcount) {
		fprintf(stdout, "<tr>\n");
		fprintf(stdout, "<th align=left valign=top>Local tests</th>\n");
		fprintf(stdout, "<td><table border=0 cellpadding=\"3\" cellspacing=\"5\" summary=\"%s local tests\">\n", host->hostname);
		fprintf(stdout, "<tr><th align=left valign=top>Service</th><th align=left valign=top>NK</th><th align=left valign=top>C/Y/R limits</th><th align=left valign=top>Configuration <i>(NB: Thresholds on client may differ)</i></th></tr>\n");
	}
	for (testi = 0; (testi < testcount); testi++) {
		tag_t *twalk;

		for (twalk = taghead; (twalk && strcasecmp(twalk->columnname, testnames[testi]->name)); twalk = twalk->next);
		if (twalk) continue;

		use_columndoc(testnames[testi]->name);
		fprintf(stdout, "<tr>");
		fprintf(stdout, "<td valign=top>%s</td>", testnames[testi]->name);
		fprintf(stdout, "<td valign=top>%s</td>", (checkalert(nkalerts, testnames[testi]->name) ? "Yes" : "No"));
		fprintf(stdout, "<td valign=top>-/-/-</td>");

		/* Make up some default configuration data */
		fprintf(stdout, "<td valign=top>");
		if (strcmp(testnames[testi]->name, "cpu") == 0) {
			fprintf(stdout, "UNIX - Yellow: Load average > 1.5, Red: Load average > 3.0<br>");
			fprintf(stdout, "Windows - Yellow: CPU utilisation > 80%%, Red: CPU utilisation > 95%%");
		}
		else if (strcmp(testnames[testi]->name, "disk") == 0) {
			fprintf(stdout, "Default limits: Yellow 90%% full, Red 95%% full<br>\n");
			print_disklist(host->hostname);
		}
		else if (strcmp(testnames[testi]->name, "memory") == 0) {
			fprintf(stdout, "Yellow: swap/pagefile use > 80%%, Red: swap/pagefile use > 90%%");
		}
		else if (strcmp(testnames[testi]->name, "procs") == 0) {
			htnames_t *walk;

			if (!host->procs) fprintf(stdout, "No processes monitored<br>\n");

			for (walk = host->procs; (walk); walk = walk->next) {
				fprintf(stdout, "%s<br>\n", walk->name);
			}
		}
		else if (strcmp(testnames[testi]->name, "svcs") == 0) {
			htnames_t *walk;

			if (!host->svcs) fprintf(stdout, "No services monitored<br>\n");

			for (walk = host->svcs; (walk); walk = walk->next) {
				fprintf(stdout, "%s<br>\n", walk->name);
			}
		}
		else {
			fprintf(stdout, "&nbsp;");
		}
		fprintf(stdout, "</td>");

		fprintf(stdout, "</tr>");
	}
	if (netcount != testcount) {
		fprintf(stdout, "</table></td>\n");
		fprintf(stdout, "</tr>\n");
	}

	/* Do the alerts */
	alert.hostname = host->hostname;
	alert.location = hinfo->page->pagepath;
	strcpy(alert.ip, "127.0.0.1");
	alert.color = COL_RED;
	alert.pagemessage = "";
	alert.ackmessage = NULL;
	alert.eventstart = 0;
	alert.nextalerttime = 0;
	alert.state = A_PAGING;
	alert.cookie = 12345;
	alert.next = NULL;
	alert_printmode(2);
	for (testi = 0; (testi < testcount); testi++) {
		alert.testname = testnames[testi]->name;
		if (have_recipient(&alert, NULL)) print_alert_recipients(&alert, &buf, &buflen);
	}

	if (buf) {
		fprintf(stdout, "<tr>\n");
		fprintf(stdout, "<th align=left valign=top>Alerts</th>\n");
		fprintf(stdout, "<td><table border=0 cellpadding=\"3\" cellspacing=\"5\" summary=\"%s alerts\">\n", host->hostname);
		fprintf(stdout, "<tr><th>Service</th><th>Recipient</th><th>1st Delay</th><th>Stop after</th><th>Repeat</th><th>Time of Day</th><th>Colors</th></tr>\n");

		fprintf(stdout, "%s", buf);

		fprintf(stdout, "</table></td>\n");
		fprintf(stdout, "</tr>\n");
	}

	/* Finish off this host */
	fprintf(stdout, "</table>\n");
}


static int coltext_compare(const void *v1, const void *v2)
{
	coltext_t **t1 = (coltext_t **)v1;
	coltext_t **t2 = (coltext_t **)v2;

	return strcmp((*t1)->colname, (*t2)->colname);
}

void load_columndocs(void)
{
	char fn[PATH_MAX];
	FILE *fd;
	char *inbuf = NULL;
	int inbufsz;

	sprintf(fn, "%s/etc/columndoc.csv", xgetenv("BBHOME"));
	fd = fopen(fn, "r"); if (!fd) return;

	initfgets(fd);

	/* Skip the header line */
	if (!unlimfgets(&inbuf, &inbufsz, fd)) { fclose(fd); return; }

	while (unlimfgets(&inbuf, &inbufsz, fd)) {
		char *s1 = NULL, *s2 = NULL;

		s1 = strtok(inbuf, coldelim);
		if (s1) s2 = strtok(NULL, coldelim);

		if (s1 && s2) {
			coltext_t *newitem = (coltext_t *)calloc(1, sizeof(coltext_t));
			newitem->colname = strdup(s1);
			newitem->coldescr = strdup(s2);
			newitem->next = chead;
			chead = newitem;
			ccount++;
		}
	}
	fclose(fd);
	if (inbuf) xfree(inbuf);
}


void print_columndocs(void)
{
	coltext_t **clist;
	coltext_t *cwalk;
	int i;

	clist = (coltext_t **)malloc(ccount * sizeof(coltext_t *));
	for (i=0, cwalk=chead; (cwalk); cwalk=cwalk->next,i++) clist[i] = cwalk;
	qsort(&clist[0], ccount, sizeof(coltext_t **), coltext_compare);

	fprintf(stdout, "<p style=\"page-break-before: always\">\n"); 
	fprintf(stdout, "<table width=\"100%%\" border=1 summary=\"Column descriptions\">\n");
	fprintf(stdout, "<tr><th colspan=2>Hobbit column descriptions</th></tr>\n");
	for (i=0; (i<ccount); i++) {
		if (clist[i]->used) {
			fprintf(stdout, "<tr><td align=left valign=top>%s</td><td>%s</td></tr>\n",
				clist[i]->colname, clist[i]->coldescr);
		}
	}

	fprintf(stdout, "</table>\n");
}

htnames_t *get_proclist(char *hostname, char *statusbuf)
{
	char *bol, *eol;
	char *marker;
	htnames_t *head = NULL, *tail = NULL;

	if (!statusbuf) return NULL;

	marker = (char *)malloc(strlen(hostname) + 3);
	sprintf(marker, "\n%s|", hostname);
	if (strncmp(statusbuf, marker+1, strlen(marker)-1) == 0) {
		/* Found at start of buffer */
		bol = statusbuf;
	}
	else {
		bol = strstr(statusbuf, marker);
		if (bol) bol++;
	}
	xfree(marker);

	if (!bol) return NULL;

	bol += strlen(hostname) + 1;  /* Skip hostname and delimiter */
	marker = bol;
	eol = strchr(bol, '\n'); if (eol) *eol = '\0';
	marker = strstr(marker, "\\n&");
	while (marker) {
		char *p;
		htnames_t *newitem;

		marker += strlen("\\n&");
		if      (strncmp(marker, "green", 5) == 0) marker += 5;
		else if (strncmp(marker, "yellow", 6) == 0) marker += 6;
		else if (strncmp(marker, "red", 3) == 0) marker += 3;
		else marker = NULL;

		if (marker) {
			marker += strspn(marker, " \t");

			p = strstr(marker, " - "); if (p) *p = '\0';
			newitem = (htnames_t *)malloc(sizeof(htnames_t));
			newitem->name = strdup(marker);
			newitem->next = NULL;
			if (!tail) {
				head = tail = newitem;
			}
			else {
				tail->next = newitem;
				tail = newitem;
			}

			if (p) {
				*p = ' ';
				marker = p;
			}

			marker = strstr(marker, "\\n&");
		}
	}
	if (eol) *eol = '\n';

	return head;
}

int main(int argc, char *argv[])
{
	int argi, hosti, testi;
	char *pagepattern = NULL, *hostpattern = NULL;
	char *envarea = NULL, *cookie = NULL, *p, *nexthost;
	char hobbitcmd[1024], procscmd[1024], svcscmd[1024];
        int alertcolors, alertinterval;
	char configfn[PATH_MAX];
	char *respbuf = NULL, *procsbuf = NULL, *svcsbuf = NULL;
	hostlist_t *hwalk;
	htnames_t *twalk;
	hostlist_t **allhosts = NULL;
	htnames_t **alltests = NULL;
	int hostcount = 0, maxtests = 0;
	time_t now = time(NULL);

	for (argi=1; (argi < argc); argi++) {
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
		else if (argnmatch(argv[argi], "--delimiter=")) {
			char *p = strchr(argv[argi], '=');
			coldelim = strdup(p+1);
		}
	}

	redirect_cgilog("hobbit-confreport");

	/* Setup the filter we use for the report */
	cookie = getenv("HTTP_COOKIE");
	if (cookie && ((p = strstr(cookie, "pagepath=")) != NULL)) {
		p += strlen("pagepath=");
		pagepattern = strdup(p);
		p = strchr(pagepattern, ';'); if (p) *p = '\0';
		if (strlen(pagepattern) == 0) { xfree(pagepattern); pagepattern = NULL; }
	}

	if (cookie && (!pagepattern) && ((p = strstr(cookie, "host=")) != NULL)) {
		p += strlen("host=");
		hostpattern = strdup(p);
		p = strchr(hostpattern, ';'); if (p) *p = '\0';
		if (strlen(hostpattern) == 0) { xfree(hostpattern); hostpattern = NULL; }
	}

	/* Fetch the list of host+test statuses we currently know about */
	if (pagepattern) {
		sprintf(hobbitcmd, "hobbitdboard page=%s fields=hostname,testname", pagepattern);
		sprintf(procscmd,  "hobbitdboard page=%s test=procs fields=hostname,msg", pagepattern);
		sprintf(svcscmd,   "hobbitdboard page=%s test=svcs fields=hostname,msg", pagepattern);
	}
	else if (hostpattern) {
		sprintf(hobbitcmd, "hobbitdboard host=%s fields=hostname,testname", hostpattern);
		sprintf(procscmd,  "hobbitdboard host=%s test=procs fields=hostname,msg", hostpattern);
		sprintf(svcscmd,   "hobbitdboard host=%s test=svcs fields=hostname,msg", hostpattern);
	}
	else {
		sprintf(hobbitcmd, "hobbitdboard fields=hostname,testname");
		sprintf(procscmd,  "hobbitdboard test=procs fields=hostname,msg");
		sprintf(svcscmd,   "hobbitdboard test=svcs fields=hostname,msg");
	}

	if (sendmessage(hobbitcmd, NULL, NULL, &respbuf, 1, BBTALK_TIMEOUT) != BB_OK) {
		errormsg("Cannot contact the Hobbit server\n");
		return 1;
	}
	if (sendmessage(procscmd, NULL, NULL, &procsbuf, 1, BBTALK_TIMEOUT) != BB_OK) {
		errormsg("Cannot contact the Hobbit server\n");
		return 1;
	}
	if (sendmessage(svcscmd, NULL, NULL, &svcsbuf, 1, BBTALK_TIMEOUT) != BB_OK) {
		errormsg("Cannot contact the Hobbit server\n");
		return 1;
	}

	if (!respbuf) {
		errormsg("Unable to find host information\n");
		return 1;
	}

	/* Parse it into a usable list */
	nexthost = respbuf;
	do {
		char *hname, *tname, *eoln;

		eoln = strchr(nexthost, '\n'); if (eoln) *eoln = '\0';
		hname = nexthost;
		tname = strchr(nexthost, '|'); if (tname) { *tname = '\0'; tname++; }

		if (hname && tname && strcmp(hname, "summary") && strcmp(tname, xgetenv("INFOCOLUMN")) && strcmp(tname, xgetenv("TRENDSCOLUMN"))) {
			htnames_t *newitem = (htnames_t *)malloc(sizeof(htnames_t));

			for (hwalk = hosthead; (hwalk && strcmp(hwalk->hostname, hname)); hwalk = hwalk->next);
			if (!hwalk) {
				hwalk = (hostlist_t *)calloc(1, sizeof(hostlist_t));
				hwalk->hostname = strdup(hname);
				hwalk->procs = get_proclist(hname, procsbuf);
				hwalk->svcs  = get_proclist(hname, svcsbuf);
				hwalk->next = hosthead;
				hosthead = hwalk;
				hostcount++;
			}

			newitem->name = strdup(tname);
			newitem->next = hwalk->tests;
			hwalk->tests = newitem;
			hwalk->testcount++;
		}

		if (eoln) {
			nexthost = eoln+1;
			if (*nexthost == '\0') nexthost = NULL;
		}
	} while (nexthost);

	allhosts = (hostlist_t **) malloc(hostcount * sizeof(hostlist_t *));
	for (hwalk = hosthead, hosti=0; (hwalk); hwalk = hwalk->next, hosti++) {
		allhosts[hosti] = hwalk;
		if (hwalk->testcount > maxtests) maxtests = hwalk->testcount;
	}
	alltests = (htnames_t **) malloc(maxtests * sizeof(htnames_t *));
	qsort(&allhosts[0], hostcount, sizeof(hostlist_t **), host_compare);

	/* Get the static info */
	load_hostnames(xgetenv("BBHOSTS"), NULL, get_fqdn());
	load_all_links();
	init_tcp_services();
	pingcolumn = xgetenv("PINGCOLUMN");
	pingplus = (char *)malloc(strlen(pingcolumn) + 2);
	sprintf(pingplus, "%s=", pingcolumn);

	/* Load alert config */
	alertcolors = colorset(xgetenv("ALERTCOLORS"), ((1 << COL_GREEN) | (1 << COL_BLUE)));
	alertinterval = 60*atoi(xgetenv("ALERTREPEAT"));
	sprintf(configfn, "%s/etc/hobbit-alerts.cfg", xgetenv("BBHOME"));
	load_alertconfig(configfn, alertcolors, alertinterval);
	load_columndocs();


	printf("Content-Type: text/html\n\n");
	sethostenv("", "", "", colorname(COL_BLUE), NULL);
	headfoot(stdout, "confreport", "", "header", COL_BLUE);

	fprintf(stdout, "<table width=\"100%%\" border=0>\n");
	fprintf(stdout, "<tr><th align=center colspan=2><font size=\"+2\">Hobbit configuration Report</font></th></tr>\n");
	fprintf(stdout, "<tr><th valign=top align=left>Date</th><td>%s</td></tr>\n", ctime(&now));
	fprintf(stdout, "<tr><th valign=top align=left>%d hosts included</th><td>\n", hostcount);
	for (hosti=0; (hosti < hostcount); hosti++) {
		fprintf(stdout, "%s ", allhosts[hosti]->hostname);
	}
	fprintf(stdout, "</td></tr>\n");
	fprintf(stdout, "</table>\n");

	headfoot(stdout, "confreport", "", "front", COL_BLUE);

	for (hosti=0; (hosti < hostcount); hosti++) {
		for (twalk = allhosts[hosti]->tests, testi = 0; (twalk); twalk = twalk->next, testi++) {
			alltests[testi] = twalk;
		}
		qsort(&alltests[0], allhosts[hosti]->testcount, sizeof(htnames_t **), test_compare);

		print_host(allhosts[hosti], alltests, allhosts[hosti]->testcount);
	}

	headfoot(stdout, "confreport", "", "back", COL_BLUE);
	print_columndocs();

	headfoot(stdout, "confreport", "", "footer", COL_BLUE);

	return 0;
}

