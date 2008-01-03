/*----------------------------------------------------------------------------*/
/* Hobbit message daemon.                                                     */
/*                                                                            */
/* Client backend module for z/VSE or VSE/ESA                                 */
/*                                                                            */
/* Copyright (C) 2005-2008 Henrik Storner <henrik@hswn.dk>                    */
/* Copyright (C) 2006-2008 Rich Smrcina                                       */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

static char zvse_rcsid[] = "$Id: zvse.c,v 1.2 2008-01-03 10:11:16 henrik Exp $";

static void zvse_cpu_report(char *hostname, char *clientclass, enum ostype_t os,
                     void *hinfo, char *fromline, char *timestr,
                     char *cpuutilstr, char *uptimestr)
{
        char *p;
        float load1, loadyellow, loadred;
        int recentlimit, ancientlimit, maxclockdiff;
        int  uphour, upmin;
        char loadresult[100];
        char myupstr[100];
        long uptimesecs = -1;
        long upday;

        int cpucolor = COL_GREEN;

        char msgline[4096];
        strbuffer_t *upmsg;

        if (!want_msgtype(hinfo, MSG_CPU)) return;

        if (!uptimestr) return;

        uptimesecs = 0;

        /*
         * z/VSE: "Uptime: 1 Days, 13 Hours, 38 Minutes"
         */

        sscanf(uptimestr,"Uptime: %ld Days, %d Hours, %d Minutes", &upday, &uphour, &upmin);
        uptimesecs = upday * 86400;
        uptimesecs += 60*(60*uphour + upmin);
        sprintf(myupstr, "%s\n", uptimestr);

        /*
         *  Looking for average CPU Utilization in CPU message
         *  Avg CPU=000%
         */
        *loadresult = '\0';
        p = strstr(cpuutilstr, "Avg CPU=") + 8 ;
        if (p) {
                if (sscanf(p, "%f%%", &load1) == 1) {
                        sprintf(loadresult, "z/VSE CPU Utilization %3.0f%%\n", load1);
                }
        }

        get_cpu_thresholds(hinfo, clientclass, &loadyellow, &loadred, &recentlimit, &ancientlimit, &maxclockdiff);

        upmsg = newstrbuffer(0);

        if (load1 > loadred) {
                cpucolor = COL_RED;
                addtobuffer(upmsg, "&red Load is CRITICAL\n");
        }
        else if (load1 > loadyellow) {
                cpucolor = COL_YELLOW;
                addtobuffer(upmsg, "&yellow Load is HIGH\n");
        }

        if ((uptimesecs != -1) && (recentlimit != -1) && (uptimesecs < recentlimit)) {
                if (cpucolor == COL_GREEN) cpucolor = COL_YELLOW;
                addtobuffer(upmsg, "&yellow Machine recently rebooted\n");
        }
        if ((uptimesecs != -1) && (ancientlimit != -1) && (uptimesecs > ancientlimit)) {
                if (cpucolor == COL_GREEN) cpucolor = COL_YELLOW;
                sprintf(msgline, "&yellow Machine has been up more than %d days\n", (ancientlimit / 86400));
                addtobuffer(upmsg, msgline);
        }

        init_status(cpucolor);
        sprintf(msgline, "status %s.cpu %s %s %s %s %s\n",
                commafy(hostname), colorname(cpucolor),
                (timestr ? timestr : "<no timestamp data>"),
                loadresult,
                myupstr,
                cpuutilstr);
        addtostatus(msgline);
        if (STRBUFLEN(upmsg)) {
                addtostrstatus(upmsg);
                addtostatus("\n");
        }

        if (fromline && !localmode) addtostatus(fromline);
        finish_status();

        freestrbuffer(upmsg);
}

static void zvse_paging_report(char *hostname, char *clientclass, enum ostype_t os,
                     void *hinfo, char *fromline, char *timestr, char *pagingstr)
{
	char *p;
        int ipagerate, pagingyellow, pagingred;
        float fpagerate=0.0;
        char pagingresult[100];

        int pagingcolor = COL_GREEN;
        char msgline[4096];
        strbuffer_t *upmsg;

        /*
         *  Looking for Paging rate in message
         *  Page Rate=0.00 /sec
         */
        *pagingresult = '\0';

	ipagerate=0;
	p = strstr(pagingstr, "Page Rate=") + 10; 
	if (p) {
        	if (sscanf(p, "%f", &fpagerate) == 1) {
			ipagerate=fpagerate + 0.5;   /*  Rounding up */
                	sprintf(pagingresult, "z/VSE Paging Rate %d per second\n", ipagerate);
                	}
		}
	else
		sprintf(pagingresult, "Can not find page rate value in:\n%s\n", pagingstr);

        get_paging_thresholds(hinfo, clientclass, &pagingyellow, &pagingred);

        upmsg = newstrbuffer(0);

        if (ipagerate > pagingred) {
                pagingcolor = COL_RED;
                addtobuffer(upmsg, "&red Paging Rate is CRITICAL\n");
        }
        else if (ipagerate > pagingyellow) {
                pagingcolor = COL_YELLOW;
                addtobuffer(upmsg, "&yellow Paging Rate is HIGH\n");
        }

        init_status(pagingcolor);
        sprintf(msgline, "status %s.paging %s %s %s %s\n",
                commafy(hostname), colorname(pagingcolor),
                (timestr ? timestr : "<no timestamp data>"),
                pagingresult, pagingstr);
        addtostatus(msgline);
        if (STRBUFLEN(upmsg)) {
                addtostrstatus(upmsg);
                addtostatus("\n");
        }

        if (fromline && !localmode) addtostatus(fromline);
        finish_status();

        freestrbuffer(upmsg);
}

static void zvse_jobs_report(char *hostname, char *clientclass, enum ostype_t os,
                      void *hinfo, char *fromline, char *timestr,
                      char *psstr)
{
        int pscolor = COL_GREEN;

        int pchecks;
        int cmdofs = -1;
        char *p, *eol;
        char msgline[4096];
        strbuffer_t *monmsg;
        static strbuffer_t *countdata = NULL;
        int anycountdata = 0;
        char *group;

        if (!want_msgtype(hinfo, MSG_PROCS)) return;
        if (!psstr) return;

        if (!countdata) countdata = newstrbuffer(0);

        clearalertgroups();
        monmsg = newstrbuffer(0);

        sprintf(msgline, "data %s.proccounts\n", commafy(hostname));
        addtobuffer(countdata, msgline);

        cmdofs = 0;   /*  Command offset for z/VSE isn't necessary  */

        pchecks = clear_process_counts(hinfo, clientclass);

        if (pchecks == 0) {
                /* Nothing to check */
                sprintf(msgline, "&%s No process checks defined\n", colorname(noreportcolor));
                addtobuffer(monmsg, msgline);
                pscolor = noreportcolor;
        }
        else if (cmdofs >= 0) {
                /* Count how many instances of each monitored process is running */
                char *pname, *pid, *bol, *nl;
                int pcount, pmin, pmax, pcolor, ptrack;

                bol = psstr;
                while (bol) {
                        nl = strchr(bol, '\n');

                        /* Take care - the ps output line may be shorter than what we look at */
                        if (nl) {
                                *nl = '\0';

                                if ((nl-bol) > cmdofs) add_process_count(bol+cmdofs);

                                *nl = '\n';
                                bol = nl+1;
                        }
                        else {
                                if (strlen(bol) > cmdofs) add_process_count(bol+cmdofs);

                                bol = NULL;
                        }
                }

                /* Check the number found for each monitored process */
                while ((pname = check_process_count(&pcount, &pmin, &pmax, &pcolor, &pid, &ptrack, &group)) != NULL) {
                        char limtxt[1024];

                        if (pmax == -1) {
                                if (pmin > 0) sprintf(limtxt, "%d or more", pmin);
                                else if (pmin == 0) sprintf(limtxt, "none");
                        }
                        else {
                                if (pmin > 0) sprintf(limtxt, "between %d and %d", pmin, pmax);
                                else if (pmin == 0) sprintf(limtxt, "at most %d", pmax);
                        }

                        if (pcolor == COL_GREEN) {
                                sprintf(msgline, "&green %s (found %d, req. %s)\n", pname, pcount, limtxt);
                                addtobuffer(monmsg, msgline);
                        }
                        else {
                                if (pcolor > pscolor) pscolor = pcolor;
                                sprintf(msgline, "&%s %s (found %d, req. %s)\n",
                                        colorname(pcolor), pname, pcount, limtxt);
                                addtobuffer(monmsg, msgline);
                                addalertgroup(group);
                        }

                        if (ptrack) {
                                /* Save the count data for later DATA message to track process counts */
                                if (!pid) pid = "default";
                                sprintf(msgline, "%s:%u\n", pid, pcount);
                                addtobuffer(countdata, msgline);
                                anycountdata = 1;
                        }
                }
        }
        else {
                pscolor = COL_YELLOW;
                sprintf(msgline, "&yellow Expected string not found in ps output header\n");
                addtobuffer(monmsg, msgline);
        }

        /* Now we know the result, so generate a status message */
        init_status(pscolor);

        group = getalertgroups();
        if (group) sprintf(msgline, "status/group:%s ", group); else strcpy(msgline, "status ");
        addtostatus(msgline);

        sprintf(msgline, "%s.procs %s %s - Processes %s\n",
                commafy(hostname), colorname(pscolor),
                (timestr ? timestr : "<No timestamp data>"),
                ((pscolor == COL_GREEN) ? "OK" : "NOT ok"));
        addtostatus(msgline);

        /* And add the info about what's wrong */
        if (STRBUFLEN(monmsg)) {
                addtostrstatus(monmsg);
                addtostatus("\n");
        }

        /* And the full list of jobs for those who want it */
        if (pslistinprocs) {
                /*
                 * Format the list of virtual machines into four per line,
                 * this list could be fairly long.
                 */
                char *tmpstr, *tok;

                /*  Make a copy of psstr, strtok() will be changing it  */
                tmpstr = strdup(psstr);

                /*  Use strtok() to split string into pieces delimited by newline  */
                tok = strtok(tmpstr, "\n");

                while (tok) {
                        sprintf(msgline, "%s\n", tok);
                        addtostatus(msgline);
                        tok = strtok(NULL, "\n");
                }

                free(tmpstr);
        }

        if (fromline && !localmode) addtostatus(fromline);
        finish_status();

        freestrbuffer(monmsg);

        if (anycountdata) sendmessage(STRBUF(countdata), NULL, NULL, NULL, 0, BBTALK_TIMEOUT);
        clearstrbuffer(countdata);
}

void handle_zvse_client(char *hostname, char *clienttype, enum ostype_t os, 
			 void *hinfo, char *sender, time_t timestamp,
			 char *clientdata)
{
	char *timestr;
	char *cpuutilstr;
	char *pagingstr;
	char *uptimestr;
	char *clockstr;
	char *msgcachestr;
	char *dfstr;
	char *jobsstr;		/* z/VSE Running jobs  */
	char *portsstr;

	char fromline[1024];

	sprintf(fromline, "\nStatus message received from %s\n", sender);

	splitmsg(clientdata);

	timestr = getdata("date");
	uptimestr = getdata("uptime");
	cpuutilstr = getdata("cpu");
	pagingstr = getdata("paging");
	msgcachestr = getdata("msgcache");
	dfstr = getdata("df");
	jobsstr = getdata("jobs");
	portsstr = getdata("ports");

	zvse_cpu_report(hostname, clienttype, os, hinfo, fromline, timestr, cpuutilstr, uptimestr);
	zvse_paging_report(hostname, clienttype, os, hinfo, fromline, timestr, pagingstr);
	zvse_jobs_report(hostname, clienttype, os, hinfo, fromline, timestr, jobsstr);
	unix_disk_report(hostname, clienttype, os, hinfo, fromline, timestr, "Available", "Cap", "Mounted", dfstr);
	unix_ports_report(hostname, clienttype, os, hinfo, fromline, timestr, 3, 4, 5, portsstr);
	linecount_report(hostname, clienttype, os, hinfo, fromline, timestr);

}

