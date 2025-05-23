/*----------------------------------------------------------------------------*/
/* Xymon status-log viewer CGI.                                               */
/*                                                                            */
/* This CGI tool shows an HTML version of a status log.                       */
/*                                                                            */
/* Copyright (C) 2004-2011 Henrik Storner <henrik@storner.dk>                 */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

static char rcsid[] = "$Id$";

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <libgen.h>

#include "libxymon.h"
#include "version.h"
#include "svcstatus-info.h"
#include "svcstatus-trends.h"

/* Command-line params */
static enum { SRC_XYMOND, SRC_HISTLOGS, SRC_CLIENTLOGS } source = SRC_XYMOND;
static int wantserviceid = 1;
SBUF_DEFINE(multigraphs);
static int locatorbased = 0;
static char *critconfigfn = NULL;
static char *accessfn = NULL;

/* CGI params */
static char *hostname = NULL;
static char *service = NULL;
static char *tstamp = NULL;
static char *skiploc = NULL;
static char *nkprio = NULL, *nkttgroup = NULL, *nkttextra = NULL;
static enum { FRM_STATUS, FRM_CLIENT } outform = FRM_STATUS;
STATIC_SBUF_DEFINE(clienturi);
static int backsecs = 0;
static time_t fromtime = 0, endtime = 0;

static char errortxt[1000];
STATIC_SBUF_DEFINE(hostdatadir);


static void errormsg(int status, char *msg)
{
	snprintf(errortxt, sizeof(errortxt),
		 "Status: %d\nRefresh: 30\nContent-type: %s\n\n<html><head><title>Invalid request</title></head>\n<body>%s</body></html>\n", 
		 status, xgetenv("HTMLCONTENTTYPE"), msg);

	errortxt[sizeof(errortxt)-1] = '\0';
}

static int parse_query(void)
{
	cgidata_t *cgidata = cgi_request();
	cgidata_t *cwalk;

	cwalk = cgidata;
	while (cwalk) {
		if (strcasecmp(cwalk->name, "HOST") == 0) {
			hostname = strdup(basename(cwalk->value));
		}
		else if (strcasecmp(cwalk->name, "SERVICE") == 0) {
			service = strdup(basename(cwalk->value));
		}
		else if (strcasecmp(cwalk->name, "HOSTSVC") == 0) {
			/* For backwards compatibility */
			char *p = strrchr(cwalk->value, '.');
			if (p) {
				*p = '\0';
				hostname = strdup(basename(cwalk->value));
				service = strdup(p+1);
				for (p=strchr(hostname, ','); (p); p = strchr(p, ',')) *p = '.';
			}
		}
		else if (strcasecmp(cwalk->name, "TIMEBUF") == 0) {
			/* Only for the historical logs */
			tstamp = strdup(basename(cwalk->value));
		}
		else if (strcasecmp(cwalk->name, "CLIENT") == 0) {
			char *p;

			hostname = strdup(cwalk->value);
			p = hostname; while ((p = strchr(p, ',')) != NULL) *p = '.';
			service = strdup("");
			outform = FRM_CLIENT;
		}
		else if (strcasecmp(cwalk->name, "SECTION") == 0) {
			service = strdup(cwalk->value);
		}
		else if (strcasecmp(cwalk->name, "NKPRIO") == 0) {
			nkprio = strdup(cwalk->value);
		}
		else if (strcasecmp(cwalk->name, "NKTTGROUP") == 0) {
			nkttgroup = strdup(cwalk->value);
		}
		else if (strcasecmp(cwalk->name, "NKTTEXTRA") == 0) {
			nkttextra = strdup(cwalk->value);
		}
		else if ((strcasecmp(cwalk->name, "SKIPLOC") == 0)   && cwalk->value && strlen(cwalk->value) && (strcmp(cwalk->value, "1") == 0)) {
			skiploc = strdup(cwalk->value);
		}
		else if ((strcmp(cwalk->name, "backsecs") == 0)   && cwalk->value && strlen(cwalk->value)) {
			backsecs += atoi(cwalk->value);
		}
		else if ((strcmp(cwalk->name, "backmins") == 0)   && cwalk->value && strlen(cwalk->value)) {
			backsecs += 60*atoi(cwalk->value);
		}
		else if ((strcmp(cwalk->name, "backhours") == 0)   && cwalk->value && strlen(cwalk->value)) {
			backsecs += 60*60*atoi(cwalk->value);
		}
		else if ((strcmp(cwalk->name, "backdays") == 0)   && cwalk->value && strlen(cwalk->value)) {
			backsecs += 24*60*60*atoi(cwalk->value);
		}
		else if ((strcmp(cwalk->name, "FROMTIME") == 0)   && cwalk->value && strlen(cwalk->value)) {
			fromtime = eventreport_time(cwalk->value);
		}
		else if ((strcmp(cwalk->name, "TOTIME") == 0)   && cwalk->value && strlen(cwalk->value)) {
			endtime = eventreport_time(cwalk->value);
		}

		cwalk = cwalk->next;
	}

	if (backsecs == 0) {
		if (getenv("TRENDSECONDS")) backsecs = atoi(getenv("TRENDSECONDS"));
		else backsecs = 48*60*60;
	}

	if (!hostname || !service || ((source == SRC_HISTLOGS) && !tstamp) ) {
		errormsg(403, "Invalid request");
		return 1;
	}

	if (strcmp(service, xgetenv("CLIENTCOLUMN")) == 0) {
		/* Make this a client request */
		char *p = strdup(basename(hostname));
		xfree(hostname); hostname = p;	/* no need to convert to dots, since we'll already have them */
		xfree(service);			/* service does double-duty as the 'section' param */
		outform = FRM_CLIENT;
	}

	if (outform == FRM_STATUS) {
		char *p, *req;
		char *hostquoted = htmlquoted(hostname);

		req = getenv("SCRIPT_NAME");
		SBUF_MALLOC(clienturi, strlen(req) + 10 + strlen(hostquoted));
#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6)
		#pragma GCC diagnostic push
		#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif  // __GNUC__
		strncpy(clienturi, req, clienturi_buflen);
#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6)
		#pragma GCC diagnostic pop
#endif  // __GNUC__
		p = strchr(clienturi, '?'); if (p) *p = '\0'; else p = clienturi + strlen(clienturi);
		snprintf(p, (clienturi_buflen - (clienturi - p)), "?CLIENT=%s", hostquoted);
	}

	return 0;
}

int loadhostdata(char *hostname, char **ip, char **displayname, char **compacts, int full)
{
	void *hinfo = NULL;
	int loadres;

	if (full) {
		loadres = load_hostnames(xgetenv("HOSTSCFG"), NULL, get_fqdn());
	}
	else {
		loadres = load_hostinfo(hostname);
	}

	if ((loadres != 0) && (loadres != -2)) {
		errormsg(500, "Cannot load host configuration");
		return 1;
	}

	if ((loadres == -2) || (hinfo = hostinfo(hostname)) == NULL) {
		errormsg(403, "No such host");
		return 1;
	}

	*ip = xmh_item(hinfo, XMH_IP);
	*displayname = xmh_item(hinfo, XMH_DISPLAYNAME);
	if (!(*displayname)) *displayname = hostname;
	*compacts = xmh_item(hinfo, XMH_COMPACT);

	return 0;
}

int do_request(void)
{
	int color = 0, flapping = 0;
	char timesincechange[100];
	time_t logtime = 0, acktime = 0, disabletime = 0;
	SBUF_DEFINE(firstline);
	char *log = NULL, *sender = NULL, *clientid = NULL, *flags = NULL;	/* These are free'd */
	char *restofmsg = NULL, *ackmsg = NULL, *dismsg = NULL, *acklist=NULL, *modifiers = NULL;	/* These are just used */
	int ishtmlformatted = 0;
	int clientavail = 0;
	char *ip = NULL, *displayname = NULL, *compacts;

	if (parse_query() != 0) return 1;

	{ 
	  char *p = NULL;
	  if (!service || !strlen(service)) p = csp_header("svcstatus");
	  else {
		if (strcasecmp(service, xgetenv("INFOCOLUMN")) == 0) p = csp_header("svcstatus-info");
		else if (strcasecmp(service, xgetenv("TRENDSCOLUMN")) == 0) p = csp_header("svcstatus-trends");
		else {
			int d = atoi(xgetenv("XYMWEBREFRESH"));
			fprintf(stdout, "Refresh: %d\n", ((d > 0) ? d : 60) );
			p = csp_header("svcstatus");
		}
	  }
	  if (p) fprintf(stdout, "%s", p);
	}
	/* Load the host data (for access control) */
	if (accessfn) {
		load_hostinfo(hostname);
		load_web_access_config(accessfn);
		if (!web_access_allowed(getenv("REMOTE_USER"), hostname, service, WEB_ACCESS_VIEW)) {
			errormsg(403, "Not available (restricted).");
			return 1;
		}
	}

	{
		char *s;
		
		s = xgetenv("CLIENTLOGS"); 
		if (s) {
			SBUF_MALLOC(hostdatadir, strlen(s) + strlen(hostname) + 12);
			snprintf(hostdatadir, hostdatadir_buflen, "%s/%s", s, hostname);
		}
		else {
			s = xgetenv("XYMONVAR");
			SBUF_MALLOC(hostdatadir, strlen(s) + strlen(hostname) + 12);
			snprintf(hostdatadir, hostdatadir_buflen, "%s/hostdata/%s", s, hostname);
		}
	}

	if (outform == FRM_CLIENT) {
		if (source == SRC_XYMOND) {
			SBUF_DEFINE(xymondreq);
			int xymondresult;
			sendreturn_t *sres = newsendreturnbuf(1, NULL);

			SBUF_MALLOC(xymondreq, 1024 + strlen(hostname) + (service ? strlen(service) : 0));
			snprintf(xymondreq, xymondreq_buflen, "clientlog %s", hostname);
			if (service && *service) snprintf(xymondreq + strlen(xymondreq), (xymondreq_buflen - strlen(xymondreq)), " section=%s", service);

			xymondresult = sendmessage(xymondreq, NULL, XYMON_TIMEOUT, sres);
			if (xymondresult != XYMONSEND_OK) {
				SBUF_DEFINE(errtxt);

				SBUF_MALLOC(errtxt, 1024 + MAX_HTMLQUOTE_FACTOR*strlen(xymondreq));
				snprintf(errtxt, errtxt_buflen, "Status not available: %s\nRequest: %s\n", strxymonsendresult(xymondresult), htmlquoted(xymondreq));
				errormsg(500, errtxt);
				return 1;
			}
			else {
				log = getsendreturnstr(sres, 1);
			}
			freesendreturnbuf(sres);
		}
		else if (source == SRC_HISTLOGS) {
			char logfn[PATH_MAX];
			FILE *fd;

			snprintf(logfn, sizeof(logfn), "%s/%s", hostdatadir, tstamp);
			fd = fopen(logfn, "r");
			if (!fd && (strtol(tstamp, NULL, 10) != 0)) {
				sprintf(logfn, "%s/%s", hostdatadir, histlogtime((time_t)atoi(tstamp)));
				fd = fopen(logfn, "r");
			}
			if (fd) {
				struct stat st;
				int n;

				fstat(fileno(fd), &st);
				if (S_ISREG(st.st_mode)) {
					log = (char *)malloc(st.st_size + 1);
					n = fread(log, 1, st.st_size, fd);
					if (n >= 0) *(log+n) = '\0'; else *log = '\0';
				}
				fclose(fd);
			}
		}

		restofmsg = (log ? log : strdup("<No data>\n"));
	}
	else if ((strcmp(service, xgetenv("TRENDSCOLUMN")) == 0) || (strcmp(service, xgetenv("INFOCOLUMN")) == 0)) {
		int fullload = (strcmp(service, xgetenv("INFOCOLUMN")) == 0);

		if (loadhostdata(hostname, &ip, &displayname, &compacts, fullload) != 0) return 1;

		ishtmlformatted = 1;
		sethostenv(displayname, ip, service, colorname(COL_GREEN), hostname);
		sethostenv_refresh(600);
		color = COL_GREEN;
		logtime = getcurrenttime(NULL);
		strncpy(timesincechange, "0 minutes", sizeof(timesincechange));

		if (strcmp(service, xgetenv("TRENDSCOLUMN")) == 0) {
			if (locatorbased && !skiploc) {
				char *cgiurl, *qres;

				qres = locator_query(hostname, ST_RRD, &cgiurl);
				if (!qres) {
					errprintf("Cannot find RRD files for host %s\n", hostname);
				}
				else {
					/* Redirect browser to the real server */
					fprintf(stdout, "Location: %s/svcstatus.sh?HOST=%s&SERVICE=%s&SKIPLOC=1\n\n",
						cgiurl, hostname, service);
					return 0;
				}
			}
			else {
				if (endtime == 0) endtime = getcurrenttime(NULL);

				if (fromtime == 0) {
					fromtime = endtime - backsecs;
					sethostenv_backsecs(backsecs);
				}
				else {
					sethostenv_eventtime(fromtime, endtime);
				}

				log = restofmsg = generate_trends(hostname, fromtime, endtime);
			}
		}
		else if (strcmp(service, xgetenv("INFOCOLUMN")) == 0) {
			log = restofmsg = generate_info(hostname, critconfigfn);
		}
	}
	else if (source == SRC_XYMOND) {
		SBUF_DEFINE(xymondreq);
		int xymondresult;
		char *items[25];
		int icount;
		time_t logage, clntstamp;
		char *sumline, *msg, *compitem, *complist;
		sendreturn_t *sres;

		if (loadhostdata(hostname, &ip, &displayname, &compacts, 0) != 0) return 1;

		complist = NULL;
		if (compacts && *compacts) {
			char *p;

			compitem = strtok(compacts, ",");
			while (compitem && !complist) {
				p = strchr(compitem, '='); if (p) *p = '\0';
				if (strcmp(service, compitem) == 0) complist = p+1;
				compitem = strtok(NULL, ",");
			}
		}

		/* We need not check that hostname is valid, has already been done with loadhostdata() */
		if (!complist) {
			pcre *dummy = NULL;

			/* Check service as a pcre pattern. And no spaces in servicenames */
			if (strchr(service, ' ') == NULL) dummy = compileregex(service);
			if (dummy == NULL) {
				errormsg(500, "Invalid testname pattern");
				return 1;
			}

			freeregex(dummy);
			SBUF_MALLOC(xymondreq, 1024 + strlen(hostname) + strlen(service));
			snprintf(xymondreq, xymondreq_buflen, "xymondlog host=%s test=%s fields=hostname,testname,color,flags,lastchange,logtime,validtime,acktime,disabletime,sender,cookie,ackmsg,dismsg,client,acklist,XMH_IP,XMH_DISPLAYNAME,clntstamp,flapinfo,modifiers", hostname, service);
		}
		else {
			pcre *dummy = NULL;
			SBUF_DEFINE(re);

			SBUF_MALLOC(re, 5 + strlen(complist));
			snprintf(re, re_buflen, "^(%s)$", complist);
			dummy = compileregex(re);
			if (dummy == NULL) {
				errormsg(500, "Invalid testname pattern");
				return 1;
			}

			freeregex(dummy);
			SBUF_MALLOC(xymondreq, 1024 + strlen(hostname) + strlen(re));
			snprintf(xymondreq, xymondreq_buflen, "xymondboard host=^%s$ test=%s fields=testname,color,lastchange", hostname, re);
		}

		sres = newsendreturnbuf(1, NULL);
		xymondresult = sendmessage(xymondreq, NULL, XYMON_TIMEOUT, sres);
		if (xymondresult == XYMONSEND_OK) log = getsendreturnstr(sres, 1);
		freesendreturnbuf(sres);
		if ((xymondresult != XYMONSEND_OK) || (log == NULL) || (strlen(log) == 0)) {
			errormsg(404, "Status not available\n");
			return 1;
		}

		if (!complist) {
			char *p;

			sumline = log; p = strchr(log, '\n'); *p = '\0';
			msg = (p+1); p = strchr(msg, '\n');
			if (!p) {
				firstline = strdup(msg);
				restofmsg = NULL;
			}
			else { 
				*p = '\0'; 
				firstline = strdup(msg); 
				restofmsg = (p+1);
				*p = '\n'; 
			}

			memset(items, 0, sizeof(items));
			p = gettok(sumline, "|"); icount = 0;
			while (p && (icount < 20)) {
				items[icount++] = p;
				p = gettok(NULL, "|");
			}

			/*
			 * hostname,		[0]
			 * testname,		[1]
			 * color,		[2]
			 * flags,		[3]
			 * lastchange,		[4]
			 * logtime,		[5]
			 * validtime,		[6]
			 * acktime,		[7]
			 * disabletime,		[8]
			 * sender,		[9]
			 * cookie,		[10]
			 * ackmsg,		[11]
			 * dismsg,		[12]
			 * client,		[13]
			 * acklist		[14]
			 * XMH_IP		[15]
			 * XMH_DISPLAYNAME	[16]
			 * clienttstamp         [17]
			 * flapping		[18]
			 * modifiers		[19]
			 */
			color = parse_color(items[2]);
			flags = strdup(items[3]);
			logage = getcurrenttime(NULL) - atoi(items[4]);
			timesincechange[0] = '\0';
			p = timesincechange;
			{
				int days = (int) (logage / 86400);
				int hours = (int) ((logage % 86400) / 3600);
				int minutes = (int) ((logage % 3600) / 60);

				if (days > 1) p += snprintf(p, (sizeof(timesincechange) - (p - timesincechange)), "%d days, ", days);
				else if (days == 1) p += snprintf(p, (sizeof(timesincechange) - (p - timesincechange)), "1 day, ");

				if (hours == 1) p += snprintf(p, (sizeof(timesincechange) - (p - timesincechange)), "1 hour, ");
				else p += snprintf(p, (sizeof(timesincechange) - (p - timesincechange)), "%d hours, ", hours);

				if (minutes == 1) p += snprintf(p, (sizeof(timesincechange) - (p - timesincechange)), "1 minute");
				else p += snprintf(p, (sizeof(timesincechange) - (p - timesincechange)), "%d minutes", minutes);
			}
			logtime = atoi(items[5]);
			if (items[7] && strlen(items[7])) acktime = atoi(items[7]);
			if (items[8] && strlen(items[8])) disabletime = atoi(items[8]);
			sender = strdup(items[9]);

			if (items[11] && strlen(items[11])) ackmsg = items[11];
			if (ackmsg) nldecode(ackmsg);

			if (items[12] && strlen(items[12])) dismsg = items[12];
			if (dismsg) nldecode(dismsg);

			if (items[13]) clientavail = (*items[13] == 'Y');

			acklist = ((items[14] && *items[14]) ? strdup(items[14]) : NULL);

			ip = (items[15] ? items[15] : "");
			displayname = ((items[16]  && *items[16]) ? items[16] : hostname);
			clntstamp = ((items[17]  && *items[17]) ? atol(items[17]) : 0);
			flapping = (items[18] ? (*items[18] == '1') : 0);
			modifiers = (items[19] && *(items[19])) ? items[19] : NULL;

			sethostenv(displayname, ip, service, colorname(COL_GREEN), hostname);
			sethostenv_refresh(60);
		}
		else {
			/* Compressed status display */
			strbuffer_t *cmsg;
			char *row, *p_row, *p_fld;
			SBUF_DEFINE(nonhistenv);

			color = COL_GREEN;

			cmsg = newstrbuffer(0);
			addtobuffer(cmsg, "<table width=\"80%\" summary=\"Compacted Status Info\">\n");

			row = strtok_r(log, "\n", &p_row);
			while (row) {
				/* testname,color,lastchange */
				char *testname, *itmcolor, *chgs;
				time_t lastchange;
				int icolor;

				testname = strtok_r(row, "|", &p_fld);
				itmcolor = strtok_r(NULL, "|", &p_fld);
				chgs = strtok_r(NULL, "|", &p_fld);
				lastchange = atoi(chgs);

				icolor = parse_color(itmcolor);
				if (icolor > color) color = icolor;

				addtobuffer(cmsg, "<tr><td align=left>&");
				addtobuffer(cmsg, itmcolor);
				addtobuffer(cmsg, "&nbsp;<a href=\"");
				addtobuffer(cmsg, hostsvcurl(hostname, testname, 1));
				addtobuffer(cmsg, "\">");
				addtobuffer(cmsg, htmlquoted(testname));
				addtobuffer(cmsg, "</a></td></tr>\n");

				row = strtok_r(NULL, "\n", &p_row);
			}

			addtobuffer(cmsg, "</table>\n");
			ishtmlformatted = 1;

			sethostenv(displayname, ip, service, colorname(color), hostname);
			sethostenv_refresh(60);
			logtime = getcurrenttime(NULL);
			strncpy(timesincechange, "0 minutes", sizeof(timesincechange));

			log = restofmsg = grabstrbuffer(cmsg);

			SBUF_MALLOC(firstline, 1024);
			snprintf(firstline, firstline_buflen, "%s Compressed status display\n", colorname(color));

			SBUF_MALLOC(nonhistenv, 10 + strlen(service));
			snprintf(nonhistenv, nonhistenv_buflen, "NONHISTS=%s", service);
			putenv(nonhistenv);
		}
	}
	else if (source == SRC_HISTLOGS) {
		char logfn[PATH_MAX];
		struct stat st;
		FILE *fd;
		/*
		 * Some clients (Unix disk reports) don't have a newline before the
		 * "Status unchanged in ..." text. Most do, but at least Solaris and
		 * AIX do not. So just look for the text, not the newline.
		 */
		char *statusunchangedtext = "Status unchanged in ";
		char *receivedfromtext = "Message received from ";
		char *clientidtext = "Client data ID ";
		char *p, *unchangedstr, *receivedfromstr, *clientidstr, *hostnamedash;
		int n;

		if (!tstamp) { errormsg(500, "Invalid request"); return 1; }

		if (loadhostdata(hostname, &ip, &displayname, &compacts, 0) != 0) return 1;
		hostnamedash = strdup(hostname);
		p = hostnamedash; while ((p = strchr(p, '.')) != NULL) *p = '_';
		p = hostnamedash; while ((p = strchr(p, ',')) != NULL) *p = '_';
		snprintf(logfn, sizeof(logfn), "%s/%s/%s/%s", xgetenv("XYMONHISTLOGS"), hostnamedash, service, tstamp);
		if (((stat(logfn, &st) == -1) || (st.st_size < 10) || (!S_ISREG(st.st_mode))) && (strtol(tstamp, NULL, 10) != 0)) {
			snprintf(logfn, sizeof(logfn), "%s/%s/%s/%s", xgetenv("XYMONHISTLOGS"), hostnamedash, service, histlogtime((time_t)atoi(tstamp)));
		}
		xfree(hostnamedash);
		if (strtol(tstamp, NULL, 10) != 0) tstamp = strdup(histlogtime((time_t)atoi(tstamp)));
		p = tstamp; while ((p = strchr(p, '_')) != NULL) *p = ' ';
		sethostenv_histlog(tstamp);

		if ((stat(logfn, &st) == -1) || (st.st_size < 10) || (!S_ISREG(st.st_mode))) {
			errormsg(404, "Historical status log not available\n");
			return 1;
		}

		fd = fopen(logfn, "r");
		if (!fd) {
			errormsg(404, "Unable to access historical logfile\n");
			return 1;
		}
		log = (char *)malloc(st.st_size+1);
		n = fread(log, 1, st.st_size, fd);
		if (n >= 0) *(log+n) = '\0'; else *log = '\0';
		fclose(fd);

		p = strchr(log, '\n'); 
		if (!p) {
			firstline = strdup(log);
			restofmsg = NULL;
		}
		else { 
			*p = '\0'; 
			firstline = strdup(log); 
			restofmsg = (p+1);
			*p = '\n'; 
		}


		color = parse_color(log);

		p = strstr(log, "<!-- [flags:"); 
		if (p) {
			p += strlen("<!-- [flags:");
			n = strspn(p, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ");
			flags = (char *)malloc(n+1);
			strncpy(flags, p, n);
			*(flags + n) = '\0';
		}

		timesincechange[0] = '\0';

		p = clientidstr = strstr(restofmsg, clientidtext);
		if (p) {
			p += strlen(clientidtext);
			n = strspn(p, "0123456789");
			clientid = (char *)malloc(n+1);
			strncpy(clientid, p, n);
			*(clientid+n) = '\0';
		}

		p = unchangedstr = strstr(restofmsg, statusunchangedtext);
		if (p) {
			p += strlen(statusunchangedtext);
			n = strcspn(p, "\n"); if (n >= sizeof(timesincechange)) n = sizeof(timesincechange);
			strncpy(timesincechange, p, n);
			timesincechange[n] = '\0';
		}

		p = receivedfromstr = strstr(restofmsg, receivedfromtext); 
		if (p) {
			p += strlen(receivedfromtext);
			n = strspn(p, "0123456789.");
			sender = (char *)malloc(n+1);
			strncpy(sender, p, n);
			*(sender+n) = '\0';
		}

		/* Kill the "Status unchanged ..." and "Message received ..." lines */
		if (unchangedstr) *unchangedstr = '\0';
		if (receivedfromstr) *receivedfromstr = '\0';
	}

	if (outform == FRM_CLIENT) {
		fprintf(stdout, "Content-type: text/plain\n\n");
		fprintf(stdout, "%s", restofmsg);
	}
	else {
		if (clientid && (source == SRC_HISTLOGS)) {
			if (locatorbased) {
				char *cgiurl, *qres;

				qres = locator_query(hostname, ST_HOSTDATA, &cgiurl);
				if (!qres) {
					errprintf("Cannot find hostdata files for host %s\n", hostname);
				}
				else {
					SBUF_REALLOC(clienturi, 1024 + strlen(cgiurl) + MAX_HTMLQUOTE_FACTOR*strlen(htmlquoted(hostname)) + strlen(clientid));
					snprintf(clienturi, clienturi_buflen, "%s/svcstatus.sh?CLIENT=%s&amp;TIMEBUF=%s", 
						cgiurl, htmlquoted(hostname), clientid);
				}
			}
			else {
				char logfn[PATH_MAX];
				struct stat st;

				snprintf(logfn, sizeof(logfn), "%s/%s", hostdatadir, clientid);
				clientavail = (stat(logfn, &st) == 0);

				if (clientavail) {
					int curlen = strlen(clienturi);

					SBUF_REALLOC(clienturi, 1024 + curlen + strlen(clientid));
					snprintf(clienturi + curlen, (clienturi_buflen - curlen), "&amp;TIMEBUF=%s", clientid);
				}
			}
		}

		fprintf(stdout, "Content-type: %s\n\n", xgetenv("HTMLCONTENTTYPE"));
		generate_html_log(hostname, 
			  displayname,
			  service, 
			  ip,
		          color, flapping,
			  (sender ? sender : "Xymon"), 
			  (flags ? flags : ""),
		          logtime, timesincechange, 
		          (firstline ? firstline : ""), 
			  (restofmsg ? restofmsg : ""), 
			  modifiers,
			  acktime, ackmsg, acklist,
			  disabletime, dismsg,
		          (source == SRC_HISTLOGS), 
			  wantserviceid, 
			  ishtmlformatted,
			  locatorbased,
			  multigraphs, (clientavail ? clienturi : NULL),
			  nkprio, nkttgroup, nkttextra,
			  backsecs,
			  stdout);
	}

	/* Cleanup CGI params */
	if (hostname) xfree(hostname);
	if (service) xfree(service);
	if (tstamp) xfree(tstamp);
	if (skiploc) xfree(skiploc);

	/* Cleanup main vars */
	if (clientid) xfree(clientid);
	if (sender) xfree(sender);
	if (flags) xfree(flags);
	if (firstline) xfree(firstline);
	if (log) xfree(log);

	return 0;
}


int main(int argc, char *argv[])
{
	int argi;

	multigraphs = ",disk,inode,qtree,quotas,snapshot,TblSpace,if_load,";

	libxymon_init(argv[0]);
	for (argi = 1; (argi < argc); argi++) {
		if (strcmp(argv[argi], "--historical") == 0) {
			source = SRC_HISTLOGS;
		}
		else if (strncmp(argv[argi], "--history=", 10) == 0) {
			char *val = strchr(argv[argi], '=')+1;

			if (strcmp(val, "none") == 0)
				histlocation = HIST_NONE;
			else if (strcmp(val, "top") == 0)
				histlocation = HIST_TOP;
			else if (strcmp(val, "bottom") == 0)
				histlocation = HIST_BOTTOM;
		}
		else if (strcmp(argv[argi], "--no-svcid") == 0) {
			wantserviceid = 0;
		}
		else if (argnmatch(argv[argi], "--templates=")) {
			char *p = strchr(argv[argi], '=');
			sethostenv_template(p+1);
		}
		else if (argnmatch(argv[argi], "--multigraphs=")) {
			char *p = strchr(argv[argi], '=');
			SBUF_MALLOC(multigraphs, strlen(p+1) + 3);
			snprintf(multigraphs, multigraphs_buflen, ",%s,", p+1);
		}
		else if (strcmp(argv[argi], "--no-disable") == 0) {
			showenadis = 0;
		}
		else if (strcmp(argv[argi], "--no-jsvalidation") == 0) {
			usejsvalidation = 0;
		}
		else if (strcmp(argv[argi], "--old-critical-config") == 0) {
			newcritconfig = 0;
		}
		else if (argnmatch(argv[argi], "--locator=")) {
			char *p = strchr(argv[argi], '=');
			locator_init(p+1);
			locatorbased = 1;
		}
		else if (argnmatch(argv[argi], "--critical-config=")) {
			char *p = strchr(argv[argi], '=');
			critconfigfn = strdup(p+1);
		}
		else if (argnmatch(argv[argi], "--access=")) {
			char *p = strchr(argv[argi], '=');
			accessfn = strdup(p+1);
		}
		else if (standardoption(argv[argi])) {
			if (showhelp) return 0;
		}
	}

	redirect_cgilog(programname);

	*errortxt = '\0';
	hostname = service = tstamp = skiploc = NULL;
	if (do_request() != 0) {
		fprintf(stdout, "%s", errortxt);
		return 1;
	}

	return 0;
}

