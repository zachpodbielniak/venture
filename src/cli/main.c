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
 * Performs a request and decodes the response.
 *
 * A JSON error body is turned back into a real #GError with its original
 * code, so a failure deep in the server surfaces here with the same meaning
 * and the same exit status it would have had locally.
 */
static JsonNode *
venture_cli_request(
	VentureCli	 *cli,
	const gchar	 *method,
	const gchar	 *path,
	JsonNode	 *body,
	GError		**error
){
	g_autoptr(SoupMessage) message = NULL;
	g_autoptr(GBytes) response = NULL;
	g_autoptr(GError) local_error = NULL;
	g_autofree gchar *url = NULL;
	g_autofree gchar *text = NULL;
	JsonNode *node;
	guint status;

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

	status = soup_message_get_status(message);
	text = g_strndup(g_bytes_get_data(response, NULL),
	                 g_bytes_get_size(response));

	node = venture_json_parse(text, &local_error);

	if (NULL == node)
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_SERIALIZATION,
		            "The server returned something that is not JSON (HTTP %u)",
		            status);
		return NULL;
	}

	if (status >= 400)
	{
		VentureError code;
		JsonObject *object;
		const gchar *slug;
		const gchar *message_text;

		object = JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node)
		                                      : NULL;
		slug = (NULL != object)
			? venture_json_object_get_string(object, "error", NULL) : NULL;
		message_text = (NULL != object)
			? venture_json_object_get_string(object, "message", NULL) : NULL;

		if ((NULL == slug) || !venture_error_from_slug(slug, &code))
			code = VENTURE_ERROR_FAILED;

		g_set_error(error, VENTURE_ERROR, code, "%s",
		            (NULL != message_text) ? message_text
		                                   : "the request failed");

		json_node_unref(node);

		return NULL;
	}

	return node;
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

	/* A single record has nothing to tabulate, so it always prints in
	 * full. */
	{
		g_autofree gchar *text = NULL;

		text = venture_json_to_string(node, TRUE);
		g_print("%s\n", text);
	}

	return 0;
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
	path = g_strdup_printf("/api/v1/%s", args[1]);
	node = venture_cli_request(cli, "POST", path, values, error);

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
	path = g_strdup_printf("/api/v1/%s/%s", args[1], args[2]);
	node = venture_cli_request(cli, "PATCH", path, values, error);

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

	path = g_strdup_printf("/api/v1/%s/%s", args[1], args[2]);
	node = venture_cli_request(cli, "DELETE", path, NULL, error);

	if (NULL == node)
		return -1;

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

/* --- Entry point --------------------------------------------------------- */

int
main(
	int	  argc,
	char	**argv
){
	g_autoptr(GOptionContext) options = NULL;
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) args = NULL;
	VentureCli cli = { NULL, NULL, NULL, VENTURE_OUTPUT_FORMAT_TABLE, FALSE };
	g_autofree gchar *server = NULL;
	g_autofree gchar *token = NULL;
	g_autofree gchar *format = NULL;
	gboolean show_version = FALSE;
	gboolean show_license = FALSE;
	gboolean quiet = FALSE;
	gint result;

	const GOptionEntry entries[] = {
		{ "server", 's', 0, G_OPTION_ARG_STRING, &server,
		  "Server base URL (default http://127.0.0.1:8747)", "URL" },
		{ "token", 't', 0, G_OPTION_ARG_STRING, &token,
		  "API token; also read from VENTURE_TOKEN", "TOKEN" },
		{ "format", 'f', 0, G_OPTION_ARG_STRING, &format,
		  "Output as table, json or yaml", "FORMAT" },
		{ "quiet", 'q', 0, G_OPTION_ARG_NONE, &quiet,
		  "Print only the data", NULL },
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
		"  report [NAME] [PERIOD]       list reports, or run one\n"
		"  health                       check the server is up\n"
		"\n"
		"Examples:\n"
		"  venturectl types sale\n"
		"  venturectl list sale period=this_month limit=10\n"
		"  venturectl list expense deductibility__eq=review\n"
		"  venturectl create expense description='Cover art' amount=250.00 \\\n"
		"                     venture_id=3 occurred_at=2026-03-14\n"
		"  venturectl update venture 3 status=paused\n"
		"  venturectl report pnl this_quarter\n"
		"  venturectl -f json list sale | jq '.records[].gross.formatted'\n"
		"\n"
		"Filters use field__operator=value. Operators: eq ne lt lte gt gte\n"
		"like ilike in not_in is_null not_null between.\n"
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
	cli.quiet = quiet;
	cli.format = venture_cli_default_format();

	if (NULL != format)
	{
		gint value;

		if (!venture_enum_from_nick(VENTURE_TYPE_OUTPUT_FORMAT, format,
		                            &value))
		{
			g_printerr("venturectl: \"%s\" is not an output format. "
			           "Use table, json or yaml.\n", format);
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
	else if (0 == g_strcmp0(args[0], "restore"))
		result = venture_cli_command_restore(&cli, args, &error);
	else if (0 == g_strcmp0(args[0], "report"))
		result = venture_cli_command_report(&cli, args, &error);
	else if ((0 == g_strcmp0(args[0], "types")) ||
	         (0 == g_strcmp0(args[0], "schema")) ||
	         (0 == g_strcmp0(args[0], "describe")))
		result = venture_cli_command_types(&cli, args, &error);
	else if (0 == g_strcmp0(args[0], "health"))
		result = venture_cli_command_health(&cli, args, &error);
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
