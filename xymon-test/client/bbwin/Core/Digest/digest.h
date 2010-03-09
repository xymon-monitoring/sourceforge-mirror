/*----------------------------------------------------------------------------*/
/* Hobbit monitor library.                                                    */
/*                                                                            */
/* This is used to implement the message digest functions.                    */
/*                                                                            */
/* Copyright (C) 2003-2006 Henrik Storner <henrik@hswn.dk>                    */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

#ifndef __DIGEST_H_
#define __DIGEST_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { D_MD5, D_SHA1, D_RMD160 } digesttype_t;

typedef struct digestctx_t {
	char *digestname;
	digesttype_t digesttype;
	void *mdctx;
} digestctx_t;

extern digestctx_t *digest_init(char *digest);
extern int digest_data(digestctx_t *ctx, unsigned char *buf, int buflen);
extern char *digest_done(digestctx_t *ctx);

#ifdef __cplusplus
}
#endif


#endif
