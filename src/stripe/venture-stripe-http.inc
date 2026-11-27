/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-stripe-http.inc - The network send of VENTURE's Stripe transport
 *
 * Copied from stripe-glib, src/providers/stripe-soup.c (send_request() and
 * its callbacks), at commit c15395e49831574ff488f40d278af967e8d95241 of
 * https://git.mcgoaty.com/ben/stripe-glib -- the commit VENTURE's
 * deps/stripe-glib is pinned to. The licence line above is that file's,
 * which carries no other notice. stripe-glib is AGPL-3.0-or-later, as
 * VENTURE is.
 *
 * It exists only until stripe-glib exports a status and a Retry-After
 * getter on StripeResponse. That struct is opaque, so a transport wrapping
 * the library's own soup transport cannot see a 429 to retry it; this copy
 * keeps them as it builds the response. With the getters, delete this file
 * and send through stripe_soup_transport_new() again.
 *
 * Unchanged in what it sends and how it bounds the wait: a private context,
 * so nothing of VENTURE's is dispatched; one absolute timer at the request's
 * deadline; no redirects; at most 4 MiB of body, with no NUL in it. Changed:
 * the status, Retry-After, Request-Id and body length are kept in the
 * Answer beside the response, and the code is in VENTURE's style.
 *
 * Included by venture-stripe-transport.c, which defines Answer.
 */

typedef struct
{
	GCancellable	*io;
	GInputStream	*stream;
	GByteArray	*body;
	GError		*error;
	gboolean	 done;
	gboolean	 expired;
	guchar		 buffer[8192];
} Transfer;

static void
http_read_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Transfer *transfer = data;
	gssize count = g_input_stream_read_finish(G_INPUT_STREAM(source), result, &transfer->error);

	if (count <= 0)
	{
		transfer->done = TRUE;
		return;
	}
	if (transfer->body->len + (gsize)count > VENTURE_STRIPE_BODY_MAX)
	{
		g_set_error_literal(&transfer->error, STRIPE_ERROR, STRIPE_ERROR_PROTOCOL, "Response too large");
		transfer->done = TRUE;
		return;
	}
	g_byte_array_append(transfer->body, transfer->buffer, (guint)count);
	g_input_stream_read_async(transfer->stream, transfer->buffer, sizeof transfer->buffer,
		G_PRIORITY_DEFAULT, transfer->io, http_read_done, transfer);
}

static void
http_headers_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Transfer *transfer = data;

	transfer->stream = soup_session_send_finish(SOUP_SESSION(source), result, &transfer->error);
	if (NULL == transfer->stream)
	{
		transfer->done = TRUE;
		return;
	}
	g_input_stream_read_async(transfer->stream, transfer->buffer, sizeof transfer->buffer,
		G_PRIORITY_DEFAULT, transfer->io, http_read_done, transfer);
}

static gboolean
http_deadline(gpointer data)
{
	Transfer *transfer = data;

	transfer->expired = TRUE;
	g_cancellable_cancel(transfer->io);
	return G_SOURCE_REMOVE;
}

static void
http_cancelled(GCancellable *caller, gpointer data)
{
	(void)caller;
	g_cancellable_cancel(G_CANCELLABLE(data));
}

static void
http_send(const StripeHttpRequest *request, GCancellable *cancellable, Answer *answer)
{
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", request->timeout, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(request->method, request->url);
	g_autoptr(GByteArray) body = g_byte_array_new();
	g_autoptr(GMainContext) context = g_main_context_new();
	g_autoptr(GCancellable) io = g_cancellable_new();
	g_autoptr(GSource) timer = g_timeout_source_new(0);
	SoupMessageHeaders *headers;
	gulong handler = 0;
	Transfer transfer;

	memset(&transfer, 0, sizeof transfer);
	transfer.io = io;
	transfer.body = body;
	if (NULL == message)
	{
		g_set_error_literal(&answer->error, STRIPE_ERROR, STRIPE_ERROR_TRANSPORT, "Invalid request");
		return;
	}
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	headers = soup_message_get_request_headers(message);
	soup_message_headers_replace(headers, "Authorization", request->authorization);
	soup_message_headers_replace(headers, "Stripe-Version", request->api_version);
	if (NULL != request->idempotency_key)
		soup_message_headers_replace(headers, "Idempotency-Key", request->idempotency_key);
	if (NULL != request->stripe_account)
		soup_message_headers_replace(headers, "Stripe-Account", request->stripe_account);
	if (0 != strcmp(request->method, "GET"))
	{
		g_autoptr(GBytes) bytes = g_bytes_new(request->body, strlen(request->body));

		soup_message_set_request_body_from_bytes(message,
			NULL != request->content_type ? request->content_type : "application/x-www-form-urlencoded", bytes);
	}

	/* The whole exchange on a context of its own, bounded by one timer. */
	g_main_context_push_thread_default(context);
	g_source_set_ready_time(timer, request->deadline_us);
	g_source_set_callback(timer, http_deadline, &transfer, NULL);
	g_source_attach(timer, context);
	if (NULL != cancellable)
		handler = g_cancellable_connect(cancellable, G_CALLBACK(http_cancelled), io, NULL);
	soup_session_send_async(session, message, G_PRIORITY_DEFAULT, io, http_headers_done, &transfer);
	while (!transfer.done)
		g_main_context_iteration(context, TRUE);
	if (0 != handler)
		g_cancellable_disconnect(cancellable, handler);
	g_source_destroy(timer);
	g_clear_object(&transfer.stream);
	soup_session_abort(session);
	g_main_context_pop_thread_default(context);

	if (transfer.expired && !(NULL != cancellable && g_cancellable_is_cancelled(cancellable)))
	{
		g_clear_error(&transfer.error);
		g_set_error_literal(&transfer.error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Provider deadline exhausted");
	}
	if (NULL != transfer.error)
	{
		answer->error = transfer.error;
		return;
	}
	if (NULL != memchr(body->data, 0, body->len))
	{
		g_set_error_literal(&answer->error, STRIPE_ERROR, STRIPE_ERROR_PROTOCOL, "Invalid response");
		return;
	}
	g_byte_array_append(body, (const guchar *)"", 1);
	headers = soup_message_get_response_headers(message);
	answer->status = soup_message_get_status(message);
	answer->retry_after = g_strdup(soup_message_headers_get_one(headers, "Retry-After"));
	answer->request_id = g_strdup(soup_message_headers_get_one(headers, "Request-Id"));
	answer->bytes = body->len - 1;
	answer->response = stripe_response_new(answer->status, (const gchar *)body->data,
		answer->request_id, answer->retry_after);
}

