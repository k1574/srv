/* See LICENSE file for copyright and license details. */
#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <netinet/in.h>
#include <pwd.h>
#include <regex.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "arg.h"
#include "data.h"
#include "http.h"
#include "sock.h"
#include "util.h"

static char *udsname;

static void
logmsg(const struct connection *c)
{
	char inaddr_str[INET6_ADDRSTRLEN /* > INET_ADDRSTRLEN */];
	char tstmp[21];

	/* create timestamp */
	if (!strftime(tstmp, sizeof(tstmp), "%Y-%m-%dT%H:%M:%SZ",
	              gmtime(&(time_t){time(NULL)}))) {
		warn("strftime: Exceeded buffer capacity");
		/* continue anyway (we accept the truncation) */
	}

	/* generate address-string */
	if (sock_get_inaddr_str(&c->ia, inaddr_str, LEN(inaddr_str))) {
		warn("sock_get_inaddr_str: Couldn't generate adress-string");
		inaddr_str[0] = '\0';
	}

	printf("%s\t%s\t%d\t%s\t%s\n", tstmp, inaddr_str, c->res.status,
	       c->req.field[REQ_HOST], c->req.uri);
}

static void
serve(struct connection *c, const struct server *srv)
{
	enum status s;
	int done;

	/* set connection timeout */
	if (sock_set_timeout(c->fd, 30)) {
		warn("sock_set_timeout: Failed");
	}

	switch (c->state) {
	case C_VACANT:
		/*
		 * we were passed a "fresh" connection which should now
		 * try to receive the header, reset buf beforehand
		 */
		memset(&c->buf, 0, sizeof(c->buf));

		c->state = C_RECV_HEADER;
		/* fallthrough */
	case C_RECV_HEADER:
		/* receive header */
		done = 0;
		if ((s = http_recv_header(c->fd, &c->buf, &done))) {
			http_prepare_error_response(&c->req, &c->res, s);
			goto response;
		}
		if (!done) {
			/* not done yet */
			return;
		}

		/* parse header */
		if ((s = http_parse_header(c->buf.data, &c->req))) {
			http_prepare_error_response(&c->req, &c->res, s);
			goto response;
		}

		/* prepare response struct */
		http_prepare_response(&c->req, &c->res, srv);
response:
		/* generate response header */
		if ((s = http_prepare_header_buf(&c->res, &c->buf))) {
			http_prepare_error_response(&c->req, &c->res, s);
			if ((s = http_prepare_header_buf(&c->res, &c->buf))) {
				/* couldn't generate the header, we failed for good */
				c->res.status = s;
				goto err;
			}
		}

		c->state = C_SEND_HEADER;
		/* fallthrough */
	case C_SEND_HEADER:
		if ((s = http_send_buf(c->fd, &c->buf))) {
			c->res.status = s;
			goto err;
		}
		if (c->buf.len > 0) {
			/* not done yet */
			return;
		}

		c->state = C_SEND_BODY;
		/* fallthrough */
	case C_SEND_BODY:
		if (c->req.method == M_GET) {
			if (c->buf.len == 0) {
				/* fill buffer with body data */
				if ((s = data_fct[c->res.type](&c->res, &c->buf,
				                               &c->progress))) {
					/* too late to do any real error handling */
					c->res.status = s;
					goto err;
				}

				/* if the buffer remains empty, we are done */
				if (c->buf.len == 0) {
					break;
				}
			} else {
				/* send buffer */
				if ((s = http_send_buf(c->fd, &c->buf))) {
					/* too late to do any real error handling */
					c->res.status = s;
					goto err;
				}
			}
			return;
		}
		break;
	default:
		warn("serve: invalid connection state");
		return;
	}
err:
	logmsg(c);

	/* clean up and finish */
	shutdown(c->fd, SHUT_RD);
	shutdown(c->fd, SHUT_WR);
	close(c->fd);
	c->state = C_VACANT;
}

static void
cleanup(void)
{
	if (udsname)
		 sock_rem_uds(udsname);
}

static void
sigcleanup(int sig)
{
	cleanup();
	kill(0, sig);
	_exit(1);
}

static void
handlesignals(void(*hdl)(int))
{
	struct sigaction sa = {
		.sa_handler = hdl,
	};

	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);
}

static int
spacetok(const char *s, char **t, size_t tlen)
{
	const char *tok;
	size_t i, j, toki, spaces;

	/* fill token-array with NULL-pointers */
	for (i = 0; i < tlen; i++) {
		t[i] = NULL;
	}
	toki = 0;

	/* don't allow NULL string or leading spaces */
	if (!s || *s == ' ') {
		return 1;
	}
start:
	/* skip spaces */
	for (; *s == ' '; s++)
		;

	/* don't allow trailing spaces */
	if (*s == '\0') {
		goto err;
	}

	/* consume token */
	for (tok = s, spaces = 0; ; s++) {
		if (*s == '\\' && *(s + 1) == ' ') {
			spaces++;
			s++;
			continue;
		} else if (*s == ' ') {
			/* end of token */
			goto token;
		} else if (*s == '\0') {
			/* end of string */
			goto token;
		}
	}
token:
	if (toki >= tlen) {
		goto err;
	}
	if (!(t[toki] = malloc(s - tok - spaces + 1))) {
		die("malloc:");
	}
	for (i = 0, j = 0; j < s - tok - spaces + 1; i++, j++) {
		if (tok[i] == '\\' && tok[i + 1] == ' ') {
			i++;
		}
		t[toki][j] = tok[i];
	}
	t[toki][s - tok - spaces] = '\0';
	toki++;

	if (*s == ' ') {
		s++;
		goto start;
	}

	return 0;
err:
	for (i = 0; i < tlen; i++) {
		free(t[i]);
		t[i] = NULL;
	}

	return 1;
}

static void
usage(void)
{
	const char *opts = "[-u user] [-g group] [-n num] [-d dir] [-l] "
	                   "[-i file] [-v vhost] ... [-m map] ...";

	die("usage: %s -p port [-h host] %s\n"
	    "       %s -U file [-p port] %s", argv0,
	    opts, argv0, opts);
}

int
main(int argc, char *argv[])
{
	struct group *grp = NULL;
	struct passwd *pwd = NULL;
	struct rlimit rlim;
	struct server srv = {
		.docindex = "index.htm",
	};
	size_t i;
	int insock, status = 0;
	const char *err;
	char *tok[4];

	/* defaults */
	int maxnprocs = 512;
	char *servedir = ".";
	char *user = "nobody";
	char *group = "nogroup";

	ARGBEGIN {
	case 'd':
		servedir = EARGF(usage());
		break;
	case 'g':
		group = EARGF(usage());
		break;
	case 'h':
		srv.host = EARGF(usage());
		break;
	case 'i':
		srv.docindex = EARGF(usage());
		if (strchr(srv.docindex, '/')) {
			die("The document index must not contain '/'");
		}
		break;
	case 'l':
		srv.listdirs = 1;
		break;
	case 'm':
		if (spacetok(EARGF(usage()), tok, 3) || !tok[0] || !tok[1]) {
			usage();
		}
		if (!(srv.map = reallocarray(srv.map, ++srv.map_len,
		                           sizeof(struct map)))) {
			die("reallocarray:");
		}
		srv.map[srv.map_len - 1].from  = tok[0];
		srv.map[srv.map_len - 1].to    = tok[1];
		srv.map[srv.map_len - 1].chost = tok[2];
		break;
	case 'n':
		maxnprocs = strtonum(EARGF(usage()), 1, INT_MAX, &err);
		if (err) {
			die("strtonum '%s': %s", EARGF(usage()), err);
		}
		break;
	case 'p':
		srv.port = EARGF(usage());
		break;
	case 'U':
		udsname = EARGF(usage());
		break;
	case 'u':
		user = EARGF(usage());
		break;
	case 'v':
		if (spacetok(EARGF(usage()), tok, 4) || !tok[0] || !tok[1] ||
		    !tok[2]) {
			usage();
		}
		if (!(srv.vhost = reallocarray(srv.vhost, ++srv.vhost_len,
		                               sizeof(*srv.vhost)))) {
			die("reallocarray:");
		}
		srv.vhost[srv.vhost_len - 1].chost  = tok[0];
		srv.vhost[srv.vhost_len - 1].regex  = tok[1];
		srv.vhost[srv.vhost_len - 1].dir    = tok[2];
		srv.vhost[srv.vhost_len - 1].prefix = tok[3];
		break;
	default:
		usage();
	} ARGEND

	if (argc) {
		usage();
	}

	/* can't have both host and UDS but must have one of port or UDS*/
	if ((srv.host && udsname) || !(srv.port || udsname)) {
		usage();
	}

	if (udsname && (!access(udsname, F_OK) || errno != ENOENT)) {
		die("UNIX-domain socket '%s': %s", udsname, errno ?
		    strerror(errno) : "File exists");
	}

	/* compile and check the supplied vhost regexes */
	for (i = 0; i < srv.vhost_len; i++) {
		if (regcomp(&srv.vhost[i].re, srv.vhost[i].regex,
		            REG_EXTENDED | REG_ICASE | REG_NOSUB)) {
			die("regcomp '%s': invalid regex",
			    srv.vhost[i].regex);
		}
	}

	/* raise the process limit */
	rlim.rlim_cur = rlim.rlim_max = maxnprocs;
	if (setrlimit(RLIMIT_NPROC, &rlim) < 0) {
		die("setrlimit RLIMIT_NPROC:");
	}

	/* validate user and group */
	errno = 0;
	if (!user || !(pwd = getpwnam(user))) {
		die("getpwnam '%s': %s", user ? user : "null",
		    errno ? strerror(errno) : "Entry not found");
	}
	errno = 0;
	if (!group || !(grp = getgrnam(group))) {
		die("getgrnam '%s': %s", group ? group : "null",
		    errno ? strerror(errno) : "Entry not found");
	}

	/* open a new process group */
	setpgid(0, 0);

	handlesignals(sigcleanup);

	/* bind socket */
	insock = udsname ? sock_get_uds(udsname, pwd->pw_uid, grp->gr_gid) :
	                   sock_get_ips(srv.host, srv.port);

	switch (fork()) {
	case -1:
		warn("fork:");
		break;
	case 0:
		/* restore default handlers */
		handlesignals(SIG_DFL);

		/* reap children automatically */
		if (signal(SIGCHLD, SIG_IGN) == SIG_ERR) {
			die("signal: Failed to set SIG_IGN on SIGCHLD");
		}

		/* limit ourselves to reading the servedir and block further unveils */
		eunveil(servedir, "r");
		eunveil(NULL, NULL);

		/* chroot */
		if (chdir(servedir) < 0) {
			die("chdir '%s':", servedir);
		}
		if (chroot(".") < 0) {
			die("chroot .:");
		}

		/* drop root */
		if (setgroups(1, &(grp->gr_gid)) < 0) {
			die("setgroups:");
		}
		if (setgid(grp->gr_gid) < 0) {
			die("setgid:");
		}
		if (setuid(pwd->pw_uid) < 0) {
			die("setuid:");
		}

		if (udsname) {
			epledge("stdio rpath proc unix", NULL);
		} else {
			epledge("stdio rpath proc inet", NULL);
		}

		if (getuid() == 0) {
			die("Won't run as root user", argv0);
		}
		if (getgid() == 0) {
			die("Won't run as root group", argv0);
		}

		/* accept incoming connections */
		while (1) {
			struct connection c = { 0 };

			if ((c.fd = accept(insock, (struct sockaddr *)&c.ia,
			                   &(socklen_t){sizeof(c.ia)})) < 0) {
				warn("accept:");
				continue;
			}

			/* fork and handle */
			switch (fork()) {
			case 0:
				do {
					serve(&c, &srv);
				} while (c.state != C_VACANT);
				exit(0);
				break;
			case -1:
				warn("fork:");
				/* fallthrough */
			default:
				/* close the connection in the parent */
				close(c.fd);
			}
		}
		exit(0);
	default:
		/* limit ourselves even further while we are waiting */
		if (udsname) {
			eunveil(udsname, "c");
			eunveil(NULL, NULL);
			epledge("stdio cpath", NULL);
		} else {
			eunveil("/", "");
			eunveil(NULL, NULL);
			epledge("stdio", NULL);
		}

		while (wait(&status) > 0)
			;
	}

	cleanup();
	return status;
}
