/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * venture-arbitrage-cli.inc - venturectl arbitrage scan|record|crafting|
 * plan|calc|export|close|reopen|abandon|execute|registries
 *
 * Thin calls on /api/v1/arbitrage and on the arbitrage actions. The scan,
 * the export and the calculators ask exactly what /arbitrage and
 * /arbitrage/calc ask; recording an opportunity posts the same question
 * plus the row's key, and the server re-runs it and performs the `record`
 * action with what it finds -- so a trade is planned from the prices as
 * they are, never from what a terminal showed a minute ago.
 *
 * close, reopen, abandon, execute and a record with explicit legs are the
 * generic action verb under a friendlier name: they go through
 * venture_cli_command_act(), which types every argument from the action's
 * own schema. A hand-written verb that sent `key=value` as text would be
 * refused for every integer and boolean parameter -- `dunning sweep
 * organization_id=1` was, for as long as it was documented.
 *
 * Included by src/cli/main.c after the act verb and the market helpers,
 * which builds without the server: nothing here needs SQLite.
 */

/* Scan options that are whole numbers. The rest -- money ("10.00 GOLD"),
 * percents ("15") and names -- travel as the text they are and are read by
 * the server, which refuses an unknown option by name. A strategy a plugin
 * registers may add options of its own, so there is no allowlist here. */
static const gchar *const venture_cli_arbitrage_int_options[] = {
	"preset_id", "data_source_id", "recipe_id", "recipe_category_id", "units", "buy_sources", "max_age_hours", "top",
	"organization_id", NULL
};

/*
 * Names the scan's routes take but quietly drop or ignore: the page's own
 * (`key`, `format`) and those a report adds (`venture_id`, `as_of`).
 * Accepting one would answer a different question than the one typed.
 * `organization_id` is not among them: the routes read it (the
 * organization the scan answers for, else the active one), so it travels
 * with the rest of the question.
 */
static const gchar *const venture_cli_arbitrage_refused_options[] = {
	"key", "format", "venture_id", "as_of", "calc", NULL
};

static const gchar venture_cli_arbitrage_usage[] =
	"usage: venturectl arbitrage VERB ...\n"
	"  scan [STRATEGY] [option=value ...]   opportunities now (spread when no strategy)\n"
	"  record ROW|KEY [STRATEGY] [option=value ...]\n"
	"                                       record a scan row (1-based, same options) or key\n"
	"  record name=NAME legs=JSON|@FILE [strategy=S] [venture_id=N] [expected=JSON|@FILE]\n"
	"         [notes=TEXT] [organization_id=N]\n"
	"                                       record a trade from explicit legs\n"
	"  crafting [option=value ...]          every recipe at the picked realms: recipe_category_id,\n"
	"                                       buy_realm, sell_realm, venue_group, units, sell_basis,\n"
	"                                       max_age_hours, share, recipe_id, data_source_id,\n"
	"                                       character, profession, expansion, only_known,\n"
	"                                       min_profit, min_margin, max_cost, min_sold_per_day,\n"
	"                                       recipe_list (a recipe_list's id, or several, comma\n"
	"                                       separated)\n"
	"  plan [reprice=1]                     the flip planner: totals and the planned trades\n"
	"  plan export csv|FORMAT [-o FILE]     the shopping list as CSV, or a plugin's format (tsm)\n"
	"  plan remove TRADE_ID                 take a planned trade off the plan\n"
	"  calc surebet ODDS... --stake AMOUNT [format=auto|decimal|american|fractional]\n"
	"  calc back-lay BACK LAY --stake AMOUNT [commission=PCT] [back_commission=PCT] [format=F]\n"
	"  calc flip BUY SELL [units=N] [cut=PCT] [fixed=AMOUNT] [deposit=PCT] [refundable=true]\n"
	"       [sale_rate=PCT] [sold_per_day=N] [share=PCT] [transfer=AMOUNT] [transit_hours=N]\n"
	"  export FORMAT [STRATEGY] [option=value ...] [-o FILE]\n"
	"  close TRADE_ID [closed_at=TIME]\n"
	"  reopen TRADE_ID\n"
	"  abandon TRADE_ID [goods=keep|write_off] [abandoned_at=TIME]\n"
	"  execute LEG_ID [occurred_at=TIME]\n"
	"  registries                           strategies, fee models, export formats, options\n"
	"Scan options: preset_id, data_source_id, buy_venues, sell_venues, venue_group\n"
	"(characters or a venue group's id), group_key, category_path, kind,\n"
	"instrument, recipe_id, recipe_category_id, units, buy_sources, sell_basis,\n"
	"total_stake, min_profit, min_roi, min_sale_rate, max_capital, max_buy_pct,\n"
	"min_confidence, max_age_hours, share, sort, top (and a plugin strategy's own).\n"
	"organization_id=N on scan, record and export answers for that organization\n"
	"(default: the active one, which for a token is the default organization).\n"
	"Money names its currency (\"10.00 GOLD\"); percents are plain numbers (15 = 15%).\n"
	"record, close, reopen, abandon and execute take --stage.\n"
	"Examples:\n"
	"  venturectl arbitrage scan spread group_key=eu min_profit=\"10.00 GOLD\" min_roi=15 top=20\n"
	"  venturectl arbitrage record 1 spread group_key=eu min_profit=\"10.00 GOLD\" min_roi=15 top=20\n"
	"  venturectl --stage arbitrage record name=\"Peacebloom flip\" strategy=spread legs=@legs.json\n"
	"  venturectl arbitrage calc surebet 2.10 2.05 --stake \"100.00 USD\"\n"
	"  venturectl arbitrage calc surebet odds=\"+150 -120\" format=american --stake \"100.00 USD\"\n"
	"  venturectl arbitrage calc back-lay 3.0 2.9 --stake \"50.00 USD\" commission=5\n"
	"  venturectl arbitrage calc flip \"1.25 GOLD\" \"2.10 GOLD\" units=20 cut=5 sale_rate=40\n"
	"  venturectl arbitrage export csv spread group_key=eu -o opportunities.csv\n"
	"  venturectl arbitrage scan spread organization_id=2 min_roi=15\n"
	"  venturectl arbitrage crafting buy_realm=Thrall sell_realm=Thrall recipe_category_id=12 units=5\n"
	"  venturectl arbitrage crafting recipe_list=2 min_profit=50\n"
	"  venturectl arbitrage plan reprice=1\n"
	"  venturectl arbitrage plan export tsm -o shopping.txt\n"
	"  venturectl arbitrage execute 12 occurred_at=2026-03-05T10:00:00Z\n"
	"  venturectl arbitrage close 4\n"
	"  venturectl arbitrage abandon 4 goods=write_off";

static const VentureCliColumn venture_cli_arbitrage_scan_columns[] = {
	{ "", "#", VENTURE_CLI_CELL_INDEX, FALSE },
	{ "title", "opportunity", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "buy.venue_key", "buy at", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "sell.venue_key", "sell at", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "buy.stale", "buy stale", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "sell.stale", "sell stale", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "units", "units", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "net", "net", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "capital", "capital", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "roi", "roi", VENTURE_CLI_CELL_FRACTION, FALSE },
	{ "confidence", "conf", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "missing", "unquoted", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "key", "key", VENTURE_CLI_CELL_TEXT, TRUE },
	{ NULL, NULL, 0, FALSE }
};

static const VentureCliColumn venture_cli_arbitrage_crafting_columns[] = {
	{ "", "#", VENTURE_CLI_CELL_INDEX, FALSE },
	{ "title", "recipe", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "recipe_category", "category", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "cost", "reagents", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "sell.venue_key", "sell at", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "sell.unit_price", "sells for", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "sell.fees", "cut", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "sell.deposit", "deposit", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "net", "profit", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "margin", "margin", VENTURE_CLI_CELL_FRACTION, FALSE },
	{ "buy_stale", "buy stale", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "sell_stale", "sell stale", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "missing", "unquoted", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "key", "key", VENTURE_CLI_CELL_TEXT, TRUE },
	{ NULL, NULL, 0, FALSE }
};

static const VentureCliColumn venture_cli_arbitrage_plan_total_columns[] = {
	{ "currency", "currency", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "trades", "trades", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "outlay", "outlay", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "gross", "sales", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "cut", "cut", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "deposit", "deposits", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "listing_loss", "lost deposits", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "revenue", "revenue", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "profit", "profit", VENTURE_CLI_CELL_MONEY, FALSE },
	{ NULL, NULL, 0, FALSE }
};

static const VentureCliColumn venture_cli_arbitrage_plan_trade_columns[] = {
	{ "id", "trade", VENTURE_CLI_CELL_NUMBER, TRUE },
	{ "name", "name", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "strategy", "strategy", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "reprice.state", "standing", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "reprice.planned_net", "planned", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "reprice.net", "now", VENTURE_CLI_CELL_MONEY, FALSE },
	{ NULL, NULL, 0, FALSE }
};

static const VentureCliColumn venture_cli_arbitrage_strategy_columns[] = {
	{ "name", "strategy", VENTURE_CLI_CELL_TEXT, TRUE },
	{ "label", "label", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "description", "description", VENTURE_CLI_CELL_TEXT, FALSE },
	{ NULL, NULL, 0, FALSE }
};

static const VentureCliColumn venture_cli_arbitrage_fee_columns[] = {
	{ "name", "fee model", VENTURE_CLI_CELL_TEXT, TRUE },
	{ "description", "description", VENTURE_CLI_CELL_TEXT, FALSE },
	{ NULL, NULL, 0, FALSE }
};

static const VentureCliColumn venture_cli_arbitrage_format_columns[] = {
	{ "name", "export format", VENTURE_CLI_CELL_TEXT, TRUE },
	{ "label", "label", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "content_type", "type", VENTURE_CLI_CELL_TEXT, FALSE },
	{ NULL, NULL, 0, FALSE }
};

static const VentureCliColumn venture_cli_arbitrage_surebet_figures[] = {
	{ "is_surebet", "Surebet", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "sum", "Sum of 1/odds", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "overround", "Overround", VENTURE_CLI_CELL_FRACTION, FALSE },
	{ "total", "Total stake", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "staked", "Staked", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "residual", "Rounding left over", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "payout", "Payout", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "profit", "Profit", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "payout_ideal", "Ideal payout", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "profit_ideal", "Ideal profit", VENTURE_CLI_CELL_MONEY, FALSE },
	{ NULL, NULL, 0, FALSE }
};

static const VentureCliColumn venture_cli_arbitrage_outcome_columns[] = {
	{ "", "#", VENTURE_CLI_CELL_INDEX, FALSE },
	{ "odds", "odds", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "no_vig", "fair chance", VENTURE_CLI_CELL_FRACTION, FALSE },
	{ "stake", "stake", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "payout", "pays", VENTURE_CLI_CELL_MONEY, FALSE },
	{ NULL, NULL, 0, FALSE }
};

static const VentureCliColumn venture_cli_arbitrage_back_lay_figures[] = {
	{ "back_stake", "Back stake", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "lay_stake", "Lay stake", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "liability", "Liability", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "if_back_wins", "If the back wins", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "if_lay_wins", "If the lay wins", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "worst", "Sure result (worst)", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "ideal", "Ideal", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "rating", "Rating", VENTURE_CLI_CELL_FRACTION, FALSE },
	{ NULL, NULL, 0, FALSE }
};

static const VentureCliColumn venture_cli_arbitrage_flip_figures[] = {
	{ "buy_cost", "Buy cost", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "gross", "Gross sale", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "sell_fees", "Sell fees", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "deposit", "Deposit", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "transfer", "Transfer", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "net", "Net", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "capital", "Capital", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "roi", "ROI", VENTURE_CLI_CELL_FRACTION, FALSE },
	{ "roi_per_day", "ROI a day", VENTURE_CLI_CELL_FRACTION, FALSE },
	{ "annualized", "Annualised", VENTURE_CLI_CELL_FRACTION, FALSE },
	{ "listing_loss", "Listing loss", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "ev", "Expected value", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "lock_days", "Days tied up", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ NULL, NULL, 0, FALSE }
};

/* The calculators' fields, typed; the server reads the money and the
 * percents, which only it can read in the right currency. */
static const VentureCliOption venture_cli_arbitrage_surebet_options[] = {
	{ "odds", "odds", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "format", "format", VENTURE_CLI_OPTION_TEXT, "auto|decimal|american|fractional" },
	{ "stake", "stake", VENTURE_CLI_OPTION_TEXT, NULL },
	{ NULL, NULL, 0, NULL }
};

static const VentureCliOption venture_cli_arbitrage_back_lay_options[] = {
	{ "back_odds", "back_odds", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "lay_odds", "lay_odds", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "format", "format", VENTURE_CLI_OPTION_TEXT, "auto|decimal|american|fractional" },
	{ "commission", "commission", VENTURE_CLI_OPTION_NUMBER, NULL },
	{ "back_commission", "back_commission", VENTURE_CLI_OPTION_NUMBER, NULL },
	{ "stake", "stake", VENTURE_CLI_OPTION_TEXT, NULL },
	{ NULL, NULL, 0, NULL }
};

static const VentureCliOption venture_cli_arbitrage_flip_options[] = {
	{ "buy", "buy", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "sell", "sell", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "units", "units", VENTURE_CLI_OPTION_INT, NULL },
	{ "cut", "cut", VENTURE_CLI_OPTION_NUMBER, NULL },
	{ "fixed", "fixed", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "deposit", "deposit", VENTURE_CLI_OPTION_NUMBER, NULL },
	{ "refundable", "refundable", VENTURE_CLI_OPTION_TEXT, "true|false" },
	{ "sale_rate", "sale_rate", VENTURE_CLI_OPTION_NUMBER, NULL },
	{ "sold_per_day", "sold_per_day", VENTURE_CLI_OPTION_NUMBER, NULL },
	{ "share", "share", VENTURE_CLI_OPTION_NUMBER, NULL },
	{ "transfer", "transfer", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "transit_hours", "transit_hours", VENTURE_CLI_OPTION_NUMBER, NULL },
	{ NULL, NULL, 0, NULL }
};

/**
 * venture_cli_arbitrage_stages:
 * @verb: (nullable): the word after `arbitrage`
 *
 * Whether `arbitrage @verb` writes through a route that honours `stage`:
 * main() refuses --stage on anything else, because a write route ignores
 * a parameter it does not read and would apply the change it was asked
 * to hold back.
 *
 * Returns: %TRUE for record, close, reopen, abandon and execute
 */
static gboolean
venture_cli_arbitrage_stages(const gchar *verb)
{
	return (0 == g_strcmp0(verb, "record")) || (0 == g_strcmp0(verb, "close")) ||
	       (0 == g_strcmp0(verb, "reopen")) || (0 == g_strcmp0(verb, "abandon")) ||
	       (0 == g_strcmp0(verb, "execute"));
}

/*
 * Reads the scan's `name=value` options from @args into @options (JSON
 * strings, as a form would send them). A bare word at @strategy_at is the
 * strategy, the way `scan spread ...` reads.
 */
static gboolean
venture_cli_arbitrage_options(
	gchar		**args,
	guint		  first,
	gboolean	  strategy_allowed,
	const gchar	 *verb,
	JsonObject	 *options,
	GError		**error
){
	guint i;

	for (i = first; NULL != args[i]; i++)
	{
		g_autofree gchar *name = NULL;
		const gchar *equals;
		const gchar *value;

		equals = strchr(args[i], '=');

		if (NULL == equals)
		{
			if (strategy_allowed && (i == first) && !json_object_has_member(options, "strategy"))
			{
				json_object_set_string_member(options, "strategy", args[i]);
				continue;
			}

			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "%s takes option=value after the strategy; \"%s\" is not one", verb, args[i]);
			return FALSE;
		}

		name = g_strndup(args[i], (gsize)(equals - args[i]));
		value = equals + 1;

		if (g_strv_contains(venture_cli_arbitrage_refused_options, name))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "%s does not take \"%s\": the scan reads neither a venture nor a time, "
			            "and key and format are named by the verb", verb, name);
			return FALSE;
		}

		if (json_object_has_member(options, name))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "%s: %s is given twice", verb, name);
			return FALSE;
		}

		if (g_strv_contains(venture_cli_arbitrage_int_options, name))
		{
			gint64 parsed;

			if (!g_ascii_string_to_signed(value, 10, 0, G_MAXINT64, &parsed, NULL))
			{
				g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				            "%s is a whole number, not \"%s\"", name, value);
				return FALSE;
			}
		}

		json_object_set_string_member(options, name, value);
	}

	return TRUE;
}

/* @options as a query string appended to @path. */
static void
venture_cli_arbitrage_query(
	GString		*path,
	JsonObject	*options
){
	g_autoptr(GList) members = json_object_get_members(options);
	GList *member;

	for (member = members; NULL != member; member = member->next)
	{
		JsonNode *node = json_object_get_member(options, member->data);
		g_autofree gchar *text = NULL;

		text = (JSON_NODE_HOLDS_VALUE(node) && (G_TYPE_STRING == json_node_get_value_type(node)))
			? g_strdup(json_node_get_string(node)) : venture_json_to_string(node, FALSE);

		g_string_append_c(path, (NULL == strchr(path->str, '?')) ? '?' : '&');
		g_string_append_uri_escaped(path, member->data, NULL, FALSE);
		g_string_append_c(path, '=');
		g_string_append_uri_escaped(path, text, NULL, FALSE);
	}
}

/* GET the scan for @options. */
static JsonNode *
venture_cli_arbitrage_run_scan(
	VentureCli	 *cli,
	JsonObject	 *options,
	GError		**error
){
	g_autoptr(GString) path = g_string_new("/api/v1/arbitrage/scan");

	venture_cli_arbitrage_query(path, options);

	return venture_cli_request(cli, "GET", path->str, NULL, error);
}

/* Reads `@FILE` into the file's contents; anything else is kept. */
static gboolean
venture_cli_arbitrage_file_value(
	const gchar	 *arg,
	gchar		**out_arg,
	GError		**error
){
	const gchar *equals = strchr(arg, '=');
	g_autofree gchar *contents = NULL;
	g_autofree gchar *name = NULL;

	if ((NULL == equals) || ('@' != equals[1]))
	{
		*out_arg = g_strdup(arg);
		return TRUE;
	}

	/* A file error's code is not a VentureError: passed on as it is, a
	 * missing file would leave with the exit code for a conflict. */
	{
		g_autoptr(GError) local_error = NULL;

		if (!g_file_get_contents(equals + 2, &contents, NULL, &local_error))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "%s", local_error->message);
			return FALSE;
		}
	}

	name = g_strndup(arg, (gsize)(equals - arg));
	*out_arg = g_strconcat(name, "=", g_strstrip(contents), NULL);

	return TRUE;
}

/*
 * Hands `arbitrage VERB ID ...` to the act verb as `act TYPE ID ACTION
 * ...`, which reads the action's schema, types each argument from it and
 * honours --stage only when the action is stageable. An `@FILE` value is
 * read first, so legs can live in a file.
 */
static gint
venture_cli_arbitrage_act(
	VentureCli	 *cli,
	const gchar	 *type,
	const gchar	 *id,
	const gchar	 *action,
	gchar		**args,
	guint		  first,
	GError		**error
){
	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
	guint i;

	g_ptr_array_add(argv, g_strdup("act"));
	g_ptr_array_add(argv, g_strdup(type));
	g_ptr_array_add(argv, g_strdup(id));
	g_ptr_array_add(argv, g_strdup(action));

	for (i = first; NULL != args[i]; i++)
	{
		gchar *expanded = NULL;

		if (NULL == strchr(args[i], '='))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "arbitrage %s takes name=value parameters; \"%s\" is not one", action, args[i]);
			return -1;
		}

		if (!venture_cli_arbitrage_file_value(args[i], &expanded, error))
			return -1;

		g_ptr_array_add(argv, expanded);
	}

	g_ptr_array_add(argv, NULL);

	return venture_cli_command_act(cli, (gchar **)argv->pdata, error);
}

/* Prints what the record route answered: the trade, or the confirmation. */
static void
venture_cli_arbitrage_print_trade(
	VentureCli	*cli,
	JsonNode	*response
){
	JsonObject *trade;

	if (VENTURE_OUTPUT_FORMAT_TABLE != cli->format)
	{
		venture_cli_output(cli, response);
		return;
	}

	if (venture_cli_report_staged(cli, response) || cli->quiet || !JSON_NODE_HOLDS_OBJECT(response))
		return;

	trade = json_node_get_object(response);
	g_print("Recorded trade #%" G_GINT64_FORMAT ": %s (%s)\n",
	        json_object_get_int_member_with_default(trade, "id", 0),
	        venture_json_object_get_string(trade, "name", ""),
	        venture_json_object_get_string(trade, "status", ""));
}

/*
 * arbitrage record ROW|KEY ...: the opportunity a scan row or a key names,
 * planned by the server from the prices as they are now.
 */
static gint
venture_cli_arbitrage_record(
	VentureCli	 *cli,
	gchar		**args,
	GError		**error
){
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(JsonNode) body = NULL;
	g_autoptr(JsonNode) response = NULL;
	g_autofree gchar *key = NULL;
	gint64 row = 0;
	guint i;

	/* Explicit legs: the record action itself, typed from its schema. */
	for (i = 2; NULL != args[i]; i++)
		if (g_str_has_prefix(args[i], "legs="))
			return venture_cli_arbitrage_act(cli, "arbitrage_trade", "0", "record", args, 2, error);

	if ((NULL == args[2]) || (NULL != strchr(args[2], '=')) ||
	    (!g_ascii_string_to_signed(args[2], 10, 1, G_MAXINT64, &row, NULL) &&
	     (NULL == strchr(args[2], ':'))))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "usage: venturectl arbitrage record ROW|KEY [STRATEGY] [option=value ...] | "
		                    "arbitrage record name=NAME legs=JSON|@FILE [...]");
		return -1;
	}

	if (!venture_cli_arbitrage_options(args, 3, TRUE, "arbitrage record", options, error))
		return -1;

	if (row > 0)
	{
		g_autoptr(JsonNode) scan = NULL;
		JsonArray *rows = NULL;
		JsonObject *found;
		JsonNode *narrow;

		/* The same question as the scan the row came from, so the row
		 * numbers agree; its key and what narrows the plan to it are then
		 * sent exactly as the page's Record button sends them. */
		scan = venture_cli_arbitrage_run_scan(cli, options, error);

		if (NULL == scan)
			return -1;

		if (JSON_NODE_HOLDS_OBJECT(scan) && json_object_has_member(json_node_get_object(scan), "rows"))
			rows = json_object_get_array_member(json_node_get_object(scan), "rows");

		if ((NULL == rows) || ((guint64)row > json_array_get_length(rows)))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
			            "The scan has %u rows now, so there is no row %" G_GINT64_FORMAT
			            "; scan again with the same options",
			            (NULL != rows) ? json_array_get_length(rows) : 0, row);
			return -1;
		}

		found = json_array_get_object_element(rows, (guint)(row - 1));
		key = g_strdup(venture_json_object_get_string(found, "key", NULL));
		narrow = json_object_get_member(found, "narrow");

		if ((NULL != narrow) && JSON_NODE_HOLDS_OBJECT(narrow))
		{
			g_autoptr(GList) members = json_object_get_members(json_node_get_object(narrow));
			GList *member;

			/* The row's narrowing wins, as the page's Record button sends
			 * it (arw_hidden_options() lets narrow override): the record
			 * re-runs the question narrowed to that row, and a wider
			 * option left in place plans against a different question. */
			for (member = members; NULL != member; member = member->next)
				json_object_set_member(options, member->data,
					json_node_copy(json_object_get_member(json_node_get_object(narrow),
					                                      member->data)));
		}
	}
	else
	{
		key = g_strdup(args[2]);
	}

	json_object_set_string_member(options, "key", key);
	body = json_node_new(JSON_NODE_OBJECT);
	json_node_set_object(body, options);

	{
		g_autofree gchar *path = venture_cli_write_path(cli, "/api/v1/arbitrage/record");

		response = venture_cli_request(cli, "POST", path, body, error);
	}

	if (NULL == response)
		return -1;

	venture_cli_arbitrage_print_trade(cli, response);
	return 0;
}

/* arbitrage calc surebet|back-lay|flip ...: the calculators, on the server. */
static gint
venture_cli_arbitrage_calc(
	VentureCli	 *cli,
	gchar		**args,
	const gchar	 *stake,
	GError		**error
){
	g_autoptr(GString) path = NULL;
	g_autoptr(GPtrArray) positional = g_ptr_array_new();
	g_autoptr(JsonNode) response = NULL;
	const VentureCliOption *options;
	const gchar *calculator;
	const gchar *verb;
	gboolean stake_given = FALSE;
	guint i;

	verb = args[2];

	if ((0 == g_strcmp0(verb, "back-lay")) || (0 == g_strcmp0(verb, "back_lay")))
	{
		calculator = "back_lay";
		options = venture_cli_arbitrage_back_lay_options;
	}
	else if (0 == g_strcmp0(verb, "surebet"))
	{
		calculator = "surebet";
		options = venture_cli_arbitrage_surebet_options;
	}
	else if (0 == g_strcmp0(verb, "flip"))
	{
		calculator = "flip";
		options = venture_cli_arbitrage_flip_options;
	}
	else
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "usage: venturectl arbitrage calc surebet|back-lay|flip ... "
		                    "(venturectl arbitrage help shows each)");
		return -1;
	}

	path = g_string_new("/api/v1/arbitrage/calc?calc=");
	g_string_append(path, calculator);

	for (i = 3; NULL != args[i]; i++)
	{
		g_autofree gchar *value = NULL;
		const gchar *wire = NULL;

		if (NULL == strchr(args[i], '='))
		{
			g_ptr_array_add(positional, args[i]);
			continue;
		}

		if (!venture_cli_option_read("arbitrage calc", args[i], options, &wire, &value, error))
			return -1;

		if (0 == g_strcmp0(wire, "stake"))
			stake_given = TRUE;

		if (NULL == value)
			continue;

		g_string_append_printf(path, "&%s=", wire);
		g_string_append_uri_escaped(path, value, NULL, FALSE);
	}

	if (NULL != stake)
	{
		if (stake_given)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			                    "Give the stake once: --stake or stake=");
			return -1;
		}

		if (0 == g_strcmp0(calculator, "flip"))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			                    "calc flip takes no stake; its capital is the buy price");
			return -1;
		}

		g_string_append(path, "&stake=");
		g_string_append_uri_escaped(path, stake, NULL, FALSE);
	}

	/* The bare words: every outcome's odds for a surebet, the back and
	 * lay odds, or the buy and sell prices. */
	if (0 == g_strcmp0(calculator, "surebet"))
	{
		if (positional->len > 0)
		{
			g_autoptr(GString) odds = g_string_new(NULL);

			for (i = 0; i < positional->len; i++)
				g_string_append_printf(odds, "%s%s", (i > 0) ? " " : "",
				                       (const gchar *)g_ptr_array_index(positional, i));

			g_string_append(path, "&odds=");
			g_string_append_uri_escaped(path, odds->str, NULL, FALSE);
		}
	}
	else if (positional->len > 0)
	{
		const gchar *first_name = (0 == g_strcmp0(calculator, "flip")) ? "buy" : "back_odds";
		const gchar *second_name = (0 == g_strcmp0(calculator, "flip")) ? "sell" : "lay_odds";

		if (positional->len != 2)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "calc %s takes two values before its options (%s and %s), not %u",
			            verb, first_name, second_name, positional->len);
			return -1;
		}

		g_string_append_printf(path, "&%s=", first_name);
		g_string_append_uri_escaped(path, g_ptr_array_index(positional, 0), NULL, FALSE);
		g_string_append_printf(path, "&%s=", second_name);
		g_string_append_uri_escaped(path, g_ptr_array_index(positional, 1), NULL, FALSE);
	}

	response = venture_cli_request(cli, "GET", path->str, NULL, error);

	if (NULL == response)
		return -1;

	if (VENTURE_OUTPUT_FORMAT_TABLE != cli->format)
	{
		venture_cli_print_answer(cli, response, (0 == g_strcmp0(calculator, "surebet")) ? "outcomes" : NULL,
		                         venture_cli_arbitrage_outcome_columns, "");
		return 0;
	}

	if (0 == g_strcmp0(calculator, "surebet"))
	{
		venture_cli_print_figures(json_node_get_object(response), venture_cli_arbitrage_surebet_figures);
		g_print("\n");
		venture_cli_print_answer(cli, response, "outcomes", venture_cli_arbitrage_outcome_columns, "");
	}
	else
	{
		venture_cli_print_figures(json_node_get_object(response),
		                          (0 == g_strcmp0(calculator, "flip")) ? venture_cli_arbitrage_flip_figures
		                                                               : venture_cli_arbitrage_back_lay_figures);
	}

	return 0;
}

/* arbitrage export FORMAT ...: the scan as a file a format registered. */
static gint
venture_cli_arbitrage_export(
	VentureCli	 *cli,
	gchar		**args,
	const gchar	 *output,
	GError		**error
){
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(GString) path = NULL;
	g_autoptr(GBytes) bytes = NULL;

	if ((NULL == args[2]) || (NULL != strchr(args[2], '=')))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "usage: venturectl arbitrage export FORMAT [STRATEGY] [option=value ...] [-o FILE] "
		                    "(arbitrage registries lists the formats)");
		return -1;
	}

	if (!venture_cli_arbitrage_options(args, 3, TRUE, "arbitrage export", options, error))
		return -1;

	path = g_string_new("/api/v1/arbitrage/export?format=");
	g_string_append_uri_escaped(path, args[2], NULL, FALSE);
	venture_cli_arbitrage_query(path, options);

	bytes = venture_cli_request_bytes(cli, "GET", path->str, error);

	if (NULL == bytes)
		return -1;

	if (NULL != output)
	{
		g_autoptr(GError) local_error = NULL;

		if (!g_file_set_contents(output, g_bytes_get_data(bytes, NULL),
		                         (gssize)g_bytes_get_size(bytes), &local_error))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "%s", local_error->message);
			return -1;
		}

		if (!cli->quiet)
			g_printerr("Wrote %" G_GSIZE_FORMAT " bytes to %s\n", g_bytes_get_size(bytes), output);

		return 0;
	}

	if (fwrite(g_bytes_get_data(bytes, NULL), 1, g_bytes_get_size(bytes), stdout) != g_bytes_get_size(bytes))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "Could not write the export");
		return -1;
	}

	return 0;
}

/* Writes @bytes to @output, or to stdout. */
static gint
venture_cli_arbitrage_write(
	VentureCli	 *cli,
	GBytes		 *bytes,
	const gchar	 *output,
	GError		**error
){
	if (NULL != output)
	{
		g_autoptr(GError) local_error = NULL;

		if (!g_file_set_contents(output, g_bytes_get_data(bytes, NULL),
		                         (gssize)g_bytes_get_size(bytes), &local_error))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "%s", local_error->message);
			return -1;
		}

		if (!cli->quiet)
			g_printerr("Wrote %" G_GSIZE_FORMAT " bytes to %s\n", g_bytes_get_size(bytes), output);

		return 0;
	}

	if (fwrite(g_bytes_get_data(bytes, NULL), 1, g_bytes_get_size(bytes), stdout) != g_bytes_get_size(bytes))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_FAILED, "Could not write the export");
		return -1;
	}

	return 0;
}

/* arbitrage crafting [option=value ...]: every recipe at the picked realms. */
static gint
venture_cli_arbitrage_crafting(
	VentureCli	 *cli,
	gchar		**args,
	GError		**error
){
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(GString) path = g_string_new("/api/v1/arbitrage/crafting");
	g_autoptr(JsonNode) response = NULL;

	if (!venture_cli_arbitrage_options(args, 2, FALSE, "arbitrage crafting", options, error))
		return -1;

	venture_cli_arbitrage_query(path, options);
	response = venture_cli_request(cli, "GET", path->str, NULL, error);

	if (NULL == response)
		return -1;

	venture_cli_print_answer(cli, response, "rows", venture_cli_arbitrage_crafting_columns,
	                         "No recipe to price: import your professions' recipes, or widen the question.");
	return 0;
}

/* arbitrage plan [reprice=1] | plan export FORMAT | plan remove TRADE_ID. */
static gint
venture_cli_arbitrage_plan(
	VentureCli	 *cli,
	gchar		**args,
	const gchar	 *output,
	GError		**error
){
	g_autoptr(JsonNode) response = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(GString) path = NULL;

	if ((NULL != output) && (0 != g_strcmp0(args[2], "export")))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "-o belongs to arbitrage plan export");
		return -1;
	}

	if (0 == g_strcmp0(args[2], "export"))
	{
		g_autoptr(GBytes) bytes = NULL;

		path = g_string_new("/api/v1/arbitrage/plan/export?format=");
		g_string_append_uri_escaped(path, (NULL != args[3]) ? args[3] : "csv", NULL, FALSE);

		if ((NULL != args[3]) && !venture_cli_arbitrage_options(args, 4, FALSE, "arbitrage plan export",
		                                                        options, error))
			return -1;

		venture_cli_arbitrage_query(path, options);
		bytes = venture_cli_request_bytes(cli, "GET", path->str, error);

		return (NULL == bytes) ? -1 : venture_cli_arbitrage_write(cli, bytes, output, error);
	}

	if (0 == g_strcmp0(args[2], "remove"))
	{
		g_autoptr(JsonNode) body = json_node_new(JSON_NODE_OBJECT);
		JsonObject *object = json_object_new();
		gint64 id;

		if (!venture_cli_positive_id(args[3], "usage: venturectl arbitrage plan remove TRADE_ID", &id, error) ||
		    !venture_cli_arbitrage_options(args, 4, FALSE, "arbitrage plan remove", options, error))
			return -1;

		json_object_set_int_member(object, "trade_id", id);

		if (json_object_has_member(options, "organization_id"))
			json_object_set_string_member(object, "organization_id",
			                              json_object_get_string_member(options, "organization_id"));

		json_node_take_object(body, object);
		response = venture_cli_request(cli, "POST", "/api/v1/arbitrage/plan/remove", body, error);

		if (NULL == response)
			return -1;

		if (VENTURE_OUTPUT_FORMAT_TABLE != cli->format)
			venture_cli_output(cli, response);
		else if (!cli->quiet)
			g_print("Removed trade #%" G_GINT64_FORMAT " from the plan\n", id);

		return 0;
	}

	if (!venture_cli_arbitrage_options(args, 2, FALSE, "arbitrage plan", options, error))
		return -1;

	path = g_string_new("/api/v1/arbitrage/plan");
	venture_cli_arbitrage_query(path, options);
	response = venture_cli_request(cli, "GET", path->str, NULL, error);

	if (NULL == response)
		return -1;

	if (VENTURE_OUTPUT_FORMAT_TABLE != cli->format)
	{
		venture_cli_output(cli, response);
		return 0;
	}

	venture_cli_print_answer(cli, response, "totals", venture_cli_arbitrage_plan_total_columns,
	                         "Nothing is planned.");
	g_print("\n");
	venture_cli_print_answer(cli, response, "trades", venture_cli_arbitrage_plan_trade_columns, "");
	return 0;
}

/* arbitrage registries: what this server's strategies, fees and exports are. */
static gint
venture_cli_arbitrage_registries(
	VentureCli	 *cli,
	GError		**error
){
	g_autoptr(JsonNode) response = NULL;
	JsonArray *names;
	guint i;

	response = venture_cli_request(cli, "GET", "/api/v1/arbitrage/registries", NULL, error);

	if (NULL == response)
		return -1;

	if (VENTURE_OUTPUT_FORMAT_TABLE != cli->format)
	{
		venture_cli_print_answer(cli, response, "strategies", venture_cli_arbitrage_strategy_columns, "");
		return 0;
	}

	venture_cli_print_answer(cli, response, "strategies", venture_cli_arbitrage_strategy_columns,
	                         "No strategies.");
	g_print("\n");
	venture_cli_print_answer(cli, response, "fee_models", venture_cli_arbitrage_fee_columns, "No fee models.");
	g_print("\n");
	venture_cli_print_answer(cli, response, "export_formats", venture_cli_arbitrage_format_columns,
	                         "No export formats.");

	names = json_object_get_array_member(json_node_get_object(response), "options");
	g_print("\nscan options:");

	for (i = 0; (NULL != names) && (i < json_array_get_length(names)); i++)
		g_print("%s %s", (i > 0) ? "," : "", json_array_get_string_element(names, i));

	g_print("\n");
	return 0;
}

static gint
venture_cli_command_arbitrage(
	VentureCli	 *cli,
	gchar		**args,
	const gchar	 *stake,
	const gchar	 *output,
	GError		**error
){
	const gchar *verb = args[1];

	if (0 == g_strcmp0(verb, "help"))
	{
		g_print("%s\n", venture_cli_arbitrage_usage);
		return 0;
	}

	if ((NULL != stake) && (0 != g_strcmp0(verb, "calc")))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "--stake belongs to arbitrage calc");
		return -1;
	}

	if ((NULL != output) && (0 != g_strcmp0(verb, "export")) && (0 != g_strcmp0(verb, "plan")))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "-o belongs to arbitrage export and arbitrage plan export");
		return -1;
	}

	if (0 == g_strcmp0(verb, "scan"))
	{
		g_autoptr(JsonObject) options = json_object_new();
		g_autoptr(JsonNode) response = NULL;

		if (!venture_cli_arbitrage_options(args, 2, TRUE, "arbitrage scan", options, error))
			return -1;

		response = venture_cli_arbitrage_run_scan(cli, options, error);

		if (NULL == response)
			return -1;

		venture_cli_print_answer(cli, response, "rows", venture_cli_arbitrage_scan_columns,
		                         "No opportunities pass the filters now.");
		return 0;
	}

	if (0 == g_strcmp0(verb, "record"))
		return venture_cli_arbitrage_record(cli, args, error);

	if (0 == g_strcmp0(verb, "crafting"))
		return venture_cli_arbitrage_crafting(cli, args, error);

	if (0 == g_strcmp0(verb, "plan"))
		return venture_cli_arbitrage_plan(cli, args, output, error);

	if (0 == g_strcmp0(verb, "calc"))
		return venture_cli_arbitrage_calc(cli, args, stake, error);

	if (0 == g_strcmp0(verb, "export"))
		return venture_cli_arbitrage_export(cli, args, output, error);

	if (0 == g_strcmp0(verb, "registries"))
		return venture_cli_arbitrage_registries(cli, error);

	if ((0 == g_strcmp0(verb, "close")) || (0 == g_strcmp0(verb, "reopen")) ||
	    (0 == g_strcmp0(verb, "abandon")) || (0 == g_strcmp0(verb, "execute")))
	{
		gboolean leg = (0 == g_strcmp0(verb, "execute"));
		g_autofree gchar *usage = NULL;
		gint64 id;

		usage = g_strdup_printf("usage: venturectl arbitrage %s %s [name=value ...]", verb,
		                        leg ? "LEG_ID" : "TRADE_ID");

		if (!venture_cli_positive_id(args[2], usage, &id, error))
			return -1;

		return venture_cli_arbitrage_act(cli, leg ? "arbitrage_leg" : "arbitrage_trade", args[2], verb,
		                                 args, 3, error);
	}

	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, venture_cli_arbitrage_usage);
	return -1;
}
