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
 * and the role its records ask of the caller.
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

		g_string_append_printf(html, "<input type=\"hidden\" name=\"%s\" value=\"",
		                       (const gchar *)member->data);
		venture_html_escape_append(html, text);
		g_string_append(html, "\">");
	}

	members = (NULL != override) ? json_object_get_members(override) : NULL;

	for (member = members; NULL != member; member = member->next)
	{
		g_autofree gchar *text = arw_node_text(json_object_get_member(override, member->data));

		if (venture_string_is_empty(text))
			continue;

		g_string_append_printf(html, "<input type=\"hidden\" name=\"%s\" value=\"",
		                       (const gchar *)member->data);
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
	HtmxRequest		*request,
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
	gint64 organization_id;
	gint64 chosen;
	guint i;

	organization_id = venture_web_active_organization(self, request);
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
	HtmxRequest		*request,
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
	venture_query_set_organization(query, venture_web_active_organization(self, request));
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
	arw_input(html, options, "Buy at", "buy_venues", "venue keys, comma separated");
	arw_input(html, options, "Sell at", "sell_venues", "venue keys, comma separated");
	arw_input(html, options, "Group", "group_key", "e.g. eu");
	arw_input(html, options, "Category", "category_path", "e.g. Trade Goods/Herb");
	arw_input(html, options, "Kind", "kind", "e.g. item");
	arw_input(html, options, "Instrument", "instrument", "one key");

	if (0 == g_strcmp0(strategy, "transform"))
		arw_input(html, options, "Recipe", "recipe_id", "a recipe's id; every active one");

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

static void
arw_venue(
	GString		*html,
	JsonObject	*row,
	const gchar	*side
){
	JsonObject *object;

	object = json_object_has_member(row, side) ? json_object_get_object_member(row, side) : NULL;

	if (NULL == object)
	{
		mdw_dash(html);
		return;
	}

	mdw_name(html, object, "venue_name", "venue_key");
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
		g_string_append(html, "</td><td>");
		arw_venue(html, row, "buy");
		g_string_append(html, "</td><td>");
		arw_venue(html, row, "sell");
		g_string_append(html, "</td><td class=\"num\">");
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
		}

		g_string_append(html, "</td></tr>");
	}

	if ((NULL == rows) || (0 == json_array_get_length(rows)))
		g_string_append(html, "<tr><td colspan=\"11\" class=\"muted\">Nothing passes the filters right now.</td></tr>");

	g_string_append(html, "</tbody></table></div></div>");
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

	(void)params;

	api = mdw_is_api(request);
	gate = arw_gate(self, request, api, VENTURE_USER_ROLE_VIEWER, VENTURE_TYPE_DATA_SOURCE);

	if (NULL != gate)
		return gate;

	asked = arw_options(htmx_request_get_query_params(request));
	answer = venture_arbitrage_scan_run(self->context, venture_web_active_organization(self, request),
	                                    asked, &error);

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
	arw_filters(self, request, html, options);
	arw_presets(self, request, html, options);
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
	HtmxResponse *gate;
	HtmxResponse *redirect;

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

	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	trade = venture_arbitrage_record_opportunity(self->context, venture_web_active_organization(self, request),
	                                             options, key, &actor, principal->role, &error);

	if (NULL == trade)
		return venture_web_error_response(error);

	if (api)
		return venture_web_json_response(venture_serializable_to_json(VENTURE_SERIALIZABLE(trade), FALSE), 201);

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
	g_autofree gchar *body = NULL;
	const gchar *format;
	HtmxResponse *gate;
	HtmxResponse *response;

	(void)params;

	gate = arw_gate(self, request, mdw_is_api(request), VENTURE_USER_ROLE_VIEWER, VENTURE_TYPE_DATA_SOURCE);

	if (NULL != gate)
		return gate;

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
	answer = venture_arbitrage_scan_run(self->context, venture_web_active_organization(self, request),
	                                    asked, &error);

	if (NULL == answer)
		return venture_web_error_response(error);

	bytes = venture_export_format_registry_export(formats, format,
		json_object_get_array_member(json_node_get_object(answer), "rows"),
		json_object_get_object_member(json_node_get_object(answer), "options"), &error);

	if (NULL == bytes)
		return venture_web_error_response(error);

	body = g_strndup(g_bytes_get_data(bytes, NULL), g_bytes_get_size(bytes));
	response = htmx_response_new_with_content(body);
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
		           answer, "guaranteed", 0);
		arw_figure(html, "Ideal profit", answer, "profit", 0);
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
	JsonNode *node;
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
		venture_web_goals_input(request, content, "Recipe", "recipe_id", "every active recipe");

	venture_web_goals_input(request, content, "Data source", "data_source_id", "id; every source");
	venture_web_goals_input(request, content, "Buy at", "buy_venues", "venue keys, comma separated");
	venture_web_goals_input(request, content, "Sell at", "sell_venues", "venue keys, comma separated");
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
		/* The twins: /api/v1/arbitrage is not a record type. */
		{ HTMX_METHOD_GET, "/api/v1/arbitrage/scan", venture_arbitrage_web_scan },
		{ HTMX_METHOD_POST, "/api/v1/arbitrage/record", venture_arbitrage_web_record },
		{ HTMX_METHOD_GET, "/api/v1/arbitrage/export", venture_arbitrage_web_export },
		{ HTMX_METHOD_GET, "/api/v1/arbitrage/calc", venture_arbitrage_web_calc },
		{ HTMX_METHOD_GET, "/api/v1/arbitrage/registries", venture_arbitrage_web_registries },
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(routes); i++)
		venture_web_server_add_classified_route(self, routes[i].method, routes[i].path,
		                                        VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE,
		                                        routes[i].handler, self);
}
