/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * venture-marketdata-accounts-cli.inc - venturectl accounts list|attention|
 * show|inventory|pnl
 *
 * Thin calls on /api/v1/accounts: the account operations pages' JSON
 * twins, asked under the pages' own query names and typed here before
 * anything is sent, so the CLI and the page cannot answer differently.
 * Included by src/cli/main.c after venture-marketdata-cli.inc, whose
 * table helpers and typed options these verbs share; it builds without
 * the server.
 */

static const VentureCliOption venture_cli_accounts_options[] = {
	{ "source", "source", VENTURE_CLI_OPTION_INT, NULL },
	{ "data_source_id", "source", VENTURE_CLI_OPTION_INT, NULL },
	{ "group", "group", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "group_key", "group", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "basis", "basis", VENTURE_CLI_OPTION_TEXT,
	  "conservative|market|min|historical|region_market|region_sale_avg" },
	{ "expiring_hours", "expiring_hours", VENTURE_CLI_OPTION_INT, NULL },
	{ "mail_days", "mail_days", VENTURE_CLI_OPTION_INT, NULL },
	{ "stale_days", "stale_days", VENTURE_CLI_OPTION_INT, NULL },
	{ "sort", "sort", VENTURE_CLI_OPTION_TEXT,
	  "attention|name|realm|gold|positions|expiry|inbound|last_seen|freshness" },
	{ "dir", "dir", VENTURE_CLI_OPTION_TEXT, "asc|desc" },
	{ "organization_id", "organization_id", VENTURE_CLI_OPTION_INT, NULL },
	{ NULL, NULL, 0, NULL }
};

static const VentureCliOption venture_cli_accounts_show_options[] = {
	{ "basis", "basis", VENTURE_CLI_OPTION_TEXT,
	  "conservative|market|min|historical|region_market|region_sale_avg" },
	{ "expiring_hours", "expiring_hours", VENTURE_CLI_OPTION_INT, NULL },
	{ "ledger", "ledger", VENTURE_CLI_OPTION_INT, NULL },
	{ "organization_id", "organization_id", VENTURE_CLI_OPTION_INT, NULL },
	{ NULL, NULL, 0, NULL }
};

/* The report's spellings (account_key, category_path, dead_days...) are
 * taken too, so an option that worked on `report account_holdings`
 * works here. */
static const VentureCliOption venture_cli_accounts_inventory_options[] = {
	{ "source", "source", VENTURE_CLI_OPTION_INT, NULL },
	{ "data_source_id", "source", VENTURE_CLI_OPTION_INT, NULL },
	{ "account", "account", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "account_key", "account", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "place", "place", VENTURE_CLI_OPTION_TEXT,
	  "bag|bank|reagent_bank|warbank|guild|mail|auction|void|equipped|currency|other" },
	{ "category", "category", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "category_path", "category", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "search", "search", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "min_value", "min_value", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "dead", "dead", VENTURE_CLI_OPTION_BOOL, NULL },
	{ "dead_days", "dead_days", VENTURE_CLI_OPTION_INT, NULL },
	{ "basis", "basis", VENTURE_CLI_OPTION_TEXT,
	  "conservative|market|min|historical|region_market|region_sale_avg" },
	{ "sort", "sort", VENTURE_CLI_OPTION_TEXT, "value|quantity|name|unit_value|accounts|days_of_supply" },
	{ "dir", "dir", VENTURE_CLI_OPTION_TEXT, "asc|desc" },
	{ "page", "page", VENTURE_CLI_OPTION_INT, NULL },
	{ "per_page", "per_page", VENTURE_CLI_OPTION_INT, NULL },
	{ "organization_id", "organization_id", VENTURE_CLI_OPTION_INT, NULL },
	{ NULL, NULL, 0, NULL }
};

static const VentureCliOption venture_cli_accounts_pnl_options[] = {
	{ "source", "source", VENTURE_CLI_OPTION_INT, NULL },
	{ "data_source_id", "source", VENTURE_CLI_OPTION_INT, NULL },
	{ "period", "period", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "group_by", "group_by", VENTURE_CLI_OPTION_TEXT, "day|week|month|account|venue|instrument|source" },
	{ "account", "account", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "account_key", "account", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "venue", "venue", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "instrument", "instrument", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "label", "label", VENTURE_CLI_OPTION_TEXT, NULL },
	{ "top", "top", VENTURE_CLI_OPTION_INT, NULL },
	{ "organization_id", "organization_id", VENTURE_CLI_OPTION_INT, NULL },
	{ NULL, NULL, 0, NULL }
};

static const VentureCliColumn venture_cli_accounts_columns[] = {
	{ "display_name", "Account", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "key", "Key", VENTURE_CLI_CELL_TEXT, TRUE },
	{ "realm", "Realm", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "kind", "Kind", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "gold", "Money", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "positions", "Listings", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "positions_expired", "Expired", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "soonest_expiry", "Soonest expiry", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "inbound", "Mail", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "last_seen", "Last seen", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "data_source_id", "Source", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ NULL, NULL, 0, FALSE }
};

static const VentureCliColumn venture_cli_accounts_summary_figures[] = {
	{ "summary.balances", "Money on hand", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "summary.inventory_value", "Inventory value", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "summary.positions_value", "Listed, at buyout", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "summary.positions", "Listings", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "summary.positions_expired", "Listings expired", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "summary.inbound_money", "Waiting in the mail", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "summary.net_30d", "Net, 30 days", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "summary.needs_login", "Places to log in to", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ NULL, NULL, 0, FALSE }
};

static const VentureCliColumn venture_cli_account_figures[] = {
	{ "account.display_name", "Account", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "account.realm", "Realm", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "account.kind", "Kind", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "account.gold", "Money", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "holdings_value", "Holdings value", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "account.last_seen", "Last seen", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "account.synced_at", "Data from", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "account.reasons", "Log in for", VENTURE_CLI_CELL_TEXT, FALSE },
	{ NULL, NULL, 0, FALSE }
};

static const VentureCliColumn venture_cli_account_position_columns[] = {
	{ "instrument_name", "Item", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "quantity", "Units", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "unit_price", "Each", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "vs_market_pct", "Of market", VENTURE_CLI_CELL_PERCENT, FALSE },
	{ "undercut", "Undercut", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "expires_at", "Expires", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "urgency", "When", VENTURE_CLI_CELL_TEXT, FALSE },
	{ NULL, NULL, 0, FALSE }
};

static const VentureCliColumn venture_cli_account_inbound_columns[] = {
	{ "sender", "From", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "subject", "Subject", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "instrument_name", "Item", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "quantity", "Units", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "money", "Money", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "expires_at", "Expires", VENTURE_CLI_CELL_TEXT, FALSE },
	{ NULL, NULL, 0, FALSE }
};

static const VentureCliColumn venture_cli_inventory_columns[] = {
	{ "instrument_name", "Item", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "instrument_key", "Key", VENTURE_CLI_CELL_TEXT, TRUE },
	{ "quantity", "Units", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "unit_value", "Each", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "value", "Value", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "share", "Of inventory", VENTURE_CLI_CELL_FRACTION, FALSE },
	{ "days_of_supply", "Days to sell", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "accounts", "Held by", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ NULL, NULL, 0, FALSE }
};

static const VentureCliColumn venture_cli_pnl_columns[] = {
	{ "label", "Group", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "period_start", "Period", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "sales_amount", "Sales", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "sold_units", "Sold", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "buys_amount", "Purchases", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "bought_units", "Bought", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "income", "Income", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "expense", "Expenses", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "net", "Net", VENTURE_CLI_CELL_MONEY, FALSE },
	{ NULL, NULL, 0, FALSE }
};

static const VentureCliColumn venture_cli_pnl_totals_columns[] = {
	{ "currency", "Currency", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "sales_amount", "Sales", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "buys_amount", "Purchases", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "income", "Income", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "expense", "Expenses", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "net", "Net", VENTURE_CLI_CELL_MONEY, FALSE },
	{ NULL, NULL, 0, FALSE }
};

static const VentureCliColumn venture_cli_flip_columns[] = {
	{ "instrument_name", "Item", VENTURE_CLI_CELL_TEXT, FALSE },
	{ "matched_units", "Units", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "avg_buy", "Bought at", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "avg_sell", "Sold at", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "profit", "Profit", VENTURE_CLI_CELL_MONEY, FALSE },
	{ "roi", "Return", VENTURE_CLI_CELL_FRACTION, FALSE },
	{ "open_units", "Still open", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ "held", "Held now", VENTURE_CLI_CELL_NUMBER, FALSE },
	{ NULL, NULL, 0, FALSE }
};

static const gchar venture_cli_accounts_usage[] =
	"usage: venturectl accounts VERB ...\n"
	"  list [source=N] [group=REALM] [basis=B] [sort=S] [dir=asc|desc]\n"
	"       [expiring_hours=N] [mail_days=N] [stale_days=N]\n"
	"  attention [the same options]     where to log in next, most urgent first\n"
	"  show SOURCE_ID KEY [basis=B] [ledger=N]\n"
	"  inventory [source=N] [account=KEY] [place=P] [category=PATH] [search=TEXT]\n"
	"            [min_value=\"100.00 GOLD\"] [dead=true] [dead_days=N] [basis=B]\n"
	"            [sort=S] [dir=asc|desc] [page=N] [per_page=N]\n"
	"  pnl [source=N] [period=P] [group_by=G] [account=KEY] [venue=KEY]\n"
	"      [instrument=KEY] [label=TEXT] [top=N]\n"
	"Every verb takes organization_id=N (default: the active organization).\n"
	"Bases: conservative (the default: the lower of the region's sale average and\n"
	"the account's realm market value), market, min, historical, region_market,\n"
	"region_sale_avg.\n"
	"Examples:\n"
	"  venturectl accounts attention\n"
	"  venturectl accounts list sort=gold dir=desc\n"
	"  venturectl accounts show 4 \"Drgold-Thorium Brotherhood\"\n"
	"  venturectl accounts inventory place=bank min_value=\"500.00 GOLD\"\n"
	"  venturectl accounts inventory dead=true dead_days=45 sort=value\n"
	"  venturectl accounts pnl period=last_90_days group_by=week\n"
	"  venturectl accounts pnl group_by=instrument account=\"Drgold-Thorium Brotherhood\"";

/* The places to log in to, each with its reasons, as lines a person reads
 * top to bottom; json and yaml print the whole answer. */
static void
venture_cli_accounts_print_attention(
	VentureCli	*cli,
	JsonNode	*answer
){
	JsonObject *root;
	JsonArray *rows;
	guint i;

	if ((VENTURE_OUTPUT_FORMAT_TABLE != cli->format) || !JSON_NODE_HOLDS_OBJECT(answer))
	{
		venture_cli_print_answer(cli, answer, "attention", NULL, "");
		return;
	}

	root = json_node_get_object(answer);
	rows = json_object_get_array_member(root, "attention");

	if (((NULL == rows) || (0 == json_array_get_length(rows))) && !cli->quiet)
		g_print("Nothing needs a login.\n");

	for (i = 0; (NULL != rows) && (i < json_array_get_length(rows)); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);
		JsonArray *reasons = json_object_get_array_member(row, "reasons");
		guint j;

		g_print("%s  [%s]\n", venture_json_object_get_string(row, "title", ""),
		        venture_json_object_get_string(row, "severity", ""));

		for (j = 0; (NULL != reasons) && (j < json_array_get_length(reasons)); j++)
		{
			JsonObject *reason = json_array_get_object_element(reasons, j);

			g_print("  - %s: %s\n", venture_json_object_get_string(reason, "account_name", ""),
			        venture_json_object_get_string(reason, "text", ""));
		}
	}

	venture_cli_print_notes(cli, root);
}

/* The money arrays of the summary as one line each, for the figures. */
static void
venture_cli_accounts_flatten_money(JsonObject *summary)
{
	static const gchar *const members[] = {
		"balances", "inventory_value", "positions_value", "inbound_money", "net_30d", NULL
	};
	guint i;

	for (i = 0; (NULL != summary) && (NULL != members[i]); i++)
	{
		JsonArray *amounts;
		g_autoptr(GString) text = g_string_new(NULL);
		guint j;

		if (!json_object_has_member(summary, members[i]))
			continue;

		amounts = json_object_get_array_member(summary, members[i]);

		for (j = 0; (NULL != amounts) && (j < json_array_get_length(amounts)); j++)
		{
			JsonObject *money = json_array_get_object_element(amounts, j);

			g_string_append_printf(text, "%s%s", (j > 0) ? ", " : "",
			                       venture_json_object_get_string(money, "formatted", ""));
		}

		json_object_set_string_member(summary, members[i], text->str);
	}
}

static gint
venture_cli_command_accounts(
	VentureCli	 *cli,
	gchar		**args,
	GError		**error
){
	g_autoptr(JsonNode) response = NULL;
	g_autoptr(GString) path = NULL;
	const gchar *verb;

	verb = (NULL != args[1]) ? args[1] : "list";

	if (0 == g_strcmp0(verb, "help"))
	{
		g_print("%s\n", venture_cli_accounts_usage);
		return 0;
	}

	/* `accounts source=2` is `accounts list source=2`. */
	if ((NULL != args[1]) && (NULL != strchr(args[1], '=')))
		verb = "list";

	if ((0 == g_strcmp0(verb, "list")) || (0 == g_strcmp0(verb, "attention")))
	{
		guint first = ((NULL != args[1]) && (NULL == strchr(args[1], '='))) ? 2 : 1;

		path = g_string_new("/api/v1/accounts");

		if (!venture_cli_query_from_args(path, (0 == g_strcmp0(verb, "list")) ? "accounts list"
		                                 : "accounts attention", args, first,
		                                 venture_cli_accounts_options, error))
			return -1;

		response = venture_cli_request(cli, "GET", path->str, NULL, error);

		if (NULL == response)
			return -1;

		if (0 == g_strcmp0(verb, "attention"))
		{
			venture_cli_accounts_print_attention(cli, response);
			return 0;
		}

		if ((VENTURE_OUTPUT_FORMAT_TABLE == cli->format) && JSON_NODE_HOLDS_OBJECT(response) && !cli->quiet)
		{
			/* Flattened in a deep copy: JSON-GLib shares nested objects,
			 * and the table below still reads the money objects. */
			g_autofree gchar *text = json_to_string(response, FALSE);
			g_autoptr(JsonNode) fresh = json_from_string(text, NULL);
			JsonObject *root = (NULL != fresh) ? json_node_get_object(fresh) : NULL;

			if (NULL != root)
			{
				venture_cli_accounts_flatten_money(json_object_get_object_member(root, "summary"));
				venture_cli_print_figures(root, venture_cli_accounts_summary_figures);
				g_print("\n");
			}
		}

		venture_cli_print_answer(cli, response, "accounts", venture_cli_accounts_columns,
		                         "No accounts yet: push them with venturectl feeds push.");
		return 0;
	}

	if (0 == g_strcmp0(verb, "show"))
	{
		static const gchar usage[] = "usage: venturectl accounts show SOURCE_ID KEY [basis=B] [ledger=N]";
		gint64 source;

		if (!venture_cli_positive_id(args[2], usage, &source, error))
			return -1;

		if ((NULL == args[3]) || ('\0' == args[3][0]))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, usage);
			return -1;
		}

		/* The key is one path segment, '/' and all: the route reads the
		 * rest of the path, as the page's own links escape it. */
		path = g_string_new(NULL);
		g_string_append_printf(path, "/api/v1/accounts/%" G_GINT64_FORMAT "/", source);
		g_string_append_uri_escaped(path, args[3], NULL, FALSE);

		if (!venture_cli_query_from_args(path, "accounts show", args, 4, venture_cli_accounts_show_options,
		                                 error))
			return -1;

		response = venture_cli_request(cli, "GET", path->str, NULL, error);

		if (NULL == response)
			return -1;

		if (VENTURE_OUTPUT_FORMAT_TABLE != cli->format)
		{
			venture_cli_print_answer(cli, response, "positions", venture_cli_account_position_columns, "");
			return 0;
		}

		venture_cli_print_figures(json_node_get_object(response), venture_cli_account_figures);
		g_print("\nListings\n");
		venture_cli_print_answer(cli, response, "positions", venture_cli_account_position_columns,
		                         "Nothing listed.");
		g_print("\nMail\n");
		venture_cli_print_answer(cli, response, "inbound", venture_cli_account_inbound_columns,
		                         "Nothing in the mail.");
		return 0;
	}

	if (0 == g_strcmp0(verb, "inventory"))
	{
		path = g_string_new("/api/v1/accounts/inventory");

		if (!venture_cli_query_from_args(path, "accounts inventory", args, 2,
		                                 venture_cli_accounts_inventory_options, error))
			return -1;

		response = venture_cli_request(cli, "GET", path->str, NULL, error);

		if (NULL == response)
			return -1;

		if ((VENTURE_OUTPUT_FORMAT_TABLE == cli->format) && !cli->quiet)
		{
			JsonObject *root = json_node_get_object(response);
			JsonObject *totals = json_object_get_object_member(root, "totals");
			JsonNode *value = (NULL != totals) ? json_object_get_member(totals, "value") : NULL;

			g_print("%" G_GINT64_FORMAT " items, %" G_GINT64_FORMAT " units, worth %s (page %" G_GINT64_FORMAT
			        " of %" G_GINT64_FORMAT ")\n\n",
			        json_object_get_int_member_with_default(totals, "instruments", 0),
			        json_object_get_int_member_with_default(totals, "units", 0),
			        ((NULL != value) && JSON_NODE_HOLDS_OBJECT(value))
			        ? venture_json_object_get_string(json_node_get_object(value), "formatted", "")
			        : "nothing priced",
			        json_object_get_int_member_with_default(root, "page", 1),
			        json_object_get_int_member_with_default(root, "pages", 0));
		}

		venture_cli_print_answer(cli, response, "rows", venture_cli_inventory_columns, "Nothing held matches.");
		return 0;
	}

	if (0 == g_strcmp0(verb, "pnl"))
	{
		path = g_string_new("/api/v1/accounts/pnl");

		if (!venture_cli_query_from_args(path, "accounts pnl", args, 2, venture_cli_accounts_pnl_options,
		                                 error))
			return -1;

		response = venture_cli_request(cli, "GET", path->str, NULL, error);

		if (NULL == response)
			return -1;

		if (VENTURE_OUTPUT_FORMAT_TABLE != cli->format)
		{
			venture_cli_print_answer(cli, response, "buckets", venture_cli_pnl_columns, "");
			return 0;
		}

		venture_cli_print_answer(cli, response, "totals", venture_cli_pnl_totals_columns, "No trades in the window.");
		g_print("\n");
		venture_cli_print_answer(cli, response, "buckets", venture_cli_pnl_columns, "");

		{
			JsonObject *flips = json_object_get_object_member(json_node_get_object(response), "flips");
			g_autoptr(JsonNode) wrapped = json_node_new(JSON_NODE_OBJECT);

			json_node_set_object(wrapped, flips);
			g_print("\nFlips\n");
			venture_cli_print_answer(cli, wrapped, "rows", venture_cli_flip_columns,
			                         "No flips: nothing bought in the window has sold yet.");
		}

		return 0;
	}

	g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
	            "accounts takes list, attention, show, inventory or pnl, not \"%s\"\n%s", verb,
	            venture_cli_accounts_usage);

	return -1;
}
