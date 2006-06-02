/*----------------------------------------------------------------------------*/
/* Hobbit monitor network test tool.                                          */
/*                                                                            */
/* Copyright (C) 2003-2006 Henrik Storner <henrik@hswn.dk>                    */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

static char rcsid[] = "$Id: bbtest-net.c,v 1.235 2006-06-02 16:24:27 henrik Exp $";

#include <limits.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ctype.h>
#include <netdb.h>
#include <sys/wait.h>
#include <rpc/rpc.h>
#include <fcntl.h>
#include <errno.h>

#include "libbbgen.h"

#ifdef HAVE_RPCENT_H
#include <rpc/rpcent.h>
#endif

#include "libbbgen.h"
#include "version.h"

#include "bbtest-net.h"
#include "dns.h"
#include "contest.h"
#include "httptest.h"
#include "httpresult.h"
#include "ldaptest.h"

char *reqenv[] = {
	"NONETPAGE",
	"BBHOSTS",
	"BBTMP",
	"BBHOME",
	"BB",
	"BBDISP",
	NULL
};

/* toolid values */
#define TOOL_CONTEST	0
#define TOOL_NSLOOKUP	1
#define TOOL_DIG	2
#define TOOL_NTP        3
#define TOOL_FPING      4
#define TOOL_HTTP       5
#define TOOL_MODEMBANK  6
#define TOOL_LDAP	7
#define TOOL_RPCINFO	8


RbtHandle	svctree;			/* All known services, has service_t records */
service_t	*pingtest = NULL;		/* Identifies the pingtest within svctree list */
int		pingcount = 0;
service_t	*dnstest = NULL;		/* Identifies the dnstest within svctree list */
service_t	*digtest = NULL;		/* Identifies the digtest within svctree list */
service_t	*httptest = NULL;		/* Identifies the httptest within svctree list */
service_t	*ldaptest = NULL;		/* Identifies the ldaptest within svctree list */
service_t	*rpctest = NULL;		/* Identifies the rpctest within svctree list */
service_t	*modembanktest = NULL;		/* Identifies the modembank test within svctree list */
RbtHandle       testhosttree;			/* All tested hosts, has testedhost_t records */
char		*nonetpage = NULL;		/* The "NONETPAGE" env. variable */
int		dnsmethod = DNS_THEN_IP;	/* How to do DNS lookups */
int 		timeout=10;			/* The timeout (seconds) for all TCP-tests */
char		*contenttestname = "content";   /* Name of the content checks column */
char		*ssltestname = "sslcert";       /* Name of the SSL certificate checks column */
char		*failtext = "not OK";
int             sslwarndays = 30;		/* If cert expires in fewer days, SSL cert column = yellow */
int             sslalarmdays = 10;		/* If cert expires in fewer days, SSL cert column = red */
char		*location = "";			/* BBLOCATION value */
int		hostcount = 0;
int		testcount = 0;
int		notesthostcount = 0;
char		**selectedhosts;
int		selectedcount = 0;
int		testuntagged = 0;
time_t		frequenttestlimit = 1800;	/* Interval (seconds) when failing hosts are retried frequently */
int		checktcpresponse = 0;
int		dotraceroute = 0;
int		fqdn = 1;
int		dosendflags = 1;
char		*pingcmd = NULL;
char		pinglog[PATH_MAX];
char		pingerrlog[PATH_MAX];
int		respcheck_color = COL_YELLOW;
int		extcmdtimeout = 30;

void dump_hostlist(void)
{
	RbtIterator handle;
	testedhost_t *walk;

	for (handle = rbtBegin(testhosttree); (handle != rbtEnd(testhosttree)); handle = rbtNext(testhosttree, handle)) {
		walk = (testedhost_t *)gettreeitem(testhosttree, handle);
		printf("Hostname: %s\n", textornull(walk->hostname));
		printf("\tIP           : %s\n", textornull(walk->ip));
		printf("\tHosttype     : %s\n", textornull(walk->hosttype));

		printf("\tFlags        :");
		if (walk->testip) printf(" testip");
		if (walk->dialup) printf(" dialup");
		if (walk->nosslcert) printf(" nosslcert");
		if (walk->dodns) printf(" dodns");
		if (walk->dnserror) printf(" dnserror");
		if (walk->repeattest) printf(" repeattest");
		if (walk->noconn) printf(" noconn");
		if (walk->noping) printf(" noping");
		if (walk->dotrace) printf(" dotrace");
		printf("\n");

		printf("\tbadconn      : %d:%d:%d\n", walk->badconn[0], walk->badconn[1], walk->badconn[2]);
		printf("\tdowncount    : %d started %s", walk->downcount, ctime(&walk->downstart));
		printf("\trouterdeps   : %s\n", textornull(walk->routerdeps));
		printf("\tdeprouterdown: %s\n", (walk->deprouterdown ? textornull(walk->deprouterdown->hostname) : ""));
		printf("\tldapauth     : '%s' '%s'\n", textornull(walk->ldapuser), textornull(walk->ldappasswd));
		printf("\tSSL alerts   : %d:%d\n", walk->sslwarndays, walk->sslalarmdays);
		printf("\n");
	}
}
void dump_testitems(void)
{
	RbtIterator handle;
	service_t *swalk;
	testitem_t *iwalk;

	for (handle = rbtBegin(svctree); handle != rbtEnd(svctree); handle = rbtNext(svctree, handle)) {
		swalk = (service_t *)gettreeitem(svctree, handle);

		printf("Service %s, port %d, toolid %d\n", swalk->testname, swalk->portnum, swalk->toolid);

		for (iwalk = swalk->items; (iwalk); iwalk = iwalk->next) {
			if (swalk == modembanktest) {
				modembank_t *mentry;
				int i;

				mentry = iwalk->privdata;
				printf("\tModembank   : %s\n", textornull(mentry->hostname));
				printf("\tStart-IP    : %s\n", u32toIP(mentry->startip));
				printf("\tBanksize    : %d\n", mentry->banksize);
				printf("\tOpen        :");
				for (i=0; i<mentry->banksize; i++) printf(" %d", mentry->responses[i]);
				printf("\n");
			}
			else {
				printf("\tHost        : %s\n", textornull(iwalk->host->hostname));
				printf("\ttestspec    : %s\n", textornull(iwalk->testspec));
				printf("\tFlags       :");
				if (iwalk->dialup) printf(" dialup");
				if (iwalk->reverse) printf(" reverse");
				if (iwalk->silenttest) printf(" silenttest");
				if (iwalk->alwaystrue) printf(" alwaystrue");
				printf("\n");
				printf("\tOpen        : %d\n", iwalk->open);
				printf("\tBanner      : %s\n", textornull(STRBUF(iwalk->banner)));
				printf("\tcertinfo    : %s\n", textornull(iwalk->certinfo));
				printf("\tDuration    : %ld.%06ld\n", (long int)iwalk->duration.tv_sec, (long int)iwalk->duration.tv_usec);
				printf("\tbadtest     : %d:%d:%d\n", iwalk->badtest[0], iwalk->badtest[1], iwalk->badtest[2]);
				printf("\tdowncount    : %d started %s", iwalk->downcount, ctime(&iwalk->downstart));
				printf("\n");
			}
		}

		printf("\n");
	}
}

testitem_t *find_test(char *hostname, char *testname)
{
	RbtIterator handle;
	testedhost_t *h;
	service_t *s;
	testitem_t *t;

	handle = rbtFind(svctree, testname);
	if (handle == rbtEnd(svctree)) return NULL;
	s = (service_t *)gettreeitem(svctree, handle);

	handle = rbtFind(testhosttree, hostname);
	if (handle == rbtEnd(testhosttree)) return NULL;
	h = (testedhost_t *)gettreeitem(testhosttree, handle);

	for (t=s->items; (t && (t->host != h)); t = t->next) ;

	return t;
}


char *deptest_failed(testedhost_t *host, char *testname)
{
	static char result[1024];

	char *depcopy;
	char depitem[MAX_LINE_LEN];
	char *p, *q;
	char *dephostname, *deptestname, *nextdep;
	testitem_t *t;

	if (host->deptests == NULL) return NULL;

	depcopy = strdup(host->deptests);
	sprintf(depitem, "(%s:", testname);
	p = strstr(depcopy, depitem);
	if (p == NULL) { xfree(depcopy); return NULL; }

	result[0] = '\0';
	dephostname = p+strlen(depitem);
	q = strchr(dephostname, ')');
	if (q) *q = '\0';

	/* dephostname now points to a list of "host1/test1,host2/test2" dependent tests. */
	while (dephostname) {
		p = strchr(dephostname, '/');
		if (p) {
			*p = '\0';
			deptestname = (p+1); 
		}
		else deptestname = "";

		p = strchr(deptestname, ',');
		if (p) {
			*p = '\0';
			nextdep = (p+1);
		}
		else nextdep = NULL;

		t = find_test(dephostname, deptestname);
		if (t && !t->open) {
			if (strlen(result) == 0) {
				strcpy(result, "\nThis test depends on the following test(s) that failed:\n\n");
			}

			if ((strlen(result) + strlen(dephostname) + strlen(deptestname) + 2) < sizeof(result)) {
				strcat(result, dephostname);
				strcat(result, "/");
				strcat(result, deptestname);
				strcat(result, "\n");
			}
		}

		dephostname = nextdep;
	}

	xfree(depcopy);
	if (strlen(result)) strcat(result, "\n\n");

	return (strlen(result) ? result : NULL);
}


service_t *add_service(char *name, int port, int namelen, int toolid)
{
	RbtIterator handle;
	service_t *svc;

	/* Avoid duplicates */
	handle = rbtFind(svctree, name);
	if (handle != rbtEnd(svctree)) {
		svc = (service_t *)gettreeitem(svctree, handle);
		return svc;
	}

	svc = (service_t *) malloc(sizeof(service_t));
	svc->portnum = port;
	svc->testname = strdup(name); 
	svc->toolid = toolid;
	svc->namelen = namelen;
	svc->items = NULL;
	rbtInsert(svctree, svc->testname, svc);

	return svc;
}

int getportnumber(char *svcname)
{
	struct servent *svcinfo;
	int result = 0;

	result = default_tcp_port(svcname);
	if (result == 0) {
		svcinfo = getservbyname(svcname, NULL);
		if (svcinfo) result = ntohs(svcinfo->s_port);
	}

	return result;
}

void load_services(void)
{
	char *netsvcs;
	char *p;

	netsvcs = strdup(init_tcp_services());

	p = strtok(netsvcs, " ");
	while (p) {
		add_service(p, getportnumber(p), 0, TOOL_CONTEST);
		p = strtok(NULL, " ");
	}
	xfree(netsvcs);

	/* Save NONETPAGE env. var in ",test1,test2," format for easy and safe grepping */
	nonetpage = (char *) malloc(strlen(xgetenv("NONETPAGE"))+3);
	sprintf(nonetpage, ",%s,", xgetenv("NONETPAGE"));
	for (p=nonetpage; (*p); p++) if (*p == ' ') *p = ',';
}


testedhost_t *init_testedhost(char *hostname)
{
	testedhost_t *newhost;

	hostcount++;
	newhost = (testedhost_t *) calloc(1, sizeof(testedhost_t));
	newhost->hostname = strdup(hostname);
	newhost->dotrace = dotraceroute;
	newhost->sslwarndays = sslwarndays;
	newhost->sslalarmdays = sslalarmdays;

	return newhost;
}

testitem_t *init_testitem(testedhost_t *host, service_t *service, char *testspec, 
                          int dialuptest, int reversetest, int alwaystruetest, int silenttest,
			  int sendasdata)
{
	testitem_t *newtest;

	testcount++;
	newtest = (testitem_t *) malloc(sizeof(testitem_t));
	newtest->host = host;
	newtest->service = service;
	newtest->dialup = dialuptest;
	newtest->reverse = reversetest;
	newtest->alwaystrue = alwaystruetest;
	newtest->silenttest = silenttest;
	newtest->senddata = sendasdata;
	newtest->testspec = (testspec ? strdup(testspec) : NULL);
	newtest->privdata = NULL;
	newtest->open = 0;
	newtest->banner = newstrbuffer(0);
	newtest->certinfo = NULL;
	newtest->certexpires = 0;
	newtest->duration.tv_sec = newtest->duration.tv_usec = -1;
	newtest->downcount = 0;
	newtest->badtest[0] = newtest->badtest[1] = newtest->badtest[2] = 0;
	newtest->next = NULL;

	return newtest;
}


int wanted_host(namelist_t *host, char *netstring)
{
	char *netlocation = bbh_item(host, BBH_NET);

	if (selectedcount == 0)
		return ((strlen(netstring) == 0) || 				   /* No BBLOCATION = do all */
			(netlocation && (strcmp(netlocation, netstring) == 0)) ||  /* BBLOCATION && matching NET: tag */
			(testuntagged && (netlocation == NULL)));		   /* No NET: tag for this host */
	else {
		/* User provided an explicit list of hosts to test */
		int i;

		for (i=0; (i < selectedcount); i++) {
			if (strcmp(selectedhosts[i], host->bbhostname) == 0) return 1;
		}
	}

	return 0;
}


void load_tests(void)
{
	char *p, *routestring = NULL;
	namelist_t *hosts, *hwalk;
	testedhost_t *h;

	hosts = load_hostnames(xgetenv("BBHOSTS"), "netinclude", get_fqdn());
	if (hosts == NULL) {
		errprintf("Cannot load bb-hosts\n");
		return;
	}

	/* Each network test tagged with NET:locationname */
	if (strlen(location) > 0) {
		routestring = (char *) malloc(strlen(location)+strlen("route_:")+1);
		sprintf(routestring, "route_%s:", location);
	}

	for (hwalk = hosts; (hwalk); hwalk = hwalk->next) {
		int anytests = 0;
		int ping_dialuptest = 0, ping_reversetest = 0;
		char *testspec;

		if (!wanted_host(hwalk, location)) continue;

		if (argnmatch(hwalk->bbhostname, "@dialup.")) {
			/* Modembank entry: "dialup displayname startIP count" */

			char *realname;
			testitem_t *newtest;
			modembank_t *newentry;
			int i, ip1, ip2, ip3, ip4, banksize;

			realname = hwalk->bbhostname + strlen("@dialup.");
			banksize = atoi(bbh_item(hwalk, BBH_BANKSIZE));
			sscanf(bbh_item(hwalk, BBH_IP), "%d.%d.%d.%d", &ip1, &ip2, &ip3, &ip4);

			newtest = init_testitem(NULL, modembanktest, NULL, 0, 0, 0, 0, 0);
			newtest->next = modembanktest->items;
			modembanktest->items = newtest;

			newtest->privdata = (void *)malloc(sizeof(modembank_t));
			newentry = (modembank_t *)newtest->privdata;
			newentry->hostname = realname;
			newentry->startip = IPtou32(ip1, ip2, ip3, ip4);
			newentry->banksize = banksize;
			newentry->responses = (int *) malloc(banksize * sizeof(int));
			for (i=0; i<banksize; i++) newentry->responses[i] = 0;

			/* No more to do for modembanks */
			continue;
		}


		h = init_testedhost(hwalk->bbhostname);

		p = bbh_custom_item(hwalk, "badconn:");
		if (p) sscanf(p+strlen("badconn:"), "%d:%d:%d", &h->badconn[0], &h->badconn[1], &h->badconn[2]);

		p = bbh_custom_item(hwalk, "route:");
		if (p) h->routerdeps = p + strlen("route:");
		if (routestring) {
			p = bbh_custom_item(hwalk, routestring);
			if (p) h->routerdeps = p + strlen(routestring);
		}

		if (bbh_item(hwalk, BBH_FLAG_NOCONN)) h->noconn = 1;
		if (bbh_item(hwalk, BBH_FLAG_NOPING)) h->noping = 1;
		if (bbh_item(hwalk, BBH_FLAG_TRACE)) h->dotrace = 1;
		if (bbh_item(hwalk, BBH_FLAG_NOTRACE)) h->dotrace = 0;
		if (bbh_item(hwalk, BBH_FLAG_TESTIP)) h->testip = 1;
		if (bbh_item(hwalk, BBH_FLAG_DIALUP)) h->dialup = 1;
		if (bbh_item(hwalk, BBH_FLAG_NOSSLCERT)) h->nosslcert = 1;
		if (bbh_item(hwalk, BBH_FLAG_LDAPFAILYELLOW)) h->ldapsearchfailyellow = 1;
		if (bbh_item(hwalk, BBH_FLAG_HIDEHTTP)) h->hidehttp = 1;

		p = bbh_item(hwalk, BBH_SSLDAYS);
		if (p) sscanf(p, "%d:%d", &h->sslwarndays, &h->sslalarmdays);

		p = bbh_item(hwalk, BBH_DEPENDS);
		if (p) h->deptests = p;

		p = bbh_item(hwalk, BBH_LDAPLOGIN);
		if (p) {
			h->ldapuser = strdup(p);
			h->ldappasswd = (strchr(h->ldapuser, ':'));
			if (h->ldappasswd) {
				*h->ldappasswd = '\0';
				h->ldappasswd++;
			}
		}

		p = bbh_item(hwalk, BBH_DESCRIPTION);
		if (p) {
			h->hosttype = strdup(p);
			p = strchr(h->hosttype, ':');
			if (p) *p = '\0';
		}

		testspec = bbh_item_walk(hwalk);
		while (testspec) {
			service_t *s = NULL;
			int dialuptest = 0, reversetest = 0, silenttest = 0, sendasdata = 0;
			int alwaystruetest = (bbh_item(hwalk, BBH_FLAG_NOCLEAR) != NULL);

			if (bbh_item_idx(testspec) == -1) {

				/* Test prefixes:
				 * - '?' denotes dialup test, i.e. report failures as clear.
				 * - '|' denotes reverse test, i.e. service should be DOWN.
				 * - '~' denotes test that ignores ping result (normally,
				 *       TCP tests are reported CLEAR if ping check fails;
				 *       with this flag report their true status)
				 */
				if (*testspec == '?') { dialuptest=1;     testspec++; }
				if (*testspec == '!') { reversetest=1;    testspec++; }
				if (*testspec == '~') { alwaystruetest=1; testspec++; }

				if (pingtest && argnmatch(testspec, pingtest->testname)) {
					char *p;

					/*
					 * Ping/conn test. Save any modifier flags for later use.
					 */
					ping_dialuptest = dialuptest;
					ping_reversetest = reversetest;
					p = strchr(testspec, '=');
					if (p) {
						char *ips;

						/* Extra ping tests - save them for later */
						h->extrapings = (extraping_t *)malloc(sizeof(extraping_t));
						h->extrapings->iplist = NULL;
						if (argnmatch(p, "=worst,")) {
							h->extrapings->matchtype = MULTIPING_WORST;
							ips = strdup(p+7);
						}
						else if (argnmatch(p, "=best,")) {
							h->extrapings->matchtype = MULTIPING_BEST;
							ips = strdup(p+6);
						}
						else {
							h->extrapings->matchtype = MULTIPING_BEST;
							ips = strdup(p+1);
						}

						do {
							ipping_t *newping = (ipping_t *)malloc(sizeof(ipping_t));

							newping->ip = ips;
							newping->open = 0;
							newping->banner = newstrbuffer(0);
							newping->next = h->extrapings->iplist;
							h->extrapings->iplist = newping;
							ips = strchr(ips, ',');
							if (ips) { *ips = '\0'; ips++; }
						} while (ips && (*ips));
					}
					s = NULL; /* Dont add the test now - ping is special (enabled by default) */
				}
				else if ((argnmatch(testspec, "ldap://")) || (argnmatch(testspec, "ldaps://"))) {
					/*
					 * LDAP test. This uses ':' a lot, so save it here.
					 */
#ifdef BBGEN_LDAP
					s = ldaptest;
					add_url_to_dns_queue(testspec);
#else
					errprintf("ldap test requested, but bbgen was built with no ldap support\n");
#endif
				}
				else if ((strcmp(testspec, "http") == 0) || (strcmp(testspec, "https") == 0)) {
					errprintf("http/https tests requires a full URL\n");
				}
				else if ( argnmatch(testspec, "http")         ||
					  argnmatch(testspec, "content=http") ||
					  argnmatch(testspec, "cont;http")    ||
					  argnmatch(testspec, "cont=")        ||
					  argnmatch(testspec, "nocont;http")  ||
					  argnmatch(testspec, "nocont=")      ||
					  argnmatch(testspec, "post;http")    ||
					  argnmatch(testspec, "post=")        ||
					  argnmatch(testspec, "nopost;http")  ||
					  argnmatch(testspec, "nopost=")      ||
					  argnmatch(testspec, "type;http")    ||
					  argnmatch(testspec, "type=")        )      {

					/* HTTP test. */
					bburl_t url;

					decode_url(testspec, &url);
					if (url.desturl->parseerror || (url.proxyurl && url.proxyurl->parseerror)) {
						s = NULL;
						errprintf("Invalid URL for http test - ignored: %s\n", testspec);
					}
					else {
						s = httptest;
						add_url_to_dns_queue(testspec);
					}
				}
				else if (argnmatch(testspec, "apache") || argnmatch(testspec, "apache=")) {
					char *userfmt = "cont=apache;%s;.";
					char *deffmt = "cont=apache;http://%s/server-status?auto;.";
					static char *statusurl = NULL;
					char *userurl;

					if (statusurl != NULL) xfree(statusurl);

					userurl = strchr(testspec, '='); 
					if (userurl) {
						bburl_t url;
						userurl++;

						decode_url(userurl, &url);
						if (url.desturl->parseerror || (url.proxyurl && url.proxyurl->parseerror)) {
							s = NULL;
							errprintf("Invalid URL for apache test - ignored: %s\n", testspec);
						}
						else {
							statusurl = (char *)malloc(strlen(userurl) + strlen(userfmt) + 1);
							sprintf(statusurl, userfmt, userurl);
							s = httptest;
						}
					}
					else {
						char *ip = bbh_item(hwalk, BBH_IP);
						statusurl = (char *)malloc(strlen(deffmt) + strlen(ip) + 1);
						sprintf(statusurl, deffmt, ip);
						s = httptest;
					}

					if (s) {
						testspec = statusurl;
						add_url_to_dns_queue(testspec);
						sendasdata = 1;
					}
				}
				else if (argnmatch(testspec, "rpc")) {
					/*
					 * rpc check via rpcinfo
					 */
					s = rpctest;
				}
				else if (argnmatch(testspec, "dns=")) {
					s = dnstest;
				}
				else if (argnmatch(testspec, "dig=")) {
					s = digtest;
				}
				else {
					/* 
					 * Simple TCP connect test. 
					 */
					char *option;
					RbtIterator handle;

					/* Remove any trailing ":s", ":q", ":Q", ":portnumber" */
					option = strchr(testspec, ':'); 
					if (option) { 
						*option = '\0'; 
						option++; 
					}
	
					/* Find the service */
					handle = rbtFind(svctree, testspec);
					s = ((handle == rbtEnd(svctree)) ? NULL : (service_t *)gettreeitem(svctree, handle));
					if (option && s) {
						/*
						 * Check if it is a service with an explicit portnumber.
						 * If it is, then create a new service record named
						 * "SERVICE_PORT" so we can merge tests for this service+port
						 * combination for multiple hosts.
						 *
						 * According to Hobbit docs, this type of services must be in
						 * BBNETSVCS - so it is known already.
						 */
						int specialport = 0;
						char *specialname;
						char *opt2 = strrchr(option, ':');

						if (opt2) {
							if (strcmp(opt2, ":s") == 0) {
								/* option = "portnumber:s" */
								silenttest = 1;
								*opt2 = '\0';
								specialport = atoi(option);
								*opt2 = ':';
							}
						}
						else if (strcmp(option, "s") == 0) {
							/* option = "s" */
							silenttest = 1;
							specialport = 0;
						}
						else {
							/* option = "portnumber" */
							specialport = atoi(option);
						}

						if (specialport) {
							specialname = (char *) malloc(strlen(s->testname)+10);
							sprintf(specialname, "%s_%d", s->testname, specialport);
							s = add_service(specialname, specialport, strlen(s->testname), TOOL_CONTEST);
							xfree(specialname);
						}
					}

					if (s) h->dodns = 1;
					if (option) *(option-1) = ':';
				}

				if (s) {
					testitem_t *newtest;

					anytests = 1;
					newtest = init_testitem(h, s, testspec, dialuptest, reversetest, alwaystruetest, silenttest, sendasdata);
					newtest->next = s->items;
					s->items = newtest;

					if (s == httptest) h->firsthttp = newtest;
					else if (s == ldaptest) h->firstldap = newtest;
				}
			}

			testspec = bbh_item_walk(NULL);
		}

		if (pingtest && !h->noconn) {
			/* Add the ping check */
			testitem_t *newtest;

			anytests = 1;
			newtest = init_testitem(h, pingtest, NULL, ping_dialuptest, ping_reversetest, 1, 0, 0);
			newtest->next = pingtest->items;
			pingtest->items = newtest;
			h->dodns = 1;
		}


		/* 
		 * Setup badXXX values.
		 *
		 * We need to do this last, because the testitem_t records do
		 * not exist until the test has been created.
		 *
		 * So after parsing the badFOO tag, we must find the testitem_t
		 * record created earlier for this test (it may not exist).
		 */
		testspec = bbh_item_walk(hwalk);
		while (testspec) {
			char *testname, *timespec, *badcounts;
			int badclear, badyellow, badred;
			int inscope;
			testitem_t *twalk;
			service_t *swalk;

			if (strncmp(testspec, "bad", 3) != 0) {
				/* Not a bad* tag - skip it */
				testspec = bbh_item_walk(NULL);
				continue;
			}


			badclear = badyellow = badred = 0;
			inscope = 1;

			testname = testspec+strlen("bad");
			badcounts = strchr(testspec, ':');
			if (badcounts) {
				if (sscanf(badcounts, ":%d:%d:%d", &badclear, &badyellow, &badred) != 3) {
					errprintf("Incorrect 'bad' counts: '%s'\n", badcounts);
					badcounts = NULL;
				}
			}
			timespec = strchr(testspec, '-');
			if (timespec) inscope = periodcoversnow(timespec);

			if (strlen(testname) && badcounts && inscope) {
				char *p;
				RbtIterator handle;
				twalk = NULL;

				p = strchr(testname, ':'); if (p) *p = '\0';
				handle = rbtFind(svctree, testname);
				swalk = ((handle == rbtEnd(svctree)) ? NULL : (service_t *)gettreeitem(svctree, handle));
				if (p) *p = ':';
				if (swalk) {
					if (swalk == httptest) twalk = h->firsthttp;
					else if (swalk == ldaptest) twalk = h->firstldap;
					else for (twalk = swalk->items; (twalk && (twalk->host != h)); twalk = twalk->next) ;
				}

				if (twalk) {
					twalk->badtest[0] = badclear;
					twalk->badtest[1] = badyellow;
					twalk->badtest[2] = badred;
				}
				else {
					dprintf("No test for badtest spec host=%s, test=%s\n",
						h->hostname, testname);
				}
			}

			testspec = bbh_item_walk(NULL);
		}


		if (anytests) {
			RbtStatus res;

			/* 
			 * Check for a duplicate host def. Causes all sorts of funny problems.
			 * However, dont drop the second definition - to do this, we will have
			 * to clean up the testitem lists as well, or we get crashes when 
			 * tests belong to a non-existing host.
			 */

			res = rbtInsert(testhosttree, h->hostname, h);
			if (res == RBT_STATUS_DUPLICATE_KEY) {
				errprintf("Host %s appears twice in bb-hosts! This may cause strange results\n", h->hostname);
			}
	
			strcpy(h->ip, bbh_item(hwalk, BBH_IP));
			if (!h->testip && (dnsmethod != IP_ONLY)) add_host_to_dns_queue(h->hostname);
		}
		else {
			/* No network tests for this host, so ignore it */
			dprintf("Did not find any network tests for host %s\n", h->hostname);
			xfree(h);
			notesthostcount++;
		}

	}

	return;
}

char *ip_to_test(testedhost_t *h)
{
	char *dnsresult;
	int nullip = (strcmp(h->ip, "0.0.0.0") == 0);

	if (!nullip && (h->testip || (dnsmethod == IP_ONLY))) {
		/* Already have the IP setup */
	}
	else if (h->dodns) {
		dnsresult = dnsresolve(h->hostname);

		if (dnsresult) {
			strcpy(h->ip, dnsresult);
		}
		else if ((dnsmethod == DNS_THEN_IP) && !nullip) {
			/* Already have the IP setup */
		}
		else {
			/* Cannot resolve hostname */
			h->dnserror = 1;
			errprintf("bbtest-net: Cannot resolve IP for host %s\n", h->hostname);
		}
	}

	return h->ip;
}


void load_ping_status(void)
{
	FILE *statusfd;
	char statusfn[PATH_MAX];
	char l[MAX_LINE_LEN];
	char host[MAX_LINE_LEN];
	int  downcount;
	time_t downstart;
	RbtIterator handle;
	testedhost_t *h;

	sprintf(statusfn, "%s/ping.%s.status", xgetenv("BBTMP"), location);
	statusfd = fopen(statusfn, "r");
	if (statusfd == NULL) return;

	while (fgets(l, sizeof(l), statusfd)) {
		unsigned int uidownstart;
		if (sscanf(l, "%s %d %u", host, &downcount, &uidownstart) == 3) {
			downstart = uidownstart;
			handle = rbtFind(testhosttree, host);
			if (handle != rbtEnd(testhosttree)) {
				h = (testedhost_t *)gettreeitem(testhosttree, handle);
				if (!h->noping && !h->noconn) {
					h->downcount = downcount;
					h->downstart = downstart;
				}
			}
		}
	}

	fclose(statusfd);
}

void save_ping_status(void)
{
	FILE *statusfd;
	char statusfn[PATH_MAX];
	testitem_t *t;
	int didany = 0;

	sprintf(statusfn, "%s/ping.%s.status", xgetenv("BBTMP"), location);
	statusfd = fopen(statusfn, "w");
	if (statusfd == NULL) return;

	for (t=pingtest->items; (t); t = t->next) {
		if (t->host->downcount) {
			fprintf(statusfd, "%s %d %u\n", t->host->hostname, t->host->downcount, (unsigned int)t->host->downstart);
			didany = 1;
			t->host->repeattest = ((time(NULL) - t->host->downstart) < frequenttestlimit);
		}
	}

	fclose(statusfd);
	if (!didany) unlink(statusfn);
}

void load_test_status(service_t *test)
{
	FILE *statusfd;
	char statusfn[PATH_MAX];
	char l[MAX_LINE_LEN];
	char host[MAX_LINE_LEN];
	int  downcount;
	time_t downstart;
	RbtIterator handle;
	testedhost_t *h;
	testitem_t *walk;

	sprintf(statusfn, "%s/%s.%s.status", xgetenv("BBTMP"), test->testname, location);
	statusfd = fopen(statusfn, "r");
	if (statusfd == NULL) return;

	while (fgets(l, sizeof(l), statusfd)) {
		unsigned int uidownstart;
		if (sscanf(l, "%s %d %u", host, &downcount, &uidownstart) == 3) {
			downstart = uidownstart;
			handle = rbtFind(testhosttree, host);
			if (handle != rbtEnd(testhosttree)) {
				h = (testedhost_t *)gettreeitem(testhosttree, handle);
				if (test == httptest) walk = h->firsthttp;
				else if (test == ldaptest) walk = h->firstldap;
				else for (walk = test->items; (walk && (walk->host != h)); walk = walk->next) ;

				if (walk) {
					walk->downcount = downcount;
					walk->downstart = downstart;
				}
			}
		}
	}

	fclose(statusfd);
}

void save_test_status(service_t *test)
{
	FILE *statusfd;
	char statusfn[PATH_MAX];
	testitem_t *t;
	int didany = 0;

	sprintf(statusfn, "%s/%s.%s.status", xgetenv("BBTMP"), test->testname, location);
	statusfd = fopen(statusfn, "w");
	if (statusfd == NULL) return;

	for (t=test->items; (t); t = t->next) {
		if (t->downcount) {
			fprintf(statusfd, "%s %d %u\n", t->host->hostname, t->downcount, (unsigned int)t->downstart);
			didany = 1;
			t->host->repeattest = ((time(NULL) - t->downstart) < frequenttestlimit);
		}
	}

	fclose(statusfd);
	if (!didany) unlink(statusfn);
}


void save_frequenttestlist(int argc, char *argv[])
{
	FILE *fd;
	char fn[PATH_MAX];
	RbtIterator handle;
	testedhost_t *h;
	int didany = 0;
	int i;

	sprintf(fn, "%s/frequenttests.%s", xgetenv("BBTMP"), location);
	fd = fopen(fn, "w");
	if (fd == NULL) return;

	for (i=1; (i<argc); i++) {
		if (!argnmatch(argv[i], "--report")) fprintf(fd, "\"%s\" ", argv[i]);
	}
	for (handle = rbtBegin(testhosttree); (handle != rbtEnd(testhosttree)); handle = rbtNext(testhosttree, handle)) {
		h = (testedhost_t *)gettreeitem(testhosttree, handle);
		if (h->repeattest) {
			fprintf(fd, "%s ", h->hostname);
			didany = 1;
		}
	}

	fclose(fd);
	if (!didany) unlink(fn);
}


void run_nslookup_service(service_t *service)
{
	testitem_t	*t;
	char		*lookup;

	for (t=service->items; (t); t = t->next) {
		if (!t->host->dnserror) {
			if (t->testspec && (lookup = strchr(t->testspec, '='))) {
				lookup++; 
			}
			else {
				lookup = t->host->hostname;
			}

			t->open = (dns_test_server(ip_to_test(t->host), lookup, t->banner) == 0);
		}
	}
}

void run_ntp_service(service_t *service)
{
	testitem_t	*t;
	char		cmd[1024];
	char		*p;
	char		cmdpath[PATH_MAX];

	p = xgetenv("NTPDATE");
	strcpy(cmdpath, (p ? p : "ntpdate"));
	for (t=service->items; (t); t = t->next) {
		if (!t->host->dnserror) {
			sprintf(cmd, "%s -u -q -p 2 %s 2>&1", cmdpath, ip_to_test(t->host));
			t->open = (run_command(cmd, "no server suitable for synchronization", t->banner, 1, extcmdtimeout) == 0);
		}
	}
}


void run_rpcinfo_service(service_t *service)
{
	testitem_t	*t;
	char		cmd[1024];
	char		*p;
	char		cmdpath[PATH_MAX];

	p = xgetenv("RPCINFO");
	strcpy(cmdpath, (p ? p : "rpcinfo"));
	for (t=service->items; (t); t = t->next) {
		if (!t->host->dnserror && (t->host->downcount == 0)) {
			sprintf(cmd, "%s -p %s 2>&1", cmdpath, ip_to_test(t->host));
			t->open = (run_command(cmd, NULL, t->banner, 1, extcmdtimeout) == 0);
		}
	}
}


int start_ping_service(service_t *service)
{
	testitem_t *t;
	char *cmd;
	char **cmdargs;
	int pfd[2];
	int status;

	/*
	 * The idea here is to run ping in a separate process, in parallel
	 * with some other time-consuming task (the TCP network tests).
	 * We cannot use the simple "popen()/pclose()" interface, because
	 *   a) ping doesn't start the tests until EOF is reached on stdin
	 *   b) EOF on stdin happens with pclose(), but it will also wait
	 *      for the process to finish.
	 *
	 * Therefore this slightly more complex solution, which in essence
	 * forks a new process running "hobbitping 2>&1 1>$BBTMP/ping.$$"
	 * The output is then picked up by the finish_ping_service().
	 */

	pingcmd = strdup(getenv_default("FPING", "hobbitping", NULL));
	pingcmd = realloc(pingcmd, strlen(pingcmd)+5);
	strcat(pingcmd, " -Ae");

	sprintf(pinglog, "%s/ping-stdout.%lu", xgetenv("BBTMP"), (unsigned long)getpid());
	sprintf(pingerrlog, "%s/ping-stderr.%lu", xgetenv("BBTMP"), (unsigned long)getpid());

	/* Setup command line and arguments */
	cmdargs = setup_commandargs(pingcmd, &cmd);

	/* Get a pipe FD */
	status = pipe(pfd);
	if (status == -1) {
		errprintf("Could not create pipe for hobbitping\n");
		return -1;
	}

	/* Now fork off the ping child-process */
	status = fork();
	if (status < 0) {
		errprintf("Could not fork() the ping child\n");
		return -1;
	}
	else if (status == 0) {
		/*
		 * child must have
		 *  - stdin fed from the parent
		 *  - stdout going to a file
		 *  - stderr going to another file. This is important, as
		 *    putting it together with stdout will wreak havoc when 
		 *    we start parsing the output later on. We could just 
		 *    dump it to /dev/null, but it might be useful to see
		 *    what went wrong.
		 */
		int outfile, errfile;

		outfile = open(pinglog, O_CREAT|O_WRONLY|O_TRUNC, S_IRUSR|S_IWUSR);
		if (outfile == -1) errprintf("Cannot create file %s : %s\n", pinglog, strerror(errno));
		errfile = open(pingerrlog, O_CREAT|O_WRONLY|O_TRUNC, S_IRUSR|S_IWUSR);
		if (errfile == -1) errprintf("Cannot create file %s : %s\n", pingerrlog, strerror(errno));

		if ((outfile == -1) || (errfile == -1)) {
			/* Ouch - cannot create our output files. Abort. */
			exit(98);
		}

		status = dup2(pfd[0], STDIN_FILENO);
		status = dup2(outfile, STDOUT_FILENO);
		status = dup2(errfile, STDERR_FILENO);
		close(pfd[0]); close(pfd[1]); close(outfile); close(errfile);

		execvp(cmd, cmdargs);

		/* Should never go here ... just kill the child */
		fprintf(stderr, "hobbitping invocation failed: %s\n", strerror(errno));
		exit(99);
	}
	else {
		/* parent */
		char ip[IP_ADDR_STRLEN];

		close(pfd[0]);
		pingcount = 0;

		/* Feed the IP's to test to the child */
		for (t=service->items; (t); t = t->next) {
			if (!t->host->dnserror && !t->host->noping) {
				sprintf(ip, "%s\n", ip_to_test(t->host));
				status = write(pfd[1], ip, strlen(ip));
				pingcount++;
				if (t->host->extrapings) {
					ipping_t *walk;

					for (walk = t->host->extrapings->iplist; (walk); walk = walk->next) {
						sprintf(ip, "%s\n", walk->ip);
						status = write(pfd[1], ip, strlen(ip));
						pingcount++;
					}
				}
			}
		}

		close(pfd[1]);	/* This is when ping starts doing tests */
	}

	return 0;
}


int finish_ping_service(service_t *service)
{
	testitem_t	*t;
	FILE		*logfd;
	char 		*p;
	char		l[MAX_LINE_LEN];
	char		pingip[MAX_LINE_LEN];
	int		ip1, ip2, ip3, ip4;
	int		pingstatus, failed = 0;

	/* 
	 * Wait for the ping child to finish.
	 * If we're lucky, it will be done already since it has run
	 * while we were doing tcp tests.
	 */
	wait(&pingstatus);
	switch (WEXITSTATUS(pingstatus)) {
	  case 0: /* All hosts reachable */
	  case 1: /* Some hosts unreachable */
	  case 2: /* Some IP's not found (should not happen) */
		break;

	  case 3: /* Bad command-line args, or not suid-root */
		errprintf("Execution of '%s' failed - program not suid root?\n", pingcmd);
		break;

	  case 98:
		errprintf("hobbitping child could not create outputfiles in %s\n", xgetenv("$BBTMP"));
		break;

	  case 99:
		errprintf("Could not run the command '%s' (exec failed)\n", pingcmd);
		break;

	  default:
		failed = 1;
		errprintf("Execution of '%s' failed with error-code %d\n", 
			pingcmd, WEXITSTATUS(pingstatus));
	}

	/* Load status of previously failed tests */
	load_ping_status();

	logfd = fopen(pinglog, "r");
	if (logfd == NULL) { errprintf("Cannot open ping output file %s\n", pinglog); return -1; }
	while (fgets(l, sizeof(l), logfd)) {
		p = strchr(l, '\n'); if (p) *p = '\0';
		if (sscanf(l, "%d.%d.%d.%d ", &ip1, &ip2, &ip3, &ip4) == 4) {

			sprintf(pingip, "%d.%d.%d.%d", ip1, ip2, ip3, ip4);

			/*
			 * Need to loop through all testitems - there may be multiple entries for
			 * the same IP-address.
			 */
			for (t=service->items; (t); t = t->next) {
				if (strcmp(t->host->ip, pingip) == 0) {
					if (t->open) dprintf("More than one ping result for %s\n", pingip);
					t->open = (strstr(l, "is alive") != NULL);
					t->banner = dupstrbuffer(l);
				}

				if (t->host->extrapings) {
					ipping_t *walk;
					for (walk = t->host->extrapings->iplist; (walk); walk = walk->next) {
						if (strcmp(walk->ip, pingip) == 0) {
							if (t->open) dprintf("More than one ping result for %s\n", pingip);
							walk->open = (strstr(l, "is alive") != NULL);
							walk->banner = dupstrbuffer(l);
						}
					}
				}
			}
		}
	}
	fclose(logfd);
	if (!debug) {
		unlink(pinglog);
		if (failed) {
			FILE *errfd;
			char buf[1024];
			
			errfd = fopen(pingerrlog, "r");
			if (errfd && fgets(buf, sizeof(buf), errfd)) {
				errprintf("%s", buf);
			}
			if (errfd) fclose(errfd);
		}
		unlink(pingerrlog);
	}

	/* 
	 * Handle the router dependency stuff. I.e. for all hosts
	 * where the ping test failed, go through the list of router
	 * dependencies and if one of the dependent hosts also has 
	 * a failed ping test, point the dependency there.
	 */
	for (t=service->items; (t); t = t->next) {
		if (!t->open && t->host->routerdeps) {
			testitem_t *router;

			strcpy(l, t->host->routerdeps);
			p = strtok(l, ",");
			while (p && (t->host->deprouterdown == NULL)) {
				for (router=service->items; 
					(router && (strcmp(p, router->host->hostname) != 0)); 
					router = router->next) ;

				if (router && !router->open) t->host->deprouterdown = router->host;

				p = strtok(NULL, ",");
			}
		}
	}

	return 0;
}

void run_modembank_service(service_t *service)
{
	testitem_t	*t;
	char		cmd[1024];
	char		startip[IP_ADDR_STRLEN], endip[IP_ADDR_STRLEN];
	char		*p;
	char		cmdpath[PATH_MAX];
	FILE		*cmdpipe;
	char		l[MAX_LINE_LEN];
	int		ip1, ip2, ip3, ip4;

	for (t=service->items; (t); t = t->next) {
		modembank_t *req = (modembank_t *)t->privdata;

		p = xgetenv("FPING");
		strcpy(cmdpath, (p ? p : "hobbitping"));
		strcpy(startip, u32toIP(req->startip));
		strcpy(endip, u32toIP(req->startip + req->banksize - 1));
		sprintf(cmd, "%s -g -Ae %s %s 2>/dev/null", cmdpath, startip, endip);

		dprintf("Running command: '%s'\n", cmd);
		cmdpipe = popen(cmd, "r");
		if (cmdpipe == NULL) {
			errprintf("Could not run the hobbitping command %s\n", cmd);
			return;
		}

		while (fgets(l, sizeof(l), cmdpipe)) {
			dprintf("modembank response: %s", l);

			if (sscanf(l, "%d.%d.%d.%d ", &ip1, &ip2, &ip3, &ip4) == 4) {
				unsigned int idx = IPtou32(ip1, ip2, ip3, ip4) - req->startip;

				if (idx >= req->banksize) {
					errprintf("Unexpected response for IP not in bank - %d.%d.%d.%d", 
						  ip1, ip2, ip3, ip4);
				}
				else {
					req->responses[idx] = (strstr(l, "is alive") != NULL);
				}
			}
		}
		pclose(cmdpipe);

		if (debug) {
			int i;

			dprintf("Results for modembank start=%s, length %d\n", u32toIP(req->startip), req->banksize);
			for (i=0; (i<req->banksize); i++)
				dprintf("\t%s is %d\n", u32toIP(req->startip+i), req->responses[i]);
		}
	}
}


int decide_color(service_t *service, char *svcname, testitem_t *test, int failgoesclear, char *cause)
{
	int color = COL_GREEN;
	int countasdown = 0;
	char *deptest = NULL;

	*cause = '\0';
	if (service == pingtest) {
		/*
		 * "noconn" is handled elsewhere.
		 * "noping" always sends back a status "clear".
		 * If DNS error, return red and count as down.
		 */
		if (test->host->noping) { 
			/* Ping test disabled - go "clear". End of story. */
			strcpy(cause, "Ping test disabled (noping)");
			return COL_CLEAR; 
		}
		else if (test->host->dnserror) { 
			strcpy(cause, "DNS lookup failure");
			color = COL_RED; countasdown = 1; 
		}
		else {
			if (test->host->extrapings == NULL) {
				/* Red if (open=0, reverse=0) or (open=1, reverse=1) */
				if ((test->open + test->reverse) != 1) { 
					sprintf(cause, "Host %s respond to ping", (test->open ? "does" : "does not"));
					color = COL_RED; countasdown = 1; 
				}
			}
			else {
				/* Host with many pings */
				int totalcount = 1;
				int okcount = test->open;
				ipping_t *walk;

				for (walk = test->host->extrapings->iplist; (walk); walk = walk->next) {
					if (walk->open) okcount++;
					totalcount++;
				}

				switch (test->host->extrapings->matchtype) {
				  case MULTIPING_BEST:
					  if (okcount == 0) {
						  color = COL_RED;
						  countasdown = 1;
						  sprintf(cause, "Host does not respond to ping on any of %d IP's", 
							  totalcount);
					  }
					  break;
				  case MULTIPING_WORST:
					  if (okcount < totalcount) {
						  color = COL_RED;
						  countasdown = 1;
						  sprintf(cause, "Host responds to ping on %d of %d IP's",
							  okcount, totalcount);
					  }
					  break;
				}
			}
		}

		/* Handle the "route" tag dependencies. */
		if ((color == COL_RED) && test->host->deprouterdown) { 
			char *routertext;

			routertext = test->host->deprouterdown->hosttype;
			if (routertext == NULL) routertext = xgetenv("BBROUTERTEXT");
			if (routertext == NULL) routertext = "router";

			strcat(cause, "\nIntermediate ");
			strcat(cause, routertext);
			strcat(cause, " down ");
			color = COL_YELLOW; 
		}

		/* Handle "badconn" */
		if ((color == COL_RED) && (test->host->downcount < test->host->badconn[2])) {
			if      (test->host->downcount >= test->host->badconn[1]) color = COL_YELLOW;
			else if (test->host->downcount >= test->host->badconn[0]) color = COL_CLEAR;
			else                                                      color = COL_GREEN;
		}

		/* Run traceroute , but not on dialup or reverse-test hosts */
		if ((color == COL_RED) && test->host->dotrace && !test->host->dialup && !test->reverse && !test->dialup) {
			char cmd[PATH_MAX];

			if (xgetenv("TRACEROUTE")) {
				sprintf(cmd, "%s %s 2>&1", xgetenv("TRACEROUTE"), test->host->ip);
			}
			else {
				sprintf(cmd, "traceroute -n -q 2 -w 2 -m 15 %s 2>&1", test->host->ip);
			}
			test->host->traceroute = newstrbuffer(0);
			run_command(cmd, NULL, test->host->traceroute, 0, extcmdtimeout);
		}
	}
	else {
		/* TCP test */
		if (test->host->dnserror) { 
			strcpy(cause, "DNS lookup failure");
			color = COL_RED; countasdown = 1; 
		}
		else {
			if (test->reverse) {
				/*
				 * Reverse tests go RED when open.
				 * If not open, they may go CLEAR if the ping test failed
				 */

				if (test->open) { 
					strcpy(cause, "Service responds when it should not");
					color = COL_RED; countasdown = 1; 
				}
				else if (failgoesclear && (test->host->downcount != 0) && !test->alwaystrue) {
					strcpy(cause, "Host appears to be down");
					color = COL_CLEAR; countasdown = 0;
				}
			}
			else {
				if (!test->open) {
					if (failgoesclear && (test->host->downcount != 0) && !test->alwaystrue) {
						strcpy(cause, "Host appears to be down");
						color = COL_CLEAR; countasdown = 0;
					}
					else {
						tcptest_t *tcptest = (tcptest_t *)test->privdata;

						strcpy(cause, "Service unavailable");
						if (tcptest) {
							switch (tcptest->errcode) {
							  case CONTEST_ETIMEOUT: 
								strcat(cause, " (connect timeout)"); 
								break;
							  case CONTEST_ENOCONN : 
								strcat(cause, " (");
								strcat(cause, strerror(tcptest->connres));
								strcat(cause, ")");
								break;
							  case CONTEST_EDNS    : 
								strcat(cause, " (DNS error)"); 
								break;
							  case CONTEST_EIO     : 
								strcat(cause, " (I/O error)"); 
								break;
							  case CONTEST_ESSL    : 
								strcat(cause, " (SSL error)"); 
								break;
							}
						}
						color = COL_RED; countasdown = 1;
					}
				}
				else {
					/* Check if we got the expected data */
					if (checktcpresponse && (service->toolid == TOOL_CONTEST) && !tcp_got_expected((tcptest_t *)test->privdata)) {
						strcpy(cause, "Unexpected service response");
						color = respcheck_color; countasdown = 1;
					}
				}
			}
		}

		/* Handle test dependencies */
		if ( failgoesclear && (color == COL_RED) && !test->alwaystrue && (deptest = deptest_failed(test->host, test->service->testname)) ) {
			strcpy(cause, deptest);
			color = COL_CLEAR;
		}

		/* Handle the "badtest" stuff for other tests */
		if ((color == COL_RED) && (test->downcount < test->badtest[2])) {
			if      (test->downcount >= test->badtest[1]) color = COL_YELLOW;
			else if (test->downcount >= test->badtest[0]) color = COL_CLEAR;
			else                                          color = COL_GREEN;
		}
	}


	/* Dialup hosts and dialup tests report red as clear */
	if ( ((color == COL_RED) || (color == COL_YELLOW)) && (test->host->dialup || test->dialup) && !test->reverse) { 
		strcat(cause, "\nDialup host or service");
		color = COL_CLEAR; countasdown = 0; 
	}

	/* If a NOPAGENET service, downgrade RED to YELLOW */
	if (color == COL_RED) {
		char *nopagename;

		/* Check if this service is a NOPAGENET service. */
		nopagename = (char *) malloc(strlen(svcname)+3);
		sprintf(nopagename, ",%s,", svcname);
		if (strstr(nonetpage, svcname) != NULL) color = COL_YELLOW;
		xfree(nopagename);
	}

	if (service == pingtest) {
		if (countasdown) {
			test->host->downcount++; 
			if (test->host->downcount == 1) test->host->downstart = time(NULL);
		}
		else test->host->downcount = 0;
	}
	else {
		if (countasdown) {
			test->downcount++; 
			if (test->downcount == 1) test->downstart = time(NULL);
		}
		else test->downcount = 0;
	}
	return color;
}


void send_results(service_t *service, int failgoesclear)
{
	testitem_t	*t;
	int		color;
	char		msgline[4096];
	char		msgtext[4096];
	char		causetext[1024];
	char		*svcname;

	svcname = strdup(service->testname);
	if (service->namelen) svcname[service->namelen] = '\0';

	dprintf("Sending results for service %s\n", svcname);

	for (t=service->items; (t); t = t->next) {
		char flags[10];
		int i;

		i = 0;
		flags[i++] = (t->open ? 'O' : 'o');
		flags[i++] = (t->reverse ? 'R' : 'r');
		flags[i++] = ((t->dialup || t->host->dialup) ? 'D' : 'd');
		flags[i++] = (t->alwaystrue ? 'A' : 'a');
		flags[i++] = (t->silenttest ? 'S' : 's');
		flags[i++] = (t->host->testip ? 'T' : 't');
		flags[i++] = (t->host->dodns ? 'L' : 'l');
		flags[i++] = (t->host->dnserror ? 'E' : 'e');
		flags[i++] = '\0';

		color = decide_color(service, svcname, t, failgoesclear, causetext);

		init_status(color);
		if (dosendflags) 
			sprintf(msgline, "status %s.%s %s <!-- [flags:%s] --> %s %s %s ", 
				commafy(t->host->hostname), svcname, colorname(color), 
				flags, timestamp, 
				svcname, ( ((color == COL_RED) || (color == COL_YELLOW)) ? "NOT ok" : "ok"));
		else
			sprintf(msgline, "status %s.%s %s %s %s %s ", 
				commafy(t->host->hostname), svcname, colorname(color), 
				timestamp, 
				svcname, ( ((color == COL_RED) || (color == COL_YELLOW)) ? "NOT ok" : "ok"));

		if (t->host->dnserror) {
			strcat(msgline, ": DNS lookup failed");
			sprintf(msgtext, "\nUnable to resolve hostname %s\n\n", t->host->hostname);
		}
		else {
			sprintf(msgtext, "\nService %s on %s is ", svcname, t->host->hostname);
			switch (color) {
			  case COL_GREEN: 
				  strcat(msgtext, "OK ");
				  strcat(msgtext, (t->reverse ? "(down)" : "(up)"));
				  strcat(msgtext, "\n");
				  break;

			  case COL_RED:
			  case COL_YELLOW:
				  if ((service == pingtest) && t->host->deprouterdown) {
					char *routertext;

					routertext = t->host->deprouterdown->hosttype;
					if (routertext == NULL) routertext = xgetenv("BBROUTERTEXT");
					if (routertext == NULL) routertext = "router";

					strcat(msgline, ": Intermediate ");
					strcat(msgline, routertext);
					strcat(msgline, " down");

					sprintf(msgtext+strlen(msgtext), 
						"%s.\nThe %s %s (IP:%s) is not reachable, causing this host to be unreachable.\n",
						failtext, routertext, 
						((testedhost_t *)t->host->deprouterdown)->hostname,
						((testedhost_t *)t->host->deprouterdown)->ip);
				  }
				  else {
					sprintf(msgtext+strlen(msgtext), "%s : %s\n", failtext, causetext);
				  }
				  break;

			  case COL_CLEAR:
				  strcat(msgtext, "OK\n");
				  if (service == pingtest) {
					  if (t->host->deprouterdown) {
						char *routertext;

						routertext = t->host->deprouterdown->hosttype;
						if (routertext == NULL) routertext = xgetenv("BBROUTERTEXT");
						if (routertext == NULL) routertext = "router";

						strcat(msgline, ": Intermediate ");
						strcat(msgline, routertext);
						strcat(msgline, " down");

						strcat(msgtext, "\nThe ");
						strcat(msgtext, routertext); strcat(msgtext, " ");
						strcat(msgtext, ((testedhost_t *)t->host->deprouterdown)->hostname);
						strcat(msgtext, " (IP:");
						strcat(msgtext, ((testedhost_t *)t->host->deprouterdown)->ip);
						strcat(msgtext, ") is not reachable, causing this host to be unreachable.\n");
					  }
					  else if (t->host->noping) {
						  strcat(msgline, ": Disabled");
						  strcat(msgtext, "Ping check disabled (noping)\n");
					  }
					  else if (t->host->dialup) {
						  strcat(msgline, ": Disabled (dialup host)");
						  strcat(msgtext, "Dialup host\n");
					  }
					  /* "clear" due to badconn: no extra text */
				  }
				  else {
					  /* Non-ping test clear: Dialup test or failed ping */
					  strcat(msgline, ": Ping failed, or dialup host/service");
					  strcat(msgtext, "Dialup host/service, or test depends on another failed test\n");
					  strcat(msgtext, causetext);
				  }
				  break;
			}
			strcat(msgtext, "\n");
		}
		strcat(msgline, "\n");
		addtostatus(msgline);
		addtostatus(msgtext);

		if ((service == pingtest) && t->host->downcount) {
			sprintf(msgtext, "\nSystem unreachable for %d poll periods (%u seconds)\n",
				t->host->downcount, (unsigned int)(time(NULL) - t->host->downstart));
			addtostatus(msgtext);
		}

		if (STRBUFLEN(t->banner)) {
			if (service == pingtest) {
				sprintf(msgtext, "\n&%s %s\n", colorname(t->open ? COL_GREEN : COL_RED), STRBUF(t->banner));
				addtostatus(msgtext);
				if (t->host->extrapings) {
					ipping_t *walk;
					for (walk = t->host->extrapings->iplist; (walk); walk = walk->next) {
						if (STRBUFLEN(walk->banner)) {
							sprintf(msgtext, "&%s %s\n", 
								colorname(walk->open ? COL_GREEN : COL_RED), STRBUF(walk->banner));
							addtostatus(msgtext);
						}
					}
				}
			}
			else {
				addtostatus("\n"); addtostrstatus(t->banner); addtostatus("\n");
			}
		}

		if ((service == pingtest) && t->host->traceroute) {
			addtostatus("Traceroute results:\n");
			addtostrstatus(t->host->traceroute);
			addtostatus("\n");
		}

		if (t->duration.tv_sec != -1) {
			sprintf(msgtext, "\nSeconds: %u.%02u\n", 
				(unsigned int)t->duration.tv_sec, (unsigned int)t->duration.tv_usec / 10000);
			addtostatus(msgtext);
		}
		addtostatus("\n\n");
		finish_status();
	}
}


void send_modembank_results(service_t *service)
{
	testitem_t	*t;
	char		msgline[1024];
	int		i, color, inuse;
	char		startip[IP_ADDR_STRLEN], endip[IP_ADDR_STRLEN];

	for (t=service->items; (t); t = t->next) {
		modembank_t *req = (modembank_t *)t->privdata;

		inuse = 0;
		strcpy(startip, u32toIP(req->startip));
		strcpy(endip, u32toIP(req->startip + req->banksize - 1));

		init_status(COL_GREEN);		/* Modembanks are always green */
		sprintf(msgline, "status dialup.%s %s %s FROM %s TO %s DATA ", 
			req->hostname, colorname(COL_GREEN), timestamp, startip, endip);
		addtostatus(msgline);
		for (i=0; i<req->banksize; i++) {
			if (req->responses[i]) {
				color = COL_GREEN;
				inuse++;
			}
			else {
				color = COL_CLEAR;
			}

			sprintf(msgline, "%s ", colorname(color));
			addtostatus(msgline);
		}

		sprintf(msgline, "\n\nUsage: %d of %d (%d%%)\n", inuse, req->banksize, ((inuse * 100) / req->banksize));
		addtostatus(msgline);
		finish_status();
	}
}


void send_rpcinfo_results(service_t *service, int failgoesclear)
{
	testitem_t	*t;
	int		color;
	char		msgline[1024];
	char		*msgbuf;
	char		causetext[1024];

	msgbuf = (char *)malloc(4096);

	for (t=service->items; (t); t = t->next) {
		char *wantedrpcsvcs = NULL;
		char *p;

		/* First see if the rpcinfo command succeeded */
		*msgbuf = '\0';

		color = decide_color(service, service->testname, t, failgoesclear, causetext);
		p = strchr(t->testspec, '=');
		if (p) wantedrpcsvcs = strdup(p+1);

		if ((color == COL_GREEN) && STRBUFLEN(t->banner) && wantedrpcsvcs) {
			char *rpcsvc, *aline;

			rpcsvc = strtok(wantedrpcsvcs, ",");
			while (rpcsvc) {
				struct rpcent *rpcinfo;
				int  svcfound = 0;
				int  aprogram;
				int  aversion;
				char aprotocol[10];
				int  aport;

				rpcinfo = getrpcbyname(rpcsvc);
				aline = STRBUF(t->banner); 
				while ((!svcfound) && rpcinfo && aline && (*aline != '\0')) {
					p = strchr(aline, '\n');
					if (p) *p = '\0';

					if (sscanf(aline, "%d %d %s %d", &aprogram, &aversion, aprotocol, &aport) == 4) {
						svcfound = (aprogram == rpcinfo->r_number);
					}

					aline = p;
					if (p) {
						*p = '\n';
						aline++;
					}
				}

				if (svcfound) {
					sprintf(msgline, "&%s Service %s (ID: %d) found on port %d\n", 
						colorname(COL_GREEN), rpcsvc, rpcinfo->r_number, aport);
				}
				else if (rpcinfo) {
					color = COL_RED;
					sprintf(msgline, "&%s Service %s (ID: %d) NOT found\n", 
						colorname(COL_RED), rpcsvc, rpcinfo->r_number);
				}
				else {
					color = COL_RED;
					sprintf(msgline, "&%s Unknown RPC service %s\n",
						colorname(COL_RED), rpcsvc);
				}
				strcat(msgbuf, msgline);

				rpcsvc = strtok(NULL, ",");
			}
		}

		if (wantedrpcsvcs) xfree(wantedrpcsvcs);

		init_status(color);
		sprintf(msgline, "status %s.%s %s %s %s %s, %s\n\n", 
			commafy(t->host->hostname), service->testname, colorname(color), timestamp, 
			service->testname, 
			( ((color == COL_RED) || (color == COL_YELLOW)) ? "NOT ok" : "ok"),
			causetext);
		addtostatus(msgline);

		/* The summary of wanted RPC services */
		addtostatus(msgbuf);

		/* rpcinfo output */
		if (t->open) {
			if (STRBUFLEN(t->banner)) {
				addtostatus("\n\n");
				addtostrstatus(t->banner);
			}
			else {
				sprintf(msgline, "\n\nNo output from rpcinfo -p %s\n", t->host->ip);
				addtostatus(msgline);
			}
		}
		else {
			addtostatus("\n\nCould not connect to the portmapper service\n");
			if (STRBUFLEN(t->banner)) addtostrstatus(t->banner);
		}
		finish_status();
	}

	xfree(msgbuf);
}


void send_sslcert_status(testedhost_t *host)
{
	int color = -1;
	RbtIterator handle;
	service_t *s;
	testitem_t *t;
	char msgline[1024];
	char *sslmsg;
	int sslmsgsize;
	time_t now = time(NULL);
	char *certowner;

	sslmsgsize = 4096;
	sslmsg = (char *)malloc(sslmsgsize);
	*sslmsg = '\0';

	for (handle = rbtBegin(svctree); handle != rbtEnd(svctree); handle = rbtNext(svctree, handle)) {
		s = (service_t *)gettreeitem(svctree, handle);
		certowner = s->testname;

		for (t=s->items; (t); t=t->next) {
			if ((t->host == host) && t->certinfo && (t->certexpires > 0)) {
				int sslcolor = COL_GREEN;

				if (s == httptest) certowner = ((http_data_t *)t->privdata)->url;
				else if (s == ldaptest) certowner = t->testspec;

				if (t->certexpires < (now+host->sslwarndays*86400)) sslcolor = COL_YELLOW;
				if (t->certexpires < (now+host->sslalarmdays*86400)) sslcolor = COL_RED;
				if (sslcolor > color) color = sslcolor;

				if (t->certexpires > now) {
					sprintf(msgline, "\n&%s SSL certificate for %s expires in %u days\n\n", 
						colorname(sslcolor), certowner,
						(unsigned int)((t->certexpires - now) / 86400));
				}
				else {
					sprintf(msgline, "\n&%s SSL certificate for %s expired %u days ago\n\n", 
						colorname(sslcolor), certowner,
						(unsigned int)((now - t->certexpires) / 86400));
				}

				if ((strlen(msgline)+strlen(sslmsg) + strlen(t->certinfo)) > sslmsgsize) {
					sslmsgsize += (4096 + strlen(t->certinfo) + strlen(msgline));
					sslmsg = (char *)realloc(sslmsg, sslmsgsize);
				}
				strcat(sslmsg, msgline);
				strcat(sslmsg, t->certinfo);
			}
		}
	}

	if (color != -1) {
		/* Send off the sslcert status report */
		init_status(color);
		sprintf(msgline, "status %s.%s %s %s\n", 
			commafy(host->hostname), ssltestname, colorname(color), timestamp);
		addtostatus(msgline);
		addtostatus(sslmsg);
		addtostatus("\n\n");
		finish_status();
	}

	xfree(sslmsg);
}

int main(int argc, char *argv[])
{
	RbtIterator handle;
	service_t *s;
	testedhost_t *h;
	testitem_t *t;
	int argi;
	int concurrency = 0;
	char *pingcolumn = "";
	char *egocolumn = NULL;
	int failgoesclear = 0;		/* IPTEST_2_CLEAR_ON_FAILED_CONN */
	int dumpdata = 0;
	int runtimewarn;		/* 300 = default BBSLEEP setting */
	int servicedumponly = 0;
	int pingrunning = 0;

	if (init_ldap_library() != 0) {
		errprintf("Failed to initialize ldap library\n");
		return 1;
	}

	if (xgetenv("CONNTEST") && (strcmp(xgetenv("CONNTEST"), "FALSE") == 0)) pingcolumn = NULL;
	runtimewarn = (xgetenv("BBSLEEP") ? atol(xgetenv("BBSLEEP")) : 300);

	for (argi=1; (argi < argc); argi++) {
		if      (argnmatch(argv[argi], "--timeout=")) {
			char *p = strchr(argv[argi], '=');
			p++; timeout = atoi(p);
		}
		else if (argnmatch(argv[argi], "--conntimeout=")) {
			int newtimeout;
			char *p = strchr(argv[argi], '=');
			p++; newtimeout = atoi(p);
			if (newtimeout > timeout) timeout = newtimeout;
			errprintf("Deprecated option '--conntimeout' should not be used\n");
		}
		else if (argnmatch(argv[argi], "--cmdtimeout=")) {
			char *p = strchr(argv[argi], '=');
			p++; extcmdtimeout = atoi(p);
		}
		else if (argnmatch(argv[argi], "--concurrency=")) {
			char *p = strchr(argv[argi], '=');
			p++; concurrency = atoi(p);
		}
		else if (argnmatch(argv[argi], "--dns-timeout=") || argnmatch(argv[argi], "--dns-max-all=")) {
			char *p = strchr(argv[argi], '=');
			p++; dnstimeout = atoi(p);
		}
		else if (argnmatch(argv[argi], "--dns=")) {
			char *p = strchr(argv[argi], '=');
			p++;
			if (strcmp(p, "only") == 0)      dnsmethod = DNS_ONLY;
			else if (strcmp(p, "ip") == 0)   dnsmethod = IP_ONLY;
			else                             dnsmethod = DNS_THEN_IP;
		}
		else if (strcmp(argv[argi], "--no-ares") == 0) {
			use_ares_lookup = 0;
		}
		else if (argnmatch(argv[argi], "--maxdnsqueue=")) {
			char *p = strchr(argv[argi], '=');
			max_dns_per_run = atoi(p+1);
		}
		else if (argnmatch(argv[argi], "--report=") || (strcmp(argv[argi], "--report") == 0)) {
			char *p = strchr(argv[argi], '=');
			if (p) {
				egocolumn = strdup(p+1);
			}
			else egocolumn = "bbtest";
			timing = 1;
		}
		else if (strcmp(argv[argi], "--test-untagged") == 0) {
			testuntagged = 1;
		}
		else if (argnmatch(argv[argi], "--frequenttestlimit=")) {
			char *p = strchr(argv[argi], '=');
			p++; frequenttestlimit = atoi(p);
		}
		else if (strcmp(argv[argi], "--timelimit=") == 0) {
			char *p = strchr(argv[argi], '=');
			p++; runtimewarn = atol(p);
		}
		else if (strcmp(argv[argi], "--huge=") == 0) {
			char *p = strchr(argv[argi], '=');
			p++; warnbytesread = atoi(p);
		}

		/* Options for TCP tests */
		else if (strcmp(argv[argi], "--checkresponse") == 0) {
			checktcpresponse = 1;
		}
		else if (argnmatch(argv[argi], "--checkresponse=")) {
			char *p = strchr(argv[argi], '=');
			checktcpresponse = 1;
			respcheck_color = parse_color(p+1);
			if (respcheck_color == -1) {
				errprintf("Invalid colorname in '%s' - using yellow\n", argv[argi]);
				respcheck_color = COL_YELLOW;
			}
		}
		else if (strcmp(argv[argi], "--no-flags") == 0) {
			dosendflags = 0;
		}

		/* Options for PING tests */
		else if (argnmatch(argv[argi], "--ping")) {
			char *p = strchr(argv[argi], '=');
			if (p) {
				p++; pingcolumn = p;
			}
			else pingcolumn = "";
		}
		else if (strcmp(argv[argi], "--noping") == 0) {
			pingcolumn = NULL;
		}
		else if (strcmp(argv[argi], "--trace") == 0) {
			dotraceroute = 1;
		}
		else if (strcmp(argv[argi], "--notrace") == 0) {
			dotraceroute = 0;
		}

		/* Options for HTTP tests */
		else if (argnmatch(argv[argi], "--content=")) {
			char *p = strchr(argv[argi], '=');
			contenttestname = strdup(p+1);
		}

		/* Options for SSL certificates */
		else if (argnmatch(argv[argi], "--ssl=")) {
			char *p = strchr(argv[argi], '=');
			ssltestname = strdup(p+1);
		}
		else if (strcmp(argv[argi], "--no-ssl") == 0) {
			ssltestname = NULL;
		}
		else if (argnmatch(argv[argi], "--sslwarn=")) {
			char *p = strchr(argv[argi], '=');
			p++; sslwarndays = atoi(p);
		}
		else if (argnmatch(argv[argi], "--sslalarm=")) {
			char *p = strchr(argv[argi], '=');
			p++; sslalarmdays = atoi(p);
		}

		/* Debugging options */
		else if (strcmp(argv[argi], "--debug") == 0) {
			debug = 1;
		}
		else if (argnmatch(argv[argi], "--dump")) {
			char *p = strchr(argv[argi], '=');

			if (p) {
				if (strcmp(p, "=before") == 0) dumpdata = 1;
				else if (strcmp(p, "=after") == 0) dumpdata = 2;
				else dumpdata = 3;
			}
			else dumpdata = 2;

			debug = 1;
		}
		else if (strcmp(argv[argi], "--no-update") == 0) {
			dontsendmessages = 1;
		}
		else if (strcmp(argv[argi], "--timing") == 0) {
			timing = 1;
		}

		/* Informational options */
		else if (strcmp(argv[argi], "--services") == 0) {
			servicedumponly = 1;
		}
		else if (strcmp(argv[argi], "--version") == 0) {
			printf("bbtest-net version %s\n", VERSION);
			if (ssl_library_version) printf("SSL library : %s\n", ssl_library_version);
			if (ldap_library_version) printf("LDAP library: %s\n", ldap_library_version);
			printf("\n");
			return 0;
		}
		else if ((strcmp(argv[argi], "--help") == 0) || (strcmp(argv[argi], "-?") == 0)) {
			printf("bbtest-net version %s\n\n", VERSION);
			printf("Usage: %s [options] [host1 host2 host3 ...]\n", argv[0]);
			printf("General options:\n");
			printf("    --timeout=N                 : Timeout (in seconds) for service tests\n");
			printf("    --concurrency=N             : Number of tests run in parallel\n");
			printf("    --dns-timeout=N             : DNS lookups timeout and fail after N seconds [30]\n");
			printf("    --dns=[only|ip|standard]    : How IP's are decided\n");
			printf("    --no-ares                   : Use the system resolver library for hostname lookups\n");
			printf("    --report[=COLUMNNAME]       : Send a status report about the running of bbtest-net\n");
			printf("    --test-untagged             : Include hosts without a NET: tag in the test\n");
			printf("    --frequenttestlimit=N       : Seconds after detecting failures in which we poll frequently\n");
			printf("    --timelimit=N               : Warns if the complete test run takes longer than N seconds [BBSLEEP]\n");
			printf("\nOptions for simple TCP service tests:\n");
			printf("    --checkresponse             : Check response from known services\n");
			printf("    --no-flags                  : Dont send extra bbgen test flags\n");
			printf("\nOptions for PING (connectivity) tests:\n");
			printf("    --ping[=COLUMNNAME]         : Enable ping checking, default columname is \"conn\"\n");
			printf("    --noping                    : Disable ping checking\n");
			printf("    --trace                     : Run traceroute on all hosts where ping fails\n");
			printf("    --notrace                   : Disable traceroute when ping fails (default)\n");
			printf("\nOptions for HTTP/HTTPS (Web) tests:\n");
			printf("    --content=COLUMNNAME        : Define default columnname for CONTENT checks (content)\n");
			printf("\nOptions for SSL certificate tests:\n");
			printf("    --ssl=COLUMNNAME            : Define columnname for SSL certificate checks (sslcert)\n");
			printf("    --no-ssl                    : Disable SSL certificate check\n");
			printf("    --sslwarn=N                 : Go yellow if certificate expires in less than N days (default:30)\n");
			printf("    --sslalarm=N                : Go red if certificate expires in less than N days (default:10)\n");
			printf("\nDebugging options:\n");
			printf("    --no-update                 : Send status messages to stdout instead of to bbd\n");
			printf("    --timing                    : Trace the amount of time spent on each series of tests\n");
			printf("    --debug                     : Output debugging information\n");
			printf("    --dump[=before|=after|=all] : Dump internal memory structures before/after tests run\n");
			printf("    --maxdnsqueue=N             : Only queue N DNS lookups at a time\n");
			printf("\nInformational options:\n");
			printf("    --services                  : Dump list of known services and exit\n");
			printf("    --version                   : Show program version and exit\n");
			printf("    --help                      : Show help text and exit\n");

			return 0;
		}
		else if (strncmp(argv[argi], "-", 1) == 0) {
			errprintf("Unknown option %s - try --help\n", argv[argi]);
		}
		else {
			/* Must be a hostname */
			if (selectedcount == 0) selectedhosts = (char **) malloc(argc*sizeof(char *));
			selectedhosts[selectedcount++] = strdup(argv[argi]);
		}
	}

	svctree = rbtNew(name_compare);
	testhosttree = rbtNew(name_compare);
	init_timestamp();
	envcheck(reqenv);
	fqdn = get_fqdn();

	/* Setup SEGV handler */
	setup_signalhandler(egocolumn ? egocolumn : "bbtest");

	if (xgetenv("BBLOCATION")) location = strdup(xgetenv("BBLOCATION"));
	if (pingcolumn && (strlen(pingcolumn) == 0)) pingcolumn = xgetenv("PINGCOLUMN");
	if (pingcolumn && xgetenv("IPTEST_2_CLEAR_ON_FAILED_CONN")) {
		failgoesclear = (strcmp(xgetenv("IPTEST_2_CLEAR_ON_FAILED_CONN"), "TRUE") == 0);
	}
	if (xgetenv("NETFAILTEXT")) failtext = strdup(xgetenv("NETFAILTEXT"));

	if (debug) {
		int i;
		printf("Command: bbtest-net");
		for (i=1; (i<argc); i++) printf(" '%s'", argv[i]);
		printf("\n");
		printf("Environment BBLOCATION='%s'\n", textornull(xgetenv("BBLOCATION")));
		printf("Environment CONNTEST='%s'\n", textornull(xgetenv("CONNTEST")));
		printf("Environment IPTEST_2_CLEAR_ON_FAILED_CONN='%s'\n", textornull(xgetenv("IPTEST_2_CLEAR_ON_FAILED_CONN")));
		printf("\n");
	}

	add_timestamp("bbtest-net startup");

	load_services();
	if (servicedumponly) {
		dump_tcp_services();
		return 0;
	}

	dnstest = add_service("dns", getportnumber("domain"), 0, TOOL_NSLOOKUP);
	digtest = add_service("dig", getportnumber("domain"), 0, TOOL_DIG);
	add_service("ntp", getportnumber("ntp"),    0, TOOL_NTP);
	rpctest  = add_service("rpc", getportnumber("sunrpc"), 0, TOOL_RPCINFO);
	httptest = add_service("http", getportnumber("http"),  0, TOOL_HTTP);
	ldaptest = add_service("ldapurl", getportnumber("ldap"), strlen("ldap"), TOOL_LDAP);
	if (pingcolumn) pingtest = add_service(pingcolumn, 0, 0, TOOL_FPING);
	modembanktest = add_service("dialup", 0, 0, TOOL_MODEMBANK);
	add_timestamp("Service definitions loaded");

	load_tests();
	add_timestamp(use_ares_lookup ? "Tests loaded" : "Tests loaded, hostname lookups done");

	flush_dnsqueue();
	if (use_ares_lookup) add_timestamp("DNS lookups completed");

	if (dumpdata & 1) { dump_hostlist(); dump_testitems(); }

	/* Ping checks first */
	if (pingtest && pingtest->items) pingrunning = (start_ping_service(pingtest) == 0);

	/* Load current status files */
	for (handle = rbtBegin(svctree); handle != rbtEnd(svctree); handle = rbtNext(svctree, handle)) {
		s = (service_t *)gettreeitem(svctree, handle);
		if (s != pingtest) load_test_status(s);
	}

	/* First run the TCP/IP and HTTP tests */
	for (handle = rbtBegin(svctree); handle != rbtEnd(svctree); handle = rbtNext(svctree, handle)) {
		s = (service_t *)gettreeitem(svctree, handle);
		if ((s->items) && (s->toolid == TOOL_CONTEST)) {
			char tname[128];

			for (t = s->items; (t); t = t->next) {
				if (!t->host->dnserror) {
					strcpy(tname, s->testname);
					if (s->namelen) tname[s->namelen] = '\0';
					t->privdata = (void *)add_tcp_test(ip_to_test(t->host), s->portnum, tname, NULL, 
									   NULL, t->silenttest, NULL, 
									   NULL, NULL, NULL);
				}
			}
		}
	}
	for (t = httptest->items; (t); t = t->next) add_http_test(t);
	add_timestamp("Test engine setup completed");

	do_tcp_tests(timeout, concurrency);
	add_timestamp("TCP tests completed");

	if (pingrunning) {
		char msg[512];

		finish_ping_service(pingtest); 
		sprintf(msg, "PING test completed (%d hosts)", pingcount);
		add_timestamp(msg);
		combo_start();
		send_results(pingtest, failgoesclear);
		if (selectedhosts == 0) save_ping_status();
		combo_end();
		add_timestamp("PING test results sent");
	}

	if (debug) {
		show_tcp_test_results();
		show_http_test_results(httptest);
	}

	for (handle = rbtBegin(svctree); handle != rbtEnd(svctree); handle = rbtNext(svctree, handle)) {
		s = (service_t *)gettreeitem(svctree, handle);
		if ((s->items) && (s->toolid == TOOL_CONTEST)) {
			for (t = s->items; (t); t = t->next) { 
				/*
				 * If the test fails due to DNS error, t->privdata is NULL
				 */
				if (t->privdata) {
					char *p;
					int i;
					tcptest_t *testresult = (tcptest_t *)t->privdata;

					t->open = testresult->open;
					t->banner = dupstrbuffer(testresult->banner);
					t->certinfo = testresult->certinfo;
					t->certexpires = testresult->certexpires;
					t->duration.tv_sec = testresult->duration.tv_sec;
					t->duration.tv_usec = testresult->duration.tv_usec;

					/* Binary data in banner ... */
					for (i=0, p=STRBUF(t->banner); (i < STRBUFLEN(t->banner)); i++, p++) {
						if (!isprint((int)*p)) *p = '.';
					}
				}
			}
		}
	}
	for (t = httptest->items; (t); t = t->next) {
		if (t->privdata) {
			http_data_t *testresult = (http_data_t *)t->privdata;

			t->certinfo = testresult->tcptest->certinfo;
			t->certexpires = testresult->tcptest->certexpires;
		}
	}

	add_timestamp("Test result collection completed");


	/* Run the ldap tests */
	for (t = ldaptest->items; (t); t = t->next) add_ldap_test(t);
	add_timestamp("LDAP test engine setup completed");

	run_ldap_tests(ldaptest, (ssltestname != NULL), timeout);
	add_timestamp("LDAP tests executed");

	if (debug) show_ldap_test_results(ldaptest);
	for (t = ldaptest->items; (t); t = t->next) {
		if (t->privdata) {
			ldap_data_t *testresult = (ldap_data_t *)t->privdata;

			t->certinfo = testresult->certinfo;
			t->certexpires = testresult->certexpires;
		}
	}
	add_timestamp("LDAP tests result collection completed");


	/* dns, dig, ntp tests */
	for (handle = rbtBegin(svctree); handle != rbtEnd(svctree); handle = rbtNext(svctree, handle)) {
		s = (service_t *)gettreeitem(svctree, handle);
		if (s->items) {
			switch(s->toolid) {
				case TOOL_NSLOOKUP:
					run_nslookup_service(s); 
					add_timestamp("NSLOOKUP tests executed");
					break;
				case TOOL_DIG:
					run_nslookup_service(s); 
					add_timestamp("DIG tests executed");
					break;
				case TOOL_NTP:
					run_ntp_service(s); 
					add_timestamp("NTP tests executed");
					break;
				case TOOL_RPCINFO:
					run_rpcinfo_service(s); 
					add_timestamp("RPC tests executed");
					break;
				case TOOL_MODEMBANK:
					run_modembank_service(s); 
					add_timestamp("Modembank tests executed");
					break;
			}
		}
	}

	combo_start();
	for (handle = rbtBegin(svctree); handle != rbtEnd(svctree); handle = rbtNext(svctree, handle)) {
		s = (service_t *)gettreeitem(svctree, handle);
		switch (s->toolid) {
			case TOOL_CONTEST:
			case TOOL_NSLOOKUP:
			case TOOL_DIG:
			case TOOL_NTP:
				send_results(s, failgoesclear);
				break;

			case TOOL_FPING:
			case TOOL_HTTP:
			case TOOL_LDAP:
				/* These handle result-transmission internally */
				break;

			case TOOL_MODEMBANK:
				send_modembank_results(s);
				break;

			case TOOL_RPCINFO:
				send_rpcinfo_results(s, failgoesclear);
				break;
		}
	}
	for (handle = rbtBegin(testhosttree); (handle != rbtEnd(testhosttree)); handle = rbtNext(testhosttree, handle)) {
		h = (testedhost_t *)gettreeitem(testhosttree, handle);
		send_http_results(httptest, h, h->firsthttp, nonetpage, failgoesclear);
		send_content_results(httptest, h, nonetpage, contenttestname, failgoesclear);
		send_ldap_results(ldaptest, h, nonetpage, failgoesclear);
		if (ssltestname && !h->nosslcert) send_sslcert_status(h);
	}

	combo_end();
	add_timestamp("Test results transmitted");

	/*
	 * The list of hosts to test frequently because of a failure must
	 * be saved - it is then picked up by the frequent-test ext script
	 * that runs bbtest-net again with the frequent-test hosts as
	 * parameter.
	 *
	 * Should the retest itself update the frequent-test file ? It
	 * would allow us to kick hosts from the frequent-test file sooner.
	 * However, it is simpler (no races) if we just let the normal
	 * test-engine be alone in updating the file. 
	 * At the worst, we'll re-test a host going up a couple of times
	 * too much.
	 *
	 * So for now update the list only if we ran with no host-parameters.
	 */
	if (selectedcount == 0) {
		/* Save current status files */
		for (handle = rbtBegin(svctree); handle != rbtEnd(svctree); handle = rbtNext(svctree, handle)) {
			s = (service_t *)gettreeitem(svctree, handle);
			if (s != pingtest) save_test_status(s);
		}
		/* Save frequent-test list */
		save_frequenttestlist(argc, argv);
	}

	shutdown_ldap_library();
	add_timestamp("bbtest-net completed");

	if (dumpdata & 2) { dump_hostlist(); dump_testitems(); }

	/* Tell about us */
	if (egocolumn) {
		char msgline[4096];
		char *timestamps;
		int color;

		/* Go yellow if it runs for too long */
		if (total_runtime() > runtimewarn) {
			errprintf("WARNING: Runtime %ld longer than time limit (%ld)\n", total_runtime(), runtimewarn);
		}
		color = (errbuf ? COL_YELLOW : COL_GREEN);

		combo_start();
		init_status(color);
		sprintf(msgline, "status %s.%s %s %s\n\n", xgetenv("MACHINE"), egocolumn, colorname(color), timestamp);
		addtostatus(msgline);

		sprintf(msgline, "bbtest-net version %s\n", VERSION);
		addtostatus(msgline);
		if (ssl_library_version) {
			sprintf(msgline, "SSL library : %s\n", ssl_library_version);
			addtostatus(msgline);
		}
		if (ldap_library_version) {
			sprintf(msgline, "LDAP library: %s\n", ldap_library_version);
			addtostatus(msgline);
		}

		sprintf(msgline, "\nStatistics:\n Hosts total           : %8d\n Hosts with no tests   : %8d\n Total test count      : %8d\n Status messages       : %8d\n Alert status msgs     : %8d\n Transmissions         : %8d\n", 
			hostcount, notesthostcount, testcount, bbstatuscount, bbnocombocount, bbmsgcount);
		addtostatus(msgline);
		sprintf(msgline, "\nDNS statistics:\n # hostnames resolved  : %8d\n # succesful           : %8d\n # failed              : %8d\n # calls to dnsresolve : %8d\n",
			dns_stats_total, dns_stats_success, dns_stats_failed, dns_stats_lookups);
		addtostatus(msgline);
		sprintf(msgline, "\nTCP test statistics:\n # TCP tests total     : %8d\n # HTTP tests          : %8d\n # Simple TCP tests    : %8d\n # Connection attempts : %8d\n # bytes written       : %8ld\n # bytes read          : %8ld\n",
			tcp_stats_total, tcp_stats_http, tcp_stats_plain, tcp_stats_connects, 
			tcp_stats_written, tcp_stats_read);
		addtostatus(msgline);

		if (errbuf) {
			addtostatus("\n\nError output:\n");
			addtostatus(errbuf);
		}

		show_timestamps(&timestamps);
		addtostatus(timestamps);

		finish_status();
		combo_end();
	}
	else show_timestamps(NULL);

	return 0;
}

