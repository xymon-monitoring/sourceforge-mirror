/*----------------------------------------------------------------------------*/
/* Xymon monitor library.                                                     */
/*                                                                            */
/* This is a library module, part of libxymon.                                */
/* It contains routines for working with RRD graphs.                          */
/*                                                                            */
/* Copyright (C) 2002-2011 Henrik Storner <henrik@storner.dk>                 */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

static char rcsid[] = "$Id$";

#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "libxymon.h"
#include "version.h"

/* This is for mapping a status-name -> RRD file */
xymonrrd_t *xymonrrds = NULL;
void * xymonrrdtree;

/* This is the information needed to generate links on the trends column page  */
xymongraph_t *xymongraphs = NULL;

static const char *xymonlinkfmt = "<table summary=\"%s Graph\"><tr><td><A HREF=\"%s&amp;action=menu\"><IMG BORDER=0 SRC=\"%s&amp;graph=hourly&amp;action=view\" ALT=\"xymongraph %s\"></A></td><td> <td align=\"left\" valign=\"top\"> <a href=\"%s&amp;graph=custom&amp;action=selzoom\"> <img src=\"%s/zoom.%s\" border=0 alt=\"Zoom graph\" style='padding: 3px'> </a> </td></tr></table>\n";


void rrd_destroy(void)
{
	/* 
	 * Must free any old data first.
	 * NB: These lists are NOT null-terminated ! 
	 *     Stop when svcname becomes a NULL.
	 */
	
	dbgprintf(" - performing rrd_destroy()\n");
	
	if (xymonrrds != NULL) {
	    xymonrrd_t *lrec;
	    lrec = xymonrrds;
	    while (lrec && lrec->svcname) {
		if (lrec->xymonrrdname != lrec->svcname) xfree(lrec->xymonrrdname);
		xfree(lrec->svcname);
		lrec++;
	    }
	    xfree(xymonrrds);
	}

	if (xymongraphs != NULL) {
	    xymongraph_t *grec;
	    grec = xymongraphs;
	    while (grec && grec->xymonrrdname) {
		if (grec->xymonpartname) xfree(grec->xymonpartname);
		xfree(grec->xymonrrdname);
		grec++;
	    }
	    xfree(xymongraphs);
	}

	xtreeDestroy(xymonrrdtree);
}

/*
 * Define the mapping between Xymon columns and RRD graphs.
 * Normally they are identical, but some RRD's use different names.
 */
static void rrd_setup(void)
{
	SBUF_DEFINE(lenv);
	char *ldef, *p, *services;
	SBUF_DEFINE(tcptests);
	int count;
	xymonrrd_t *lrec;
	xymongraph_t *grec;


	rrd_destroy();

	/* Get the tcp services, and count how many there are */
	services = init_tcp_services();
	SBUF_MALLOC(tcptests, strlen(services)+1);
#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6)
	#pragma GCC diagnostic push
	#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif  // __GNUC__
	strncpy(tcptests, services, tcptests_buflen);
#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6)
	#pragma GCC diagnostic pop
#endif  // __GNUC__
	count = 0; p = strtok(tcptests, " "); while (p) { count++; p = strtok(NULL, " "); }
	strncpy(tcptests, services, tcptests_buflen);

	/* Setup the xymonrrds table, mapping test-names to RRD files */
	SBUF_MALLOC(lenv, strlen(xgetenv("TEST2RRD")) + strlen(tcptests) + count*strlen(",=tcp") + 1);
	strncpy(lenv, xgetenv("TEST2RRD"), lenv_buflen); 
	p = lenv+strlen(lenv)-1; if (*p == ',') *p = '\0';	/* Drop a trailing comma */
	p = strtok(tcptests, " "); 
	while (p) {
		unsigned int curlen = strlen(lenv);
		snprintf(lenv+curlen, (lenv_buflen - curlen), ",%s=tcp", p); 
		p = strtok(NULL, " ");
	}
	xfree(tcptests);
	xfree(services);

	count = 0; p = lenv; do { count++; p = strchr(p+1, ','); } while (p);
	xymonrrds = (xymonrrd_t *)calloc((count+1), sizeof(xymonrrd_t));

	xymonrrdtree = xtreeNew(strcasecmp);
	lrec = xymonrrds; ldef = strtok(lenv, ",");
	while (ldef) {
		p = strchr(ldef, '=');
		if (p) {
			*p = '\0'; 
			lrec->svcname = strdup(ldef);
			lrec->xymonrrdname = strdup(p+1);
		}
		else {
			lrec->svcname = lrec->xymonrrdname = strdup(ldef);
		}
		xtreeAdd(xymonrrdtree, lrec->svcname, lrec);

		ldef = strtok(NULL, ",");
		lrec++;
	}
	xfree(lenv);

	/* Setup the xymongraphs table, describing how to make graphs from an RRD */
	lenv = strdup(xgetenv("GRAPHS"));
	p = lenv+strlen(lenv)-1; if (*p == ',') *p = '\0';	/* Drop a trailing comma */
	count = 0; p = lenv; do { count++; p = strchr(p+1, ','); } while (p);
	xymongraphs = (xymongraph_t *)calloc((count+1), sizeof(xymongraph_t));

	grec = xymongraphs; ldef = strtok(lenv, ",");
	while (ldef) {
		p = strchr(ldef, ':');
		if (p) {
			*p = '\0'; 
			grec->xymonrrdname = strdup(ldef);
			grec->xymonpartname = strdup(p+1);
			p = strchr(grec->xymonpartname, ':');
			if (p) {
				*p = '\0';
				grec->maxgraphs = atoi(p+1);
				if (strlen(grec->xymonpartname) == 0) {
					xfree(grec->xymonpartname);
					grec->xymonpartname = NULL;
				}
			}
		}
		else {
			grec->xymonrrdname = strdup(ldef);
		}

		ldef = strtok(NULL, ",");
		grec++;
	}
	xfree(lenv);

	/* Mark that we've done this at least once -- for those who aren't tracking it on their own */
	dbgprintf(" - performed rrd_setup()\n");
}


xymonrrd_t *find_xymon_rrd(char *service, char *flags)
{
	/* Lookup an entry in the xymonrrds table */
	xtreePos_t handle;

	if (xymonrrdtree == NULL) rrd_setup();

	handle = xtreeFind(xymonrrdtree, service);
	if (handle == xtreeEnd(xymonrrdtree)) 
		return NULL;
	else {
		return (xymonrrd_t *)xtreeData(xymonrrdtree, handle);
	}
}

xymongraph_t *find_xymon_graph(char *rrdname)
{
	/* Lookup an entry in the xymongraphs table */
	xymongraph_t *grec;
	int found = 0;
	char *dchar;

	if (xymonrrdtree == NULL) rrd_setup();

	grec = xymongraphs; 
	while (!found && (grec->xymonrrdname != NULL)) {
		found = (strncmp(grec->xymonrrdname, rrdname, strlen(grec->xymonrrdname)) == 0);
		if (found) {
			/* Check that it's not a partial match, e.g. "ftp" matches "ftps" */
			dchar = rrdname + strlen(grec->xymonrrdname);
			if ( (*dchar != '.') && (*dchar != ',') && (*dchar != '\0') ) found = 0;
		}

		if (!found) grec++;
	}

	return (found ? grec : NULL);
}


static char *xymon_graph_text(char *hostname, char *dispname, char *service, int bgcolor,
			      xymongraph_t *graphdef, int itemcount, hg_stale_rrds_t nostale, const char *fmt,
			      int locatorbased, time_t starttime, time_t endtime)
{
	STATIC_SBUF_DEFINE(rrdurl);
	static int gwidth = 0, gheight = 0;
	SBUF_DEFINE(svcurl);
	int rrdparturlsize;
	char rrdservicename[100];
	char *cgiurl = xgetenv("CGIBINURL");

	if (locatorbased) {
		char *qres = locator_query(hostname, ST_RRD, &cgiurl);
		if (!qres) {
			errprintf("Cannot find RRD files for host %s\n", hostname);
			return "";
		}
	}

	if (!gwidth) {
		gwidth = atoi(xgetenv("RRDWIDTH"));
		gheight = atoi(xgetenv("RRDHEIGHT"));
	}

	dbgprintf("rrdlink_url: host %s, rrd %s (partname:%s, maxgraphs:%d, count=%d)\n", 
		hostname, 
		graphdef->xymonrrdname, textornull(graphdef->xymonpartname), graphdef->maxgraphs, itemcount);

	if ((service != NULL) && (strcmp(graphdef->xymonrrdname, "tcp") == 0)) {
		snprintf(rrdservicename, sizeof(rrdservicename), "tcp:%s", service);
	}
	else if ((service != NULL) && (strcmp(graphdef->xymonrrdname, "ncv") == 0)) {
		snprintf(rrdservicename, sizeof(rrdservicename), "ncv:%s", service);
	}
	else if ((service != NULL) && (strcmp(graphdef->xymonrrdname, "devmon") == 0)) {
		snprintf(rrdservicename, sizeof(rrdservicename), "devmon:%s", service);
	}
	else {
		strncpy(rrdservicename, graphdef->xymonrrdname, sizeof(rrdservicename));
		rrdservicename[sizeof(rrdservicename)-1] = '\0'; /* Make sure it is null terminated */
	}

	SBUF_MALLOC(svcurl, 
		    2048                    + 
		    strlen(cgiurl)          +
		    strlen(hostname)        + 
		    strlen(rrdservicename)  + 
		    strlen(urlencode(dispname ? dispname : hostname)));

	rrdparturlsize = 2048 +
			 strlen(fmt)        +
			 3*svcurl_buflen    +
			 strlen(rrdservicename) +
			 strlen(xgetenv("XYMONSKIN")) +
			 strlen(xgetenv("IMAGEFILETYPE"));

	if (rrdurl == NULL) {
		SBUF_MALLOC(rrdurl, rrdparturlsize);
	}
	*rrdurl = '\0';

	{
		SBUF_DEFINE(rrdparturl);
		int first = 1;
		int step;

		step = (graphdef->maxgraphs ? graphdef->maxgraphs : 5);
		if (itemcount) {
			int gcount = (itemcount / step); if ((gcount*step) != itemcount) gcount++;
			step = (itemcount / gcount);
		}

		SBUF_MALLOC(rrdparturl, rrdparturlsize);
		do {
			if (itemcount > 0) {
				snprintf(svcurl, svcurl_buflen, 
					"%s/showgraph.sh?host=%s&amp;service=%s&amp;graph_width=%d&amp;graph_height=%d&amp;first=%d&amp;count=%d", 
					cgiurl, hostname, rrdservicename, 
					gwidth, gheight,
					first, step);
			}
			else {
				snprintf(svcurl, svcurl_buflen,
					"%s/showgraph.sh?host=%s&amp;service=%s&amp;graph_width=%d&amp;graph_height=%d", 
					cgiurl, hostname, rrdservicename,
					gwidth, gheight);
			}

			strncat(svcurl, "&amp;disp=", (svcurl_buflen - strlen(svcurl)));
			strncat(svcurl, urlencode(dispname ? dispname : hostname), (svcurl_buflen - strlen(svcurl)));

			if (nostale == HG_WITHOUT_STALE_RRDS) strncat(svcurl, "&amp;nostale", (svcurl_buflen - strlen(svcurl)));
			if (bgcolor != -1) snprintf(svcurl+strlen(svcurl), (svcurl_buflen - strlen(svcurl)), "&amp;color=%s", colorname(bgcolor));
			snprintf(svcurl+strlen(svcurl), (svcurl_buflen - strlen(svcurl)), "&amp;graph_start=%d&amp;graph_end=%d", (int)starttime, (int)endtime);

			snprintf(rrdparturl, rrdparturl_buflen, fmt, rrdservicename, svcurl, svcurl, rrdservicename, svcurl, xgetenv("XYMONSKIN"), xgetenv("IMAGEFILETYPE"));
			if ((strlen(rrdparturl) + strlen(rrdurl) + 1) >= rrdurl_buflen) {
				SBUF_REALLOC(rrdurl, rrdurl_buflen + strlen(rrdparturl) + 4096);
			}
			strncat(rrdurl, rrdparturl, (rrdurl_buflen - strlen(rrdurl)));
			first += step;
		} while (first <= itemcount);
		xfree(rrdparturl);
	}

	dbgprintf("URLtext: %s\n", rrdurl);

	xfree(svcurl);

	return rrdurl;
}

char *xymon_graph_data(char *hostname, char *dispname, char *service, int bgcolor,
			xymongraph_t *graphdef, int itemcount,
			hg_stale_rrds_t nostale, int locatorbased,
			time_t starttime, time_t endtime)
{
	return xymon_graph_text(hostname, dispname, 
				 service, bgcolor, graphdef, 
				 itemcount, nostale,
				 xymonlinkfmt,
				 locatorbased, starttime, endtime);
}


rrdtpldata_t *setup_template(char *params[])
{
	int i;
	rrdtpldata_t *result;
	rrdtplnames_t *nam;
	int dsindex = 1;

	result = (rrdtpldata_t *)calloc(1, sizeof(rrdtpldata_t));
	result->dsnames = xtreeNew(strcmp);

	for (i = 0; (params[i]); i++) {
		if (strncasecmp(params[i], "DS:", 3) == 0) {
			char *pname, *pend;

			pname = params[i] + 3;
			pend = strchr(pname, ':');
			if (pend) {
				int plen = (pend - pname);

				nam = (rrdtplnames_t *)calloc(1, sizeof(rrdtplnames_t));
				nam->idx = dsindex++;

				if (result->template == NULL) {
					result->template = (char *)malloc(plen + 1);
					*result->template = '\0';
					nam->dsnam = (char *)malloc(plen+1); strncpy(nam->dsnam, pname, plen); nam->dsnam[plen] = '\0';
				}
				else {
					/* Hackish way of getting the colon delimiter */
					pname--; plen++;
					result->template = (char *)realloc(result->template, strlen(result->template) + plen + 1);
					nam->dsnam = (char *)malloc(plen); strncpy(nam->dsnam, pname+1, plen-1); nam->dsnam[plen-1] = '\0';
				}
				strncat(result->template, pname, plen);

				xtreeAdd(result->dsnames, nam->dsnam, nam);
			}
		}
	}

	return result;
}


