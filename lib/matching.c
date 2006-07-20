/*----------------------------------------------------------------------------*/
/* Hobbit monitor library.                                                    */
/*                                                                            */
/* This is a library module, part of libbbgen.                                */
/* It contains routines for matching names and expressions                    */
/*                                                                            */
/* Copyright (C) 2002-2006 Henrik Storner <henrik@storner.dk>                 */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

static char rcsid[] = "$Id: matching.c,v 1.7 2006-07-20 16:06:41 henrik Exp $";

#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#include <pcre.h>

#include "libbbgen.h"

static pcre *compileregex_opts(const char *pattern, int flags)
{
	pcre *result;
	const char *errmsg;
	int errofs;

	dbgprintf("Compiling regex %s\n", pattern);
	result = pcre_compile(pattern, flags, &errmsg, &errofs, NULL);
	if (result == NULL) {
		errprintf("pcre compile '%s' failed (offset %d): %s\n", pattern, errofs, errmsg);
		return NULL;
	}

	return result;
}

pcre *compileregex(const char *pattern)
{
	return compileregex_opts(pattern, PCRE_CASELESS);
}

pcre *multilineregex(const char *pattern)
{
	return compileregex_opts(pattern, PCRE_CASELESS|PCRE_MULTILINE);
}

int matchregex(char *needle, pcre *pcrecode)
{
	int ovector[30];
	int result;

	if (!needle || !pcrecode) return 0;

	result = pcre_exec(pcrecode, NULL, needle, strlen(needle), 0, 0, ovector, (sizeof(ovector)/sizeof(int)));
	return (result >= 0);
}

void freeregex(pcre *pcrecode)
{
	if (!pcrecode) return;

	pcre_free(pcrecode);
}

int namematch(char *needle, char *haystack, pcre *pcrecode)
{
	char *xhay;
	char *xneedle;
	char *match;
	int result = 0;

	if (pcrecode) {
		/* Do regex matching. The regex has already been compiled for us. */
		return matchregex(needle, pcrecode);
	}

	if (strcmp(haystack, "*") == 0) {
		/* Match anything */
		return 1;
	}

	/* Implement a simple, no-wildcard match */
	xhay = malloc(strlen(haystack) + 3);
	sprintf(xhay, ",%s,", haystack);
	xneedle = malloc(strlen(needle)+2);
	sprintf(xneedle, "%s,", needle);

	match = strstr(xhay, xneedle);
	if (match) {
		if (*(match-1) == '!') {
			/* Matched, but was a negative rule. */
			result = 0;
		}
		else if (*(match-1) == ',') {
			/* Matched */
			result = 1;
		}
		else {
			/* Matched a partial string. Fail. */
			result = 0;
		}
	}
	else {
		/* 
		 * It is not in the list. If the list is exclusively negative matches,
		 * we must return a positive result for "no match".
		 */
		char *p;

		/* Find the first name in the list that does not begin with a '!' */
		p = xhay+1;
		while (p && (*p == '!')) {
			p = strchr(p, ','); if (p) p++;
		}
		if (*p == '\0') result = 1;
	}

	xfree(xhay);
	xfree(xneedle);

	return result;
}

int timematch(char *tspec)
{
	int result;

	result = within_sla(tspec, 0);

	return result;
}


