/*----------------------------------------------------------------------------*/
/* Hobbit message daemon.                                                     */
/*                                                                            */
/* This is the main alert module for hobbitd. It receives alert messages,     */
/* keeps track of active alerts, enable/disable, acks etc., and triggers      */
/* outgoing alerts by calling send_alert().                                   */
/*                                                                            */
/* Copyright (C) 2004-2005 Henrik Storner <henrik@hswn.dk>                    */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

/*
 * Information from the Hobbit docs about "page" modules:
 *
 *   page
 *   ----
 *   @@page|timestamp|sender|hostname|testname|expiretime|color|prevcolor|changetime|location
 *   <message>
 *   @@
 *
 *   @@ack|timestamp|sender|hostname|testname|expiretime
 *   <ack message>
 *   @@
 *
 *   @@notify|timestamp|sender|hostname|testname|pagepath
 *   <notify message>
 *   @@
 *
 *   Note that "page" modules get messages whenever the alert-state of a test
 *   changes. I.e. a message is generated whenever a test goes from a color
 *   that is non-alerting to a color that is alerting, or vice versa.
 *
 *   How does the pager know when a test is disabled ? It will get a "page"
 *   message with color=blue, if the old color of the test was in an alert
 *   state. (If it wasn't, the pager module does not need to know that the
 *   test has been disabled). It should then clear any stored info about
 *   active alerts for this host.test combination.
 */

static char rcsid[] = "$Id: hobbitd_alert.c,v 1.70 2006-03-18 07:38:37 henrik Exp $";

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <limits.h>

#include "libbbgen.h"

#include "hobbitd_worker.h"
#include "do_alert.h"

static volatile int running = 1;
static volatile time_t nextcheckpoint = 0;

RbtHandle hostnames;
RbtHandle testnames;
RbtHandle locations;
activealerts_t *ahead = NULL;

char *statename[] = {
	/* A_PAGING, A_NORECIP, A_ACKED, A_RECOVERED, A_NOTIFY, A_DEAD */
	"paging", "norecip", "acked", "recovered", "notify", "dead"
};

char *find_name(RbtHandle tree, char *name)
{
	char *result;
	RbtIterator handle;

	handle = rbtFind(tree, name);
	if (handle == rbtEnd(tree)) {
		result = strdup(name);
		rbtInsert(tree, result, NULL);
	}
	else {
		void *k1, *k2;

		rbtKeyValue(tree, handle, &k1, &k2);
		result = (char *)k1;
	}
	
	return result;
}

activealerts_t *find_active(char *hostname, char *testname)
{
	RbtIterator handle;
	void *k1, *k2;
	char *hwalk, *twalk;
	activealerts_t *awalk;

	handle = rbtFind(hostnames, hostname);
	if (handle == rbtEnd(hostnames)) return NULL;
	rbtKeyValue(hostnames, handle, &k1, &k2);
	hwalk = (char *)k1;

	handle = rbtFind(testnames, testname);
	if (handle == rbtEnd(testnames)) return NULL;
	rbtKeyValue(testnames, handle, &k1, &k2);
	twalk = (char *)k1;

	for (awalk = ahead; (awalk && ((awalk->hostname != hwalk) || (awalk->testname != twalk))); awalk=awalk->next) ;
	return awalk;
}


void sig_handler(int signum)
{
	switch (signum) {
	  case SIGCHLD:
		  break;

	  case SIGUSR1:
		  nextcheckpoint = 0;
		  break;

	  default:
		  running = 0;
		  break;
	}
}

void save_checkpoint(char *filename)
{
	char *subfn;
	FILE *fd = fopen(filename, "w");
	activealerts_t *awalk;
	unsigned char *pgmsg = "", *ackmsg = "";

	if (fd == NULL) return;

	for (awalk = ahead; (awalk); awalk = awalk->next) {
		if (awalk->state == A_DEAD) continue;

		fprintf(fd, "%s|%s|%s|%s|%s|%d|%d|%s|",
			awalk->hostname, awalk->testname, awalk->location, awalk->ip,
			colorname(awalk->maxcolor),
			(int) awalk->eventstart,
			(int) awalk->nextalerttime,
			statename[awalk->state]);
		if (awalk->pagemessage) pgmsg = nlencode(awalk->pagemessage);
		fprintf(fd, "%s|", pgmsg);
		if (awalk->ackmessage) ackmsg = nlencode(awalk->ackmessage);
		fprintf(fd, "%s\n", ackmsg);
	}
	fclose(fd);

	subfn = (char *)malloc(strlen(filename)+5);
	sprintf(subfn, "%s.sub", filename);
	save_state(subfn);
	xfree(subfn);
}

void load_checkpoint(char *filename)
{
	char *subfn;
	FILE *fd;
	char *inbuf = NULL;
	int inbufsz;
	char statuscmd[1024];
	char *statusbuf = NULL;

	fd = fopen(filename, "r");
	if (fd == NULL) return;

	sprintf(statuscmd, "hobbitdboard color=%s fields=hostname,testname,color", xgetenv("ALERTCOLORS"));
	sendmessage(statuscmd, NULL, NULL, &statusbuf, 1, BBTALK_TIMEOUT);

	initfgets(fd);
	while (unlimfgets(&inbuf, &inbufsz, fd)) {
		char *item[20], *p;
		int i;

		p = strchr(inbuf, '\n'); if (p) *p = '\0';

		i = 0; p = gettok(inbuf, "|");
		while (p && (i < 20)) {
			item[i++] = p;
			p = gettok(NULL, "|");
		}

		if (i == 9) {
			/* There was no ack message */
			item[i++] = "";
		}

		if (i > 9) {
			char *key, *valid = NULL;
			activealerts_t *newalert = (activealerts_t *)malloc(sizeof(activealerts_t));
			newalert->hostname = find_name(hostnames, item[0]);
			newalert->testname = find_name(testnames, item[1]);
			newalert->location = find_name(locations, item[2]);
			strcpy(newalert->ip, item[3]);
			newalert->color = newalert->maxcolor = parse_color(item[4]);
			newalert->eventstart = (time_t) atoi(item[5]);
			newalert->nextalerttime = (time_t) atoi(item[6]);
			newalert->state = A_PAGING;

			if (statusbuf) {
				key = (char *)malloc(strlen(newalert->hostname) + strlen(newalert->testname) + 100);
				sprintf(key, "\n%s|%s|%s\n", newalert->hostname, newalert->testname, colorname(newalert->color));
				valid = strstr(statusbuf, key);
				if (!valid && (strncmp(statusbuf, key+1, strlen(key+1)) == 0)) valid = statusbuf;
			}
			if (!valid) {
				errprintf("Stale alert for %s:%s dropped\n", newalert->hostname, newalert->testname);
				xfree(newalert);
				continue;
			}

			while (strcmp(item[7], statename[newalert->state]) && (newalert->state < A_DEAD)) 
				newalert->state++;
			/* Config might have changed while we were down */
			if (newalert->state == A_NORECIP) newalert->state = A_PAGING;
			newalert->pagemessage = newalert->ackmessage = NULL;
			if (strlen(item[8])) {
				nldecode(item[8]);
				newalert->pagemessage = strdup(item[8]);
			}
			if (strlen(item[9])) {
				nldecode(item[9]);
				newalert->ackmessage = strdup(item[9]);
			}
			newalert->next = ahead;
			ahead = newalert;
		}
	}
	fclose(fd);
	if (inbuf) xfree(inbuf);

	subfn = (char *)malloc(strlen(filename)+5);
	sprintf(subfn, "%s.sub", filename);
	load_state(subfn, statusbuf);
	xfree(subfn);
	if (statusbuf) xfree(statusbuf);
}

int main(int argc, char *argv[])
{
	char *msg;
	int seq;
	int argi;
	int alertcolors, alertinterval;
	char *configfn = NULL;
	char *checkfn = NULL;
	int checkpointinterval = 900;
	char acklogfn[PATH_MAX];
	FILE *acklogfd = NULL;
	char notiflogfn[PATH_MAX];
	FILE *notiflogfd = NULL;
	char *tracefn = NULL;
	struct sigaction sa;
	int configchanged;
	time_t lastxmit = 0;

	MEMDEFINE(acklogfn);
	MEMDEFINE(notiflogfn);

	/* Dont save the error buffer */
	save_errbuf = 0;

	/* Load alert config */
	alertcolors = colorset(xgetenv("ALERTCOLORS"), ((1 << COL_GREEN) | (1 << COL_BLUE)));
	alertinterval = 60*atoi(xgetenv("ALERTREPEAT"));

	/* Create our loookup-trees */
	hostnames = rbtNew(name_compare);
	testnames = rbtNew(name_compare);
	locations = rbtNew(name_compare);

	for (argi=1; (argi < argc); argi++) {
		if (argnmatch(argv[argi], "--debug")) {
			debug = 1;
		}
		else if (argnmatch(argv[argi], "--config=")) {
			configfn = strdup(strchr(argv[argi], '=')+1);
		}
		else if (argnmatch(argv[argi], "--checkpoint-file=")) {
			checkfn = strdup(strchr(argv[argi], '=')+1);
		}
		else if (argnmatch(argv[argi], "--checkpoint-interval=")) {
			char *p = strchr(argv[argi], '=') + 1;
			checkpointinterval = atoi(p);
		}
		else if (argnmatch(argv[argi], "--dump-config")) {
			load_alertconfig(configfn, alertcolors, alertinterval);
			dump_alertconfig();
			return 0;
		}
		else if (argnmatch(argv[argi], "--cfid")) {
			include_configid = 1;
		}
		else if (argnmatch(argv[argi], "--test")) {
			char *testhost = NULL, *testservice = NULL, *testpage = NULL, *testcolor = "red";
			namelist_t *hinfo;
			int testdur = 0;
			FILE *logfd = NULL;
			activealerts_t *awalk = NULL;;

			argi++; if (argi < argc) testhost = argv[argi];
			argi++; if (argi < argc) testservice = argv[argi];
			argi++; if (argi < argc) testdur = atoi(argv[argi]);
			argi++; if (argi < argc) testcolor = argv[argi];
			argi++; if (argi < argc) fakestarttime = (time_t) atoi(argv[argi]);

			if ((testhost == NULL) || (testservice == NULL)) {
				printf("Usage: hobbitd_alert --test HOST SERVICE [duration [color [time]]]\n");
				return 1;
			}

			load_hostnames(xgetenv("BBHOSTS"), NULL, get_fqdn());
			hinfo = hostinfo(testhost);
			if (hinfo) {
				testpage = strdup(bbh_item(hinfo, BBH_PAGEPATH));
			}
			else {
				errprintf("Host not found in bb-hosts - assuming it is on the top page\n");
				testpage = "";
			}

			awalk = (activealerts_t *)malloc(sizeof(activealerts_t));
			awalk->hostname = find_name(hostnames, testhost);
			awalk->testname = find_name(testnames, testservice);
			awalk->location = find_name(locations, testpage);
			strcpy(awalk->ip, "127.0.0.1");
			awalk->color = awalk->maxcolor = parse_color(testcolor);
			awalk->pagemessage = "Test of the alert configuration";
			awalk->ackmessage = NULL;
			awalk->eventstart = time(NULL) - testdur*60;
			awalk->nextalerttime = 0;
			awalk->state = A_PAGING;
			awalk->cookie = 12345;
			awalk->next = NULL;

			logfd = fopen("/dev/null", "w");
			starttrace(NULL);
			testonly = 1;

			load_alertconfig(configfn, alertcolors, alertinterval);
			send_alert(awalk, logfd);
			return 0;
		}
		else if (argnmatch(argv[argi], "--trace=")) {
			tracefn = strdup(strchr(argv[argi], '=')+1);
			starttrace(tracefn);
		}
		else {
			errprintf("Unknown option '%s'\n", argv[argi]);
		}
	}

	if (checkfn) {
		load_checkpoint(checkfn);
		nextcheckpoint = time(NULL) + checkpointinterval;
		dprintf("Next checkpoint at %d, interval %d\n", (int) nextcheckpoint, checkpointinterval);
	}

	setup_signalhandler("hobbitd_alert");
	/* Need to handle these ourselves, so we can shutdown and save state-info */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sig_handler;
	sigaction(SIGPIPE, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT,  &sa, NULL);
	sigaction(SIGCHLD, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);

	if (xgetenv("BBSERVERLOGS")) {
		sprintf(acklogfn, "%s/acknowledge.log", xgetenv("BBSERVERLOGS"));
		acklogfd = fopen(acklogfn, "a");
		sprintf(notiflogfn, "%s/notifications.log", xgetenv("BBSERVERLOGS"));
		notiflogfd = fopen(notiflogfn, "a");
	}

	/*
	 * The general idea here is that this loop handles receiving of alert-
	 * and ack-messages from the master daemon, and maintains a list of 
	 * host+test combinations that may have alerts going out.
	 *
	 * This module does not deal with any specific alert-configuration, 
	 * it just picks up the alert messages, maintains the list of 
	 * known tests that are in some sort of critical condition, and
	 * periodically pushes alerts to the do_alert.c module for handling.
	 *
	 * The only modification of alerts that happen here is the handling
	 * of when the next alert is due. It calls into the next_alert() 
	 * routine to learn when an alert should be repeated, and also 
	 * deals with Acknowledgments that stop alerts from going out for
	 * a period of time.
	 */
	while (running) {
		char *eoln, *restofmsg;
		char *metadata[20];
		char *p;
		int metacount;
		char *hostname = NULL, *testname = NULL;
		struct timeval timeout;
		time_t now;
		int anytogo;
		activealerts_t *awalk, *khead, *tmp;
		int childstat;

		now = time(NULL);
		if (checkfn && (now > nextcheckpoint)) {
			dprintf("Saving checkpoint\n");
			nextcheckpoint = now + checkpointinterval;
			save_checkpoint(checkfn);

			if (acklogfd) acklogfd = freopen(acklogfn, "a", acklogfd);
			if (notiflogfd) notiflogfd = freopen(notiflogfn, "a", notiflogfd);
		}

		timeout.tv_sec = 60; timeout.tv_usec = 0;
		msg = get_hobbitd_message(C_PAGE, "hobbitd_alert", &seq, &timeout);
		if (msg == NULL) {
			running = 0;
			continue;
		}

		/* See what time it is - must happen AFTER the timeout */
		now = time(NULL);

		/* Split the message in the first line (with meta-data), and the rest */
 		eoln = strchr(msg, '\n');
		if (eoln) {
			*eoln = '\0';
			restofmsg = eoln+1;
		}
		else {
			restofmsg = "";
		}

		/* 
		 * Now parse the meta-data into elements.
		 * We use our own "gettok()" routine which works
		 * like strtok(), but can handle empty elements.
		 */
		metacount = 0; 
		p = gettok(msg, "|");
		while (p && (metacount < 19)) {
			metadata[metacount] = p;
			metacount++;
			p = gettok(NULL, "|");
		}
		metadata[metacount] = NULL;

		if (metacount > 3) hostname = metadata[3];
		if (metacount > 4) testname = metadata[4];

		if ((metacount > 10) && (strncmp(metadata[0], "@@page", 6) == 0)) {
			/* @@page|timestamp|sender|hostname|testname|hostip|expiretime|color|prevcolor|changetime|location|cookie */

			int newcolor, newalertstatus, oldalertstatus;

			dprintf("Got page message from %s:%s\n", hostname, testname);
			traceprintf("@@page %s:%s:%s=%s\n", hostname, testname, metadata[10], metadata[7]);

			awalk = find_active(hostname, testname);
			if (awalk == NULL) {
				char *hwalk = find_name(hostnames, hostname);
				char *twalk = find_name(testnames, testname);
				char *pwalk = find_name(locations, metadata[10]);

				awalk = (activealerts_t *)calloc(1, sizeof(activealerts_t));
				awalk->hostname = hwalk;
				awalk->testname = twalk;
				awalk->location = pwalk;
				awalk->cookie = -1;
				awalk->state = A_DEAD;
				/*
				 * Use changetime here, if we restart the alert module then
				 * this gets the duration values more right than using "now".
				 * Also, define this only when a new alert arrives - we should
				 * NOT clear this when a status goes yellow->red, or if it
				 * flaps between yellow and red.
				 */
				awalk->eventstart = atoi(metadata[9]);
				awalk->next = ahead;
				ahead = awalk;
				traceprintf("New record\n");
			}

			newcolor = parse_color(metadata[7]);
			oldalertstatus = ((alertcolors & (1 << awalk->color)) != 0);
			newalertstatus = ((alertcolors & (1 << newcolor)) != 0);

			traceprintf("state %d->%d\n", oldalertstatus, newalertstatus);

			if (newalertstatus) {
				/* It's in an alert state. */
				awalk->color = newcolor;
				awalk->state = A_PAGING;

				if (newcolor > awalk->maxcolor) {
					if (awalk->maxcolor != 0) {
						/*
						 * Severity has increased (yellow -> red).
						 * Clear the repeat-interval, and set maxcolor to
						 * the new color. If it drops to yellow again,
						 * maxcolor stays at red, so a test that flaps
						 * between yellow and red will only alert on red
						 * the first time, and then follow the repeat
						 * interval.
						 */
						dprintf("Severity increased, cleared repeat interval: %s/%s %s->%s\n",
							awalk->hostname, awalk->testname,
							colorname(awalk->maxcolor), colorname(newcolor));
						clear_interval(awalk);
					}

					awalk->maxcolor = newcolor;
				}
			}
			else {
				/* 
				 * Send one "recovered" message out now, then go to A_DEAD.
				 * Dont update the color here - we want recoveries to go out 
				 * only if the alert color triggered an alert
				 */
				awalk->state = A_RECOVERED;
			}

			if (oldalertstatus != newalertstatus) {
				dprintf("Alert status changed from %d to %d\n", oldalertstatus, newalertstatus);
				clear_interval(awalk);
			}

			strcpy(awalk->ip, metadata[5]);
			awalk->cookie = atoi(metadata[11]);

			if (awalk->pagemessage) xfree(awalk->pagemessage);
			awalk->pagemessage = strdup(restofmsg);
		}
		else if ((metacount > 5) && (strncmp(metadata[0], "@@ack", 5) == 0)) {
 			/* @@ack|timestamp|sender|hostname|testname|hostip|expiretime */

			/*
			 * An ack is handled simply by setting the next
			 * alert-time to when the ack expires.
			 */
			time_t nextalert = atoi(metadata[6]);

			dprintf("Got ack message from %s:%s\n", hostname, testname);
			traceprintf("@@ack: %s:%s now=%d, ackeduntil %d\n",
				     hostname, testname, (int)now, (int)nextalert);

			awalk = find_active(hostname, testname);
			if (awalk && (awalk->state == A_PAGING)) {
				traceprintf("Record updated\n");
				if (acklogfd) {
					fprintf(acklogfd, "%d\t%d\t%d\t%d\t%s\t%s.%s\t%s\t%s\n",
						(int)now, awalk->cookie, 
						(int)((nextalert - now) / 60), awalk->cookie,
						"np_filename_not_used", 
						hostname, testname, 
						colorname(awalk->color),
						nlencode(restofmsg));
					fflush(acklogfd);
				}
				awalk->state = A_ACKED;
				awalk->nextalerttime = nextalert;
				if (awalk->ackmessage) xfree(awalk->ackmessage);
				awalk->ackmessage = strdup(restofmsg);
			}
			else {
				traceprintf("No record\n");
			}
		}
		else if ((metacount > 4) && (strncmp(metadata[0], "@@notify", 5) == 0)) {
			/* @@notify|timestamp|sender|hostname|testname|pagepath */

			char *hwalk = find_name(hostnames, hostname);
			char *twalk = find_name(testnames, testname);
			char *pwalk = find_name(locations, (metadata[5] ? metadata[5] : ""));

			awalk = (activealerts_t *)malloc(sizeof(activealerts_t));
			awalk->hostname = hwalk;
			awalk->testname = twalk;
			awalk->ip[0] = '\0';
			awalk->location = pwalk;
			awalk->color = 0;
			awalk->cookie = -1;
			awalk->pagemessage = strdup(restofmsg);
			awalk->ackmessage = NULL;
			awalk->eventstart = time(NULL);
			awalk->nextalerttime = 0;
			awalk->state = A_NOTIFY;
			awalk->next = ahead;
			ahead = awalk;
		}
		else if ((metacount > 3) && 
			 ((strncmp(metadata[0], "@@drophost", 10) == 0) || (strncmp(metadata[0], "@@dropstate", 11) == 0))) {
			/* @@drophost|timestamp|sender|hostname */
			/* @@dropstate|timestamp|sender|hostname */
			RbtIterator handle;

			handle = rbtFind(hostnames, hostname);
			if (handle != rbtEnd(hostnames)) {
				void *k1, *k2;
				char *hwalk;

				rbtKeyValue(hostnames, handle, &k1, &k2);
				hwalk = (char *)k1;

				for (awalk = ahead; (awalk); awalk = awalk->next) {
					if (awalk->hostname == hwalk) awalk->state = A_DEAD;
				}
			}
		}
		else if ((metacount > 4) && (strncmp(metadata[0], "@@droptest", 10) == 0)) {
			/* @@droptest|timestamp|sender|hostname|testname */

			awalk = find_active(hostname, testname);
			if (awalk) awalk->state = A_DEAD;
		}
		else if ((metacount > 4) && (strncmp(metadata[0], "@@renamehost", 12) == 0)) {
			/* @@renamehost|timestamp|sender|hostname|newhostname */
			htnames_t *hwalk;

			/* 
			 * We handle rename's simply by dropping the alert. If there is still an
			 * active alert for the host, it will have to be dealt with when the next
			 * status update arrives.
			 */
			RbtIterator handle;

			handle = rbtFind(hostnames, hostname);
			if (handle != rbtEnd(hostnames)) {
				void *k1, *k2;
				char *hwalk;

				rbtKeyValue(hostnames, handle, &k1, &k2);
				hwalk = (char *)k1;

				for (awalk = ahead; (awalk); awalk = awalk->next) {
					if (awalk->hostname == hwalk) awalk->state = A_DEAD;
				}
			}
		}
		else if ((metacount > 5) && (strncmp(metadata[0], "@@renametest", 12) == 0)) {
			/* @@renametest|timestamp|sender|hostname|oldtestname|newtestname */

			/* 
			 * We handle rename's simply by dropping the alert. If there is still an
			 * active alert for the host, it will have to be dealt with when the next
			 * status update arrives.
			 */
			awalk = find_active(hostname, testname);
			if (awalk) awalk->state = A_DEAD;
		}
		else if (strncmp(metadata[0], "@@shutdown", 10) == 0) {
			running = 0;
			continue;
		}
		else if (strncmp(metadata[0], "@@logrotate", 11) == 0) {
			char *fn = xgetenv("HOBBITCHANNEL_LOGFILENAME");
			if (fn && strlen(fn)) {
				freopen(fn, "a", stdout);
				freopen(fn, "a", stderr);

				if (tracefn) {
					stoptrace();
					starttrace(tracefn);
				}
			}
			continue;
		}
		else if (strncmp(metadata[0], "@@idle", 6) == 0) {
			/* Timeout */
		}

		/*
		 * When a burst of alerts happen, we get lots of alert messages
		 * coming in quickly. So lets handle them in bunches and only 
		 * do the full alert handling once every 10 secs - that lets us
		 * combine a bunch of alerts into one transmission process.
		 */
		if (now < (lastxmit+10)) continue;
		lastxmit = now;

		/* 
		 * Loop through the activealerts list and see if anything is pending.
		 * This is an optimization, we could just as well just fork off the
		 * notification child and let it handle all of it. But there is no
		 * reason to fork a child process unless it is going to do something.
		 */
		configchanged = load_alertconfig(configfn, alertcolors, alertinterval);
		anytogo = 0;
		for (awalk = ahead; (awalk); awalk = awalk->next) {
			int anymatch = 0;

			switch (awalk->state) {
			  case A_NORECIP:
				if (!configchanged) break;

				/* The configuration has changed - switch NORECIP -> PAGING */
				awalk->state = A_PAGING;
				clear_interval(awalk);
				/* Fall through */

			  case A_PAGING:
				if (have_recipient(awalk, &anymatch)) {
					if (awalk->nextalerttime <= now) anytogo++;
				}
				else {
					if (!anymatch) {
						awalk->state = A_NORECIP;
						cleanup_alert(awalk);
					}
				}
				break;

			  case A_ACKED:
				if (awalk->nextalerttime <= now) {
					/* An ack has expired, so drop the ack message and switch to A_PAGING */
					anytogo++;
					if (awalk->ackmessage) xfree(awalk->ackmessage);
					awalk->state = A_PAGING;
				}
				break;

			  case A_RECOVERED:
			  case A_NOTIFY:
				anytogo++;
				break;

			  case A_DEAD:
				break;
			}
		}
		dprintf("%d alerts to go\n", anytogo);

		if (anytogo) {
			pid_t childpid;

			childpid = fork();
			if (childpid == 0) {
				/* The child */
				start_alerts();
				for (awalk = ahead; (awalk); awalk = awalk->next) {
					switch (awalk->state) {
					  case A_PAGING:
						if (awalk->nextalerttime <= now) {
							send_alert(awalk, notiflogfd);
						}
						break;

					  case A_ACKED:
						/* Cannot be A_ACKED unless the ack is still valid, so no alert. */
						break;

					  case A_RECOVERED:
					  case A_NOTIFY:
						send_alert(awalk, notiflogfd);
						break;

					  case A_NORECIP:
					  case A_DEAD:
						break;
					}
				}
				finish_alerts();

				/* Child does not continue */
				exit(0);
			}
			else if (childpid < 0) {
				errprintf("Fork failed, cannot send alerts: %s\n", strerror(errno));
			}
		}

		/* Update the state flag and the next-alert timestamp */
		for (awalk = ahead; (awalk); awalk = awalk->next) {
			switch (awalk->state) {
			  case A_PAGING:
				if (awalk->nextalerttime <= now) awalk->nextalerttime = next_alert(awalk);
				break;

			  case A_NORECIP:
				break;

			  case A_ACKED:
				/* Still cannot get here except if ack is still valid */
				break;

			  case A_RECOVERED:
			  case A_NOTIFY:
				awalk->state = A_DEAD;
				/* Fall through */

			  case A_DEAD:
				cleanup_alert(awalk); 
				break;
			}
		}

		/* Two-phase cleanup. All A_DEAD items are moved to a kill-list. */
		khead = NULL; awalk = ahead;
		while (awalk) {
			if ((awalk == ahead) && (awalk->state == A_DEAD)) {
				/* head of alert chain is going away */

				/* Unlink ahead from the chain ... */
				tmp = ahead;
				ahead = ahead->next;

				/* ... and link it into the kill-list */
				tmp->next = khead;
				khead = tmp;

				/* We're still at the head of the chain. */
				awalk = ahead;
			}
			else if (awalk->next && (awalk->next->state == A_DEAD)) {
				/* Unlink awalk->next from the chain ... */
				tmp = awalk->next;
				awalk->next = tmp->next;

				/* ... and link it into the kill-list */
				tmp->next = khead;
				khead = tmp;

				/* awalk stays unchanged */
			}
			else {
				awalk = awalk->next;
			}
		}

		/* khead now holds a list of dead items */
		while (khead) {
			tmp = khead;
			khead = khead->next;

			if (tmp->pagemessage) xfree(tmp->pagemessage);
			if (tmp->ackmessage) xfree(tmp->ackmessage);
			xfree(tmp);
		}

		/* Pickup any finished child processes to avoid zombies */
		while (wait3(&childstat, WNOHANG, NULL) > 0) ;
	}

	if (checkfn) save_checkpoint(checkfn);
	if (acklogfd) fclose(acklogfd);
	if (notiflogfd) fclose(notiflogfd);
	stoptrace();

	MEMUNDEFINE(notiflogfn);
	MEMUNDEFINE(acklogfn);

	return 0;
}

