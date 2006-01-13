/*----------------------------------------------------------------------------*/
/* Hobbit CGI proxy.                                                          */
/*                                                                            */
/* This CGI can gateway a Hobbit/BB message sent via HTTP PORT to a Hobbit    */
/* server running on the local host.                                          */
/*                                                                            */
/* Copyright (C) 2002-2006 Henrik Storner <henrik@hswn.dk>                    */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

static char rcsid[] = "$Id: bbmessage.c,v 1.1 2006-01-13 13:52:06 henrik Exp $";

#include "libbbgen.h"

int main(int argc, char *argv[])
{
	int result = 1;
	cgidata_t *cgidata = NULL;

	cgidata = cgi_request();
	if (cgidata) {
		printf("Content-Type: application/octet-stream\n\n");
		result = sendmessage(cgidata->value, "127.0.0.1", stdout, NULL, 1, BBTALK_TIMEOUT);
	}

	return result;
}

