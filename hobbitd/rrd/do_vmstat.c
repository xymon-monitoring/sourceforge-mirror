/*----------------------------------------------------------------------------*/
/* Big Brother message daemon.                                                */
/*                                                                            */
/* Copyright (C) 2004 Henrik Storner <henrik@hswn.dk>                         */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

static char vmstat_rcsid[] = "$Id: do_vmstat.c,v 1.9 2005-01-25 17:53:45 henrik Exp $";

typedef struct vmstat_layout_t {
	int index;
	char *name;
} vmstat_layout_t;

/* This one matches the vmstat output from Solaris 8, possibly earlier ones as well */
static vmstat_layout_t vmstat_solaris_layout[] = {
	{ 0, "cpu_r" },
	{ 1, "cpu_b" },
	{ 2, "cpu_w" },
	{ 3, "mem_swap" },
	{ 4, "mem_free" },
	{ 5, "mem_re" },
	{ 6, "mem_mf" },
	{ 7, "mem_pi" },
	{ 8, "mem_po" },
	{ 11, "sr" },
	{ 16, "cpu_int" },
	{ 17, "cpu_syc" },
	{ 18, "cpu_csw" },
	{ 19, "cpu_usr" },
	{ 20, "cpu_sys" },
	{ 21, "cpu_idl" },
	{ -1, NULL }
};

/* This one for OSF, taken from LARRD 0.43c */
static vmstat_layout_t vmstat_osf_layout[] = {
	{ 0, "cpu_r" },
	{ 1, "cpu_b" },
	{ 2, "cpu_w" },
	{ 4, "mem_free" },
	{ 6, "mem_mf" },
	{ 10, "mem_pi" },
	{ 11, "mem_po" },
	{ 12, "cpu_int" },
	{ 13, "cpu_syc" },
	{ 14, "cpu_csw" },
	{ 15, "cpu_usr" },
	{ 16, "cpu_sys" },
	{ 17, "cpu_idl" },
	{ -1, NULL }
};

/* This one for AIX, taken from LARRD 0.43c */
static vmstat_layout_t vmstat_aix_layout[] = {
	{ 0, "cpu_r" },
	{ 1, "cpu_b" },
	{ 2, "mem_avm" },
	{ 3, "mem_free" },
	{ 4, "mem_re" },
	{ 5, "mem_pi" },
	{ 6, "mem_po" },
	{ 7, "mem_fr" },
	{ 8, "sr" },
	{ 9, "mem_cy" },
	{ 10, "cpu_int" },
	{ 11, "cpu_syc" },
	{ 12, "cpu_csw" },
	{ 13, "cpu_usr" },
	{ 14, "cpu_sys" },
	{ 15, "cpu_idl" },
	{ 16, "cpu_wait" },
	{ -1, NULL }
};

/* This one for HP/UX, taken from LARRD 0.43c */
static vmstat_layout_t vmstat_hpux_layout[] = {
	{ 0, "cpu_r" },
	{ 1, "cpu_b" },
	{ 2, "cpu_w" },
	{ 3, "mem_avm" },
	{ 4, "mem_free" },
	{ 5, "mem_re" },
	{ 6, "mem_flt" },
	{ 7, "mem_pi" },
	{ 8, "mem_po" },
	{ 9, "mem_fr" },
	{ 11, "sr" },
	{ 12, "cpu_int" },
	{ 14, "cpu_csw" },
	{ 15, "cpu_usr" },
	{ 16, "cpu_sys" },
	{ 17, "cpu_idl" },
	{ -1, NULL }
};

/* This one is all newer Linux procps versions, with kernel 2.4+ */
static vmstat_layout_t vmstat_linux_layout[] = {
	{ 0, "cpu_r" },
	{ 1, "cpu_b" },
	{ -1, "cpu_w" },	/* Not present for 2.4+ kernels, so log as "Undefined" */
	{ 2, "mem_swpd" },
	{ 3, "mem_free" },
	{ 4, "mem_buff" },
	{ 5, "mem_cach" },
	{ 6, "mem_si" },
	{ 7, "mem_so" },
	{ 8, "dsk_bi" },
	{ 9, "dsk_bo" },
	{ 10, "cpu_int" },
	{ 11, "cpu_csw" },
	{ 12, "cpu_usr" },
	{ 13, "cpu_sys" },
	{ 14, "cpu_idl" },
	{ 15, "cpu_wait"  },	/* Requires kernel 2.6, but may not be present */
	{ -1, NULL }
};

/* This one is for Debian 3.0 (Woody), and possibly others with a Linux 2.2 kernel */
static vmstat_layout_t vmstat_debian3_layout[] = {
	{ 0, "cpu_r" },
	{ 1, "cpu_b" },
	{ 2, "cpu_w" },
	{ 3, "mem_swpd" },
	{ 4, "mem_free" },
	{ 5, "mem_buff" },
	{ 6, "mem_cach" },
	{ 7, "mem_si" },
	{ 8, "mem_so" },
	{ 9, "dsk_bi" },
	{ 10, "dsk_bo" },
	{ 11, "cpu_int" },
	{ 12, "cpu_csw" },
	{ 13, "cpu_usr" },
	{ 14, "cpu_sys" },
	{ 15, "cpu_idl" },
	{ -1, "cpu_wait" },
	{ -1, NULL }
};

/* This one matched FreeBSD 4.10 */
static vmstat_layout_t vmstat_freebsd_layout[] = {
	{ 0, "cpu_r" },
	{ 1, "cpu_b" },
	{ 2, "cpu_w" },
	{ 3, "mem_avm" },
	{ 4, "mem_free" },
	{ 5, "mem_flt" },
	{ 6, "mem_re" },
	{ 7, "mem_pi" },
	{ 8, "mem_po" },
	{ 9, "mem_fr" },
	{ 10, "sr" },
	{ 11, "dsk_da0" },
	{ 12, "dsk_fd0" },
	{ 13, "cpu_int" },
	{ 15, "cpu_csw" },
	{ 16, "cpu_sys" },
	{ 17, "cpu_usr" },
	{ 18, "cpu_idl" },
	{ -1, NULL }
};

#define MAX_VMSTAT_VALUES 30

int do_vmstat_larrd(char *hostname, char *testname, char *msg, time_t tstamp)
{
	enum ostype_t ostype;
	vmstat_layout_t *layout = NULL;
	char *datapart = msg;
	int values[MAX_VMSTAT_VALUES];
	int defcount, defidx, datacount, result;
	char *p;
	char **creparams;

	if ((strncmp(msg, "status", 6) == 0) || (strncmp(msg, "data", 4) == 0)) {
		/* Full message, including "status" or "data" line - so skip the first line. */
		datapart = strchr(msg, '\n');
		if (datapart) {
			datapart++; 
		}
		else {
			errprintf("Too few lines (only 1) in vmstat report from %s\n", hostname);
			return -1;
		}
	}

	ostype = get_ostype(datapart);
	datapart = strchr(datapart, '\n');
	if (datapart) {
		datapart++; 
	}
	else {
		errprintf("Too few lines (only 1 or 2) in vmstat report from %s\n", hostname);
		return -1;
	}

	/* Pick up the values in the datapart line. Stop at newline. */
	p = strchr(datapart, '\n'); if (p) *p = '\0';
	p = strtok(datapart, " "); datacount = 0;
	while (p && (datacount < MAX_VMSTAT_VALUES)) {
		values[datacount++] = atoi(p);
		p = strtok(NULL, " ");
	}

	switch (ostype) {
	  case OS_SOLARIS: 
		layout = vmstat_solaris_layout; break;
	  case OS_OSF:
		layout = vmstat_osf_layout; break;
	  case OS_AIX: 
		layout = vmstat_aix_layout; break;
	  case OS_HPUX: 
		layout = vmstat_hpux_layout; break;
	  case OS_LINUX:
	  case OS_REDHAT:
	  case OS_DEBIAN:
		layout = vmstat_linux_layout;
		break;
	  case OS_DEBIAN3:
		layout = vmstat_debian3_layout; break;
	  case OS_FREEBSD:
		layout = vmstat_freebsd_layout; break;
	  case OS_SCO:
		errprintf("Cannot handle sco vmstat from host '%s' \n", hostname);
		return -1;
	  case OS_SNMP:
		errprintf("Cannot handle SNMP vmstat from host '%s' \n", hostname);
		return -1;
	  case OS_WIN32:
		errprintf("Cannot handle Win32 vmstat from host '%s' \n", hostname);
		return -1;
	  case OS_UNKNOWN:
		errprintf("Host '%s' reports vmstat for an unknown OS\n", hostname);
		return -1;
	}

	if (layout == NULL) {
		return -1;
	}

	/* How many values are defined in the dataset ? */
	for (defcount = 0; (layout[defcount].name); defcount++) ;

	/* Setup the create-parameters */
	creparams = (char **)xmalloc((defcount+7)*sizeof(char *));
	creparams[0] = "rrdcreate";
	creparams[1] = rrdfn;
	for (defidx=0; (defidx < defcount); defidx++) {
		creparams[2+defidx] = (char *)xmalloc(strlen(layout[defidx].name) + strlen("DS::GAUGE:600:0:U") + 1);
		sprintf(creparams[2+defidx], "DS:%s:GAUGE:600:0:U", layout[defidx].name);
	}
	creparams[2+defcount+0] = rra1;
	creparams[2+defcount+1] = rra2;
	creparams[2+defcount+2] = rra3;
	creparams[2+defcount+3] = rra4;
	creparams[2+defcount+4] = NULL;

	/* Setup the update string, picking out values according to the layout */
	p = rrdvalues + sprintf(rrdvalues, "%d", (int)tstamp);
	for (defidx=0; (defidx < defcount); defidx++) {
		int dataidx = layout[defidx].index;

		if ((dataidx > datacount) || (dataidx == -1)) {
			p += sprintf(p, ":U");
		}
		else {
			p += sprintf(p, ":%d", values[layout[defidx].index]);
		}
	}

	sprintf(rrdfn, "vmstat.rrd");
	result = create_and_update_rrd(hostname, rrdfn, creparams, update_params);

	for (defidx=0; (defidx < defcount); defidx++) xfree(creparams[2+defidx]);
	xfree(creparams);

	return result;
}

