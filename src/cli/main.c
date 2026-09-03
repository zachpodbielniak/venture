/*
 * main.c - venturectl, the VENTURE command-line client
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * venturectl drives the server over its REST API. It deliberately does not
 * open the database: one writer, one set of validation rules, one audit
 * trail. A second process writing directly would bypass all three.
 *
 * It links only libventure-core.a and yaml-glib, so `make venturectl` works
 * on a machine that cannot build the server at all.
 *
 * The output is designed to be piped. Every subcommand takes --format, the
 * default is a table for a terminal and JSON when stdout is not one, and a
 * failure sets a distinct exit code so a shell script can branch on it.
 */

#include "venture.h"

#include <libsoup/soup.h>
#include <stdio.h>
#include <yaml-glib.h>

#include <stdlib.h>
#include <unistd.h>

typedef struct
{
	SoupSession		*session;
	gchar			*base_url;
	gchar			*token;
	VentureOutputFormat	 format;
	gboolean		 quiet;

	/* Whether --server and --token came from the command line rather than
	 * the environment. `mcp` has to tell the difference: it refuses a
	 * token in argv, and silently ignoring one somebody passed would be
	 * worse than refusing it. */
	gboolean		 server_from_argv;
	gboolean		 token_from_argv;

	/* `mcp` only: let its write tools apply rather than hold. */
	gboolean		 apply_writes;

	/* Propose a write instead of making it. Only create, update and
	 * delete honour it, and passing it to anything else is refused. */
	gboolean		 stage;
} VentureCli;

/* --- Output -------------------------------------------------------------- */

/*
 * Chooses the default output format. A terminal gets a table; a pipe gets
 * JSON, so `venturectl list sale | jq` works without a flag.
 */
static VentureOutputFormat
venture_cli_default_format(void)
{
	return isatty(STDOUT_FILENO) ? VENTURE_OUTPUT_FORMAT_TABLE
	                             : VENTURE_OUTPUT_FORMAT_JSON;
}

static void
venture_cli_print_error(const GError *error)
{
	g_printerr("venturectl: %s\n",
	           (NULL != error) ? error->message : "unknown failure");
}

/*
 * Renders an array of records as an aligned table, choosing columns from
 * what the records actually contain rather than from a hardcoded list.
 */
static void
venture_cli_print_records(
	VentureCli	*cli,
	JsonNode	*node
){
	g_autoptr(GPtrArray) columns = NULL;
	JsonArray *records;
	JsonObject *root;
	guint i;
	guint j;

	if (!JSON_NODE_HOLDS_OBJECT(node))
	{
		g_autofree gchar *text = NULL;

		text = venture_json_to_string(node, TRUE);
		g_print("%s\n", text);
		return;
	}

	root = json_node_get_object(node);

	if (!json_object_has_member(root, "records"))
	{
		g_autofree gchar *text = NULL;

		text = venture_json_to_string(node, TRUE);
		g_print("%s\n", text);
		return;
	}

	records = json_object_get_array_member(root, "records");

	if (0 == json_array_get_length(records))
	{
		if (!cli->quiet)
			g_print("No records.\n");

		return;
	}

	/*
	 * A record has forty fields and a terminal has eighty columns, so
	 * only the few that identify a row are shown. --format=json is there
	 * for everything else.
	 */
	columns = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(columns, g_strdup("id"));

	{
		static const gchar *const preferred[] = {
			"name", "title", "display_name", "description", "status",
			"occurred_at", NULL
		};
		JsonObject *first;
		gsize k;

		first = json_array_get_object_element(records, 0);

		for (k = 0; NULL != preferred[k]; k++)
		{
			if (json_object_has_member(first, preferred[k]))
				g_ptr_array_add(columns, g_strdup(preferred[k]));
		}
	}

	{
		g_autofree gsize *widths = NULL;

		widths = g_new0(gsize, columns->len);

		for (i = 0; i < columns->len; i++)
			widths[i] = strlen(g_ptr_array_index(columns, i));

		for (j = 0; j < json_array_get_length(records); j++)
		{
			JsonObject *record;

			record = json_array_get_object_element(records, j);

			for (i = 0; i < columns->len; i++)
			{
				g_autofree gchar *cell = NULL;
				gsize length;

				cell = venture_json_to_string(
					json_object_get_member(record,
					                       g_ptr_array_index(columns, i)),
					FALSE);
				length = (NULL != cell) ? strlen(cell) : 0;

				if (length > widths[i])
					widths[i] = MIN(length, 40);
			}
		}

		for (i = 0; i < columns->len; i++)
		{
			g_print("%-*s  ", (int)widths[i],
			        (const gchar *)g_ptr_array_index(columns, i));
		}

		g_print("\n");

		for (j = 0; j < json_array_get_length(records); j++)
		{
			JsonObject *record;

			record = json_array_get_object_element(records, j);

			for (i = 0; i < columns->len; i++)
			{
				g_autofree gchar *cell = NULL;
				g_autofree gchar *trimmed = NULL;
				JsonNode *member;

				member = json_object_get_member(record,
					g_ptr_array_index(columns, i));

				if ((NULL != member) && JSON_NODE_HOLDS_VALUE(member) &&
				    (G_TYPE_STRING == json_node_get_value_type(member)))
					cell = g_strdup(json_node_get_string(member));
				else
					cell = venture_json_to_string(member, FALSE);

				if (0 == g_strcmp0(cell, "null"))
				{
					g_free(cell);
					cell = g_strdup("");
				}

				trimmed = venture_truncate(cell, 40);
				g_print("%-*s  ", (int)widths[i],
				        (NULL != trimmed) ? trimmed : "");
			}

			g_print("\n");
		}
	}

	if (!cli->quiet && json_object_has_member(root, "total"))
	{
		g_print("\n%" G_GINT64_FORMAT " total\n",
		        json_object_get_int_member(root, "total"));
	}
}

/*
 * One CSV cell from one JSON member. Money and anything else structured
 * that carries a formatted twin prints that; other structures print as
 * compact JSON, which a spreadsheet can at least hold without corrupting.
 */
static gchar *
venture_cli_csv_cell(JsonNode *member)
{
	if ((NULL == member) || JSON_NODE_HOLDS_NULL(member))
		return g_strdup("");

	if (JSON_NODE_HOLDS_VALUE(member) &&
	    (G_TYPE_STRING == json_node_get_value_type(member)))
		return g_strdup(json_node_get_string(member));

	if (JSON_NODE_HOLDS_OBJECT(member))
	{
		JsonObject *object;

		object = json_node_get_object(member);

		if (json_object_has_member(object, "formatted"))
		{
			return g_strdup(json_object_get_string_member(object,
			                                              "formatted"));
		}
	}

	return venture_json_to_string(member, FALSE);
}

/*
 * Renders a response as CSV: a list's records as rows, a single record as
 * one row. Columns come from the first record in wire order, so the header
 * matches what `describe` documents. Unlike the table, nothing is truncated
 * and every field appears -- CSV is for the spreadsheet, not the eye.
 */
static void
venture_cli_print_csv(JsonNode *node)
{
	g_autoptr(JsonArray) wrapped = NULL;
	g_autoptr(GList) members = NULL;
	JsonArray *records;
	JsonObject *root;
	JsonObject *first;
	GList *iter;
	guint length;
	guint j;

	if (!JSON_NODE_HOLDS_OBJECT(node))
	{
		g_autofree gchar *text = NULL;

		text = venture_json_to_string(node, TRUE);
		g_print("%s\n", text);
		return;
	}

	root = json_node_get_object(node);

	if (json_object_has_member(root, "records"))
	{
		records = json_object_get_array_member(root, "records");
	}
	else
	{
		/* A single record becomes a one-row sheet with the same header
		 * a list would have, so `get` and `list` outputs concatenate. */
		wrapped = json_array_new();
		json_array_add_object_element(wrapped, json_object_ref(root));
		records = wrapped;
	}

	length = json_array_get_length(records);

	if (0 == length)
		return;

	first = json_array_get_object_element(records, 0);
	members = json_object_get_members(first);

	for (iter = members; NULL != iter; iter = iter->next)
	{
		g_autofree gchar *escaped = NULL;

		escaped = venture_csv_escape(iter->data);
		g_print("%s%s", (iter == members) ? "" : ",", escaped);
	}

	g_print("\n");

	for (j = 0; j < length; j++)
	{
		JsonObject *record;

		record = json_array_get_object_element(records, j);

		for (iter = members; NULL != iter; iter = iter->next)
		{
			g_autofree gchar *cell = NULL;
			g_autofree gchar *escaped = NULL;

			cell = venture_cli_csv_cell(
				json_object_get_member(record, iter->data));
			escaped = venture_csv_escape(cell);
			g_print("%s%s", (iter == members) ? "" : ",", escaped);
		}

		g_print("\n");
	}
}

static void
venture_cli_output(
	VentureCli	*cli,
	JsonNode	*node
){
	switch (cli->format)
	{
	case VENTURE_OUTPUT_FORMAT_JSON:
	{
		g_autofree gchar *text = NULL;

		text = venture_json_to_string(node, TRUE);
		g_print("%s\n", text);
		return;
	}

	case VENTURE_OUTPUT_FORMAT_CSV:
		venture_cli_print_csv(node);
		return;

	case VENTURE_OUTPUT_FORMAT_YAML:
	{
		g_autoptr(YamlDocument) document = NULL;
		g_autoptr(YamlGenerator) generator = NULL;
		g_autofree gchar *text = NULL;

		document = yaml_document_from_json_node(node);

		if (NULL == document)
		{
			text = venture_json_to_string(node, TRUE);
			g_print("%s\n", text);
			return;
		}

		generator = yaml_generator_new();
		yaml_generator_set_document(generator, document);
		yaml_generator_set_indent(generator, 2);
		text = yaml_generator_to_data(generator, NULL, NULL);
		g_print("%s", (NULL != text) ? text : "");
		return;
	}

	case VENTURE_OUTPUT_FORMAT_TABLE:
	default:
		venture_cli_print_records(cli, node);
		return;
	}
}

/* --- HTTP ---------------------------------------------------------------- */

/*
 * Sends one request and returns the raw response body with its status.
 * Everything both decoders need -- the URL check, the bearer token, the
 * body encoding -- lives here once.
 */
static GBytes *
venture_cli_send(
	VentureCli	 *cli,
	const gchar	 *method,
	const gchar	 *path,
	JsonNode	 *body,
	guint		 *out_status,
	GError		**error
){
	g_autoptr(SoupMessage) message = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autofree gchar *url = NULL;
	GBytes *response;

	url = g_strconcat(cli->base_url, path, NULL);
	message = soup_message_new(method, url);

	if (NULL == message)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" is not a usable URL", url);
		return NULL;
	}

	if (NULL != cli->token)
	{
		g_autofree gchar *authorization = NULL;

		authorization = g_strconcat("Bearer ", cli->token, NULL);
		soup_message_headers_append(soup_message_get_request_headers(message),
		                            "Authorization", authorization);
	}

	if (NULL != body)
	{
		g_autofree gchar *encoded = NULL;
		g_autoptr(GBytes) bytes = NULL;

		encoded = venture_json_to_string(body, FALSE);
		bytes = g_bytes_new(encoded, strlen(encoded));
		soup_message_set_request_body_from_bytes(message, "application/json",
		                                         bytes);
	}

	response = soup_session_send_and_read(cli->session, message, NULL,
	                                      &local_error);

	if (NULL == response)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NETWORK,
		            "Cannot reach %s: %s", cli->base_url,
		            local_error->message);
		return NULL;
	}

	*out_status = soup_message_get_status(message);

	return response;
}

/*
 * Turns a JSON error body back into a real #GError with its original code,
 * so a failure deep in the server surfaces here with the same meaning and
 * the same exit status it would have had locally.
 */
static void
venture_cli_error_from_body(
	const gchar	 *text,
	GError		**error
){
	g_autoptr(JsonNode) node = NULL;
	JsonObject *object = NULL;
	const gchar *slug;
	const gchar *message_text;
	VentureError code;

	node = venture_json_parse(text, NULL);

	if ((NULL != node) && JSON_NODE_HOLDS_OBJECT(node))
		object = json_node_get_object(node);

	slug = (NULL != object)
		? venture_json_object_get_string(object, "error", NULL) : NULL;
	message_text = (NULL != object)
		? venture_json_object_get_string(object, "message", NULL) : NULL;

	if ((NULL == slug) || !venture_error_from_slug(slug, &code))
		code = VENTURE_ERROR_FAILED;

	g_set_error(error, VENTURE_ERROR, code, "%s",
	            (NULL != message_text) ? message_text
	                                   : "the request failed");
}

/*
 * Performs a request and decodes the JSON response.
 */
static JsonNode *
venture_cli_request(
	VentureCli	 *cli,
	const gchar	 *method,
	const gchar	 *path,
	JsonNode	 *body,
	GError		**error
){
	g_autoptr(GBytes) response = NULL;
	g_autofree gchar *text = NULL;
	JsonNode *node;
	guint status;

	response = venture_cli_send(cli, method, path, body, &status, error);

	if (NULL == response)
		return NULL;

	text = g_strndup(g_bytes_get_data(response, NULL),
	                 g_bytes_get_size(response));

	if (status >= 400)
	{
		venture_cli_error_from_body(text, error);
		return NULL;
	}

	node = venture_json_parse(text, NULL);

	if (NULL == node)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
		            "The server returned something that is not JSON (HTTP %u)",
		            status);
		return NULL;
	}

	return node;
}

/*
 * Fetches a path and returns the body untouched, for responses the server
 * has already rendered -- a report's CSV comes back exactly as the web
 * export would produce it. Failures still decode as JSON errors.
 */
static gchar *
venture_cli_request_text(
	VentureCli	 *cli,
	const gchar	 *path,
	GError		**error
){
	g_autoptr(GBytes) response = NULL;
	g_autofree gchar *text = NULL;
	guint status;

	response = venture_cli_send(cli, "GET", path, NULL, &status, error);

	if (NULL == response)
		return NULL;

	text = g_strndup(g_bytes_get_data(response, NULL),
	                 g_bytes_get_size(response));

	if (status >= 400)
	{
		venture_cli_error_from_body(text, error);
		return NULL;
	}

	return g_steal_pointer(&text);
}

/*
 * Fetches a path and returns the bytes untouched.
 *
 * Distinct from venture_cli_request_text() because an archive is not text:
 * g_strndup'ing it is fine, but every consumer of that string measures it
 * with strlen and stops at the first NUL, which for a zip is within the
 * first few bytes. Failures are still JSON, and are decoded from a copy.
 */
static GBytes *
venture_cli_request_bytes(
	VentureCli	 *cli,
	const gchar	 *method,
	const gchar	 *path,
	GError		**error
){
	g_autoptr(GBytes) response = NULL;
	guint status;

	response = venture_cli_send(cli, method, path, NULL, &status, error);

	if (NULL == response)
		return NULL;

	if (status >= 400)
	{
		g_autofree gchar *text = NULL;

		text = g_strndup(g_bytes_get_data(response, NULL),
		                 g_bytes_get_size(response));
		venture_cli_error_from_body(text, error);

		return NULL;
	}

	return g_steal_pointer(&response);
}

/* --- Subcommands --------------------------------------------------------- */

/*
 * Turns `key=value` arguments into a JSON object for create and update.
 * Values stay strings; the server's decoder is tolerant and knows each
 * field's real type, so the CLI does not have to guess.
 */
static JsonNode *
venture_cli_values_from_args(
	gchar	**args,
	gint	  first
){
	g_autoptr(JsonBuilder) builder = NULL;
	gint i;

	builder = json_builder_new();
	json_builder_begin_object(builder);

	for (i = first; (NULL != args) && (NULL != args[i]); i++)
	{
		g_auto(GStrv) parts = NULL;

		parts = g_strsplit(args[i], "=", 2);

		if ((NULL == parts[0]) || (NULL == parts[1]))
			continue;

		json_builder_set_member_name(builder, parts[0]);
		json_builder_add_string_value(builder, parts[1]);
	}

	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

static gint
venture_cli_command_list(
	VentureCli	 *cli,
	gchar		**args,
	GError		**error
){
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GString) path = NULL;
	gint i;

	if ((NULL == args) || (NULL == args[1]))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Which record type? Try: venturectl types");
		return -1;
	}

	path = g_string_new("/api/v1/");
	g_string_append_uri_escaped(path, args[1], NULL, FALSE);

	/* Remaining `key=value` arguments become query parameters, so
	 * `venturectl list sale status__eq=active limit=10` reads naturally. */
	for (i = 2; NULL != args[i]; i++)
	{
		g_auto(GStrv) parts = NULL;

		parts = g_strsplit(args[i], "=", 2);

		if ((NULL == parts[0]) || (NULL == parts[1]))
			continue;

		g_string_append_c(path, (2 == i) ? '?' : '&');
		g_string_append_uri_escaped(path, parts[0], NULL, FALSE);
		g_string_append_c(path, '=');
		g_string_append_uri_escaped(path, parts[1], NULL, FALSE);
	}

	node = venture_cli_request(cli, "GET", path->str, NULL, error);

	if (NULL == node)
		return -1;

	venture_cli_output(cli, node);

	return 0;
}

static gint
venture_cli_command_get(
	VentureCli	 *cli,
	gchar		**args,
	GError		**error
){
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *path = NULL;

	if ((NULL == args) || (NULL == args[1]) || (NULL == args[2]))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Usage: venturectl get <type> <id>");
		return -1;
	}

	path = g_strdup_printf("/api/v1/%s/%s", args[1], args[2]);
	node = venture_cli_request(cli, "GET", path, NULL, error);

	if (NULL == node)
		return -1;

	/* A single record has nothing to tabulate, so the table default
	 * prints it in full as JSON. An explicitly requested format is
	 * honoured: -f csv on a get produces a one-row sheet. */
	if (VENTURE_OUTPUT_FORMAT_TABLE == cli->format)
	{
		g_autofree gchar *text = NULL;

		text = venture_json_to_string(node, TRUE);
		g_print("%s\n", text);
	}
	else
	{
		venture_cli_output(cli, node);
	}

	return 0;
}

/*
 * The path a write goes to, with `?stage=1` when --stage was passed.
 *
 * One function for the three write commands, so they cannot disagree about
 * the spelling -- and so a command that forgot it would be a missing call
 * rather than a missing string.
 */
static gchar *
venture_cli_write_path(
	VentureCli	*cli,
	const gchar	*path
){
	if (cli->stage)
		return g_strconcat(path, "?stage=1", NULL);

	return g_strdup(path);
}

/*
 * Whether a reply says the change was staged rather than made.
 *
 * Read from the reply, never from the flag: a server that ignored the
 * parameter answers with the record it just wrote, and reporting that as
 * waiting for approval would send somebody looking for a decision nobody
 * has to make while the figure sits in the accounts.
 */
static gboolean
venture_cli_report_staged(
	VentureCli	*cli,
	JsonNode	*node
){
	JsonObject *object;
	JsonObject *confirmation;

	if (!JSON_NODE_HOLDS_OBJECT(node))
		return FALSE;

	object = json_node_get_object(node);

	if (!json_object_get_boolean_member_with_default(object, "staged", FALSE))
		return FALSE;

	if (cli->quiet)
		return TRUE;

	confirmation = json_object_has_member(object, "confirmation")
		? json_object_get_object_member(object, "confirmation") : NULL;

	{
		const gchar *id;

		id = (NULL != confirmation)
			? venture_json_object_get_string(confirmation, "id", "?") : "?";

		g_print("Not applied. It is waiting for approval as %s.\n", id);
		g_print("  approve: POST /api/v1/confirmations/%s/approve\n", id);
		g_print("  reject:  POST /api/v1/confirmations/%s/reject\n", id);
	}

	return TRUE;
}

static gint
venture_cli_command_create(
	VentureCli	 *cli,
	gchar		**args,
	GError		**error
){
	g_autoptr(JsonNode) values = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *path = NULL;

	if ((NULL == args) || (NULL == args[1]))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Usage: venturectl create <type> field=value ...");
		return -1;
	}

	values = venture_cli_values_from_args(args, 2);
	{
		g_autofree gchar *base = NULL;

		base = g_strdup_printf("/api/v1/%s", args[1]);
		path = venture_cli_write_path(cli, base);
	}

	node = venture_cli_request(cli, "POST", path, values, error);

	if (NULL == node)
		return -1;

	venture_cli_report_staged(cli, node);

	{
		g_autofree gchar *text = NULL;

		text = venture_json_to_string(node, TRUE);
		g_print("%s\n", text);
	}

	return 0;
}

static gint
venture_cli_command_update(
	VentureCli	 *cli,
	gchar		**args,
	GError		**error
){
	g_autoptr(JsonNode) values = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *path = NULL;

	if ((NULL == args) || (NULL == args[1]) || (NULL == args[2]))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Usage: venturectl update <type> <id> field=value ...");
		return -1;
	}

	values = venture_cli_values_from_args(args, 3);
	{
		g_autofree gchar *base = NULL;

		base = g_strdup_printf("/api/v1/%s/%s", args[1], args[2]);
		path = venture_cli_write_path(cli, base);
	}

	node = venture_cli_request(cli, "PATCH", path, values, error);

	if (NULL == node)
		return -1;

	venture_cli_report_staged(cli, node);

	{
		g_autofree gchar *text = NULL;

		text = venture_json_to_string(node, TRUE);
		g_print("%s\n", text);
	}

	return 0;
}

static gint
venture_cli_command_delete(
	VentureCli	 *cli,
	gchar		**args,
	GError		**error
){
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *path = NULL;

	if ((NULL == args) || (NULL == args[1]) || (NULL == args[2]))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Usage: venturectl delete <type> <id>");
		return -1;
	}

	{
		g_autofree gchar *base = NULL;

		base = g_strdup_printf("/api/v1/%s/%s", args[1], args[2]);
		path = venture_cli_write_path(cli, base);
	}

	node = venture_cli_request(cli, "DELETE", path, NULL, error);

	if (NULL == node)
		return -1;

	/* Never "Deleted" for a change that is only waiting to be: the
	 * sentence is the whole of what most callers read. */
	if (venture_cli_report_staged(cli, node))
		return 0;

	if (!cli->quiet)
		g_print("Deleted %s %s. This is recoverable.\n", args[1], args[2]);

	return 0;
}

static gint
venture_cli_command_restore(
	VentureCli	 *cli,
	gchar		**args,
	GError		**error
){
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *path = NULL;

	if ((NULL == args) || (NULL == args[1]) || (NULL == args[2]))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Usage: venturectl restore <type> <id>");
		return -1;
	}

	path = g_strdup_printf("/api/v1/%s/%s/restore", args[1], args[2]);
	node = venture_cli_request(cli, "POST", path, NULL, error);

	if (NULL == node)
		return -1;

	venture_cli_output(cli, node);

	return 0;
}

/*
 * Reads a secret from standard input.
 *
 * Not from argv, and there is no flag to put one there. A command line is
 * visible to every process on the host through /proc and lands in the
 * shell's history file; a secret that has been in either is a secret that
 * has to be rotated. Standard input goes to this process and nowhere else.
 *
 *   printf '%s' "$TOKEN" | venturectl forge set-token 1
 *   venturectl forge set-token 1 < token.txt
 *
 * A trailing newline is stripped, because every way of producing one of
 * these adds it and no forge token ends in whitespace.
 */
static gchar *
venture_cli_read_secret(GError **error)
{
	g_autoptr(GString) buffer = NULL;
	gchar chunk[1024];
	gsize got;

	buffer = g_string_new(NULL);

	while (0 < (got = fread(chunk, 1, sizeof(chunk), stdin)))
		g_string_append_len(buffer, chunk, (gssize)got);

	if (ferror(stdin))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED,
		                    "Could not read the secret from standard input");
		return NULL;
	}

	while ((buffer->len > 0) &&
	       (('\n' == buffer->str[buffer->len - 1]) ||
	        ('\r' == buffer->str[buffer->len - 1])))
		g_string_truncate(buffer, buffer->len - 1);

	return g_string_free(g_steal_pointer(&buffer), FALSE);
}

/*
 * venturectl forge set-token|set-secret|verify ID
 *
 * The one command group that is not generic over record types, and it earns
 * the exception: a credential is not a field with a flag on it. Each kind
 * has its own correct way of being set -- a password must be hashed, a
 * forge token must not be -- so a generic "write this sensitive field"
 * command would be a way to get one of them wrong.
 */
static gint
venture_cli_command_forge(
	VentureCli	 *cli,
	gchar		**args,
	GError		**error
){
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonNode) body = NULL;
	g_autofree gchar *secret = NULL;
	g_autofree gchar *path = NULL;
	const gchar *action;
	const gchar *id;

	action = (NULL != args[1]) ? args[1] : NULL;
	id = (NULL != args[1]) ? args[2] : NULL;

	if ((NULL == action) || (NULL == id))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Usage: venturectl forge set-token|set-secret|"
		                    "verify <id>\n"
		                    "       set-token and set-secret read the value "
		                    "from standard input");
		return -1;
	}

	if (0 == g_strcmp0(action, "verify"))
	{
		path = g_strdup_printf("/api/v1/forge/%s/verify", id);
		node = venture_cli_request(cli, "POST", path, NULL, error);

		if (NULL == node)
			return -1;

		venture_cli_output(cli, node);

		return 0;
	}

	if ((0 != g_strcmp0(action, "set-token")) &&
	    (0 != g_strcmp0(action, "set-secret")))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "\"%s\" is not a forge action. Try set-token, set-secret "
		            "or verify.", action);
		return -1;
	}

	secret = venture_cli_read_secret(error);

	if (NULL == secret)
		return -1;

	builder = json_builder_new();
	json_builder_begin_object(builder);

	if (0 == g_strcmp0(action, "set-token"))
	{
		if ('\0' == secret[0])
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			                    "Nothing arrived on standard input. Pipe the "
			                    "token in, or redirect a file.");
			return -1;
		}

		json_builder_set_member_name(builder, "token");
		json_builder_add_string_value(builder, secret);
		path = g_strdup_printf("/api/v1/forge/%s/token", id);
	}
	else
	{
		/* An empty secret is meaningful here: it asks the server to
		 * generate one, which it returns once. */
		json_builder_set_member_name(builder, "secret");
		json_builder_add_string_value(builder, secret);
		path = g_strdup_printf("/api/v1/forge/%s/webhook-secret", id);
	}

	json_builder_end_object(builder);
	body = json_builder_get_root(builder);

	node = venture_cli_request(cli, "POST", path, body, error);

	if (NULL == node)
		return -1;

	venture_cli_output(cli, node);

	return 0;
}

static gint
venture_cli_command_report(
	VentureCli	 *cli,
	gchar		**args,
	GError		**error
){
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(GString) path = NULL;

	if ((NULL == args) || (NULL == args[1]))
	{
		/* With no report named, list what there is rather than erroring:
		 * that is what the operator wanted to know. */
		node = venture_cli_request(cli, "GET", "/api/v1/reports", NULL, error);

		if (NULL == node)
			return -1;

		{
			JsonArray *reports;
			guint i;

			reports = json_node_get_array(node);

			for (i = 0; i < json_array_get_length(reports); i++)
			{
				JsonObject *report;

				report = json_array_get_object_element(reports, i);
				g_print("%-14s %s\n",
				        json_object_get_string_member(report, "name"),
				        json_object_get_string_member(report, "description"));
			}
		}

		return 0;
	}

	path = g_string_new("/api/v1/reports/");
	g_string_append_uri_escaped(path, args[1], NULL, FALSE);

	if (NULL != args[2])
	{
		g_string_append(path, "?period=");
		g_string_append_uri_escaped(path, args[2], NULL, FALSE);
	}

	/* The server already renders a report as CSV for the web export, so
	 * -f csv passes the body through rather than re-deriving it here and
	 * risking two renderings that disagree. */
	if (VENTURE_OUTPUT_FORMAT_CSV == cli->format)
	{
		g_autofree gchar *text = NULL;

		g_string_append(path, (NULL != args[2]) ? "&" : "?");
		g_string_append(path, "format=csv");

		text = venture_cli_request_text(cli, path->str, error);

		if (NULL == text)
			return -1;

		g_print("%s", text);

		return 0;
	}

	node = venture_cli_request(cli, "GET", path->str, NULL, error);

	if (NULL == node)
		return -1;

	{
		g_autofree gchar *text = NULL;

		text = venture_json_to_string(node, TRUE);
		g_print("%s\n", text);
	}

	return 0;
}

static gint
venture_cli_command_types(
	VentureCli	 *cli,
	gchar		**args,
	GError		**error
){
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *path = NULL;

	path = (NULL != args[1])
		? g_strdup_printf("/api/v1/schema/%s", args[1])
		: g_strdup("/api/v1/schema");

	node = venture_cli_request(cli, "GET", path, NULL, error);

	if (NULL == node)
		return -1;

	if (NULL != args[1])
	{
		JsonObject *description;
		JsonArray *fields;
		guint i;

		description = json_node_get_object(node);
		fields = json_object_get_array_member(description, "fields");

		g_print("%s (table %s)\n\n",
		        json_object_get_string_member(description, "name"),
		        json_object_get_string_member(description, "table"));

		for (i = 0; i < json_array_get_length(fields); i++)
		{
			JsonObject *field;
			g_autoptr(GString) detail = NULL;
			g_autofree gchar *wire = NULL;
			const gchar *name;
			const gchar *type;

			field = json_array_get_object_element(fields, i);
			name = json_object_get_string_member(field, "name");
			type = json_object_get_string_member(field, "type");

			/*
			 * Printed in the spelling you have to type.
			 *
			 * Properties are `forge-id` in C and `forge_id` on the
			 * wire, and this is the one place a person or an agent
			 * looks up a field name before using it. Showing the C
			 * spelling here sends them to write `forge-id=1`, which
			 * the server ignores field by field -- the record saves
			 * and the value simply is not there.
			 */
			wire = g_strdelimit(g_strdup(name), "-", '_');

			detail = g_string_new(NULL);

			if (json_object_get_boolean_member(field, "required"))
				g_string_append(detail, " required");

			/* What a reference points at. Without this the type says
			 * "reference" and leaves you to guess which table. */
			if (json_object_has_member(field, "references"))
			{
				g_string_append_printf(detail, " -> %s",
					json_object_get_string_member(field, "references"));
			}

			/* And what an enum will actually accept. */
			if (json_object_has_member(field, "choices"))
			{
				JsonArray *choices;
				guint c;

				choices = json_object_get_array_member(field, "choices");
				g_string_append(detail, " [");

				for (c = 0; c < json_array_get_length(choices); c++)
				{
					JsonObject *choice;

					choice = json_array_get_object_element(choices, c);

					if (c > 0)
						g_string_append_c(detail, '|');

					g_string_append(detail,
						json_object_get_string_member(choice, "value"));
				}

				g_string_append_c(detail, ']');
			}

			g_print("  %-26s %-10s%s\n", wire, type, detail->str);

			if (json_object_has_member(field, "help"))
			{
				const gchar *help;

				help = json_object_get_string_member(field, "help");

				if ((NULL != help) && ('\0' != help[0]))
					g_print("  %-26s %s\n", "", help);
			}
		}

		return 0;
	}

	{
		JsonArray *types;
		guint i;

		types = json_node_get_array(node);

		for (i = 0; i < json_array_get_length(types); i++)
		{
			JsonObject *type;

			type = json_array_get_object_element(types, i);
			g_print("%s\n", json_object_get_string_member(type, "name"));
		}
	}

	return 0;
}

/*
 * venturectl kb search|sync|reindex|export
 *
 * A verb family rather than a per-type subcommand: knowledge bases are
 * ordinary record types, so `list kb_article` and `create knowledge_base`
 * already work and nothing here duplicates them. What these four do is the
 * part that is not CRUD -- retrieval, and the three operations that keep an
 * index in step with the world.
 */
static gint
venture_cli_command_kb(
	VentureCli	 *cli,
	gchar		**args,
	GError		**error
){
	const gchar *action;

	action = (NULL != args[1]) ? args[1] : NULL;

	if (venture_string_is_empty(action))
	{
		g_printerr("Usage: venturectl kb search QUERY [--kb SLUG] "
		           "[--limit N]\n"
		           "       venturectl kb sync KB_ID\n"
		           "       venturectl kb reindex [KB_ID] [--force]\n"
		           "       venturectl kb export KB_ID [--format zip|tar.gz]"
		           "\n");
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "no kb action given");
		return -1;
	}

	if (0 == g_strcmp0(action, "search"))
	{
		g_autoptr(JsonNode) node = NULL;
		g_autofree gchar *path = NULL;
		g_autofree gchar *escaped = NULL;
		g_autoptr(GString) url = NULL;
		g_autofree gchar *text = NULL;
		const gchar *query;
		gsize i;

		query = args[2];

		if (venture_string_is_empty(query))
		{
			g_set_error_literal(error, VENTURE_ERROR,
			                    VENTURE_ERROR_INVALID_ARGUMENT,
			                    "Say what to search for");
			return -1;
		}

		escaped = g_uri_escape_string(query, NULL, TRUE);
		url = g_string_new("/api/v1/kb/search?q=");
		g_string_append(url, escaped);

		/*
		 * Flags are read off the tail rather than through GOption:
		 * the option parser has already run and consumed the global
		 * flags, and re-running it here would take --format, which
		 * means something else at this level.
		 */
		for (i = 3; NULL != args[i]; i++)
		{
			if ((0 == g_strcmp0(args[i], "--kb")) && (NULL != args[i + 1]))
			{
				g_autofree gchar *value = NULL;

				value = g_uri_escape_string(args[i + 1], NULL, TRUE);
				g_string_append_printf(url, "&kb=%s", value);
				i++;
			}
			else if ((0 == g_strcmp0(args[i], "--limit")) &&
			         (NULL != args[i + 1]))
			{
				g_string_append_printf(url, "&limit=%s", args[i + 1]);
				i++;
			}
		}

		node = venture_cli_request(cli, "GET", url->str, NULL, error);

		if (NULL == node)
			return -1;

		text = venture_json_to_string(node, TRUE);
		g_print("%s\n", text);

		return 0;
	}

	if ((0 == g_strcmp0(action, "sync")) ||
	    (0 == g_strcmp0(action, "reindex")))
	{
		g_autoptr(JsonNode) node = NULL;
		g_autoptr(GString) url = NULL;
		g_autofree gchar *text = NULL;
		const gchar *id;
		gsize i;

		id = (NULL != args[2]) ? args[2] : "0";

		if ((0 == g_strcmp0(action, "sync")) &&
		    (0 == g_strcmp0(id, "0")))
		{
			g_set_error_literal(error, VENTURE_ERROR,
			                    VENTURE_ERROR_INVALID_ARGUMENT,
			                    "Say which knowledge base to sync");
			return -1;
		}

		url = g_string_new(NULL);
		g_string_append_printf(url, "/api/v1/kb/%s/%s", id, action);

		for (i = 3; NULL != args[i]; i++)
		{
			if (0 == g_strcmp0(args[i], "--force"))
				g_string_append(url, "?force=true");
		}

		node = venture_cli_request(cli, "POST", url->str, NULL, error);

		if (NULL == node)
			return -1;

		text = venture_json_to_string(node, TRUE);
		g_print("%s\n", text);

		return 0;
	}

	if ((0 == g_strcmp0(action, "crossref")) ||
	    (0 == g_strcmp0(action, "article")))
	{
		g_autoptr(JsonNode) node = NULL;
		g_autoptr(JsonNode) body = NULL;
		g_autoptr(GString) url = NULL;
		g_autofree gchar *text = NULL;
		const gchar *type_name;
		const gchar *id;

		type_name = args[2];
		id = args[3];

		if (venture_string_is_empty(type_name) || venture_string_is_empty(id))
		{
			g_printerr("Usage: venturectl kb %s TYPE ID%s\n", action,
			           (0 == g_strcmp0(action, "article"))
			               ? " --kb KB_ID" : "");
			g_set_error_literal(error, VENTURE_ERROR,
			                    VENTURE_ERROR_INVALID_ARGUMENT,
			                    "say which record");
			return -1;
		}

		url = g_string_new(NULL);

		if (0 == g_strcmp0(action, "crossref"))
		{
			g_string_append_printf(url, "/api/v1/kb/crossref/%s/%s",
			                       type_name, id);
		}
		else
		{
			g_autoptr(JsonBuilder) builder = NULL;
			const gchar *kb_id = NULL;
			gsize i;

			for (i = 4; NULL != args[i]; i++)
			{
				if ((0 == g_strcmp0(args[i], "--kb")) &&
				    (NULL != args[i + 1]))
				{
					kb_id = args[i + 1];
					i++;
				}
			}

			if (venture_string_is_empty(kb_id))
			{
				g_set_error_literal(error, VENTURE_ERROR,
				                    VENTURE_ERROR_INVALID_ARGUMENT,
				                    "Say which knowledge base with --kb");
				return -1;
			}

			g_string_append_printf(url, "/api/v1/kb/from/%s/%s",
			                       type_name, id);

			builder = json_builder_new();
			json_builder_begin_object(builder);
			json_builder_set_member_name(builder, "kb_id");
			json_builder_add_int_value(builder,
				g_ascii_strtoll(kb_id, NULL, 10));
			json_builder_end_object(builder);
			body = json_builder_get_root(builder);
		}

		node = venture_cli_request(cli, "POST", url->str, body, error);

		if (NULL == node)
			return -1;

		text = venture_json_to_string(node, TRUE);
		g_print("%s\n", text);

		return 0;
	}

	if (0 == g_strcmp0(action, "export"))
	{
		g_autoptr(GBytes) archive = NULL;
		g_autoptr(GString) url = NULL;
		const gchar *format = "zip";
		const gchar *id;
		gconstpointer data;
		gsize size = 0;
		gsize i;

		id = args[2];

		if (venture_string_is_empty(id))
		{
			g_set_error_literal(error, VENTURE_ERROR,
			                    VENTURE_ERROR_INVALID_ARGUMENT,
			                    "Say which knowledge base to export");
			return -1;
		}

		for (i = 3; NULL != args[i]; i++)
		{
			if ((0 == g_strcmp0(args[i], "--format")) &&
			    (NULL != args[i + 1]))
			{
				format = args[i + 1];
				i++;
			}
		}

		url = g_string_new(NULL);
		g_string_append_printf(url, "/api/v1/kb/%s/export?format=%s", id,
		                       format);

		archive = venture_cli_request_bytes(cli, "GET", url->str, error);

		if (NULL == archive)
			return -1;

		/*
		 * Written to stdout so it can be redirected, which is what
		 * every other export in this CLI does. Writing a file here
		 * would mean inventing a name and a directory policy.
		 */
		data = g_bytes_get_data(archive, &size);

		if (size != fwrite(data, 1, size, stdout))
		{
			g_set_error_literal(error, VENTURE_ERROR,
			                    VENTURE_ERROR_FAILED,
			                    "Could not write the archive to stdout");
			return -1;
		}

		return 0;
	}

	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
	            "\"%s\" is not a kb action. Try search, sync, reindex, "
	            "export, crossref or article.", action);

	return -1;
}

static gint
venture_cli_command_health(
	VentureCli	 *cli,
	gchar		**args,
	GError		**error
){
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *text = NULL;

	node = venture_cli_request(cli, "GET", "/api/v1/health", NULL, error);

	if (NULL == node)
		return -1;

	text = venture_json_to_string(node, TRUE);
	g_print("%s\n", text);

	return 0;
}

/*
 * venturectl mcp
 *
 * A stdio MCP server, so an AI coding agent can drive VENTURE the way it
 * drives any other tool server: through an .mcp.json entry naming this
 * command. The tool surface is built at startup from GET /api/v1/schema, so
 * a record type registered by a plugin is offered without a line changing
 * here.
 *
 * Two things about this subcommand are deliberately unlike the others.
 *
 * It takes its credential from the environment -- VENTURE_TOKEN -- and
 * refuses --token outright. An MCP server is spawned by an agent from a
 * config file, and a credential written into that file's argv is visible in
 * `ps` to every account on the host; a process's environment is readable
 * only by its owner. --server is honoured, because a hostname is not a
 * secret; with none given the server reads VENTURE_URL, then VENTURE_SERVER.
 *
 * And it must not write anything to standard output except protocol
 * messages: stdout is the transport. Every diagnostic goes to stderr,
 * including GLib's, which by default does not.
 */
static gint
venture_cli_command_mcp(
	VentureCli	 *cli,
	gchar		**args,
	GError		**error
){
	g_autoptr(VentureMcpServer) server = NULL;

	/*
	 * Refused rather than ignored.
	 *
	 * `mcp` reads its token from the environment and would otherwise
	 * quietly disregard one passed as an argument -- and somebody who
	 * passed it would believe it had been used, having already exposed it
	 * in `ps` and in the shell history for nothing.
	 */
	if (cli->token_from_argv)
	{
		g_set_error_literal(error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT,
		                    "`venturectl mcp` does not take --token. An argv "
		                    "is world-readable through /proc and lands in "
		                    "the shell's history, and an MCP server is "
		                    "spawned from a config file that would then hold "
		                    "the credential. Set VENTURE_TOKEN in the "
		                    "environment instead.\n"
		                    "Rotate that token: it has already been exposed.");
		return -1;
	}

	/* --server is not a secret, so it is honoured. With none given the
	 * server reads VENTURE_URL, then VENTURE_SERVER. */
	server = venture_mcp_server_new(
		cli->server_from_argv ? cli->base_url : NULL, NULL, error);

	if (NULL == server)
		return -1;

	venture_mcp_server_set_stage_writes(server, !cli->apply_writes);

	if (!venture_mcp_server_load_catalog(server, error))
		return -1;

	if (!cli->quiet)
	{
		g_auto(GStrv) types = NULL;

		types = venture_mcp_catalog_list_types(
			venture_mcp_server_get_catalog(server));

		/* stderr, not stdout: stdout carries the protocol. */
		g_printerr("venturectl mcp: %u record types, writes %s\n",
		           g_strv_length(types),
		           cli->apply_writes ? "applied" : "staged");
	}

	if (!venture_mcp_server_run(server, error))
		return -1;

	return 0;
}

/* --- Entry point --------------------------------------------------------- */

int
main(
	int	  argc,
	char	**argv
){
	g_autoptr(GOptionContext) options = NULL;
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) args = NULL;
	VentureCli cli = { NULL, NULL, NULL, VENTURE_OUTPUT_FORMAT_TABLE, FALSE,
	                   FALSE, FALSE, FALSE, FALSE };
	g_autofree gchar *server = NULL;
	g_autofree gchar *token = NULL;
	g_autofree gchar *format = NULL;
	gboolean show_version = FALSE;
	gboolean show_license = FALSE;
	gboolean quiet = FALSE;
	gboolean apply_writes = FALSE;
	gboolean stage = FALSE;
	gint result;

	const GOptionEntry entries[] = {
		{ "server", 's', 0, G_OPTION_ARG_STRING, &server,
		  "Server base URL (default http://127.0.0.1:8747)", "URL" },
		{ "token", 't', 0, G_OPTION_ARG_STRING, &token,
		  "API token; also read from VENTURE_TOKEN", "TOKEN" },
		{ "format", 'f', 0, G_OPTION_ARG_STRING, &format,
		  "Output as table, json, yaml or csv", "FORMAT" },
		{ "quiet", 'q', 0, G_OPTION_ARG_NONE, &quiet,
		  "Print only the data", NULL },
		{ "apply-writes", 0, 0, G_OPTION_ARG_NONE, &apply_writes,
		  "mcp only: let write tools apply instead of staging", NULL },
		{ "stage", 0, 0, G_OPTION_ARG_NONE, &stage,
		  "create/update/delete only: propose the change for approval "
		  "instead of making it", NULL },
		{ "version", 'V', 0, G_OPTION_ARG_NONE, &show_version,
		  "Print the version and exit", NULL },
		{ "license", 0, 0, G_OPTION_ARG_NONE, &show_license,
		  "Print licensing information and exit", NULL },
		{ G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_STRING_ARRAY, &args,
		  NULL, NULL },
		{ NULL }
	};

	options = g_option_context_new("<command> [arguments] - talk to a VENTURE server");
	g_option_context_add_main_entries(options, entries, NULL);
	g_option_context_set_description(options,
		"Commands:\n"
		"  types [TYPE]                 list record types, or describe one\n"
		"  describe TYPE                same as `types TYPE`: fields, references,\n"
		"                               enum choices and what each one means\n"
		"  list TYPE [key=value ...]    list records, filtered\n"
		"  get TYPE ID                  fetch one record\n"
		"  create TYPE field=value ...  create a record\n"
		"  update TYPE ID field=value   change a record\n"
		"  delete TYPE ID               delete a record (recoverable)\n"
		"  restore TYPE ID              bring a deleted record back\n"
		"  forge set-token ID           set a forge's access token (stdin)\n"
		"  forge set-secret ID          set or generate its webhook secret\n"
		"  forge verify ID              record which account the token is\n"
		"  report [NAME] [PERIOD]       list reports, or run one\n"
		"  kb search QUERY              search the knowledge bases by\n"
		"                               meaning; --kb SLUG, --limit N\n"
		"  kb sync KB_ID                bring a base into line with its\n"
		"                               source directory on the server\n"
		"  kb reindex [KB_ID] [--force] re-embed articles that need it\n"
		"  kb export KB_ID              write an archive to stdout;\n"
		"                               --format zip|tar.gz\n"
		"  kb crossref TYPE ID          link the knowledge that bears\n"
		"                               on one record\n"
		"  kb article TYPE ID --kb N    write a KB article from a record\n"
		"  health                       check the server is up\n"
		"  mcp [--apply-writes]         serve the API to an AI agent over\n"
		"                               stdio as an MCP server\n"
		"\n"
		"Examples:\n"
		"  venturectl types sale\n"
		"  venturectl list sale period=this_month limit=10\n"
		"  venturectl list expense deductibility__eq=review\n"
		"  venturectl create expense description='Cover art' amount=250.00 \\\n"
		"                     venture_id=3 occurred_at=2026-03-14\n"
		"  venturectl update venture 3 status=paused\n"
		"  venturectl report pnl this_quarter\n"
		"  venturectl -f csv report receivables > aging.csv\n"
		"  printf '%s' \"$FORGE_TOKEN\" | venturectl forge set-token 1\n"
		"  venturectl -f json list sale | jq '.records[].gross.formatted'\n"
		"  VENTURE_TOKEN=... venturectl mcp        # stdio MCP server\n"
		"\n"
		"Filters use field__operator=value. Operators: eq ne lt lte gt gte\n"
		"like ilike in not_in is_null not_null between.\n"
		"\n"
		"Periods: today, yesterday, this_week, last_week, this_month,\n"
		"last_month, this_quarter, last_quarter, this_year, last_year,\n"
		"ytd, qtd, mtd, fy, fy_2026, last_30_days, 2026, 2026-03, 2026-Q2,\n"
		"2026-03-14, 2026-01-01..2026-03-31, all.\n"
		"\n"
		"Output defaults to a table on a terminal and JSON when piped.\n"
		"\n"
		"Exit codes: 2 usage, 3 not found, 4 conflict, 5 auth, 6 unsupported,\n"
		"7 network, 8 validation, 1 anything else.\n");

	if (!g_option_context_parse(options, &argc, &argv, &error))
	{
		g_printerr("%s\n", error->message);
		return venture_error_to_exit_code(VENTURE_ERROR_INVALID_ARGUMENT);
	}

	if (show_version)
	{
		g_print("venturectl %s\n", venture_get_version_string());
		return 0;
	}

	if (show_license)
	{
		g_print("VENTURE %s\n"
		        "Copyright (C) 2026 Zach Podbielniak\n\n"
		        "Licensed under the GNU Affero General Public License, "
		        "version 3 or later.\n"
		        "This is free software: you are free to change and "
		        "redistribute it.\n"
		        "There is NO WARRANTY, to the extent permitted by law.\n",
		        venture_get_version_string());
		return 0;
	}

	if ((NULL == args) || (NULL == args[0]))
	{
		g_autofree gchar *help = NULL;

		help = g_option_context_get_help(options, TRUE, NULL);
		g_print("%s", help);

		return venture_error_to_exit_code(VENTURE_ERROR_INVALID_ARGUMENT);
	}

	cli.base_url = (NULL != server)
		? g_strdup(server)
		: g_strdup((NULL != g_getenv("VENTURE_SERVER"))
			? g_getenv("VENTURE_SERVER") : "http://127.0.0.1:8747");

	/* Trailing slashes would produce double-slashed paths. */
	if (g_str_has_suffix(cli.base_url, "/"))
		cli.base_url[strlen(cli.base_url) - 1] = '\0';

	/* A token on the command line is visible in the process list, so the
	 * environment is the documented way and takes no flag. */
	cli.token = (NULL != token) ? g_strdup(token)
	                            : g_strdup(g_getenv("VENTURE_TOKEN"));
	cli.server_from_argv = (NULL != server);
	cli.token_from_argv = (NULL != token);
	cli.apply_writes = apply_writes;
	cli.stage = stage;

	/*
	 * Refused rather than ignored, for the same reason `mcp` refuses
	 * --token: a flag that quietly does nothing on nine of ten commands
	 * teaches that it did something.
	 */
	if (apply_writes && (0 != g_strcmp0(args[0], "mcp")))
	{
		g_printerr("venturectl: --apply-writes only means something to "
		           "`venturectl mcp`, which holds its writes by default. "
		           "\"%s\" applies them either way.\n", args[0]);
		g_free(cli.base_url);
		g_free(cli.token);
		return venture_error_to_exit_code(VENTURE_ERROR_INVALID_ARGUMENT);
	}

	/*
	 * Same rule, same reason. Only the three generic write verbs go
	 * through a route that reads `stage`; on anything else the parameter
	 * would be an unknown one, which a write route *ignores* -- so a
	 * quietly accepted --stage would apply the change it was asked to
	 * hold back.
	 */
	if (stage && (0 != g_strcmp0(args[0], "create")) &&
	    (0 != g_strcmp0(args[0], "update")) &&
	    (0 != g_strcmp0(args[0], "delete")))
	{
		g_printerr("venturectl: --stage only means something to create, "
		           "update and delete. \"%s\" would ignore it.\n", args[0]);
		g_free(cli.base_url);
		g_free(cli.token);
		return venture_error_to_exit_code(VENTURE_ERROR_INVALID_ARGUMENT);
	}

	cli.quiet = quiet;
	cli.format = venture_cli_default_format();

	if (NULL != format)
	{
		gint value;

		if (!venture_enum_from_nick(VENTURE_TYPE_OUTPUT_FORMAT, format,
		                            &value))
		{
			g_printerr("venturectl: \"%s\" is not an output format. "
			           "Use table, json, yaml or csv.\n", format);
			return venture_error_to_exit_code(VENTURE_ERROR_INVALID_ARGUMENT);
		}

		cli.format = (VentureOutputFormat)value;
	}

	cli.session = soup_session_new();

	if (0 == g_strcmp0(args[0], "list"))
		result = venture_cli_command_list(&cli, args, &error);
	else if (0 == g_strcmp0(args[0], "get"))
		result = venture_cli_command_get(&cli, args, &error);
	else if (0 == g_strcmp0(args[0], "create"))
		result = venture_cli_command_create(&cli, args, &error);
	else if (0 == g_strcmp0(args[0], "update"))
		result = venture_cli_command_update(&cli, args, &error);
	else if (0 == g_strcmp0(args[0], "delete"))
		result = venture_cli_command_delete(&cli, args, &error);
	else if (0 == g_strcmp0(args[0], "forge"))
		result = venture_cli_command_forge(&cli, args, &error);
	else if (0 == g_strcmp0(args[0], "restore"))
		result = venture_cli_command_restore(&cli, args, &error);
	else if (0 == g_strcmp0(args[0], "report"))
		result = venture_cli_command_report(&cli, args, &error);
	else if ((0 == g_strcmp0(args[0], "types")) ||
	         (0 == g_strcmp0(args[0], "schema")) ||
	         (0 == g_strcmp0(args[0], "describe")))
		result = venture_cli_command_types(&cli, args, &error);
	else if (0 == g_strcmp0(args[0], "kb"))
		result = venture_cli_command_kb(&cli, args, &error);
	else if (0 == g_strcmp0(args[0], "health"))
		result = venture_cli_command_health(&cli, args, &error);
	else if (0 == g_strcmp0(args[0], "mcp"))
		result = venture_cli_command_mcp(&cli, args, &error);
	else
	{
		g_printerr("venturectl: \"%s\" is not a command. Try --help.\n",
		           args[0]);
		result = -1;
		g_set_error_literal(&error, VENTURE_ERROR,
		                    VENTURE_ERROR_INVALID_ARGUMENT, "unknown command");
	}

	g_clear_object(&cli.session);
	g_free(cli.base_url);
	g_free(cli.token);

	if (0 != result)
	{
		if ((NULL != error) &&
		    (VENTURE_ERROR_INVALID_ARGUMENT != (VentureError)error->code))
			venture_cli_print_error(error);
		else if (NULL != error)
			venture_cli_print_error(error);

		return venture_error_to_exit_code(
			(NULL != error) ? (VentureError)error->code
			                : VENTURE_ERROR_FAILED);
	}

	return 0;
}
