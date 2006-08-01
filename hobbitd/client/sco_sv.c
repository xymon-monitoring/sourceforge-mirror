/*----------------------------------------------------------------------------*/
/* Hobbit message daemon.                                                     */
/*                                                                            */
/* Client backend module for SCO_SV                                           */
/*                                                                            */
/* Copyright (C) 2005-2006 Henrik Storner <henrik@hswn.dk>                    */
/* Copyright (C) 2006 Charles Goyard                                          */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

static char sco_sv_rcsid[] = "$Id: sco_sv.c,v 1.1 2006-08-01 21:36:01 henrik Exp $";

void handle_sco_sv_client(char *hostname, char *clienttype, enum ostype_t os, 
			  namelist_t *hinfo, char *sender, time_t timestamp,
			  char *clientdata)
{
        char *timestr;
        char *uptimestr;
        char *clockstr;
        char *msgcachestr;
        char *whostr;
        char *psstr;
        char *topstr;
        char *dfstr;
        char *freestr;
        char *msgsstr;
        char *netstatstr;
        char *vmstatstr;
        char *ifstatstr;
        char *portsstr;
        char fromline[1024];

	sprintf(fromline, "\nStatus message received from %s\n", sender);

	splitmsg(clientdata);


        timestr = getdata("date");
        uptimestr = getdata("uptime");
        clockstr = getdata("clock");
        msgcachestr = getdata("msgcache");
        whostr = getdata("who");
        psstr = getdata("ps");
        topstr = getdata("top");
        dfstr = getdata("df");
        freestr = getdata("free");
        msgsstr = getdata("msgs");
        netstatstr = getdata("netstat");
        ifstatstr = getdata("ifstat");
        vmstatstr = getdata("vmstat");
        portsstr = getdata("ports");
	
	unix_cpu_report(hostname, clienttype, os, hinfo, fromline, timestr, uptimestr, clockstr, msgcachestr, whostr, psstr, topstr);
	unix_disk_report(hostname, clienttype, os, hinfo, fromline, timestr, "Available", "Capacity", "Mounted", dfstr);
	unix_procs_report(hostname, clienttype, os, hinfo, fromline, timestr, "COMMAND", NULL, psstr);
	unix_ports_report(hostname, clienttype, os, hinfo, fromline, timestr, 3, 4, 5, portsstr);

	msgs_report(hostname, clienttype, os, hinfo, fromline, timestr, msgsstr);
	file_report(hostname, clienttype, os, hinfo, fromline, timestr);
	linecount_report(hostname, clienttype, os, hinfo, fromline, timestr);

	unix_netstat_report(hostname, clienttype, os, hinfo, fromline, timestr, netstatstr);
	unix_ifstat_report(hostname, clienttype, os, hinfo, fromline, timestr, ifstatstr);
	unix_vmstat_report(hostname, clienttype, os, hinfo, fromline, timestr, vmstatstr);

}

