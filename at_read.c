/*
   Copyright (C) 2009 - 2010

   Artem Makhutov <artem@makhutov.org>
   http://www.makhutov.org

   Dmitry Vagin <dmitry2004@yandex.ru>

   Copyright (C) 2010 - 2011
   bg <bg_one@mail.ru>
*/
#ifndef _GNU_SOURCE
#define _GNU_SOURCE			/* vasprintf() in asterisk/utils.h */
#endif /* #ifndef _GNU_SOURCE */

#include "ast_config.h"

#include <sys/types.h>
#include <errno.h>

#include <asterisk/channel.h>		/* ast_waitfor_n_fd() */
#include <asterisk/logger.h>		/* ast_debug() */

#include "chan_quectel.h"
#include "at_read.h"
#include "ringbuffer.h"


/*!
 * \brief Wait for activity on an socket
 * \param fd -- file descriptor
 * \param ms  -- pointer to an int containing a timeout in ms
 * \return 0 on timeout and the socket fd (non-zero) otherwise
 * \retval 0 timeout
 */

EXPORT_DEF int at_wait (int fd, int* ms)
{
	int exception, outfd;

	outfd = ast_waitfor_n_fd (&fd, 1, ms, &exception);

	if (outfd < 0)
	{
		outfd = 0;
	}

	return outfd;
}

#/* return number of bytes read */
EXPORT_DEF ssize_t at_read (int fd, const char * dev, struct ringbuffer* rb)
{
	struct iovec	iov[2];
	int		iovcnt;
	ssize_t		n = -1;

	/* TODO: read until major error */
	iovcnt = rb_write_iov (rb, iov);

	if (iovcnt > 0)
	{
		n = readv (fd, iov, iovcnt);

		if (n < 0)
		{
			if (errno != EINTR && errno != EAGAIN)
			{
				ast_debug (1, "[%s] readv() error: %d\n", dev, errno);
				return n;
			}

			return 0;
		}
		else if (n > 0)
		{
			rb_write_upd (rb, n);

			ast_debug (5, "[%s] receive %zu byte, used %zu, free %zu, read %zu, write %zu\n",
				dev, n, rb_used (rb), rb_free (rb), rb->read, rb->write);

			iovcnt = rb_read_all_iov (rb, iov);

			if (iovcnt > 0)
			{
				if (iovcnt == 2)
				{
					ast_debug (5, "[%s] [%.*s%.*s]\n", dev,
							(int) iov[0].iov_len, (char*) iov[0].iov_base,
							(int) iov[1].iov_len, (char*) iov[1].iov_base);
				}
				else
				{
					ast_debug (5, "[%s] [%.*s]\n", dev,
							(int) iov[0].iov_len, (char*) iov[0].iov_base);
				}
			}
		}
	}
	else
		ast_log (LOG_ERROR, "[%s] at cmd receive buffer overflow\n", dev);
	return n;
}

EXPORT_DEF int at_read_result_iov (struct pvt *pvt, int * read_result, struct ringbuffer* rb, struct iovec iov[2])
{
	char dev[sizeof(PVT_ID(pvt))];
	ast_copy_string(dev, PVT_ID(pvt), sizeof(dev));
	int	iovcnt = 0;
	int	res;
	size_t	s;

	s = rb_used (rb);
	ast_verb (100, "[%s] %s used=%d read_result=%d str=\"%.*s\"\n", dev, __func__, s, *read_result, s, (char*)rb->buffer + rb->read);
	if (s > 0)
	{
		ast_verb (100, "[%s] %s: len=%4$d input=\"%.*s\"\n", dev, __func__, s, *read_result, (char*)rb->buffer + rb->read);

		if (*read_result == 0)
		{
			res = rb_memcmp (rb, "\r\n", 2);
			if (rb_memcmp (rb, "\n", 1) == 0)
			{
				rb_read_upd (rb, 1);
				*read_result = 1;

				return at_read_result_iov (pvt, read_result, rb, iov);
			}
			else if (res == 0)
			{
				rb_read_upd (rb, 2);
				*read_result = 1;

				return at_read_result_iov (pvt, read_result, rb, iov);
			}
			else if (res > 0)
			{
				if (rb_memcmp (rb, "\n", 1) == 0)
				{
					rb_read_upd (rb, 1);

					return at_read_result_iov (pvt, read_result, rb, iov);
				}

				if (rb_read_until_char_iov (rb, iov, '\r') > 0)
				{
					s = iov[0].iov_len + iov[1].iov_len + 1;
				}

				rb_read_upd (rb, s);

				return at_read_result_iov (pvt, read_result, rb, iov);
			}

			return 0;
		}
		else
		{
			ast_verb (100, "[%s] %s: Comparing ..\n", dev, __func__);
			if (rb_memcmp (rb, "+CSSI:", 6) == 0)
			{
				iovcnt = rb_read_n_iov (rb, iov, 8);
				if (iovcnt > 0)
				{
					*read_result = 0;
				}

				return iovcnt;
			}
			else if (rb_memcmp (rb, "\r\n+CSSU:", 8) == 0 || rb_memcmp (rb, "\r\n+CMS ERROR:", 13) == 0 || rb_memcmp (rb, "\r\n+CMGS:", 8) == 0 || rb_memcmp (rb, "\r\nOK", 4) == 0)
			{
				ast_verb (100, "[%s] %s: OK matched\n", dev, __func__);
				rb_read_upd (rb, 2);
				return at_read_result_iov (pvt, read_result, rb, iov);
			}
			else if (rb_memcmp (rb, "> ", 2) == 0)
			{
				*read_result = 0;
				return rb_read_n_iov (rb, iov, 2);
			}
			else if (rb_memcmp (rb, "+CMT:", 5) == 0 || (rb_memcmp (rb, "CONNECT", 7) == 0 && pvt->connect_length > 0))
			{
				char *endptr;
				if(rb_memcmp (rb, "+CMT:", 5) == 0) {
					int len = strtol(memchr(rb->buffer + rb->read, ',', rb->used) + 1, &endptr, 10);
					s = (size_t)memchr(rb->buffer + rb->read, '\n', rb->used) - (size_t)(rb->buffer + rb->read) + (len+8)*2 + 2;
					ast_verb (100, "[%s] %s: +CMT matched. read %d bytes (len=%d)\n", dev, __func__, s, len);
				}
				else if(rb_memcmp (rb, "CONNECT", 7) == 0) {
					s = (size_t)memchr(rb->buffer + rb->read, '\n', rb->used) - (size_t)(rb->buffer + rb->read) + pvt->connect_length + 1;
					ast_verb (100, "[%s] %s: CONNECT matched. read %d bytes (%d)\n", dev, __func__, s, pvt->connect_length);
				}
				else {
					ast_verb (100, "[%s] %s: none matched. read %d bytes (%d)\n", dev, __func__, s);
				}
				iovcnt = rb_read_n_iov (rb, iov, s);
				if (iovcnt > 0)
				{
					*read_result = 0;
				}
				ast_verb (100, "[%s] %s: +CMT/CONNECT return %d bytes (read_result=%d)\n", dev, __func__, iov[0].iov_len + iov[1].iov_len + 1, *read_result);
				return iovcnt;
			}
			else if (rb_memcmp (rb, "+CMGR:", 6) == 0 || rb_memcmp (rb, "+CNUM:", 6) == 0 || rb_memcmp (rb, "ERROR+CNUM:", 11) == 0 || rb_memcmp (rb, "+CLCC:", 6) == 0)
			{
				iovcnt = rb_read_until_mem_iov (rb, iov, "\n\r\nOK\r\n", 7);
				if (iovcnt > 0)
				{
					*read_result = 0;
				}

				return iovcnt;
			}
			else
			{
				ast_verb (100, "[%s] %s: General matched (rb=0x%02x,iov_len=0x%02x)\n", dev, __func__, rb, rb->size - rb->read);
				iovcnt = rb_read_until_mem_iov (rb, iov, "\r\n", 2);
				ast_verb (100, "[%s] %s: rb_read_until_mem_iov returned (rb=0x%02x,iov_len=0x%02x, iovcnt=%d)\n", dev, __func__, rb, rb->size - rb->read, iovcnt);
				ast_verb (100, "[%s] %s: \"%.*s\"\n", dev, __func__, iov[0].iov_len + iov[1].iov_len + 1, (char*)rb->buffer + rb->read);
				if (iovcnt > 0)
				{
					*read_result = 0;
					s = iov[0].iov_len + iov[1].iov_len + 1;

					return rb_read_n_iov (rb, iov, s);
				}
			}
		}
	}

	return 0;
}

EXPORT_DEF at_res_t at_read_result_classification (struct ringbuffer * rb, size_t len)
{
	at_res_t at_res = RES_UNKNOWN;
	unsigned idx;

	ast_verb (100, "%s: at_responses .ids_first=%d .ids=%d .name_first=%d .name=%d", __func__, at_responses.ids_first, at_responses.ids, at_responses.name_first, at_responses.name_last);
	for(idx = at_responses.ids_first; idx < at_responses.ids; idx++)
	{
		if (rb_memcmp (rb, at_responses.responses[idx].id, at_responses.responses[idx].idlen) == 0)
		{
			at_res = at_responses.responses[idx].res;
			break;
		}
	}

	ast_verb (100, "%s: %d (%s) matched (len=%d)", __func__, at_res, at_res2str (at_res), len);
	switch (at_res)
	{
		case RES_SMS_PROMPT:
			len = 2;
			break;

		case RES_CMGR:
			len += 7;
			break;

		case RES_CSSI:
			len = 8;
			break;
		default:
			len += 1;
			break;
	}

	rb_read_upd (rb, len);

	ast_verb (100, "%s: receive result at_res=%d (%s) str=\"%.*s\"\n", __func__, at_res, at_res2str (at_res), len, (char*)rb->buffer + rb->read);

	return at_res;
}
