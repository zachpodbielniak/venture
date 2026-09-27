/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include <unistd.h>
#include "venture-test-util.h"
#include "printing-goldens.h"

/* Fixed Bash goldens catch framing, byte-count and layout drift, including
 * every barcode system the source tool actually accepts. */
static void
test_goldens(void)
{
	guint	i;

	for (i = 0; i < G_N_ELEMENTS(golden); i++)
	{
		VentureEscposDocument	document;
		g_autoptr(GBytes)		bytes = NULL;
		g_autoptr(GError)		error = NULL;
		const gchar				*qr[] = {"https://example.com/é", NULL};
		const gchar				*barcode[] = {"12345678", NULL};
		const gchar				*name = golden[i].name;

		venture_escpos_document_init(&document, NULL);
		document.body = golden[i].body;

		if (!strcmp(name, "prose") || !strcmp(name, "list") || !strcmp(name, "indent"))
		{
			document.width = 24;
		}
		if (!strcmp(name, "fences"))
		{
			document.width = 16;
		}
		if (!strcmp(name, "per_line"))
		{
			document.width = 24;
			document.reflow = 0;
		}
		if (!strcmp(name, "code"))
		{
			document.font = "auto";
			document.width = 16;
		}
		if (!strcmp(name, "auto_b"))
		{
			document.font = "auto";
		}
		if (!strcmp(name, "style"))
		{
			document.alignment = "right";
			document.bold = TRUE;
			document.double_height = TRUE;
			document.cut = FALSE;
		}
		if (!strcmp(name, "title"))
		{
			document.title = "A title";
			document.font = "a";
		}
		if (!strcmp(name, "qr"))
		{
			document.qr = qr;
		}
		if (!strcmp(name, "cut"))
		{
			document.cut_only = TRUE;
		}
		if (!strcmp(name, "no_wrap"))
		{
			document.wrap = FALSE;
			document.feed = 0;
		}
		if (!strcmp(name, "cp437") || !strcmp(name, "cp858"))
		{
			document.codepage = name;
		}
		if (g_str_has_prefix(name, "barcode_"))
		{
			document.barcodes = barcode;
			document.barcode_type = name + 8;
		}
		g_test_message("Bash golden: %s", name);
		bytes = venture_escpos_render(&document, &error);
		g_assert_no_error(error);
		g_assert_nonnull(bytes);

		if (g_bytes_get_size(bytes) != golden[i].length)
		{
			g_autofree gchar	*actual =
				g_base64_encode(g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes));
			g_autofree gchar	*expected =
				g_base64_encode((const guchar *) golden[i].bytes, golden[i].length);
			g_test_message("actual %s expected %s", actual, expected);
		}
		g_assert_cmpmem(g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes), golden[i].bytes,
						golden[i].length);
	}
}

/* Explicit timestamp avoids dependence on wall time and local timezone;
 * raw mode must preserve embedded NUL and printer opcodes unchanged. */
static void
test_timestamp_raw(void)
{
	g_autoptr(GError)		error = NULL;
	VentureEscposDocument	document;
	g_autoptr(GBytes)		bytes = NULL;
	g_autoptr(GBytes)		raw = g_bytes_new_static("\000\033@", 3);
	g_autoptr(GString)		expected = g_string_new_len(golden[7].bytes, golden[7].length);
	gsize					offset = 17 + 6 + strlen("A title\n") + 3;

	venture_escpos_document_init(&document, NULL);
	document.font = "a";
	document.title = "A title";
	document.body = "body";
	document.timestamp = "2026-09-27 13:45";
	g_string_insert(expected, offset, "2026-09-27 13:45\n");
	bytes = venture_escpos_render(&document, &error);
	g_assert_no_error(error);
	g_assert_nonnull(bytes);
	g_assert_cmpmem(g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes), expected->str,
					expected->len);
	g_clear_pointer(&bytes, g_bytes_unref);
	document.raw = raw;
	bytes = venture_escpos_render(&document, &error);
	g_assert_no_error(error);
	g_assert_nonnull(bytes);
	g_assert_true(g_bytes_equal(bytes, raw));
}

/* A bad replacement must neither retarget an existing printer nor guess an
 * enum. Config export must retain destinations and their layout defaults. */
static void
test_config(void)
{
	gboolean	success;

	static const gchar *bad[] = {
		"printing: {default: missing, printers: []}",
		"printing: {default: a, printers: [{name: a, host: localhost}, {name: a, host: "
		"localhost}]}",
		"printing: {default: a, printers: [{name: a, host: localhost, font: c}]}",
		"printing: {default: a, printers: [{name: a, host: localhost, codepage: utf8}]}",
		"printing: {default: a, printers: [{name: a, host: localhost, alignment: middle}]}",
		"printing: {default: a, printers: [{name: a, host: localhost, timeout: 0}]}",
		"printing: {default: a, printers: [{name: a, host: localhost, port: 65536}]}",
		"printing: {default: a, printers: [{name: a, host: localhost, cut: perhaps}]}"};
	g_autoptr(VentureConfig)	config = venture_config_new();
	g_autoptr(VentureConfig)	copy = venture_config_new();
	g_autoptr(GError)			error = NULL;
	g_autofree gchar			*yaml = NULL;
	const VenturePrinter		*printer;
	guint						i;

	/* Empty quoted YAML scalars become null in yaml-glib; shipped defaults
	 * must remain a valid disabled configuration on every startup. */
	success = venture_config_apply_yaml_string(config, venture_config_get_default_yaml(), &error);
	g_assert_no_error(error);
	g_assert_true(success);
	g_assert_cmpuint(venture_config_get_printers(config)->len, ==, 0);
	printer = venture_config_find_printer(config, NULL, &error);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_assert_null(printer);
	g_clear_error(&error);
	success = venture_config_apply_yaml_string(
		config,
		"printing:\n  default: till\n  printers:\n    - {name: till, host: 127.0.0.1}\n    - "
		"{name: office, host: localhost, port: 9200, font: a, width: 40, cut: false}\n",
		&error);
	g_assert_no_error(error);
	g_assert_true(success);
	g_assert_cmpuint(venture_config_get_printers(config)->len, ==, 2);
	printer = venture_config_find_printer(config, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(printer);
	g_assert_cmpstr(printer->name, ==, "till");
	g_assert_cmpuint(printer->port, ==, 9100);
	yaml = venture_config_to_yaml(config, FALSE);
	success = venture_config_apply_yaml_string(copy, yaml, &error);
	g_assert_no_error(error);
	g_assert_true(success);
	g_assert_cmpuint(venture_config_find_printer(copy, "office", NULL)->width, ==, 40);

	for (i = 0; i < G_N_ELEMENTS(bad); i++)
	{
		success = venture_config_apply_yaml_string(config, bad[i], &error);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
		g_assert_false(success);
		g_clear_error(&error);
		g_assert_cmpstr(venture_config_find_printer(config, NULL, NULL)->name, ==, "till");
	}
}

typedef struct
{
	GSocketConnection	*connection;
	GByteArray			*captured;
	GCancellable		*cancel;
	guint8				buffer[4096];
	guint				reads;
	guint				replies;
	gboolean			status;
	gboolean			stall;
} FakePrinter;

static void fake_read(FakePrinter *fake);

/* Capture bytes until EOF or cancellation; unexpected transport errors must not pass silently. */
static void
fake_read_done(
	GObject			*source,
	GAsyncResult	*result,
	gpointer		data
){
	FakePrinter			*fake = data;
	g_autoptr(GError)	error = NULL;
	gssize				count = g_input_stream_read_finish(G_INPUT_STREAM(source), result, &error);

	fake->reads--;

	if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
	{
		return;
	}
	g_assert_no_error(error);

	if (count <= 0)
	{
		return;
	}
	g_byte_array_append(fake->captured, fake->buffer, count);

	if (fake->status && fake->captured->len >= (fake->replies + 1) * 3)
	{
		guint8	response = fake->replies ? VENTURE_ESCPOS_STATUS_PAPER_OUT
			: VENTURE_ESCPOS_STATUS_OFFLINE;

		/* One byte to loopback cannot fill the send buffer; the socket also
		 * has a timeout so a broken test peer cannot hang the suite. */
		g_output_stream_write(g_io_stream_get_output_stream(G_IO_STREAM(fake->connection)),
							  &response, 1, fake->cancel, &error);
		g_assert_no_error(error);
		fake->replies++;
	}
	fake_read(fake);
}

/* Keep exactly one read outstanding so the fixture buffer stays valid through completion. */
static void
fake_read(
	FakePrinter	*fake
){
	fake->reads++;
	g_input_stream_read_async(g_io_stream_get_input_stream(G_IO_STREAM(fake->connection)),
							  fake->buffer, sizeof fake->buffer, G_PRIORITY_DEFAULT, fake->cancel,
							  fake_read_done, fake);
}

/* Hold the accepted connection until reads settle; a stalled peer intentionally never reads. */
static gboolean
fake_incoming(
	GSocketService		*service,
	GSocketConnection	*connection,
	GObject				*source,
	gpointer			data
){
	FakePrinter	*fake = data;

	(void) service;
	(void) source;
	fake->connection = g_object_ref(connection);
	g_socket_set_timeout(g_socket_connection_get_socket(connection), 1);

	if (!fake->stall)
	{
		fake_read(fake);
	}
	return TRUE;
}

/* Real async loopback capture catches partial writes, status ordering and
 * lifetime errors. A peer accepting without reading must hit one deadline. */
static void
test_sender(
	gconstpointer	mode
){
	FakePrinter					fake;
	VenturePrinter				printer;
	g_autoptr(GSocketService)	service = g_socket_service_new();
	g_autoptr(GSocketAddress)	address = NULL;
	g_autoptr(GSocketAddress)	effective = NULL;
	g_autoptr(GInetAddress)		loopback = g_inet_address_new_from_string("127.0.0.1");
	g_autoptr(GBytes)			bytes = NULL;
	g_autoptr(GBytes)			reply = NULL;
	g_autoptr(GError)			error = NULL;
	gint64						start;
	gint64						end;
	gchar						host[] = "127.0.0.1";

	memset(&fake, 0, sizeof fake);
	memset(&printer, 0, sizeof printer);
	fake.status = !g_strcmp0(mode, "status");
	fake.stall = !g_strcmp0(mode, "stall");
	fake.captured = g_byte_array_new();
	fake.cancel = g_cancellable_new();
	address = g_inet_socket_address_new(loopback, 0);
	g_socket_listener_add_address(G_SOCKET_LISTENER(service), address, G_SOCKET_TYPE_STREAM,
								  G_SOCKET_PROTOCOL_TCP, NULL, &effective, &error);
	g_assert_no_error(error);
	printer.host = host;
	printer.port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(effective));
	printer.timeout = 1;
	g_signal_connect(service, "incoming", G_CALLBACK(fake_incoming), &fake);

	if (!g_strcmp0(mode, "refused"))
	{
		/* Stop before closing: a running service still has an accept
		 * pending on the socket, and closing under it makes GIO warn
		 * "Socket is already closed", which the harness makes fatal. */
		g_socket_service_stop(service);
		g_socket_listener_close(G_SOCKET_LISTENER(service));
	}
	if (fake.stall)
	{
		bytes = g_bytes_new_take(g_malloc0(32 * 1024 * 1024), 32 * 1024 * 1024);
	}
	else if (!fake.status)
	{
		bytes = g_bytes_new_static(golden[0].bytes, golden[0].length);
	}
	start = g_get_monotonic_time();
	reply = venture_printer_exchange(&printer, bytes, &error);
	end = g_get_monotonic_time();
	g_assert_cmpint(end - start, <, 5 * G_TIME_SPAN_SECOND);

	if (fake.stall)
	{
		g_assert_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
		g_assert_null(reply);
	}
	else if (!g_strcmp0(mode, "refused"))
	{
		g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED);
		g_assert_null(reply);
	}
	else
	{
		g_assert_no_error(error);
		g_assert_nonnull(reply);
		/* Drain already queued input without ever waiting unboundedly. */
		while (fake.captured->len < (fake.status ? 6 : golden[0].length) &&
			  g_get_monotonic_time() - start < 3 * G_TIME_SPAN_SECOND)
		{
			g_main_context_iteration(NULL, FALSE);
		}
		if (fake.status)
		{
			g_assert_cmpmem(fake.captured->data, fake.captured->len, "\020\004\001\020\004\004", 6);
			g_assert_cmpmem(g_bytes_get_data(reply, NULL), g_bytes_get_size(reply), "\010\140", 2);
		}
		else
		{
			g_assert_cmpmem(fake.captured->data, fake.captured->len, golden[0].bytes,
							golden[0].length);
		}
	}
	g_cancellable_cancel(fake.cancel);

	while (fake.reads && g_get_monotonic_time() - end < 3 * G_TIME_SPAN_SECOND)
	{
		g_main_context_iteration(NULL, FALSE);
	}
	g_assert_cmpuint(fake.reads, ==, 0);
	g_socket_service_stop(service);
	g_socket_listener_close(G_SOCKET_LISTENER(service));
	g_clear_object(&fake.connection);
	g_object_unref(fake.cancel);
	g_byte_array_unref(fake.captured);
}

typedef struct
{
	GMainLoop	*loop;
	GBytes		*body;
	GError		*error;
} HttpCall;

/* Finish the HTTP operation before releasing its stack-backed callback state. */
static void
http_done(
	GObject			*source,
	GAsyncResult	*result,
	gpointer		data
){
	HttpCall	*call = data;

	call->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &call->error);
	g_main_loop_quit(call->loop);
}

/* Cancel the request instead of quitting early: the completion callback must
 * release its stack-backed state before the helper returns. */
static gboolean
http_expired(
	gpointer	data
){
	g_cancellable_cancel(G_CANCELLABLE(data));
	return G_SOURCE_REMOVE;
}

/* Drive the same context as the fake printer, with a whole-request deadline
 * so a broken route cannot leave the test waiting indefinitely. */
static guint
http_request(
	SoupSession	*session,
	guint		port,
	const gchar	*method,
	const gchar	*path,
	const gchar	*body,
	gchar		**out
){
	g_autofree gchar		*url = g_strdup_printf("http://127.0.0.1:%u%s", port, path);
	g_autoptr(SoupMessage)	message = soup_message_new(method, url);
	g_autoptr(GMainLoop)	loop = g_main_loop_new(NULL, FALSE);
	g_autoptr(GCancellable)	cancel = g_cancellable_new();
	g_autoptr(GSource)		deadline = g_timeout_source_new_seconds(5);
	HttpCall				call;

	memset(&call, 0, sizeof call);
	call.loop = loop;
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);

	if (body)
	{
		g_autoptr(GBytes)	bytes = g_bytes_new(body, strlen(body));

		soup_message_set_request_body_from_bytes(message, "application/json", bytes);
	}
	g_source_set_callback(deadline, http_expired, cancel, NULL);
	g_source_attach(deadline, g_main_loop_get_context(loop));
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, cancel, http_done,
									 &call);
	g_main_loop_run(loop);
	g_source_destroy(deadline);
	g_assert_no_error(call.error);

	if (out)
	{
		*out = g_strndup(g_bytes_get_data(call.body, NULL), g_bytes_get_size(call.body));
	}
	g_bytes_unref(call.body);
	return soup_message_get_status(message);
}

/* Search binary receipts by length because ESC/POS contains embedded NUL bytes. */
static gboolean
contains_bytes(
	const guint8	*bytes,
	gsize			size,
	const gchar		*text
){
	gsize	i;
	gsize	length = strlen(text);

	for (i = 0; i + length <= size; i++)
	{
		if (!memcmp(bytes + i, text, length))
		{
			return TRUE;
		}
	}
	return FALSE;
}

/* Both the web and socket-free checks save the same complete payment, so
 * a missing required field fails before transport can mask the regression. */
static VentureEntity *
create_payment(
	VentureDatabase	*db,
	gint64			org
){
	g_autoptr(VentureEntity)	company = NULL;
	g_autoptr(VentureEntity)	payment = NULL;
	g_autoptr(GError)			error = NULL;
	g_autoptr(VentureMoney)		amount = NULL;
	g_autoptr(GDateTime)		date = g_date_time_new_utc(2026, 9, 27, 0, 0, 0);
	gboolean					success;

	amount = venture_money_from_string("12.34 USD", "USD", &error);
	g_assert_no_error(error);
	g_assert_nonnull(amount);
	company =
		g_object_new(VENTURE_TYPE_COMPANY, "name", "Test customer", "organization-id", org, NULL);
	success = venture_database_save(db, company, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(success);
	payment = g_object_new(VENTURE_TYPE_PAYMENT, "organization-id", org, "customer-id",
						   venture_entity_get_id(company), "amount", amount, "date", date, "method", "cash", NULL);
	success = venture_database_save(db, payment, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(success);
	return g_steal_pointer(&payment);
}

/* Exercise required payment fields without a socket: transport denial must
 * not leave the receipt's saved-money and date path untested. */
static void
test_payment(void)
{
	g_autoptr(VentureConfig)	config = venture_config_new();
	g_autoptr(VentureDatabase)	db = NULL;
	g_autoptr(VentureContext)	context = NULL;
	g_autoptr(VentureEntity)	payment = NULL;
	g_autoptr(GBytes)			bytes = NULL;
	g_autoptr(GError)			error = NULL;
	g_autofree gchar			*state = NULL;
	gboolean					success;

	state = g_dir_make_tmp("venture-printing-payment-XXXXXX", &error);
	g_assert_no_error(error);
	g_assert_nonnull(state);
	g_object_set(config, "state-dir", state, "mail-receipts", FALSE, NULL);
	db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	success = venture_database_migrate(db, venture_entity_registry_get_default(), &error);
	g_assert_no_error(error);
	g_assert_true(success);
	context = venture_context_new(config, db);
	payment = create_payment(db, venture_context_get_default_organization_id(context));
	bytes = venture_financial_documents_thermal(context, payment, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(bytes);
	g_assert_true(contains_bytes(g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes),
								 "Amount received: $12.34"));
	g_assert_true(contains_bytes(g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes), "Date:"));
	g_clear_object(&context);
	g_clear_object(&db);
	venture_test_remove_tree(state);
}

/* Exercise the actual API to the fake printer: a successful HTTP response
 * alone would miss a route that never sent the amount. Empty config must
 * remove controls in both looks and unknown names must never contact TCP. */
static void
test_web(void)
{
	gboolean					success;
	g_autoptr(VentureConfig)	config = venture_config_new();
	g_autoptr(VentureDatabase)	db = NULL;
	g_autoptr(VentureContext)	context = NULL;
	g_autoptr(VentureWebServer)	server = NULL;
	g_autoptr(VentureEntity)	payment = NULL;

	g_autoptr(SoupSession)		session = soup_session_new_with_options("timeout", 5, NULL);
	g_autoptr(GSocketService)	service = g_socket_service_new();
	g_autoptr(GError)			error = NULL;
	g_autofree gchar			*state = NULL;
	g_autofree gchar			*yaml = NULL;
	g_autofree gchar			*path = NULL;
	g_autofree gchar			*page = NULL;
	g_autofree gchar			*response = NULL;
	guint16						port = (guint16)(40000 + getpid() % 10000);
	guint16						printer_port;
	gint64						org;
	gint64						id;
	gint64						until;
	FakePrinter					fake;

	memset(&fake, 0, sizeof fake);
	fake.captured = g_byte_array_new();
	fake.cancel = g_cancellable_new();
	/* Restrict the fake to loopback; never listen on the LAN. */
	{
		g_autoptr(GInetAddress)		address = g_inet_address_new_from_string("127.0.0.1");
		g_autoptr(GSocketAddress)	bind = g_inet_socket_address_new(address, 0);
		g_autoptr(GSocketAddress)	effective = NULL;

		g_socket_listener_add_address(G_SOCKET_LISTENER(service), bind, G_SOCKET_TYPE_STREAM,
									  G_SOCKET_PROTOCOL_TCP, NULL, &effective, &error);
		g_assert_no_error(error);
		printer_port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(effective));
	}
	g_signal_connect(service, "incoming", G_CALLBACK(fake_incoming), &fake);
	state = g_dir_make_tmp("venture-printing-XXXXXX", &error);
	g_assert_no_error(error);
	g_assert_nonnull(state);
	g_object_set(config, "state-dir", state, "server-port", (gint64) port, "security-require-auth",
				 FALSE, "mail-receipts", FALSE, "server-base-url", "https://venture.example", NULL);
	db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	success = venture_database_migrate(db, venture_entity_registry_get_default(), &error);
	g_assert_no_error(error);
	g_assert_true(success);
	context = venture_context_new(config, db);
	org = venture_context_get_default_organization_id(context);
	payment = create_payment(db, org);
	id = venture_entity_get_id(payment);
	server = venture_web_server_new(context, &error);
	g_assert_no_error(error);
	success = venture_web_server_start(server, &error);
	g_assert_no_error(error);
	g_assert_true(success);
	path = g_strdup_printf("/e/payment/%" G_GINT64_FORMAT, id);
	g_assert_cmpuint(http_request(session, port, "GET", path, NULL, &page), ==, 200);
	g_assert_null(strstr(page, "Print receipt"));
	g_clear_pointer(&page, g_free);
	g_assert_cmpuint(http_request(session, port, "GET", "/api/v1/printers", NULL, &response), ==,
					 200);
	{
		g_autoptr(JsonNode)	list = venture_json_parse(response, &error);

		g_assert_no_error(error);
		g_assert_true(JSON_NODE_HOLDS_ARRAY(list));
		g_assert_cmpuint(json_array_get_length(json_node_get_array(list)), ==, 0);
	}
	g_clear_pointer(&response, g_free);
	{
		g_autofree gchar	*print_path =
			g_strdup_printf("/api/v1/print/payment/%" G_GINT64_FORMAT, id);
		g_assert_cmpuint(http_request(session, port, "POST", print_path, "{}", NULL), ==, 404);
	}
	yaml = g_strdup_printf("printing: {default: test, printers: [{name: test, host: 127.0.0.1, "
						   "port: %u, timeout: 1}]}\n",
						   printer_port);
	success = venture_config_apply_yaml_string(config, yaml, &error);
	g_assert_no_error(error);
	g_assert_true(success);
	g_assert_cmpuint(http_request(session, port, "GET", path, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "Print receipt"));
	g_assert_nonnull(strstr(page, "value=\"test\" selected"));
	g_clear_pointer(&page, g_free);
	g_object_set(config, "ui-look", "classic", NULL);
	g_assert_cmpuint(http_request(session, port, "GET", path, NULL, &page), ==, 200);
	g_assert_nonnull(strstr(page, "Print receipt"));
	g_clear_pointer(&page, g_free);
	g_clear_pointer(&path, g_free);
	path = g_strdup_printf("/api/v1/print/payment/%" G_GINT64_FORMAT, id);
	g_assert_cmpuint(http_request(session, port, "POST", path, "{\"printer\":\"unknown\"}", NULL),
					 ==, 404);
	g_assert_cmpuint(http_request(session, port, "POST", path, "{\"host\":\"127.0.0.1\"}", NULL),
					 ==, 400);
	g_assert_cmpuint(fake.captured->len, ==, 0);
	g_assert_cmpuint(http_request(session, port, "POST", path, "{}", NULL), ==, 200);
	/* Accept may still be queued when TCP delivery completes; wait for it
	 * as well as EOF before inspecting or freeing the fake peer. */
	until = g_get_monotonic_time() + G_TIME_SPAN_SECOND;

	while ((!fake.connection || fake.reads) && g_get_monotonic_time() < until)
	{
		g_main_context_iteration(NULL, FALSE);
	}
	g_assert_true(
		contains_bytes(fake.captured->data, fake.captured->len, "Amount received: $12.34"));
	g_assert_true(contains_bytes(fake.captured->data, fake.captured->len,
								 "https://venture.example/e/payment/"));
	g_cancellable_cancel(fake.cancel);
	until = g_get_monotonic_time() + G_TIME_SPAN_SECOND;

	while (fake.reads && g_get_monotonic_time() < until)
	{
		g_main_context_iteration(NULL, FALSE);
	}
	g_assert_cmpuint(fake.reads, ==, 0);
	g_socket_service_stop(service);
	g_socket_listener_close(G_SOCKET_LISTENER(service));
	g_clear_object(&fake.connection);
	g_object_unref(fake.cancel);
	g_byte_array_unref(fake.captured);
	venture_web_server_stop(server);
	g_clear_object(&server);
	g_clear_object(&context);
	g_clear_object(&db);
	venture_test_remove_tree(state);
}

/* A draft invoice with shipping must use the PDF's totals helper, not an
 * independently rounded or settlement-derived total. */
static void
test_invoice(void)
{
	gboolean					success;
	g_autoptr(VentureConfig)	config = venture_config_new();
	g_autoptr(VentureDatabase)	db = NULL;
	g_autoptr(VentureContext)	context = NULL;
	g_autoptr(VentureEntity)	invoice = NULL;
	g_autoptr(VentureEntity)	company = NULL;
	g_autoptr(GError)			error = NULL;
	g_autoptr(JsonNode)			node = NULL;
	g_autoptr(GBytes)			bytes = NULL;
	g_autofree gchar			*state = NULL;
	g_autofree gchar			*json = NULL;
	gint64						org;

	state = g_dir_make_tmp("venture-printing-invoice-XXXXXX", &error);
	g_assert_no_error(error);
	g_assert_nonnull(state);
	g_object_set(config, "state-dir", state, "mail-receipts", FALSE, NULL);
	db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	success = venture_database_migrate(db, venture_entity_registry_get_default(), &error);
	g_assert_no_error(error);
	g_assert_true(success);
	context = venture_context_new(config, db);
	org = venture_context_get_default_organization_id(context);
	company = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", org, "name", "Customer", NULL);
	success = venture_database_save(db, company, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(success);
	json = g_strdup_printf("{\"company_id\":%" G_GINT64_FORMAT
						   ",\"number\":\"INV-PRINT\",\"lines\":[{\"description\":\"Books\","
						   "\"quantity\":2,\"unit_price\":\"9.50 USD\"}]}",
						   venture_entity_get_id(company));
	node = venture_json_parse(json, &error);
	g_assert_no_error(error);
	invoice = venture_document_service_compose_invoice(venture_document_service_get(db), org,
													   json_node_get_object(node), NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(invoice);
	{
		g_autoptr(VentureMoney)	shipping = venture_money_from_string("2.00 USD", "USD", NULL);

		g_object_set(invoice, "shipping-amount", shipping, NULL);
		success = venture_database_save(db, invoice, NULL, &error);
		g_assert_no_error(error);
		g_assert_true(success);
	}
	bytes = venture_financial_documents_thermal(context, invoice, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(bytes);
	g_assert_true(
		contains_bytes(g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes), "Total: $21.00"));
	g_assert_true(
		contains_bytes(g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes), "Books: $19.00"));
	g_clear_object(&context);
	g_clear_object(&db);
	venture_test_remove_tree(state);
}

/* Register socket-free checks independently so transport restrictions cannot hide renderer
 * failures. */
int
main(
	int		argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/printing/goldens", test_goldens);
	g_test_add_func("/printing/timestamp-raw", test_timestamp_raw);
	g_test_add_func("/printing/config", test_config);
	g_test_add_func("/printing/payment", test_payment);
	g_test_add_func("/printing/web", test_web);
	g_test_add_func("/printing/invoice", test_invoice);
	g_test_add_data_func("/printing/sender/success", "success", test_sender);
	g_test_add_data_func("/printing/sender/refused", "refused", test_sender);
	g_test_add_data_func("/printing/sender/stall", "stall", test_sender);
	g_test_add_data_func("/printing/sender/status", "status", test_sender);
	return g_test_run();
}
