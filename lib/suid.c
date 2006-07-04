/*----------------------------------------------------------------------------*/
/* Hobbit monitor library.                                                    */
/*                                                                            */
/* This is a library module, part of libbbgen.                                */
/* It contains routines for handling UID changes.                             */
/*                                                                            */
/* Copyright (C) 2006 Henrik Storner <henrik@hswn.dk>                         */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

static char rcsid[] = "$Id: suid.c,v 1.1 2006-07-04 11:31:34 henrik Exp $";

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "libbbgen.h"

static uid_t myuid = -1;

#ifdef HPUX

void drop_root(void)
{
	if (myuid == -1) myuid = getuid();
	setresuid(-1, myuid, -1);
}

void get_root(void)
{
	setresuid(-1, 0, -1);
}

#else

void drop_root(void)
{
	if (myuid == -1) myuid = getuid();
	seteuid(myuid);
}

void get_root(void)
{
	seteuid(0);
}

#endif

void drop_root_and_removesuid(char *fn)
{
	struct stat st;

	if ( (stat(fn, &st) == 0)    &&
	     (st.st_mode & S_ISUID)  &&
	     (st.st_uid == 0)          ) {

		/* We now know that fn is suid-root */
		chmod(fn, (st.st_mode & (~S_ISUID)));
	}

	drop_root();
}

