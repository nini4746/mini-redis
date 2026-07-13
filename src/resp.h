#ifndef RESP_H
#define RESP_H

#include "conn.h"

/* Incremental RESP2 multibulk parser.
 *
 * Tries to parse one complete command from c->rbuf.
 * Return values:
 *    1  a full command was parsed; c->argv/c->argc are filled and the
 *       consumed bytes have been removed from the read buffer.
 *    0  incomplete: need more bytes. The read buffer is left untouched.
 *   -1  protocol error. Caller should reply -ERR and close the connection.
 *
 * The parser never assumes the whole request arrived at once, and it
 * loops naturally over a buffer that holds several pipelined requests
 * (call it repeatedly until it returns 0).
 */
int resp_parse(client *c);

/* Free the argv allocated by the last resp_parse. */
void resp_free_argv(client *c);

/* Reply helpers (append RESP-encoded output to the client write buffer). */
void reply_simple(client *c, const char *s);          /* +s\r\n            */
void reply_error(client *c, const char *s);           /* -s\r\n            */
void reply_integer(client *c, long long n);           /* :n\r\n            */
void reply_bulk(client *c, const void *p, size_t len); /* $len\r\n<p>\r\n  */
void reply_bulk_cstr(client *c, const char *s);
void reply_null(client *c);                           /* $-1\r\n           */
void reply_array_header(client *c, long long n);      /* *n\r\n            */

#endif
