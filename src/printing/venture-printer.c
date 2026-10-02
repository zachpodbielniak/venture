/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

typedef struct
{
	GMainLoop			*loop;
	GCancellable		*cancel;
	GSocketConnection	*connection;
	GBytes				*bytes;
	GError				*error;
	guint8				status[2];
	guint8				query[3];
	guint				phase;
	gboolean			expired;
} PrinterCall;

/* Only completion callbacks stop the loop, keeping callback state alive until cancellation
 * finishes. */
static void
call_done(
	PrinterCall	*call
){
	g_main_loop_quit(call->loop);
}

/* Cancel the entire exchange so DNS, writes and status reads share one deadline. */
static gboolean
call_expired(
	gpointer	data
){
	PrinterCall	*call = data;

	call->expired = TRUE;
	g_cancellable_cancel(call->cancel);
	return G_SOURCE_REMOVE;
}

static void send_status(PrinterCall *call);

/* Read each status answer before issuing another query to preserve response ordering. */
static void
status_read(
	GObject			*source,
	GAsyncResult	*result,
	gpointer		data
){
	PrinterCall	*call = data;
	gssize		n = g_input_stream_read_finish(G_INPUT_STREAM(source), result, &call->error);

	if (n != 1)
	{
		if (!call->error)
		{
			g_set_error_literal(&call->error, G_IO_ERROR, G_IO_ERROR_CLOSED,
								"Printer closed without a status response");
		}
		call_done(call);
		return;
	}
	call->phase++;

	if (call->phase == 2)
	{
		call_done(call);
	}
	else
	{
		send_status(call);
	}
}

/* A print completes after all bytes are sent; a status query must also await its answer. */
static void
sent(
	GObject			*source,
	GAsyncResult	*result,
	gpointer		data
){
	PrinterCall	*call = data;
	gsize		written;

	if (!g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result, &written, &call->error) ||
	   call->bytes)
	{
		call_done(call);
		return;
	}
	g_input_stream_read_async(g_io_stream_get_input_stream(G_IO_STREAM(call->connection)),
							  &call->status[call->phase], 1, G_PRIORITY_DEFAULT, call->cancel,
							  status_read, call);
}

/* Keep the query buffer alive through the asynchronous write. */
static void
send_status(
	PrinterCall	*call
){
	/* DLE EOT 1 is printer state; DLE EOT 4 is the paper sensor. Read
	 * each answer before the next query, as the Bash client does. */
	call->query[0] = VENTURE_ESCPOS_DLE;
	call->query[1] = VENTURE_ESCPOS_EOT;
	call->query[2] = call->phase ? VENTURE_ESCPOS_STATUS_PAPER : VENTURE_ESCPOS_STATUS_PRINTER;
	g_output_stream_write_all_async(g_io_stream_get_output_stream(G_IO_STREAM(call->connection)),
									call->query, sizeof call->query, G_PRIORITY_DEFAULT,
									call->cancel, sent, call);
}

/* Begin the chosen exchange only after the configured destination accepts the connection. */
static void
connected(
	GObject			*source,
	GAsyncResult	*result,
	gpointer		data
){
	PrinterCall	*call = data;

	call->connection =
		g_socket_client_connect_to_host_finish(G_SOCKET_CLIENT(source), result, &call->error);

	if (!call->connection)
	{
		call_done(call);
		return;
	}
	if (!call->bytes)
	{
		send_status(call);
	}
	else
	{
		gsize			size;
		gconstpointer	bytes = g_bytes_get_data(call->bytes, &size);

		g_output_stream_write_all_async(
			g_io_stream_get_output_stream(G_IO_STREAM(call->connection)), bytes, size,
			G_PRIORITY_DEFAULT, call->cancel, sent, call);
	}
}

GBytes *
venture_printer_exchange(
	const VenturePrinter	*printer,
	GBytes					*bytes,
	GError					**error
){
	g_autoptr(GSocketClient)	client = g_socket_client_new();
	g_autoptr(GCancellable)		cancel = g_cancellable_new();
	g_autoptr(GMainLoop)		loop = g_main_loop_new(g_main_context_get_thread_default(), FALSE);
	g_autoptr(GSource)			deadline = NULL;
	g_autofree gchar			*host = NULL;
	PrinterCall					call;
	GBytes						*answer = NULL;

	memset(&call, 0, sizeof call);
	call.loop = loop;
	call.cancel = cancel;
	call.bytes = bytes ? g_bytes_ref(bytes) : NULL;
	/* Snapshot before iterating: config reload can run in the nested loop. */
	host = g_strdup(printer->host);
	g_socket_client_set_enable_proxy(client, FALSE);
	g_socket_client_set_timeout(client, printer->timeout);
	deadline = g_timeout_source_new(printer->timeout * 1000);
	g_source_set_callback(deadline, call_expired, &call, NULL);
	g_source_attach(deadline, g_main_loop_get_context(loop));
	g_socket_client_connect_to_host_async(client, host, printer->port, cancel, connected, &call);
	g_main_loop_run(loop);
	g_source_destroy(deadline);

	if (call.expired)
	{
		g_clear_error(&call.error);
		g_set_error_literal(
			&call.error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT,
			"Printer deadline exceeded; delivery may be partial, check paper before retrying");
	}
	if (call.error)
	{
		g_propagate_error(error, call.error);
	}
	else
	{
		answer = g_bytes_new(call.status, bytes ? 0 : sizeof call.status);
	}
	/* No buffered stream: closing cannot flush an unbounded amount of data. */
	g_clear_object(&call.connection);
	g_clear_pointer(&call.bytes, g_bytes_unref);
	return answer;
}
