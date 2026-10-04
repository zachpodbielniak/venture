/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * venture-marketdata-cli.inc - venturectl market quote|promote
 *
 * Thin calls on /api/v1/market. The records need no verbs of their own
 * -- `list instrument`, `create watchlist_entry ...` -- these are what is
 * not a record: asking the price oracle, and promoting a store's venue or
 * instrument into a record. Included by src/cli/main.c, which builds
 * without the server.
 */

/* The query-string names `market quote` passes through; `venue` is the
 * friendlier spelling of `where`, and `product` of `product_id`. */
static const gchar *const venture_cli_market_quote_keys[] = {
	"basis", "where", "venue", "venue_id", "product", "product_id", "at",
	"currency", "prefer_currency", "fallback", "source", NULL
};

static gint
venture_cli_command_market(
	VentureCli	 *cli,
	gchar		**args,
	GError		**error
){
	g_autoptr(JsonNode) response = NULL;

	if ((NULL != args[1]) && (0 == g_strcmp0(args[1], "promote")))
	{
		g_autoptr(JsonBuilder) builder = NULL;
		g_autoptr(JsonNode) body = NULL;
		gint64 source_id;

		if ((NULL == args[2]) || (NULL == args[3]) || (NULL == args[4]) ||
		    !g_ascii_string_to_signed(args[2], 10, 1, G_MAXINT64, &source_id, NULL) ||
		    ((0 != g_strcmp0(args[3], "instrument")) && (0 != g_strcmp0(args[3], "venue"))))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			                    "usage: venturectl market promote SOURCE_ID instrument|venue KEY");
			return -1;
		}

		builder = json_builder_new();
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "data_source_id");
		json_builder_add_int_value(builder, source_id);
		json_builder_set_member_name(builder, "kind");
		json_builder_add_string_value(builder, args[3]);
		json_builder_set_member_name(builder, "key");
		json_builder_add_string_value(builder, args[4]);
		json_builder_end_object(builder);
		body = json_builder_get_root(builder);

		response = venture_cli_request(cli, "POST", "/api/v1/market/promote", body, error);

		if (NULL == response)
			return -1;

		venture_cli_output(cli, response);
		return 0;
	}

	if ((NULL == args[1]) || (0 != g_strcmp0(args[1], "quote")))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "usage: venturectl market quote INSTRUMENT_ID|product=ID "
		                    "[basis=B] [venue=KEY|GROUP] [venue_id=N] [at=DATE] "
		                    "[currency=C] [fallback=true] [source=S] | "
		                    "market promote SOURCE_ID instrument|venue KEY");
		return -1;
	}

	{
		g_autoptr(GString) path = g_string_new("/api/v1/market/quote");
		gchar separator = '?';
		gint64 id;
		guint i;

		for (i = 2; NULL != args[i]; i++)
		{
			const gchar *equals = strchr(args[i], '=');
			g_autofree gchar *name = NULL;
			g_autofree gchar *escaped = NULL;

			if ((NULL == equals) &&
			    g_ascii_string_to_signed(args[i], 10, 1, G_MAXINT64, &id, NULL))
			{
				g_string_append_printf(path, "%cinstrument_id=%" G_GINT64_FORMAT, separator, id);
				separator = '&';
				continue;
			}

			name = (NULL != equals) ? g_strndup(args[i], (gsize)(equals - args[i])) : NULL;

			if ((NULL == name) || !g_strv_contains(venture_cli_market_quote_keys, name))
			{
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				            "market quote does not take \"%s\"; it takes an instrument id and "
				            "basis, venue, venue_id, product, at, currency, prefer_currency, "
				            "fallback and source", args[i]);
				return -1;
			}

			escaped = g_uri_escape_string(equals + 1, NULL, FALSE);
			g_string_append_printf(path, "%c%s=%s", separator,
			                       (0 == g_strcmp0(name, "venue")) ? "where"
			                       : (0 == g_strcmp0(name, "product")) ? "product_id" : name,
			                       escaped);
			separator = '&';
		}

		response = venture_cli_request(cli, "GET", path->str, NULL, error);

		if (NULL == response)
			return -1;

		venture_cli_output(cli, response);
		return 0;
	}
}
