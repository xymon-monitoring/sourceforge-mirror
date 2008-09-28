/*----------------------------------------------------------------------------*/
/* Hobbit monitor library.                                                    */
/*                                                                            */
/* This is a library module, part of libbbgen.                                */
/* It contains routines for sending and receiving data to/from the BB daemon  */
/*                                                                            */
/* Copyright (C) 2002-2008 Henrik Storner <henrik@storner.dk>                 */
/*                                                                            */
/* This program is released under the GNU General Public License (GPL),       */
/* version 2. See the file "COPYING" for details.                             */
/*                                                                            */
/*----------------------------------------------------------------------------*/

static char rcsid[] = "$Id: sendmsg.c,v 1.89 2008/04/29 08:54:55 henrik Exp henrik $";

#include "config.h"

#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#include <errno.h>
#include <netdb.h>
#include <fcntl.h>
#include <stdio.h>

#ifdef HAVE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

#include "libbbgen.h"

#define BBSENDRETRIES 2

/* These commands go to BBDISPLAYS */
static char *multircptcmds[] = { "status", "combo", "meta", "data", "notify", "enable", "disable", "drop", "rename", "client", NULL };

/* Stuff for combo message handling */
int		bbmsgcount = 0;		/* Number of messages transmitted */
int		bbstatuscount = 0;	/* Number of status items reported */
int		bbnocombocount = 0;	/* Number of status items reported outside combo msgs */
static int	bbmsgqueued;		/* Anything in the buffer ? */
static strbuffer_t *bbmsg = NULL;	/* Complete combo message buffer */
static strbuffer_t *msgbuf = NULL;	/* message buffer for one status message */
static int	msgcolor;		/* color of status message in msgbuf */
static int      maxmsgspercombo = 100;	/* 0 = no limit. 100 is a reasonable default. */
static int      sleepbetweenmsgs = 0;
static int      bbdportnumber = 0;
static char     *bbdispproxyhost = NULL;
static int      bbdispproxyport = 0;
static char	*proxysetting = NULL;

static int	bbmetaqueued;		/* Anything in the buffer ? */
static strbuffer_t *metamsg = NULL;	/* Complete meta message buffer */
static strbuffer_t *metabuf = NULL;	/* message buffer for one meta message */

int dontsendmessages = 0;
int sendcompressedmessages = 0;
int sendssl = 0;
char *sslcertfn = NULL;
char *sslkeyfn = NULL;

#ifdef HAVE_OPENSSL
static SSL_METHOD *sslmethod = NULL;
static SSL_CTX *sslctx = NULL;
static SSL *sslobj = NULL;
#endif

typedef enum { 	TALK_DONE, 					/* Done */
		TALK_INIT_CONNECTION, TALK_CONNECTING, 		/* Connecting to peer */
		TALK_SEND, TALK_RECEIVE, 			/* Un-encrypted data exchange */
#ifdef HAVE_OPENSSL
		TALK_SSLSEND, TALK_SSLRECEIVE, 			/* Encrypted data exchange */
		TALK_SEND_STARTTLS,				/* We're requesting switch to SSL */
		TALK_SSLREAD_SETUP, TALK_SSLWRITE_SETUP,	/* SSL read/write during initial handshake */
		TALK_SSLREAD_SEND, TALK_SSLWRITE_SEND, 		/* SSL read/write during TALK_SEND */
		TALK_SSLREAD_RECEIVE, TALK_SSLWRITE_RECEIVE, 	/* SSL read/write during TALK_RECEIVE */
#endif
} talkstate_t;

static char *tsstrings[TALK_SSLWRITE_RECEIVE+1] = {
	"talk_done", 
	"talk_init_connection", "talk_connecting", 
	"talk_send", "talk_receive",
	"talk_sslsend", "talk_sslreceive",
	"talk_send_starttls",
	"talk_sslread_setup", "talk_sslwrite_setup",
	"talk_sslread_send", "talk_sslwrite_send",
	"talk_sslread_receive", "talk_sslwrite_receive"
};

static int sslinitialize(void)
{
	static int initdone = 0;

	if (initdone) return 0;

#ifdef HAVE_OPENSSL
	/* Setup OpenSSL */
	SSL_load_error_strings();
	SSL_library_init();
	sslmethod = SSLv23_client_method();
	sslctx = SSL_CTX_new(sslmethod);
	if (sslctx == NULL) {
		errprintf("Cannot create SSL context\n");
		ERR_print_errors_fp(stderr);
		return 1;
	}
	SSL_CTX_set_options(sslctx, SSL_OP_NO_SSLv2);

	/* Load a client certificate if there is one */
	if (sslcertfn && sslkeyfn) {
		if (SSL_CTX_use_certificate_chain_file(sslctx, sslcertfn) <= 0) {
			errprintf("Cannot load SSL certificate\n");
			ERR_print_errors_fp(stderr);
			return 1;
		}
		if (SSL_CTX_use_PrivateKey_file(sslctx, sslkeyfn, SSL_FILETYPE_PEM) <= 0) {
			errprintf("Cannot load SSL private key\n");
			ERR_print_errors_fp(stderr);
			return 1;
		}
		if (!SSL_CTX_check_private_key(sslctx)) {
			errprintf("Invalid private key, does not match certificate\n");
			return 1;
		}
	}

	initdone = 1;
#endif

	return 0;
}


static void closesocket(int fd)
{
#ifdef HAVE_OPENSSL
	if (sslobj) {
		SSL_shutdown(sslobj);
		SSL_free(sslobj);
		sslobj = NULL;
	}
#endif

	shutdown(fd, SHUT_RDWR);
	close(fd);
}

void setproxy(char *proxy)
{
	if (proxysetting) xfree(proxysetting);
	proxysetting = strdup(proxy);
}

static void setup_transport(char *recipient)
{
	static int transport_is_setup = 0;
	int default_port;

	if (transport_is_setup) return;
	transport_is_setup = 1;

	if (strcmp(recipient, "local") == 0) {
		dbgprintf("Using local Unix domain socket transport\n");
		return;
	}

	if (strncmp(recipient, "http://", 7) == 0) {
		/*
		 * Send messages via http. This requires e.g. a CGI on the webserver to
		 * receive the POST we do here.
		 */
		default_port = 80;

		if (proxysetting == NULL) proxysetting = getenv("http_proxy");
		if (proxysetting) {
			char *p;

			bbdispproxyhost = strdup(proxysetting);
			if (strncmp(bbdispproxyhost, "http://", 7) == 0) bbdispproxyhost += strlen("http://");
 
			p = strchr(bbdispproxyhost, ':');
			if (p) {
				*p = '\0';
				p++;
				bbdispproxyport = atoi(p);
			}
			else {
				bbdispproxyport = 8080;
			}
		}
	}
	else {
		/* 
		 * Non-HTTP transport - lookup portnumber in both BBPORT env.
		 * and the "bbd" entry from /etc/services.
		 */
		default_port = 1984;

		if (xgetenv("BBPORT")) bbdportnumber = atoi(xgetenv("BBPORT"));
	
	
		/* Next is /etc/services "bbd" entry */
		if ((bbdportnumber <= 0) || (bbdportnumber > 65535)) {
			struct servent *svcinfo;

			svcinfo = getservbyname("bbd", NULL);
			if (svcinfo) bbdportnumber = ntohs(svcinfo->s_port);
		}
	}

	/* Last resort: The default value */
	if ((bbdportnumber <= 0) || (bbdportnumber > 65535)) {
		bbdportnumber = default_port;
	}

	dbgprintf("Transport setup is:\n");
	dbgprintf("bbdportnumber = %d\n", bbdportnumber),
	dbgprintf("bbdispproxyhost = %s\n", (bbdispproxyhost ? bbdispproxyhost : "NONE"));
	dbgprintf("bbdispproxyport = %d\n", bbdispproxyport);
}

static talkstate_t process_receive(char *recvbuf, int n, sendreturn_t *response, talkstate_t currstate)
{
	strbuffer_t *cmprbuf;
	char *outp;

	if (n <= 0) return TALK_DONE;

	dbgprintf("Read %d bytes\n", n);
	recvbuf[n] = '\0';

	/*
	 * When running over a HTTP transport, we must strip
	 * off the HTTP headers we get back, so the response
	 * is consistent with what we get from the normal bbd
	 * transport.
	 * (Non-http transport sets "haveseenhttphdrs" to 1)
	 */
	if (!response->haveseenhttphdrs) {
		outp = strstr(recvbuf, "\r\n\r\n");
		if (outp) {
			outp += 4;
			n -= (outp - recvbuf);
			response->haveseenhttphdrs = 1;
		}
		else n = 0;
	}
	else outp = recvbuf;

	if (n <= 0) return currstate; /* Ignore data if we're still inside http headers */

	if (response->respfd) {
		switch (response->cmprhandling) {
			case CMP_NONE:
				fwrite(outp, n, 1, response->respfd);
				break;

			case CMP_LOOKFORMARKER:
				if (strncmp(outp, compressionmarker, compressionmarkersz) != 0) {
					response->cmprhandling = CMP_NONE;
					fwrite(outp, n, 1, response->respfd);
					break;
				}
				else {
					response->cmprhandle = uncompress_stream_init();
					response->cmprhandling = CMP_DECOMPRESS;
					outp += 4;
					n -= 4;
					cmprbuf = uncompress_stream_data(response->cmprhandle, outp, n);
					fwrite(STRBUF(cmprbuf), STRBUFLEN(cmprbuf), 1, response->respfd);
					freestrbuffer(cmprbuf);
				}
				break;

			case CMP_DECOMPRESS:
				cmprbuf = uncompress_stream_data(response->cmprhandle, outp, n);
				fwrite(STRBUF(cmprbuf), STRBUFLEN(cmprbuf), 1, response->respfd);
				freestrbuffer(cmprbuf);
				break;
		}
	}
	else if (response->respstr) {
		addtobufferraw(response->respstr, recvbuf, n);
	}

	if (!response->fullresponse) {
		return (strchr(outp, '\n') == NULL) ? currstate : TALK_DONE;
	}

	return currstate;
}


static int sendtobbd(char *recipient, char *message, int timeout, sendreturn_t *response)
{
	struct in_addr addr;
	enum { C_IP, C_UNIX } conntype = C_IP;
	int	sockfd;
	int	res;
	char *msgptr = message;
	int msgremain = strlen(message);
	char *rcptip = NULL;
	int rcptport = 0;
	int connretries = BBSENDRETRIES;
	char *httpmessage = NULL;
	char recvbuf[32768];
	char starttlsbuf[20];
	char *starttlsptr = starttlsbuf;
	talkstate_t talkstate = TALK_DONE, postconnectstate = TALK_DONE, postsendstate = TALK_DONE;
	int talkresult = BB_OK;

	if (dontsendmessages && !response) {
		printf("%s\n", message);
		return BB_OK;
	}

	setup_transport(recipient);
	dbgprintf("Recipient listed as '%s'\n", recipient);

	if (strcmp(recipient, "local") == 0) {
		/* Connect via local unix domain socket $BBTMP/hobbitd_if */
		dbgprintf("Unix domain protocol\n");
		conntype = C_UNIX;
		rcptip = strdup("local/unix");
		postconnectstate = TALK_SEND;
	}
	else if ((strncmp(recipient, "http://", 7) != 0) && (strncmp(recipient, "https://", 8) != 0)) {
		/* Hobbit protocol, directly to hobbitd */
		char *p;

		rcptip = strdup(recipient);
		rcptport = bbdportnumber;
		p = strchr(rcptip, ':');
		if (p) {
			*p = '\0'; p++; rcptport = atoi(p);
		}
		dbgprintf("Hobbit protocol on port %d\n", rcptport);

		if (sendcompressedmessages) {
			static strbuffer_t *cbuf = NULL;

			if (cbuf) freestrbuffer(cbuf);

			cbuf = compress_buffer(message, msgremain);
			if (cbuf) {
				msgremain = STRBUFLEN(cbuf);
				msgptr = message = STRBUF(cbuf);
				response->cmprhandling = CMP_LOOKFORMARKER;
			}
		}

#ifdef HAVE_OPENSSL
		postconnectstate = (sendssl ? TALK_SEND_STARTTLS : TALK_SEND);
#else
		postconnectstate = TALK_SEND;
#endif

	}
	else {
		char *bufp;
		char *posturl = NULL;
		char *posthost = NULL;

		if (bbdispproxyhost == NULL) {
			char *p;

			/*
			 * No proxy. "recipient" is "http://host[:port]/url/for/post"
			 * Strip off "http://", and point "posturl" to the part after the hostname.
			 * If a portnumber is present, strip it off and update rcptport.
			 */
			rcptip = strdup(recipient+strlen("http://"));
			rcptport = bbdportnumber;

			p = strchr(rcptip, '/');
			if (p) {
				posturl = strdup(p);
				*p = '\0';
			}

			p = strchr(rcptip, ':');
			if (p) {
				*p = '\0';
				p++;
				rcptport = atoi(p);
			}

			posthost = strdup(rcptip);

			dbgprintf("BB-HTTP protocol directly to host %s\n", posthost);
		}
		else {
			char *p;

			/*
			 * With proxy. The full "recipient" must be in the POST request.
			 */
			rcptip = strdup(bbdispproxyhost);
			rcptport = bbdispproxyport;

			posturl = strdup(recipient);

			p = strchr(recipient + strlen("http://"), '/');
			if (p) {
				*p = '\0';
				posthost = strdup(recipient + strlen("http://"));
				*p = '/';

				p = strchr(posthost, ':');
				if (p) *p = '\0';
			}

			dbgprintf("BB-HTTP protocol via proxy to host %s\n", posthost);
		}

		if ((posturl == NULL) || (posthost == NULL)) {
			errprintf("Unable to parse HTTP recipient\n");
			return BB_EBADURL;
		}

		bufp = msgptr = httpmessage = malloc(strlen(message)+1024);
		bufp += sprintf(httpmessage, "POST %s HTTP/1.0\n", posturl);
		bufp += sprintf(bufp, "MIME-version: 1.0\n");
		bufp += sprintf(bufp, "Content-Type: application/octet-stream\n");
		bufp += sprintf(bufp, "Content-Length: %d\n", strlen(message));
		bufp += sprintf(bufp, "Host: %s\n", posthost);
		bufp += sprintf(bufp, "\n%s", message);
		msgremain = (bufp - msgptr);

		if (posturl) xfree(posturl);
		if (posthost) xfree(posthost);
		response->haveseenhttphdrs = 0;

		dbgprintf("BB-HTTP message is:\n%s\n", httpmessage);

		postconnectstate = TALK_SEND;
	}

	if (conntype == C_IP) {
		/* Setup SSL (if we use it), bail out if it fails */
		if (sslinitialize() != 0) BB_ECONNFAILED;

		if (inet_aton(rcptip, &addr) == 0) {
			/* recipient is not an IP - do DNS lookup */

			struct hostent *hent;
			char hostip[IP_ADDR_STRLEN];

			hent = gethostbyname(rcptip);
			if (hent) {
				memcpy(&addr, *(hent->h_addr_list), sizeof(struct in_addr));
				strcpy(hostip, inet_ntoa(addr));

				if (inet_aton(hostip, &addr) == 0) return BB_EBADIP;
			}
			else {
				errprintf("Cannot determine IP address of message recipient %s\n", rcptip);
				return BB_EIPUNKNOWN;
			}
		}
	}

	talkstate = TALK_INIT_CONNECTION;
	postsendstate = TALK_DONE;
	if (response && (response->respstr || response->respfd)) postsendstate = TALK_RECEIVE;
#ifdef HAVE_OPENSSL
	if (sendssl && (postsendstate == TALK_RECEIVE)) postsendstate = TALK_SSLRECEIVE;
	/* NB: The "starttls" line MUST be 18 bytes, since this is what the daemon expects. */
	sprintf(starttlsbuf, "starttls %08d\n", msgremain);
#endif
	while (talkstate != TALK_DONE) {
		fd_set readfds, writefds;
		struct timeval tmo;

		FD_ZERO(&writefds);
		FD_ZERO(&readfds);

		dbgprintf("talkstate=%s\n", tsstrings[talkstate]);
		switch (talkstate) {
		  case TALK_DONE:
			/* Cannot happen ... */
			break;

		  case TALK_INIT_CONNECTION:
			dbgprintf("Will connect to address %s port %d\n", rcptip, rcptport);

			if (conntype == C_IP) {
				struct sockaddr_in saddr;

				memset(&saddr, 0, sizeof(saddr));
				saddr.sin_family = AF_INET;
				saddr.sin_addr.s_addr = addr.s_addr;
				saddr.sin_port = htons(rcptport);

				/* Get a non-blocking socket */
				sockfd = socket(PF_INET, SOCK_STREAM, 0);
				if (sockfd == -1) return BB_ENOSOCKET;
				res = fcntl(sockfd, F_SETFL, O_NONBLOCK);
				if (res != 0) return BB_ECANNOTDONONBLOCK;

				res = connect(sockfd, (struct sockaddr *)&saddr, sizeof(saddr));
			}
			else {
				struct sockaddr_un localaddr;

				memset(&localaddr, 0, sizeof(localaddr));
				localaddr.sun_family = AF_UNIX;
				sprintf(localaddr.sun_path, "%s/hobbitd_if", xgetenv("BBTMP"));
		
				/* Get a non-blocking socket */
				sockfd = socket(PF_UNIX, SOCK_STREAM, 0);
				if (sockfd == -1) return BB_ENOSOCKET;
				res = fcntl(sockfd, F_SETFL, O_NONBLOCK);
				if (res != 0) return BB_ECANNOTDONONBLOCK;
		
				res = connect(sockfd, (struct sockaddr *)&localaddr, sizeof(localaddr));
			}

			if ((res == -1) && (errno != EINPROGRESS)) {
				errprintf("connect to hobbitd failed - %s\n", strerror(errno));
				talkstate = TALK_DONE;
				talkresult = BB_ECONNFAILED;
			}
			talkstate = TALK_CONNECTING;
			FD_SET(sockfd, &writefds); break;
			break;

		  case TALK_CONNECTING:
			FD_SET(sockfd, &writefds); break;
			break;

		  case TALK_SEND:
			FD_SET(sockfd, &writefds); break;
			break;

		  case TALK_RECEIVE:
			FD_SET(sockfd, &readfds); break;
			break;

#ifdef HAVE_OPENSSL
		  case TALK_SEND_STARTTLS:
		  case TALK_SSLSEND:
			FD_SET(sockfd, &writefds); break;
			break;

		  case TALK_SSLRECEIVE:
			FD_SET(sockfd, &readfds); break;
			break;

		  case TALK_SSLWRITE_SETUP:
		  case TALK_SSLWRITE_SEND:
		  case TALK_SSLWRITE_RECEIVE:
			FD_SET(sockfd, &writefds); break;
			break;

		  case TALK_SSLREAD_SETUP:
		  case TALK_SSLREAD_SEND:
		  case TALK_SSLREAD_RECEIVE:
			FD_SET(sockfd, &readfds); break;
			break;
#endif
		}

		tmo.tv_sec = timeout;  tmo.tv_usec = 0;
		res = select(sockfd+1, &readfds, &writefds, NULL, (timeout ? &tmo : NULL));

		/* Handle special error cases first: select() failures and timeouts */
		if (res == -1) {
			errprintf("Select failure while talking to hobbitd@%s:%d!\n", rcptip, rcptport);
			closesocket(sockfd);
			talkstate = TALK_DONE;
			talkresult = BB_ESELFAILED;
		}
		else if (res == 0) {
			/* Timeout! */

			if ((talkstate == TALK_CONNECTING) && (connretries > 0)) {
				dbgprintf("Timeout while connecting to hobbitd@%s:%d - retrying\n", rcptip, rcptport);
				closesocket(sockfd);
				connretries--;
				if (connretries > 0) {
					sleep(1);
					talkstate = TALK_INIT_CONNECTION;
				}
				else {
					talkstate = TALK_DONE;
					talkresult = BB_ETIMEOUT;
				}
			}
			else {
				talkstate = TALK_DONE;
				talkresult = BB_ETIMEOUT;
			}
		}

		switch (talkstate) {
		  case TALK_DONE:
		  case TALK_INIT_CONNECTION:
			break;

		  case TALK_CONNECTING:
			if (!FD_ISSET(sockfd, &writefds)) break;
			/* Havent seen our connect() status yet - must be now */
			{
				int connres;
				socklen_t connressize = sizeof(connres);

				res = getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &connres, &connressize);
				dbgprintf("Connect status is %d\n", connres);
				if (connres != 0) {
					errprintf("Could not connect to hobbitd@%s:%d - %s\n", 
						  rcptip, rcptport, strerror(connres));
					talkstate = TALK_DONE;
					talkresult = BB_ECONNFAILED;
				}
				else {
					talkstate = postconnectstate;
				}
			}
			break;

		  case TALK_SEND:
			if (!FD_ISSET(sockfd, &writefds)) break;
			res = write(sockfd, msgptr, msgremain);
			if (res < 0) {
				errprintf("Write error while sending message to hobbitd@%s:%d\n", rcptip, rcptport);
				talkstate = TALK_DONE;
				talkresult = BB_EWRITEERROR;
			}
			else {
				msgptr += res;
				msgremain -= res;
				if (msgremain == 0) {
					shutdown(sockfd, SHUT_WR);	/* Needed to signal "no more data from client" */
					talkstate = postsendstate;
				}
			}
			break;

		  case TALK_RECEIVE:
			if (!FD_ISSET(sockfd, &readfds)) break;
			res = read(sockfd,  recvbuf, sizeof(recvbuf)-1);
			if (res < 0) {
				errprintf("Read error while reading response from hobbitd@%s:%d\n", rcptip, rcptport);
				talkstate = TALK_DONE;
				talkresult = BB_EREADERROR;
			}
			else if (res == 0) {
				talkstate = TALK_DONE;
				talkresult = BB_OK;
			}
			else {
				talkstate = process_receive(recvbuf, res, response, talkstate);
			}
			break;


#ifdef HAVE_OPENSSL

		  case TALK_SEND_STARTTLS:
			if (!FD_ISSET(sockfd, &writefds)) break;
			res = write(sockfd, starttlsptr, strlen(starttlsptr));
			if (res == -1) {
				errprintf("Write error while sending STARTTLS to hobbitd@%s:%d\n", rcptip, rcptport);
				talkstate = TALK_DONE;
				talkresult = BB_EWRITEERROR;
			}
			else {
				starttlsptr += res;
				if (*starttlsptr == '\0') {
					talkstate = TALK_SSLREAD_SETUP;
					sslobj = SSL_new(sslctx);
					SSL_set_fd(sslobj, sockfd);
					SSL_set_connect_state(sslobj);

					/*
					 * We must begin the SSL handshake right away,
					 * but just falling through won't work because the
					 * FD_ISSET() checks will prevent us from reaching
					 * SSL_do_handshake(). Hence the <gasp!> goto ..
					 */
					goto letsdossl;
				}
			}
			break;

		  case TALK_SSLWRITE_SETUP:
		  case TALK_SSLREAD_SETUP:
			if ((talkstate == TALK_SSLWRITE_SETUP) && (!FD_ISSET(sockfd, &writefds))) break;
			if ((talkstate == TALK_SSLREAD_SETUP) && (!FD_ISSET(sockfd, &readfds))) break;
letsdossl:
			res = SSL_do_handshake(sslobj);
			if (res == 1) {
				/* SSL handshake completed */
				talkstate = TALK_SSLSEND;
			}
			else {
				char errmsg[120];

				switch (SSL_get_error(sslobj, res)) {
				  case SSL_ERROR_WANT_READ:
					talkstate = TALK_SSLREAD_SETUP;
					break;
				  case SSL_ERROR_WANT_WRITE:
					talkstate = TALK_SSLWRITE_SETUP;
					break;
				  default:
					ERR_error_string(res, errmsg);
					errprintf("SSL handshake error: %s\n", errmsg);
					talkstate = TALK_DONE;
					talkresult = BB_EWRITEERROR;
					break;
				}
			}
			break;

		  case TALK_SSLWRITE_SEND:
		  case TALK_SSLREAD_SEND:
		  case TALK_SSLSEND:
			if ((talkstate == TALK_SSLWRITE_SEND) && (!FD_ISSET(sockfd, &writefds))) break;
			if ((talkstate == TALK_SSLREAD_SEND) && (!FD_ISSET(sockfd, &readfds))) break;
			if ((talkstate == TALK_SSLSEND) && (!FD_ISSET(sockfd, &writefds))) break;
			res = SSL_write(sslobj, msgptr, msgremain);
			if (res <= 0) {
				switch (SSL_get_error(sslobj, res)) {
				  case SSL_ERROR_WANT_READ:
					talkstate = TALK_SSLREAD_SEND;
					break;
				  case SSL_ERROR_WANT_WRITE:
					talkstate = TALK_SSLWRITE_SEND;
					break;
				  case SSL_ERROR_ZERO_RETURN:
					break;
				  default:
					errprintf("SSL send error\n");
					talkstate = TALK_DONE;
					talkresult = BB_EWRITEERROR;
					break;
				}
			}
			else {
				msgptr += res;
				msgremain -= res;
				if (msgremain == 0) talkstate = postsendstate;
			}
			break;

		  case TALK_SSLWRITE_RECEIVE:
		  case TALK_SSLREAD_RECEIVE:
		  case TALK_SSLRECEIVE:
			if ((talkstate == TALK_SSLWRITE_RECEIVE) && (!FD_ISSET(sockfd, &writefds))) break;
			if ((talkstate == TALK_SSLREAD_RECEIVE) && (!FD_ISSET(sockfd, &readfds))) break;
			if ((talkstate == TALK_SSLRECEIVE) && (!FD_ISSET(sockfd, &readfds))) break;
			res = SSL_read(sslobj,  recvbuf, sizeof(recvbuf)-1);
			if (res <= 0) {
				switch (SSL_get_error(sslobj, res)) {
				  case SSL_ERROR_WANT_READ:
					talkstate = TALK_SSLREAD_RECEIVE;
					break;
				  case SSL_ERROR_WANT_WRITE:
					talkstate = TALK_SSLWRITE_RECEIVE;
					break;
				  case SSL_ERROR_ZERO_RETURN:
					talkstate = TALK_DONE;
					talkresult = BB_OK;
					break;
				  default:
					errprintf("SSL receive error\n");
					talkstate = TALK_DONE;
					talkresult = BB_EREADERROR;
					break;
				}
			}
			else {
				talkstate = process_receive(recvbuf, res, response, talkstate);
			}
			break;
#endif /* HAVE_OPENSSL */

		}
	}

	dbgprintf("Closing connection\n");
	closesocket(sockfd);

	if (response && response->respstr && (response->cmprhandling == CMP_LOOKFORMARKER) && 
	    (strncmp(STRBUF(response->respstr), compressionmarker, compressionmarkersz) == 0)) {
		/* Decompress the message */
		strbuffer_t *s = uncompress_buffer(STRBUF(response->respstr), STRBUFLEN(response->respstr), NULL);
		freestrbuffer(response->respstr);
		response->respstr = s;
	}

	if (response && response->cmprhandle) uncompress_stream_done(response->cmprhandle);
	if (rcptip) xfree(rcptip);
	if (httpmessage) xfree(httpmessage);

	return talkresult;
}


static int sendtomany(char *onercpt, char *morercpts, char *msg, int timeout, sendreturn_t *response)
{
	int allservers = 1, first = 1, result = BB_OK;
	char *bbdlist, *rcpt;

	/*
	 * Even though this is the "sendtomany" routine, we need to decide if the
	 * request should go to all servers, or just a single server. The default 
	 * is to send to all servers - but commands that trigger a response can
	 * only go to a single server.
	 *
	 * "schedule" is special - when scheduling an action there is no response, but 
	 * when it is the blank "schedule" command there will be a response. So a 
	 * schedule action goes to all BBDISPLAYS, the blank "schedule" goes to a single
	 * server.
	 */

	if (strcmp(onercpt, "0.0.0.0") != 0) 
		allservers = 0;
	else if (strncmp(msg, "schedule", 8) == 0)
		/* See if it's just a blank "schedule" command */
		allservers = (strcmp(msg, "schedule") != 0);
	else {
		char *msgcmd;
		int i;

		/* See if this is a multi-recipient command */
		i = strspn(msg, "abcdefghijklmnopqrstuvwxyz");
		msgcmd = (char *)malloc(i+1);
		strncpy(msgcmd, msg, i); *(msgcmd+i) = '\0';
		for (i = 0; (multircptcmds[i] && strcmp(multircptcmds[i], msgcmd)); i++) ;
		xfree(msgcmd);

		allservers = (multircptcmds[i] != NULL);
	}

	if (allservers && !morercpts) {
		errprintf("No recipients listed! BBDISP was %s, BBDISPLAYS %s\n",
			  onercpt, textornull(morercpts));
		return BB_EBADIP;
	}

	if (strcmp(onercpt, "0.0.0.0") != 0) 
		bbdlist = strdup(onercpt);
	else
		bbdlist = strdup(morercpts);

	rcpt = strtok(bbdlist, " \t");
	while (rcpt) {
		int oneres;

		if (first) {
			/* We grab the result from the first server */
			oneres =  sendtobbd(rcpt, msg, timeout, response);
			if (oneres == BB_OK) first = 0;
		}
		else {
			/* Secondary servers do not yield a response */
			oneres =  sendtobbd(rcpt, msg, timeout, NULL);
		}

		/* Save any error results */
		if (result == BB_OK) result = oneres;

		/*
		 * Handle more servers IF we're doing all servers, OR
		 * we are still at the first one (because the previous
		 * ones failed).
		 */
		if (allservers || first) 
			rcpt = strtok(NULL, " \t");
		else 
			rcpt = NULL;
	}

	xfree(bbdlist);

	return result;
}

sendreturn_t *newsendreturnbuf(int fullresponse, FILE *respfd)
{
	sendreturn_t *result;

	result = (sendreturn_t *)calloc(1, sizeof(sendreturn_t));
	result->fullresponse = fullresponse;
	result->respfd = respfd;
	if (!respfd) {
		/* No response file, so return it in a strbuf */
		result->respstr = newstrbuffer(0);
	}
	result->haveseenhttphdrs = 1;
	result->cmprhandle = NULL;
	result->cmprhandling = CMP_NONE;

	return result;
}

void freesendreturnbuf(sendreturn_t *s)
{
	if (!s) return;
	if (s->respstr) freestrbuffer(s->respstr);
	xfree(s);
}

char *getsendreturnstr(sendreturn_t *s, int takeover)
{
	char *result = NULL;

	if (!s) return NULL;
	if (!s->respstr) return NULL;
	result = STRBUF(s->respstr);
	if (takeover) s->respstr = NULL;

	return result;
}



sendresult_t sendmessage(char *msg, char *recipient, int timeout, sendreturn_t *response)
{
	static char *bbdisp = NULL;
	int res = 0;

 	if ((bbdisp == NULL) && xgetenv("BBDISP")) bbdisp = strdup(xgetenv("BBDISP"));
	if (recipient == NULL) recipient = bbdisp;
	if (recipient == NULL) {
		errprintf("No recipient for message\n");
		return BB_EBADIP;
	}

	res = sendtomany((recipient ? recipient : bbdisp), xgetenv("BBDISPLAYS"), msg, timeout, response);

	if (res != BB_OK) {
		char *statustext = "";

		switch (res) {
		  case BB_OK            : statustext = "OK"; break;
		  case BB_EBADIP        : statustext = "Bad IP address"; break;
		  case BB_EIPUNKNOWN    : statustext = "Cannot resolve hostname"; break;
		  case BB_ENOSOCKET     : statustext = "Cannot get a socket"; break;
		  case BB_ECANNOTDONONBLOCK   : statustext = "Non-blocking I/O failed"; break;
		  case BB_ECONNFAILED   : statustext = "Connection failed"; break;
		  case BB_ESELFAILED    : statustext = "select(2) failed"; break;
		  case BB_ETIMEOUT      : statustext = "timeout"; break;
		  case BB_EWRITEERROR   : statustext = "write error"; break;
		  case BB_EREADERROR    : statustext = "read error"; break;
		  case BB_EBADURL       : statustext = "Bad URL"; break;
		  default:                statustext = "Unknown error"; break;
		};

		errprintf("Whoops ! bb failed to send message - %s\n", statustext, res);
	}

	/* Give it a break */
	if (sleepbetweenmsgs) usleep(sleepbetweenmsgs);
	bbmsgcount++;
	return res;
}


/* Routines for handling combo message transmission */
static void combo_params(void)
{
	static int issetup = 0;

	if (issetup) return;

	issetup = 1;

	if (xgetenv("BBMAXMSGSPERCOMBO")) maxmsgspercombo = atoi(xgetenv("BBMAXMSGSPERCOMBO"));
	if (maxmsgspercombo == 0) {
		/* Force it to 100 */
		dbgprintf("BBMAXMSGSPERCOMBO is 0, setting it to 100\n");
		maxmsgspercombo = 100;
	}

	if (xgetenv("BBSLEEPBETWEENMSGS")) sleepbetweenmsgs = atoi(xgetenv("BBSLEEPBETWEENMSGS"));
}

void combo_start(void)
{
	combo_params();

	if (bbmsg == NULL) bbmsg = newstrbuffer(0);
	clearstrbuffer(bbmsg);
	addtobuffer(bbmsg, "combo\n");
	bbmsgqueued = 0;
}

void meta_start(void)
{
	if (metamsg == NULL) metamsg = newstrbuffer(0);
	clearstrbuffer(metamsg);
	bbmetaqueued = 0;
}

static void combo_flush(void)
{

	if (!bbmsgqueued) {
		dbgprintf("Flush, but bbmsg is empty\n");
		return;
	}

	if (debug) {
		char *p1, *p2;

		dbgprintf("Flushing combo message\n");
		p1 = p2 = STRBUF(bbmsg);

		do {
			p2++;
			p1 = strstr(p2, "\nstatus ");
			if (p1) {
				p1++; /* Skip the newline */
				p2 = strchr(p1, '\n');
				if (p2) *p2='\0';
				printf("      %s\n", p1);
				if (p2) *p2='\n';
			}
		} while (p1 && p2);
	}

	sendmessage(STRBUF(bbmsg), NULL, BBTALK_TIMEOUT, NULL);
	combo_start();	/* Get ready for the next */
}

static void meta_flush(void)
{
	if (!bbmetaqueued) {
		dbgprintf("Flush, but bbmeta is empty\n");
		return;
	}

	sendmessage(STRBUF(metamsg), NULL, BBTALK_TIMEOUT, NULL);
	meta_start();	/* Get ready for the next */
}

static void combo_add(strbuffer_t *buf)
{
	/* Check if there is room for the message + 2 newlines */
	if (maxmsgspercombo && (bbmsgqueued >= maxmsgspercombo)) {
		/* Nope ... flush buffer */
		combo_flush();
	}
	else {
		/* Yep ... add delimiter before new status (but not before the first!) */
		if (bbmsgqueued) addtobuffer(bbmsg, "\n\n");
	}

	addtostrbuffer(bbmsg, buf);
	bbmsgqueued++;
}

static void meta_add(strbuffer_t *buf)
{
	/* Check if there is room for the message + 2 newlines */
	if (maxmsgspercombo && (bbmetaqueued >= maxmsgspercombo)) {
		/* Nope ... flush buffer */
		meta_flush();
	}
	else {
		/* Yep ... add delimiter before new status (but not before the first!) */
		if (bbmetaqueued) addtobuffer(metamsg, "\n\n");
	}

	addtostrbuffer(metamsg, buf);
	bbmetaqueued++;
}

void combo_end(void)
{
	combo_flush();
	dbgprintf("%d status messages merged into %d transmissions\n", bbstatuscount, bbmsgcount);
}

void meta_end(void)
{
	meta_flush();
}

void init_status(int color)
{
	if (msgbuf == NULL) msgbuf = newstrbuffer(0);
	clearstrbuffer(msgbuf);
	msgcolor = color;
	bbstatuscount++;
}

void init_meta(char *metaname)
{
	if (metabuf == NULL) metabuf = newstrbuffer(0);
	clearstrbuffer(metabuf);
}

void addtostatus(char *p)
{
	addtobuffer(msgbuf, p);
}

void addtostrstatus(strbuffer_t *p)
{
	addtostrbuffer(msgbuf, p);
}

void addtometa(char *p)
{
	addtobuffer(metabuf, p);
}

void finish_status(void)
{
	if (debug) {
		char *p = strchr(STRBUF(msgbuf), '\n');

		if (p) *p = '\0';
		dbgprintf("Adding to combo msg: %s\n", STRBUF(msgbuf));
		if (p) *p = '\n';
	}

	combo_add(msgbuf);
}

void finish_meta(void)
{
	meta_add(metabuf);
}


