/*	$OpenBSD$	*/

/*
 * Copyright (c) 2013 Eric Faurot <eric@openbsd.org>
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

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/uio.h>

#include <event.h>
#include <fcntl.h>
#include <imsg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "smtpd-defines.h"
#include "smtpd-api.h"
#include "log.h"

static int (*handler_update)(void);
static int (*handler_check)(int, const char *);
static int (*handler_lookup)(int, const char *, char *, size_t);
static int (*handler_fetch)(int, char *, size_t);

static int		 quit;
static struct imsgbuf	 ibuf;
static struct imsg	 imsg;
static size_t		 rlen;
static char		*rdata;
static struct ibuf	*buf;

static void
table_msg_get(void *dst, size_t len)
{
	if (len > rlen) {
		log_warnx("warn: table-proc: bad msg len");
		fatalx("table-proc: exiting");
	}

	if (len == 0)
		return;

	if (dst)
		memmove(dst, rdata, len);

	rlen -= len;
	rdata += len;
}

static void
table_msg_end(void)
{
	if (rlen) {
		log_warnx("warn: table-proc: bogus data");
		fatalx("table-proc: exiting");
	}
	imsg_free(&imsg);
}

static void
table_msg_add(const void *data, size_t len)
{
	if (buf == NULL)
		buf = imsg_create(&ibuf, PROC_TABLE_OK, 0, 0, 1024);
	if (buf == NULL) {
		log_warnx("warn: table-api: imsg_create failed");
		fatalx("table-api: exiting");
	}
	if (imsg_add(buf, data, len) == -1) {
		log_warnx("warn: table-api: imsg_add failed");
		fatalx("table-api: exiting");
	}
}

static void
table_msg_close(void)
{
	imsg_close(&ibuf, buf);
	buf = NULL;
}

static void
table_msg_dispatch(void)
{
	uint32_t	 version;
	char		 res[4096];
	int		 type, r;

	switch (imsg.hdr.type) {
	case PROC_TABLE_OPEN:
		table_msg_get(&version, sizeof(version));
		table_msg_end();

		if (version != PROC_TABLE_API_VERSION) {
			log_warnx("warn: table-api: bad API version");
			fatalx("table-api: terminating");
		}

		imsg_compose(&ibuf, PROC_TABLE_OK, 0, 0, -1, NULL, 0);
		break;

	case PROC_TABLE_UPDATE:
		table_msg_end();

		if (handler_update)
			r = handler_update();
		else
			r = 1;

		imsg_compose(&ibuf, PROC_TABLE_OK, 0, 0, -1, &r, sizeof(r));
		break;

	case PROC_TABLE_CLOSE:
		quit = 1;
		break;

	case PROC_TABLE_CHECK:
		table_msg_get(&type, sizeof(type));
		if (rlen == 0) {
			log_warnx("warn: table-api: no key");
			fatalx("table-api: exiting");
		}
		if (rdata[rlen - 1] != '\0') {
			log_warnx("warn: table-api: key not NUL-terminated");
			fatalx("table-api: exiting");
		}

		if (handler_check)
			r = handler_check(type, rdata);
		else
			r = -1;
		table_msg_get(NULL, rlen);
		table_msg_end();

		table_msg_add(&r, sizeof(r));
		table_msg_close();
		break;

	case PROC_TABLE_LOOKUP:
		table_msg_get(&type, sizeof(type));

		if (rlen == 0) {
			log_warnx("warn: table-api: no key");
			fatalx("table-api: exiting");
		}
		if (rdata[rlen - 1] != '\0') {
			log_warnx("warn: table-api: key not NUL-terminated");
			fatalx("table-api: exiting");
		}

		if (handler_lookup)
			r = handler_lookup(type, rdata, res, sizeof(res));
		else
			r = -1;

		table_msg_get(NULL, rlen);
		table_msg_end();

		table_msg_add(&r, sizeof(r));
		if (r == 1)
			table_msg_add(res, strlen(res) + 1);
		table_msg_close();
		break;


	case PROC_TABLE_FETCH:
		table_msg_get(&type, sizeof(type));
		table_msg_end();

		if (handler_fetch)
			r = handler_fetch(type, res, sizeof(res));
		else
			r = -1;

		table_msg_add(&r, sizeof(r));
		if (r == 1)
			table_msg_add(res, strlen(res) + 1);
		table_msg_close();
		break;

	default:
		log_warnx("warn: table-api: bad message %i", imsg.hdr.type);
		fatalx("table-api: exiting");
	}
}

void
table_api_on_update(int(*cb)(void))
{
	handler_update = cb;
}

void
table_api_on_check(int(*cb)(int, const char *))
{
	handler_check = cb;
}

void
table_api_on_lookup(int(*cb)(int, const char *, char *, size_t))
{
	handler_lookup = cb;
}

void
table_api_on_fetch(int(*cb)(int, char *, size_t))
{
	handler_fetch = cb;
}

int
table_api_dispatch(void)
{
	ssize_t	n;

	imsg_init(&ibuf, 0);

	while (1) {
		n = imsg_get(&ibuf, &imsg);
		if (n == -1) {
			log_warn("warn: table-api: imsg_get");
			break;
		}

		if (n) {
			rdata = imsg.data;
			rlen = imsg.hdr.len - IMSG_HEADER_SIZE;
			table_msg_dispatch();
			if (quit)
				break;
			imsg_flush(&ibuf);
			continue;
		}

		n = imsg_read(&ibuf);
		if (n == -1) {
			log_warn("warn: table-api: imsg_read");
			break;
		}
		if (n == 0) {
			log_warnx("warn: table-api: pipe closed");
			break;
		}
	}

	return (1);
}
