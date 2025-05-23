/*----------------------------------------------------------------------------*/
/* Xymon message daemon.                                                      */
/*                                                                            */
/* This is the master daemon, xymond.                                         */
/*                                                                            */
/* This is a daemon that implements the Big Brother network protocol, with    */
/* additional protocol items implemented for Xymon.                           */
/*                                                                            */
/* This daemon maintains the full state of the Xymon system in memory,        */
/* eliminating the need for file-based storage of e.g. status logs. The web   */
/* frontend programs (xymongen, combostatus, hostsvc.cgi etc) can retrieve    */
/* current statuslogs from this daemon to build the Xymon webpages. However,  */
/* a "plugin" mechanism is also implemented to allow "worker modules" to      */
/* pickup various types of events that occur in the system. This allows       */
/* such modules to e.g. maintain the standard Xymon file-based storage, or    */
/* implement history logging or RRD database updates. This plugin mechanism   */
/* uses System V IPC mechanisms for a high-performance/low-latency communi-   */
/* cation between  xymond and the worker modules - under no circumstances     */
/* should the daemon be tasked with storing data to a low-bandwidth channel.  */
/*                                                                            */
/* Copyright (C) 2004-2011 Henrik Storner <henrik@hswn.dk>                    */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

static char rcsid[] = "$Id$";

#include <limits.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <ctype.h>
#include <signal.h>
#include <time.h>
#include <errno.h>

#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <sys/msg.h>

#include "libxymon.h"

#define DISABLED_UNTIL_OK -1

/* The initial size of an input buffer. Make this large enough for most traffic. */
#define XYMON_INBUF_INITIAL   (128*1024)

/* How much the input buffer grows per re-allocation */
#define XYMON_INBUF_INCREMENT (256*1024)

/* How long can the first line of a status message be? This is for the copy statically copied */
#define MAXLINE1SIZE 255

/* How long to keep an ack after the status has recovered */
#define ACKCLEARDELAY 720 /* 12 minutes */

/* How long messages are good for (by default) before going purple - minutes */
#define DEFAULT_VALIDITY 30

/* How many subsequent messages 'modify' overrides are valid for - messages */
#define DEFAULT_MODIFY_VALIDITY 3

#define DEFAULT_PURPLE_INTERVAL 60
int purplecheckinterval = DEFAULT_PURPLE_INTERVAL; /* Seconds - check for purples every 60s */

#define DEFAULT_STATS_INTERVAL (5*60)
int statsinterval = DEFAULT_STATS_INTERVAL;	/* Seconds - report xymond status every 5m */

/* How long are sub-client messages valid */
#define MAX_SUBCLIENT_LIFETIME ((DEFAULT_VALIDITY * 60) + (DEFAULT_PURPLE_INTERVAL*2)) /* 30 minutes + a bit */

#define DEFAULT_FLAPCOUNT 5
int flapcount = DEFAULT_FLAPCOUNT;
int flapthreshold = (DEFAULT_FLAPCOUNT+1)*5*60;	/* Seconds - if more than flapcount changes during this period, it's flapping */

#define DEFAULT_CHECKPOINT_INTERVAL 900
int checkpointinterval = DEFAULT_CHECKPOINT_INTERVAL;	/* Seconds - how often to save checkpoint file */

#define DEFAULT_RELOAD_INTERVAL 600
int reloadinterval = DEFAULT_RELOAD_INTERVAL;	/* Seconds - how often to check hosts.cfg for changes */


typedef struct ackinfo_t {
	int level;
	time_t received, validuntil, cleartime;
	char *ackedby;
	char *msg;
	struct ackinfo_t *next;
} ackinfo_t;

typedef struct testinfo_t {
	char *name;
	int clientsave;
} testinfo_t;

enum modifytype_t { MODIFY_NORM, MODIFY_DOWN, MODIFY_UP };

typedef struct modifier_t {
	char *source, *cause;
	int color;
	int valid;		/* # of next status messages to modify */
	enum modifytype_t type;	/* normal, onlydown, onlyup */
	time_t validtime;	/* time the modify is valid until */
	struct modifier_t *next;
} modifier_t;

/* This holds all information about a single status */
typedef struct xymond_log_t {
	struct xymond_hostlist_t *host;
	testinfo_t *test;
	char *origin;
	int color, oldcolor, activealert, histsynced, downtimeactive, flapping, oldflapcolor, currflapcolor;
	char *testflags;
	char *grouplist;        /* For extended status reports (e.g. from xymond_client) */
	char *sender;
	time_t *lastchange;	/* Table of times when the currently logged status began */
	time_t logtime;		/* time when last update was received */
	time_t validtime;	/* time when status is no longer valid */
	time_t enabletime;	/* time when test auto-enables after a disable */
	time_t acktime;		/* time when test acknowledgement expires */
	time_t redstart, yellowstart;
	int maxackedcolor;	/* The most severe color that has been acked */
	unsigned char *message;
	unsigned char *line1;	/* The first line of the message, starting with color */
	size_t msgsz;
	unsigned char *dismsg, *ackmsg;
	char *cookie;
	time_t cookieexpires;
	struct modifier_t *modifiers;
	char  *modifierbuf;	/* nl-encoded list of all modifier messages (needed on channel posts, so we need it cached) */
	ackinfo_t *acklist;	/* Holds list of acks */
	unsigned long statuschangecount;
	struct xymond_log_t *next;
} xymond_log_t;

typedef struct clientmsg_list_t {
	char *collectorid;
	time_t timestamp;
	char *msg;
	struct clientmsg_list_t *next;
} clientmsg_list_t;

/* This is a list of the hosts we have seen reports for, and links to their status logs */
typedef struct xymond_hostlist_t {
	char *hostname;
	char *ip;
	enum { H_NORMAL, H_SUMMARY } hosttype;
	xymond_log_t *logs;
	xymond_log_t *pinglog; /* Points to entry in logs list, but we need it often */
	clientmsg_list_t *clientmsgs;
	time_t clientmsgtstamp;
} xymond_hostlist_t;

typedef struct filecache_t {
	char *fn;
	long len;
	unsigned char *fdata;
} filecache_t;

typedef struct senderstats_t {
	char *senderip;
	unsigned long msgcount;
} senderstats_t;

void *rbhosts;				/* The hosts we have reports from */
void *rbtests;				/* The tests (columns) we have seen */
void *rborigins;			/* The origins we have seen */
void *rbcookies;			/* The cookies we use */
void *rbfilecache;
void *rbsenders;

void *maintsenders = NULL;
void *statussenders = NULL;
void *adminsenders = NULL;
void *wwwsenders = NULL;
void *tracelist = NULL;

int      traceall = 0;
int      ignoretraced = 0;
int      clientsavemem = 1;	/* In memory */
int      clientsavedisk = 0;	/* On disk via the CLICHG channel */
int      allow_downloads = 1;
int	 defaultvalidity = 30;	/* Minutes */
int	 ackeachcolor = 0;
int	 defaultcookietime = 86400;	/* 1 day */

/* This struct describes an active connection with a Xymon client */
typedef struct conn_t {
	char *sender;
	char *certcn;
	unsigned char *buf, *bufp;	/* Message buffer and pointer */
	int msgsz;
	size_t buflen, bufsz;		/* Active and maximum length of buffer */
	enum { NOTALK, RECEIVING, STARTTLSWAIT, RESPONDING } doingwhat;	/* Communications state (NOTALK, READING, RESPONDING) */
} conn_t;

enum droprencmd_t { CMD_DROPHOST, CMD_DROPTEST, CMD_RENAMEHOST, CMD_RENAMETEST, CMD_DROPSTATE };

static volatile int running = 1;
static volatile int reloadconfig = 0;
static volatile time_t nextcheckpoint = 0;
static volatile time_t nextreload = 0;
static volatile int dologswitch = 0;
static volatile int gotalarm = 0;

/* Our channels to worker modules */
xymond_channel_t *statuschn = NULL;	/* Receives full "status" messages */
xymond_channel_t *stachgchn = NULL;	/* Receives brief message about a status change */
xymond_channel_t *pagechn   = NULL;	/* Receives alert messages (triggered from status changes) */
xymond_channel_t *datachn   = NULL;	/* Receives raw "data" messages */
xymond_channel_t *noteschn  = NULL;	/* Receives raw "notes" messages */
xymond_channel_t *enadischn = NULL;	/* Receives "enable" and "disable" messages */
xymond_channel_t *clientchn = NULL;	/* Receives "client" messages */
xymond_channel_t *clichgchn = NULL;	/* Receives "clichg" messages */
xymond_channel_t *userchn   = NULL;	/* Receives "usermsg" messages */

static int backfeedqueue = -1;
static unsigned long backfeedcount = 0;
static unsigned long bfqchkcount = 0;
static int bfqchannels = 0;
int bfqids[10];
static char *bf_buf = NULL;
static size_t bf_bufsz = 0;

#define NO_COLOR (COL_COUNT)
static char *colnames[COL_COUNT+1];
int alertcolors, okcolors;
enum alertstate_t { A_OK, A_ALERT, A_UNDECIDED };

typedef struct ghostlist_t {
	char *name;
	char *sender;
	time_t tstamp, matchtime;
} ghostlist_t;
void *rbghosts;

typedef struct multisrclist_t {
	char *id;
	char *senders[2];
	time_t tstamp;
} multisrclist_t;
void *rbmultisrc;

enum ghosthandling_t ghosthandling = GH_LOG;

char *checkpointfn = NULL;
FILE *dbgfd = NULL;
char *dbghost = NULL;
time_t boottimer = 0;
int  hostcount = 0;
char *ackinfologfn = NULL;
FILE *ackinfologfd = NULL;

char *defaultreddelay = NULL, *defaultyellowdelay = NULL;

typedef struct xymond_statistics_t {
	char *cmd;
	unsigned long netcount, bfqcount;
} xymond_statistics_t;

xymond_statistics_t xymond_stats[] = {
	{ "extcombo", },
	{ "combodata", },
	{ "combo", },
	{ "modify", },
	{ "status", },
	{ "data", },
	{ "clientsubmit", },
	{ "clientconfig", },
	{ "summary", },
	{ "notes", },
	{ "usermsg", },
	{ "enable", },
	{ "disable", },
	{ "config", },
	{ "download", },
	{ "flush", },
	{ "reload", },
	{ "rotate", },
	{ "query", },
	{ "xymondlog", },
	{ "hobbitdlog", },
	{ "xymondxlog", },
	{ "hobbitdxlog", },
	{ "xymondboard", },
	{ "hobbitdboard", },
	{ "xymondxboard", },
	{ "hobbitdxboard", },
	{ "hostinfo", },
	{ "histsync", },
	{ "xymondack", },
	{ "hobbitdack", },
	{ "ack", },
	{ "ackinfo", },
	{ "drop", },
	{ "rename", },
	{ "dummy", },
	{ "ping", },
	{ "notify", },
	{ "schedule", },
	{ "client", },
	{ "clientlog", },
	{ "ghostlist", },
	{ "multisrclist", },
	{ "senderstats", },
	{ "Timeouts", },
	{ NULL, }
};

enum boardfield_t { F_NONE, F_IP, F_HOSTNAME, F_TESTNAME, F_MATCHEDTAG, F_COLOR, F_FLAGS, 
		    F_LASTCHANGE, F_LOGTIME, F_VALIDTIME, F_ACKTIME, F_DISABLETIME,
		    F_SENDER, F_COOKIE, F_LINE1,
		    F_ACKMSG, F_DISMSG, F_MSG, F_CLIENT, F_CLIENTTSTAMP,
		    F_ACKLIST,
		    F_HOSTINFO,
		    F_FLAPINFO,
		    F_STATS,
		    F_MODIFIERS,
		    F_LAST };

typedef struct boardfieldnames_t {
	char *name;
	enum boardfield_t id;
} boardfieldnames_t;
boardfieldnames_t boardfieldnames[] = {
	{ "ip", F_IP },
	{ "hostname", F_HOSTNAME },
	{ "matchedtag", F_MATCHEDTAG },
	{ "matchedtags", F_MATCHEDTAG },
	{ "testname", F_TESTNAME },
	{ "color", F_COLOR },
	{ "flags", F_FLAGS },
	{ "lastchange", F_LASTCHANGE },
	{ "logtime", F_LOGTIME },
	{ "validtime", F_VALIDTIME },
	{ "acktime", F_ACKTIME },
	{ "disabletime", F_DISABLETIME },
	{ "sender", F_SENDER },
	{ "cookie", F_COOKIE },
	{ "line1", F_LINE1 },
	{ "ackmsg", F_ACKMSG },
	{ "dismsg", F_DISMSG },
	{ "msg", F_MSG },
	{ "client", F_CLIENT },
	{ "clntstamp", F_CLIENTTSTAMP },
	{ "acklist", F_ACKLIST },
	{ "XMH_", F_HOSTINFO },
	{ "flapinfo", F_FLAPINFO },
	{ "stats", F_STATS },
	{ "modifiers", F_MODIFIERS },
	{ NULL, F_LAST },
};
typedef struct boardfields_t {
	enum boardfield_t field;
	enum xmh_item_t xmhfield; /* Only for field == F_HOSTINFO */
} boardfield_t;

enum filtertype_t { FILTER_XMH, FILTER_PAGEPATH, FILTER_TEST, FILTER_FIELD, FILTER_FIELDTIME, FILTER_TAG, FILTER_COLOR, FILTER_ACKLEVEL, FILTER_NOTDOWN, FILTER_DOWN };

/* Filtration comparison flags */
#define COMPARE_GT	(1 << 0)
#define COMPARE_GE	(1 << 1)
#define COMPARE_LT	(1 << 2)
#define COMPARE_LE	(1 << 3)
#define COMPARE_EQ	(1 << 4)
#define COMPARE_NE	(1 << 5)
#define COMPARE_INTVL	(1 << 29)

typedef struct hostfilter_rec_t {
	enum filtertype_t filtertype;
	pcre *wantedptn; int wantedvalue;
	struct hostfilter_rec_t *next;
	enum xmh_item_t field;	/* Only for filtertype == FILTER_XMH */
	enum boardfield_t boardfield;	/* Only for filtertype == FILTER_FIELD(TIME) */
	unsigned int flags;	/* Private filter flags */
	xtreePos_t handle;
} hostfilter_rec_t;


#define CHANNELTERMINATOR "\n@@\n"
#define CHANNELTERMINATORLEN 4		/* Length NOT including the terminating '\0' */

/* Statistics counters */
unsigned long msgs_total = 0;
unsigned long msgs_total_last = 0;
time_t last_stats_time = 0;

/* List of scheduled (future) tasks */
typedef struct scheduletask_t {
	int id;
	time_t executiontime;
	char *command;
	char *sender;
	struct scheduletask_t *next;
} scheduletask_t;
scheduletask_t *schedulehead = NULL;
int nextschedid = 1;

void update_statistics(char *cmd, int viabfq)
{
	int i;

	dbgprintf("-> update_statistics\n");

	if (!cmd) {
		dbgprintf("No command for update_statistics\n");
		return;
	}

	msgs_total++;

	i = 0;
	while (xymond_stats[i].cmd && strncmp(xymond_stats[i].cmd, cmd, strlen(xymond_stats[i].cmd))) { i++; }
	if (viabfq)
		xymond_stats[i].bfqcount++;
	else {
		xymond_stats[i].netcount++;
	}

	if (!xymond_stats[i].cmd) {
		char *eoln = strchr(cmd, '\n');
		if (eoln) *eoln = '\0';
		errprintf("Bogus message %s\n", cmd);
		if (eoln) *eoln = '\n';
	}

	dbgprintf("<- update_statistics\n");
}

char *generate_stats(void)
{
	static strbuffer_t *statsbuf = NULL;
	time_t now = getcurrenttime(NULL);
	time_t nowtimer = gettimer();
	int i, clients;
	char bootuptxt[40];
	char uptimetxt[40];
	xtreePos_t ghandle;
	time_t uptime = (nowtimer - boottimer);
	time_t boottstamp = (now - uptime);
	char msgline[2048];
	unsigned long net_total = 0, bfq_total = 0;

	dbgprintf("-> generate_stats\n");

	if (statsbuf == NULL) {
		statsbuf = newstrbuffer(8192);
	}
	else {
		clearstrbuffer(statsbuf);
	}

	strftime(bootuptxt, sizeof(bootuptxt), "%d-%b-%Y %T", localtime(&boottstamp));
	sprintf(uptimetxt, "%d days, %02d:%02d:%02d", 
		(int)(uptime / 86400), (int)(uptime % 86400)/3600, (int)(uptime % 3600)/60, (int)(uptime % 60));

	init_timestamp();

	sprintf(msgline, "status+11 %s.xymond %s %s - xymon daemon up: %s\nStatistics for Xymon daemon\nVersion: %s\nUp since %s (%s)\n\n",
		xgetenv("MACHINE"), colorname(errbuf ? COL_YELLOW : COL_GREEN), timestamp, uptimetxt, VERSION, bootuptxt, uptimetxt);
	addtobuffer(statsbuf, msgline);
	for (i = 0; (xymond_stats[i].cmd); i++) {
		net_total += xymond_stats[i].netcount;
		bfq_total += xymond_stats[i].bfqcount;
	}

	sprintf(msgline, "Incoming messages      : %10ld (%10ld net %10ld BFQ)\n", msgs_total, net_total, bfq_total);
	addtobuffer(statsbuf, msgline);
	for (i = 0; (xymond_stats[i].cmd); i++) {
		if ((xymond_stats[i].netcount + xymond_stats[i].bfqcount) == 0) continue;

		sprintf(msgline, "- %-20s : %10ld (%10ld net %10ld BFQ)\n", xymond_stats[i].cmd, 
			xymond_stats[i].netcount + xymond_stats[i].bfqcount,
			xymond_stats[i].netcount, xymond_stats[i].bfqcount);
		addtobuffer(statsbuf, msgline);
	}
	sprintf(msgline, "- %-20s : %10ld\n", "Bogus/Unknown ", xymond_stats[i].netcount);
	addtobuffer(statsbuf, msgline);

	if ((now > last_stats_time) && (last_stats_time > 0)) {
		sprintf(msgline, "Incoming messages/sec  : %10ld (average last %d seconds)\n", 
			((msgs_total - msgs_total_last) / (now - last_stats_time)), 
			(int)(now - last_stats_time));
		addtobuffer(statsbuf, msgline);
	}
	msgs_total_last = msgs_total;

	addtobuffer(statsbuf, "\n");
	clients = semctl(statuschn->semid, CLIENTCOUNT, GETVAL);
	sprintf(msgline, "status channel messages: %10ld (%d readers)\n", statuschn->msgcount, clients);
	addtobuffer(statsbuf, msgline);
	clients = semctl(stachgchn->semid, CLIENTCOUNT, GETVAL);
	sprintf(msgline, "stachg channel messages: %10ld (%d readers)\n", stachgchn->msgcount, clients);
	addtobuffer(statsbuf, msgline);
	clients = semctl(pagechn->semid, CLIENTCOUNT, GETVAL);
	sprintf(msgline, "page   channel messages: %10ld (%d readers)\n", pagechn->msgcount, clients);
	addtobuffer(statsbuf, msgline);
	clients = semctl(datachn->semid, CLIENTCOUNT, GETVAL);
	sprintf(msgline, "data   channel messages: %10ld (%d readers)\n", datachn->msgcount, clients);
	addtobuffer(statsbuf, msgline);
	clients = semctl(noteschn->semid, CLIENTCOUNT, GETVAL);
	sprintf(msgline, "notes  channel messages: %10ld (%d readers)\n", noteschn->msgcount, clients);
	addtobuffer(statsbuf, msgline);
	clients = semctl(enadischn->semid, CLIENTCOUNT, GETVAL);
	sprintf(msgline, "enadis channel messages: %10ld (%d readers)\n", enadischn->msgcount, clients);
	addtobuffer(statsbuf, msgline);
	clients = semctl(clientchn->semid, CLIENTCOUNT, GETVAL);
	sprintf(msgline, "client channel messages: %10ld (%d readers)\n", clientchn->msgcount, clients);
	addtobuffer(statsbuf, msgline);
	clients = semctl(clichgchn->semid, CLIENTCOUNT, GETVAL);
	sprintf(msgline, "clichg channel messages: %10ld (%d readers)\n", clichgchn->msgcount, clients);
	addtobuffer(statsbuf, msgline);
	clients = semctl(userchn->semid, CLIENTCOUNT, GETVAL);
	sprintf(msgline, "user   channel messages: %10ld (%d readers)\n", userchn->msgcount, clients);
	addtobuffer(statsbuf, msgline);

	ghandle = xtreeFirst(rbghosts);
	if (ghandle != xtreeEnd(rbghosts)) addtobuffer(statsbuf, "\n\nGhost reports (last 10m):\n");
	for (; (ghandle != xtreeEnd(rbghosts)); ghandle = xtreeNext(rbghosts, ghandle)) {
		ghostlist_t *gwalk = (ghostlist_t *)xtreeData(rbghosts, ghandle);

		/* Skip records older than 10 minutes */
		if (gwalk->tstamp < (nowtimer - 600)) continue;
		sprintf(msgline, "  %-15s reported host %s\n", gwalk->sender, htmlquoted(gwalk->name));
		addtobuffer(statsbuf, msgline);
	}

	ghandle = xtreeFirst(rbmultisrc);
	if (ghandle != xtreeEnd(rbmultisrc)) addtobuffer(statsbuf, "\n\nMulti-source statuses (last 10m):\n");
	for (; (ghandle != xtreeEnd(rbmultisrc)); ghandle = xtreeNext(rbmultisrc, ghandle)) {
		multisrclist_t *mwalk = (multisrclist_t *)xtreeData(rbmultisrc, ghandle);

		/* Skip records older than 10 minutes */
		if (mwalk->tstamp < (nowtimer - 600)) continue;
		sprintf(msgline, "  %-25s reported by %s and %s\n", mwalk->id, mwalk->senders[0], mwalk->senders[1]);
		addtobuffer(statsbuf, msgline);
	}

	if (errbuf) {
		addtobuffer(statsbuf, "\n\nLatest error messages:\n");
		addtobuffer(statsbuf, prehtmlquoted(errbuf));
		addtobuffer(statsbuf, "\n");
	}

	dbgprintf("<- generate_stats\n");

	return STRBUF(statsbuf);
}


char *totalclientmsg(clientmsg_list_t *msglist)
{
	static strbuffer_t *result = NULL;
	clientmsg_list_t *mwalk;
	time_t nowtimer = gettimer();

	if (!result) result = newstrbuffer(10240);

	clearstrbuffer(result);
	for (mwalk = msglist; (mwalk); mwalk = mwalk->next) {
		if ((mwalk->timestamp + MAX_SUBCLIENT_LIFETIME) < nowtimer) continue; /* Expired data */
		addtobuffer_many(result, "\n[collector:", mwalk->collectorid, "]\n", mwalk->msg, NULL);
	}

	return STRBUF(result);
}

enum alertstate_t decide_alertstate(int color)
{
	if ((okcolors & (1 << color)) != 0) return A_OK;
	else if ((alertcolors & (1 << color)) != 0) return A_ALERT;
	else return A_UNDECIDED;
}


xymond_hostlist_t *create_hostlist_t(char *hostname, char *ip)
{
	xymond_hostlist_t *hitem;

	hitem = (xymond_hostlist_t *) calloc(1, sizeof(xymond_hostlist_t));
	dbgprintf(" -- create_hostlist_t for %s (%s)\n", hostname, ip);
	hitem->hostname = strdup(hostname);
	hitem->ip = strdup(ip);
	if (strcmp(hostname, "summary") == 0) hitem->hosttype = H_SUMMARY;
	else hitem->hosttype = H_NORMAL;
	xtreeAdd(rbhosts, hitem->hostname, hitem);

	return hitem;
}

testinfo_t *create_testinfo(char *name)
{
	testinfo_t *newrec;

	newrec = (testinfo_t *)calloc(1, sizeof(testinfo_t));
	newrec->name = strdup(name);
	dbgprintf(" -- create_testinfo for %s\n", name);
	newrec->clientsave = clientsavedisk;
	xtreeAdd(rbtests, newrec->name, newrec);

	return newrec;
}

void posttochannel(xymond_channel_t *channel, char *channelmarker, 
		   char *msg, char *sender, char *hostname, xymond_log_t *log, char *readymsg)
{
	struct sembuf s;
	int clients;
	int n;
	struct timeval tstamp;
	struct timezone tz;
	int semerr = 0;
	unsigned int bufsz = channel->maxsize;	/* only master ever posts */
	size_t bufmax = bufsz - CHANNELTERMINATORLEN - 1;	/* Terminating \0 */
	size_t originalsize, byteswritten = 0;
	void *hi;
	char *pagepath, *classname, *osname;
	time_t timeroffset = (getcurrenttime(NULL) - gettimer());

	dbgprintf("-> posttochannel\n");

	/* First see how many users are on this channel */
	clients = semctl(channel->semid, CLIENTCOUNT, GETVAL);
	if (clients == 0) {
		dbgprintf("Dropping %s message - no readers on %s channel\n", channelmarker, channelnames[channel->channelid]);
		return;
	}

	/* 
	 * Wait for BOARDBUSY to go low.
	 * We need a loop here, because if we catch a signal
	 * while waiting on the semaphore, then we need to
	 * re-start the semaphore wait. Otherwise we may
	 * end up with semaphores that are out of sync
	 * (GOCLIENT goes up while a worker waits for it 
	 *  to go to 0).
	 */
	gotalarm = 0; alarm(5);
	do {
		s.sem_num = BOARDBUSY; s.sem_op = 0; s.sem_flg = 0;
		n = semop(channel->semid, &s, 1);
		if (n == -1) {
			semerr = errno;
			if (semerr != EINTR) errprintf("semop failed on %s, %s\n", channelmarker, strerror(errno));
		}
	} while ((n == -1) && (semerr == EINTR) && running && !gotalarm);
	alarm(0);
	if (!running) return;

	/* Check if the alarm fired */
	if (gotalarm) {
		errprintf("BOARDBUSY locked at %d on '%s', GETNCNT is %d, GETPID is %d, %d clients\n",
			  semctl(channel->semid, BOARDBUSY, GETVAL), channelnames[channel->channelid], 
			  semctl(channel->semid, BOARDBUSY, GETNCNT),
			  semctl(channel->semid, BOARDBUSY, GETPID),
			  semctl(channel->semid, CLIENTCOUNT, GETVAL));
		return;
	}

	/* Check if we failed to grab the semaphore */
	if (n == -1) {
		errprintf("Dropping %s message due to semaphore error\n", channelmarker);
		return;
	}

	/* All clear, post the message */
	if (channel->seq == 999999) channel->seq = 0;
	channel->seq++;
	channel->msgcount++;
	gettimeofday(&tstamp, &tz);
	if (readymsg) {
		byteswritten = snprintf(channel->channelbuf, bufmax,
			    "@@%s#%u/%s|%d.%06d|%s|%s", 
			    channelmarker, channel->seq, 
			    (hostname ? hostname : "*"), 
			    (int) tstamp.tv_sec, (int) tstamp.tv_usec,
			    sender, readymsg);
		if (byteswritten > bufmax) {
			char *p, *overmsg = readymsg;
			*(overmsg+100) = '\0';
			p = strchr(overmsg, '\n'); if (p) *p = '\0';
			errprintf("Oversize %s msg from %s truncated (n=%d, limit %d)\nFirst line: %s\n", 
				   channelmarker, sender, byteswritten, bufsz, overmsg);
			*(channel->channelbuf + bufmax) = '\0';
			byteswritten = bufmax;
		}
	}
	else {
		switch(channel->channelid) {
		  case C_STATUS:
			hi = hostinfo(hostname);
			pagepath = (hi ? xmh_item(hi, XMH_ALLPAGEPATHS) : "");
			classname = (hi ? xmh_item(hi, XMH_CLASS) : "");
			if (!classname) classname = "";

			byteswritten = snprintf(channel->channelbuf, bufmax,
				"@@%s#%u/%s|%d.%06d|%s|%s|%s|%s|%d|%s|%s|%s|%d|%d|%s|%d|%s|%d|%s|%s|%d|%s\n%s", 
				channelmarker, channel->seq, hostname, 		/*  0 */
				(int) tstamp.tv_sec, (int) tstamp.tv_usec,	/*  1 */
				sender, 					/*  2 */
				log->origin, 					/*  3 */
				hostname, 					/*  4 */
				log->test->name, 				/*  5 */
				(int) log->validtime, 				/*  6 */
				colnames[log->color], 				/*  7 */
				(log->testflags ? log->testflags : ""),		/*  8 */
				colnames[log->oldcolor], 			/*  9 */
				(int) log->lastchange[0], 			/* 10 */
				(int)log->acktime, nlencode(log->ackmsg),	/* 11+12 */
				(int)log->enabletime, nlencode(log->dismsg),	/* 13+14 */
				(int)(log->host->clientmsgtstamp + timeroffset), /* 15 */
				classname,					/* 16 */
				pagepath,					/* 17 */
				(int)log->flapping,				/* 18 */
				(log->modifiers ? log->modifierbuf : ""),	/* 19 */
				msg);						/* 20 */

			if (byteswritten > bufmax) {
				errprintf("Oversize status msg from %s for %s:%s truncated (n=%d, limit=%d)\n", 
					sender, hostname, log->test->name, byteswritten, bufsz);
				*(channel->channelbuf + bufmax) = '\0';
				byteswritten = bufmax;
			}
			break;

		  case C_STACHG:
			byteswritten = snprintf(channel->channelbuf, bufmax,
				"@@%s#%u/%s|%d.%06d|%s|%s|%s|%s|%d|%s|%s|%d|%d|%s|%d|%d|%s\n%s", 
				channelmarker, channel->seq, hostname, 		/*  0 */
				(int) tstamp.tv_sec, (int) tstamp.tv_usec,	/*  1 */
				sender,						/*  2 */ 
				log->origin,					/*  3 */ 
				hostname,					/*  4 */ 
				log->test->name,				/*  5 */ 
				(int) log->validtime,				/*  6 */ 
				colnames[log->color],				/*  7 */ 
				colnames[log->oldcolor],			/*  8 */ 
				(int) log->lastchange[0],			/*  9 */
				(int)log->enabletime, nlencode(log->dismsg),	/* 10+11 */
				log->downtimeactive,				/* 12 */
				(int) (log->host->clientmsgtstamp + timeroffset), /* 13 */
				(log->modifiers ? log->modifierbuf : ""),	/* 14 */
				msg);						/* 15 */

			if (byteswritten > bufmax) {
				errprintf("Oversize stachg msg from %s for %s:%s truncated (n=%d, limit=%d)\n", 
					sender, hostname, log->test->name, byteswritten, bufsz);
				*(channel->channelbuf + bufmax) = '\0';
				byteswritten = bufmax;
			}
			break;

		  case C_CLICHG:
			byteswritten = snprintf(channel->channelbuf, bufmax,
				"@@%s#%u/%s|%d.%06d|%s|%s|%d\n%s",
				channelmarker, channel->seq, hostname, (int) tstamp.tv_sec, (int) tstamp.tv_usec,
				sender, hostname, (int) (log->host->clientmsgtstamp + timeroffset), 
				totalclientmsg(log->host->clientmsgs));
			if (byteswritten > bufmax) {
				errprintf("Oversize clichg msg from %s for %s truncated (n=%d, limit=%d)\n", 
					sender, hostname, byteswritten, bufsz);
				*(channel->channelbuf + bufmax) = '\0';
				byteswritten = bufmax;
			}
			break;

		  case C_PAGE:
			if (strcmp(channelmarker, "ack") == 0) {
				byteswritten = snprintf(channel->channelbuf, bufmax,
					"@@%s#%u/%s|%d.%06d|%s|%s|%s|%s|%d\n%s", 
					channelmarker, channel->seq, hostname, (int) tstamp.tv_sec, (int) tstamp.tv_usec,
					sender, hostname, 
					log->test->name, log->host->ip,
					(int) log->acktime, msg);
			}
			else {
				hi = hostinfo(hostname);
				pagepath = (hi ? xmh_item(hi, XMH_ALLPAGEPATHS) : "");
				classname = (hi ? xmh_item(hi, XMH_CLASS) : "");
				osname = (hi ? xmh_item(hi, XMH_OS) : "");
				if (!classname) classname = "";
				if (!osname) osname = "";

				byteswritten = snprintf(channel->channelbuf, bufmax,
					"@@%s#%u/%s|%d.%06d|%s|%s|%s|%s|%d|%s|%s|%d|%s|%s|%s|%s|%s|%s\n%s", 
					channelmarker, channel->seq, hostname, (int) tstamp.tv_sec, (int) tstamp.tv_usec,
					sender, hostname, 
					log->test->name, log->host->ip, (int) log->validtime, 
					colnames[log->color], colnames[log->oldcolor], (int) log->lastchange[0],
					pagepath, 
					(log->cookie ? log->cookie : ""), 
					osname, classname, 
					(log->grouplist ? log->grouplist : ""),
					(log->modifiers ? log->modifierbuf : ""),
					msg
				);
			}
			if (byteswritten > bufmax) {
				errprintf("Oversize page/ack/notify msg from %s for %s:%s truncated (n=%d, limit=%d)\n", 
					sender, hostname, (log->test->name ? log->test->name : "<none>"), byteswritten, bufsz);
				*(channel->channelbuf + bufmax) = '\0';
				byteswritten = bufmax;
			}
			break;

		  case C_DATA:
		  case C_CLIENT:
			/* Data channel messages are pre-formatted so we never go here */
			break;

		  case C_NOTES:
		  case C_USER:
			byteswritten = snprintf(channel->channelbuf, bufmax,
				"@@%s#%u/%s|%d.%06d|%s|%s\n%s", 
				channelmarker, channel->seq, hostname, (int) tstamp.tv_sec, (int) tstamp.tv_usec,
				sender, hostname, msg);
			if (byteswritten > bufmax) {
				errprintf("Oversize %s msg from %s for %s truncated (n=%d, limit=%d)\n", 
					((channel->channelid == C_NOTES) ? "notes" : "user"), 
					sender, hostname, byteswritten, bufsz);
				*(channel->channelbuf + bufmax) = '\0';
				byteswritten = bufmax;
			}
			break;

		  case C_ENADIS:
			{
				char *dism = "";

				if (log->dismsg) dism = nlencode(log->dismsg);
				byteswritten = snprintf(channel->channelbuf, bufmax,
						"@@%s#%u/%s|%d.%06d|%s|%s|%s|%d|%s",
						channelmarker, channel->seq, hostname, (int) tstamp.tv_sec, (int)tstamp.tv_usec,
						sender, hostname, log->test->name, (int) log->enabletime, dism);
				if (byteswritten > bufmax) {
					errprintf("Oversize enadis msg from %s for %s:%s truncated (n=%d, limit=%d)\n", 
							sender, hostname, log->test->name, byteswritten, bufsz);
					*(channel->channelbuf + bufmax) = '\0';
					byteswritten = bufmax;
				}
			}
			break;

		  case C_FEEDBACK_QUEUE:
		  case C_LAST:
			break;
		}
	}

	/* Terminate the message */
		// We don't actually need to do this now since we're memcpy'ing directly over the end
		// *(channel->channelbuf + bufmax) = '\0';
		// byteswritten = bufmax;
	memcpy(channel->channelbuf+byteswritten, CHANNELTERMINATOR, CHANNELTERMINATORLEN);
	*(channel->channelbuf + byteswritten + CHANNELTERMINATORLEN) = '\0';

	/* Let the readers know it is there.  */
	clients = semctl(channel->semid, CLIENTCOUNT, GETVAL); /* Get it again, maybe changed since last check */
	dbgprintf("Posting message %u to %d readers\n", channel->seq, clients);
	/* Up BOARDBUSY */
	s.sem_num = BOARDBUSY; 
	s.sem_op = (clients - semctl(channel->semid, BOARDBUSY, GETVAL)); 
	if (s.sem_op <= 0) {
		errprintf("How did this happen? clients=%d, s.sem_op=%d\n", clients, s.sem_op);
		s.sem_op = clients;
	}
	s.sem_flg = 0;
	n = semop(channel->semid, &s, 1);

	/* Make sure GOCLIENT is 0 */
	n = semctl(channel->semid, GOCLIENT, GETVAL);
	if (n > 0) {
		errprintf("Oops ... GOCLIENT is high (%d)\n", n);
	}

	s.sem_num = GOCLIENT; s.sem_op = clients; s.sem_flg = 0; 		/* Up GOCLIENT */
	n = semop(channel->semid, &s, 1);

	dbgprintf("<- posttochannel\n");

	return;
}

void posttoall(char *msg)
{
	logprintf("Sending %s\n", msg);
	posttochannel(statuschn, msg, NULL, "xymond", NULL, NULL, "");
	posttochannel(stachgchn, msg, NULL, "xymond", NULL, NULL, "");
	posttochannel(pagechn, msg, NULL, "xymond", NULL, NULL, "");
	posttochannel(datachn, msg, NULL, "xymond", NULL, NULL, "");
	posttochannel(noteschn, msg, NULL, "xymond", NULL, NULL, "");
	posttochannel(enadischn, msg, NULL, "xymond", NULL, NULL, "");
	posttochannel(clientchn, msg, NULL, "xymond", NULL, NULL, "");
	posttochannel(clichgchn, msg, NULL, "xymond", NULL, NULL, "");
	posttochannel(userchn, msg, NULL, "xymond", NULL, NULL, "");
}


void *buildsenderlist(char *senderlist)
{
	void *result = xtreeNew(strcasecmp);
	char *tok, *tokr = NULL;

	tok = strtok_r(senderlist, ",", &tokr);
	while (tok) {
		xtreeAdd(result, strdup(tok), NULL);
		tok = strtok_r(NULL, ",", &tokr);
	}

	return result;
}


int oksender(void *oklist, void *target, char *sender, char *msgbuf)
{
	char *eoln = NULL;

	if (!oklist) return 1;	/* No access checks = access allowed */

	if (target) {
		/* OK for a host to report about itself */
		char *name = xmh_item(target, XMH_HOSTNAME);
		if (strcasecmp(name, sender) == 0) return 1; /* Sender and target match = access allowed */
		name = xmh_item(target, XMH_CLIENTALIAS);
		if (strcasecmp(name, sender) == 0) return 1; /* Sender and client alias match = access allowed */
	}

	/* Someone else is reporting on a host. Check access list */
	if (oklist && (xtreeFind(oklist, sender) != xtreeEnd(oklist))) return 1; /* In auth list = access allowed */

	/* Sorry, not allowed. Refuse and log the message */
	if (msgbuf) { eoln = strchr(msgbuf, '\n'); if (eoln) *eoln = '\0'; }
	errprintf("Refused message from %s: %s\n", sender, (msgbuf ? msgbuf : "<empty>"));
	if (msgbuf && eoln) *eoln = '\n';

	return 0;
}


char *log_ghost(char *hostname, char *sender, char *msg)
{
	xtreePos_t ghandle;
	ghostlist_t *gwalk;
	char *result = NULL;
	time_t nowtimer = gettimer();

	dbgprintf("-> log_ghost\n");

	/* If debugging, log the full request */
	if (dbgfd) {
		fprintf(dbgfd, "\n---- combo message from %s ----\n%s---- end message ----\n", sender, msg);
		fflush(dbgfd);
	}

	if ((hostname == NULL) || (sender == NULL)) return NULL;

	ghandle = xtreeFind(rbghosts, hostname);
	gwalk = (ghandle != xtreeEnd(rbghosts)) ? (ghostlist_t *)xtreeData(rbghosts, ghandle) : NULL;

	/* Disallow weird test names - Note: a future version may restrict these to valid hostname + DNS labels and not preserve case */
	if (!gwalk && (strlen(hostname) != strspn(hostname, "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:,._-")) ) {
		errprintf("Bogus message from %s: Invalid new hostname '%s'\n", sender, hostname);
		return NULL;
	}

	if ((gwalk == NULL) || ((gwalk->matchtime + 600) < nowtimer)) {
		int found = 0;

		if (ghosthandling == GH_MATCH) {
			/* See if we can find this host just by ignoring domains */
			char *hostnodom, *p;
			void *hrec;

			hostnodom = strdup(hostname);
			p = strchr(hostnodom, '.'); if (p) *p = '\0';
			for (hrec = first_host(); (hrec && !found); hrec = next_host(hrec, 0)) {
				char *candname;
			
				candname = xmh_item(hrec, XMH_HOSTNAME);
				p = strchr(candname, '.'); if (p) *p = '\0';
				found = (strcasecmp(hostnodom, candname) == 0);
				if (p) *p = '.';
	
				if (found) {
					result = candname;
					xmh_set_item(hrec, XMH_CLIENTALIAS, hostname);
					errprintf("Matched ghost '%s' to host '%s'\n", hostname, result);
				}
			}
		}

		if (!found) {
			if (gwalk == NULL) {
				gwalk = (ghostlist_t *)calloc(1, sizeof(ghostlist_t));
				gwalk->name = strdup(hostname);
				gwalk->sender = strdup(sender);
				gwalk->tstamp = gwalk->matchtime = nowtimer;
				xtreeAdd(rbghosts, gwalk->name, gwalk);
			}
			else {
				if (gwalk->sender) xfree(gwalk->sender);
				gwalk->sender = strdup(sender);
				gwalk->tstamp = gwalk->matchtime = nowtimer;
			}
		}
	}
	else {
		if (gwalk->sender) xfree(gwalk->sender);
		gwalk->sender = strdup(sender);
		gwalk->tstamp = nowtimer;
	}

	dbgprintf("<- log_ghost\n");

	return result;
}

void log_multisrc(xymond_log_t *log, char *newsender)
{
	xtreePos_t ghandle;
	multisrclist_t *gwalk;
	char id[1024];

	dbgprintf("-> log_multisrc\n");

	snprintf(id, sizeof(id), "%s:%s", log->host->hostname, log->test->name);
	ghandle = xtreeFind(rbmultisrc, id);
	if (ghandle == xtreeEnd(rbmultisrc)) {
		gwalk = (multisrclist_t *)calloc(1, sizeof(multisrclist_t));
		gwalk->id = strdup(id);
		gwalk->senders[0] = strdup(log->sender);
		gwalk->senders[1] = strdup(newsender);
		gwalk->tstamp = gettimer();
		xtreeAdd(rbmultisrc, gwalk->id, gwalk);
	}
	else {
		gwalk = (multisrclist_t *)xtreeData(rbmultisrc, ghandle);
		xfree(gwalk->senders[0]); gwalk->senders[0] = strdup(log->sender);
		xfree(gwalk->senders[1]); gwalk->senders[1] = strdup(newsender);
		gwalk->tstamp = gettimer();
	}

	dbgprintf("<- log_multisrc\n");
}

xymond_log_t *find_log(hostfilter_rec_t *filter, xymond_hostlist_t **host)
{
	hostfilter_rec_t *fwalk;
	xymond_hostlist_t *hrec = NULL;
	testinfo_t *trec = NULL;
	xymond_log_t *lwalk;

	*host = NULL;
	if (!filter) return NULL;

	for (fwalk = filter; (fwalk); fwalk = fwalk->next) {
		switch(fwalk->filtertype) {
		  case FILTER_XMH:
			if ((fwalk->field == XMH_HOSTNAME) && (fwalk->handle != xtreeEnd(rbhosts))) *host = hrec = xtreeData(rbhosts, fwalk->handle);
			break;

		  case FILTER_TEST:
			if (fwalk->handle != xtreeEnd(rbtests)) trec = xtreeData(rbtests, fwalk->handle);
			break;

		  default:
			break;
		}
	}

	if (!hrec || !trec) return NULL;

	for (lwalk = hrec->logs; (lwalk && (lwalk->test != trec)); lwalk = lwalk->next);
	return lwalk;
}

char *check_downtime(char *hostname, char *testname)
{
	void *hinfo = hostinfo(hostname);
	char *dtag;
	char *holkey;

	if (hinfo == NULL) return NULL;

	dtag = xmh_item(hinfo, XMH_DOWNTIME);
	holkey = xmh_item(hinfo, XMH_HOLIDAYS);
	if (dtag && *dtag) {
		static char *downtag = NULL;
		static unsigned char *cause = NULL;
		static int causelen = 0;
		char *s1, *s2, *s3, *s4, *s5, *p;
		char timetxt[30];

		if (downtag) xfree(downtag);
		if (cause) xfree(cause);

		p = downtag = strdup(dtag);
		do {
			/* Its either DAYS:START:END or SERVICE:DAYS:START:END:CAUSE */

			s1 = p; p += strcspn(p, ":"); if (*p != '\0') { *p = '\0'; p++; }
			s2 = p; p += strcspn(p, ":"); if (*p != '\0') { *p = '\0'; p++; }
			s3 = p; p += strcspn(p, ":;,"); 
			if ((*p == ',') || (*p == ';') || (*p == '\0')) { 
				if (*p != '\0') { *p = '\0'; p++; }
				snprintf(timetxt, sizeof(timetxt), "%s:%s:%s", s1, s2, s3);
				cause = strdup("Planned downtime");
				s1 = "*";
			}
			else if (*p == ':') {
				*p = '\0'; p++; 
				s4 = p; p += strcspn(p, ":"); if (*p != '\0') { *p = '\0'; p++; }
				s5 = p; p += strcspn(p, ",;"); if (*p != '\0') { *p = '\0'; p++; }
				snprintf(timetxt, sizeof(timetxt), "%s:%s:%s", s2, s3, s4);
				getescapestring(s5, &cause, &causelen);
			}

			if (within_sla(holkey, timetxt, 0)) {
				char *onesvc, *buf;

				if (strcmp(s1, "*") == 0) return cause;

				onesvc = strtok_r(s1, ",", &buf);
				while (onesvc) {
					if (strcmp(onesvc, testname) == 0) return cause;
					onesvc = strtok_r(NULL, ",", &buf);
				}
			}

			/* If we didn't use the "cause" we just created, it must be freed */
			if (cause) xfree(cause);
		} while (*p);
	}

	return NULL;
}


int accept_test(void *hrec, char *testname)
{
	char *accept = xmh_item(hrec, XMH_ACCEPT_ONLY);
	char *p, *endp;

	if (!accept || !testname || !(*testname)) return 1;

	p = strstr(accept, testname);
	if (p) {
	    int testlength = strlen(testname);

	    while (p) {
		/*
		 * p points to where the testname is in the accept string. Must check that it
		 * points to a full word.
		 *
		 * Check :
		 * - if p points at (beginning of accept string, or there is a ',' right before p) AND
		 * - (p+strlen(testname) hits end of accept string, or it hits a ',')
		 */
		endp = p + testlength;
		if (((*endp == '\0') || (*endp == ',')) && ((p == accept) || (*(p-1) == ','))) return 1;
		/* no match, keep looking */
		p = strstr(endp, testname);
	    }
	}

	return 0;
}


void get_hts(char *msg, char *sender, char *origin,
	     xymond_hostlist_t **host, testinfo_t **test, char **grouplist, xymond_log_t **log, 
	     int *color, char **downcause, int *alltests, int createhost, int createlog)
{
	/*
	 * This routine takes care of finding existing status log records, or
	 * (if they don't exist) creating new ones for an incoming status.
	 *
	 * "msg" contains an incoming message. First list is of the form "KEYWORD host,domain.test COLOR"
	 */

	char *firstline, *p;
	char *hosttest, *hostname, *testname, *colstr, *grp;
	char *hostip = NULL;
	xtreePos_t hosthandle, testhandle, originhandle;
	xymond_hostlist_t *hwalk = NULL;
	testinfo_t *twalk = NULL;
	int is_summary = 0;
	char *owalk = NULL;
	xymond_log_t *lwalk = NULL;

	dbgprintf("-> get_hts\n");

	*host = NULL;
	*test = NULL;
	*log = NULL;
	*color = -1;
	if (grouplist) *grouplist = NULL;
	if (downcause) *downcause = NULL;
	if (alltests) *alltests = 0;

	hosttest = hostname = testname = colstr = grp = NULL;
	p = strchr(msg, '\n');
	if (p == NULL) {
		firstline = strdup(msg);
	}
	else {
		*p = '\0';
		firstline = strdup(msg); 
		*p = '\n';
	}

	p = strtok(firstline, " \t"); /* Keyword ... */
	if (p) {
		/* There might be a group-list */
		grp = strstr(p, "/group:");
		if (grp) grp += 7;
	}
	if (p) hosttest = strtok(NULL, " \t"); /* ... HOST.TEST combo ... */
	if (hosttest == NULL) goto done;
	colstr = strtok(NULL, " \t"); /* ... and the color (if any) */
	if (colstr) {
		*color = parse_color(colstr);
		/* Don't create log-entries if we get a bad color spec or if it's the 'client ' pseudo-color */
		if (*color == -1 || *color == COL_CLIENT) createlog = 0;
	}
	else createlog = 0;

	if (strncmp(msg, "summary", 7) == 0) {
		/* Summary messages are handled specially */
		hostname = hosttest;	/* This will always be "summary" */
		testname = strchr(hosttest, '.');
		if (testname) { *testname = '\0'; testname++; }
		is_summary = 1;
	}
	else {
		char *knownname;

		hostname = hosttest;
		testname = strrchr(hosttest, '.');
		if (testname) { *testname = '\0'; testname++; }
		uncommafy(hostname);	/* For BB agent compatibility */

		knownname = knownhost(hostname, &hostip, ghosthandling);
		if (knownname == NULL) {
			knownname = log_ghost(hostname, sender, msg);
			if (knownname == NULL) goto done;
		}
		hostname = knownname;
	}

	hosthandle = xtreeFind(rbhosts, hostname);
	if (hosthandle == xtreeEnd(rbhosts)) hwalk = NULL;
	else hwalk = xtreeData(rbhosts, hosthandle);

	if (createhost && (hosthandle == xtreeEnd(rbhosts))) {
		hwalk = create_hostlist_t(hostname, hostip);
		hostcount++;
	}

	if (testname && *testname) {
		if (alltests && (*testname == '*')) {
			*alltests = 1;
			return;
		}

		testhandle = xtreeFind(rbtests, testname);
		if (testhandle != xtreeEnd(rbtests)) twalk = xtreeData(rbtests, testhandle);

		/* Disallow new, weird test names - Note: a future version of Xymon may restrict this further and not preserve case */
		/* NB: Summary messages are stored under the pseudo-host 'summary' but have invalid test names unpacked by xymongen */
		if (!twalk && !is_summary && (strlen(testname) != strspn(testname, "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:\\/_-")) ) {
			errprintf("Bogus message from %s: Invalid new testname '%s'\n", sender, testname);
			return;
		}
		if (createlog && (twalk == NULL)) twalk = create_testinfo(testname);
	}
	else {
		if (createlog) errprintf("Bogus message from %s: No testname '%s'\n", sender, msg);
	}

	if (origin) {
		originhandle = xtreeFind(rborigins, origin);
		if (originhandle != xtreeEnd(rborigins)) owalk = xtreeData(rborigins, originhandle);
		if (createlog && (owalk == NULL)) {
			owalk = strdup(origin);
			xtreeAdd(rborigins, owalk, owalk);
		}
	}

	if (hwalk && twalk && owalk) {
		for (lwalk = hwalk->logs; (lwalk && ((lwalk->test != twalk) || (lwalk->origin != owalk))); lwalk = lwalk->next);
		if (createlog && (lwalk == NULL)) {
			dbgprintf(" -- get_hts creating new log record: host %s, test %s, color %s, group %s, origin %s\n", hostname, testname, colstr, grp, origin);
			lwalk = (xymond_log_t *)calloc(1, sizeof(xymond_log_t));
			lwalk->lastchange = (time_t *)calloc((flapcount > 0) ? flapcount : 1, sizeof(time_t));
			lwalk->lastchange[0] = getcurrenttime(NULL);
			lwalk->color = lwalk->oldcolor = NO_COLOR;
			lwalk->host = hwalk;
			lwalk->test = twalk;
			lwalk->modifiers = NULL;
			lwalk->modifierbuf = NULL;
			lwalk->origin = owalk;
			lwalk->next = hwalk->logs;
			hwalk->logs = lwalk;
			if (strcmp(testname, xgetenv("PINGCOLUMN")) == 0) hwalk->pinglog = lwalk;
		}
	}

done:
	if (colstr) {
		if ((*color == COL_RED) || (*color == COL_YELLOW) || (*color == COL_PURPLE)) {
			char *cause;

			cause = check_downtime(hostname, testname);
			if (lwalk) lwalk->downtimeactive = (cause != NULL);
			if (cause) *color = COL_BLUE;
			if (downcause) *downcause = cause;
		}
		else {
			if (lwalk) lwalk->downtimeactive = 0;
		}
	}

	if (grouplist && grp) *grouplist = strdup(grp);

	xfree(firstline);

	*host = hwalk;
	*test = twalk;
	*log = lwalk;

	dbgprintf("<- get_hts\n");
}


void clear_cookie(xymond_log_t *log)
{
	if (!log->cookie) return;

	xtreeDelete(rbcookies, log->cookie);
#ifndef HAVE_BINARY_TREE
	xfree(log->cookie);
#endif
	log->cookie = NULL; log->cookieexpires = 0;
}


xymond_log_t *find_cookie(char *cookie)
{
	/*
	 * Find a cookie we have issued.
	 */
	xymond_log_t *result = NULL;
	xtreePos_t cookiehandle;

	dbgprintf("-> find_cookie\n");

	cookiehandle = xtreeFind(rbcookies, cookie);
	if (cookiehandle != xtreeEnd(rbcookies)) {
		dbgprintf(" - found a cookie matching %s\n", cookie);
		result = xtreeData(rbcookies, cookiehandle);
		if (result == NULL) errprintf("BUG: find_cookie given cookie %s that returned a NULL result from xtreeData\n", cookie);
		else if (result->cookieexpires <= getcurrenttime(NULL)) {
			dbgprintf(" - cookie %s expired at %d, so we didn't find a cookie\n", cookie, result->cookieexpires);
			clear_cookie(result);
			result = NULL;
		}
	}

	dbgprintf("<- find_cookie\n");

	return result;
}


static int changedelay(void *hinfo, int newcolor, char *testname, int currcolor)
{
	char *key, *tok, *dstr = NULL;
	int keylen, result = 0;

	/* Ignore any delays when we start with a purple status */
	if (currcolor == COL_PURPLE) return 0;

	switch (newcolor) {
	  case COL_RED: dstr = xmh_item(hinfo, XMH_DELAYRED); if (!dstr) dstr = defaultreddelay; break;
	  case COL_YELLOW: dstr = xmh_item(hinfo, XMH_DELAYYELLOW); if (!dstr) dstr = defaultyellowdelay; break;
	  default: break;
	}

	if (!dstr) return result;

	/* Check "DELAYRED=cpu:10,disk:30,ssh:20" - number is in minutes */
	keylen = strlen(testname) + 1;
	key = (char *)malloc(keylen + 1);
	sprintf(key, "%s:", testname);

	dstr = strdup(dstr);
	tok = strtok(dstr, ",");
	while (tok && (strncmp(key, tok, keylen) != 0)) tok = strtok(NULL, ",");
	if (tok) result = 60*atoi(tok+keylen); /* Convert to seconds */

	xfree(key);
	xfree(dstr);

	return result;
}

static int isset_noflap(void *hinfo, char *testname, char *hostname)
{
	char *tok, *dstr;
	int keylen;

	dstr = xmh_item(hinfo, XMH_NOFLAP);
	if (!dstr) return 0; /* no 'noflap' set */

	/* Check bare noflap (disable for host) vs "noflap=test1,test2" */
	/* A bare noflap will be set equal to the key itself (usually NOFLAP) like a flag */
	if (strcmp(dstr, "NOFLAP") == 0) return 1;

	/* if not 'NOFLAP', we should receive "=test1,test2". Skip the = */
	if (*dstr == '=') dstr++;
	
	keylen = strlen(testname);
	tok = strtok(dstr, ",");
	while (tok && (strncmp(testname, tok, keylen) != 0)) tok = strtok(NULL, ",");
	if (!tok) return 0; /* specifies noflap, but this test is not in the list */

	/* do not use flapping for the test */
	dbgprintf("Ignoring flapping for %s:%s due to noflap set.\n", hostname, testname);

	return 1;
}

void handle_status(unsigned char *msg, char *sender, char *hostname, char *testname, char *grouplist, 
		   xymond_log_t *log, int newcolor, char *downcause, int modifyonly)
{
	int validity = defaultvalidity;
	time_t now = getcurrenttime(NULL);
	int msglen, issummary, modifychanged = 0;
	enum alertstate_t oldalertstatus, newalertstatus;
	int delayval = 0;
	void *hinfo = hostinfo(hostname);

	dbgprintf("->handle_status\n");

	if (msg == NULL) {
		errprintf("handle_status got a NULL message for %s.%s, sender %s, color %s\n", 
			  textornull(hostname), textornull(testname), textornull(sender), colorname(newcolor));
		return;
	}

	msglen = strlen(msg);
	if (msglen == 0) {
		errprintf("Bogus status message for %s.%s contains no data: Sent from %s\n", 
			  textornull(hostname), textornull(testname), textornull(sender));
		return;
	}
	if (msg_data(msg, 0) == (char *)msg) {
		errprintf("Bogus status message: msg_data finds no host.test. Sent from: '%s', data:'%s'\n",
			  sender, msg);
		return;
	}

	/* XXX: TODO: Needs revisiting; get_hts() has already run, so
	 * 	we're leaving test record debris around */
	/* Check if disallowed, but let internally-generated messages through */
	/* Otherwise existing tests never go purple */
	// if ((strcmp(sender, "xymond") != 0) && !accept_test(hinfo, testname)) {
	//	dbgprintf("Rejected status message for %s.%s sent from %s\n", 
	//		  textornull(hostname), textornull(testname), textornull(sender));
	//	return;
	// }

	issummary = (log->host->hosttype == H_SUMMARY);

	if (strncmp(msg, "status+", 7) == 0) {
		validity = durationvalue(msg+7);
	}

	if (log->modifiers) {
		/*
		 * Modifiers are present, but first expire any stale ones and
		 * ensure what's left aren't past their "valid" count.
		 * (Decrementing IFF this is an Original Status Message) 
		 */
		int mupcolor = COL_GREEN;	/* least useful modifyup */
		int mdowncolor = COL_COUNT;	/* least useful modifydown */
		static strbuffer_t *modifierbuf;
		modifier_t *mwalk;
		modifier_t *mlast;
		int mcolor = -1;

		if (modifierbuf == NULL) modifierbuf = newstrbuffer(1024);

		/* 
		 * If handle_modify called us (this is a modify message, not a
		 * status message), it will set this flag. If this was a new 'cause'
		 * it will set this to 2. We track this so we can consider a 
		 * status as "changed" even if the color eventually stays the same.
		 */
		modifychanged = (modifyonly == 2);

		mlast = NULL;
		mwalk = log->modifiers;
		while (mwalk) {
			dbgprintf(" -- modifier found; source=%s, type=%d, validnum=%d, expires=%d, cause=%s",
			 mwalk->source, (int)mwalk->type, mwalk->valid, mwalk->validtime, mwalk->cause);

			/* Decrement validness count, but only if this is an Original Status Message */
			if (!modifyonly) mwalk->valid--;

			/* Note: !mwalk->valid is safe
			 * A modify message only being judged on timestamp gets
			 * a negative valid number to begin with
			 */
			if (!mwalk->valid || (mwalk->validtime && (mwalk->validtime < now))) {
				modifier_t *zombie;

				/* Modifier no longer valid */
				dbgprintf(" -- expiring this modifier: %s\n", mwalk->source);
				zombie = mwalk;
				if (zombie->source) xfree(zombie->source);
				if (zombie->cause) xfree(zombie->cause);

				/* Remove this modifier from the list. Make sure log->modifiers is updated */
				if (mwalk == log->modifiers) log->modifiers = mwalk->next;
				/* ... link the previous entry to the next, since we're about to free the current record */
				if (mlast) mlast->next = mwalk->next;
				mwalk = mwalk->next;
				xfree(zombie);

				modifychanged = 1; /* Something was removed */
				continue; /* walk to next modifer */
			}

			/* append valid cause for later nlencoding */
			addtobuffer(modifierbuf, mwalk->cause);

			/* Track three separate modifer colors overall	 */
			/* best "modifydown", worst "modifyup", worst regular modify */

			if      ((mwalk->type == MODIFY_DOWN) && (mwalk->color < mdowncolor)) mdowncolor = mwalk->color;
			else if ((mwalk->type == MODIFY_UP)   && (mwalk->color > mupcolor)) mupcolor = mwalk->color;
			else if ((mwalk->type == MODIFY_NORM) && (mwalk->color > mcolor)) mcolor = mwalk->color;
			mlast = mwalk;
			mwalk = mwalk->next;
		}
		if (STRBUFLEN(modifierbuf) || modifychanged) {		/* Might have removed our last one */
			if (log->modifierbuf) xfree(log->modifierbuf);
			log->modifierbuf = strdup(nlencode(STRBUF(modifierbuf)));
			clearstrbuffer(modifierbuf);
		}

		dbgprintf(" -- final calculation: newcolor=%d, mcolor=%d, mdowncolor=%d, mupcolor=%d\n", newcolor, mcolor, mdowncolor, mupcolor);
		/* If there was an active modifier, this overrides the current "newcolor" status value */
		if (mcolor != -1) newcolor = mcolor;

		/* ... then decrease by any MODIFY_DOWN  */
		if (mdowncolor < newcolor) newcolor = mdowncolor;

		/* ... then increase by any MODIFY_UP */
		if (mupcolor > newcolor) newcolor = mupcolor;

		dbgprintf(" -- overridden via modify to newcolor=%d\n", newcolor);
	}

	/*
	 * Flap check. 
	 *
	 * We check if more than flapcount changes have occurred 
	 * within "flapthreshold" seconds. If yes, and the newcolor 
	 * is less serious than the old color, then we ignore the
	 * color change and keep the status at the more serious level.
	 */
	if (modifyonly || issummary) {
		/* Nothing */
	}
	else if ((flapcount > 0) && ((now - log->lastchange[flapcount-1]) < flapthreshold) && (!isset_noflap(hinfo, testname, hostname))) {
		if (!log->flapping) {
			logprintf("Flapping detected for %s:%s - %d changes in %d seconds\n",
				  hostname, testname, flapcount, (now - log->lastchange[flapcount-1]));
			log->flapping = 1;
			log->oldflapcolor = log->color;
			log->currflapcolor = newcolor;
		}
		else {
			log->oldflapcolor = log->currflapcolor;
			log->currflapcolor = newcolor;
		}

		/* Make sure we maintain the most critical level reported by the flapping unit */
		if (newcolor < log->color) newcolor = log->color;

		/* 
		 * If the status is actually changing, but we've detected it's a
		 * flap and therefore suppress atatus change events, then we must
		 * update the lastchange-times here because it won't be done in
		 * the status-change handler.
		 */
		if ((log->oldflapcolor != log->currflapcolor) && (newcolor == log->color)) {
			int i;
			for (i=flapcount-1; (i > 0); i--)
				log->lastchange[i] = log->lastchange[i-1];
			log->lastchange[0] = now;
		}
	}
	else {
		log->flapping = 0;
	}

	if (log->enabletime == DISABLED_UNTIL_OK) {
		/* The test is disabled until we get an OK status */
		if ((newcolor != COL_BLUE) && (decide_alertstate(newcolor) == A_OK)) {
			/* It's OK now - clear the disable status */
			log->enabletime = 0;
			if (log->dismsg) { xfree(log->dismsg); log->dismsg = NULL; }
			posttochannel(enadischn, channelnames[C_ENADIS], msg, sender, log->host->hostname, log, NULL);
		}
		else {
			/* Still not OK - keep it BLUE */
			newcolor = COL_BLUE;
		}
	}
	else if (log->enabletime > now) {
		/* The test is currently disabled. */
		newcolor = COL_BLUE;
	}
	else if (log->enabletime) {
		/* A disable has expired. Clear the timestamp and the message buffer */
		log->enabletime = 0;
		if (log->dismsg) { xfree(log->dismsg); log->dismsg = NULL; }
		posttochannel(enadischn, channelnames[C_ENADIS], msg, sender, log->host->hostname, log, NULL);
	}
	else {
		/* If we got a downcause, and the status is not disabled, use downcause as the disable text */
		if (log->dismsg) { xfree(log->dismsg); log->dismsg = NULL; }
		if (downcause && (newcolor == COL_BLUE)) log->dismsg = strdup(downcause);
	}

	if (log->acktime) {
		/* Handling of ack'ed tests */

		if (decide_alertstate(newcolor) == A_OK) {
			/* The test recovered. Clear the ack. */
			log->acktime = 0;
			log->maxackedcolor = 0;
		}

		if (ackeachcolor && (log->maxackedcolor < newcolor)) {
			/* Severity has increased above the one that was acked. Clear the current ack */
			log->acktime = 0;
		}

		if (log->acktime > now) {
			/* Don't need to do anything about an acked test */
		}
		else {
			/* The acknowledge has expired. Clear the timestamp and the message buffer */
			log->acktime = 0;
			log->maxackedcolor = 0;
			if (log->ackmsg) { xfree(log->ackmsg); log->ackmsg = NULL; }
		}
	}

	if (!modifyonly) {
		log->logtime = now;

		/*
		 * Decide how long this status is valid.
		 *
		 * Normally we'll just set the valid time according 
		 * to the validity of the status report.
		 *
		 * If the status is acknowledged, make it valid for the longest period
		 * of the acknowledgment and the normal validity (so an acknowledged status
		 * does not go purple because it is not being updated due to the host being down).
		 *
		 * Same tweak must be done for disabled tests.
		 */
		log->validtime = now + validity*60;
		if (log->acktime    && (log->acktime > log->validtime))    log->validtime = log->acktime;
		if (log->enabletime) {
			if (log->enabletime == DISABLED_UNTIL_OK) log->validtime = INT_MAX;
			else if (log->enabletime > log->validtime) log->validtime = log->enabletime;
		}
		else if ((newcolor == COL_PURPLE) && (xmh_item(hinfo, XMH_DOWNTIME) != NULL) ) {
			/*
			 * If DOWNTIME is configured, we don't want to wait the default amount of time
			 * to re-scan for validity.
			*/
			log->validtime = now + 60;
		}

		/* 
		 * If we have an existing status, check if the sender has changed.
		 * This could be an indication of a mis-configured host reporting with
		 * the wrong hostname.
		 */
		if (log->sender && (strcmp(log->sender, sender) != 0)) {
			/*
			 * There are a few exceptions:
			 * - if sender is "xymond", then this is an internal update, e.g. a status going purple.
			 * - if the host has "pulldata" enabled, then the sender shows up as the host doing the
			 *   data collection, so it does not make sense to check it (thanks to Cade Robinson).
			 * - some multi-homed hosts use a random IP for sending us data.
			 */
			if ( (strcmp(log->sender, "xymond") != 0) && (strcmp(sender, "xymond") != 0) && (!conn_null_ip(sender)))  {
				if ((xmh_item(hinfo, XMH_PULLDATA) == NULL) && (xmh_item(hinfo, XMH_FLAG_MULTIHOMED) == NULL)) {
					log_multisrc(log, sender);
				}
			}
		}
		if (log->sender) xfree(log->sender);
		log->sender = strdup(sender);
	}


	/* Handle delayed red/yellow */
	switch (newcolor) {
	  case COL_RED:
		if (log->redstart == 0) log->redstart = now;
		/*
		 * Do NOT clear yellowstart. If we changed green->red, then it is already clear. 
		 * When changing yellow->red, we may drop down to yellow again later and then we 
		 * want to count the red time as part of the yellow status.
		 * But do set yellowstart if it is 0. If we go green->red now, and then later 
		 * red->yellow, we do want it to look as if the yellow began when the red status 
		 * happened.
		 */
		if (log->yellowstart == 0) log->yellowstart = now;
		break;

	  case COL_YELLOW:
		if (log->yellowstart == 0) log->yellowstart = now;
		log->redstart = 0;	/* Clear it here, so brief red's from a yellow state does not trigger red */
		break;

	  default:
		log->yellowstart = log->redstart = 0;
		break;
	}

	if ((newcolor == COL_RED) && ((delayval = changedelay(hinfo, COL_RED, testname, log->color)) > 0)) {
		if ((now - log->redstart) >= delayval) {
			/* Time's up - we will go red */
		}
		else {
			delayval = changedelay(hinfo, COL_YELLOW, testname, log->color);
			if ((now - log->redstart) >= delayval) {
				/* The yellow delay has been passed, so go yellow */
				newcolor = COL_YELLOW;
			}
			else {
				/* Neither yellow nor red delay passed - keep current color */
				newcolor = log->color;
			}
		}
	}
	else if ((newcolor == COL_YELLOW) && ((delayval = changedelay(hinfo, COL_YELLOW, testname, log->color)) > 0)) {
		if ((now - log->yellowstart) < delayval) newcolor = log->color; /* Keep current color */
	}


	log->oldcolor = log->color;
	log->color = newcolor;
	oldalertstatus = decide_alertstate(log->oldcolor);
	newalertstatus = decide_alertstate(newcolor);

	/* grouplist and log->grouplist can point to the same address.. */
	if (log->grouplist && log->grouplist != grouplist) xfree(log->grouplist);
	if (grouplist && !log->grouplist) log->grouplist = strdup(grouplist);

	if (log->acklist) {
		ackinfo_t *awalk;

		if ((oldalertstatus != A_OK) && (newalertstatus == A_OK)) {
			/* The status recovered. Set the "clearack" timer, unless it is just because we are in a DOWNTIME period */
			if (!log->downtimeactive) {
				time_t cleartime = now + ACKCLEARDELAY;
				for (awalk = log->acklist; (awalk); awalk = awalk->next) awalk->cleartime = cleartime;
			}
		}
		else if ((oldalertstatus == A_OK) && (newalertstatus != A_OK)) {
			/* The status went into a failure-mode. Any acks are revived */
			for (awalk = log->acklist; (awalk); awalk = awalk->next) awalk->cleartime = awalk->validuntil;
		}
	}

	if (msg != log->message) { /* They can be the same when called from handle_enadis() or check_purple_status() */
		char *p, *eoln = NULL;

		/*
		 * Note here:
		 * - log->msgsz is the buffer size INCLUDING the final \0.
		 * - msglen is the message length WITHOUT the final \0.
		 */
		if ((log->message == NULL) || (log->msgsz == 0)) {
			/* No buffer - get one */
			log->message = (unsigned char *)malloc(msglen+1);
			memcpy(log->message, msg, msglen+1);
			log->msgsz = msglen+1;
		}
		else if (log->msgsz > msglen) {
			/* Message - including \0 - fits into the existing buffer. */
			memcpy(log->message, msg, msglen+1);
		}
		else {
			/* Message does not fit into existing buffer. Grow it. */
			log->message = (unsigned char *)realloc(log->message, msglen+1);
			memcpy(log->message, msg, msglen+1);
			log->msgsz = msglen+1;
		}

		/* Isolate the first line and get at the test flags. They are immediately after the color */
		if (log->line1 == NULL) log->line1 = malloc(MAXLINE1SIZE * sizeof(unsigned char) + 1);
		eoln = strchr(msg, '\n'); if (eoln) *eoln = '\0';
		p = msg_data(msg, 0);
		snprintf(log->line1, MAXLINE1SIZE, "%s", p);	/* even if null, we terminate here */
		if (eoln) *eoln = '\n';
		p += strlen(colorname(newcolor));

		if (strncmp(p, " <!-- [flags:", 13) == 0) {
			char *flagstart = p+13;
			char *flagend = strchr(flagstart, ']');

			if (flagend) {
				*flagend = '\0';
				if (log->testflags == NULL)
					log->testflags = strdup(flagstart);
				else if (strlen(log->testflags) >= strlen(flagstart))
					strcpy(log->testflags, flagstart);
				else {
					log->testflags = realloc(log->testflags, strlen(flagstart));
					strcpy(log->testflags, flagstart);
				}
				*flagend = ']';
			}
		}
	}

	/* If in an alert state, we may need to generate a cookie */
	if (newalertstatus == A_ALERT) {
		if (log->cookieexpires < now) {
			char scookie[20];

			clear_cookie(log);

			/* Need to ensure that cookies are unique, hence the loop */
			do {
				snprintf(scookie, 20, "%ju", (uintmax_t)random());
			} while (find_cookie(scookie));

			dbgprintf("- generated cookie: %s\n", scookie);
			log->cookie = strdup(scookie);
			xtreeAdd(rbcookies, log->cookie, log);

			/*
			 * This is fundamentally flawed. The cookie should be generated by
			 * the alert module, because it may not be sent to the user for
			 * a long time, depending on the alert configuration.
			 * That's for later - for now, we'll just give it a long enough 
			 * lifetime so that cookies will be valid.
			 */
			log->cookieexpires = now + defaultcookietime; /* 1 day by default */
		}
	}
	else {
		/* Not alert state, so clear any cookies */
		if (log->cookie) clear_cookie(log);
	}

	if (!issummary && (!log->histsynced || (log->oldcolor != newcolor) || modifychanged)) {
		/*
		 * Change of color goes to the status-change channel.
		 */
		dbgprintf("posting to stachg channel: host=%s, test=%s\n", hostname, testname);
		posttochannel(stachgchn, channelnames[C_STACHG], msg, sender, hostname, log, NULL);
		log->histsynced = 1;

		/*
		 * Don't update the log->lastchange timestamp while DOWNTIME is active.
		 * (It is only seen as active if the color has been forced BLUE).
		 */
		if (!log->downtimeactive && ((log->oldcolor != newcolor) || modifychanged)) {
			int i;
			if (log->host->clientmsgs && (newalertstatus == A_ALERT) && log->test->clientsave) {
				posttochannel(clichgchn, channelnames[C_CLICHG], msg, sender, 
						hostname, log, NULL);
			}

			if ((log->oldcolor != newcolor) && (flapcount > 0)) {
				/* We keep track of flaps, so update the lastchange table */
				for (i=flapcount-1; (i > 0); i--)
					log->lastchange[i] = log->lastchange[i-1];
			}
			log->lastchange[0] = now;
			log->statuschangecount++;
		}
	}

	if (!issummary) {
		if (newalertstatus == A_ALERT) {
			/* Status is critical, send alerts */
			dbgprintf("posting alert to page channel\n");

			log->activealert = 1;
			posttochannel(pagechn, channelnames[C_PAGE], msg, sender, hostname, log, NULL);
		}
		else if (log->activealert && (oldalertstatus != A_OK) && (newalertstatus == A_OK)) {
			/* Status has recovered, send recovery notice */
			dbgprintf("posting recovery to page channel\n");

			log->activealert = 0;
			posttochannel(pagechn, channelnames[C_PAGE], msg, sender, hostname, log, NULL);
		}
		else if (log->activealert && (log->oldcolor != newcolor)) {
			/* 
			 * Status is in-between critical and recovered, but we do have an
			 * active alert for this status. So tell the pager module that the
			 * color has changed.
			 */
			dbgprintf("posting color change to page channel\n");
			posttochannel(pagechn, channelnames[C_PAGE], msg, sender, hostname, log, NULL);
		}
		else if (log->activealert && modifychanged) {
			/* 
			 * A modifier was added or removed when we do have an
			 * active alert for this status. So tell the pager module so that it can use
			 * the updated data.
			 */
			dbgprintf("posting modifier change to page channel\n");
			posttochannel(pagechn, channelnames[C_PAGE], msg, sender, hostname, log, NULL);
		}
	}

	/* Don't post to the status channel if this was just a modify */
	if (!modifyonly || modifychanged) {
		dbgprintf("posting to status channel\n");
		posttochannel(statuschn, channelnames[C_STATUS], msg, sender, hostname, log, NULL);
	}

	dbgprintf("<-handle_status\n");
	return;
}

void handle_modify(char *msg, xymond_log_t *log, int color, time_t now)
{
	char *tok, *eoln, *modstr, *sourcename, *cause;
	modifier_t *mwalk;
	int newcolor;
	enum modifytype_t type;
	int n, isnewcause = 0, validity = 0;
	time_t validtime = 0;

	/* "modify[up|down][+N][+Nv] HOSTNAME.TESTNAME COLOR SOURCE CAUSE ..." */
	dbgprintf("->handle_modify\n");

	if ((color < 0) || (color >= COL_COUNT)) return;

	sourcename = cause = NULL;

	/*
	 * Modify messages come in one of three varieties:
	 * 	modify = neutrally override existing color
	 *		(the worst neutral modify will be the resulting color)
	 * 	modifydown = only DECREASES alert status (red -> yellow -> green)
	 * 	modifyup = only INCREASES alert status (green -> yellow, or yellow -> red)
	 * 
	 * modifydown and modifyup are needed to ensure you don't accidentally
	 * override an underlying 'red' state with a yellow caused by some other minor issue
	 */
	eoln = strchr(msg, ' ');
	if (!eoln) { errprintf("Received garbled modify: %s\n", msg); return; }
	*eoln = '\0'; modstr = strdup(msg); *eoln = ' ';


	type =  (!strncmp(modstr, "modifydown", 10)) ? MODIFY_DOWN :
		(!strncmp(modstr, "modifyup",    8)) ? MODIFY_UP   : MODIFY_NORM;

	/*
	 * Figure out if a duration or validity specifier (or both) are present.
	 * If not, 'DEFAULT_MODIFY_VALIDITY' is set. If so, then one/both are set.
	 * The first one that expires will cause it to be removed (in handle_status).
	 */

	dbgprintf(" - modify msg=%s\n", msg);
	tok = strtok(modstr, "+");
	if (tok) tok = strtok(NULL, "+"); /* skip MODIFY[up|down]+ */
	while (tok) {
		/* 4v = "validity", otherwise regular duration */
		if (strcmp((tok + strlen(tok) - 1), "v") == 0) {
			n = atoi(tok);
			validity = (n > 0) ? n : DEFAULT_MODIFY_VALIDITY; // could be garbage

		} else {
			validtime = durationvalue(tok)*60 + now;
			if (!validity) validity = -10; /* Just make sure it's not 0 */

		}
		// dbgprintf(" -- modify specifier %s now: validity: %d, expires %d\n", tok, validity, validtime);
		tok = strtok(NULL, "+"); /* could have both */
        }

	xfree(modstr);

	tok = strtok(msg, " "); /* Skip "modify" */
	if (tok) tok = strtok(NULL, " "); /* Skip HOSTNAME.TESTNAME */
	if (tok) tok = strtok(NULL, " "); /* Skip COLOR */
	if (tok) sourcename = strtok(NULL, " ");
	if (sourcename) cause = strtok(NULL, "\r\n");

	if (!cause) {
		errprintf("Garbled modify statement: source=%s, type=%d, validnum=%d, expires=%d, cause=%s\n", sourcename, type, validity, validtime, cause);
		return;
	}
	else dbgprintf(" - parse of modify statement; source=%s, type=%d, validnum=%d, expires=%d, cause=%s\n", sourcename, type, validity, validtime, cause);

	/* Got all tokens - find the modifier, if this is just an update */
	n = strlen(sourcename);
	for (mwalk = log->modifiers; (mwalk && strncmp(mwalk->source, sourcename, n)); mwalk = mwalk->next);

	if (!mwalk) {
		/* New modifier record */
		dbgprintf(" - creating new modify record for '%s'\n", sourcename);
		isnewcause = 1;
		mwalk = (modifier_t *)calloc(1, sizeof(modifier_t));
		mwalk->source = strdup(sourcename);
		mwalk->next = log->modifiers;
		log->modifiers = mwalk;
	}

	mwalk->color = color;
	mwalk->validtime = validtime;	/* 0 if unset */
	mwalk->valid = (validity ? validity : DEFAULT_MODIFY_VALIDITY); /* if duration ONLY, this will be < 0 */
	mwalk->type = type;
	if (mwalk->cause) xfree(mwalk->cause);
	mwalk->cause = (char *)malloc(strlen(cause) + 10); /* 10 for maxlength of colorname + markers */
	sprintf(mwalk->cause, "&%s %s\n", colnames[mwalk->color], cause);

	/*
	 * Modify messages always get sent to handle_status for evaluation.
	 * It's possible a status change will result, or just a new status message.
	 */
	handle_status(log->message, log->sender,  
		log->host->hostname, log->test->name, log->grouplist, log, log->color, NULL, (isnewcause ? 2 : 1) );

	dbgprintf("<-handle_modify\n");
}


void handle_data(char *msg, char *sender, char *origin, char *hostname, char *testname)
{
	void *hi;
	int n, buflen = 0;
	char *classname, *pagepath;

	dbgprintf("->handle_data\n");

	hi = hostinfo(hostname);
	classname = (hi ? xmh_item(hi, XMH_CLASS) : NULL);
	pagepath = (hi ? xmh_item(hi, XMH_ALLPAGEPATHS) : "");

	if (origin) buflen += strlen(origin); else dbgprintf("   origin is NULL\n");
	if (hostname) buflen += strlen(hostname); else dbgprintf("  hostname is NULL\n");
	if (testname) buflen += strlen(testname); else dbgprintf("  testname is NULL\n");
	if (classname) buflen += strlen(classname);
	if (pagepath) buflen += strlen(pagepath);
	if (msg) buflen += strlen(msg); else dbgprintf("  msg is NULL\n");
	buflen += 6;

	n = snprintf(datachn->workmem, datachn->maxsize, "%s|%s|%s|%s|%s\n%s", 
		 (origin ? origin : ""), 
		 (hostname ? hostname : ""), 
		 (testname ? testname : ""), 
		 (classname ? classname : ""),
		 (pagepath ? pagepath : ""),
		 msg);

	if (n >= datachn->maxsize) errprintf("Data msg from %s/%s for %s.%s truncated by %d bytes\n", 
		textornull(sender), textornull(origin), textornull(hostname), textornull(testname), n - datachn->maxsize + 1);
	posttochannel(datachn, channelnames[C_DATA], msg, sender, hostname, NULL, datachn->workmem);
	dbgprintf("<-handle_data\n");
}

void handle_notes(char *msg, char *sender, char *hostname)
{
	dbgprintf("->handle_notes\n");
	posttochannel(noteschn, channelnames[C_NOTES], msg, sender, hostname, NULL, NULL);
	dbgprintf("<-handle_notes\n");
}

void handle_usermsg(char *msg, char *sender, char *hostname)
{
	dbgprintf("->handle_usermsg\n");
	posttochannel(userchn, channelnames[C_USER], msg, sender, hostname, NULL, NULL);
	dbgprintf("<-handle_usermsg\n");
}

void handle_enadis(int enabled, conn_t *msg, char *sender, int viabfq)
{
	char *firstline = NULL, *hosttest = NULL, *durstr = NULL, *txtstart = NULL;
	char *hname = NULL, *tname = NULL;
	time_t expires = 0;
	int alltests = 0;
	xtreePos_t hosthandle, testhandle;
	xymond_hostlist_t *hwalk = NULL;
	testinfo_t *twalk = NULL;
	xymond_log_t *log;
	char *p;
	char *hostip = NULL;

	dbgprintf("->handle_enadis\n");

	p = strchr(msg->buf, '\n'); if (p) *p = '\0';
	firstline = strdup(msg->buf);
	if (p) *p = '\n';

	p = strtok(firstline, " \t");
	if (p) hosttest = strtok(NULL, " \t");
	if (hosttest) durstr = strtok(NULL, " \t");
	if (!hosttest) {
		errprintf("Invalid enable/disable from %s - no host/test specified\n", sender);
		goto done;
	}

	if (!enabled) {
		if (durstr) {
			if (strcmp(durstr, "-1") == 0) {
				expires = DISABLED_UNTIL_OK;
			}
			else {
				int expirerounding;

				expires = 60*durationvalue(durstr) + getcurrenttime(NULL);

				/* If "expires" is not on the ":00" seconds, bump expire time to next whole minute */
				expirerounding = (60 - (expires % 60));
				if (expirerounding < 60) expires += expirerounding;
			}

			txtstart = msg->buf + (durstr + strlen(durstr) - firstline);
			txtstart += strspn(txtstart, " \t\r\n");
			if (*txtstart == '\0') txtstart = "(No reason given)";
		}
		else {
			errprintf("Invalid disable from %s - no duration specified\n", sender);
			goto done;
		}
	}

	p = hosttest + strlen(hosttest) - 1;
	if (*p == '*') {
		/* It ends with a '*' so assume this is for all tests */
		alltests = 1;
		*p = '\0';
		p--;
		if (*p == '.') *p = '\0';
	}
	else {
		/* No wildcard -> get the test name */
		p = strrchr(hosttest, '.');
		if (p == NULL) goto done; /* "enable foo" ... surely you must be joking. */
		*p = '\0';
		tname = (p+1);
	}
	uncommafy(hosttest);
	hname = knownhost(hosttest, &hostip, ghosthandling);
	if (hname == NULL) goto done;

	hosthandle = xtreeFind(rbhosts, hname);
	if (hosthandle == xtreeEnd(rbhosts)) {
		/* Unknown host */
		goto done;
	}
	else hwalk = xtreeData(rbhosts, hosthandle);

	if (!viabfq && !oksender(maintsenders, hostinfo(hwalk->hostname), msg->sender, msg->buf)) goto done;

	if (tname) {
		testhandle = xtreeFind(rbtests, tname);
		if (testhandle == xtreeEnd(rbtests)) {
			/* Unknown test */
			goto done;
		}
		else twalk = xtreeData(rbtests, testhandle);
	}

	if (enabled) {
		/* Enable is easy - just clear the enabletime */
		if (alltests) {
			for (log = hwalk->logs; (log); log = log->next) {
				log->enabletime = 0;
				if (log->dismsg) {
					xfree(log->dismsg);
					log->dismsg = NULL;
				}
				posttochannel(enadischn, channelnames[C_ENADIS], msg->buf, sender, log->host->hostname, log, NULL);
			}
		}
		else {
			for (log = hwalk->logs; (log && (log->test != twalk)); log = log->next) ;
			if (log) {
				log->enabletime = 0;
				if (log->dismsg) {
					xfree(log->dismsg);
					log->dismsg = NULL;
				}
				posttochannel(enadischn, channelnames[C_ENADIS], msg->buf, sender, log->host->hostname, log, NULL);
			}
		}
	}
	else {
		/* disable code goes here */

		if (alltests) {
			for (log = hwalk->logs; (log); log = log->next) {
				log->enabletime = expires;
				log->validtime = (expires == DISABLED_UNTIL_OK) ? INT_MAX : log->validtime;
				if (txtstart) {
					if (log->dismsg) xfree(log->dismsg);
					log->dismsg = strdup(txtstart);
				}
				posttochannel(enadischn, channelnames[C_ENADIS], msg->buf, sender, log->host->hostname, log, NULL);
				/* Trigger an immediate status update */
				handle_status(log->message, sender, log->host->hostname, log->test->name, log->grouplist, log, COL_BLUE, NULL, 0);
			}
		}
		else {
			for (log = hwalk->logs; (log && (log->test != twalk)); log = log->next) ;
			if (log) {
				log->enabletime = expires;
				log->validtime = (expires == DISABLED_UNTIL_OK) ? INT_MAX : log->validtime;
				if (txtstart) {
					if (log->dismsg) xfree(log->dismsg);
					log->dismsg = strdup(txtstart);
				}
				posttochannel(enadischn, channelnames[C_ENADIS], msg->buf, sender, log->host->hostname, log, NULL);

				/* Trigger an immediate status update */
				handle_status(log->message, sender, log->host->hostname, log->test->name, log->grouplist, log, COL_BLUE, NULL, 0);
			}
		}

	}

done:
	xfree(firstline);

	dbgprintf("<-handle_enadis\n");

	return;
}


void handle_ack(char *msg, char *sender, xymond_log_t *log, int duration)
{
	char *p;

	dbgprintf("->handle_ack\n");

	log->acktime = getcurrenttime(NULL)+duration*60;
	if (log->color > log->maxackedcolor) log->maxackedcolor = log->color;
	if (log->validtime < log->acktime) log->validtime = log->acktime;

	p = msg;
	p += strspn(p, " \t");			/* Skip the space ... */
	p += strspn(p, "-0123456789");		/* and the cookie ... */
	p += strspn(p, " \t");			/* and the space ... */
	p += strspn(p, "0123456789hdwmy");	/* and the duration ... */
	p += strspn(p, " \t");			/* and the space ... */
	log->ackmsg = strdup(p);

	/* Tell the pagers */
	posttochannel(pagechn, "ack", log->ackmsg, sender, log->host->hostname, log, NULL);

	dbgprintf("<-handle_ack\n");
	return;
}

void handle_ackinfo(char *msg, char *sender, xymond_log_t *log)
{
	int level = -1;
	time_t validuntil = -1, itemval;
	time_t received = getcurrenttime(NULL);
	char *ackedby = NULL, *ackmsg = NULL;
	char *tok, *item;
	int itemno = 0;

	tok = msg;
	while (tok) {
		tok += strspn(tok, " \t\n");
		item = tok; itemno++;
		tok = strchr(tok, '\n'); if (tok) { *tok = '\0'; tok++; }

		switch (itemno) {
		  case 1: break; /* First line has just the HOST.TEST */
		  case 2: level = atoi(item); break;
		  case 3: itemval = atoi(item); 
			  if (itemval == -1) itemval = 365*24*60*60; /* 1 year */
			  validuntil = received + itemval; 
			  break;
		  case 4: ackedby = strdup(item); break;
		  case 5: ackmsg = strdup(item); break;
		}
	}

	if ((level >= 0) && (validuntil > received) && ackedby && ackmsg) {
		ackinfo_t *newack;
		int isnew;

		dbgprintf("Got ackinfo: Level=%d,until=%d,ackby=%s,msg=%s\n", level, validuntil, ackedby, ackmsg);

		/* See if we already have this ack in the list */
		for (newack = log->acklist; (newack && ((level != newack->level) || strcmp(newack->ackedby, ackedby))); newack = newack->next);

		isnew = (newack == NULL);
		dbgprintf("This ackinfo is %s\n", (isnew ? "new" : "old"));
		if (isnew) {
			dbgprintf("Creating new ackinfo record\n");
			newack = (ackinfo_t *)malloc(sizeof(ackinfo_t));
		}
		else {
			/* Drop the old data so we don't leak memory */
			dbgprintf("Dropping old ackinfo data: From %s, msg=%s\n", newack->ackedby, newack->msg);
			if (newack->ackedby) xfree(newack->ackedby); 
			if (newack->msg) xfree(newack->msg);
		}

		newack->level = level;
		newack->received = received;
		newack->validuntil = newack->cleartime = validuntil;
		newack->ackedby = ackedby;
		newack->msg = ackmsg;

		if (isnew) {
			newack->next = log->acklist;
			log->acklist = newack;
		}

		if (ackinfologfd) {
			char timestamp[25];

			strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&received));
			fprintf(ackinfologfd, "%s %s %s %s %d %d %d %d %s\n",
				timestamp, log->host->hostname, log->test->name,
				newack->ackedby, newack->level, 
				(int)log->lastchange[0], (int)newack->received, (int)newack->validuntil, 
				nlencode(newack->msg));
			fflush(ackinfologfd);
		}
	}
	else {
		if (ackedby) xfree(ackedby);
		if (ackmsg) xfree(ackmsg);
	}
}

void handle_notify(char *msg, char *sender, char *hostname, char *testname)
{
	char *msgtext, *channelmsg;
	void *hi;

	dbgprintf("-> handle_notify\n");

	hi = hostinfo(hostname);

	msgtext = msg_data(msg, 0);
	channelmsg = (char *)malloc(1024 + strlen(msgtext));

	/* Tell the pagers */
	sprintf(channelmsg, "%s|%s|%s\n%s", 
		hostname, (testname ? testname : ""), (hi ? xmh_item(hi, XMH_ALLPAGEPATHS) : ""), msgtext);
	posttochannel(pagechn, "notify", msg, sender, hostname, NULL, channelmsg);

	xfree(channelmsg);

	dbgprintf("<- handle_notify\n");
	return;
}

void handle_client(char *msg, char *sender, char *hostname, char *collectorid, 
		   char *clientos, char *clientclass)
{
	char *theclass;
	int n, msglen, buflen = 0;
	xtreePos_t hosthandle;
	clientmsg_list_t *cwalk, *chead, *ctail, *czombie;

	dbgprintf("->handle_client\n");

	/* Default class is the OS */
	if (!collectorid) collectorid = "";
	theclass = (clientclass ? clientclass : clientos);
	buflen += strlen(hostname) + strlen(clientos) + strlen(theclass) + strlen(collectorid);
	if (msg) { msglen = strlen(msg); buflen += msglen; } else { dbgprintf("  msg is NULL\n"); return; }
	buflen += 6;

	if (clientsavemem) {
		hosthandle = xtreeFind(rbhosts, hostname);
		if (hosthandle != xtreeEnd(rbhosts)) {
			xymond_hostlist_t *hwalk;
			hwalk = xtreeData(rbhosts, hosthandle);

			for (cwalk = hwalk->clientmsgs; (cwalk && strcmp(cwalk->collectorid, collectorid)); cwalk = cwalk->next) ;
			if (cwalk) {
				if (strlen(cwalk->msg) >= msglen)
					strcpy(cwalk->msg, msg);
				else {
					xfree(cwalk->msg);
					cwalk->msg = strdup(msg);
				}
			}
			else {
				cwalk = (clientmsg_list_t *)calloc(1, sizeof(clientmsg_list_t));
				cwalk->collectorid = strdup(collectorid);
				cwalk->next = hwalk->clientmsgs;
				hwalk->clientmsgs = cwalk;
				cwalk->msg = strdup(msg);
			}

			hwalk->clientmsgtstamp = cwalk->timestamp = gettimer();

			/* Purge any outdated client sub-messages */
			chead = ctail = NULL;
			cwalk = hwalk->clientmsgs; 
			while (cwalk) {
				if ((cwalk->timestamp + MAX_SUBCLIENT_LIFETIME) < hwalk->clientmsgtstamp) {
					/* This entry has expired */
					czombie = cwalk;
					cwalk = cwalk->next;
					xfree(czombie->msg);
					xfree(czombie->collectorid);
					xfree(czombie);
				}
				else {
					if (ctail) {
						ctail->next = cwalk; 
						ctail = cwalk; 
					} 
					else { 
						chead = ctail = cwalk; 
					}

					cwalk = cwalk->next;
					ctail->next = NULL;
				}
			}
			hwalk->clientmsgs = chead;
		}
	}

	n = snprintf(clientchn->workmem, clientchn->maxsize, "%s|%s|%s|%s\n%s", hostname, clientos, theclass, collectorid, msg);

	if (n >= clientchn->maxsize) errprintf("Client msg from %s for %s.%s truncated by %d bytes\n", 
		textornull(sender), textornull(hostname), textornull(collectorid), n - clientchn->maxsize + 1);
	posttochannel(clientchn, channelnames[C_CLIENT], msg, sender, hostname, NULL, clientchn->workmem);
	dbgprintf("<-handle_client\n");
}


void flush_acklist(xymond_log_t *zombie, int flushall)
{
	ackinfo_t *awalk, *newhead = NULL, *newtail = NULL;
	time_t now = getcurrenttime(NULL);

	awalk = zombie->acklist;
	while (awalk) {
		ackinfo_t *tmp = awalk;
		awalk = awalk->next;

		if (flushall || (tmp->cleartime < now) || (tmp->validuntil < now)) {
			if (tmp->ackedby) xfree(tmp->ackedby);
			if (tmp->msg) xfree(tmp->msg);
			xfree(tmp);
		}
		else {
			/* We have a record we want to keep */
			if (newhead == NULL) {
				newhead = newtail = tmp;
			}
			else {
				newtail->next = tmp;
				newtail = tmp;
			}
		}
	}

	if (newtail) newtail->next = NULL;
	zombie->acklist = newhead;
}

char *acklist_string(xymond_log_t *log, int level)
{
	static strbuffer_t *res = NULL;
	ackinfo_t *awalk;
	char tmpstr[512];

	if (log->acklist == NULL) return NULL;

	if (res) clearstrbuffer(res); else res = newstrbuffer(0);

	for (awalk = log->acklist; (awalk); awalk = awalk->next) {
		if ((level != -1) && (awalk->level != level)) continue;
		snprintf(tmpstr, sizeof(tmpstr), "%d:%d:%d:%s:%s\n", 
			 (int)awalk->received, (int)awalk->validuntil, 
			 awalk->level, awalk->ackedby, awalk->msg);
		tmpstr[sizeof(tmpstr)-1] = '\0';
		addtobuffer(res, tmpstr);
	}

	return STRBUF(res);
}

void free_log_t(xymond_log_t *zombie)
{
	modifier_t *modwalk, *modtmp;

	dbgprintf("-> free_log_t\n");
	if (zombie->host && zombie->host->hostname) dbgprintf(" - for hostname %s\n", zombie->host->hostname);
	if (zombie->test && zombie->test->name) dbgprintf(" -- freeing testname %s\n", zombie->test->name);

	modwalk = zombie->modifiers;
	while (modwalk) {
		modtmp = modwalk;
		modwalk = modwalk->next;

		if (modtmp->source) xfree(modtmp->source);
		if (modtmp->cause) xfree(modtmp->cause);
		xfree(modtmp);
	}
	if (zombie->modifierbuf) xfree(zombie->modifierbuf);
	if (zombie->sender) xfree(zombie->sender);
	if (zombie->message) xfree(zombie->message);
	if (zombie->line1) xfree(zombie->line1);
	if (zombie->dismsg) xfree(zombie->dismsg);
	if (zombie->ackmsg) xfree(zombie->ackmsg);
	if (zombie->grouplist) xfree(zombie->grouplist);
	if (zombie->lastchange) xfree(zombie->lastchange);
	if (zombie->testflags) xfree(zombie->testflags);
	flush_acklist(zombie, 1);
	xfree(zombie);
	dbgprintf("<- free_log_t\n");
}

void handle_dropnrename(enum droprencmd_t cmd, char *sender, char *hostname, char *n1, char *n2)
{
	char *hostip = NULL;
	xtreePos_t hosthandle, testhandle;
	xymond_hostlist_t *hwalk;
	testinfo_t *twalk, *newt;
	xymond_log_t *lwalk;
	char *marker = NULL;
	char *canonhostname;

	dbgprintf("-> handle_dropnrename\n");

	{
		/*
		 * We pass drop- and rename-messages to the workers, whether 
		 * we know about this host or not. It could be that the drop command
		 * arrived after we had already re-loaded the hosts.cfg file, and 
		 * so the host is no longer known by us - but there is still some
		 * data stored about it that needs to be cleaned up.
		 */

		char *msgbuf = (char *)malloc(20 + strlen(hostname) + (n1 ? strlen(n1) : 0) + (n2 ? strlen(n2) : 0));

		*msgbuf = '\0';
		switch (cmd) {
		  case CMD_DROPTEST:
			marker = "droptest";
			sprintf(msgbuf, "%s|%s", hostname, n1);
			break;
		  case CMD_DROPHOST:
			marker = "drophost";
			sprintf(msgbuf, "%s", hostname);
			break;
		  case CMD_RENAMEHOST:
			marker = "renamehost";
			sprintf(msgbuf, "%s|%s", hostname, n1);
			break;
		  case CMD_RENAMETEST:
			marker = "renametest";
			sprintf(msgbuf, "%s|%s|%s", hostname, n1, n2);
			break;
		  case CMD_DROPSTATE:
			marker = "dropstate";
			sprintf(msgbuf, "%s", hostname);
			break;
		}

		if (strlen(msgbuf)) {
			/* Tell the workers */
			logprintf("Sending %s (from %s) with %s\n", marker, sender, msgbuf);
			posttochannel(statuschn, marker, NULL, sender, NULL, NULL, msgbuf);
			posttochannel(stachgchn, marker, NULL, sender, NULL, NULL, msgbuf);
			posttochannel(pagechn, marker, NULL, sender, NULL, NULL, msgbuf);
			posttochannel(datachn, marker, NULL, sender, NULL, NULL, msgbuf);
			posttochannel(noteschn, marker, NULL, sender, NULL, NULL, msgbuf);
			posttochannel(enadischn, marker, NULL, sender, NULL, NULL, msgbuf);
			posttochannel(clientchn, marker, NULL, sender, NULL, NULL, msgbuf);
			posttochannel(clichgchn, marker, NULL, sender, NULL, NULL, msgbuf);
			posttochannel(userchn, marker, NULL, sender, NULL, NULL, msgbuf);
		}

		xfree(msgbuf);
	}


	/*
	 * Now clean up our internal state info, if there is any.
	 * NB: knownhost() may return NULL, if the hosts.cfg file was re-loaded before
	 * we got around to cleaning up a host.
	 */
	canonhostname = knownhost(hostname, &hostip, ghosthandling);
	if (canonhostname) hostname = canonhostname;
	dbgprintf(" - canonhostname was: %s\n", canonhostname);

	hosthandle = xtreeFind(rbhosts, hostname);
	dbgprintf(" - hosthandle: %p\n", hosthandle);
	if (hosthandle == xtreeEnd(rbhosts)) goto done;
	else hwalk = xtreeData(rbhosts, hosthandle);
	dbgprintf(" - hwalk: %p\n", hwalk);
	if (hwalk == NULL) { errprintf("- droptest given with null host data\n"); goto done; }
	
	switch (cmd) {
	  case CMD_DROPTEST:
		testhandle = xtreeFind(rbtests, n1);
		if (testhandle == xtreeEnd(rbtests)) goto done;
		twalk = xtreeData(rbtests, testhandle);
		dbgprintf(" - twalk: %p\n", twalk);
		if (twalk == NULL) { errprintf("- droptest given with null test data\n"); goto done; }

		for (lwalk = hwalk->logs; (lwalk && (lwalk->test != twalk)); lwalk = lwalk->next) ;
		if (lwalk == NULL) { errprintf("- droptest given with test data but no log record\n"); goto done; }
		if (lwalk == hwalk->pinglog) hwalk->pinglog = NULL;
		if (lwalk == hwalk->logs) {
			hwalk->logs = hwalk->logs->next;
		}
		else {
			xymond_log_t *plog;
			for (plog = hwalk->logs; (plog->next != lwalk); plog = plog->next) ;
			plog->next = lwalk->next;
		}
		free_log_t(lwalk);
		break;

	  case CMD_DROPHOST:
	  case CMD_DROPSTATE:
		/* Loop through the host logs and free them */
		lwalk = hwalk->logs;
		while (lwalk) {
			xymond_log_t *tmp = lwalk;
			lwalk = lwalk->next;

			free_log_t(tmp);
		}

		/* Now loop through the client logs and free those */
		while (hwalk->clientmsgs) {
			clientmsg_list_t *czombie = hwalk->clientmsgs;
			hwalk->clientmsgs = hwalk->clientmsgs->next;

			xfree(czombie->collectorid);
			xfree(czombie->msg);
			xfree(czombie);
		}

		/* Unlink the hostlist entry */
		xtreeDelete(rbhosts, hostname);
		hostcount--;

		/* Free the hostlist entry */
#ifndef HAVE_BINARY_TREE
		xfree(hwalk->hostname);
		xfree(hwalk->ip);
#endif
		xfree(hwalk);
		break;

	  case CMD_RENAMEHOST:
		xtreeDelete(rbhosts, hostname);
		xfree(hwalk->hostname);
		hwalk->hostname = strdup(n1);
		xtreeAdd(rbhosts, hwalk->hostname, hwalk);
		break;

	  case CMD_RENAMETEST:
		testhandle = xtreeFind(rbtests, n1);
		if (testhandle == xtreeEnd(rbtests)) goto done;
		twalk = xtreeData(rbtests, testhandle);

		for (lwalk = hwalk->logs; (lwalk && (lwalk->test != twalk)); lwalk = lwalk->next) ;
		if (lwalk == NULL) goto done;

		if (lwalk == hwalk->pinglog) hwalk->pinglog = NULL;

		testhandle = xtreeFind(rbtests, n2);
		if (testhandle == xtreeEnd(rbtests)) {
			newt = create_testinfo(n2);
		}
		else {
			newt = xtreeData(rbtests, testhandle);
		}
		lwalk->test = newt;
		break;
	}

done:
	dbgprintf("<- handle_dropnrename\n");

	return;
}


unsigned char *get_filecache(char *fn, long *len)
{
	xtreePos_t handle;
	filecache_t *item;
	unsigned char *result;

	handle = xtreeFind(rbfilecache, fn);
	if (handle == xtreeEnd(rbfilecache)) return NULL;

	item = (filecache_t *)xtreeData(rbfilecache, handle);
	if (item->len < 0) return NULL;

	result = (unsigned char *)malloc(item->len);
	memcpy(result, item->fdata, item->len);
	*len = item->len;

	return result;
}


void add_filecache(char *fn, unsigned char *buf, off_t buflen)
{
	xtreePos_t handle;
	filecache_t *newitem;

	handle = xtreeFind(rbfilecache, fn);
	if (handle == xtreeEnd(rbfilecache)) {
		newitem = (filecache_t *)malloc(sizeof(filecache_t));
		newitem->fn = strdup(fn);
		newitem->len = buflen;
		newitem->fdata = (unsigned char *)malloc(buflen);
		memcpy(newitem->fdata, buf, buflen);
		xtreeAdd(rbfilecache, newitem->fn, newitem);
	}
	else {
		newitem = (filecache_t *)xtreeData(rbfilecache, handle);
		if (newitem->fdata) xfree(newitem->fdata);
		newitem->len = buflen;
		newitem->fdata = (unsigned char *)malloc(buflen);
		memcpy(newitem->fdata, buf, buflen);
	}
}


void flush_filecache(void)
{
	xtreePos_t handle;

	logprintf("Flushing filecache\n");
	for (handle = xtreeFirst(rbfilecache); (handle != xtreeEnd(rbfilecache)); handle = xtreeNext(rbfilecache, handle)) {
		filecache_t *item = (filecache_t *)xtreeData(rbfilecache, handle);
		if (item->fdata) xfree(item->fdata);
		item->len = -1;
	}
}


int get_config(char *fn, conn_t *msg)
{
	char fullfn[PATH_MAX];
	FILE *fd = NULL;
	size_t fnlen;
	static int allowallconfigs = -1;
	struct stat st;
	strbuffer_t *inbuf, *result;

	dbgprintf("-> get_config %s\n", fn);

	if (allowallconfigs == -1) allowallconfigs = (strcmp(xgetenv("ALLOWALLCONFIGFILES"), "TRUE") == 0);

	fnlen = strlen(fn);
	if (fnlen < 4) {
		errprintf("Config filename requested (%s) is too short\n", fn);
		return -1;
	}

	/* Make sure it ends in .cfg */
	if ((!allowallconfigs) && (strcasecmp(fn + fnlen - 4, ".cfg") != 0) ) {
		errprintf("Config filename requested (%s) doesn't end in '.cfg'\n", fn);
		return -1;
	}

	snprintf(fullfn, sizeof(fullfn), "%s/etc/%s", xgetenv("XYMONHOME"), fn);
	if (! (lstat(fullfn, &st) == 0) ) {
		errprintf("Config file %s (%s) error: %s\n", fn, fullfn, strerror(errno));
		return -1;
        }
	if (! S_ISREG(st.st_mode)) {
		errprintf("Config file requested (%s) is not a regular file\n", fullfn);
		return -1;
	}

	fd = stackfopen(fullfn, "r", NULL);
	if (fd == NULL) {
		errprintf("Config file %s not readable\n", fn);
		return -1;
	}

	inbuf = newstrbuffer(0);
	result = newstrbuffer(0);
	while (stackfgets(inbuf, NULL) != NULL) addtostrbuffer(result, inbuf);
	stackfclose(fd);
	freestrbuffer(inbuf);

	msg->buflen = STRBUFLEN(result);
	msg->buf = grabstrbuffer(result);
	msg->bufp = msg->buf + msg->buflen;

	dbgprintf("<- get_config\n");

	return 0;
}

int get_hostsconfig(char *fn, conn_t *msg)
{
	char fullfn[PATH_MAX];
	FILE *fd = NULL;
	strbuffer_t *inbuf, *result;
	char *incache;
	long flen;

	dbgprintf("-> get_hostsconfig %s\n", fn);
	sprintf(fullfn, "%s/etc/%s", xgetenv("XYMONHOME"), fn);

	incache = get_filecache(fullfn, &flen);
	if (incache) {
		msg->buflen = flen;
		msg->buf = incache;
		msg->bufp = msg->buf + msg->buflen;

		dbgprintf("<- get_hostsconfig (returning cached copy of host config)\n");
		return 0;
	}

	fd = stackfopen(fullfn, "r", NULL);
	if (fd == NULL) {
		errprintf("Config file %s not found\n", fn);
		return -1;
	}

	inbuf = newstrbuffer(0);
	result = newstrbuffer(0);
	while (stackfgets(inbuf, NULL) != NULL) addtostrbuffer(result, inbuf);
	stackfclose(fd);
	freestrbuffer(inbuf);

	dbgprintf(" - get_hostsconfig: adding host config file to cache\n");
	add_filecache(fullfn, STRBUF(result), STRBUFLEN(result));

	msg->buflen = STRBUFLEN(result);
	msg->buf = grabstrbuffer(result);
	msg->bufp = msg->buf + msg->buflen;

	dbgprintf("<- get_hostsconfig\n");

	return 0;
}

int get_binary(char *fn, conn_t *msg)
{
	char fullfn[PATH_MAX];
	int fd;
	struct stat st;
	unsigned char *result;
	long flen;

	dbgprintf("-> get_binary %s\n", fn);
	snprintf(fullfn, sizeof(fullfn), "%s/download/%s", xgetenv("XYMONHOME"), fn);

	result = get_filecache(fullfn, &flen);
	if (!result) {
		fd = open(fullfn, O_RDONLY);
		if (fd == -1) {
			errprintf("Download file %s not found\n", fn);
			return -1;
		}

		if (fstat(fd, &st) == 0) {
			ssize_t n;

			result = (unsigned char *)malloc(st.st_size);
			n = read(fd, result, st.st_size);
			if (n != st.st_size) {
				errprintf("Error reading from %s : %s\n", fn, strerror(errno));
				xfree(result); result = NULL;
				close(fd);
				return -1;
			}

			flen = st.st_size;
			add_filecache(fullfn, result, flen);
			close(fd);
		}
		else {
			errprintf("Impossible - cannot fstat() an open file ..\n");
			close(fd);
			return -1;
		}
	}

	msg->buflen = flen;
	msg->buf = result;
	msg->bufp = msg->buf + msg->buflen;

	dbgprintf("<- get_binary\n");

	return 0;
}

/*
 * This routine is used when adding multiple timestamp strings
 * in one addtobuffer() call. This must avoid overwriting the
 * same static buffer, so we have a fixed set of available
 * buffers that we use.
 * Hence you must do a "timestr(-999)" when done with the
 * multi-buffer call.
 */
char *timestr(time_t tstamp)
{
	static char *result[10] = { NULL, };
	static int residx = -1;
	char *p;

	if (tstamp < DISABLED_UNTIL_OK) {
		residx = -1;
	}
	else if (tstamp == DISABLED_UNTIL_OK) {
		return "Until OK";
	}
	else if (tstamp == 0) {
		return "N/A";
	}
	else {
		if (result[++residx] == NULL) result[residx] = (char *)malloc(30);
		strcpy(result[residx], ctime(&tstamp));
		p = strchr(result[residx], '\n'); if (p) *p = '\0';
	}

	return result[residx];
}


int parseinequality(char *str, unsigned int *flags)
{
	if (strncmp(str, ">=", 2) == 0) {
		*(flags) |= COMPARE_GE;
		return 2;
	}
	else if (strncmp(str, "<=", 2) == 0) {
		*(flags) |= COMPARE_LE;
		return 2;
	}
	else if (strncmp(str, "!=", 2) == 0) {
		*(flags) |= COMPARE_NE;
		return 2;
	}
	else if (strncmp(str, ">", 1) == 0) {
		*(flags) |= COMPARE_GT;
		return 1;
	}
	else if (strncmp(str, "<", 1) == 0) {
		*(flags) |= COMPARE_LT;
		return 1;
	}
	else if (strncmp(str, "=", 1) == 0) {
		*(flags) |= COMPARE_EQ;
		return 1;
	}
	else errprintf("Missing comparison operator: %s\n", str);
	return 0;
}

hostfilter_rec_t *setup_filter(char *buf, char **fields, int *acklevel, int *havehostfilter)
{
	char *tok;
	hostfilter_rec_t *filterhead = NULL, *filtertail = NULL;
	static pcre *xmhptn = NULL;

	dbgprintf("-> setup_filter: %s\n", buf);

	if (!xmhptn) xmhptn = compileregex("^(XMH_.*)=(.*)");

	*fields = NULL;

	tok = strtok(buf, " \t\r\n");
	if (tok) tok = strtok(NULL, " \t\r\n");
	while (tok) {
		/* Get filter */
		hostfilter_rec_t *newrec = NULL;
		char *xmhfld = NULL, *xmhval = NULL;

		if ((strncmp(tok, "XMH_", 4) == 0) && pickdata(tok, xmhptn, 1, &xmhfld, &xmhval)) {
			enum xmh_item_t fld = xmh_key_idx(xmhfld);

			if ((fld != XMH_LAST) && (*xmhfld) && (*xmhval)) {
				newrec = (hostfilter_rec_t *)calloc(1, sizeof(hostfilter_rec_t));
				newrec->filtertype = FILTER_XMH;
				newrec->field = fld;
				newrec->wantedptn = compileregex(xmhval);
			}
			xfree(xmhfld); xfree(xmhval);
		}
		else if ((strncmp(tok, "page=", 5) == 0) && (*(tok+5))) {
			newrec = (hostfilter_rec_t *)calloc(1, sizeof(hostfilter_rec_t));
			newrec->filtertype = FILTER_PAGEPATH;
			newrec->wantedptn = compileregex(tok+5);
		}
		else if ((strncmp(tok, "host=", 5) == 0) && (*(tok+5))) {
			newrec = (hostfilter_rec_t *)calloc(1, sizeof(hostfilter_rec_t));
			newrec->filtertype = FILTER_XMH;
			newrec->field = XMH_HOSTNAME;
			newrec->wantedptn = compileregex(tok+5);
			newrec->handle = xtreeFind(rbhosts, tok+5);
		}
		else if ((strncmp(tok, "net=", 4) == 0) && (*(tok+4))) {
			newrec = (hostfilter_rec_t *)calloc(1, sizeof(hostfilter_rec_t));
			newrec->filtertype = FILTER_XMH;
			newrec->field = XMH_NET;
			newrec->wantedptn = compileregex(tok+4);
		}
		else if ((strncmp(tok, "ip=", 3) == 0) && (*(tok+3))) {
			newrec = (hostfilter_rec_t *)calloc(1, sizeof(hostfilter_rec_t));
			newrec->filtertype = FILTER_XMH;
			newrec->field = XMH_IP;
			newrec->wantedptn = compileregex(tok+3);
		}
		else if ((strncmp(tok, "lastchange", 10) == 0) && (*(tok+10))) {
			int skipchar;
			newrec = (hostfilter_rec_t *)calloc(1, sizeof(hostfilter_rec_t));
			newrec->filtertype = FILTER_FIELDTIME;
			newrec->boardfield = F_LASTCHANGE;
			skipchar = parseinequality(tok+10, &newrec->flags);
			if (skipchar) newrec->wantedvalue = atoi(tok+skipchar+10);
			else xfree(newrec);
		}
		else if ((strncmp(tok, "logtime", 7) == 0) && (*(tok+7))) {
			int skipchar;
			newrec = (hostfilter_rec_t *)calloc(1, sizeof(hostfilter_rec_t));
			newrec->filtertype = FILTER_FIELDTIME;
			newrec->boardfield = F_LOGTIME;
			skipchar = parseinequality(tok+7, &newrec->flags);
			if (skipchar) newrec->wantedvalue = atoi(tok+skipchar+7);
			else xfree(newrec);
		}
		else if ((strncmp(tok, "validtime", 9) == 0) && (*(tok+9))) {
			int skipchar;
			newrec = (hostfilter_rec_t *)calloc(1, sizeof(hostfilter_rec_t));
			newrec->filtertype = FILTER_FIELDTIME;
			newrec->boardfield = F_VALIDTIME;
			skipchar = parseinequality(tok+9, &newrec->flags);
			if (skipchar) newrec->wantedvalue = atoi(tok+skipchar+9);
			else xfree(newrec);
		}
		else if ((strncmp(tok, "acktime", 7) == 0) && (*(tok+7))) {
			int skipchar;
			newrec = (hostfilter_rec_t *)calloc(1, sizeof(hostfilter_rec_t));
			newrec->filtertype = FILTER_FIELDTIME;
			newrec->boardfield = F_ACKTIME;
			skipchar = parseinequality(tok+7, &newrec->flags);
			if (skipchar) newrec->wantedvalue = atoi(tok+skipchar+7);
			else xfree(newrec);
		}
		else if ((strncmp(tok, "disabletime", 11) == 0) && (*(tok+11))) {
			int skipchar;
			newrec = (hostfilter_rec_t *)calloc(1, sizeof(hostfilter_rec_t));
			newrec->filtertype = FILTER_FIELDTIME;
			newrec->boardfield = F_DISABLETIME;
			skipchar = parseinequality(tok+11, &newrec->flags);
			if (skipchar) newrec->wantedvalue = atoi(tok+skipchar+11);
			else xfree(newrec);
		}
		else if ((strncmp(tok, "msg=", 4) == 0) && (*(tok+4))) {
			newrec = (hostfilter_rec_t *)calloc(1, sizeof(hostfilter_rec_t));
			newrec->filtertype = FILTER_FIELD;
			newrec->boardfield = F_MSG;
			newrec->wantedptn = compileregex(tok+4);
		}
		else if ((strncmp(tok, "ackmsg=", 7) == 0) && (*(tok+7))) {
			newrec = (hostfilter_rec_t *)calloc(1, sizeof(hostfilter_rec_t));
			newrec->filtertype = FILTER_FIELD;
			newrec->boardfield = F_ACKMSG;
			newrec->wantedptn = compileregex(tok+7);
		}
		else if ((strncmp(tok, "dismsg=", 7) == 0) && (*(tok+7))) {
			newrec = (hostfilter_rec_t *)calloc(1, sizeof(hostfilter_rec_t));
			newrec->filtertype = FILTER_FIELD;
			newrec->boardfield = F_DISMSG;
			newrec->wantedptn = compileregex(tok+7);
		}
		else if ((strncmp(tok, "test=", 5) == 0) && (*(tok+5))) {
			newrec = (hostfilter_rec_t *)calloc(1, sizeof(hostfilter_rec_t));
			newrec->filtertype = FILTER_TEST;
			newrec->wantedptn = compileregex(tok+5);
			newrec->handle = xtreeFind(rbtests, tok+5);
		}
		else if ((strncmp(tok, "tag=", 4) == 0) && (*(tok+4))) {
			newrec = (hostfilter_rec_t *)calloc(1, sizeof(hostfilter_rec_t));
			newrec->filtertype = FILTER_TAG;
			newrec->wantedptn = compileregex(tok+4);
			newrec->handle = xtreeFind(rbtests, tok+4);
		}
		else if ((strncmp(tok, "color=", 6) == 0) && (*(tok+6))) {
			char *col, *tokptr = NULL;

			newrec = (hostfilter_rec_t *)calloc(1, sizeof(hostfilter_rec_t));
			newrec->filtertype = FILTER_COLOR;
			col = strtok_r(tok+6, ",", &tokptr);
			while (col) {
				newrec->wantedvalue |= (1 << parse_color(col));
				col = strtok_r(NULL, ",", &tokptr);
			}
		}
		else if ((strncmp(tok, "acklevel=", 9) == 0) && (*(tok+9))) {
			newrec = (hostfilter_rec_t *)calloc(1, sizeof(hostfilter_rec_t));
			newrec->filtertype = FILTER_ACKLEVEL;
			newrec->wantedvalue = atoi(tok+9);
			if (*acklevel) *acklevel = newrec->wantedvalue;
		}
		else if (strncmp(tok, "notdown", 7) == 0) {
			newrec = (hostfilter_rec_t *)calloc(1, sizeof(hostfilter_rec_t));
			newrec->filtertype = FILTER_NOTDOWN;
			if ((*(tok+7) == '=') && (*(tok+8))) {
				newrec->wantedptn = compileregex(tok+8);
			}
			else {
				char *ptn = (char *)malloc(strlen(xgetenv("PINGCOLUMN")) + 3);
				sprintf(ptn, "^%s$", xgetenv("PINGCOLUMN"));
				newrec->wantedptn = compileregex(ptn);
				xfree(ptn);
			}
		}
		else if (strncmp(tok, "down", 4) == 0) {
			newrec = (hostfilter_rec_t *)calloc(1, sizeof(hostfilter_rec_t));
			newrec->filtertype = FILTER_DOWN;
			if ((*(tok+4) == '=') && (*(tok+5))) {
				newrec->wantedptn = compileregex(tok+5);
			}
			else {
				char *ptn = (char *)malloc(strlen(xgetenv("PINGCOLUMN")) + 3);
				sprintf(ptn, "^%s$", xgetenv("PINGCOLUMN"));
				newrec->wantedptn = compileregex(ptn);
				xfree(ptn);
			}
		}
		else if (strncmp(tok, "fields=", 7) == 0) {
			*fields = tok+7;
		}
		else {
			/* Might be an old-style HOST.TEST request */
			char *hname, *tname, *hostip = NULL;

			hname = tok;
			tname = strrchr(tok, '.');
			if (tname) { *tname = '\0'; tname++; }
			uncommafy(hname);
			hname = knownhost(hname, &hostip, ghosthandling);

			if (hname && tname) {
				newrec = (hostfilter_rec_t *)calloc(1, sizeof(hostfilter_rec_t));
				newrec->filtertype = FILTER_XMH;
				newrec->field = XMH_HOSTNAME;
				newrec->wantedptn = compileregex(hname);
				newrec->handle = xtreeFind(rbhosts, hname);

				newrec->next = (hostfilter_rec_t *)calloc(1, sizeof(hostfilter_rec_t));
				newrec->next->filtertype = FILTER_TEST;
				newrec->next->wantedptn = compileregex(tname);
				newrec->next->handle = xtreeFind(rbtests, tname);
			}
		}

		if (newrec) {
			if (filtertail) {
				filtertail->next = newrec; 
			}
			else {
				filterhead = filtertail = newrec;
			}
			while (filtertail->next) filtertail = filtertail->next;
		}

		tok = strtok(NULL, " \t\r\n");
	}

	if (havehostfilter) {
		hostfilter_rec_t *fwalk = filterhead;
	
		*havehostfilter = 0;
		while (fwalk && !(*havehostfilter)) { 
			*havehostfilter = ((fwalk->filtertype == FILTER_XMH) && (fwalk->field == XMH_HOSTNAME)); 
			fwalk = fwalk->next;
		}
	}

	dbgprintf("<- setup_filter: %s\n", buf);

	return filterhead;
}


boardfield_t *setup_fields(char *fieldstr)
{
	char *s, *tok;
	int tsize = 0;
	boardfield_t *boardfields = NULL;

	boardfields = (boardfield_t *)calloc(++tsize, sizeof(boardfield_t));

	s = strdup(fieldstr);
	tok = strtok(s, ",");
	while (tok) {
		enum boardfield_t fieldid = F_LAST;
		enum xmh_item_t xmhfieldid = XMH_LAST;
		int validfield = 1;

		if (strncmp(tok, "BBH_", 4) == 0) memmove(tok, "XMH_", 4);	/* For compatibility */

		if (strncmp(tok, "XMH_", 4) == 0) {
			fieldid = F_HOSTINFO;
			xmhfieldid = xmh_key_idx(tok);
			validfield = (xmhfieldid != XMH_LAST);
		}
		else {
			int i;
			for (i=0; (boardfieldnames[i].name && strcmp(tok, boardfieldnames[i].name)); i++) ;
			if (boardfieldnames[i].name) {
				fieldid = boardfieldnames[i].id;
				xmhfieldid = XMH_LAST;
			}
		}

		if ((fieldid != F_LAST) && validfield) {
			boardfields[tsize-1].field = fieldid;
			boardfields[tsize-1].xmhfield = xmhfieldid;
			boardfields = (boardfield_t *)realloc(boardfields, (++tsize)*sizeof(boardfield_t));
		}

		tok = strtok(NULL, ",");
	}

	boardfields[tsize-1].field = F_NONE;
	boardfields[tsize-1].xmhfield = XMH_LAST;

	xfree(s);

	return boardfields;
}

void clear_filter(hostfilter_rec_t *filter)
{
	hostfilter_rec_t *fwalk, *zombie;

	fwalk = filter;
	while (fwalk) {
		zombie = fwalk; fwalk = fwalk->next;
		if (zombie->wantedptn) freeregex(zombie->wantedptn);
		xfree(zombie);
	}
}

int match_host_filter(void *hinfo, hostfilter_rec_t *filter, int matchontests, char ***tags)
{
	int matched = 1, downcount, matchcount;
	hostfilter_rec_t *fwalk;
	char *val;
	xtreePos_t hosthandle;
	xymond_hostlist_t *hwalk;
	xymond_log_t *lwalk;
	char **taglist = NULL;
	int taglistsz = 0;

	if (tags) *tags = NULL;

	if (!filter) return 1;	/* Empty filter matches all */
	fwalk = filter;
	while (fwalk && matched) {
		switch (fwalk->filtertype) {
		  case FILTER_XMH:
			val = xmh_item(hinfo, fwalk->field);
			matched = (val ? matchregex(val, fwalk->wantedptn) : 0);
			break;

		  case FILTER_PAGEPATH:
			matched = 0;
			val = xmh_item_multi(hinfo, XMH_PAGEPATH);
			while (val && !matched) {
				matched = matchregex(val, fwalk->wantedptn);
				val = xmh_item_multi(NULL, XMH_PAGEPATH);
			}
			break;

		  case FILTER_TAG:
			if (!matchontests) break;	/* When processing a xymondboard, we don't want to return entries that have a test in hosts.cfg but no test result */
			matched = 0;
			val = xmh_item_walk(hinfo);
			while (val) {
				if (matchregex(val, fwalk->wantedptn)) {
					matched = 1;
					if (tags) {
						taglistsz++;
						taglist = (char **)realloc(taglist, (taglistsz+1)*sizeof(char *));
						taglist[taglistsz-1] = val;
						taglist[taglistsz] = NULL;
					}
				}
				val = xmh_item_walk(NULL);
			}
			break;

		  case FILTER_DOWN:
			/* Want only hosts that are down */
			hosthandle = xtreeFind(rbhosts, xmh_item(hinfo, XMH_HOSTNAME));
			hwalk = xtreeData(rbhosts, hosthandle);
			if (!hwalk) continue;
			for (lwalk = hwalk->logs, downcount = 0, matchcount = 0; (lwalk); lwalk = lwalk->next) {
				if (matchregex(lwalk->test->name, fwalk->wantedptn)) {
					matchcount++;
					if (lwalk->color == COL_RED) {
						downcount++;
						if (tags) {
							taglistsz++;
							taglist = (char **)realloc(taglist, (taglistsz+1)*sizeof(char *));
							taglist[taglistsz-1] = lwalk->test->name;
							taglist[taglistsz] = NULL;
						}
					}
				}
			}
			if ((matchcount <= 0) || (downcount < matchcount)) matched = 0;
			break;

		  case FILTER_NOTDOWN:
			/* Dont want hosts where a service is down. Any test matching the pattern and is red will trigger this host not being matched */
			hosthandle = xtreeFind(rbhosts, xmh_item(hinfo, XMH_HOSTNAME));
			hwalk = xtreeData(rbhosts, hosthandle);
			if (!hwalk) continue;
			for (lwalk = hwalk->logs, downcount = 0, matchcount = 0; (lwalk); lwalk = lwalk->next) {
				if (matchregex(lwalk->test->name, fwalk->wantedptn)) {
					matchcount++;
					if (lwalk->color == COL_RED) {
						downcount++;
						if (tags) {
							taglistsz++;
							taglist = (char **)realloc(taglist, (taglistsz+1)*sizeof(char *));
							taglist[taglistsz-1] = lwalk->test->name;
							taglist[taglistsz] = NULL;
						}
					}
				}
			}
			if (downcount > 0) matched = 0;
			break;

		  default:
			break;
		}

		fwalk = fwalk->next;
	}

	if (taglist && tags) *tags = taglist;
	return matched;
}

int match_test_filter(xymond_log_t *log, hostfilter_rec_t *filter)
{
	char *testedstr;
	int testedval;
	int matched = 1;
	hostfilter_rec_t *fwalk;

	if (!filter) return 1;	/* Empty filter matches all */
	fwalk = filter;
	while (fwalk && matched) {
		switch (fwalk->filtertype) {
		  case FILTER_TEST:
			matched = matchregex(log->test->name, fwalk->wantedptn);
			break;

		  case FILTER_COLOR:
			matched = ( ((1 << log->color) & fwalk->wantedvalue) != 0);
			break;

		  case FILTER_NOTDOWN:
			matched = (log->color != COL_RED);
			break;

		  case FILTER_DOWN:
			matched = (log->color == COL_RED);
			break;

		  case FILTER_FIELDTIME:
			switch (fwalk->boardfield) {
			  case F_LASTCHANGE: testedval = log->lastchange[0]; break;
			  case F_LOGTIME: testedval = log->logtime; break;
			  case F_VALIDTIME: testedval = log->validtime; break;
			  case F_ACKTIME: testedval = log->acktime; break;
			  case F_DISABLETIME: testedval = log->enabletime; break;
			  default: errprintf("Unknown FILTER_FIELDTIME field given\n"); testedval = -2; break;
			}
			matched = (testedval != -2) ? 
				( 	((fwalk->flags & COMPARE_GT) && (testedval >  fwalk->wantedvalue)) ||
					((fwalk->flags & COMPARE_GE) && (testedval >= fwalk->wantedvalue)) ||
					((fwalk->flags & COMPARE_LT) && (testedval <  fwalk->wantedvalue)) ||
					((fwalk->flags & COMPARE_LE) && (testedval <= fwalk->wantedvalue)) ||
					((fwalk->flags & COMPARE_EQ) && (testedval == fwalk->wantedvalue)) ||
					((fwalk->flags & COMPARE_NE) && (testedval != fwalk->wantedvalue)) ) : 0;
			break;

		  case FILTER_FIELD:
			switch (fwalk->boardfield) {
			  case F_MSG: testedstr = log->message; break;
			  case F_ACKMSG: testedstr = log->ackmsg; break;
			  case F_DISMSG: testedstr = log->dismsg; break;
			  default: errprintf("Unknown FILTER_FIELD field given\n"); testedstr = NULL; break;
			}
			matched = (testedstr ? matchregex(testedstr, fwalk->wantedptn) : 0 );
			break;

		  default:
			break;
		}

		fwalk = fwalk->next;
	}

	return matched;
}


strbuffer_t *generate_outbuf(strbuffer_t **prebuf, boardfield_t *boardfields, xymond_hostlist_t *hwalk, xymond_log_t *lwalk, int acklevel)
{
	int f_idx;
	enum boardfield_t f_type;
	strbuffer_t *buf;
	char *eoln;
	void *hinfo = NULL;
	char *acklist = NULL;
	modifier_t *mwalk;
	time_t timeroffset = (getcurrenttime(NULL) - gettimer());
	char l[1024];

	buf = (prebuf ? *prebuf : newstrbuffer(0));

	/* Now generate the data */
	for (f_idx = 0, f_type = boardfields[0].field; ((f_type != F_NONE) && (f_type != F_LAST)); f_type = boardfields[++f_idx].field) {
		if ((lwalk == NULL) && (f_type != F_HOSTINFO)) continue;
		if (f_idx > 0) addtobuffer(buf, "|");

		switch (f_type) {
		  case F_NONE: break;
		  case F_IP: snprintf(l, sizeof(l), "%s", hwalk->ip); addtobuffer(buf, l); break;
		  case F_HOSTNAME: addtobuffer(buf, hwalk->hostname); break;
		  case F_TESTNAME: addtobuffer(buf, lwalk->test->name); break;
		  case F_COLOR: addtobuffer(buf, colnames[lwalk->color]); break;
		  case F_FLAGS: if (lwalk->testflags) addtobuffer(buf, lwalk->testflags); break;
		  case F_LASTCHANGE: snprintf(l, sizeof(l), "%d", (int)lwalk->lastchange[0]); addtobuffer(buf, l); break;
		  case F_LOGTIME: snprintf(l, sizeof(l), "%d", (int)lwalk->logtime); addtobuffer(buf, l); break;
		  case F_VALIDTIME: snprintf(l, sizeof(l), "%d", (int)lwalk->validtime); addtobuffer(buf, l); break;
		  case F_ACKTIME: snprintf(l, sizeof(l), "%d", (int)lwalk->acktime); addtobuffer(buf, l); break;
		  case F_DISABLETIME: snprintf(l, sizeof(l), "%d", (int)lwalk->enabletime); addtobuffer(buf, l); break;
		  case F_SENDER: addtobuffer(buf, lwalk->sender); break;
		  case F_COOKIE: if (lwalk->cookie) addtobuffer(buf, lwalk->cookie); break;
		  case F_LINE1: addtobuffer(buf, lwalk->line1); break;
		  case F_ACKMSG: if (lwalk->ackmsg) addtobuffer(buf, nlencode(lwalk->ackmsg)); break;
		  case F_DISMSG: if (lwalk->dismsg) addtobuffer(buf, nlencode(lwalk->dismsg)); break;
		  case F_MSG: addtobuffer(buf, nlencode(lwalk->message)); break;
		  case F_CLIENT: addtobuffer(buf, (hwalk->clientmsgs ? "Y" : "N")); break;
		  case F_CLIENTTSTAMP: snprintf(l, sizeof(l), "%ld", (hwalk->clientmsgs ? (long) (hwalk->clientmsgtstamp + timeroffset) : 0)); addtobuffer(buf, l); break;

		  case F_ACKLIST: 
			flush_acklist(lwalk, 0);
			acklist = acklist_string(lwalk, acklevel);
		        if (acklist) addtobuffer(buf, nlencode(acklist));
			break;

		  case F_HOSTINFO:
			hinfo = hostinfo(hwalk->hostname);
			if (hinfo) {
				char *infostr = xmh_item(hinfo, boardfields[f_idx].xmhfield);
				if (infostr) addtobuffer(buf, infostr);
			}
			break;

		  case F_FLAPINFO:
			snprintf(l, sizeof(l), "%d/%ld/%ld/%s/%s", 
				 lwalk->flapping, 
				 lwalk->lastchange[0], (flapcount > 0) ? lwalk->lastchange[flapcount-1] : 0,
				 colnames[lwalk->oldflapcolor], colnames[lwalk->currflapcolor]);
			addtobuffer(buf, l);
			break;

		  case F_STATS:
			snprintf(l, sizeof(l), "statuschanges=%lu", lwalk->statuschangecount);
			addtobuffer(buf, l);
			break;

		  case F_MODIFIERS:
			if (lwalk->modifiers) addtobuffer(buf, lwalk->modifierbuf);
			break;

		  case F_MATCHEDTAG: break;
		  case F_LAST: break;
		}
	}
	addtobuffer(buf, "\n");

	return buf;
}


strbuffer_t *generate_hostinfo_outbuf(strbuffer_t **prebuf, boardfield_t *boardfields, void *hinfo, char **tags)
{
	int f_idx, tagi;
	strbuffer_t *buf;
	char l[1024];
	char *infostr = NULL;

	buf = (prebuf ? *prebuf : newstrbuffer(0));

	for (f_idx = 0; (boardfields[f_idx].field != F_NONE); f_idx++) {
		if (f_idx > 0) addtobuffer(buf, "|");

		switch (boardfields[f_idx].field) {
		  case F_HOSTINFO:
			infostr = xmh_item(hinfo, boardfields[f_idx].xmhfield);
			if (infostr) {
				if (boardfields[f_idx].xmhfield != XMH_RAW) infostr = nlencode(infostr);
				addtobuffer(buf, infostr);
			}
			break;

		  case F_MATCHEDTAG:
			if (tags) {
				tagi = 0;
				while (tags[tagi]) {
					if (tagi > 0) 
						addtobuffer_many(buf, "|", tags[tagi++], NULL);
					else
						addtobuffer(buf, tags[tagi++]);
				}
			}
			break;

		  default: break;
		}
	}

	addtobuffer(buf, "\n");
	return buf;
}


void get_sender(conn_t *msg, char *msgtext, char *prestring)
{
	char *msgfrom;

	if (msg->certcn) {
		msg->sender = strdup(msg->certcn);
		return;
	}

	if (msg->sender && (strcmp(msg->sender, "BFQ") == 0)) return;

	msgfrom = strstr(msgtext, prestring);
	if (msgfrom) {
		char *tokr, *s;

		if (msg->sender) xfree(msg->sender);
		s = strtok_r(msgfrom + strlen(prestring), " \r\n\t", &tokr);
		msg->sender = strdup(s ? s : "");
	}
	else {
		if (!msg->sender) msg->sender = strdup("");
	}
}


void do_message(conn_t *msg, char *origin, int viabfq)
{
	static int nesting = 0;
	xymond_hostlist_t *h;
	testinfo_t *t;
	xymond_log_t *log;
	int color;
	char *downcause;
	char *grouplist;
	time_t now, timeroffset;
	char *msgfrom;

	nesting++;
	if (debug) {
		char *eoln = strchr(msg->buf, '\n');

		if (eoln) *eoln = '\0';
		dbgprintf("-> do_message/%d (%d bytes): %s\n", nesting, msg->buflen, msg->buf);
		if (eoln) *eoln = '\n';
	}

	if (strncmp(msg->buf, "compress:", 9) == 0) {
		char *p = NULL;
		strbuffer_t *expbuf = NULL;
		enum compressiontype_t compressiontype = COMP_UNKNOWN;
		unsigned char *cbegin;
		size_t expandedsz = 0;

		p = msg->buf+9; if (!p) goto done;
		compressiontype = parse_compressiontype(p);
		if (compressiontype == COMP_UNKNOWN) {
			errprintf("Unsupported/unknown compression type in message; skipping\n");
			goto done;
		}
		p = strchr(msg->buf, ' '); if (!++p) goto done;		// skip to size
		expandedsz = (size_t)atol(p); dbgprintf(" - compressed message; expecting %zu bytes\n", expandedsz);

		cbegin = strchr(msg->buf, '\n');
		if (cbegin) {
			ptrdiff_t len;
			cbegin++;
			len = msg->buflen - (cbegin - msg->buf); // dbgprintf(" - data length: %td\n", len);

			// expbuf = uncompress_buffer(cbegin, len, NULL);
			expbuf = newstrbuffer(expandedsz + 2048); // safety
			// (void)uncompress_to_my_buffer(cbegin, len, expbuf);
			expbuf = uncompress_message(compressiontype, cbegin, len, expandedsz, expbuf, NULL);
		}

		if (expbuf && (STRBUFLEN(expbuf) == expandedsz)) {
			char *origbuf, *origbufp;
			size_t origbuflen, origbufsz;

			dbgprintf(" - compressed msg found, %zu bytes\n", expandedsz);
			origbuf = msg->buf; origbuflen = msg->buflen; origbufsz = msg->bufsz; origbufp = msg->bufp;

			msg->buflen = msg->bufsz = STRBUFLEN(expbuf);
			msg->buf = grabstrbuffer(expbuf);
			msg->bufp = msg->buf + msg->buflen; *(msg->bufp) = '\0';

			do_message(msg, origin, viabfq);

			xfree(msg->buf); // uncompress gives us a new buffer each time
			msg->buf=origbuf; msg->buflen=origbuflen; msg->bufsz=origbufsz; msg->bufp=origbufp;

			goto done;
		}
		else {
			errprintf("Garbled %s-compressed message: expected %zu bytes, but expanded to %zu bytes\n",
				  comptype2str(compressiontype), expandedsz, (expbuf ? STRBUFLEN(expbuf) : 0));
			if (expbuf != NULL) freestrbuffer(expbuf);
			goto done;
		}
	}

	/* Handle size:#### messages that have made it through (likely via compression) */
	if (strncmp(msg->buf, "size:", 5) == 0) {
		char *p = NULL;
		strbuffer_t *expbuf = NULL;
		size_t expandedsz = 0;
		unsigned char *cbegin;

		p = msg->buf+5; if (!p) goto done;
		p = strchr(msg->buf, ' '); if (!++p) goto done;		// skip to size
		expandedsz = (size_t)atol(p); dbgprintf(" - xymon v5 message came through; expecting %zu bytes\n", expandedsz);

		cbegin = strchr(msg->buf, '\n');
		if (cbegin) {
			ptrdiff_t len;
			cbegin++;
			len = msg->bufsz - (cbegin - msg->buf);

			if (len == expandedsz) {
				char *origbuf, *origbufp;
				size_t origbuflen, origbufsz;

				dbgprintf(" - valid xymond v5 msg found, %zu bytes\n", expandedsz);

				origbuf = msg->buf; origbuflen = msg->buflen; origbufsz = msg->bufsz; origbufp = msg->bufp;
				msg->buflen = msg->bufsz = len;
				msg->buf = cbegin;
				msg->bufp = msg->buf + msg->buflen; *(msg->bufp) = '\0';

				do_message(msg, origin, viabfq);

				msg->buf=origbuf; msg->buflen=origbuflen; msg->bufsz=origbufsz; msg->bufp=origbufp;
				goto done;
			}
			else {
				errprintf("Garbled xymon v5 message: expected %zu bytes, but got %zu bytes\n",
					  expandedsz, len);
				goto done;
			}
		}
	}

	/* Most likely, we will not send a response */
	msg->doingwhat = NOTALK;
	now = getcurrenttime(NULL);
	timeroffset = (getcurrenttime(NULL) - gettimer());

	/* Save sender statistics */
	{
		xtreePos_t handle;
		senderstats_t *rec;

		handle = xtreeFind(rbsenders, msg->sender);
		if (handle == xtreeEnd(rbsenders)) {
			rec = (senderstats_t *)malloc(sizeof(senderstats_t));
			rec->senderip = strdup(msg->sender);
			rec->msgcount = 0;
			xtreeAdd(rbsenders, rec->senderip, rec);
		}
		else {
			rec = (senderstats_t *)xtreeData(rbsenders, handle);
		}

		rec->msgcount++;
	}

	if (traceall || tracelist) {
		int found = 0;

		if (traceall) {
			found = 1;
		}
		else {
#if 0
			--- FIXME ---
			int i = 0;
			do {
				if ((tracelist[i].ipval & tracelist[i].ipmask) == (ntohl(msg->sender) & tracelist[i].ipmask)) {
					found = 1;
				}
				i++;
			} while (!found && (tracelist[i].ipval != 0));
#endif
		}

		if (found) {
			char tracefn[PATH_MAX];
			struct timeval tv;
			struct timezone tz;
			FILE *fd;

			gettimeofday(&tv, &tz);

			snprintf(tracefn, sizeof(tracefn), "%s/%d_%06d_%s.trace", xgetenv("XYMONTMP"), 
				(int) tv.tv_sec, (int) tv.tv_usec, msg->sender);
			fd = fopen(tracefn, "w");
			if (fd) {
				fwrite(msg->buf, msg->buflen, 1, fd);
				fclose(fd);
			}

			if (ignoretraced) goto done;
		}
	}

	/* Count statistics */
	update_statistics(msg->buf, viabfq);

	if (strncmp(msg->buf, "extcombo ", 9) == 0) {
		char *ofsline, *origbuf, *p, *ofsstr, *tokr = NULL;
		off_t startofs, endofs;

		origbuf = ofsline = msg->buf;
		p = strchr(ofsline, '\n');
		if (p) 
			*p = '\0';
		else {
			/* Abort */
			goto done;
		}

		ofsstr = strtok_r(ofsline+9, " ", &tokr);
		startofs = (off_t)atol(ofsstr);
		if ((startofs <= 0) || (startofs >= msg->bufsz)) {
			/* Invalid offsets, abort */
			errprintf("Invalid start-offset in extcombo: startofs=%d, bufsz=%d\n", startofs, msg->bufsz);
			goto done;
		}

		do {
			char savechar;

			ofsstr = strtok_r(NULL, " ", &tokr);
			if (!ofsstr) continue;

			endofs = (off_t)atol(ofsstr);
			if ((endofs <= 0) || (endofs <= startofs) || (endofs > msg->bufsz)) {
				/* Invalid offsets, abort */
				errprintf("Invalid end-offset in extcombo: endofs=%d, bufsz=%d\n", endofs, msg->bufsz);
				msg->buf = origbuf;
				goto done;
			}

			msg->buf = origbuf + startofs;
			msg->buflen = (endofs - startofs);
			savechar = *(msg->buf + msg->buflen);
			*(msg->buf + msg->buflen) = '\0';

			do_message(msg, origin, viabfq);
			*(msg->buf + msg->buflen) = savechar;
			startofs = endofs;
		} while (ofsstr);

		msg->buf = origbuf;
	}
	else if (strncmp(msg->buf, "combo\n", 6) == 0) {
		char *currmsg, *nextmsg;

		currmsg = msg->buf+6;
		do {
			int validsender = 1;

			nextmsg = strstr(currmsg, "\n\nstatus");
			if (nextmsg) { *(nextmsg+1) = '\0'; nextmsg += 2; }

			/* Pick out the real sender of this message */
			get_sender(msg, currmsg, "\nStatus message received from ");

			if (statussenders) {
				get_hts(currmsg, msg->sender, origin, &h, &t, &grouplist, &log, &color, &downcause, NULL, 0, 0);
				if (!viabfq && !oksender(statussenders, (h ? hostinfo(h->hostname) : NULL), msg->sender, currmsg)) validsender = 0;
			}

			if (validsender) {
				get_hts(currmsg, msg->sender, origin, &h, &t, &grouplist, &log, &color, &downcause, NULL, 1, 1);
				if (h && dbgfd && dbghost && (strcasecmp(h->hostname, dbghost) == 0)) {
					fprintf(dbgfd, "\n---- combo message from %s ----\n%s---- end message ----\n", msg->sender, currmsg);
					fflush(dbgfd);
				}

				switch (color) {
				  case COL_PURPLE:
					errprintf("Ignored PURPLE status update from %s for %s.%s\n",
						  msg->sender, (h ? h->hostname : "<unknown>"), (t ? t->name : "unknown"));
					break;

				  case COL_CLIENT:
					/* Pseudo color, allows us to send "client" data from a standard BB utility */
					/* In HOSTNAME.TESTNAME, the TESTNAME is used as the collector-ID */
					/* TODO: combine this and normal client parsing into a get_clientdata() routine */
					if (h) {
						char *line1, *p, *hostname, *collectorid, *clientos = '\0', *clientclass = '\0';
						char savech = '\0';

						/* Syntax: status HOSTNAME.COLLECTORID client SERVEROSTYPE CONFIGCLASS */
						p = currmsg + strcspn(currmsg, "\r\n");
						if ((*p == '\r') || (*p == '\n')) { savech = *p; *p = '\0'; }
						else p = NULL;
						line1 = strdup(currmsg); if (p) *p = savech;

						p = strtok(line1, " \t"); /* Skip the status keyword */
						if (!p) { errprintf("BUG: Didn't get a line1 parsing client 'status' msg\n"); xfree (line1); break; }

						hostname = strtok(NULL, " \t"); /* Actually, HOSTNAME.COLLECTORID */
						if (!hostname) { errprintf("BUG: Didn't get a hostname parsing client msg\n"); xfree(line1); break; }

						p = strtok(NULL, " \t"); /* Skip the client keyword */
						if (p != NULL) clientos = strtok(NULL, " \t"); /* Get the SERVEROSTYPE */
						if (clientos != NULL) clientclass = strtok(NULL, " \t"); /* Get the CONFIGCLASS */

						collectorid = strrchr(hostname, '.'); if (collectorid) { *collectorid = '\0'; collectorid++; }
						/* Backwards compat for previous RPM versions, which needed a fake "default" collector name */
						if (collectorid && ( (strcmp(collectorid,"")==0) || (strcmp(collectorid,"xymonclient")==0) || (strcmp(collectorid,xgetenv("CLIENTCOLUMN"))==0) ) ) { *collectorid = '\0'; }

						dbgprintf(" found a client message for %s, collector '%s', os '%s', class '%s'\n", h->hostname, collectorid, clientos, clientclass);
						handle_client(currmsg, msg->sender, h->hostname, (collectorid ? collectorid : ""), (clientos ? clientos : "") , (clientclass ? clientclass : ""));
						xfree(line1);
					}
					break;

				  default:
					/* Count individual status-messages also */
					update_statistics(currmsg, viabfq);

					if (h && t && log && (color != -1)) {
						handle_status(currmsg, msg->sender, h->hostname, t->name, grouplist, log, color, downcause, 0);
					}
					break;
				}
			}

			currmsg = nextmsg;
		} while (currmsg);
	}
	else if (strncmp(msg->buf, "combodata\n", 10) == 0) {
		char *currmsg, *nextmsg;

		char *hostname = NULL, *testname = NULL;
		char *bhost, *ehost, *btest;
		char savechar;

		currmsg = msg->buf+10;
		do {

			nextmsg = strstr(currmsg, "\n\ndata");
			if (nextmsg) { *(nextmsg+1) = '\0'; nextmsg += 2; }

			/* Pick out the real sender of this message */
			get_sender(msg, currmsg, "\nData message received from ");

			bhost = currmsg + strlen("data"); bhost += strspn(bhost, " \t");
			ehost = bhost + strcspn(bhost, " \t\r\n");
			savechar = *ehost; *ehost = '\0';

			btest = strrchr(bhost, '.');
			if (btest) {
				*btest = '\0';
				hostname = strdup(bhost);
				uncommafy(hostname);	/* For BB compatibility */
				*btest = '.';
				testname = strdup(btest+1);

				if (*hostname == '\0') { errprintf("Invalid data message from %s - blank hostname\n", msg->sender); xfree(hostname); hostname = NULL; }
				if (*testname == '\0') { errprintf("Invalid data message from %s - blank testname\n", msg->sender); xfree(testname); testname = NULL; }
			}
			else {
				errprintf("Invalid data message - no testname in '%s'\n", bhost);
			}

			*ehost = savechar;

			if (hostname && testname) {
				char *hname, *hostip = NULL;

				// MEMDEFINE(hostip);

				hname = knownhost(hostname, &hostip, ghosthandling);

				if (hname == NULL) {
					hname = log_ghost(hostname, msg->sender, currmsg);
				}

				if (hname == NULL) {
					/* Ignore it */
				}
				else if (!oksender(statussenders, hostip, msg->sender, currmsg)) {
					/* Invalid sender */
					errprintf("Invalid data message - sender %s not allowed for host %s\n", msg->sender, hostname);
				}
				else {
					handle_data(currmsg, msg->sender, origin, hname, testname);
				}

				xfree(hostname); xfree(testname);

				// MEMUNDEFINE(hostip);
			}

			currmsg = nextmsg;
		} while (currmsg);
	}
	else if (strncmp(msg->buf, "modify", 6) == 0) {
		char *currmsg, *nextmsg;

		currmsg = msg->buf;
		do {
			nextmsg = strstr(currmsg, "\n\nmodify");
			if (nextmsg) { *(nextmsg+1) = '\0'; nextmsg += 2; }

			get_hts(currmsg, msg->sender, origin, &h, &t, NULL, &log, &color, NULL, NULL, 0, 0);
			if (h && t && log && (viabfq || oksender(statussenders, (h ? hostinfo(h->hostname) : NULL), msg->sender, currmsg))) {
				handle_modify(currmsg, log, color, now);
			}

			currmsg = nextmsg;
		} while (currmsg);
	}
	else if (strncmp(msg->buf, "status", 6) == 0) {
		get_sender(msg, msg->buf, "\nStatus message received from ");

		if (statussenders) {
			get_hts(msg->buf, msg->sender, origin, &h, &t, &grouplist, &log, &color, &downcause, NULL, 0, 0);
			if (!viabfq && !oksender(statussenders, (h ? hostinfo(h->hostname) : NULL), msg->sender, msg->buf)) goto done;
		}

		get_hts(msg->buf, msg->sender, origin, &h, &t, &grouplist, &log, &color, &downcause, NULL, 1, 1);
		if (h && dbgfd && dbghost && (strcasecmp(h->hostname, dbghost) == 0)) {
			fprintf(dbgfd, "\n---- status message from %s ----\n%s---- end message ----\n", msg->sender, msg->buf);
			fflush(dbgfd);
		}

		switch (color) {
		  case COL_PURPLE:
			errprintf("Ignored PURPLE status update from %s for %s.%s\n",
				  msg->sender, (h ? h->hostname : "<unknown>"), (t ? t->name : "unknown"));
			break;

		  case COL_CLIENT:
			/* Pseudo color, allows us to send "client" data from a standard BB utility */
			/* In HOSTNAME.TESTNAME, the TESTNAME is used as the collector-ID */
			/* TODO: combine this and normal client parsing into a get_clientdata() routine */
			if (h) {
				char *line1, *p, *hostname, *collectorid, *clientos = '\0', *clientclass = '\0';
				char savech = '\0';

				/* Syntax: status HOSTNAME.COLLECTORID client SERVEROSTYPE CONFIGCLASS */
				p = msg->buf + strcspn(msg->buf, "\r\n");
				if ((*p == '\r') || (*p == '\n')) { savech = *p; *p = '\0'; }
				else p = NULL;
				line1 = strdup(msg->buf); if (p) *p = savech;

				p = strtok(line1, " \t"); /* Skip the status keyword */
				if (!p) { errprintf("BUG: Didn't get a line1 parsing client 'status' msg\n"); xfree(line1); break; }

				hostname = strtok(NULL, " \t"); /* Actually, HOSTNAME.COLLECTORID */
				if (!hostname) { errprintf("BUG: Didn't get a hostname parsing client msg\n"); xfree(line1); break; }

				p = strtok(NULL, " \t"); /* Skip the client keyword */
				if (p) clientos = strtok(NULL, " \t"); /* Get the SERVEROSTYPE */
				if (clientos) clientclass = strtok(NULL, " \t"); /* Get the CONFIGCLASS */

				collectorid = strrchr(hostname, '.'); if (collectorid) { *collectorid = '\0'; collectorid++; }
				/* Backwards compat for previous RPM versions, which needed a fake "default" collector name */
				if (collectorid && ( (strcmp(collectorid,"")==0) || (strcmp(collectorid,"xymonclient")==0) || (strcmp(collectorid,xgetenv("CLIENTCOLUMN"))==0) ) ) { *collectorid = '\0'; }

				dbgprintf(" found a client message for %s, collector '%s', os '%s', class '%s'\n", h->hostname, collectorid, clientos, clientclass);
				handle_client(msg->buf, msg->sender, h->hostname, (collectorid ? collectorid : ""), (clientos ? clientos : "") , (clientclass ? clientclass : ""));
				xfree(line1);
			}
			break;

		  default:
			if (h && t && log && (color != -1)) {
				handle_status(msg->buf, msg->sender, h->hostname, t->name, grouplist, log, color, downcause, 0);
			}
			break;
		}
	}
	else if (strncmp(msg->buf, "data", 4) == 0) {
		char *hostname = NULL, *testname = NULL;
		char *bhost, *ehost, *btest;
		char savechar;

		get_sender(msg, msg->buf, "\nStatus message received from ");

		bhost = msg->buf + strlen("data"); bhost += strspn(bhost, " \t");
		ehost = bhost + strcspn(bhost, " \t\r\n");
		savechar = *ehost; *ehost = '\0';

		btest = strrchr(bhost, '.');
		if (btest) {
			*btest = '\0';
			hostname = strdup(bhost);
			uncommafy(hostname);	/* For BB compatibility */
			*btest = '.';
			testname = strdup(btest+1);

			if (*hostname == '\0') { errprintf("Invalid data message from %s - blank hostname\n", msg->sender); xfree(hostname); hostname = NULL; }
			if (*testname == '\0') { errprintf("Invalid data message from %s - blank testname\n", msg->sender); xfree(testname); testname = NULL; }
		}
		else {
			errprintf("Invalid data message - no testname in '%s'\n", bhost);
		}

		*ehost = savechar;

		if (hostname && testname) {
			char *hname, *hostip = NULL;

			hname = knownhost(hostname, &hostip, ghosthandling);

			if (hname == NULL) {
				hname = log_ghost(hostname, msg->sender, msg->buf);
			}

			if (hname == NULL) {
				/* Ignore it */
			}
			else if (!viabfq && !oksender(statussenders, hostinfo(hname), msg->sender, msg->buf)) {
				/* Invalid sender */
				errprintf("Invalid data message - sender %s not allowed for host %s\n", msg->sender, hostname);
			}
			else {
				handle_data(msg->buf, msg->sender, origin, hname, testname);
			}

			xfree(hostname); xfree(testname);
		}
	}
	else if (strncmp(msg->buf, "summary", 7) == 0) {
		/* Summaries are always allowed. Or should we ? */
		get_hts(msg->buf, msg->sender, origin, &h, &t, NULL, &log, &color, NULL, NULL, 1, 1);
		if (h && t && log && (color != -1)) {
			handle_status(msg->buf, msg->sender, h->hostname, t->name, NULL, log, color, NULL, 0);
		}
	}
	else if ((strncmp(msg->buf, "notes", 5) == 0) || (strncmp(msg->buf, "usermsg", 7) == 0)) {
		char *id = NULL;

		{
			/* Get the message ID */
			char *bid, *eid;
			char savechar;

			bid = msg->buf + strcspn(msg->buf, " \t\r\n"); bid += strspn(bid, " \t");
			eid = bid + strcspn(bid, " \t\r\n");
			savechar = *eid; *eid = '\0';
			id = strdup(bid);
			*eid = savechar;

			uncommafy(id);	/* For BB compatibility */
		}

		/* 
		 * We don't validate the ID, because "notes" may also send messages
		 * for documenting pages or column-names. And the "usermsg" stuff can be
		 * anything in the "ID" field. So we just insist that there IS an ID.
		 */
		if (*id) {
			if (*msg->buf == 'n') {
				/* "notes" message */
				if (!viabfq && !oksender(maintsenders, NULL, msg->sender, msg->buf)) {
					/* Invalid sender */
					errprintf("Invalid notes message - sender %s not allowed for host %s\n", 
						  msg->sender, id);
				}
				else {
					handle_notes(msg->buf, msg->sender, id);
				}
			}
			else if (*msg->buf == 'u') {
				/* "usermsg" message */
				if (!viabfq && !oksender(statussenders, NULL, msg->sender, msg->buf)) {
					/* Invalid sender */
					errprintf("Invalid user message - sender %s not allowed for host %s\n", 
						  msg->sender, id);
				}
				else {
					handle_usermsg(msg->buf, msg->sender, id);
				}
			}
		}
		else {
			errprintf("Invalid notes/user message from %s - blank ID\n", msg->sender); 
		}

		xfree(id);
	}
	else if (strncmp(msg->buf, "enable", 6) == 0) {
		handle_enadis(1, msg, msg->sender, viabfq);
	}
	else if (strncmp(msg->buf, "disable", 7) == 0) {
		handle_enadis(0, msg, msg->sender, viabfq);
	}
	else if (strncmp(msg->buf, "config hosts.cfg", 16) == 0) {
		char *conffn, *p;

		if (viabfq || !oksender(statussenders, NULL, msg->sender, msg->buf)) goto done;

		/* This is special-cased, even if our actual hostfn is not "hosts.cfg" */
		/* Send to get_hostsconfig, which caches until our next reloadconfig time */
		p = msg->buf + 6; p += strspn(p, " \t");
		p = strtok(p, " \t\r\n");
		conffn = strdup(p);
		xfree(msg->buf);
		if (conffn && (strstr(conffn, "../") == NULL) && (get_hostsconfig(conffn, msg) == 0) ) {
			msg->doingwhat = RESPONDING;
			msg->bufp = msg->buf;
		}
		xfree(conffn);
	}
	else if (strncmp(msg->buf, "config", 6) == 0) {
		char *conffn, *p;

		if (viabfq || !oksender(statussenders, NULL, msg->sender, msg->buf)) goto done;

		p = msg->buf + 6; p += strspn(p, " \t");
		p = strtok(p, " \t\r\n");
		conffn = strdup(p);
		xfree(msg->buf);
		if (conffn && (strstr(conffn, "../") == NULL) && (get_config(conffn, msg) == 0) ) {
			msg->doingwhat = RESPONDING;
			msg->bufp = msg->buf;
		}
		xfree(conffn);
	}
	else if (allow_downloads && (strncmp(msg->buf, "download", 8) == 0)) {
		char *fn, *p;

		if (viabfq || !oksender(statussenders, NULL, msg->sender, msg->buf)) goto done;

		p = msg->buf + 8; p += strspn(p, " \t");
		p = strtok(p, " \t\r\n");
		fn = strdup(p);
		xfree(msg->buf);
		if (fn && (strstr(fn, "../") == NULL) && (get_binary(fn, msg) == 0) ) {
			msg->doingwhat = RESPONDING;
			msg->bufp = msg->buf;
		}
		xfree(fn);
	}
	else if (strncmp(msg->buf, "flush filecache", 15) == 0) {
		flush_filecache();
	}
	else if ( (strcmp(msg->buf, "reload") == 0) || (strcmp(msg->buf, "rotate") == 0) ) {
		posttoall(msg->buf);
	}
	else if (strncmp(msg->buf, "query ", 6) == 0) {
		get_hts(msg->buf, msg->sender, origin, &h, &t, NULL, &log, &color, NULL, NULL, 0, 0);
		if (viabfq || !oksender(statussenders, (h ? hostinfo(h->hostname) : NULL), msg->sender, msg->buf)) goto done;

		if (log) {
			xfree(msg->buf);
			msg->doingwhat = RESPONDING;
			if (log->message) {
				unsigned char *bol, *eoln;
				int msgcol;
				char response[500];

				bol = log->line1;
				msgcol = parse_color(bol);
				if (msgcol != -1) {
					/* Skip the color - it may be different in real life */
					bol += strlen(colorname(msgcol));
					bol += strspn(bol, " \t");
				}
				eoln = strchr(bol, '\n'); if (eoln) *eoln = '\0';
				snprintf(response, sizeof(response), "%s %s\n", colorname(log->color), bol);
				response[sizeof(response)-1] = '\0';
				if (eoln) *eoln = '\n';

				msg->buf = msg->bufp = strdup(response);
				msg->buflen = strlen(msg->buf);
			}
			else {
				msg->buf = msg->bufp = strdup("");
				msg->buflen = 0;
			}
		}
	}
	else if ((strncmp(msg->buf, "xymondlog ", 10) == 0) || (strncmp(msg->buf, "hobbitdlog ", 11) == 0)) {
		/* 
		 * Request for a single status log
		 * xymondlog HOST.TEST [fields=FIELDLIST]
		 *
		 */
		hostfilter_rec_t *logfilter;
		boardfield_t *logfields;
		char *fields;
		int acklevel = -1;

		if (viabfq || !oksender(wwwsenders, NULL, msg->sender, msg->buf)) goto done;

		logfilter = setup_filter(msg->buf, &fields, &acklevel, NULL);
		if (!fields) fields = "hostname,testname,color,flags,lastchange,logtime,validtime,acktime,disabletime,sender,cookie,ackmsg,dismsg,client,modifiers";
		logfields = setup_fields(fields);

		log = find_log(logfilter, &h);
		if (log) {
			strbuffer_t *logdata;

			flush_acklist(log, 0);
			if (log->message == NULL) {
				errprintf("%s.%s has a NULL message\n", log->host->hostname, log->test->name);
				log->message = strdup("No data");
				log->msgsz = strlen(log->message) + 1;
			}

			xfree(msg->buf);
			logdata = generate_outbuf(NULL, logfields, h, log, acklevel);
			addtobuffer(logdata, msg_data(log->message, 0));

			msg->doingwhat = RESPONDING;
			msg->buflen = STRBUFLEN(logdata);
			msg->bufp = msg->buf = grabstrbuffer(logdata);
		}

		clear_filter(logfilter);
		xfree(logfields);
	}
	else if ((strncmp(msg->buf, "xymondxlog ", 11) == 0) || (strncmp(msg->buf, "hobbitdxlog ", 12) == 0)) {
		/* 
		 * Request for a single status log in XML format
		 * xymondxlog HOST.TEST
		 *
		 */
		if (viabfq || !oksender(wwwsenders, NULL, msg->sender, msg->buf)) goto done;

		get_hts(msg->buf, msg->sender, origin, &h, &t, NULL, &log, &color, NULL, NULL, 0, 0);
		if (log) {
			strbuffer_t *response = newstrbuffer(0);

			flush_acklist(log, 0);
			if (log->message == NULL) {
				errprintf("%s.%s has a NULL message\n", log->host->hostname, log->test->name);
				log->message = strdup("No data");
				log->msgsz = strlen(log->message) + 1;
			}

			addtobuffer_many(response, 
				"<?xml version='1.0' encoding='ISO-8859-1'?>\n",
				"<ServerStatus>\n",
				"  <ServerName>", 	h->hostname, 				"</ServerName>\n",
				"  <Type>", 		log->test->name, 			"</Type>\n",
				"  <Status>", 		colnames[log->color], 			"</Status>\n",
				"  <TestFlags>", 	(log->testflags ? log->testflags : ""), "</TestFlags>\n",
				"  <LastChange>", 	timestr(log->lastchange[0]), 		"</LastChange>\n",
				"  <LogTime>", 		timestr(log->logtime), 			"</LogTime>\n",
				"  <ValidTime>", 	timestr(log->validtime), 		"</ValidTime>\n",
				"  <AckTime>", 		timestr(log->acktime), 			"</AckTime>\n",
				"  <DisableTime>", 	timestr(log->enabletime), 		"</DisableTime>\n",
				"  <Sender>",           log->sender,                            "</Sender>\n", 
				NULL);
			timestr(-999);

			if (log->cookie && (log->cookieexpires > now))
				addtobuffer_many(response, "  <Cookie>", log->cookie, "</Cookie>\n", NULL);
			else
				addtobuffer(response, "  <Cookie>N/A</Cookie>\n");

			if (log->ackmsg && (log->acktime > now))
				addtobuffer_many(response, "  <AckMsg><![CDATA[", log->ackmsg, "]]></AckMsg>\n", NULL);
			else
				addtobuffer(response, "  <AckMsg>N/A</AckMsg>\n");

			if (log->dismsg && (log->enabletime > now))
				addtobuffer_many(response, "  <DisMsg><![CDATA[", log->dismsg, "]]></DisMsg>\n", NULL);
			else
				addtobuffer(response, "  <DisMsg>N/A</DisMsg>\n");

			addtobuffer_many(response, 
				"  <Message><![CDATA[", msg_data(log->message, 0), "]]></Message>\n",
				"</ServerStatus>\n",
				NULL);

			msg->doingwhat = RESPONDING;
			msg->buflen = STRBUFLEN(response);
			msg->bufp = msg->buf = grabstrbuffer(response);
		}
	}
	else if ((strncmp(msg->buf, "xymondboard", 11) == 0) || (strncmp(msg->buf, "hobbitdboard", 12) == 0)) {
		/* 
		 * Request for a summmary of all known status logs
		 *
		 */
		xtreePos_t hosthandle;
		xymond_hostlist_t *hwalk;
		xymond_log_t *lwalk, *firstlog;
		time_t *dummytimes;
		static testinfo_t trendstest, infotest, clienttest;
		static xymond_log_t trendslogrec, infologrec, clientlogrec;
		static int faketestinit = 0;
		hostfilter_rec_t *logfilter;
		boardfield_t *logfields;
		char *fields = NULL;
		int acklevel = -1, havehostfilter = 0;
		strbuffer_t *response;
		static size_t lastboardsize = 0;

		if (viabfq || !oksender(wwwsenders, NULL, msg->sender, msg->buf)) goto done;

		logfilter = setup_filter(msg->buf, &fields, &acklevel, &havehostfilter);
		if (!fields) fields = "hostname,testname,color,flags,lastchange,logtime,validtime,acktime,disabletime,sender,cookie,line1";
		logfields = setup_fields(fields);

		response = newstrbuffer(lastboardsize);

		/* Setup fake log-records for the "clientlog", "info" and "trends" data. */
		dummytimes = (time_t *)calloc((flapcount > 0) ? flapcount : 1, sizeof(time_t));

		if (!faketestinit) {
			memset(&clienttest, 0, sizeof(clienttest));
			clienttest.name = xgetenv("CLIENTCOLUMN");
			memset(&clientlogrec, 0, sizeof(clientlogrec));
			clientlogrec.test = &clienttest;

			memset(&infotest, 0, sizeof(infotest));
			infotest.name = xgetenv("INFOCOLUMN");
			memset(&infologrec, 0, sizeof(infologrec));
			infologrec.test = &infotest;

			memset(&trendstest, 0, sizeof(trendstest));
			trendstest.name = xgetenv("TRENDSCOLUMN");
			memset(&trendslogrec, 0, sizeof(trendslogrec));
			trendslogrec.test = &trendstest;

			clientlogrec.color = infologrec.color = trendslogrec.color = COL_GREEN;
			clientlogrec.message = infologrec.message = trendslogrec.message = "";
			clientlogrec.sender = infologrec.sender = trendslogrec.sender = "xymond";
			faketestinit = 1;
		}
		clientlogrec.lastchange = infologrec.lastchange = trendslogrec.lastchange = dummytimes;


		for (hosthandle = xtreeFirst(rbhosts); (hosthandle != xtreeEnd(rbhosts)); hosthandle = xtreeNext(rbhosts, hosthandle)) {
			hwalk = xtreeData(rbhosts, hosthandle);
			if (!hwalk) {
				errprintf("host-tree has a record with no data\n");
				continue;
			}

			/* If there is a hostname filter, drop the "summary" 'hosts' */
			if (havehostfilter && (hwalk->hosttype != H_NORMAL)) continue;

			firstlog = hwalk->logs;

			if (hwalk->hosttype == H_NORMAL) {
				void *hinfo = hostinfo(hwalk->hostname);

				if (!hinfo) {
					errprintf("Hostname '%s' in tree, but no host-info\n", hwalk->hostname);
					continue;
				}

				/* Host/pagename filter */
				if (!match_host_filter(hinfo, logfilter, 0, NULL)) continue;

				/* Handle NOINFO, NOCLIENT and NOTRENDS here */
				if (hwalk->clientmsgs && !xmh_item(hinfo, XMH_FLAG_NOCLIENT)) {
					clientlogrec.next = firstlog;
					firstlog = &clientlogrec;
				}
				if (!xmh_item(hinfo, XMH_FLAG_NOINFO)) {
					infologrec.next = firstlog;
					firstlog = &infologrec;
				}
				if (!xmh_item(hinfo, XMH_FLAG_NOTRENDS)) {
					trendslogrec.next = firstlog;
					firstlog = &trendslogrec;
				}
			}

			clientlogrec.host = trendslogrec.host = infologrec.host = hwalk;

			for (lwalk = firstlog; (lwalk); lwalk = lwalk->next) {
				if (!match_test_filter(lwalk, logfilter)) continue;

				if (lwalk->message == NULL) {
					errprintf("%s.%s has a NULL message\n", (lwalk->host->hostname ? lwalk->host->hostname : "<no host>"), (lwalk->test->name ? lwalk->test->name : "<no test>"));
					lwalk->message = strdup("No data");
					lwalk->msgsz = strlen(lwalk->message) + 1;
				}

				response = generate_outbuf(&response, logfields, hwalk, lwalk, acklevel);
			}
		}

		if (STRBUFLEN(response) > lastboardsize) lastboardsize = STRBUFLEN(response);

		xfree(msg->buf);
		msg->doingwhat = RESPONDING;
		msg->buflen = STRBUFLEN(response);
		msg->bufp = msg->buf = grabstrbuffer(response);

		clear_filter(logfilter);
		xfree(logfields);
		xfree(dummytimes);
	}
	else if ((strncmp(msg->buf, "xymondxboard", 12) == 0) || (strncmp(msg->buf, "hobbitdxboard", 13) == 0)) {
		/* 
		 * Request for a summmary of all known status logs in XML format
		 *
		 */
		xtreePos_t hosthandle;
		xymond_hostlist_t *hwalk;
		xymond_log_t *lwalk;
		hostfilter_rec_t *logfilter;
		char *fields = NULL;
		int acklevel = -1, havehostfilter = 0;
		static size_t lastboardsize = 0;
		strbuffer_t *response;

		if (viabfq || !oksender(wwwsenders, NULL, msg->sender, msg->buf)) goto done;

		logfilter = setup_filter(msg->buf, &fields, &acklevel, &havehostfilter);
		response = newstrbuffer(lastboardsize);

		addtobuffer(response, "<?xml version='1.0' encoding='ISO-8859-1'?>\n");
		addtobuffer(response, "<StatusBoard>\n");

		for (hosthandle = xtreeFirst(rbhosts); (hosthandle != xtreeEnd(rbhosts)); hosthandle = xtreeNext(rbhosts, hosthandle)) {
			hwalk = xtreeData(rbhosts, hosthandle);
			if (!hwalk) {
				errprintf("host-tree has a record with no data\n");
				continue;
			}

			/* If there is a hostname filter, drop the "summary" 'hosts' */
			if (havehostfilter && (hwalk->hosttype != H_NORMAL)) continue;

			if (hwalk->hosttype == H_NORMAL) {
				void *hinfo;
				hinfo = hostinfo(hwalk->hostname);

				if (!hinfo) {
					errprintf("Hostname '%s' in tree, but no host-info\n", hwalk->hostname);
					continue;
				}

				/* Host/pagename filter */
				if (!match_host_filter(hinfo, logfilter, 0, NULL)) continue;
			}

			for (lwalk = hwalk->logs; (lwalk); lwalk = lwalk->next) {
				char *eoln;

				if (!match_test_filter(lwalk, logfilter)) continue;

				if (lwalk->message == NULL) {
					errprintf("%s.%s has a NULL message\n", lwalk->host->hostname, lwalk->test->name);
					lwalk->message = strdup("No data");
					lwalk->msgsz = strlen(lwalk->message) + 1;
				}

				eoln = strchr(lwalk->message, '\n');
				if (eoln) *eoln = '\0';

				addtobuffer_many(response, 
					"  <ServerStatus>\n",
					"    <ServerName>", hwalk->hostname, "</ServerName>\n",
					"    <Type>", lwalk->test->name, "</Type>\n",
					"    <Status>", colorname(lwalk->color), "</Status>\n",
					"    <TestFlags>", (lwalk->testflags ? lwalk->testflags : ""), "</TestFlags>\n",
					"    <LastChange>", timestr(lwalk->lastchange[0]), "</LastChange>\n",
					"    <LogTime>", timestr(lwalk->logtime), "</LogTime>\n",
					"    <ValidTime>", timestr(lwalk->validtime), "</ValidTime>\n",
					"    <AckTime>", timestr(lwalk->acktime), "</AckTime>\n",
					"    <DisableTime>", timestr(lwalk->enabletime), "</DisableTime>\n",
					"    <Sender>", lwalk->sender, "</Sender>\n",
					NULL);
				timestr(-999);

				if (lwalk->cookie && (lwalk->cookieexpires > now))
					addtobuffer_many(response, "    <Cookie>", lwalk->cookie, "</Cookie>\n", NULL);
				else
					addtobuffer(response, "    <Cookie>N/A</Cookie>\n");

				addtobuffer_many(response, 
					"    <MessageSummary><![CDATA[", lwalk->message, "]]></MessageSummary>\n",
					"  </ServerStatus>\n",
					NULL);
				if (eoln) *eoln = '\n';
			}
		}
		addtobuffer(response, "</StatusBoard>\n");

		xfree(msg->buf);
		msg->doingwhat = RESPONDING;
		msg->buflen = STRBUFLEN(response);
		msg->bufp = msg->buf = grabstrbuffer(response);
		if (msg->buflen > lastboardsize) lastboardsize = msg->buflen;

		clear_filter(logfilter);
	}
	else if (strncmp(msg->buf, "hostinfo", 8) == 0) {
		/* 
		 * Request for host configuration info
		 *
		 */
		void *hinfo;
		strbuffer_t *response;
		static size_t lastboardsize = 0;
		char *clonehost;

		if (viabfq || !oksender(wwwsenders, NULL, msg->sender, msg->buf)) goto done;

		response = newstrbuffer(lastboardsize);

		clonehost = strstr(msg->buf, " clone=");
		if (clonehost) {
			void *hinfo;

			clonehost += strlen(" clone=");
			hinfo = hostinfo(clonehost);

			if (hinfo) {
				enum xmh_item_t idx;
				char *val;

				for (idx = 0; (idx < XMH_LAST); idx++) {
					val = xmh_item(hinfo, idx);
					if (val) {
						addtobuffer_many(response, xmh_item_id(idx), ":", val, "\n", NULL);
					}
				}

				val = xmh_item_walk(hinfo);
				while (val) {
					addtobuffer_many(response, val, "\n", NULL);
					val = xmh_item_walk(NULL);
				}
			}
		}
		else {
			hostfilter_rec_t *hostfilter;
			boardfield_t *hostfields;
			char *fields = NULL, **tags = NULL;

			hostfilter = setup_filter(msg->buf, &fields, NULL, NULL);
			if (!fields) fields = "XMH_HOSTNAME,XMH_IP,XMH_RAW";
			hostfields = setup_fields(fields);

			for (hinfo = first_host(); (hinfo); hinfo = next_host(hinfo, 0)) {
				if (!match_host_filter(hinfo, hostfilter, 1, &tags)) continue;
				response = generate_hostinfo_outbuf(&response, hostfields, hinfo, tags);
			}
			if (tags) xfree(tags);

			clear_filter(hostfilter);
			xfree(hostfields);
		}

		xfree(msg->buf);
		msg->doingwhat = RESPONDING;
		msg->buflen = STRBUFLEN(response);
		msg->bufp = msg->buf = grabstrbuffer(response);
		if (msg->buflen > lastboardsize) lastboardsize = msg->buflen;
	}
	else if ((strncmp(msg->buf, "histsync", 8) == 0)) {
		/* 
		 * Mark one or many status logs as in need of a re-sync.
		 * This causes a spurious stachg post to be sent, and a
		 * spurious re-validation by xymond_history.
		 */
		xtreePos_t hosthandle;
		xymond_hostlist_t *hwalk;
		void *hinfo;
		xymond_log_t *lwalk;
		hostfilter_rec_t *logfilter;
		char *fields = NULL;
		int acklevel = -1, havehostfilter = 0;
		strbuffer_t *response;

		/* administrative - can cause high I/O load */
		if (!oksender(adminsenders, NULL, msg->sender, msg->buf)) goto done;

		logfilter = setup_filter(msg->buf, &fields, &acklevel, &havehostfilter);

		for (hosthandle = xtreeFirst(rbhosts); (hosthandle != xtreeEnd(rbhosts)); hosthandle = xtreeNext(rbhosts, hosthandle)) {

			hwalk = xtreeData(rbhosts, hosthandle);
			if (!hwalk) { errprintf("host-tree has a record with no data\n"); continue; }

			/* Summaries don't get a stachg anyway */
			if (hwalk->hosttype != H_NORMAL) continue;

			hinfo = hostinfo(hwalk->hostname);
			if (!hinfo) { errprintf("Hostname '%s' in tree, but no host-info\n", hwalk->hostname); continue; }

			/* Host/pagename filter */
			if (!match_host_filter(hinfo, logfilter, 0, NULL)) continue;
			dbgprintf(" - host %s matched histsync; clearing matching testnames\n", hwalk->hostname);

			for (lwalk = hwalk->logs; (lwalk); lwalk = lwalk->next) {
				// if (!match_test_filter(lwalk, logfilter)) continue;
				// dbgprintf(" - forcing a resync of history for %s.%s the next time through\n", lwalk->host->hostname, lwalk->test->name);
				// lwalk->histsynced = 0;
				if (match_test_filter(lwalk, logfilter)) lwalk->histsynced = 0;
			}
		}

		clear_filter(logfilter);
	}

	else if ((strncmp(msg->buf, "xymondack", 9) == 0) || (strncmp(msg->buf, "hobbitdack", 10) == 0) || (strncmp(msg->buf, "ack ack_event", 13) == 0)) {
		/* xymondack COOKIE DURATION TEXT */
		char *p, *cookie, *durstr, *tok, *mcopy;
		int duration;
		xymond_log_t *lwalk;

		if (!viabfq && !oksender(maintsenders, NULL, msg->sender, msg->buf)) goto done;

		/*
		 * For just a bit of compatibility with the old BB system,
		 * we will accept an "ack ack_event" message. This allows us
		 * to work with existing acknowledgement scripts.
		 */
		if (strncmp(msg->buf, "xymondack", 9) == 0) p = msg->buf + 9;
		else if (strncmp(msg->buf, "hobbitdack", 10) == 0) p = msg->buf + 10;
		else if (strncmp(msg->buf, "ack ack_event", 13) == 0) p = msg->buf + 13;
		else p = msg->buf;

		cookie = durstr = NULL;
		mcopy = strdup(p);
		tok = strtok(mcopy, " ");
		if (tok) { cookie = tok; tok = strtok(NULL, "\n"); }
		if (tok) durstr = tok;
		if (cookie && durstr) {
			log = find_cookie((*cookie == '-') ? cookie+1 : cookie);
			if (log) {
				duration = durationvalue(durstr);
				if (*cookie == '-') {
					/*
					 * Negative cookies mean to ack all pending alerts for
					 * the host. So loop over the host logs and ack all that
					 * have a valid cookie (i.e. not NULL)
					 */
					for (lwalk = log->host->logs; (lwalk); lwalk = lwalk->next) {
						if (lwalk->cookie) handle_ack(p, msg->sender, lwalk, duration);
					}
				}
				else {
					handle_ack(p, msg->sender, log, duration);
				}
			}
			else {
				errprintf("Cookie %s not found, dropping ack\n", cookie);
			}
		}
		else {
			errprintf("Bogus ack message from %s: '%s'\n", msg->sender, msg->buf);
		}
		xfree(mcopy);
	}
	else if (strncmp(msg->buf, "ackinfo ", 8) == 0) {
		/* ackinfo HOST.TEST\nlevel\nvaliduntil\nackedby\nmsg */
		int ackall = 0;

		if (!viabfq && !oksender(maintsenders, NULL, msg->sender, msg->buf)) goto done;

		get_hts(msg->buf, msg->sender, origin, &h, &t, NULL, &log, &color, NULL, &ackall, 0, 0);
		if (log) {
			handle_ackinfo(msg->buf, msg->sender, log);
		}
		else if (ackall) {
			xymond_log_t *lwalk;

			for (lwalk = h->logs; (lwalk); lwalk = lwalk->next) {
				if (decide_alertstate(lwalk->color) != A_OK) {
					handle_ackinfo(msg->buf, msg->sender, lwalk);
				}
			}
		}
	}
	else if (strncmp(msg->buf, "drop ", 5) == 0) {
		char *hostname = NULL, *testname = NULL;
		char *p;

		if (!viabfq && !oksender(adminsenders, NULL, msg->sender, msg->buf)) goto done;

		p = msg->buf + 4; p += strspn(p, " \t");
		hostname = strtok(p, " \t");
		if (hostname) testname = strtok(NULL, " \t");

		if (hostname && !testname) {
			handle_dropnrename(CMD_DROPHOST, msg->sender, hostname, NULL, NULL);
		}
		else if (hostname && testname) {
			handle_dropnrename(CMD_DROPTEST, msg->sender, hostname, testname, NULL);
		}
	}
	else if (strncmp(msg->buf, "rename ", 7) == 0) {
		char *hostname = NULL, *n1 = NULL, *n2 = NULL;
		char *p;

		if (!viabfq && !oksender(adminsenders, NULL, msg->sender, msg->buf)) goto done;

		p = msg->buf + 6; p += strspn(p, " \t");
		hostname = strtok(p, " \t");
		if (hostname) n1 = strtok(NULL, " \t");
		if (n1) n2 = strtok(NULL, " \t");

		if (hostname && n1 && !n2) {
			/* Host rename */
			handle_dropnrename(CMD_RENAMEHOST, msg->sender, hostname, n1, NULL);
		}
		else if (hostname && n1 && n2) {
			/* Test rename */
			handle_dropnrename(CMD_RENAMETEST, msg->sender, hostname, n1, n2);
		}
	}
	else if (strncmp(msg->buf, "dummy", 5) == 0) {
		/* Do nothing */
	}
	else if (strncmp(msg->buf, "ping", 4) == 0) {
		/* Tell them we're here */
		char id[128];

		if (viabfq) goto done;

		snprintf(id, sizeof(id), "xymond %s\n", VERSION);
		msg->doingwhat = RESPONDING;
		xfree(msg->buf);
		msg->bufp = msg->buf = strdup(id);
		msg->buflen = strlen(msg->buf);
	}
	else if (strncmp(msg->buf, "proxyping", 9) == 0) {
		/* A proxyping was sent directly to us */
		char id[128];

		if (viabfq) goto done;

		snprintf(id, sizeof(id), "xymond %s (not a proxy)\n", VERSION);
		msg->doingwhat = RESPONDING;
		xfree(msg->buf);
		msg->bufp = msg->buf = strdup(id);
		msg->buflen = strlen(msg->buf);
	}
	else if (strncmp(msg->buf, "notify", 6) == 0) {
		if (!viabfq && !oksender(maintsenders, NULL, msg->sender, msg->buf)) goto done;
		get_hts(msg->buf, msg->sender, origin, &h, &t, NULL, &log, &color, NULL, NULL, 0, 0);
		if (h && t) handle_notify(msg->buf, msg->sender, h->hostname, t->name);
	}
	else if (strncmp(msg->buf, "schedule", 8) == 0) {
		char *cmd;

		/*
		 * Schedule a later command. This is either
		 * "schedule" - no params: list the currently scheduled commands.
		 * "schedule TIME COMMAND": Add a COMMAND to run at TIME.
		 * "schedule cancel ID": Cancel the scheduled command with id ID.
		 */
		cmd = msg->buf + 8; cmd += strspn(cmd, " ");

		if (strlen(cmd) == 0) {
			if (!viabfq) {
				strbuffer_t *response = newstrbuffer(0);
				scheduletask_t *swalk;
				char tbuf[50];

				for (swalk = schedulehead; (swalk); swalk = swalk->next) {
					snprintf(tbuf, sizeof(tbuf), "%d|%d", swalk->id, (int)swalk->executiontime);
					addtobuffer_many(response, tbuf, "|", swalk->sender, "|", nlencode(swalk->command), "\n", NULL);
				}

				xfree(msg->buf);
				msg->doingwhat = RESPONDING;
				msg->buflen = STRBUFLEN(response);
				msg->bufp = msg->buf = grabstrbuffer(response);
			}
			else goto done;
		}
		else {
			if (strncmp(cmd, "cancel", 6) != 0) {
				scheduletask_t *newitem = (scheduletask_t *)malloc(sizeof(scheduletask_t));

				newitem->id = nextschedid++;
				newitem->executiontime = (time_t) atoi(cmd);
				cmd += strspn(cmd, "0123456789");
				cmd += strspn(cmd, " ");
				newitem->sender = strdup(msg->sender);
				newitem->command = strdup(cmd);
				newitem->next = schedulehead;
				schedulehead = newitem;
			}
			else {
				scheduletask_t *swalk, *sprev;
				int id;
				
				id = atoi(cmd + 6);
				swalk = schedulehead; sprev = NULL; 
				while (swalk && (swalk->id != id)) {
					sprev = swalk; 
					swalk = swalk->next;
				}

				if (swalk) {
					xfree(swalk->sender);
					xfree(swalk->command);
					if (sprev == NULL) {
						schedulehead = swalk->next;
					}
					else {
						sprev->next = swalk->next;
					}
					xfree(swalk);
				}
			}
		}
	}
	else if ((strncmp(msg->buf, "client", 6) == 0) && (strncmp(msg->buf, "clientlog", 9) != 0)) {
		/* "client[/COLLECTORID] HOSTNAME.CLIENTOS CLIENTCLASS" */
		/* or "clientsubmit[/COLLECTORID] HOSTNAME.CLIENTOS CLIENTCLASS" (same as client, but don't send config back) */
		/* or "clientconfig[/COLLECTORID] HOSTNAME.CLIENTOS CLIENTCLASS" (same as client, but don't handle msg, only send config back) */
		int process_clientmsg = 1;
		int send_clientconfig = 1;
		char *hostname = NULL, *clientos = NULL, *clientclass = NULL, *collectorid = NULL;
		char *hname = NULL;
		char *line1, *p;
		char savech = '\0';

		msgfrom = strstr(msg->buf, "\n[proxy]\n");
		if (msgfrom) {
			char *ipline = strstr(msgfrom, "\nClientIP:");

			if (ipline) get_sender(msg, ipline, "\nClientIP:");
		}

		/* TODO: combine this and legacy status-as-client parsing into a get_clientdata() routine */
		p = msg->buf + strcspn(msg->buf, "\r\n");
		if ((*p == '\r') || (*p == '\n')) {
			savech = *p;
			*p = '\0';
		}
		else {
			p = NULL;
		}
		line1 = strdup(msg->buf); if (p) *p = savech;

		/* check for special handling -- no response, or respond only */
		if (strncmp(line1, "clientsubmit", 12) == 0) send_clientconfig = 0;
		else if (strncmp(line1, "clientconfig", 12) == 0) process_clientmsg = 0;

		p = strtok(line1, " \t"); /* Skip the client.* keyword */
		if (p) collectorid = strchr(p, '/');
		if (collectorid) collectorid++;
		if (p) hostname = strtok(NULL, " \t"); /* Actually, HOSTNAME.CLIENTOS */
		if (hostname) {
			clientos = strrchr(hostname, '.'); 
			if (clientos) { *clientos = '\0'; clientos++; }
			uncommafy(hostname);

			/* Next tok from 'HOSTNAME.CLIENTOS CLIENTCLASS' */
			clientclass = strtok(NULL, " \t");
		}

		if (hostname && clientos) {
			char *hostip = NULL;

			hname = knownhost(hostname, &hostip, ghosthandling);

			if (hname == NULL) {
				hname = log_ghost(hostname, msg->sender, msg->buf);
			}

			if (hname == NULL) {
				/* Ignore it */
			}
			else if (!viabfq && !oksender(statussenders, hostinfo(hname), msg->sender, msg->buf)) {
				/* Invalid sender */
				errprintf("Invalid client message - sender %s not allowed for host %s\n", msg->sender, hostname);
				hname = NULL;
			}
			else {
				void *hinfo = hostinfo(hname);

				if (xtreeFind(rbhosts, hname) == xtreeEnd(rbhosts)) {
					/* No hostlist record */
					(void *)create_hostlist_t(hname, hostip);
					hostcount++;
				}

				if (hinfo) {
					if (clientos) xmh_set_item(hinfo, XMH_OS, clientos);
					if (clientclass && !collectorid) {
						/*
						 * If the default client sends an explicit class,
						 * save it for later use unless there is an
						 * explicit override (XMH_CLASS is alread set).
						 */
						char *forcedclass = xmh_item(hinfo, XMH_CLASS);

						if (!forcedclass) 
							xmh_set_item(hinfo, XMH_CLASS, clientclass);
						else 
							clientclass = forcedclass;
					}
				}

				if (process_clientmsg) handle_client(msg->buf, msg->sender, hname, collectorid, clientos, clientclass);
			}
		}

		if (!viabfq && send_clientconfig && hname) {
			char *cfg;
			
			cfg = get_clientconfig(hname, clientclass, clientos);
			if (cfg) {
				msg->doingwhat = RESPONDING;
				xfree(msg->buf);
				msg->bufp = msg->buf = strdup(cfg);
				msg->buflen = strlen(msg->buf);
			}
		}

		xfree(line1);
	}
	else if (strncmp(msg->buf, "clientlog ", 10) == 0) {
		char *hostname, *p;
		xtreePos_t hosthandle;
		if (viabfq || !oksender(wwwsenders, NULL, msg->sender, msg->buf)) goto done;

		p = msg->buf + strlen("clientlog"); p += strspn(p, "\t ");
		hostname = p; p += strcspn(p, "\t "); if (*p) { *p = '\0'; p++; }
		p += strspn(p, "\t ");

		hosthandle = xtreeFind(rbhosts, hostname);
		if (hosthandle != xtreeEnd(rbhosts)) {
			xymond_hostlist_t *hwalk;
			hwalk = xtreeData(rbhosts, hosthandle);

			if (hwalk->clientmsgs) {
				char *sections = NULL;
				char *cmsg = totalclientmsg(hwalk->clientmsgs);

				if (strncmp(p, "section=", 8) == 0) sections = strdup(p+8);

				xfree(msg->buf);
				msg->buf = NULL;
				msg->doingwhat = RESPONDING;

				if (!sections) {
					msg->bufp = msg->buf = strdup(cmsg);
					msg->buflen = strlen(msg->buf);
				}
				else {
					char *onesect;
					strbuffer_t *resp;

					onesect = strtok(sections, ",");
					resp = newstrbuffer(0);
					while (onesect) {
						char *sectmarker = (char *)malloc(strlen(onesect) + 4);
						char *beginp, *endp;

						sprintf(sectmarker, "\n[%s]", onesect);
						beginp = strstr(cmsg, sectmarker);
						if (beginp) {
							beginp += 1; /* Skip the newline */
							endp = strstr(beginp, "\n[");
							if (endp) { endp++; *endp = '\0'; }
							addtobuffer(resp, beginp);
							if (endp) *endp = '[';
						}

						xfree(sectmarker);
						onesect = strtok(NULL, ",");
					}

					msg->buflen = STRBUFLEN(resp);
					msg->buf = grabstrbuffer(resp);
					if (!msg->buf) msg->buf = strdup("");
					msg->bufp = msg->buf;
					xfree(sections);
				}
			}
		}
	}
	else if (strncmp(msg->buf, "ghostlist", 9) == 0) {
		/* NB: inverted */
		if (!viabfq && oksender(wwwsenders, NULL, msg->sender, msg->buf)) {
			xtreePos_t ghandle;
			ghostlist_t *gwalk;
			strbuffer_t *resp;
			char msgline[1024];

			resp = newstrbuffer(0);

			for (ghandle = xtreeFirst(rbghosts); (ghandle != xtreeEnd(rbghosts)); ghandle = xtreeNext(rbghosts, ghandle)) {
				gwalk = (ghostlist_t *)xtreeData(rbghosts, ghandle);
				snprintf(msgline, sizeof(msgline), "%s|%s|%ld\n", 
					 gwalk->name, gwalk->sender, (long int)(gwalk->tstamp + timeroffset));
				addtobuffer(resp, msgline);
			}

			msg->doingwhat = RESPONDING;
			xfree(msg->buf);
			msg->buflen = STRBUFLEN(resp);
			msg->buf = grabstrbuffer(resp);
			if (!msg->buf) msg->buf = strdup("");
			msg->bufp = msg->buf;
		}
	}

	else if (strncmp(msg->buf, "multisrclist", 12) == 0) {
		/* NB: inverted */
		if (!viabfq && oksender(wwwsenders, NULL, msg->sender, msg->buf)) {
			xtreePos_t mhandle;
			multisrclist_t *mwalk;
			strbuffer_t *resp;
			char msgline[1024];

			resp = newstrbuffer(0);

			for (mhandle = xtreeFirst(rbmultisrc); (mhandle != xtreeEnd(rbmultisrc)); mhandle = xtreeNext(rbmultisrc, mhandle)) {
				mwalk = (multisrclist_t *)xtreeData(rbmultisrc, mhandle);
				snprintf(msgline, sizeof(msgline), "%s|%s|%s|%ld\n", 
					 mwalk->id, mwalk->senders[0], mwalk->senders[1], (long int)(mwalk->tstamp + timeroffset));
				addtobuffer(resp, msgline);
			}

			msg->doingwhat = RESPONDING;
			xfree(msg->buf);
			msg->buflen = STRBUFLEN(resp);
			msg->buf = grabstrbuffer(resp);
			if (!msg->buf) msg->buf = strdup("");
			msg->bufp = msg->buf;
		}
	}
	else if (strncmp(msg->buf, "senderstats", 11) == 0) {
		xtreePos_t handle;
		senderstats_t *rec;
		strbuffer_t *resp;
		char msgline[1024];

		if (viabfq || !oksender(wwwsenders, NULL, msg->sender, msg->buf)) goto done;

		resp = newstrbuffer(0);

		for (handle = xtreeFirst(rbsenders); (handle != xtreeEnd(rbsenders)); handle = xtreeNext(rbsenders, handle)) {
			rec = (senderstats_t *)xtreeData(rbsenders, handle);
			snprintf(msgline, sizeof(msgline), "%s %lu\n", rec->senderip, rec->msgcount);
			addtobuffer(resp, msgline);
		}

		msg->doingwhat = RESPONDING;
		xfree(msg->buf);
		msg->buflen = STRBUFLEN(resp);
		msg->buf = grabstrbuffer(resp);
		if (!msg->buf) msg->buf = strdup("");
		msg->bufp = msg->buf;
	}

done:
	dbgprintf("<- do_message/%d\n", nesting);
	nesting--;
}


void save_checkpoint(void)
{
	char *tempfn;
	FILE *fd;
	xtreePos_t hosthandle;
	xymond_hostlist_t *hwalk;
	xymond_log_t *lwalk;
	time_t now = getcurrenttime(NULL);
	scheduletask_t *swalk;
	ackinfo_t *awalk;
	int iores = 0;

	if (checkpointfn == NULL) return;

	dbgprintf("-> save_checkpoint\n");
	tempfn = malloc(strlen(checkpointfn) + 20);
	sprintf(tempfn, "%s.%d", checkpointfn, (int)now);
	fd = fopen(tempfn, "w");
	if (fd == NULL) {
		errprintf("Cannot open checkpoint file %s : %s\n", tempfn, strerror(errno));
		xfree(tempfn);
		return;
	}

	for (hosthandle = xtreeFirst(rbhosts); ((hosthandle != xtreeEnd(rbhosts)) && (iores >= 0)); hosthandle = xtreeNext(rbhosts, hosthandle)) {
		char *msgstr;

		hwalk = xtreeData(rbhosts, hosthandle);

		for (lwalk = hwalk->logs; (lwalk); lwalk = lwalk->next) {
			if (lwalk->dismsg && (lwalk->enabletime < now) && (lwalk->enabletime != DISABLED_UNTIL_OK)) {
				xfree(lwalk->dismsg);
				lwalk->dismsg = NULL;
				lwalk->enabletime = 0;
			}
			if (lwalk->ackmsg && (lwalk->acktime < now)) {
				xfree(lwalk->ackmsg);
				lwalk->ackmsg = NULL;
				lwalk->acktime = 0;
			}
			flush_acklist(lwalk, 0);
			iores = fprintf(fd, "@@XYMONDCHK-V1|%s|%s|%s|%s|%s|%s|%s|%d|%d|%d|%d|%d|%s|%d|%s", 
				lwalk->origin, hwalk->hostname, lwalk->test->name, lwalk->sender,
				colnames[lwalk->color], 
				(lwalk->testflags ? lwalk->testflags : ""),
				colnames[lwalk->oldcolor],
				(int)lwalk->logtime, (int) lwalk->lastchange[0], (int) lwalk->validtime, 
				(int) lwalk->enabletime, (int) lwalk->acktime, 
				(lwalk->cookie ? lwalk->cookie : ""), (int) lwalk->cookieexpires,
				nlencode(lwalk->message));
			if (lwalk->dismsg) msgstr = nlencode(lwalk->dismsg); else msgstr = "";
			if (iores >= 0) iores = fprintf(fd, "|%s", msgstr);
			if (lwalk->ackmsg) msgstr = nlencode(lwalk->ackmsg); else msgstr = "";
			if (iores >= 0) iores = fprintf(fd, "|%s", msgstr);
			if (iores >= 0) iores = fprintf(fd, "|%d|%d", (int)lwalk->redstart, (int)lwalk->yellowstart);
			if (iores >= 0) iores = fprintf(fd, "\n");

			for (awalk = lwalk->acklist; (awalk && (iores >= 0)); awalk = awalk->next) {
				iores = fprintf(fd, "@@XYMONDCHK-V1|.acklist.|%s|%s|%d|%d|%d|%d|%s|%s\n",
						hwalk->hostname, lwalk->test->name,
			 			(int)awalk->received, (int)awalk->validuntil, (int)awalk->cleartime,
						awalk->level, awalk->ackedby, awalk->msg);
			}
		}
	}

	for (swalk = schedulehead; (swalk && (iores >= 0)); swalk = swalk->next) {
		iores = fprintf(fd, "@@XYMONDCHK-V1|.task.|%d|%d|%s|%s\n", 
			swalk->id, (int)swalk->executiontime, swalk->sender, nlencode(swalk->command));
	}

	if (iores < 0) {
		errprintf("I/O error while saving the checkpoint file: %s\n", strerror(errno));
		exit(1);
	}

	iores = fclose(fd);
	if (iores == EOF) {
		errprintf("I/O error while closing the checkpoint file: %s\n", strerror(errno));
		exit(1);
	}

	iores = rename(tempfn, checkpointfn);
	if (iores == -1) {
		errprintf("I/O error while renaming the checkpoint file: %s\n", strerror(errno));
		exit(1);
	}

	xfree(tempfn);
	dbgprintf("<- save_checkpoint\n");
}


void load_checkpoint(char *fn)
{
	FILE *fd;
	strbuffer_t *inbuf;
	char *item, *eoln;
	int i, err;
	char *hostip = NULL;
	xtreePos_t hosthandle, testhandle, originhandle;
	xymond_hostlist_t *hitem = NULL;
	testinfo_t *t = NULL;
	char *origin = NULL;
	xymond_log_t *ltail = NULL;
	char *originname, *hostname, *testname, *sender, *testflags, *statusmsg, *disablemsg, *ackmsg, *cookie; 
	time_t logtime, lastchange, validtime, enabletime, acktime, cookieexpires, yellowstart, redstart;
	int color = COL_GREEN, oldcolor = COL_GREEN;
	int count = 0;

	fd = fopen(fn, "r");
	if (fd == NULL) {
		errprintf("Cannot access checkpoint file %s for restore\n", fn);
		return;
	}

	inbuf = newstrbuffer(0);
	initfgets(fd);
	while (unlimfgets(inbuf, fd)) {
		originname = hostname = testname = sender = testflags = statusmsg = disablemsg = ackmsg = cookie = NULL;
		logtime = lastchange = validtime = enabletime = acktime = cookieexpires = yellowstart = redstart = 0;
		err = 0;
		eoln = NULL;

		if ((strncmp(STRBUF(inbuf), "@@XYMONDCHK-V1|.task.|", 22) == 0) || (strncmp(STRBUF(inbuf), "@@HOBBITDCHK-V1|.task.|", 23) == 0)) {
			scheduletask_t *newtask = (scheduletask_t *)calloc(1, sizeof(scheduletask_t));

			item = gettok(STRBUF(inbuf), "|\n"); i = 0;
			while (item && !err) {
				switch (i) {
				  case 0: break;
				  case 1: break;
				  case 2: newtask->id = atoi(item); break;
				  case 3: newtask->executiontime = (time_t) atoi(item); break;
				  case 4: newtask->sender = strdup(item); break;
				  case 5: nldecode(item); newtask->command = strdup(item); break;
				  default: break;
				}
				item = gettok(NULL, "|\n"); i++;
			}

			if (newtask->id && (newtask->executiontime > getcurrenttime(NULL)) && newtask->sender && newtask->command) {
				newtask->next = schedulehead;
				schedulehead = newtask;
			}
			else {
				if (newtask->sender) xfree(newtask->sender);
				if (newtask->command) xfree(newtask->command);
				xfree(newtask);
			}

			continue;
		}

		if ((strncmp(STRBUF(inbuf), "@@XYMONDCHK-V1|.acklist.|", 25) == 0) || (strncmp(STRBUF(inbuf), "@@HOBBITDCHK-V1|.acklist.|", 26) == 0)) {
			xymond_log_t *log = NULL;
			ackinfo_t *newack = (ackinfo_t *)calloc(1, sizeof(ackinfo_t));

			hitem = NULL;

			item = gettok(STRBUF(inbuf), "|\n"); i = 0;
			while (item) {

				switch (i) {
				  case 0: break;
				  case 1: break;
				  case 2: 
					hosthandle = xtreeFind(rbhosts, item); 
					hitem = xtreeData(rbhosts, hosthandle);
					break;
				  case 3: 
					testhandle = xtreeFind(rbtests, item);
					t = (testhandle == xtreeEnd(rbtests)) ? NULL : xtreeData(rbtests, testhandle);
					break;
				  case 4: newack->received = atoi(item); break;
				  case 5: newack->validuntil = atoi(item); break;
				  case 6: newack->cleartime = atoi(item); break;
				  case 7: newack->level = atoi(item); break;
				  case 8: newack->ackedby = strdup(item); break;
				  case 9: newack->msg = strdup(item); break;
				  default: break;
				}
				item = gettok(NULL, "|\n"); i++;
			}

			if (hitem && t) {
				for (log = hitem->logs; (log && (log->test != t)); log = log->next) ;
			}

			if (log && newack->msg) {
				newack->next = log->acklist;
				log->acklist = newack;
			}
			else {
				if (newack->ackedby) xfree(newack->ackedby);
				if (newack->msg) xfree(newack->msg);
				xfree(newack);
			}

			continue;
		}

		if ((strncmp(STRBUF(inbuf), "@@XYMONDCHK-V1|.", 16) == 0) || (strncmp(STRBUF(inbuf), "@@HOBBITDCHK-V1|.", 17) == 0)) continue;

		item = gettok(STRBUF(inbuf), "|\n"); i = 0;
		while (item && !err) {
			switch (i) {
			  case 0: err = ((strcmp(item, "@@XYMONDCHK-V1") != 0) && (strcmp(item, "@@HOBBITDCHK-V1") != 0) && (strcmp(item, "@@BBGENDCHK-V1") != 0)); break;
			  case 1: originname = item; break;
			  case 2: if (strlen(item)) hostname = item; else err=1; break;
			  case 3: if (strlen(item)) testname = item; else err=1; break;
			  case 4: sender = item; break;
			  case 5: color = parse_color(item); if (color == -1) err = 1; break;
			  case 6: testflags = item; break;
			  case 7: oldcolor = parse_color(item); if (oldcolor == -1) oldcolor = NO_COLOR; break;
			  case 8: logtime = atoi(item); break;
			  case 9: lastchange = atoi(item); break;
			  case 10: validtime = atoi(item); break;
			  case 11: enabletime = atoi(item); break;
			  case 12: acktime = atoi(item); break;
			  case 13: cookie = item; break;
			  case 14: cookieexpires = atoi(item); break;
			  case 15: if (strlen(item)) statusmsg = item; else err=1; break;
			  case 16: disablemsg = item; break;
			  case 17: ackmsg = item; break;
			  case 18: redstart = atoi(item); break;
			  case 19: yellowstart = atoi(item); break;
			  default: err = 1;
			}

			item = gettok(NULL, "|\n"); i++;
		}

		if (i < 17) {
			errprintf("Too few fields in record - found %d, expected 17\n", i);
			err = 1;
		}

		if (err) continue;

		/* Only load hosts we know; they may have been dropped while we were offline */
		hostname = knownhost(hostname, &hostip, ghosthandling);
		if (hostname == NULL) continue;

		/* Ignore the "client", "info" and "trends" data, since we generate on the fly now. */
		if (strcmp(testname, xgetenv("INFOCOLUMN")) == 0) continue;
		if (strcmp(testname, xgetenv("TRENDSCOLUMN")) == 0) continue;
		if (strcmp(testname, xgetenv("CLIENTCOLUMN")) == 0) continue;

		/* Rename the now-forgotten internal statuses */
		if (strcmp(hostname, getenv("MACHINEDOTS")) == 0) {
			if (strcmp(testname, "bbgen") == 0) testname = "xymongen";
			else if (strcmp(testname, "bbtest") == 0) testname = "xymonnet";
			else if (strcmp(testname, "hobbitd") == 0) testname = "xymond";
		}

		dbgprintf("Status: Host=%s, test=%s\n", hostname, testname); count++;

		hosthandle = xtreeFind(rbhosts, hostname);
		if (hosthandle == xtreeEnd(rbhosts)) {
			/* New host */
			hitem = create_hostlist_t(hostname, hostip);
			hostcount++;
		}
		else {
			hitem = xtreeData(rbhosts, hosthandle);
		}

		testhandle = xtreeFind(rbtests, testname);
		if (testhandle == xtreeEnd(rbtests)) {
			t = create_testinfo(testname);
		}
		else t = xtreeData(rbtests, testhandle);

		originhandle = xtreeFind(rborigins, originname);
		if (originhandle == xtreeEnd(rborigins)) {
			origin = strdup(originname);
			xtreeAdd(rborigins, origin, origin);
		}
		else origin = xtreeData(rborigins, originhandle);

		if (hitem->logs == NULL) {
			ltail = hitem->logs = (xymond_log_t *) calloc(1, sizeof(xymond_log_t));
		}
		else {
			ltail->next = (xymond_log_t *)calloc(1, sizeof(xymond_log_t));
			ltail = ltail->next;
		}

		if (strcmp(testname, xgetenv("PINGCOLUMN")) == 0) hitem->pinglog = ltail;

		/* Fixup validtime in case of ack'ed or disabled tests */
		if (validtime < acktime) validtime = acktime;
		if (validtime < enabletime) validtime = enabletime;

		ltail->test = t;
		ltail->host = hitem;
		ltail->origin = origin;
		ltail->color = color;
		ltail->oldcolor = oldcolor;
		ltail->activealert = (decide_alertstate(color) == A_ALERT);
		ltail->histsynced = 0;
		ltail->testflags = ( (testflags && strlen(testflags)) ? strdup(testflags) : NULL);
		ltail->sender = strdup(sender);
		ltail->logtime = logtime;
		ltail->lastchange = (time_t *)calloc((flapcount > 0) ? flapcount : 1, sizeof(time_t));
		ltail->lastchange[0] = lastchange;
		ltail->validtime = validtime;
		ltail->enabletime = enabletime;
		if (ltail->enabletime == DISABLED_UNTIL_OK) ltail->validtime = INT_MAX;
		ltail->acktime = acktime;
		ltail->redstart = redstart;
		ltail->yellowstart = yellowstart;
		nldecode(statusmsg);
		ltail->message = strdup(statusmsg);
		ltail->msgsz = strlen(statusmsg)+1;
		ltail->line1 = malloc(MAXLINE1SIZE * sizeof(unsigned char) + 1);
		eoln = strchr(ltail->message, '\n'); if (eoln) *eoln = '\0';
		snprintf(ltail->line1, MAXLINE1SIZE, "%s", msg_data(ltail->message, 0));
		if (eoln) *eoln = '\n';

		if (disablemsg && strlen(disablemsg)) {
			nldecode(disablemsg);
			ltail->dismsg = strdup(disablemsg);
		}
		else 
			ltail->dismsg = NULL;

		if (ackmsg && strlen(ackmsg)) {
			nldecode(ackmsg);
			ltail->ackmsg = strdup(ackmsg);
		}
		else 
			ltail->ackmsg = NULL;

		if (cookie && *cookie) {
			ltail->cookie = strdup(cookie);
			ltail->cookieexpires = cookieexpires;
			xtreeAdd(rbcookies, ltail->cookie, ltail);
		}
		else {
			ltail->cookie = NULL;
			ltail->cookieexpires = 0;
		}

		ltail->acklist = NULL;
		ltail->next = NULL;
	}

	fclose(fd);
	freestrbuffer(inbuf);
	dbgprintf("Loaded %d status logs\n", count);
}


void check_purple_status(void)
{
	xtreePos_t hosthandle;
	xymond_hostlist_t *hwalk;
	xymond_log_t *lwalk;
	time_t now = getcurrenttime(NULL);

	dbgprintf("-> check_purple_status\n");
	for (hosthandle = xtreeFirst(rbhosts); (hosthandle != xtreeEnd(rbhosts)); hosthandle = xtreeNext(rbhosts, hosthandle)) {
		hwalk = xtreeData(rbhosts, hosthandle);

		lwalk = hwalk->logs;
		while (lwalk) {
			if (lwalk->validtime < now) {
				dbgprintf("Purple log from %s %s\n", hwalk->hostname, lwalk->test->name);
				if (hwalk->hosttype == H_SUMMARY) {
					/*
					 * A summary has gone stale. Drop it.
					 */
					xymond_log_t *tmp;

					if (lwalk == hwalk->logs) {
						tmp = hwalk->logs;
						hwalk->logs = lwalk->next;
						lwalk = lwalk->next;
					}
					else {
						for (tmp = hwalk->logs; (tmp->next != lwalk); tmp = tmp->next);
						tmp->next = lwalk->next;
						tmp = lwalk;
						lwalk = lwalk->next;
					}
					free_log_t(tmp);
				}
				else {
					char *cause;
					int newcolor = COL_PURPLE;
					void *hinfo = hostinfo(hwalk->hostname);

					/*
					 * See if this is a host where the "conn" test shows it is down.
					 * If yes, then go CLEAR, instead of PURPLE.
					 */
					if (hwalk->pinglog && hinfo && (xmh_item(hinfo, XMH_FLAG_NOCLEAR) == NULL)) {
						switch (hwalk->pinglog->color) {
						  case COL_RED:
						  case COL_YELLOW:
						  case COL_BLUE:
						  case COL_CLEAR: /* if the "route:" tag is involved */
							newcolor = COL_CLEAR;
							break;

						  default:
							newcolor = COL_PURPLE;
							break;
						}
					}

					/* Tests on dialup hosts go clear, not purple */
					if ((newcolor == COL_PURPLE) && hinfo && xmh_item(hinfo, XMH_FLAG_DIALUP)) {
						newcolor = COL_CLEAR;
					}

					cause = check_downtime(hwalk->hostname, lwalk->test->name);
					if (lwalk) lwalk->downtimeactive = (cause != NULL);
					if (cause) {
						newcolor = COL_BLUE;
						/* If the status is not disabled, use downcause as the disable text */
						if (!lwalk->dismsg) lwalk->dismsg = strdup(cause);                                         
					}

					handle_status(lwalk->message, "xymond", 
						hwalk->hostname, lwalk->test->name, lwalk->grouplist, lwalk, newcolor, NULL, 0);
					lwalk = lwalk->next;
				}
			}
			else {
				lwalk = lwalk->next;
			}
		}
	}
	dbgprintf("<- check_purple_status\n");
}

void sig_handler(int signum)
{
	switch (signum) {
	  case SIGCHLD:
		break;

	  case SIGALRM:
		gotalarm = 1;
		break;

	  case SIGTERM:
	  case SIGINT:
		running = 0;
		break;

	  case SIGHUP:
		reloadconfig = 1;
		dologswitch = 1;
		break;

	  case SIGUSR1:
		nextcheckpoint = 0;
		break;

	  case SIGUSR2:
		/* Debug toggle must also toggle tcplib debugging */
		sigusr2_handler(signum);
		conn_register_infohandler(NULL, (debug ? INFO_DEBUG : INFO_WARN));
	}
}


enum conn_cbresult_t server_callback(tcpconn_t *connection, enum conn_callback_t id, void *userdata)
{
	int n = 0;
	conn_t *conn = (conn_t *)userdata;

	dbgprintf("CB %s\n", conn_callback_names[id]);

	switch (id) {
	  case CONN_CB_NEWCONNECTION:          /* Server mode: New incoming connection accepted */
		conn = (conn_t *)calloc(1, sizeof(conn_t));
		conn->doingwhat = RECEIVING;
		conn->bufsz = XYMON_INBUF_INITIAL;
		conn->buf = (unsigned char *)malloc(conn->bufsz);
		conn->bufp = conn->buf;
		conn->buflen = 0;
		conn->msgsz = -1;
		conn->sender = strdup(conn_print_ip(connection));
		connection->userdata = conn;
		break;

	  case CONN_CB_SSLHANDSHAKE_OK:        /* Client/server mode: SSL handshake completed OK (peer certificate ready) */
		conn->certcn = conn_peer_certificate_cn(connection);
		if (conn->certcn) dbgprintf("Got connection using client certificate issued to %s\n", conn->certcn);
		/* Fall through */
	  case CONN_CB_SSLHANDSHAKE_FAILED:    /* Client/server mode: SSL handshake failed (connection will close) */
		conn->bufp = conn->buf;
		conn->buflen = 0;
		break;

	  case CONN_CB_READCHECK:              /* Client/server mode: Check if application wants to read data */
		return (conn->doingwhat == RECEIVING) ? CONN_CBRESULT_OK : CONN_CBRESULT_FAILED;

	  case CONN_CB_WRITECHECK:             /* Client/server mode: Check if application wants to write data */
		return ((conn->doingwhat == RESPONDING) || (conn->doingwhat == STARTTLSWAIT)) ? CONN_CBRESULT_OK : CONN_CBRESULT_FAILED;

	  case CONN_CB_READ:                   /* Client/server mode: Ready for application to read data w/ conn_read() */
		n = conn_read(connection, (char *)conn->bufp, (conn->bufsz - conn->buflen - 1));
		if (n == 0) {
			switch (connection->connstate) {
			  case CONN_SSL_ACCEPT_READ:
			  case CONN_SSL_ACCEPT_WRITE:
			  case CONN_SSL_CONNECT_READ:
			  case CONN_SSL_CONNECT_WRITE:
			  case CONN_SSL_STARTTLS_READ:
			  case CONN_SSL_STARTTLS_WRITE:
			  case CONN_SSL_READ:
			  case CONN_SSL_WRITE:
				return CONN_CBRESULT_OK;
			  default:
				break;
			}
		}

		if (n < 0) {
			if (conn->buf && conn->buflen) {
				*(conn->bufp) = '\0';
				do_message(conn, "", 0);
			}
			else {
				conn->doingwhat = NOTALK;
			}
		}
		else if ((n > 0) && (conn->msgsz > 0) && ((conn->buflen+n) >= conn->msgsz)) {
			/* End of input data on this connection */
			// dbgprintf("Got the entire message, preparing response\n");
			conn->bufp += n;
			*(conn->bufp) = '\0';
			do_message(conn, "", 0);
		}
		else {
			*(conn->bufp + n) = '\0';
			// dbgprintf("Got data: %s\n", conn->bufp);

			conn->bufp += n;
			conn->buflen += n;

			if (strncasecmp(conn->buf, "size:", 5) == 0) {
				/* Got a message with size data. Ok to test for this every time, since we will remove the 'size:' line when we have it all */
				unsigned char *eosz = strchr(conn->buf, '\n');
				if (eosz) {
					int szlen = (eosz - conn->buf + 1);
					conn->msgsz = atoi(conn->buf + 5);
					if ((conn->msgsz <= 0) || (conn->msgsz > MAX_XYMON_INBUFSZ)) {
						/* Invalid message size */
						errprintf("Negative or huge size value in message from %s, dropping it\n", conn->sender);
						conn->doingwhat = NOTALK;
					}
					else {
						/* 
						 * Create a new buffer large enough for the entire message, 
						 * so we need not do any realloc()'s later. Add 2 kB extra, to
						 * avoid triggering the "grow-the-buffer" code just below.
						 * And copy the remainder of the data after the "size:.." line
						 * to the new buffer.
						 */
						unsigned char *newbuf = (unsigned char *)malloc(conn->msgsz + 2048);
						conn->buflen -= szlen;
						memmove(conn->buf, eosz+1, conn->buflen + 1);   /* Move the '\0' also */
						// strcpy(newbuf, eosz+1);
						xfree(conn->buf);
						conn->buf = newbuf;
						conn->bufp = conn->buf + conn->buflen;

						dbgprintf("Expect message of size %zu, currently have %zu\n", conn->msgsz, conn->buflen);
						if (conn->buflen >= conn->msgsz)
							do_message(conn, "", 0);
					}
				}
			}
			else if (strncasecmp(conn->buf, "starttls\n", 9) == 0) {
#ifdef HAVE_OPENSSL
				if (connection->sslhandling == CONN_SSL_STARTTLS_SERVER) 
				{
					// dbgprintf("Got a STARTTLS command, sending OK\n");
					conn->doingwhat = STARTTLSWAIT;
					strcpy(conn->buf, "OK TLS\n");
				}
				else 
#endif
				{
					// dbgprintf("Got a STARTTLS command, sending ERR\n");
					conn->doingwhat = RESPONDING;
					strcpy(conn->buf, "ERR No TLS\n");
				}
				conn->bufp = conn->buf;
				conn->buflen = strlen(conn->buf);
			}
			else if ((n == 0) && (connection->connstate == CONN_PLAINTEXT)) {
				/* No more data */
				do_message(conn, "", 0);
			}

			/* Grow the input buffer - within reason ... */
			if ((conn->bufsz - conn->buflen) < 2048) {
				if (conn->bufsz < MAX_XYMON_INBUFSZ) {
					conn->bufsz += XYMON_INBUF_INCREMENT;
					conn->buf = (unsigned char *) realloc(conn->buf, conn->bufsz);
					conn->bufp = conn->buf + conn->buflen;
				}
				else {
					/* Someone is flooding us */
					char *eoln;

					*(conn->buf + 200) = '\0';
					eoln = strchr(conn->buf, '\n');
					if (eoln) *eoln = '\0';
					errprintf("Data flooding from %s - 1st line %s\n", conn->sender, conn->buf);
					conn->doingwhat = NOTALK;
				}
			}
		}
		break;

	  case CONN_CB_WRITE:                  /* Client/server mode: Ready for application to write data w/ conn_write() */
		n = conn_write(connection, conn->bufp, conn->buflen);
		if (n == 0) {
			switch (connection->connstate) {
			  case CONN_SSL_ACCEPT_READ:
			  case CONN_SSL_ACCEPT_WRITE:
			  case CONN_SSL_CONNECT_READ:
			  case CONN_SSL_CONNECT_WRITE:
			  case CONN_SSL_STARTTLS_READ:
			  case CONN_SSL_STARTTLS_WRITE:
			  case CONN_SSL_READ:
			  case CONN_SSL_WRITE:
				return CONN_CBRESULT_OK;
			  default:
				break;
			}
		}


		if (n < 0) {
			conn->buflen = 0;
		}
		else {
			conn->bufp += n;
			conn->buflen -= n;
		}

		if (conn->buflen == 0) {
			if (conn->doingwhat == STARTTLSWAIT) {
				conn->doingwhat = RECEIVING;
				return CONN_CBRESULT_STARTTLS;
			}
			else
				conn->doingwhat = NOTALK;
		}
		break;

	  case CONN_CB_CLOSED:                 /* Client/server mode: Connection has been closed */
	  case CONN_CB_CLEANUP:                /* Client/server mode: Connection cleanup */
		if (conn) {
			xfree(conn->sender);
			if (conn->certcn) xfree(conn->certcn);
			if (conn->buf) xfree(conn->buf);
			xfree(conn);
			conn = connection->userdata = NULL;
		}
		break;

	  case CONN_CB_CONNECT_START:
	  case CONN_CB_CONNECT_COMPLETE:
		break;

	  case CONN_CB_CONNECT_FAILED:
	  case CONN_CB_TIMEOUT:
		conn->doingwhat = NOTALK;
		/* NB: With the new networking code, there may be a better way */
		/*     to track and report different types of failures here -jc */
		errprintf("Discarding timed-out partial msg from %s\n", conn->sender);		
		update_statistics("Timeouts", 0);
		conn_close_connection(connection, NULL);
		break;
	}

	if (conn && (conn->doingwhat == NOTALK)) conn_close_connection(connection, NULL);

	return CONN_CBRESULT_OK;
}


int main(int argc, char *argv[])
{
#ifdef IPV6_SUPPORT
	char *listen_addresses = "[::]:1984";
	char *tls_listen_addresses = NULL;
#else
	char *listen_addresses = "0.0.0.0:1984";
	char *tls_listen_addresses = NULL;
#endif

	char *certfn = NULL, *keyfn = NULL, *rootcafn = NULL;
	int requireclientcert = 0;
	char *hostsfn = NULL;
	char *restartfn = NULL;
	int do_purples = 1;
	time_t nextpurpleupdate;
	int lsocket, opt;
	int listenq = 512;
	int argi;
	struct timeval tv;
	struct timezone tz;
	int daemonize = 0;
	struct sigaction sa;
	time_t conn_timeout = 30;
	char *envarea = NULL;
	int create_backfeedqueue = 0;
	time_t nexttcpcheck;
	int tcpcheckinterval; int bfqchunksize;

	libxymon_init(argv[0]);

	libxymon_init(argv[0]);

	boottimer = gettimer();

	/* Create our trees */
	rbhosts = xtreeNew(strcasecmp);
	rbtests = xtreeNew(strcasecmp);
	rborigins = xtreeNew(strcasecmp);
	rbcookies = xtreeNew(strcasecmp);
	rbfilecache = xtreeNew(strcasecmp);
	rbghosts = xtreeNew(strcasecmp);
	rbmultisrc = xtreeNew(strcasecmp);
	rbsenders = xtreeNew(strcmp);

	/* For wildcard notify's */
	create_testinfo("*");

	colnames[COL_GREEN] = "green";
	colnames[COL_YELLOW] = "yellow";
	colnames[COL_RED] = "red";
	colnames[COL_CLEAR] = "clear";
	colnames[COL_BLUE] = "blue";
	colnames[COL_PURPLE] = "purple";
	colnames[NO_COLOR] = "none";
	gettimeofday(&tv, &tz);
	srandom(tv.tv_usec);

	defaultreddelay = xgetenv("DELAYRED"); if (defaultreddelay && (strlen(defaultreddelay) == 0)) defaultreddelay = NULL;
	defaultyellowdelay = xgetenv("DELAYYELLOW"); if (defaultyellowdelay && (strlen(defaultyellowdelay) == 0)) defaultyellowdelay = NULL;
	defaultcookietime = atoi(xgetenv("ACK_COOKIE_EXPIRATION"));

	/* Load alert config */
	alertcolors = colorset(xgetenv("ALERTCOLORS"), ((1 << COL_GREEN) | (1 << COL_BLUE)));
	okcolors = colorset(xgetenv("OKCOLORS"), (1 << COL_RED));

	for (argi=1; (argi < argc); argi++) {
		if (argnmatch(argv[argi], "--listen=")) {
			listen_addresses = strdup(strchr(argv[argi], '=') + 1);
		}
		else if (argnmatch(argv[argi], "--timeout=")) {
			char *p = strchr(argv[argi], '=') + 1;
			int newconn_timeout = atoi(p);
			if ((newconn_timeout < 5) || (newconn_timeout > 60)) 
				errprintf("--timeout must be between 5 and 60\n");
			else
				conn_timeout = newconn_timeout;
		}
		else if (argnmatch(argv[argi], "--hosts=")) {
			char *p = strchr(argv[argi], '=') + 1;
			hostsfn = strdup(p);
		}
		else if (argnmatch(argv[argi], "--tls-listen=")) {
			tls_listen_addresses = strdup(strchr(argv[argi], '=') + 1);
		}
		else if (argnmatch(argv[argi], "--tls-certificate=")) {
			char *p = strchr(argv[argi], '=') + 1;
			certfn = strdup(p);
		}
		else if (argnmatch(argv[argi], "--tls-key=")) {
			char *p = strchr(argv[argi], '=') + 1;
			keyfn = strdup(p);
		}
		else if (argnmatch(argv[argi], "--tls-clientrootca=")) {
			char *p = strchr(argv[argi], '=') + 1;
			rootcafn = strdup(p);
		}
		else if (argnmatch(argv[argi], "--tls-requireclientcert")) {
			requireclientcert = 1;
		}
		else if (argnmatch(argv[argi], "--checkpoint-file=")) {
			char *p = strchr(argv[argi], '=') + 1;
			checkpointfn = strdup(p);
		}
		else if (argnmatch(argv[argi], "--checkpoint-interval=")) {
			char *p = strchr(argv[argi], '=') + 1;
			checkpointinterval = atoi(p);
		}
		else if (argnmatch(argv[argi], "--reload-interval=")) {
			char *p = strchr(argv[argi], '=') + 1;
			reloadinterval = atoi(p);
		}
		else if (argnmatch(argv[argi], "--restart=")) {
			char *p = strchr(argv[argi], '=') + 1;
			restartfn = strdup(p);
		}
		else if (argnmatch(argv[argi], "--ghosts=")) {
			char *p = strchr(argv[argi], '=') + 1;

			if (strcmp(p, "allow") == 0) ghosthandling = GH_ALLOW;
			else if (strcmp(p, "drop") == 0) ghosthandling = GH_IGNORE;
			else if (strcmp(p, "log") == 0) ghosthandling = GH_LOG;
			else if (strcmp(p, "match") == 0) ghosthandling = GH_MATCH;
		}
		else if (argnmatch(argv[argi], "--merge-clientlocal")) {
			set_clientlocal_mergemode(1);
		}
		else if (argnmatch(argv[argi], "--no-purple")) {
			do_purples = 0;
		}
		else if (argnmatch(argv[argi], "--lqueue=")) {
			char *p = strchr(argv[argi], '=') + 1;
			listenq = atoi(p);
		}
		else if (argnmatch(argv[argi], "--daemon")) {
			daemonize = 1;
		}
		else if (argnmatch(argv[argi], "--no-daemon")) {
			daemonize = 0;
		}
		else if (argnmatch(argv[argi], "--ack-log=")) {
			char *p = strchr(argv[argi], '=');
			ackinfologfn = strdup(p+1);
		}
		else if (argnmatch(argv[argi], "--maint-senders=")) {
			/* Who is allowed to send us "enable", "disable", "ack", "notes" messages */
			char *p = strchr(argv[argi], '=');
			maintsenders = buildsenderlist(p+1);
		}
		else if (argnmatch(argv[argi], "--status-senders=")) {
			/* Who is allowed to send us "status", "combo", "summary", "data" messages */
			char *p = strchr(argv[argi], '=');
			statussenders = buildsenderlist(p+1);
		}
		else if (argnmatch(argv[argi], "--admin-senders=")) {
			/* Who is allowed to send us "drop", "rename", "config", "query" messages */
			char *p = strchr(argv[argi], '=');
			adminsenders = buildsenderlist(p+1);
		}
		else if (argnmatch(argv[argi], "--www-senders=")) {
			/* Who is allowed to send us "xymondboard", "xymondlog"  messages */
			char *p = strchr(argv[argi], '=');
			wwwsenders = buildsenderlist(p+1);
		}
		else if (argnmatch(argv[argi], "--dbghost=")) {
			char *p = strchr(argv[argi], '=');
			dbghost = strdup(p+1);
		}
		else if (argnmatch(argv[argi], "--flap-seconds=")) {
			char *p = strchr(argv[argi], '=');
			flapthreshold = atoi(p+1);
		}
		else if (argnmatch(argv[argi], "--flap-count=")) {
			char *p = strchr(argv[argi], '=');
			flapcount = atoi(p+1);
			if (flapcount < 0) flapcount = 0;
		}
		else if (strcmp(argv[argi], "--ack-each-color") == 0) {
			ackeachcolor = 1;
		}
		else if (argnmatch(argv[argi], "--trace=")) {
			char *p = strchr(argv[argi], '=');
			tracelist = buildsenderlist(p+1);
		}
		else if (strcmp(argv[argi], "--trace-all") == 0) {
			traceall = 1;
		}
		else if (strcmp(argv[argi], "--ignore-traced") == 0) {
			ignoretraced = 1;
		}
		else if (strcmp(argv[argi], "--no-clientlog") == 0) {
			 clientsavemem = 0;
			 clientsavedisk = 0;
		}
		else if (argnmatch(argv[argi], "--store-clientlogs")) {
			int anypos = 0, anyneg = 0;
			char *val = strchr(argv[argi], '=');

			if (!val) {
				clientsavedisk = 1;
			}
			else {
				char *tok;

				val = strdup(val+1);
				tok = strtok(val, ",");
				while (tok) {
					testinfo_t *t;
					
					if (*tok == '!') {
						anyneg = 1;
						t = create_testinfo(tok+1);
						t->clientsave = 0;
					}
					else {
						anypos = 1;
						t = create_testinfo(tok);
						t->clientsave = 1;
					}

					tok = strtok(NULL, ",");
				}
				xfree(val);

				/* 
				 * If only positive testnames listed, let default
				 * be NOT to save, and vice versa.  If mixed, 
				 * warn about it.
				 */
				if (anypos && !anyneg) clientsavedisk = 0;
				else if (anyneg && !anypos) clientsavedisk = 1;
				else {
					errprintf("Mixed list of testnames for --store-clientlogs option, will only save listed tests.\n");
					clientsavedisk = 0;
				}

			}

			if (anypos || clientsavedisk) clientsavemem = 1;
		}
		else if (strcmp(argv[argi], "--no-download") == 0) {
			 allow_downloads = 0;
		}
		else if (strncmp(argv[argi], "--bfq", 5) == 0) {
			 create_backfeedqueue = 1;
			 if (strncmp(argv[argi], "--bfq=", 6) == 0) {
				char *p = strchr(argv[argi], '=');
				bfqchannels = atoi(p+1);
			 }
			if ((bfqchannels < 0) || (bfqchannels > 9)) { errprintf("Invalid number of BFQ channels: %d\n", bfqchannels); return -1; }
		}
		else if (strcmp(argv[argi], "--no-bfq") == 0) {
			 create_backfeedqueue = 0;
		}
		else if (standardoption(argv[argi])) {
			if (showhelp) {
				printf("Options:\n");
				printf("\t--listen=IP:PORT              : The address the daemon listens on\n");
				printf("\t--hosts=FILENAME              : The hosts.cfg file\n");
				printf("\t--ghosts=allow|drop|log       : How to handle unknown hosts\n");
				return 1;
			}
		}
		else {
			errprintf("Unknown option '%s' - ignored\n", argv[argi]);
		}
	}

	if (xgetenv("HOSTSCFG") && (hostsfn == NULL)) {
		hostsfn = strdup(xgetenv("HOSTSCFG"));
	}

	if (xgetenv("STATUSLIFETIME")) {
		int n = atoi(xgetenv("STATUSLIFETIME"));
		if (n > 0) defaultvalidity = n;
	}

	/* Make sure we load hosts.cfg file, and not via the network from ourselves */
	hostsfn = (char *)realloc(hostsfn, strlen(hostsfn) + 2);
	memmove(hostsfn+1, hostsfn, strlen(hostsfn)+1);
	*hostsfn = '!';

	if ((ghosthandling != GH_ALLOW) && (hostsfn == NULL)) {
		errprintf("No hosts.cfg file specified, required when using ghosthandling\n");
		exit(1);
	}

	logprintf("Loading hostnames\n");
	load_hostnames(hostsfn, NULL, get_fqdn());
	logprintf("Loading client config\n");
	load_clientconfig();
	nextreload = getcurrenttime(NULL) + reloadinterval;

	if (restartfn) {
		logprintf("Loading saved state\n");
		load_checkpoint(restartfn);
		xfree(restartfn);
	}

	tcpcheckinterval = atoi(xgetenv("XYMONDTCPINTERVAL"));
	bfqchunksize = atoi(xgetenv("BFQCHUNKSIZE"));
	nexttcpcheck = getcurrenttime(NULL) + tcpcheckinterval;
	nextcheckpoint = getcurrenttime(NULL) + checkpointinterval;
	nextpurpleupdate = getcurrenttime(NULL) + 600;	/* Wait 10 minutes the first time */
	last_stats_time = getcurrenttime(NULL);	/* delay sending of the first status report until we're fully running */


	/* Set up a socket to listen for new connections */
	logprintf("Setting up network listener(s) on %s%s\n", listen_addresses,
		  (certfn && keyfn) ? " (STARTTLS enabled)" : " (STARTTLS disabled)");
	if (tls_listen_addresses) errprintf("Setting up TLS network listener(s) on %s\n", tls_listen_addresses);
	conn_register_infohandler(NULL, (debug ? INFO_DEBUG : INFO_WARN));
	conn_init_server(listenq, 1000000*conn_timeout, 
			 certfn, keyfn, rootcafn, requireclientcert, 
			 listen_addresses, tls_listen_addresses, server_callback);

	/* Go daemon */
	if (daemonize) {
		pid_t childpid;

		/* Become a daemon */
		childpid = fork();
		if (childpid < 0) {
			/* Fork failed */
			errprintf("Could not fork\n");
			exit(1);
		}
		else if (childpid > 0) {
			/* Parent just exits */
			exit(0);
		}

		/* Child (daemon) continues here */
		setsid();
	}

	if (pidfn != NULL) {
		/* Save PID */
		FILE *fd = fopen(pidfn, "w");
		if (fd) {
			if (fprintf(fd, "%lu\n", (unsigned long)getpid()) <= 0) {
				errprintf("Error writing PID file %s: %s\n", pidfn, strerror(errno));
			}
			fclose(fd);
		}
		else {
			errprintf("Cannot open PID file %s: %s\n", pidfn, strerror(errno));
		}
	}

	logprintf("Setting up signal handlers\n");
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sig_handler;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGCHLD, &sa, NULL);
	sigaction(SIGALRM, &sa, NULL);

	logprintf("Setting up xymond channels\n");
	statuschn = setup_channel(C_STATUS, CHAN_MASTER);
	if (statuschn == NULL) { errprintf("Cannot setup status channel\n"); return 1; }
	stachgchn = setup_channel(C_STACHG, CHAN_MASTER);
	if (stachgchn == NULL) { errprintf("Cannot setup stachg channel\n"); return 1; }
	pagechn   = setup_channel(C_PAGE, CHAN_MASTER);
	if (pagechn == NULL) { errprintf("Cannot setup page channel\n"); return 1; }
	datachn   = setup_channel(C_DATA, CHAN_MASTER);
	if (datachn == NULL) { errprintf("Cannot setup data channel\n"); return 1; }
	noteschn  = setup_channel(C_NOTES, CHAN_MASTER);
	if (noteschn == NULL) { errprintf("Cannot setup notes channel\n"); return 1; }
	enadischn  = setup_channel(C_ENADIS, CHAN_MASTER);
	if (enadischn == NULL) { errprintf("Cannot setup enadis channel\n"); return 1; }
	clientchn  = setup_channel(C_CLIENT, CHAN_MASTER);
	if (clientchn == NULL) { errprintf("Cannot setup client channel\n"); return 1; }
	clichgchn  = setup_channel(C_CLICHG, CHAN_MASTER);
	if (clichgchn == NULL) { errprintf("Cannot setup clichg channel\n"); return 1; }
	userchn  = setup_channel(C_USER, CHAN_MASTER);
	if (userchn == NULL) { errprintf("Cannot setup user channel\n"); return 1; }

	/* Can create and loop over multiple bfq's */
	if (create_backfeedqueue) {
		int i;

		for (i = 0; i <= bfqchannels; i++) {
			logprintf("Setting up backfeed queue %d\n", i);
			bfqids[i]  = setup_feedback_queue(i, CHAN_MASTER);
			if (bfqids[i] == -1) { errprintf("Cannot setup backfeed-client channel %d\n", i); return 1; }
		}

		bf_bufsz = 1024*shbufsz(C_FEEDBACK_QUEUE);
		logprintf("Backfeed queue buffer allocation: %d bytes\n", bf_bufsz);
		bf_buf = (char *)calloc(1, bf_bufsz);
		if (bf_buf == NULL) { errprintf("Cannot allocate backfeed queue buffer (%d): %s\n", bf_bufsz, strerror(errno)); return 1; }
		*bf_buf = '\0';
		bf_bufsz -= sizeof(long); /* leave space for msgp at the beginning */
	}

	logprintf("Setting up logfiles\n");
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	if (!logfn && getenv("XYMONLAUNCH_LOGFILENAME")) {
		/* No log file on the command line, but our STDOUT is already */
		/* being piped somewhere. Record this for when it's time to re-open on rotation */
		logfn = xgetenv("XYMONLAUNCH_LOGFILENAME");
		dbgprintf("xymond: Already logging out to %s, per xymonlaunch\n", logfn);
	}

	reopen_file("/dev/null", "r", stdin);
	if (logfn) {
		reopen_file(logfn, "a", stdout);
		reopen_file(logfn, "a", stderr);
	}

	if (ackinfologfn) {
		ackinfologfd = fopen(ackinfologfn, "a");
		if (ackinfologfd == NULL) {
			errprintf("Cannot open ack logfile %s: %s\n", ackinfologfn, strerror(errno));
		}
	}

	if (dbghost) {
		char fname[PATH_MAX];

		snprintf(fname, sizeof(fname), "%s/xymond.dbg", xgetenv("XYMONTMP"));
		dbgfd = fopen(fname, "a");
		if (dbgfd == NULL) errprintf("Cannot open debug file %s: %s\n", fname, strerror(errno));
	}

	logprintf("Setup complete\n");
	do {
		/*
		 * The endless loop.
		 *
		 * First attend to the housekeeping chores:
		 * - send out our heartbeat signal;
		 * - pick up children to avoid zombies;
		 * - rotate logs, if we have been asked to;
		 * - re-load the hosts.cfg configuration if needed;
		 * - check for stale status-logs that must go purple;
		 * - inject our own statistics message.
		 * - save the checkpoint file;
		 *
		 * Then do the network I/O.
		 */
		fd_set fdread, fdwrite;
		int maxfd, n;
		struct timeval tmo;

		time_t now = getcurrenttime(NULL);
		int childstat;
		int backfeeddata;
		int bfqempty = -1;
		int bfqexiting = 0;

		/* Pickup any finished child processes to avoid zombies */
		while (wait3(&childstat, WNOHANG, NULL) > 0) ;

		if (dologswitch) {
			logprintf("Reopening logfiles\n");
			if (logfn) {
				reopen_file(logfn, "a", stdout);
				reopen_file(logfn, "a", stderr);
			}
			if (ackinfologfd) reopen_file(ackinfologfn, "a", ackinfologfd);
			dologswitch = 0;
			posttoall("logrotate");
		}

		if ((reloadconfig || (now >= nextreload)) && hostsfn) {
			xtreePos_t hosthandle;
			int loadresult;

			logprintf("Reloading hostnames\n");
			reloadconfig = 0;
			nextreload = now + reloadinterval;
			loadresult = load_hostnames(hostsfn, NULL, get_fqdn());
			flush_filecache();

			if (loadresult == 0) {
				/* Scan our list of hosts and weed out those we do not know about any more */
				logprintf("Rescanning host tree\n");
				hosthandle = xtreeFirst(rbhosts);
				while (hosthandle != xtreeEnd(rbhosts)) {
					xymond_hostlist_t *hwalk;

					hwalk = xtreeData(rbhosts, hosthandle);

					if (hwalk->hosttype == H_SUMMARY) {
						/* Leave the summaries as-is */
						hosthandle = xtreeNext(rbhosts, hosthandle);
					}
					else if (hwalk->hostname == NULL) {
						errprintf("Null hostname record in state; can't remove, moving on\n");
						hosthandle = xtreeNext(rbhosts, hosthandle);
					}
					else if (hostinfo(hwalk->hostname) == NULL) {
						/* Remove all state info about this host. This will NOT remove files. */
						// dbgprintf("Host info record for '%s' is now empty; removing...\n", hwalk->hostname);
						handle_dropnrename(CMD_DROPSTATE, "xymond", hwalk->hostname, NULL, NULL);

						/* Must restart tree-walk after deleting node from the tree */
						// dbgprintf("Restarting hostinfo scan from the beginning...\n");
						hosthandle = xtreeFirst(rbhosts);
					}
					else {
						// dbgprintf(" - record %s present...\n", hwalk->hostname);
						hosthandle = xtreeNext(rbhosts, hosthandle);
					}
				}

				posttoall("reload");
			}

			logprintf("Reloading client config\n");
			load_clientconfig();
		}

		if (do_purples && (now >= nextpurpleupdate)) {
			nextpurpleupdate = getcurrenttime(NULL) + purplecheckinterval;
			check_purple_status();
		}

		if ((last_stats_time + statsinterval) <= now) {
			char *buf;
			xymond_hostlist_t *h;
			testinfo_t *t;
			xymond_log_t *log;
			int color;

			logprintf("Generating stats\n");
			buf = generate_stats();
			get_hts(buf, "xymond", "", &h, &t, NULL, &log, &color, NULL, NULL, 1, 1);
			if (!h || !t || !log) {
				errprintf("xymond servername MACHINE='%s' not listed in hosts.cfg, dropping xymond status\n",
					  xgetenv("MACHINE"));
			}
			else {
				handle_status(buf, "xymond", h->hostname, t->name, NULL, log, color, NULL, 0);
			}
			last_stats_time = now;
			flush_errbuf();
		}

		if (now > nextcheckpoint) {
			pid_t childpid;

			nextcheckpoint = now + checkpointinterval;
			childpid = fork();
			if (childpid == -1) {
				errprintf("Could not fork checkpoint child:%s\n", strerror(errno));
			}
			else if (childpid == 0) {
				logprintf("Saving checkpoint file\n");
				save_checkpoint();
				exit(0);
			}
		}

		while (create_backfeedqueue && !bfqexiting) {
			ssize_t sz;
			conn_t msg;
			static int qnumber = 0;

			if (qnumber > bfqchannels) qnumber = 0;

			/* FIXME: This section has been patched so much, it needs a refactor and perf analysis -jc */			
			sz = msgrcv(bfqids[qnumber++], bf_buf, bf_bufsz, 0L, IPC_NOWAIT);
			backfeeddata = (sz > 0);

			if ((sz == -1) && !((errno == ENOMSG) || (errno == EAGAIN) )) {
				if (errno == E2BIG) {
					/* someone dropped something huge on our message queue */
					/* since we can't guarantee what it is to downstream users, just log and discard it */
					sz = msgrcv(backfeedqueue, bf_buf, bf_bufsz, 0L, IPC_NOWAIT | MSG_NOERROR);
					if (sz != -1) {
						errprintf("Dropping oversized message from BFQ %d (limit=%zu). Increase MAXMSG_BFQ in xymonserver.cfg\n", backfeedqueue, bf_bufsz);
					}
					else {
						running = 0;
						errprintf("ERROR reading oversized message from BFQ %d: %s; exiting\n", backfeedqueue, strerror(errno));
					}
				}
				else if ((errno == EACCES) || (errno == EFAULT) || (errno == EINVAL)) {
					running = 0;
					errprintf("ERROR reading from BFQ %d: %s; exiting\n", backfeedqueue, strerror(errno));
				}
				else errprintf("Skipping backfeed queue message due to error: %s\n", strerror(errno));
			}
			else if (backfeeddata) {
				backfeedcount++;

				msg.buf = bf_buf;
				msg.msgsz = sz + 1;
				msg.bufsz = msg.buflen = sz;
				msg.bufp = msg.buf + msg.buflen;
				msg.doingwhat = RECEIVING;
				msg.sender = strdup("BFQ");
				msg.certcn = NULL;

				do_message(&msg, "", 1);
				*bf_buf = '\0';

				if (backfeedcount >= bfqchkcount) {
					dbgprintf("bfq count for this loop: %d, exceeds %d\n", backfeedcount, bfqchkcount);
					bfqchkcount += bfqchunksize; /* They both start at 0 on launch; only check time every XX messages */
					if (getcurrenttime(NULL) >= nexttcpcheck) { bfqexiting=1; dbgprintf("Exiting bfq early (at %d, limit now %d) to handle TCP\n", backfeedcount, bfqchkcount); }
				}
				bfqempty = 0;
			}
			else if (++bfqempty > bfqchannels) bfqexiting = 1;
		}

		/*
		 * Do the select() with a static 2 second timeout. 
		 * This is long enough that we will suspend activity for
		 * some time if there's nothing to do, but short enough for
		 * us to attend to the housekeeping stuff without undue delay.
		 */
		maxfd = conn_fdset(&fdread, &fdwrite);
		tmo.tv_sec = 0; tmo.tv_usec = 50000;
		n = select(maxfd+1, &fdread, &fdwrite, NULL, &tmo);
		if (n < 0) {
			/* Ignore EINTR, just carry on. All other errors are fatal. */
			if (errno != EINTR) {
				errprintf("Fatal error in select: %s\n", strerror(errno));
				running = 0;
				continue;
					}
				}

		conn_process_active(&fdread, &fdwrite);

		/* Purge the old and dead connections */
		conn_trimactive();

		/* Pick up new connections */
		conn_process_listeners(&fdread);

		/* When should we next try to handle TCP by? */
		nexttcpcheck = getcurrenttime(NULL) + tcpcheckinterval;

		/* Any scheduled tasks that need attending to? */
		{
			scheduletask_t *swalk, *sprev;

			swalk = schedulehead; sprev = NULL;
			while (swalk) {
				if (swalk->executiontime <= now) {
					scheduletask_t *runtask = swalk;
					conn_t task;

					/* Unlink the entry */
					if (sprev == NULL) 
						schedulehead = swalk->next;
					else
						sprev->next = swalk->next;
					swalk = swalk->next;

					memset(&task, 0, sizeof(task));
					task.doingwhat = NOTALK;
					task.sender = runtask->sender;
					task.buf = task.bufp = runtask->command;
					task.buflen = strlen(runtask->command); task.bufsz = task.buflen+1;
					do_message(&task, "", 1);

					errprintf("Ran scheduled task %d from %s: %s\n", 
						  runtask->id, runtask->sender, runtask->command);
					xfree(runtask->sender); xfree(runtask->command); xfree(runtask);
				}
				else {
					sprev = swalk;
					swalk = swalk->next;
				}
			}
		}
	} while (running);

	/* Tell the workers we to shutdown also */
	running = 1;   /* Kludge, but it's the only way to get posttochannel to do something. */
	posttoall("shutdown");
	running = 0;

	/* Close the channels */
	logprintf("Closing xymond channels\n");
	close_channel(statuschn, CHAN_MASTER);
	close_channel(stachgchn, CHAN_MASTER);
	close_channel(pagechn, CHAN_MASTER);
	close_channel(datachn, CHAN_MASTER);
	close_channel(noteschn, CHAN_MASTER);
	close_channel(enadischn, CHAN_MASTER);
	close_channel(clientchn, CHAN_MASTER);
	close_channel(clichgchn, CHAN_MASTER);
	close_channel(userchn, CHAN_MASTER);

	if (backfeedqueue >= 0) close_feedback_queue(backfeedqueue, CHAN_MASTER);
	if (bf_buf) xfree(bf_buf);

	logprintf("Saving final checkpoint file\n");
	save_checkpoint();
	if (pidfn) unlink(pidfn);

#ifdef DEBUG_FOR_VALGRIND
	initialize_hostlist();
	destroy_hosttree();
#endif
	xfree(hostsfn);

	if (dbgfd) fclose(dbgfd);

	if (hostsfn) xfree(hostsfn);
	return 0;
}

