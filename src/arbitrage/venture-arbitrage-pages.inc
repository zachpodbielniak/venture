/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * venture-arbitrage-pages.inc - /arbitrage, /arbitrage/calc and their
 * /api/v1/arbitrage twins
 *
 * Included by venture-web-server.c after venture-marketdata-pages.inc,
 * whose small drawing helpers (mdw_*) it shares. Every page is a
 * renderer: it turns the request into the scan's options, asks the one
 * implementation (venture_arbitrage_scan_run(), venture_arbitrage_calculate()),
 * and draws the JSON; the twin returns that JSON untouched.
 *
 * "Record attempt" re-runs the question and goes through the opportunity's
 * plan and the `record` action (venture_arbitrage_record_opportunity()),
 * so what is recorded is what the data says now and every check the
 * action makes applies. Saving a preset posts to the generic record form
 * for `arbitrage_strategy`; no form here is written for a record type.
 *
 * Every handler checks its own session or token, the arbitrage module
 * and the role its records ask of the caller, and answers for one
 * organization: a page the sidebar's pick, an API call its
 * organization_id or else the active one (venture_web_request_organization()).
 */

/* --- Gates and the question --------------------------------------------------- */

/* A JSON value as text: a string as it is, a number written out. */
static gchar *
arw_node_text(JsonNode *node)
{
	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node))
		return NULL;

	if (G_TYPE_STRING == json_node_get_value_type(node))
		return g_strdup(json_node_get_string(node));

	if (G_TYPE_INT64 == json_node_get_value_type(node))
		return g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(node));

	if (G_TYPE_BOOLEAN == json_node_get_value_type(node))
		return g_strdup(json_node_get_boolean(node) ? "true" : "false");

	return NULL;
}

static HtmxResponse *
arw_gate(
	VentureWebServer	*self,
	HtmxRequest		*request,
	gboolean		 api,
	VentureUserRole		 role,
	GType			 reads
){
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	HtmxResponse *gate;

	gate = api ? venture_web_api_require(self, request, role)
	           : venture_web_ui_require_session(self, request);

	if (NULL != gate)
		return gate;

	gate = api ? venture_web_require_module_api(self, "arbitrage")
	           : venture_web_require_module_ui(self, request, "arbitrage");

	if (NULL != gate)
		return gate;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_web_require_for_type(self, principal, reads, role, &error))
		return venture_web_error_response(error);

	return NULL;
}

/* The scan's options from a query string or a form: every field but the
 * page's own (the opportunity's key, the calculator's name). */
static JsonObject *
arw_options(GHashTable *fields)
{
	JsonObject *options;
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	options = json_object_new();

	if (NULL == fields)
		return options;

	g_hash_table_iter_init(&iter, fields);

	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		if ((0 == g_strcmp0(key, "key")) || (0 == g_strcmp0(key, "format")) ||
		    venture_string_is_empty(value))
			continue;

		json_object_set_string_member(options, key, value);
	}

	return options;
}

/* The question as a query string, for links that keep it. */
static gchar *
arw_query_string(
	JsonObject	*options,
	const gchar	*skip
){
	g_autoptr(GString) query = NULL;
	g_autoptr(GList) members = NULL;
	GList *member;

	query = g_string_new(NULL);
	members = (NULL != options) ? json_object_get_members(options) : NULL;

	for (member = members; NULL != member; member = member->next)
	{
		g_autofree gchar *text = NULL;
		g_autofree gchar *escaped = NULL;

		if (0 == g_strcmp0(member->data, skip))
			continue;

		text = arw_node_text(json_object_get_member(options, member->data));

		if (venture_string_is_empty(text))
			continue;

		escaped = g_uri_escape_string(text, NULL, FALSE);
		g_string_append_printf(query, "%s%s=%s", (query->len > 0) ? "&" : "",
		                       (const gchar *)member->data, escaped);
	}

	return g_string_free(g_steal_pointer(&query), FALSE);
}

/* Hidden inputs carrying the question, so a form keeps it. */
static void
arw_hidden_options(
	GString		*html,
	JsonObject	*options,
	JsonObject	*override
){
	g_autoptr(GList) members = NULL;
	GList *member;

	members = (NULL != options) ? json_object_get_members(options) : NULL;

	for (member = members; NULL != member; member = member->next)
	{
		g_autofree gchar *text = NULL;

		if ((NULL != override) && json_object_has_member(override, member->data))
			continue;

		text = arw_node_text(json_object_get_member(options, member->data));

		if (venture_string_is_empty(text))
			continue;

		/* The name too: a strategy's narrow keys are its own to choose. */
		g_string_append(html, "<input type=\"hidden\" name=\"");
		venture_html_escape_append(html, (const gchar *)member->data);
		g_string_append(html, "\" value=\"");
		venture_html_escape_append(html, text);
		g_string_append(html, "\">");
	}

	members = (NULL != override) ? json_object_get_members(override) : NULL;

	for (member = members; NULL != member; member = member->next)
	{
		g_autofree gchar *text = arw_node_text(json_object_get_member(override, member->data));

		if (venture_string_is_empty(text))
			continue;

		/* The name too: a strategy's narrow keys are its own to choose. */
		g_string_append(html, "<input type=\"hidden\" name=\"");
		venture_html_escape_append(html, (const gchar *)member->data);
		g_string_append(html, "\" value=\"");
		venture_html_escape_append(html, text);
		g_string_append(html, "\">");
	}
}

/* A text box refilled from the normalised question. */
static void
arw_input(
	GString		*html,
	JsonObject	*options,
	const gchar	*label,
	const gchar	*name,
	const gchar	*placeholder
){
	g_autofree gchar *value = NULL;

	value = (NULL != options) ? arw_node_text(json_object_get_member(options, name)) : NULL;
	g_string_append(html, "<div class=\"field\"><label>");
	venture_html_escape_append(html, label);
	g_string_append_printf(html, "<input name=\"%s\" value=\"", name);
	venture_html_escape_append(html, (NULL != value) ? value : "");
	g_string_append(html, "\" placeholder=\"");
	venture_html_escape_append(html, placeholder);
	g_string_append(html, "\"></label></div>");
}

static void
arw_select(
	GString			*html,
	JsonObject		*options,
	const gchar		*label,
	const gchar		*name,
	const gchar *const	*choices,
	const gchar		*empty
){
	const gchar *chosen;
	guint i;

	chosen = venture_json_object_get_string(options, name, NULL);
	g_string_append(html, "<div class=\"field\"><label>");
	venture_html_escape_append(html, label);
	g_string_append_printf(html, "<select name=\"%s\">", name);

	if (NULL != empty)
	{
		g_string_append(html, "<option value=\"\">");
		venture_html_escape_append(html, empty);
		g_string_append(html, "</option>");
	}

	for (i = 0; NULL != choices[i]; i++)
	{
		g_string_append_printf(html, "<option value=\"%s\"%s>%s</option>", choices[i],
		                       (0 == g_strcmp0(choices[i], chosen)) ? " selected" : "", choices[i]);
	}

	g_string_append(html, "</select></label></div>");
}

/* --- /arbitrage ----------------------------------------------------------------- */

static void
arw_plan_form(
	GString		*html,
	JsonObject	*options,
	JsonObject	*row,
	const gchar	*action,
	const gchar	*label
);


/* The strategies as tabs, the chosen one marked. */
static void
arw_tabs(
	VentureWebServer	*self,
	GString			*html,
	const gchar		*chosen
){
	VentureArbitrageStrategyRegistry *strategies;
	g_auto(GStrv) names = NULL;
	guint i;

	strategies = venture_context_get_arbitrage_strategies(self->context);
	names = venture_arbitrage_strategy_registry_dup_names(strategies);
	g_string_append(html, "<nav class=\"arbitrage-tabs\" aria-label=\"Strategies\">");

	for (i = 0; NULL != names[i]; i++)
	{
		gboolean current = (0 == g_strcmp0(names[i], chosen));

		g_string_append_printf(html, "<a class=\"arbitrage-tab\" href=\"/arbitrage?strategy=%s\"%s title=\"",
		                       names[i], current ? " aria-current=\"page\"" : "");
		venture_html_escape_append(html,
			venture_arbitrage_strategy_registry_get_description(strategies, names[i]));
		g_string_append(html, "\">");
		venture_html_escape_append(html, venture_arbitrage_strategy_registry_get_label(strategies, names[i]));
		g_string_append(html, "</a>");
	}

	g_string_append(html, "</nav>");
}

/* The presets to load, and the form that saves this question as one. */
static void
arw_presets(
	VentureWebServer	*self,
	gint64			 organization_id,
	GString			*html,
	JsonObject		*options
){
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) presets = NULL;
	g_autoptr(JsonObject) rest = NULL;
	g_autoptr(GList) members = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *text = NULL;
	GList *member;
	gint64 chosen;
	guint i;

	chosen = venture_json_object_get_int(options, "preset_id", 0);
	query = venture_query_new(VENTURE_TYPE_ARBITRAGE_STRATEGY);
	venture_query_set_organization(query, organization_id);
	venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 200);
	presets = venture_database_find(venture_context_get_database(self->context), query, NULL);

	g_string_append(html, "<div class=\"card arbitrage-presets\"><div class=\"card-head\"><h2>Presets</h2>"
	                      "</div><div class=\"card-body\"><form method=\"get\" action=\"/arbitrage\" "
	                      "class=\"form-grid\"><div class=\"field\"><label>Preset<select name=\"preset_id\">"
	                      "<option value=\"\">none</option>");

	for (i = 0; (NULL != presets) && (i < presets->len); i++)
	{
		VentureEntity *preset = g_ptr_array_index(presets, i);
		g_autofree gchar *name = venture_entity_get_display_name(preset);

		g_string_append_printf(html, "<option value=\"%" G_GINT64_FORMAT "\"%s>",
		                       venture_entity_get_id(preset),
		                       (venture_entity_get_id(preset) == chosen) ? " selected" : "");
		venture_html_escape_append(html, name);
		g_string_append(html, "</option>");
	}

	g_string_append(html, "</select></label></div><button class=\"btn\" type=\"submit\">Load</button></form>");

	/* Saving goes through the generic record form: the preset's own
	 * fields, and the rest of the question as its options. */
	rest = json_object_new();
	members = json_object_get_members(options);

	for (member = members; NULL != member; member = member->next)
	{
		static const gchar *const own[] = {
			"strategy", "preset_id", "data_source_id", "buy_venues", "sell_venues", NULL
		};

		if (!g_strv_contains(own, member->data))
			json_object_set_member(rest, member->data,
			                       json_node_copy(json_object_get_member(options, member->data)));
	}

	node = json_node_new(JSON_NODE_OBJECT);
	json_node_set_object(node, rest);
	text = venture_json_to_string(node, FALSE);
	g_string_append_printf(html, "<form method=\"post\" action=\"/e/arbitrage_strategy\" class=\"form-grid\">"
	                       "<input type=\"hidden\" name=\"organization_id\" value=\"%" G_GINT64_FORMAT "\">",
	                       organization_id);
	g_string_append(html, "<input type=\"hidden\" name=\"strategy\" value=\"");
	venture_html_escape_append(html, venture_json_object_get_string(options, "strategy", "spread"));
	g_string_append(html, "\">");

	if (venture_json_object_get_int(options, "data_source_id", 0) > 0)
		g_string_append_printf(html, "<input type=\"hidden\" name=\"data-source-id\" value=\"%" G_GINT64_FORMAT "\">",
		                       venture_json_object_get_int(options, "data_source_id", 0));

	g_string_append(html, "<input type=\"hidden\" name=\"buy-venues\" value=\"");
	venture_html_escape_append(html, venture_json_object_get_string(options, "buy_venues", ""));
	g_string_append(html, "\"><input type=\"hidden\" name=\"sell-venues\" value=\"");
	venture_html_escape_append(html, venture_json_object_get_string(options, "sell_venues", ""));
	g_string_append(html, "\"><input type=\"hidden\" name=\"options\" value=\"");
	venture_html_escape_append(html, text);
	g_string_append(html, "\"><div class=\"field\"><label>Save this question as<input name=\"name\" "
	                      "required placeholder=\"e.g. EU herbs\"></label></div>"
	                      "<button class=\"btn\" type=\"submit\">Save preset</button></form></div></div>");
}

static void
arw_filters(
	VentureWebServer	*self,
	gint64			 organization_id,
	GString			*html,
	JsonObject		*options
){
	static const gchar *const bases[] = { "min", "market", "sale_avg", "region_median", "bid", NULL };
	static const gchar *const sorts[] = { "profit", "roi", "roi_per_day", "annualized", "ev", "confidence", NULL };
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) sources = NULL;
	const gchar *strategy;
	gint64 chosen;
	guint i;

	strategy = venture_json_object_get_string(options, "strategy", "spread");
	chosen = venture_json_object_get_int(options, "data_source_id", 0);
	query = venture_query_new(VENTURE_TYPE_DATA_SOURCE);
	venture_query_set_organization(query, organization_id);
	venture_query_add_order(query, "name", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 200);
	sources = venture_database_find(venture_context_get_database(self->context), query, NULL);

	g_string_append(html, "<div class=\"card\"><form method=\"get\" action=\"/arbitrage\" "
	                      "class=\"form-grid arbitrage-filters\" role=\"search\" aria-label=\"Scan filters\">");
	g_string_append(html, "<input type=\"hidden\" name=\"strategy\" value=\"");
	venture_html_escape_append(html, strategy);
	g_string_append(html, "\">");

	if (venture_json_object_get_int(options, "preset_id", 0) > 0)
		g_string_append_printf(html, "<input type=\"hidden\" name=\"preset_id\" value=\"%" G_GINT64_FORMAT "\">",
		                       venture_json_object_get_int(options, "preset_id", 0));

	g_string_append(html, "<div class=\"field\"><label>Data source<select name=\"data_source_id\">"
	                      "<option value=\"\">every source</option>");

	for (i = 0; (NULL != sources) && (i < sources->len); i++)
	{
		VentureEntity *source = g_ptr_array_index(sources, i);
		g_autofree gchar *name = venture_entity_get_display_name(source);

		g_string_append_printf(html, "<option value=\"%" G_GINT64_FORMAT "\"%s>", venture_entity_get_id(source),
		                       (venture_entity_get_id(source) == chosen) ? " selected" : "");
		venture_html_escape_append(html, name);
		g_string_append(html, "</option>");
	}

	g_string_append(html, "</select></label></div>");

	/* The venue group picker Deals has: buy and sell only at the group's
	 * venues (the user's banking realms, say). */
	{
		g_autoptr(JsonNode) groups = venture_marketdata_venue_group_choices(self->context, organization_id);
		g_autoptr(JsonObject) holder = json_object_new();

		json_object_set_array_member(holder, "venue_groups", json_array_ref(json_node_get_array(groups)));
		mdw_venue_group_picker(html, holder, venture_json_object_get_string(options, "venue_group", NULL));
	}

	arw_input(html, options, "Buy at", "buy_venues", "venue keys, comma separated");
	arw_input(html, options, "Sell at", "sell_venues", "venue keys, comma separated");
	arw_input(html, options, "Group", "group_key", "e.g. eu");
	arw_input(html, options, "Category", "category_path", "e.g. Trade Goods/Herb");
	arw_input(html, options, "Kind", "kind", "e.g. item");
	arw_input(html, options, "Instrument", "instrument", "one key");

	if (0 == g_strcmp0(strategy, "transform"))
	{
		arw_input(html, options, "Recipe", "recipe_id", "a recipe's id; every active one");
		arw_input(html, options, "Recipe category", "recipe_category_id", "a category's id, and beneath it");
	}

	arw_input(html, options, (0 == g_strcmp0(strategy, "transform")) ? "Batches" : "Units", "units", "1");

	if (0 == g_strcmp0(strategy, "spread"))
		arw_input(html, options, "Buy sources per item", "buy_sources", "1; more to drop-ship");

	if ((0 == g_strcmp0(strategy, "cover")) || (0 == g_strcmp0(strategy, "back_lay")))
		arw_input(html, options, "Stake", "total_stake", "e.g. 100.00 USD");
	else
		arw_select(html, options, "Sell at price", "sell_basis", bases, "the strategy's");

	arw_input(html, options, "Least profit", "min_profit", "e.g. 10.00 GOLD");
	arw_input(html, options, "Least ROI %", "min_roi", "e.g. 15");
	arw_input(html, options, "Least sale rate %", "min_sale_rate", "e.g. 20");
	arw_input(html, options, "Most capital", "max_capital", "e.g. 500.00 GOLD");
	arw_input(html, options, "Most % of sale average", "max_buy_pct", "e.g. 70");
	arw_input(html, options, "Least confidence", "min_confidence", "0 to 1");
	arw_input(html, options, "Newest within hours", "max_age_hours", "any age");
	arw_input(html, options, "Share of sales %", "share", "100");
	arw_select(html, options, "Sort by", "sort", sorts, NULL);
	arw_input(html, options, "How many", "top", "50");
	g_string_append(html, "<button class=\"btn\" type=\"submit\">Scan</button></form></div>");
}

/* The trend at the buy venue as a sparkline, or nothing. */
static void
arw_trend(
	GString		*html,
	JsonObject	*row
){
	g_autofree gdouble *values = NULL;
	g_autofree gchar *spark = NULL;
	g_autofree gchar *label = NULL;
	JsonArray *trend;
	guint i;
	guint n;

	trend = json_object_has_member(row, "trend") ? json_object_get_array_member(row, "trend") : NULL;
	n = (NULL != trend) ? json_array_get_length(trend) : 0;

	if (0 == n)
	{
		mdw_dash(html);
		return;
	}

	values = g_new0(gdouble, n);

	for (i = 0; i < n; i++)
	{
		JsonNode *node = json_array_get_element(trend, i);

		values[i] = JSON_NODE_HOLDS_NULL(node) ? NAN : (gdouble)json_node_get_int(node);
	}

	label = g_strdup_printf("Lowest price at the buy venue over 14 days, %s",
	                        venture_json_object_get_string(row, "title", ""));
	spark = venture_web_chart_sparkline(label, values, n);
	g_string_append(html, spark);
}

static void
arw_missing(
	GString		*html,
	JsonObject	*row
){
	JsonArray *missing;
	guint i;

	missing = json_object_has_member(row, "missing") ? json_object_get_array_member(row, "missing") : NULL;

	if ((NULL == missing) || (0 == json_array_get_length(missing)))
		return;

	g_string_append(html, "<div class=\"arbitrage-unquoted\">Unquoted: ");

	for (i = 0; i < json_array_get_length(missing); i++)
	{
		if (i > 0)
			g_string_append(html, "; ");

		venture_html_escape_append(html, json_array_get_string_element(missing, i));
	}

	g_string_append(html, "</div>");
}

/* A row's buy or sell side, or NULL when the strategy names none. */
static JsonObject *
arw_side(
	JsonObject	*row,
	const gchar	*side
){
	JsonNode *node = json_object_get_member(row, side);

	return ((NULL != node) && JSON_NODE_HOLDS_OBJECT(node)) ? json_node_get_object(node) : NULL;
}

/* A side's venue cell, with how old its price is when the scan says --
 * as on Deals, a stale side is greyed and flagged. */
static void
arw_venue(
	VentureWebServer	*self,
	GString			*html,
	JsonObject		*row,
	const gchar		*side
){
	JsonObject *object = arw_side(row, side);

	g_string_append(html, "<td");

	if ((NULL != object) && json_object_has_member(object, "age_seconds"))
		mdw_stale_class(html, object, "");

	g_string_append(html, ">");

	if (NULL == object)
	{
		mdw_dash(html);
		g_string_append(html, "</td>");
		return;
	}

	mdw_name(html, object, "venue_name", "venue_key");

	if (json_object_has_member(object, "age_seconds"))
		mdw_age(self, html, object, "");

	g_string_append(html, "</td>");
}

static void
arw_results(
	VentureWebServer	*self,
	GString			*html,
	JsonObject		*root,
	JsonObject		*options
){
	VentureExportFormatRegistry *formats;
	g_auto(GStrv) names = NULL;
	g_autofree gchar *query = NULL;
	JsonArray *rows;
	guint i;

	rows = json_object_get_array_member(root, "rows");
	formats = venture_context_get_export_formats(self->context);
	names = venture_export_format_registry_dup_names(formats);
	query = arw_query_string(options, NULL);

	g_string_append(html, "<div class=\"card\"><div class=\"card-head\"><h2>Opportunities</h2></div>"
	                      "<div class=\"card-body\"><p class=\"arbitrage-exports\">Export: ");

	for (i = 0; NULL != names[i]; i++)
	{
		g_string_append_printf(html, "%s<a href=\"/arbitrage/export?format=%s%s", (i > 0) ? " \xc2\xb7 " : "",
		                       names[i], (query[0] != '\0') ? "&amp;" : "");
		venture_html_escape_append(html, query);
		g_string_append(html, "\">");
		venture_html_escape_append(html, venture_export_format_registry_get_label(formats, names[i]));
		g_string_append(html, "</a>");
	}

	g_string_append(html, "</p></div><div class=\"table-wrap\"><table class=\"data arbitrage-table\"><thead><tr>"
	                      "<th scope=\"col\">Opportunity</th><th scope=\"col\">Buy at</th>"
	                      "<th scope=\"col\">Sell at</th><th scope=\"col\" class=\"num\">Units</th>"
	                      "<th scope=\"col\" class=\"num\">Net</th><th scope=\"col\" class=\"num\">Capital</th>"
	                      "<th scope=\"col\" class=\"num\">ROI</th><th scope=\"col\" class=\"num\">ROI a day</th>"
	                      "<th scope=\"col\" class=\"num\">Confidence</th><th scope=\"col\">Trend</th>"
	                      "<th scope=\"col\">Record</th></tr></thead><tbody>");

	for (i = 0; (NULL != rows) && (i < json_array_get_length(rows)); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);
		JsonObject *narrow;
		gboolean blanked;

		blanked = json_object_has_member(row, "missing") &&
		          (json_array_get_length(json_object_get_array_member(row, "missing")) > 0);
		g_string_append(html, "<tr><td>");
		venture_html_escape_append(html, venture_json_object_get_string(row, "title", ""));
		arw_missing(html, row);
		g_string_append(html, "</td>");
		arw_venue(self, html, row, "buy");
		arw_venue(self, html, row, "sell");
		g_string_append(html, "<td class=\"num\">");
		mdw_int(html, row, "units");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_money(html, row, "net");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_money(html, row, "capital");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_percent(html, row, "roi", 100.0);
		g_string_append(html, "</td><td class=\"num\">");
		mdw_percent(html, row, "roi_per_day", 100.0);
		g_string_append(html, "</td><td class=\"num\">");
		mdw_decimal(html, row, "confidence");
		g_string_append(html, "</td><td>");
		arw_trend(html, row);
		g_string_append(html, "</td><td>");

		/* The question again, narrowed to this one, and its key: the
		 * plan is made from what the data says when the button is
		 * pressed. */
		if (blanked)
			mdw_dash(html);
		else
		{
			narrow = json_object_has_member(row, "narrow") ? json_object_get_object_member(row, "narrow")
			                                                : NULL;
			g_string_append(html, "<form method=\"post\" action=\"/arbitrage/record\">");
			arw_hidden_options(html, options, narrow);
			g_string_append(html, "<input type=\"hidden\" name=\"key\" value=\"");
			venture_html_escape_append(html, venture_json_object_get_string(row, "key", ""));
			g_string_append(html, "\"><button class=\"btn\" type=\"submit\">Record attempt</button></form>");
			arw_plan_form(html, options, row, "/arbitrage/plan/add", "Add to plan");
		}

		g_string_append(html, "</td></tr>");
	}

	if ((NULL == rows) || (0 == json_array_get_length(rows)))
		g_string_append(html, "<tr><td colspan=\"11\" class=\"muted\">Nothing passes the filters right now.</td></tr>");

	g_string_append(html, "</tbody></table></div></div>");

	/* Under the opportunities: the providers of the rows shown. */
	mdw_attribution(html, root);
}

static HtmxResponse *
venture_arbitrage_web_scan(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) asked = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GString) html = NULL;
	JsonObject *root;
	JsonObject *options;
	gboolean api;
	HtmxResponse *gate;
	gint64 organization_id;

	(void)params;

	api = mdw_is_api(request);
	gate = arw_gate(self, request, api, VENTURE_USER_ROLE_VIEWER, VENTURE_TYPE_DATA_SOURCE);

	if (NULL != gate)
		return gate;

	if (!venture_web_request_organization(self, request, NULL, &organization_id, &error))
		return venture_web_error_response(error);

	asked = arw_options(htmx_request_get_query_params(request));
	answer = venture_arbitrage_scan_run(self->context, organization_id, asked, &error);

	if (NULL == answer)
		return venture_web_error_response(error);

	if (api)
		return venture_web_json_response(answer, 200);

	root = json_node_get_object(answer);
	options = json_object_get_object_member(root, "options");
	venture_arbitrage_add_trends(self->context, json_object_get_array_member(root, "rows"));

	html = g_string_new(NULL);
	mdw_head(html, "Trading", "Arbitrage",
	         "Opportunities in the latest prices: what to buy where, where it sells, and what it "
	         "makes after every venue's fees. Nothing reaches the books until you record an attempt.");
	arw_tabs(self, html, json_object_get_string_member(root, "strategy"));
	g_string_append(html, "<p class=\"muted\"><a href=\"/arbitrage/calc\">Calculators</a> \xc2\xb7 "
	                      "<a href=\"/e/arbitrage_trade\">Trades</a> \xc2\xb7 "
	                      "<a href=\"/reports/arbitrage_performance\">How strategies did</a></p>");
	/* The organization the scan answered for, not the sidebar's pick
	 * read again: the controls must offer what the results came from. */
	arw_filters(self, organization_id, html, options);
	arw_presets(self, organization_id, html, options);
	mdw_notes(html, root);
	arw_results(self, html, root, options);

	if (json_object_get_boolean_member(root, "truncated"))
		g_string_append(html, "<p class=\"muted market-more\">More passed the filters than are shown; "
		                      "raise How many or narrow the question.</p>");

	return venture_web_html_response(venture_web_page(self, request, "/arbitrage", "Arbitrage", html->str),
	                                 200);
}

/* --- Recording ----------------------------------------------------------------- */

static HtmxResponse *
venture_arbitrage_web_record(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) options = NULL;
	g_autoptr(VentureEntity) trade = NULL;
	g_autofree gchar *location = NULL;
	g_autofree gchar *key = NULL;
	VentureActor actor;
	gboolean api;
	gboolean stage = FALSE;
	HtmxResponse *gate;
	HtmxResponse *redirect;
	gint64 organization_id;

	(void)params;

	api = mdw_is_api(request);
	gate = arw_gate(self, request, api, VENTURE_USER_ROLE_EDITOR, VENTURE_TYPE_ARBITRAGE_TRADE);

	if (NULL != gate)
		return gate;

	/* A form from the page, or JSON from a client: the question and the
	 * opportunity's key either way. */
	if (g_str_has_prefix((NULL != htmx_request_get_content_type(request))
	                     ? htmx_request_get_content_type(request) : "",
	                     "application/x-www-form-urlencoded"))
	{
		key = g_strdup(htmx_request_get_form_value(request, "key"));
		options = arw_options(htmx_request_get_form_data(request));
	}
	else
	{
		g_autoptr(JsonNode) body = htmx_request_get_json(request, &error);

		if ((NULL == body) || !JSON_NODE_HOLDS_OBJECT(body))
		{
			if (NULL == error)
				g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				                    "Send the question and the opportunity's key as a JSON object");
			return venture_web_error_response(error);
		}

		options = json_object_ref(json_node_get_object(body));
		key = g_strdup(venture_json_object_get_string(options, "key", NULL));
		json_object_remove_member(options, "key");
	}

	/* The organization rides in the JSON with the question (a page posts
	 * none: it answers for the sidebar's pick). */
	if (!venture_web_request_organization(self, request, options, &organization_id, &error))
		return venture_web_error_response(error);

	json_object_remove_member(options, "organization_id");
	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);

	/* ?stage=1 proposes the trade rather than recording it, as every other
	 * write route does; a stageable action reached only by an unstaged
	 * route would make --stage a lie for the one write that names money
	 * before anybody has looked at it. */
	if (!venture_confirmation_parse_stage_flag(htmx_request_get_query_param(request, "stage"), &stage, &error))
		return venture_web_error_response(error);

	if (stage)
	{
		g_autoptr(JsonNode) staged = NULL;
		VentureConfirmation *confirmation;
		JsonObject *object;

		confirmation = venture_arbitrage_stage_opportunity(self->context, organization_id, options, key,
		                                                   &actor, principal->role, "rest-api", &error);

		if (NULL == confirmation)
			return venture_web_error_response(error);

		object = json_object_new();
		json_object_set_string_member(object, "status", "awaiting_approval");
		json_object_set_boolean_member(object, "staged", TRUE);
		json_object_set_member(object, "confirmation", venture_confirmation_to_json(confirmation));
		staged = json_node_new(JSON_NODE_OBJECT);
		json_node_take_object(staged, object);

		/* The page's form never asks for this; a proposal is answered
		 * the way the action route answers one, whoever asked. */
		return venture_web_json_response(staged, 202);
	}

	trade = venture_arbitrage_record_opportunity(self->context, organization_id, options, key, &actor,
	                                             principal->role, &error);

	if (NULL == trade)
		return venture_web_error_response(error);

	if (api)
	{
		/* venture_web_json_response() serialises and keeps no reference. */
		g_autoptr(JsonNode) created = venture_serializable_to_json(VENTURE_SERIALIZABLE(trade), FALSE);

		return venture_web_json_response(created, 201);
	}

	/* "Add to plan" is the same record, shown on the plan rather than
	 * on its own page: a planned trade is a line of the plan. */
	if (0 == g_strcmp0(htmx_request_get_path(request), "/arbitrage/plan/add"))
		location = g_strdup("/arbitrage/plan");
	else
		location = g_strdup_printf("/e/arbitrage_trade/%" G_GINT64_FORMAT, venture_entity_get_id(trade));

	redirect = htmx_response_new();
	htmx_response_set_status(redirect, 303);
	htmx_response_add_header(redirect, "Location", location);

	return redirect;
}

/* --- Export -------------------------------------------------------------------- */

static HtmxResponse *
venture_arbitrage_web_export(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	VentureWebServer *self = data;
	VentureExportFormatRegistry *formats;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) asked = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autofree gchar *disposition = NULL;
	const gchar *format;
	HtmxResponse *gate;
	HtmxResponse *response;
	gint64 organization_id;

	(void)params;

	gate = arw_gate(self, request, mdw_is_api(request), VENTURE_USER_ROLE_VIEWER, VENTURE_TYPE_DATA_SOURCE);

	if (NULL != gate)
		return gate;

	if (!venture_web_request_organization(self, request, NULL, &organization_id, &error))
		return venture_web_error_response(error);

	formats = venture_context_get_export_formats(self->context);
	format = mdw_param(request, "format");

	if (!venture_export_format_registry_has(formats, format))
	{
		g_auto(GStrv) names = venture_export_format_registry_dup_names(formats);
		g_autofree gchar *list = g_strjoinv(", ", names);

		g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND, "format is one of %s", list);
		return venture_web_error_response(error);
	}

	asked = arw_options(htmx_request_get_query_params(request));
	answer = venture_arbitrage_scan_run(self->context, organization_id, asked, &error);

	if (NULL == answer)
		return venture_web_error_response(error);

	bytes = venture_export_format_registry_export(formats, format,
		json_object_get_array_member(json_node_get_object(answer), "rows"),
		json_object_get_object_member(json_node_get_object(answer), "options"), &error);

	if (NULL == bytes)
		return venture_web_error_response(error);

	/* Bytes, never a string: an export format is a plugin's to define,
	 * and a copy through g_strndup() would end the body at its first
	 * zero byte. */
	response = htmx_response_new();
	htmx_response_set_bytes(response, bytes);
	htmx_response_set_content_type(response, venture_export_format_registry_get_content_type(formats, format));
	htmx_response_set_status(response, 200);
	disposition = g_strdup_printf("attachment; filename=\"arbitrage-%s.%s\"", format,
	                              venture_export_format_registry_get_extension(formats, format));
	htmx_response_add_header(response, "Content-Disposition", disposition);
	htmx_response_add_header(response, "Cache-Control", "no-store");

	return response;
}

/* --- /arbitrage/calc ------------------------------------------------------------- */

static void
arw_calc_money_list(
	GString		*html,
	JsonObject	*answer,
	const gchar	*member,
	guint		 i
){
	JsonArray *array;
	g_autoptr(VentureMoney) money = NULL;
	g_autofree gchar *text = NULL;

	array = json_object_has_member(answer, member) ? json_object_get_array_member(answer, member) : NULL;

	if ((NULL == array) || (i >= json_array_get_length(array)))
	{
		mdw_dash(html);
		return;
	}

	money = venture_money_from_json(json_array_get_element(array, i), NULL, NULL);
	text = (NULL != money) ? venture_money_to_display_string(money, TRUE) : NULL;
	venture_html_escape_append(html, (NULL != text) ? text : "");
}

static void
arw_figure(
	GString		*html,
	const gchar	*label,
	JsonObject	*answer,
	const gchar	*member,
	gint		 kind
){
	g_string_append(html, "<div class=\"stat\"><div class=\"stat-label\">");
	venture_html_escape_append(html, label);
	g_string_append(html, "</div><div class=\"stat-value figure\">");

	if (0 == kind)
		mdw_money(html, answer, member);
	else if (1 == kind)
		mdw_percent(html, answer, member, 100.0);
	else
		mdw_decimal(html, answer, member);

	g_string_append(html, "</div></div>");
}

/* One calculator's card: its form, refilled, and what it answered. */
static void
arw_calc_card(
	GString			*html,
	HtmxRequest		*request,
	const gchar		*calculator,
	const gchar		*title,
	const gchar *const	*fields,
	const gchar *const	*labels,
	const gchar *const	*hints,
	JsonObject		*answer,
	const GError		*error
){
	gboolean mine;
	guint i;

	mine = (0 == g_strcmp0(mdw_param(request, "calc"), calculator));
	g_string_append_printf(html, "<section class=\"card arbitrage-calc\" id=\"%s\"><div class=\"card-head\"><h2>",
	                       calculator);
	venture_html_escape_append(html, title);
	g_string_append_printf(html, "</h2></div><div class=\"card-body\"><form method=\"get\" "
	                       "action=\"/arbitrage/calc#%s\" class=\"form-grid\">"
	                       "<input type=\"hidden\" name=\"calc\" value=\"%s\">", calculator, calculator);

	for (i = 0; NULL != fields[i]; i++)
	{
		const gchar *value = mine ? mdw_param(request, fields[i]) : NULL;

		g_string_append(html, "<div class=\"field\"><label>");
		venture_html_escape_append(html, labels[i]);
		g_string_append_printf(html, "<input name=\"%s\" value=\"", fields[i]);
		venture_html_escape_append(html, (NULL != value) ? value : "");
		g_string_append(html, "\" placeholder=\"");
		venture_html_escape_append(html, hints[i]);
		g_string_append(html, "\"></label></div>");
	}

	g_string_append(html, "<button class=\"btn\" type=\"submit\">Calculate</button></form>");

	if (mine && (NULL != error))
	{
		g_string_append(html, "<div class=\"notice negative\" role=\"alert\">");
		venture_html_escape_append(html, error->message);
		g_string_append(html, "</div>");
	}

	if (!mine || (NULL == answer))
	{
		g_string_append(html, "</div></section>");
		return;
	}

	g_string_append(html, "<div class=\"stat-row arbitrage-answer\">");

	if (0 == g_strcmp0(calculator, "surebet"))
	{
		JsonArray *outcomes = json_object_get_array_member(answer, "outcomes");

		arw_figure(html, json_object_get_boolean_member(answer, "is_surebet") ? "Sure profit" : "Sure loss",
		           answer, "profit", 0);
		arw_figure(html, "Sure payout", answer, "payout", 0);
		arw_figure(html, "Ideal profit", answer, "profit_ideal", 0);
		arw_figure(html, "Staked", answer, "staked", 0);
		arw_figure(html, "Rounding left over", answer, "residual", 0);
		arw_figure(html, "Overround", answer, "overround", 1);
		g_string_append(html, "</div><div class=\"table-wrap\"><table class=\"data\"><thead><tr>"
		                      "<th scope=\"col\">Outcome</th><th scope=\"col\" class=\"num\">Odds</th>"
		                      "<th scope=\"col\" class=\"num\">Fair chance</th>"
		                      "<th scope=\"col\" class=\"num\">Stake</th>"
		                      "<th scope=\"col\" class=\"num\">Pays</th></tr></thead><tbody>");

		for (i = 0; (NULL != outcomes) && (i < json_array_get_length(outcomes)); i++)
		{
			JsonObject *outcome = json_array_get_object_element(outcomes, i);

			g_string_append_printf(html, "<tr><td>%u</td><td class=\"num\">", i + 1);
			mdw_decimal(html, outcome, "odds");
			g_string_append(html, "</td><td class=\"num\">");
			mdw_percent(html, outcome, "no_vig", 100.0);
			g_string_append(html, "</td><td class=\"num\">");
			arw_calc_money_list(html, answer, "stakes", i);
			g_string_append(html, "</td><td class=\"num\">");
			arw_calc_money_list(html, answer, "payouts", i);
			g_string_append(html, "</td></tr>");
		}

		g_string_append(html, "</tbody></table></div>");
	}
	else if (0 == g_strcmp0(calculator, "back_lay"))
	{
		arw_figure(html, "Lay stake", answer, "lay_stake", 0);
		arw_figure(html, "Liability", answer, "liability", 0);
		arw_figure(html, "If the back wins", answer, "if_back_wins", 0);
		arw_figure(html, "If the lay wins", answer, "if_lay_wins", 0);
		arw_figure(html, "Sure result", answer, "worst", 0);
		arw_figure(html, "Ideal", answer, "ideal", 0);
		arw_figure(html, "Rating", answer, "rating", 1);
		g_string_append(html, "</div>");
	}
	else
	{
		arw_figure(html, "Net", answer, "net", 0);
		arw_figure(html, "Capital", answer, "capital", 0);
		arw_figure(html, "ROI", answer, "roi", 1);
		arw_figure(html, "ROI a day", answer, "roi_per_day", 1);
		arw_figure(html, "Annualised", answer, "annualized", 1);
		arw_figure(html, "Listing loss", answer, "listing_loss", 0);
		arw_figure(html, "Expected value", answer, "ev", 0);
		arw_figure(html, "Days tied up", answer, "lock_days", 2);
		g_string_append(html, "</div>");
	}

	g_string_append(html, "</div></section>");
}

static HtmxResponse *
venture_arbitrage_web_calc(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	static const gchar *const surebet_fields[] = { "odds", "format", "stake", NULL };
	static const gchar *const surebet_labels[] = { "Best odds per outcome", "Odds format", "Total stake", NULL };
	static const gchar *const surebet_hints[] = { "e.g. 2.10, 2.05 or +110 -105", "auto", "e.g. 100.00 USD", NULL };
	static const gchar *const back_lay_fields[] = {
		"back_odds", "lay_odds", "format", "commission", "back_commission", "stake", NULL
	};
	static const gchar *const back_lay_labels[] = {
		"Back odds", "Lay odds", "Odds format", "Exchange commission %", "Back venue commission %",
		"Back stake", NULL
	};
	static const gchar *const back_lay_hints[] = {
		"e.g. 3.0", "e.g. 2.9", "auto", "e.g. 5", "0", "e.g. 50.00 USD", NULL
	};
	static const gchar *const flip_fields[] = {
		"buy", "sell", "units", "cut", "fixed", "deposit", "refundable", "sale_rate", "sold_per_day",
		"share", "transfer", "transit_hours", NULL
	};
	static const gchar *const flip_labels[] = {
		"Buy price", "Sell price", "Units", "Cut %", "Fixed fee per order", "Deposit % of sale",
		"Deposit refunded on a sale", "Sale rate %", "Sold a day", "Share of sales %",
		"Transfer cost", "Transit hours", NULL
	};
	static const gchar *const flip_hints[] = {
		"e.g. 1.25 GOLD", "e.g. 2.10 GOLD", "1", "e.g. 5", "none", "e.g. 15", "true", "e.g. 40",
		"e.g. 12", "100", "none", "0", NULL
	};
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) input = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GString) html = NULL;
	const gchar *calculator;
	JsonObject *answered;
	gboolean api;
	HtmxResponse *gate;

	(void)params;

	api = mdw_is_api(request);
	gate = arw_gate(self, request, api, VENTURE_USER_ROLE_VIEWER, VENTURE_TYPE_ARBITRAGE_STRATEGY);

	if (NULL != gate)
		return gate;

	calculator = mdw_param(request, "calc");
	input = arw_options(htmx_request_get_query_params(request));

	/* arw_options() drops `format` because on the scan pages it names an
	 * export; here it is the odds format, and dropping it read "+200 100"
	 * American odds as decimal ones whatever the form said. */
	if (NULL != mdw_param(request, "format"))
		json_object_set_string_member(input, "format", mdw_param(request, "format"));

	if (NULL != calculator)
		answer = venture_arbitrage_calculate(calculator, input, &error);

	if (api)
	{
		if (NULL == calculator)
			g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			                    "calc is surebet, back_lay or flip");

		if (NULL == answer)
			return venture_web_error_response(error);

		return venture_web_json_response(answer, 200);
	}

	answered = (NULL != answer) ? json_node_get_object(answer) : NULL;
	html = g_string_new(NULL);
	mdw_head(html, "Trading", "Calculators",
	         "Worked out on the server: stakes are rounded to the currency's smallest unit, and what "
	         "that rounding leaves over is shown, never hidden.");
	arw_calc_card(html, request, "surebet", "Surebet", surebet_fields, surebet_labels, surebet_hints,
	              answered, error);
	arw_calc_card(html, request, "back_lay", "Back and lay", back_lay_fields, back_lay_labels,
	              back_lay_hints, answered, error);
	arw_calc_card(html, request, "flip", "Flip ROI", flip_fields, flip_labels, flip_hints, answered, error);

	return venture_web_html_response(venture_web_page(self, request, "/arbitrage/calc", "Calculators",
	                                                  html->str), 200);
}

/* --- What is registered ---------------------------------------------------------- */

static HtmxResponse *
venture_arbitrage_web_registries(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	VentureWebServer *self = data;
	VentureArbitrageStrategyRegistry *strategies;
	VentureFeeModelRegistry *fees;
	VentureExportFormatRegistry *formats;
	g_auto(GStrv) strategy_names = NULL;
	g_auto(GStrv) fee_names = NULL;
	g_auto(GStrv) format_names = NULL;
	JsonObject *root;
	JsonArray *array;
	g_autoptr(JsonNode) node = NULL;
	HtmxResponse *gate;
	guint i;

	(void)params;

	gate = arw_gate(self, request, TRUE, VENTURE_USER_ROLE_VIEWER, VENTURE_TYPE_ARBITRAGE_STRATEGY);

	if (NULL != gate)
		return gate;

	strategies = venture_context_get_arbitrage_strategies(self->context);
	fees = venture_context_get_fee_models(self->context);
	formats = venture_context_get_export_formats(self->context);
	strategy_names = venture_arbitrage_strategy_registry_dup_names(strategies);
	fee_names = venture_fee_model_registry_dup_names(fees);
	format_names = venture_export_format_registry_dup_names(formats);
	root = json_object_new();
	array = json_array_new();

	for (i = 0; NULL != strategy_names[i]; i++)
	{
		JsonObject *entry = json_object_new();

		json_object_set_string_member(entry, "name", strategy_names[i]);
		json_object_set_string_member(entry, "label",
			venture_arbitrage_strategy_registry_get_label(strategies, strategy_names[i]));
		json_object_set_string_member(entry, "description",
			venture_arbitrage_strategy_registry_get_description(strategies, strategy_names[i]));
		json_array_add_object_element(array, entry);
	}

	json_object_set_array_member(root, "strategies", array);
	array = json_array_new();

	for (i = 0; NULL != fee_names[i]; i++)
	{
		JsonObject *entry = json_object_new();

		json_object_set_string_member(entry, "name", fee_names[i]);
		json_object_set_string_member(entry, "description",
		                              venture_fee_model_registry_get_description(fees, fee_names[i]));
		json_array_add_object_element(array, entry);
	}

	json_object_set_array_member(root, "fee_models", array);
	array = json_array_new();

	for (i = 0; NULL != format_names[i]; i++)
	{
		JsonObject *entry = json_object_new();

		json_object_set_string_member(entry, "name", format_names[i]);
		json_object_set_string_member(entry, "label",
		                              venture_export_format_registry_get_label(formats, format_names[i]));
		json_object_set_string_member(entry, "content_type",
		                              venture_export_format_registry_get_content_type(formats, format_names[i]));
		json_array_add_object_element(array, entry);
	}

	json_object_set_array_member(root, "export_formats", array);
	array = json_array_new();

	for (i = 0; NULL != venture_arbitrage_scan_option_names()[i]; i++)
		json_array_add_string_element(array, venture_arbitrage_scan_option_names()[i]);

	json_object_set_array_member(root, "options", array);
	node = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(node, root);

	return venture_web_json_response(node, 200);
}

/* --- The two reports' controls --------------------------------------------------- */

static void
venture_arbitrage_web_report_controls(
	VentureWebServer	*self,
	HtmxRequest		*request,
	VentureReport		*report,
	GString			*content
){
	const gchar *name;
	const gchar *organization;

	(void)self;

	name = venture_report_get_name(report);

	if ((0 != g_strcmp0(name, "arbitrage_scan")) && (0 != g_strcmp0(name, "craft_arbitrage")))
		return;

	g_string_append(content, "<form method=\"get\" class=\"form-grid\">");
	organization = htmx_request_get_query_param(request, "organization_id");

	if (!venture_string_is_empty(organization))
	{
		g_string_append(content, "<input type=\"hidden\" name=\"organization_id\" value=\"");
		venture_html_escape_append(content, organization);
		g_string_append(content, "\">");
	}

	if (0 == g_strcmp0(name, "arbitrage_scan"))
	{
		venture_web_goals_input(request, content, "Strategy", "strategy", "spread");
		venture_web_goals_input(request, content, "Preset", "preset_id", "a preset's id");
	}
	else
	{
		venture_web_goals_input(request, content, "Recipe", "recipe_id", "every active recipe");
		venture_web_goals_input(request, content, "Recipe category", "recipe_category_id",
		                        "a category's id, and beneath it");
	}

	venture_web_goals_input(request, content, "Data source", "data_source_id", "id; every source");
	venture_web_goals_input(request, content, "Buy at", "buy_venues", "venue keys, comma separated");
	venture_web_goals_input(request, content, "Sell at", "sell_venues", "venue keys, comma separated");
	venture_web_goals_input(request, content, "Venue group", "venue_group", "characters or a group's id");
	venture_web_goals_input(request, content, "Group", "group_key", "every group");
	venture_web_goals_input(request, content, "Units", "units", "1");
	venture_web_goals_input(request, content, "Sell at price", "sell_basis", "min, market, sale_avg, region_median, bid");
	venture_web_goals_input(request, content, "Newest within hours", "max_age_hours", "any age");

	if (0 == g_strcmp0(name, "arbitrage_scan"))
	{
		venture_web_goals_input(request, content, "Category", "category_path", "a store category path");
		venture_web_goals_input(request, content, "Kind", "kind", "e.g. item");
		venture_web_goals_input(request, content, "Instrument", "instrument", "one key");
		venture_web_goals_input(request, content, "Recipe", "recipe_id", "transform: one recipe");
		venture_web_goals_input(request, content, "Recipe category", "recipe_category_id",
		                        "transform: a category's id");
		venture_web_goals_input(request, content, "Buy sources per item", "buy_sources", "1");
		venture_web_goals_input(request, content, "Stake", "total_stake", "e.g. 100.00 USD");
		venture_web_goals_input(request, content, "Least profit", "min_profit", "e.g. 10.00 GOLD");
		venture_web_goals_input(request, content, "Least ROI %", "min_roi", "e.g. 15");
		venture_web_goals_input(request, content, "Least sale rate %", "min_sale_rate", "e.g. 20");
		venture_web_goals_input(request, content, "Most capital", "max_capital", "e.g. 500.00 GOLD");
		venture_web_goals_input(request, content, "Most % of sale average", "max_buy_pct", "e.g. 70");
		venture_web_goals_input(request, content, "Least confidence", "min_confidence", "0 to 1");
		venture_web_goals_input(request, content, "Share of sales %", "share", "100");
		venture_web_goals_input(request, content, "Sort by", "sort", "profit");
		venture_web_goals_input(request, content, "How many", "top", "50");
	}

	g_string_append(content, "<button class=\"btn\" type=\"submit\">Run report</button></form>");
}

/* --- /arbitrage/crafting ------------------------------------------------------- */

/* A <select> of the answer's realm choices, by label. */
static void
arw_realm_picker(
	GString		*html,
	JsonObject	*root,
	const gchar	*label,
	const gchar	*name,
	const gchar	*selected,
	const gchar	*empty
){
	JsonArray *choices;
	gboolean found = FALSE;
	guint i;

	choices = json_object_has_member(root, "realm_choices") ? json_object_get_array_member(root, "realm_choices")
	                                                        : NULL;
	g_string_append(html, "<div class=\"field\"><label>");
	venture_html_escape_append(html, label);
	g_string_append_printf(html, "<select name=\"%s\"><option value=\"\">", name);
	venture_html_escape_append(html, empty);
	g_string_append(html, "</option>");

	for (i = 0; (NULL != choices) && (i < json_array_get_length(choices)); i++)
	{
		JsonObject *choice = json_array_get_object_element(choices, i);
		const gchar *value = venture_json_object_get_string(choice, "value", "");
		gboolean chosen = (0 == g_strcmp0(value, selected));

		found = found || chosen;
		g_string_append(html, "<option value=\"");
		venture_html_escape_append(html, value);
		g_string_append_printf(html, "\"%s>", chosen ? " selected" : "");
		venture_html_escape_append(html, venture_json_object_get_string(choice, "name", value));
		g_string_append(html, "</option>");
	}

	/* A realm asked for that the list does not offer stays chosen rather
	 * than quietly becoming "every realm". */
	if (!found && !venture_string_is_empty(selected))
	{
		g_string_append(html, "<option value=\"");
		venture_html_escape_append(html, selected);
		g_string_append(html, "\" selected>");
		venture_html_escape_append(html, selected);
		g_string_append(html, "</option>");
	}

	g_string_append(html, "</select></label></div>");
}

/* The recipe categories as a picker: a profession, an expansion, a part
 * of its book, each with everything filed beneath it. */
static void
arw_recipe_category_picker(
	GString		*html,
	JsonObject	*root,
	gint64		 selected
){
	JsonArray *categories;
	guint i;

	categories = json_object_has_member(root, "categories") ? json_object_get_array_member(root, "categories")
	                                                        : NULL;
	g_string_append(html, "<div class=\"field\"><label>Recipe category<select name=\"recipe_category_id\">"
	                      "<option value=\"\">every recipe</option>");

	for (i = 0; (NULL != categories) && (i < json_array_get_length(categories)); i++)
	{
		JsonObject *category = json_array_get_object_element(categories, i);
		gint64 id = venture_json_object_get_int(category, "id", 0);

		g_string_append_printf(html, "<option value=\"%" G_GINT64_FORMAT "\"%s>", id,
		                       (id == selected) ? " selected" : "");
		venture_html_escape_append(html, venture_json_object_get_string(category, "path", ""));
		g_string_append(html, "</option>");
	}

	g_string_append(html, "</select></label></div>");
}

/* A form that adds one opportunity to the plan: the question, narrowed
 * to it, and its key -- the plan is made from what the data says when
 * the button is pressed, exactly as "Record attempt" does. */
static void
arw_plan_form(
	GString		*html,
	JsonObject	*options,
	JsonObject	*row,
	const gchar	*action,
	const gchar	*label
){
	JsonObject *narrow;

	narrow = json_object_has_member(row, "narrow") ? json_object_get_object_member(row, "narrow") : NULL;
	g_string_append_printf(html, "<form method=\"post\" action=\"%s\">", action);
	arw_hidden_options(html, options, narrow);
	g_string_append(html, "<input type=\"hidden\" name=\"key\" value=\"");
	venture_html_escape_append(html, venture_json_object_get_string(row, "key", ""));
	g_string_append(html, "\"><button class=\"btn\" type=\"submit\">");
	venture_html_escape_append(html, label);
	g_string_append(html, "</button></form>");
}

/* A money cell that sorts by its amount; @object may be NULL. */
static void
arw_money_cell(
	GString		*html,
	JsonObject	*object,
	const gchar	*member
){
	g_string_append(html, "<td class=\"num\"");
	mdw_sort_value(html, object, member);
	g_string_append(html, ">");
	mdw_money(html, object, member);
	g_string_append(html, "</td>");
}

/* A craft's reagents, each where it is cheapest and how old that is. */
static void
arw_craft_inputs(
	VentureWebServer	*self,
	GString			*html,
	JsonObject		*row
){
	JsonArray *inputs;
	guint i;

	inputs = json_object_has_member(row, "inputs") ? json_object_get_array_member(row, "inputs") : NULL;

	if ((NULL == inputs) || (0 == json_array_get_length(inputs)))
		return;

	g_string_append_printf(html, "<details class=\"craft-inputs\"><summary>%u reagent%s</summary><ul>",
	                       json_array_get_length(inputs), (1 == json_array_get_length(inputs)) ? "" : "s");

	for (i = 0; i < json_array_get_length(inputs); i++)
	{
		JsonObject *input = json_array_get_object_element(inputs, i);

		g_string_append_printf(html, "<li%s>%" G_GINT64_FORMAT " \xc3\x97 ",
		                       mdw_stale(input, "") ? " class=\"is-stale\"" : "",
		                       venture_json_object_get_int(input, "quantity", 0));
		venture_html_escape_append(html, venture_json_object_get_string(input, "name", ""));

		if (venture_json_object_get_bool(input, "reusable", FALSE))
			g_string_append(html, " <span class=\"muted\">(reusable, bought once)</span>");

		if (NULL != venture_json_object_get_string(input, "venue_name", NULL))
		{
			g_string_append(html, " at ");
			venture_html_escape_append(html, venture_json_object_get_string(input, "venue_name", ""));
			g_string_append(html, " ");
			mdw_age(self, html, input, "");
			g_string_append(html, ": ");
			mdw_money(html, input, "unit_price");
			g_string_append(html, " each, ");
			mdw_money(html, input, "cost");
		}
		else
		{
			g_string_append(html, ": ");
			venture_html_escape_append(html, venture_json_object_get_string(input, "problem", "unquoted"));
		}

		g_string_append(html, "</li>");
	}

	g_string_append(html, "</ul></details>");
}

static void
arw_warnings(
	GString		*html,
	JsonObject	*row
){
	JsonArray *warnings;
	guint i;

	warnings = json_object_has_member(row, "warnings") ? json_object_get_array_member(row, "warnings") : NULL;

	for (i = 0; (NULL != warnings) && (i < json_array_get_length(warnings)); i++)
	{
		g_string_append(html, "<div class=\"muted arbitrage-warning\">");
		venture_html_escape_append(html, json_array_get_string_element(warnings, i));
		g_string_append(html, "</div>");
	}
}

static void
arw_craft_rows(
	VentureWebServer	*self,
	GString			*html,
	JsonObject		*root,
	gboolean		 can_plan
){
	JsonObject *options;
	JsonArray *rows;
	guint i;

	options = json_object_get_object_member(root, "options");
	rows = json_object_get_array_member(root, "rows");

	g_string_append(html, "<div class=\"card\"><div class=\"card-head\"><h2>Recipes</h2></div>"
	                      "<div class=\"table-wrap\"><table class=\"data arbitrage-table crafting-table\">"
	                      "<thead><tr><th scope=\"col\">Recipe</th><th scope=\"col\">Category</th>"
	                      "<th scope=\"col\" class=\"num\">Reagents</th><th scope=\"col\">Sell at</th>"
	                      "<th scope=\"col\" class=\"num\">Sells for</th><th scope=\"col\" class=\"num\">Gross</th>"
	                      "<th scope=\"col\" class=\"num\">AH cut</th><th scope=\"col\" class=\"num\">Deposit</th>"
	                      "<th scope=\"col\" class=\"num\">Profit</th><th scope=\"col\" class=\"num\">Margin</th>"
	                      "<th scope=\"col\" class=\"num\">ROI</th><th scope=\"col\" class=\"num\">Sale rate</th>"
	                      "<th scope=\"col\">Plan</th></tr></thead><tbody>");

	for (i = 0; (NULL != rows) && (i < json_array_get_length(rows)); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);
		JsonObject *sell = arw_side(row, "sell");
		JsonNode *net = json_object_get_member(row, "net");
		gboolean pays;
		gboolean blanked;

		blanked = json_object_has_member(row, "missing") &&
		          (json_array_get_length(json_object_get_array_member(row, "missing")) > 0);

		g_string_append(html, "<tr><td class=\"row-head\"");
		mdw_sort_text(html, venture_json_object_get_string(row, "title", ""));
		g_string_append(html, ">");

		if ((NULL != venture_json_object_get_string(row, "instrument_key", NULL)) &&
		    (venture_json_object_get_int(row, "data_source_id", 0) > 0))
		{
			g_autofree gchar *path = venture_marketdata_instrument_path(
				venture_json_object_get_int(row, "data_source_id", 0),
				venture_json_object_get_string(row, "instrument_key", NULL),
				(NULL != sell) ? venture_json_object_get_string(sell, "venue_key", NULL) : NULL);

			g_string_append(html, "<a href=\"");
			venture_html_escape_append(html, path);
			g_string_append(html, "\">");
			venture_html_escape_append(html, venture_json_object_get_string(row, "title", ""));
			g_string_append(html, "</a>");
		}
		else
			venture_html_escape_append(html, venture_json_object_get_string(row, "title", ""));

		arw_craft_inputs(self, html, row);
		arw_missing(html, row);
		arw_warnings(html, row);
		g_string_append(html, "</td><td>");

		if (NULL != venture_json_object_get_string(row, "recipe_category", NULL))
			venture_html_escape_append(html, venture_json_object_get_string(row, "recipe_category", ""));
		else
			mdw_dash(html);

		/* The reagents' cost, aged by the oldest of their prices, as
		 * Deals ages a buy. */
		g_string_append(html, "</td><td class=\"num");
		g_string_append(html, mdw_stale(row, "buy_") ? " is-stale\"" : "\"");
		mdw_sort_value(html, row, "cost");
		g_string_append(html, ">");
		mdw_money(html, row, "cost");
		g_string_append(html, "<br>");
		mdw_age(self, html, row, "buy_");
		g_string_append(html, "</td>");
		arw_venue(self, html, row, "sell");

		arw_money_cell(html, sell, "unit_price");
		arw_money_cell(html, row, "gross");
		arw_money_cell(html, sell, "fees");
		arw_money_cell(html, sell, "deposit");
		arw_money_cell(html, row, "net");
		g_string_append(html, "<td class=\"num\"");
		mdw_sort_value(html, row, "margin");
		g_string_append(html, ">");
		mdw_percent(html, row, "margin", 100.0);
		g_string_append(html, "</td><td class=\"num\"");
		mdw_sort_value(html, row, "roi");
		g_string_append(html, ">");
		mdw_percent(html, row, "roi", 100.0);
		g_string_append(html, "</td><td class=\"num\"");
		mdw_sort_value(html, row, "sale_rate");
		g_string_append(html, ">");
		mdw_percent(html, row, "sale_rate", 100.0);
		g_string_append(html, "</td><td>");

		/* Only a craft that pays can be planned: a plan is re-asked
		 * with losses left out, so a losing one would be refused. */
		pays = (NULL != net) && JSON_NODE_HOLDS_OBJECT(net) &&
		       (json_object_get_int_member_with_default(json_node_get_object(net), "amount", 0) > 0);

		if (can_plan && !blanked && pays)
			arw_plan_form(html, options, row, "/arbitrage/plan/add", "Add to plan");
		else
			mdw_dash(html);

		g_string_append(html, "</td></tr>");
	}

	if ((NULL == rows) || (0 == json_array_get_length(rows)))
		g_string_append(html, "<tr><td colspan=\"13\" class=\"muted\">No recipe to price. Import your "
		                      "professions' recipes (see Crafting in the docs), or widen the question.</td></tr>");

	g_string_append(html, "</tbody></table></div></div>");
	mdw_attribution(html, root);
}

static HtmxResponse *
venture_arbitrage_web_crafting(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	static const gchar *const bases[] = { "min", "market", "sale_avg", "region_median", "bid", NULL };
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) asked = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GString) html = NULL;
	JsonObject *root;
	JsonObject *question;
	gboolean api;
	gboolean can_plan;
	HtmxResponse *gate;
	gint64 organization_id;

	(void)params;

	api = mdw_is_api(request);
	gate = arw_gate(self, request, api, VENTURE_USER_ROLE_VIEWER, VENTURE_TYPE_DATA_SOURCE);

	if (NULL != gate)
		return gate;

	if (!venture_web_request_organization(self, request, NULL, &organization_id, &error))
		return venture_web_error_response(error);

	asked = arw_options(htmx_request_get_query_params(request));
	json_object_remove_member(asked, "organization_id");
	answer = venture_arbitrage_crafting(self->context, organization_id, asked, &error);

	if (NULL == answer)
		return venture_web_error_response(error);

	if (api)
		return venture_web_json_response(answer, 200);

	root = json_node_get_object(answer);
	question = json_object_get_object_member(root, "question");
	principal = venture_auth_authenticate(self->auth, request);
	can_plan = venture_web_require_for_type(self, principal, VENTURE_TYPE_ARBITRAGE_TRADE,
	                                        VENTURE_USER_ROLE_EDITOR, NULL);

	html = g_string_new(NULL);
	mdw_head(html, "Trading", "Crafting",
	         "What every recipe makes on the latest prices: reagents where they are cheapest (commodities "
	         "from the region's market), the craft sold where it fetches most, after the auction house's "
	         "cut and deposit.");
	g_string_append(html, "<p class=\"muted\"><a href=\"/arbitrage/plan\">Flip planner</a> \xc2\xb7 "
	                      "<a href=\"/arbitrage?strategy=transform\">Arbitrage</a> \xc2\xb7 "
	                      "<a href=\"/e/recipe\">Recipes</a></p>");

	g_string_append(html, "<div class=\"card\"><form method=\"get\" action=\"/arbitrage/crafting\" "
	                      "class=\"form-grid arbitrage-filters\" role=\"search\" aria-label=\"Crafting filters\">");
	arw_recipe_category_picker(html, root, venture_json_object_get_int(question, "recipe_category_id", 0));
	arw_realm_picker(html, root, "Buy reagents at", "buy_realm",
	                 venture_json_object_get_string(question, "buy_realm", NULL), "the cheapest realm");
	arw_realm_picker(html, root, "Sell at", "sell_realm",
	                 venture_json_object_get_string(question, "sell_realm", NULL), "the best realm");
	mdw_venue_group_picker(html, root, venture_json_object_get_string(question, "venue_group", NULL));
	arw_input(html, question, "Batches", "units", "1");
	arw_select(html, question, "Sell at price", "sell_basis", bases, "lowest listing");
	arw_input(html, question, "Newest within hours", "max_age_hours", "any age");
	arw_input(html, question, "Share of sales %", "share", "100");

	if (venture_json_object_get_int(question, "data_source_id", 0) > 0)
		g_string_append_printf(html, "<input type=\"hidden\" name=\"data_source_id\" value=\"%" G_GINT64_FORMAT "\">",
		                       venture_json_object_get_int(question, "data_source_id", 0));

	if (venture_json_object_get_int(question, "recipe_id", 0) > 0)
		g_string_append_printf(html, "<input type=\"hidden\" name=\"recipe_id\" value=\"%" G_GINT64_FORMAT "\">",
		                       venture_json_object_get_int(question, "recipe_id", 0));

	g_string_append(html, "<button class=\"btn\" type=\"submit\">Price</button></form></div>");
	mdw_notes(html, root);
	arw_craft_rows(self, html, root, can_plan);

	if (json_object_get_boolean_member(root, "truncated"))
		g_string_append(html, "<p class=\"muted market-more\">More recipes were priced than are shown; "
		                      "narrow to a recipe category.</p>");

	return venture_web_html_response(venture_web_page(self, request, "/arbitrage/crafting", "Crafting",
	                                                  html->str), 200);
}

/* --- /arbitrage/plan ---------------------------------------------------------- */

/* One venue's list: a heading, then the lines, its totals in the foot so
 * a sort never moves them among the lines they add up. */
static void
arw_plan_venues(
	GString		*html,
	JsonObject	*root,
	const gchar	*section,
	const gchar	*title,
	const gchar	*price_label,
	const gchar	*fees_label,
	const gchar	*empty
){
	JsonArray *venues;
	guint i;
	guint j;

	venues = json_object_has_member(root, section) ? json_object_get_array_member(root, section) : NULL;

	if ((NULL == venues) || (0 == json_array_get_length(venues)))
	{
		if (NULL != empty)
		{
			g_string_append(html, "<div class=\"card\"><div class=\"card-head\"><h2>");
			venture_html_escape_append(html, title);
			g_string_append(html, "</h2></div><div class=\"card-body\"><p class=\"muted\">");
			venture_html_escape_append(html, empty);
			g_string_append(html, "</p></div></div>");
		}

		return;
	}

	g_string_append(html, "<div class=\"card plan-section\"><div class=\"card-head\"><h2>");
	venture_html_escape_append(html, title);
	g_string_append(html, "</h2></div>");

	for (i = 0; i < json_array_get_length(venues); i++)
	{
		JsonObject *venue = json_array_get_object_element(venues, i);
		JsonArray *lines = json_object_get_array_member(venue, "lines");
		JsonArray *totals = json_object_get_array_member(venue, "totals");

		g_string_append(html, "<div class=\"card-body plan-venue\"><h3>");
		venture_html_escape_append(html, venture_json_object_get_string(venue, "venue_name", ""));
		g_string_append(html, "</h3></div><div class=\"table-wrap\"><table class=\"data plan-table\"><thead><tr>"
		                      "<th scope=\"col\">Item</th><th scope=\"col\" class=\"num\">Quantity</th>"
		                      "<th scope=\"col\" class=\"num\">");
		venture_html_escape_append(html, price_label);
		g_string_append(html, "</th><th scope=\"col\" class=\"num\">Amount</th><th scope=\"col\" class=\"num\">");
		venture_html_escape_append(html, fees_label);
		g_string_append(html, "</th><th scope=\"col\">Trades</th></tr></thead><tbody>");

		for (j = 0; (NULL != lines) && (j < json_array_get_length(lines)); j++)
		{
			JsonObject *line = json_array_get_object_element(lines, j);
			JsonArray *trades = json_object_get_array_member(line, "trades");
			guint k;

			g_string_append(html, "<tr><td class=\"row-head\">");
			venture_html_escape_append(html, venture_json_object_get_string(line, "instrument_name", ""));
			g_string_append(html, "</td><td class=\"num\">");
			mdw_int(html, line, "quantity");
			g_string_append(html, "</td>");
			arw_money_cell(html, line, "unit_price");
			arw_money_cell(html, line, "amount");
			arw_money_cell(html, line, "fees");
			g_string_append(html, "<td>");

			for (k = 0; (NULL != trades) && (k < json_array_get_length(trades)); k++)
			{
				gint64 id = json_array_get_int_element(trades, k);

				g_string_append_printf(html, "%s<a href=\"/e/arbitrage_trade/%" G_GINT64_FORMAT "\">#%"
				                       G_GINT64_FORMAT "</a>", (k > 0) ? " " : "", id, id);
			}

			g_string_append(html, "</td></tr>");
		}

		g_string_append(html, "</tbody><tfoot>");

		for (j = 0; (NULL != totals) && (j < json_array_get_length(totals)); j++)
		{
			JsonObject *total = json_array_get_object_element(totals, j);

			g_string_append(html, "<tr><td colspan=\"3\">Total");

			if (json_array_get_length(totals) > 1)
			{
				g_string_append(html, " in ");
				venture_html_escape_append(html, venture_json_object_get_string(total, "currency", ""));
			}

			g_string_append(html, "</td><td class=\"num\" colspan=\"2\">");
			mdw_money(html, total, "amount");
			g_string_append(html, "</td><td></td></tr>");
		}

		g_string_append(html, "</tfoot></table></div>");
	}

	g_string_append(html, "</div>");
}

static void
arw_plan_totals(
	GString		*html,
	JsonObject	*root
){
	JsonArray *totals;
	guint i;

	totals = json_object_has_member(root, "totals") ? json_object_get_array_member(root, "totals") : NULL;

	for (i = 0; (NULL != totals) && (i < json_array_get_length(totals)); i++)
	{
		JsonObject *total = json_array_get_object_element(totals, i);
		g_autofree gchar *trades = g_strdup_printf("%" G_GINT64_FORMAT,
		                                           venture_json_object_get_int(total, "trades", 0));

		g_string_append(html, "<div class=\"stat-row market-stats plan-totals\">");
		mdw_stat(html, "Trades", trades);
		mdw_stat_of(html, "Gold outlay", mdw_money, total, "outlay");
		mdw_stat_of(html, "Sales", mdw_money, total, "gross");
		mdw_stat_of(html, "AH cut", mdw_money, total, "cut");
		mdw_stat_of(html, "Deposits to post", mdw_money, total, "deposit");
		mdw_stat_of(html, "Expected lost deposits", mdw_money, total, "listing_loss");
		mdw_stat_of(html, "Revenue after fees", mdw_money, total, "revenue");
		mdw_stat_of(html, "Expected profit", mdw_money, total, "profit");
		g_string_append(html, "</div>");
	}
}

static void
arw_plan_trades(
	GString		*html,
	JsonObject	*root,
	gboolean	 can_remove
){
	JsonArray *trades;
	gboolean repriced;
	guint i;

	trades = json_object_get_array_member(root, "trades");
	repriced = json_object_get_boolean_member(root, "repriced");

	g_string_append(html, "<div class=\"card\"><div class=\"card-head\"><h2>Planned trades</h2></div>"
	                      "<div class=\"table-wrap\"><table class=\"data plan-trades\"><thead><tr>"
	                      "<th scope=\"col\">Trade</th><th scope=\"col\">Strategy</th>"
	                      "<th scope=\"col\" class=\"num\">Planned profit</th>");

	if (repriced)
		g_string_append(html, "<th scope=\"col\" class=\"num\">Now</th><th scope=\"col\">Standing</th>");

	g_string_append(html, "<th scope=\"col\">Remove</th></tr></thead><tbody>");

	for (i = 0; (NULL != trades) && (i < json_array_get_length(trades)); i++)
	{
		JsonObject *trade = json_array_get_object_element(trades, i);
		JsonObject *expected = json_object_has_member(trade, "expected")
			? json_object_get_object_member(trade, "expected") : NULL;
		JsonObject *reprice = json_object_has_member(trade, "reprice")
			? json_object_get_object_member(trade, "reprice") : NULL;
		gint64 id = venture_json_object_get_int(trade, "id", 0);
		JsonArray *profit = ((NULL != expected) && json_object_has_member(expected, "profit") &&
		                     JSON_NODE_HOLDS_ARRAY(json_object_get_member(expected, "profit")))
			? json_object_get_array_member(expected, "profit") : NULL;

		g_string_append_printf(html, "<tr><td class=\"row-head\"><a href=\"/e/arbitrage_trade/%" G_GINT64_FORMAT
		                       "\">", id);
		venture_html_escape_append(html, venture_json_object_get_string(trade, "name", ""));
		g_string_append(html, "</a></td><td>");
		venture_html_escape_append(html, venture_json_object_get_string(trade, "strategy", ""));
		g_string_append(html, "</td><td class=\"num\">");

		if ((NULL != profit) && (json_array_get_length(profit) > 0))
		{
			g_autoptr(VentureMoney) money = venture_money_from_string(json_array_get_string_element(profit, 0),
			                                                          NULL, NULL);
			g_autofree gchar *text = (NULL != money) ? venture_money_to_display_string(money, TRUE) : NULL;

			venture_html_escape_append(html, (NULL != text) ? text : json_array_get_string_element(profit, 0));
		}
		else
			mdw_dash(html);

		g_string_append(html, "</td>");

		if (repriced)
		{
			const gchar *state = (NULL != reprice) ? venture_json_object_get_string(reprice, "state", "") : "";
			const gchar *tone = (0 == g_strcmp0(state, "ok")) ? "good"
			                  : (0 == g_strcmp0(state, "stale")) ? "meh" : "bad";

			arw_money_cell(html, reprice, "net");
			g_string_append(html, "<td>");

			if (NULL != reprice)
			{
				mdw_chip_open(html, tone);
				venture_html_escape_append(html, state);
				g_string_append(html, "</span> <span class=\"muted\">");
				venture_html_escape_append(html, venture_json_object_get_string(reprice, "message", ""));
				g_string_append(html, "</span>");
			}
			else
				g_string_append(html, "<span class=\"muted\">not priced again</span>");

			g_string_append(html, "</td>");
		}

		g_string_append(html, "<td>");

		if (can_remove)
			g_string_append_printf(html, "<form method=\"post\" action=\"/arbitrage/plan/remove\">"
			                       "<input type=\"hidden\" name=\"trade_id\" value=\"%" G_GINT64_FORMAT "\">"
			                       "<button class=\"btn\" type=\"submit\">Remove</button></form>", id);
		else
			mdw_dash(html);

		g_string_append(html, "</td></tr>");
	}

	if ((NULL == trades) || (0 == json_array_get_length(trades)))
		g_string_append_printf(html, "<tr><td colspan=\"%d\" class=\"muted\">Nothing is planned.</td></tr>",
		                       repriced ? 6 : 4);

	g_string_append(html, "</tbody></table></div></div>");
}

/* The export formats the plan offers: its own CSV, and every registered
 * format that is not the scan's (a game addon's shopping import). */
static gboolean
arw_plan_format(
	VentureExportFormatRegistry	*formats,
	const gchar			*format
){
	if (0 == g_strcmp0(format, "csv"))
		return TRUE;

	return (0 != g_strcmp0(format, "shopping_list")) && venture_export_format_registry_has(formats, format);
}

static HtmxResponse *
venture_arbitrage_web_plan(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	VentureWebServer *self = data;
	VentureExportFormatRegistry *formats;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GString) html = NULL;
	g_auto(GStrv) names = NULL;
	JsonObject *root;
	gboolean api;
	gboolean reprice;
	gboolean can_remove;
	HtmxResponse *gate;
	gint64 organization_id;
	guint i;

	(void)params;

	api = mdw_is_api(request);
	gate = arw_gate(self, request, api, VENTURE_USER_ROLE_VIEWER, VENTURE_TYPE_ARBITRAGE_TRADE);

	if (NULL != gate)
		return gate;

	if (!venture_web_request_organization(self, request, NULL, &organization_id, &error))
		return venture_web_error_response(error);

	reprice = mdw_flag(request, "reprice");
	answer = venture_arbitrage_planner(self->context, organization_id, reprice, &error);

	if (NULL == answer)
		return venture_web_error_response(error);

	if (api)
		return venture_web_json_response(answer, 200);

	root = json_node_get_object(answer);
	principal = venture_auth_authenticate(self->auth, request);
	can_remove = venture_web_require_for_type(self, principal, VENTURE_TYPE_ARBITRAGE_TRADE,
	                                          VENTURE_USER_ROLE_EDITOR, NULL);
	formats = venture_context_get_export_formats(self->context);
	names = venture_export_format_registry_dup_names(formats);

	html = g_string_new(NULL);
	mdw_head(html, "Trading", "Flip planner",
	         "Everything planned, as a shopping list: what to buy at each realm and the most to pay, then "
	         "what to post at each and at what price, with the gold it needs and what it should bring back "
	         "after the auction house's cut and deposits.");
	g_string_append(html, "<p class=\"muted\"><a href=\"/market/deals\">Deals</a> \xc2\xb7 "
	                      "<a href=\"/arbitrage/crafting\">Crafting</a> \xc2\xb7 <a href=\"/arbitrage\">Arbitrage</a>"
	                      " \xc2\xb7 <a href=\"/e/arbitrage_trade\">Trades</a></p>");
	g_string_append(html, "<div class=\"card\"><div class=\"card-body plan-actions\">"
	                      "<a class=\"btn\" href=\"/arbitrage/plan?reprice=1\">Price again</a> "
	                      "<span class=\"muted\">Export:</span> <a href=\"/arbitrage/plan/export?format=csv\">CSV</a>");

	for (i = 0; NULL != names[i]; i++)
	{
		if ((0 == g_strcmp0(names[i], "csv")) || !arw_plan_format(formats, names[i]))
			continue;

		g_string_append_printf(html, " \xc2\xb7 <a href=\"/arbitrage/plan/export?format=%s\">", names[i]);
		venture_html_escape_append(html, venture_export_format_registry_get_label(formats, names[i]));
		g_string_append(html, "</a>");
	}

	g_string_append(html, "</div></div>");
	mdw_notes(html, root);
	arw_plan_totals(html, root);
	arw_plan_venues(html, root, "buy", "Buy", "Most a unit", "Fees", "Nothing to buy.");
	arw_plan_venues(html, root, "fees", "Moving lots", "Each", "Fees", NULL);
	arw_plan_venues(html, root, "sell", "Post", "Post at", "AH cut", "Nothing to post.");
	arw_plan_trades(html, root, can_remove);

	return venture_web_html_response(venture_web_page(self, request, "/arbitrage/plan", "Flip planner",
	                                                  html->str), 200);
}

static HtmxResponse *
venture_arbitrage_web_plan_export(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	VentureWebServer *self = data;
	VentureExportFormatRegistry *formats;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GBytes) bytes = NULL;
	g_autofree gchar *disposition = NULL;
	const gchar *format;
	HtmxResponse *gate;
	HtmxResponse *response;
	gint64 organization_id;

	(void)params;

	gate = arw_gate(self, request, mdw_is_api(request), VENTURE_USER_ROLE_VIEWER, VENTURE_TYPE_ARBITRAGE_TRADE);

	if (NULL != gate)
		return gate;

	if (!venture_web_request_organization(self, request, NULL, &organization_id, &error))
		return venture_web_error_response(error);

	formats = venture_context_get_export_formats(self->context);
	format = (NULL != mdw_param(request, "format")) ? mdw_param(request, "format") : "csv";

	if (!arw_plan_format(formats, format))
	{
		g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "The plan exports as csv, or a format a plugin registered (tsm); not %s", format);
		return venture_web_error_response(error);
	}

	answer = venture_arbitrage_planner(self->context, organization_id, FALSE, &error);

	if (NULL == answer)
		return venture_web_error_response(error);

	if (0 == g_strcmp0(format, "csv"))
		bytes = venture_arbitrage_planner_csv(json_node_get_object(answer));
	else
	{
		g_autoptr(JsonArray) rows = venture_arbitrage_planner_export_rows(json_node_get_object(answer));
		g_autoptr(JsonObject) options = json_object_new();

		bytes = venture_export_format_registry_export(formats, format, rows, options, &error);

		if (NULL == bytes)
			return venture_web_error_response(error);
	}

	response = htmx_response_new();
	htmx_response_set_bytes(response, bytes);
	htmx_response_set_content_type(response, (0 == g_strcmp0(format, "csv"))
	                               ? "text/csv; charset=utf-8"
	                               : venture_export_format_registry_get_content_type(formats, format));
	htmx_response_set_status(response, 200);
	disposition = g_strdup_printf("attachment; filename=\"flip-plan-%s.%s\"", format,
	                              (0 == g_strcmp0(format, "csv")) ? "csv"
	                              : venture_export_format_registry_get_extension(formats, format));
	htmx_response_add_header(response, "Content-Disposition", disposition);
	htmx_response_add_header(response, "Cache-Control", "no-store");

	return response;
}

static HtmxResponse *
venture_arbitrage_web_plan_remove(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) body = NULL;
	const gchar *text;
	VentureActor actor;
	gboolean api;
	HtmxResponse *gate;
	gint64 organization_id;
	gint64 trade_id;

	(void)params;

	api = mdw_is_api(request);
	gate = arw_gate(self, request, api, VENTURE_USER_ROLE_EDITOR, VENTURE_TYPE_ARBITRAGE_TRADE);

	if (NULL != gate)
		return gate;

	/* A form from the page, or JSON {"trade_id": N} from a client. */
	if (g_str_has_prefix((NULL != htmx_request_get_content_type(request))
	                     ? htmx_request_get_content_type(request) : "",
	                     "application/x-www-form-urlencoded"))
	{
		text = htmx_request_get_form_value(request, "trade_id");
		trade_id = (NULL != text) ? g_ascii_strtoll(text, NULL, 10) : 0;
	}
	else
	{
		body = htmx_request_get_json(request, NULL);
		trade_id = ((NULL != body) && JSON_NODE_HOLDS_OBJECT(body))
			? venture_json_object_get_int(json_node_get_object(body), "trade_id", 0) : 0;
	}

	if (!venture_web_request_organization(self, request, ((NULL != body) && JSON_NODE_HOLDS_OBJECT(body))
	                                      ? json_node_get_object(body) : NULL, &organization_id, &error))
		return venture_web_error_response(error);

	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);

	if (!venture_arbitrage_planner_remove(self->context, organization_id, trade_id, &actor, &error))
		return venture_web_error_response(error);

	if (api)
	{
		g_autoptr(JsonNode) done = json_node_new(JSON_NODE_OBJECT);
		JsonObject *object = json_object_new();

		json_object_set_int_member(object, "removed", trade_id);
		json_node_take_object(done, object);

		return venture_web_json_response(done, 200);
	}

	return venture_web_redirect_to("/arbitrage/plan");
}

/* --- Routes ---------------------------------------------------------------------- */

static void
venture_arbitrage_pages_register(VentureWebServer *self)
{
	static const struct
	{
		HtmxMethod		 method;
		const gchar		*path;
		HtmxRouteCallback	 handler;
	} routes[] = {
		{ HTMX_METHOD_GET, "/arbitrage", venture_arbitrage_web_scan },
		{ HTMX_METHOD_POST, "/arbitrage/record", venture_arbitrage_web_record },
		{ HTMX_METHOD_GET, "/arbitrage/export", venture_arbitrage_web_export },
		{ HTMX_METHOD_GET, "/arbitrage/calc", venture_arbitrage_web_calc },
		{ HTMX_METHOD_GET, "/arbitrage/crafting", venture_arbitrage_web_crafting },
		{ HTMX_METHOD_GET, "/arbitrage/plan", venture_arbitrage_web_plan },
		{ HTMX_METHOD_POST, "/arbitrage/plan/add", venture_arbitrage_web_record },
		{ HTMX_METHOD_POST, "/arbitrage/plan/remove", venture_arbitrage_web_plan_remove },
		{ HTMX_METHOD_GET, "/arbitrage/plan/export", venture_arbitrage_web_plan_export },
		/* The twins: /api/v1/arbitrage is not a record type. */
		{ HTMX_METHOD_GET, "/api/v1/arbitrage/scan", venture_arbitrage_web_scan },
		{ HTMX_METHOD_POST, "/api/v1/arbitrage/record", venture_arbitrage_web_record },
		{ HTMX_METHOD_GET, "/api/v1/arbitrage/export", venture_arbitrage_web_export },
		{ HTMX_METHOD_GET, "/api/v1/arbitrage/calc", venture_arbitrage_web_calc },
		{ HTMX_METHOD_GET, "/api/v1/arbitrage/registries", venture_arbitrage_web_registries },
		{ HTMX_METHOD_GET, "/api/v1/arbitrage/crafting", venture_arbitrage_web_crafting },
		{ HTMX_METHOD_GET, "/api/v1/arbitrage/plan", venture_arbitrage_web_plan },
		{ HTMX_METHOD_POST, "/api/v1/arbitrage/plan/remove", venture_arbitrage_web_plan_remove },
		{ HTMX_METHOD_GET, "/api/v1/arbitrage/plan/export", venture_arbitrage_web_plan_export },
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(routes); i++)
		venture_web_server_add_classified_route(self, routes[i].method, routes[i].path,
		                                        VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE,
		                                        routes[i].handler, self);
}
