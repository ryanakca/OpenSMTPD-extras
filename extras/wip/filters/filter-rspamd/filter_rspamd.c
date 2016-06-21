/*
 * Copyright (c) 2016 Gilles Chehade <gilles@poolp.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "includes.h"

#include <sys/types.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <unistd.h>

#include "smtpd-defines.h"
#include "smtpd-api.h"
#include "log.h"
#include "iobuf.h"
#include "ioev.h"

#define RSPAMD_HOST "127.0.0.1"
#define RSPAMD_PORT "11333"

struct transaction {
	char	       *from;
	char	       *rcpt;

	int		inbody;
	struct iobuf	headers;
	struct iobuf	body;
};

struct session {
	uint64_t	id;

	struct iobuf	iobuf;
	struct io	io;
	
	char	       *ip;
	char	       *hostname;
	char	       *helo;

	struct transaction	tx;
};

struct sockaddr_storage	ss;

static struct session  *rspamd_session_init(uint64_t);
static void		rspamd_session_free(struct session *);
static void		rspamd_transaction_clear(struct transaction *);
static void		rspamd_io(struct io *, int);

static int
on_connect(uint64_t id, struct filter_connect *conn)
{
	struct session	*rs;

	rs = rspamd_session_init(id);

	//r->ip = xstrdup(sockaddr_to_text(&conn->local), "on_connect");
	rs->ip = xstrdup("192.168.1.1", "on_connect");
	rs->hostname = xstrdup(conn->hostname, "on_connect");
	filter_api_set_udata(id, rs);
	return filter_api_accept(id);
}

static int
on_helo(uint64_t id, const char *helo)
{
	struct session	*rs = filter_api_get_udata(id);

	rs->helo = xstrdup(helo, "on_helo");
	return filter_api_accept(id);
}

static int
on_mail(uint64_t id, struct mailaddr *mail)
{
	struct session	*rs = filter_api_get_udata(id);

	rspamd_transaction_clear(&rs->tx);
	rs->tx.from = xstrdup("gilles@poolp.org", "on_mail");
	return filter_api_accept(id);
}

static int
on_rcpt(uint64_t id, struct mailaddr *rcpt)
{
	struct session	*rs = filter_api_get_udata(id);

	log_debug("debug: on_rcpt");
	rs->tx.rcpt = xstrdup("gilles+rcpt@poolp.org", "on_rcpt");
	return filter_api_accept(id);
}

static int
on_data(uint64_t id)
{
	log_debug("debug: on_data");
	return filter_api_accept(id);
}

static void
on_dataline(uint64_t id, const char *line)
{
	struct session	*rs = filter_api_get_udata(id);

	if (!strlen(line))
		rs->tx.inbody = 1;

	if (! rs->tx.inbody)
		iobuf_xfqueue(&rs->tx.headers, "io", "%s\r\n", line);
	else
		iobuf_xfqueue(&rs->tx.body, "io", "%s\r\n", line);

	log_debug("%.*s [%s]", iobuf_len(&rs->tx.headers), iobuf_data(&rs->tx.headers), line);
}

static int
on_eom(uint64_t id, size_t size)
{
	struct session	*rs = filter_api_get_udata(id);
	
	iobuf_xinit(&rs->iobuf, LINE_MAX, LINE_MAX, "on_eom");
	io_init(&rs->io, -1, rs, rspamd_io, &rs->iobuf);
	if (io_connect(&rs->io, (struct sockaddr *)&ss, NULL) == -1)
		return filter_api_accept(id);

//	iobuf_xfqueue(&rs->iobuf, "io",
	log_debug(
	    "POST /check HTTP/1.0\r\n"
	    "IP: %s\r\n"
	    "Helo: %s\r\n"
	    "Hostname: %s\r\n"
	    "From: %s\r\n"
	    "Rcpt: %s\r\n"
	    "Pass: all\r\n"
	    "Content-Length: %d\r\n\r\n%.*s%.*s",
	    rs->ip,
	    rs->helo,
	    rs->hostname,
	    rs->tx.from,
	    rs->tx.rcpt,
	    iobuf_len(&rs->tx.headers) + iobuf_len(&rs->tx.body),
	    iobuf_len(&rs->tx.headers), iobuf_data(&rs->tx.headers),
	    iobuf_len(&rs->tx.body), iobuf_data(&rs->tx.body));
}

static void
on_commit(uint64_t id)
{
	log_debug("debug: on_commit");
}

static void
on_rollback(uint64_t id)
{
	log_debug("debug: on_rollback");
}

static void
on_disconnect(uint64_t id)
{
	rspamd_session_free((struct session *)filter_api_get_udata(id));
}

static struct session *
rspamd_session_init(uint64_t id)
{
	struct session	*rs;

	rs = xcalloc(1, sizeof *rs, "on_connect");
	rs->id = id;

	iobuf_xinit(&rs->tx.headers, LINE_MAX, LINE_MAX, "on_eom");
	iobuf_xinit(&rs->tx.body, LINE_MAX, LINE_MAX, "on_eom");

	return rs;
}

static void
rspamd_transaction_clear(struct transaction *tx)
{
	free(tx->from);
	free(tx->rcpt);
}

static void
rspamd_session_free(struct session *rs)
{
	iobuf_clear(&rs->iobuf);
	io_clear(&rs->io);

	rspamd_transaction_clear(&rs->tx);

	free(rs->ip);
	free(rs->hostname);
	free(rs->helo);
	free(rs);
}

static void
rspamd_io(struct io *io, int evt)
{
	struct session	*r = io->arg;
	char		*l;

	switch (evt) {
	case IO_CONNECTED:
		log_debug("debug: CONNECTED");
		io_set_write(io);
		break;
	case IO_LOWAT:
		log_debug("debug: LOWAT");
		io_set_read(io);
		break;
	case IO_DATAIN:
		log_debug("debug: DATAIN");
		while ((l = iobuf_getline(&r->iobuf, NULL)) != NULL)
			log_debug("debug: DATAIN: [%s]", l);
		if (iobuf_len(&r->iobuf) != 0) {
			log_debug("debug: DATAIN: [%.*s]",
			    iobuf_len(&r->iobuf),
			    iobuf_data(&r->iobuf));
		}
		iobuf_normalize(&r->iobuf);		
		break;
	case IO_DISCONNECTED:
		log_debug("debug: DISCONNECT");
		exit(1);
		break;
	case IO_TIMEOUT:
		log_debug("debug: TIMEOUT");
		break;
	case IO_ERROR:
		log_debug("debug: ERROR");
		break;
	default:
		log_debug("debug: WTF");
		break;
	}
	return;
}


static void
rspamd_resolve(const char *h, const char *p)
{
	struct addrinfo hints, *addresses, *ai;
	int fd, r;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	if ((r = getaddrinfo(h, p, &hints, &addresses)))
		fatalx("resolve: getaddrinfo %s", gai_strerror(r));
	for (ai = addresses; ai; ai = ai->ai_next) {
		if ((fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) == -1)
			continue;
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == -1) {
			close(fd);
			continue;
		}
		close(fd);
		memmove(&ss, ai->ai_addr, ai->ai_addrlen);
		break;
	}
	freeaddrinfo(addresses);
	if (!ai)
		fatalx("resolve: failed");
}

int
main(int argc, char **argv)
{
	int ch, C = 0, d = 0, v = 0;
	const char *l = NULL;
	char *c = NULL, *h = RSPAMD_HOST, *p = RSPAMD_PORT, *s = NULL;

	log_init(1);

	while ((ch = getopt(argc, argv, "dh:l:p:s:v")) != -1) {
		switch (ch) {
		case 'C':
			C = 1;
			break;
		case 'c':
			c = optarg;
			break;
		case 'd':
			d = 1;
			break;
		case 'h':
			h = optarg;
			break;
		case 'l':
			l = optarg;
			break;
		case 'p':
			p = optarg;
			break;
		case 's':
			s = optarg;
			break;
		case 'v':
			v |= TRACE_DEBUG;
			break;
		default:
			log_warnx("warn: bad option");
			return 1;
			/* NOTREACHED */
		}
	}
	argc -= optind;
	argv += optind;

	if (c)
		c = strip(c);
	if (h)
		h = strip(h);
	if (p)
		p = strip(p);

	log_init(d);
	log_verbose(v);

	log_debug("debug: starting...");

	rspamd_resolve(h, p);

	filter_api_on_connect(on_connect);
	filter_api_on_helo(on_helo);
	filter_api_on_mail(on_mail);
	filter_api_on_rcpt(on_rcpt);
	filter_api_on_data(on_data);
	filter_api_on_dataline(on_dataline);
	filter_api_on_eom(on_eom);
	filter_api_on_commit(on_commit);
	filter_api_on_rollback(on_rollback);
	filter_api_on_disconnect(on_disconnect);

	if (c)
		filter_api_set_chroot(c);
	if (C)
		filter_api_no_chroot();

	filter_api_loop();
	log_debug("debug: exiting");

	return 1;
}