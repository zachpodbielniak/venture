/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * venture-marketdata-pages.inc - The Trading pages under /market and their
 * /api/v1/market twins
 *
 * Included by venture-web-server.c after venture-marketdata-web.inc. Every
 * page here is a renderer: it turns the request into the question struct,
 * asks the one implementation in venture-marketdata-browse.c, and draws
 * the JSON it answers. The /api/v1/market twin of each page parses the
 * same request the same way and returns that JSON untouched, so the page
 * and the API cannot answer differently. Nothing here reads a store.
 *
 * Every handler checks its own session or token, its role and the
 * marketdata module, and reads one organization: a page the sidebar's
 * pick, an API call its organization_id or else the active one
 * (venture_web_request_organization()). Writes are
 * an editor's: promoting an instrument creates a record, and the watch
 * and alert buttons promote before handing over to the generic forms,
 * prefilled through the query string, so no form here is hand-written for
 * a record type.
 */

/* --- Reading the request --------------------------------------------------- */

/* A query parameter, or NULL when absent or blank. */
static const gchar *
mdw_param(
	HtmxRequest	*request,
	const gchar	*name
){
	const gchar *value;

	value = htmx_request_get_query_param(request, name);

	return venture_string_is_empty(value) ? NULL : value;
}

/* A whole-number parameter in [0, @max]; absent is @fallback. */
static gboolean
mdw_number(
	HtmxRequest	 *request,
	const gchar	 *name,
	gint64		  fallback,
	gint64		  max,
	gint64		 *out,
	GError		**error
){
	const gchar *text;

	*out = fallback;
	text = mdw_param(request, name);

	if (NULL == text)
		return TRUE;

	if (!g_ascii_string_to_signed(text, 10, 0, max, out, NULL))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		            "%s must be a whole number from 0 to %" G_GINT64_FORMAT, name, max);
		return FALSE;
	}

	return TRUE;
}

static gboolean
mdw_flag(
	HtmxRequest	*request,
	const gchar	*name
){
	const gchar *text;

	text = mdw_param(request, name);

	return (NULL != text) &&
	       ((0 == g_strcmp0(text, "1")) || (0 == g_ascii_strcasecmp(text, "true")) ||
	        (0 == g_ascii_strcasecmp(text, "on")));
}

/* The source id a path names: a positive number, or NOT_FOUND. */
static gboolean
mdw_path_id(
	GHashTable	 *params,
	const gchar	 *name,
	gint64		 *out,
	GError		**error
){
	const gchar *text;

	text = g_hash_table_lookup(params, name);

	if ((NULL == text) || !g_ascii_string_to_signed(text, 10, 1, G_MAXINT64, out, NULL))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "There is no such %s", name);
		return FALSE;
	}

	return TRUE;
}

/*
 * The key a path names: everything after "/i/<source>/". The server hands
 * a handler the path with its escapes already undone, so a key holding a
 * slash ("evil/key", escaped as %2F in every link) arrives as two
 * segments; the route therefore matches the rest of the path, and the key
 * is read from it here rather than from a one-segment parameter. Undoing
 * the escapes again would read a key holding "%" as something else.
 */
static gchar *
mdw_path_key(
	HtmxRequest	 *request,
	GError		**error
){
	const gchar *path;
	const gchar *at;
	const gchar *key;

	path = htmx_request_get_path(request);
	at = (NULL != path) ? strstr(path, "/market/i/") : NULL;
	key = (NULL != at) ? strchr(at + strlen("/market/i/"), '/') : NULL;

	if ((NULL == key) || ('\0' == key[1]) || !g_utf8_validate(key + 1, -1, NULL))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "That is not an instrument key");
		return NULL;
	}

	return g_strdup(key + 1);
}

/*
 * The gates every page shares: a session (or token), the module, and the
 * role the record type the page reads asks of this caller -- the same
 * judgement the generic routes make (tenant authority, a support session,
 * owner-only types), so a page shows nothing its records' own routes
 * would not.
 */
static HtmxResponse *
mdw_gate(
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

	gate = api ? venture_web_require_module_api(self, "marketdata")
	           : venture_web_require_module_ui(self, request, "marketdata");

	if (NULL != gate)
		return gate;

	principal = venture_auth_authenticate(self->auth, request);

	if (!venture_web_require_for_type(self, principal, reads, role, &error))
		return venture_web_error_response(error);

	return NULL;
}

static gboolean
mdw_is_api(HtmxRequest *request)
{
	return g_str_has_prefix(htmx_request_get_path(request), "/api/");
}

/* --- Drawing values -------------------------------------------------------- */

static void
mdw_dash(GString *html)
{
	g_string_append(html, "<span class=\"muted\">\xe2\x80\x94</span>");
}

/* A money object of an answer, read the way its currency reads. */
static void
mdw_money(
	GString		*html,
	JsonObject	*object,
	const gchar	*member
){
	g_autoptr(VentureMoney) money = NULL;
	g_autofree gchar *text = NULL;
	JsonNode *node;

	node = (NULL != object) ? json_object_get_member(object, member) : NULL;

	if ((NULL == node) || !JSON_NODE_HOLDS_OBJECT(node))
	{
		mdw_dash(html);
		return;
	}

	money = venture_money_from_json(node, NULL, NULL);

	if (NULL == money)
	{
		mdw_dash(html);
		return;
	}

	text = venture_money_to_display_string(money, TRUE);
	venture_html_escape_append(html, text);
}

/* Minor units in @currency, as money, for series figures. */
static void
mdw_minor(
	GString		*html,
	JsonObject	*object,
	const gchar	*member,
	const gchar	*currency
){
	g_autofree gchar *text = NULL;
	JsonNode *node;

	node = (NULL != object) ? json_object_get_member(object, member) : NULL;

	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node))
	{
		mdw_dash(html);
		return;
	}

	text = venture_web_chart_format_minor((gdouble)json_node_get_int(node), (gpointer)currency);
	venture_html_escape_append(html, text);
}

static void
mdw_int(
	GString		*html,
	JsonObject	*object,
	const gchar	*member
){
	JsonNode *node;

	node = (NULL != object) ? json_object_get_member(object, member) : NULL;

	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node))
	{
		mdw_dash(html);
		return;
	}

	g_string_append_printf(html, "%" G_GINT64_FORMAT, json_node_get_int(node));
}

/* A number as a percent: @scale 1 for a percent already, 100 for a share. */
static void
mdw_percent(
	GString		*html,
	JsonObject	*object,
	const gchar	*member,
	gdouble		 scale
){
	g_autofree gchar *text = NULL;
	JsonNode *node;

	node = (NULL != object) ? json_object_get_member(object, member) : NULL;

	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node))
	{
		mdw_dash(html);
		return;
	}

	text = venture_web_chart_format_percent(json_node_get_double(node) * scale, NULL);
	venture_html_escape_append(html, text);
}

static void
mdw_decimal(
	GString		*html,
	JsonObject	*object,
	const gchar	*member
){
	g_autofree gchar *text = NULL;
	JsonNode *node;

	node = (NULL != object) ? json_object_get_member(object, member) : NULL;

	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node))
	{
		mdw_dash(html);
		return;
	}

	text = venture_web_chart_format_number(json_node_get_double(node), NULL);
	venture_html_escape_append(html, text);
}

static const gchar *
mdw_text(
	JsonObject	*object,
	const gchar	*member
){
	JsonNode *node;

	node = (NULL != object) ? json_object_get_member(object, member) : NULL;

	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node) ||
	    (G_TYPE_STRING != json_node_get_value_type(node)))
		return NULL;

	return json_node_get_string(node);
}

/* An ISO time as "3 minutes ago", in a <time> with the exact moment. */
static void
mdw_when(
	GString		*html,
	JsonObject	*object,
	const gchar	*member
){
	g_autoptr(GDateTime) moment = NULL;
	g_autofree gchar *relative = NULL;
	const gchar *text;

	text = mdw_text(object, member);
	moment = (NULL != text) ? g_date_time_new_from_iso8601(text, NULL) : NULL;

	if (NULL == moment)
	{
		mdw_dash(html);
		return;
	}

	relative = venture_time_to_relative_string(moment);
	g_string_append(html, "<time datetime=\"");
	venture_html_escape_append(html, text);
	g_string_append(html, "\">");
	venture_html_escape_append(html, relative);
	g_string_append(html, "</time>");
}

/* A name, or the key when the store gave none. */
static void
mdw_name(
	GString		*html,
	JsonObject	*object,
	const gchar	*name_member,
	const gchar	*key_member
){
	const gchar *name;

	name = mdw_text(object, name_member);
	venture_html_escape_append(html, (NULL != name) ? name : mdw_text(object, key_member));
}

static void
mdw_notes(
	GString		*html,
	JsonObject	*root
){
	JsonArray *notes;
	guint i;

	notes = json_object_get_array_member(root, "notes");

	for (i = 0; (NULL != notes) && (i < json_array_get_length(notes)); i++)
	{
		g_string_append(html, "<div class=\"notice info market-note\">");
		venture_html_escape_append(html, json_array_get_string_element(notes, i));
		g_string_append(html, "</div>");
	}
}

/*
 * The answer's attribution lines, under the data they name. Every page
 * that shows a source's data calls this; the answer decides which lines
 * (none when no source shown declares one).
 */
static void
mdw_attribution(
	GString		*html,
	JsonObject	*root
){
	venture_marketdata_attribution_append_answer_html(html, root);
}

static void
mdw_head(
	GString		*html,
	const gchar	*eyebrow,
	const gchar	*title,
	const gchar	*subtitle
){
	g_string_append(html, "<div class=\"page-head\"><div class=\"page-title\">");

	if (NULL != eyebrow)
	{
		g_string_append(html, "<div class=\"eyebrow\">");
		venture_html_escape_append(html, eyebrow);
		g_string_append(html, "</div>");
	}

	g_string_append(html, "<h1>");
	venture_html_escape_append(html, title);
	g_string_append(html, "</h1>");

	if (NULL != subtitle)
	{
		g_string_append(html, "<p class=\"muted\">");
		venture_html_escape_append(html, subtitle);
		g_string_append(html, "</p>");
	}

	g_string_append(html, "</div></div>");
}

/* A <select> of the answer's sources, the chosen one selected. */
static void
mdw_source_picker(
	GString		*html,
	JsonObject	*root,
	gint64		 chosen,
	gboolean	 any
){
	JsonArray *sources;
	guint i;

	sources = json_object_get_array_member(root, "sources");
	g_string_append(html, "<div class=\"field\"><label>Data source<select name=\"source\">");

	if (any)
		g_string_append(html, "<option value=\"\">every source</option>");

	for (i = 0; (NULL != sources) && (i < json_array_get_length(sources)); i++)
	{
		JsonObject *source = json_array_get_object_element(sources, i);
		gint64 id = json_object_get_int_member(source, "id");

		g_string_append_printf(html, "<option value=\"%" G_GINT64_FORMAT "\"%s>", id,
		                       (id == chosen) ? " selected" : "");
		venture_html_escape_append(html, mdw_text(source, "name"));
		g_string_append(html, "</option>");
	}

	g_string_append(html, "</select></label></div>");
}

static void
mdw_input(
	GString		*html,
	HtmxRequest	*request,
	const gchar	*label,
	const gchar	*name,
	const gchar	*placeholder
){
	const gchar *value;

	value = htmx_request_get_query_param(request, name);
	g_string_append(html, "<div class=\"field\"><label>");
	venture_html_escape_append(html, label);
	g_string_append_printf(html, "<input name=\"%s\" value=\"", name);
	venture_html_escape_append(html, (NULL != value) ? value : "");
	g_string_append(html, "\" placeholder=\"");
	venture_html_escape_append(html, placeholder);
	g_string_append(html, "\"></label></div>");
}

/* The labels of a series of ISO times, in the configured zone. */
static gchar *
mdw_time_label(
	VentureWebServer	*self,
	const gchar		*iso,
	const gchar		*format
){
	g_autoptr(GDateTime) moment = NULL;
	g_autoptr(GDateTime) local = NULL;
	GTimeZone *zone;

	moment = (NULL != iso) ? g_date_time_new_from_iso8601(iso, NULL) : NULL;

	if (NULL == moment)
		return g_strdup("");

	zone = venture_context_get_timezone(self->context);
	local = (NULL != zone) ? g_date_time_to_timezone(moment, zone) : g_date_time_ref(moment);

	return g_date_time_format(local, format);
}

/* --- /market/browse ------------------------------------------------------- */

static gboolean
mdw_browse_query(
	VentureWebServer		 *self,
	HtmxRequest			 *request,
	VentureMarketdataBrowseQuery	 *query,
	GError				**error
){
	gint64 value;

	venture_marketdata_browse_query_init(query);

	if (!venture_web_request_organization(self, request, NULL, &query->organization_id, error))
		return FALSE;

	if (!mdw_number(request, "source", 0, G_MAXINT64, &query->data_source_id, error) ||
	    !mdw_number(request, "page", 1, 1000000, &value, error))
		return FALSE;

	query->page = (guint)value;

	if (!mdw_number(request, "per_page", 50, VENTURE_MARKETDATA_BROWSE_MAX_PAGE, &value, error))
		return FALSE;

	query->per_page = (guint)value;
	query->search = mdw_param(request, "search");
	query->category = mdw_param(request, "category");
	query->venue = mdw_param(request, "venue");
	query->group_key = mdw_param(request, "group");
	query->sort = mdw_param(request, "sort");
	query->descending = (0 == g_strcmp0(mdw_param(request, "dir"), "desc"));
	query->in_stock_only = mdw_flag(request, "stock");

	return TRUE;
}

/*
 * The browse query string with one thing changed: the sort (and its
 * direction), or the page. Everything else the person asked is kept, so
 * a click on a column re-sorts the same question.
 */
static gchar *
mdw_browse_link(
	HtmxRequest	*request,
	const gchar	*sort,
	gboolean	 descending,
	gint64		 page
){
	static const gchar *const kept[] = {
		"source", "search", "category", "venue", "group", "stock", "per_page", NULL
	};
	g_autoptr(GString) link = NULL;
	guint i;

	link = g_string_new("/market/browse?");

	for (i = 0; NULL != kept[i]; i++)
	{
		const gchar *value = mdw_param(request, kept[i]);

		if (NULL == value)
			continue;

		g_string_append_printf(link, "%s=", kept[i]);
		g_string_append_uri_escaped(link, value, NULL, FALSE);
		g_string_append_c(link, '&');
	}

	g_string_append_printf(link, "sort=%s&dir=%s&page=%" G_GINT64_FORMAT,
	                       sort, descending ? "desc" : "asc", page);

	return g_string_free(g_steal_pointer(&link), FALSE);
}

static void
mdw_sort_header(
	GString		*html,
	HtmxRequest	*request,
	JsonObject	*echo,
	const gchar	*sort,
	const gchar	*label,
	gboolean	 numeric
){
	g_autofree gchar *link = NULL;
	const gchar *current;
	gboolean active;
	gboolean descending;

	current = mdw_text(echo, "sort");
	active = (0 == g_strcmp0(current, sort));
	descending = json_object_get_boolean_member(echo, "descending");

	/* A second click on the active column turns it round. */
	link = mdw_browse_link(request, sort, active ? !descending : FALSE, 1);
	g_string_append_printf(html, "<th scope=\"col\" class=\"%s%s\"", numeric ? "num" : "",
	                       active ? " sorted" : "");

	if (active)
		g_string_append_printf(html, " aria-sort=\"%s\"", descending ? "descending" : "ascending");

	g_string_append(html, "><a class=\"th-sort\" href=\"");
	venture_html_escape_append(html, link);
	g_string_append(html, "\">");
	venture_html_escape_append(html, label);

	if (active)
		g_string_append(html, descending ? " \xe2\x86\x93" : " \xe2\x86\x91");

	g_string_append(html, "</a></th>");
}

static HtmxResponse *
venture_marketdata_web_browse(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GString) html = NULL;
	VentureMarketdataBrowseQuery query;
	JsonObject *root;
	JsonObject *echo;
	JsonArray *rows;
	JsonArray *list;
	gboolean api;
	gint64 page;
	gint64 pages;
	gint64 source_id;
	HtmxResponse *gate;
	guint i;

	(void)params;

	api = mdw_is_api(request);
	gate = mdw_gate(self, request, api, VENTURE_USER_ROLE_VIEWER, VENTURE_TYPE_DATA_SOURCE);

	if (NULL != gate)
		return gate;

	if (!mdw_browse_query(self, request, &query, &error))
		return venture_web_error_response(error);

	answer = venture_marketdata_browse(self->context, &query, &error);

	if (NULL == answer)
		return venture_web_error_response(error);

	if (api)
		return venture_web_json_response(answer, 200);

	root = json_node_get_object(answer);
	echo = json_object_get_object_member(root, "query");
	rows = json_object_get_array_member(root, "rows");
	source_id = json_object_get_int_member(root, "data_source_id");
	html = g_string_new(NULL);
	mdw_head(html, "Trading", "Browse",
	         "Every instrument the data sources list, at every venue, now. Sorted on the store's "
	         "own columns, so any order is a page away.");

	/* The question, as a form that works without script. */
	g_string_append(html, "<div class=\"card\"><form method=\"get\" action=\"/market/browse\" "
	                      "class=\"form-grid market-filters\" role=\"search\" aria-label=\"Browse filters\">");
	mdw_source_picker(html, root, source_id, FALSE);
	mdw_input(html, request, "Search", "search", "name or key");
	mdw_input(html, request, "Category", "category", "path, e.g. Trade Goods/Herb");

	g_string_append(html, "<div class=\"field\"><label>Venue<select name=\"venue\">"
	                      "<option value=\"\">every venue</option>");
	list = json_object_get_array_member(root, "venues");

	for (i = 0; (NULL != list) && (i < json_array_get_length(list)); i++)
	{
		JsonObject *venue = json_array_get_object_element(list, i);

		g_string_append(html, "<option value=\"");
		venture_html_escape_append(html, mdw_text(venue, "key"));
		g_string_append_printf(html, "\"%s>",
		                       (0 == g_strcmp0(mdw_text(venue, "key"), query.venue)) ? " selected" : "");
		mdw_name(html, venue, "name", "key");
		g_string_append(html, "</option>");
	}

	g_string_append(html, "</select></label></div><div class=\"field\"><label>Group<select name=\"group\">"
	                      "<option value=\"\">every group</option>");
	list = json_object_get_array_member(root, "groups");

	for (i = 0; (NULL != list) && (i < json_array_get_length(list)); i++)
	{
		const gchar *group = json_array_get_string_element(list, i);

		g_string_append(html, "<option value=\"");
		venture_html_escape_append(html, group);
		g_string_append_printf(html, "\"%s>", (0 == g_strcmp0(group, query.group_key)) ? " selected" : "");
		venture_html_escape_append(html, group);
		g_string_append(html, "</option>");
	}

	g_string_append_printf(html, "</select></label></div>"
	                       "<div class=\"field\"><label><input type=\"checkbox\" name=\"stock\" value=\"1\"%s>"
	                       " In stock only</label></div>",
	                       query.in_stock_only ? " checked" : "");

	if (NULL != query.sort)
	{
		g_string_append(html, "<input type=\"hidden\" name=\"sort\" value=\"");
		venture_html_escape_append(html, query.sort);
		g_string_append_printf(html, "\"><input type=\"hidden\" name=\"dir\" value=\"%s\">",
		                       query.descending ? "desc" : "asc");
	}

	g_string_append(html, "<button class=\"btn\" type=\"submit\">Show</button></form></div>");
	mdw_notes(html, root);

	g_string_append(html, "<div class=\"card\"><div class=\"table-wrap\"><table class=\"data market-table\">"
	                      "<thead><tr>");
	mdw_sort_header(html, request, echo, "name", "Instrument", FALSE);
	mdw_sort_header(html, request, echo, "venue", "Venue", FALSE);
	mdw_sort_header(html, request, echo, "min_price", "Lowest", TRUE);
	mdw_sort_header(html, request, echo, "quantity", "Quantity", TRUE);
	mdw_sort_header(html, request, echo, "market_value", "Market value", TRUE);
	mdw_sort_header(html, request, echo, "region_median", "Region median", TRUE);
	mdw_sort_header(html, request, echo, "pct_vs_region", "Of region", TRUE);
	mdw_sort_header(html, request, echo, "sale_rate", "Sale rate", TRUE);
	mdw_sort_header(html, request, echo, "updated", "Updated", FALSE);
	g_string_append(html, "</tr></thead><tbody>");

	for (i = 0; (NULL != rows) && (i < json_array_get_length(rows)); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);

		g_string_append(html, "<tr><td><a href=\"");
		venture_html_escape_append(html, mdw_text(row, "url"));
		g_string_append(html, "\">");
		mdw_name(html, row, "instrument_name", "instrument_key");
		g_string_append(html, "</a>");

		if (NULL != mdw_text(row, "category"))
		{
			g_string_append(html, " <span class=\"muted\">");
			venture_html_escape_append(html, mdw_text(row, "category"));
			g_string_append(html, "</span>");
		}

		g_string_append(html, "</td><td>");
		mdw_name(html, row, "venue_name", "venue_key");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_money(html, row, "min_price");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_int(html, row, "quantity");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_money(html, row, "market_value");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_money(html, row, "region_median");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_percent(html, row, "pct_vs_region", 1.0);
		g_string_append(html, "</td><td class=\"num\">");
		mdw_percent(html, row, "sale_rate", 100.0);
		g_string_append(html, "</td><td>");
		mdw_when(html, row, "taken_at");
		g_string_append(html, "</td></tr>");
	}

	if ((NULL == rows) || (0 == json_array_get_length(rows)))
		g_string_append(html, "<tr><td colspan=\"9\" class=\"muted\">Nothing matches.</td></tr>");

	g_string_append(html, "</tbody></table></div>");

	page = json_object_get_int_member(root, "page");
	pages = json_object_get_int_member(root, "pages");

	if (pages > 1)
	{
		const gchar *sort = mdw_text(echo, "sort");
		gboolean descending = json_object_get_boolean_member(echo, "descending");

		g_string_append(html, "<nav class=\"pager\" aria-label=\"Pages\">");

		if (page > 1)
		{
			g_autofree gchar *link = mdw_browse_link(request, sort, descending, page - 1);

			g_string_append(html, "<a class=\"btn btn-sm\" href=\"");
			venture_html_escape_append(html, link);
			g_string_append(html, "\">\xe2\x80\xb9 Prev</a>");
		}

		g_string_append_printf(html, "<span class=\"pager-state\">Page %" G_GINT64_FORMAT " of %"
		                       G_GINT64_FORMAT " \xc2\xb7 %" G_GINT64_FORMAT " rows</span>",
		                       page, pages, json_object_get_int_member(root, "total"));

		if (page < pages)
		{
			g_autofree gchar *link = mdw_browse_link(request, sort, descending, page + 1);

			g_string_append(html, "<a class=\"btn btn-sm\" href=\"");
			venture_html_escape_append(html, link);
			g_string_append(html, "\">Next \xe2\x80\xba</a>");
		}

		g_string_append(html, "</nav>");
	}

	g_string_append(html, "</div>");

	mdw_attribution(html, root);

	return venture_web_html_response(venture_web_page(self, request, "/market/browse", "Browse",
	                                                  html->str), 200);
}

/* --- /market/i/:source/:key ------------------------------------------------ */

/* The labels and two series of the snapshots chart, from the hourly points. */
static gchar *
mdw_snapshots_chart(
	VentureWebServer	*self,
	JsonObject		*root,
	const gchar		*currency,
	const gchar		*name
){
	g_autoptr(GPtrArray) labels = NULL;
	g_autoptr(GArray) prices = NULL;
	g_autoptr(GArray) quantities = NULL;
	g_autofree gchar *title = NULL;
	VentureWebChartSeries series[2];
	VentureWebChart chart;
	JsonArray *points;
	guint i;

	points = json_object_get_array_member(root, "hourly");
	labels = g_ptr_array_new_with_free_func(g_free);
	prices = g_array_new(FALSE, FALSE, sizeof(gdouble));
	quantities = g_array_new(FALSE, FALSE, sizeof(gdouble));

	for (i = 0; (NULL != points) && (i < json_array_get_length(points)); i++)
	{
		JsonObject *point = json_array_get_object_element(points, i);
		JsonNode *node;
		gdouble price = NAN;
		gdouble quantity = NAN;

		g_ptr_array_add(labels, mdw_time_label(self, mdw_text(point, "at"), "%b %d %H:00"));
		node = json_object_get_member(point, "min_price");

		/* A point in another currency is a gap on this axis, not a
		 * number in the wrong unit. */
		if ((NULL != node) && JSON_NODE_HOLDS_VALUE(node) &&
		    (0 == g_strcmp0(mdw_text(point, "currency"), currency)))
			price = (gdouble)json_node_get_int(node);

		node = json_object_get_member(point, "quantity");

		if ((NULL != node) && JSON_NODE_HOLDS_VALUE(node))
			quantity = (gdouble)json_node_get_int(node);

		g_array_append_val(prices, price);
		g_array_append_val(quantities, quantity);
	}

	g_ptr_array_add(labels, NULL);
	title = g_strdup_printf("%s: lowest price and quantity every hour, 14 days", name);
	memset(series, 0, sizeof(series));
	series[0].name = "Lowest price";
	series[0].values = (const gdouble *)(gpointer)prices->data;
	series[0].format = venture_web_chart_format_minor;
	series[0].format_data = (gpointer)currency;
	series[1].name = "Quantity";
	series[1].values = (const gdouble *)(gpointer)quantities->data;
	series[1].secondary = TRUE;
	memset(&chart, 0, sizeof(chart));
	chart.title = title;
	chart.summary = "The last snapshot of each hour at this venue.";
	chart.labels = (const gchar *const *)labels->pdata;
	chart.n_points = prices->len;
	chart.series = series;
	chart.n_series = 2;

	return venture_web_chart_line(&chart);
}

/* One heat map from the answer's matrix called @member. */
static gchar *
mdw_heat_chart(
	JsonObject	*heat,
	const gchar	*member,
	const gchar	*title,
	gboolean	 money,
	gboolean	 low_is_strong
){
	g_autoptr(GArray) values = NULL;
	gchar *hours[25];
	const gchar *weekdays[8];
	VentureWebChartHeat map;
	JsonArray *days;
	JsonArray *names;
	gchar *drawn;
	guint d;
	guint h;

	days = json_object_get_array_member(heat, member);
	names = json_object_get_array_member(heat, "weekdays");
	values = g_array_new(FALSE, FALSE, sizeof(gdouble));

	for (d = 0; d < 7; d++)
	{
		JsonArray *row = ((NULL != days) && (d < json_array_get_length(days)))
			? json_array_get_array_element(days, d) : NULL;

		weekdays[d] = ((NULL != names) && (d < json_array_get_length(names)))
			? json_array_get_string_element(names, d) : "";

		for (h = 0; h < 24; h++)
		{
			JsonNode *cell = ((NULL != row) && (h < json_array_get_length(row)))
				? json_array_get_element(row, h) : NULL;
			gdouble value = NAN;

			if ((NULL != cell) && JSON_NODE_HOLDS_VALUE(cell))
				value = (gdouble)json_node_get_int(cell);

			g_array_append_val(values, value);
		}
	}

	weekdays[7] = NULL;

	for (h = 0; h < 24; h++)
		hours[h] = g_strdup_printf("%02u", h);

	hours[24] = NULL;
	memset(&map, 0, sizeof(map));
	map.title = title;
	map.summary = "Mean of the hourly figures that fell in each weekday and hour.";
	map.row_labels = weekdays;
	map.n_rows = 7;
	map.column_labels = (const gchar *const *)hours;
	map.n_columns = 24;
	map.values = (const gdouble *)(gpointer)values->data;
	map.format = money ? venture_web_chart_format_minor : venture_web_chart_format_number;
	map.format_data = money ? (gpointer)mdw_text(heat, "currency") : NULL;
	map.low_is_strong = low_is_strong;
	drawn = venture_web_chart_heat(&map);

	for (h = 0; h < 24; h++)
		g_free(hours[h]);

	return drawn;
}

/* The daily history: the fullest snapshot of each day, as a chart. */
static gchar *
mdw_daily_chart(
	VentureWebServer	*self,
	JsonObject		*root,
	const gchar		*currency,
	const gchar		*name
){
	g_autoptr(GPtrArray) labels = NULL;
	g_autoptr(GArray) prices = NULL;
	g_autoptr(GArray) quantities = NULL;
	g_autofree gchar *title = NULL;
	VentureWebChartSeries series[2];
	VentureWebChart chart;
	JsonArray *days;
	guint i;

	days = json_object_get_array_member(root, "daily");
	labels = g_ptr_array_new_with_free_func(g_free);
	prices = g_array_new(FALSE, FALSE, sizeof(gdouble));
	quantities = g_array_new(FALSE, FALSE, sizeof(gdouble));

	for (i = 0; (NULL != days) && (i < json_array_get_length(days)); i++)
	{
		JsonObject *day = json_array_get_object_element(days, i);
		JsonNode *node;
		gdouble price = NAN;
		gdouble quantity;

		g_ptr_array_add(labels, mdw_time_label(self, mdw_text(day, "day"), "%b %d"));
		node = json_object_get_member(day, "price_at_max");

		if ((NULL != node) && JSON_NODE_HOLDS_VALUE(node) &&
		    (0 == g_strcmp0(mdw_text(day, "currency"), currency)))
			price = (gdouble)json_node_get_int(node);

		quantity = (gdouble)json_object_get_int_member(day, "max_quantity");
		g_array_append_val(prices, price);
		g_array_append_val(quantities, quantity);
	}

	g_ptr_array_add(labels, NULL);
	title = g_strdup_printf("%s: each day's fullest market, 60 days", name);
	memset(series, 0, sizeof(series));
	series[0].name = "Lowest price then";
	series[0].values = (const gdouble *)(gpointer)prices->data;
	series[0].format = venture_web_chart_format_minor;
	series[0].format_data = (gpointer)currency;
	series[1].name = "Most offered";
	series[1].values = (const gdouble *)(gpointer)quantities->data;
	series[1].secondary = TRUE;
	memset(&chart, 0, sizeof(chart));
	chart.title = title;
	chart.summary = "Per day, the most units on offer in any one snapshot and the lowest price in it.";
	chart.labels = (const gchar *const *)labels->pdata;
	chart.n_points = prices->len;
	chart.series = series;
	chart.n_series = 2;

	return venture_web_chart_line(&chart);
}

static void
mdw_stat(
	GString		*html,
	const gchar	*label,
	const gchar	*value_html
){
	g_string_append(html, "<div class=\"stat\"><span class=\"stat-label\">");
	venture_html_escape_append(html, label);
	g_string_append(html, "</span><span class=\"stat-value market-figure\">");
	g_string_append(html, value_html);
	g_string_append(html, "</span></div>");
}

/* A stat whose value is drawn by @draw from @object's @member. */
static void
mdw_stat_of(
	GString		*html,
	const gchar	*label,
	void		(*draw)(GString *, JsonObject *, const gchar *),
	JsonObject	*object,
	const gchar	*member
){
	g_autoptr(GString) value = g_string_new(NULL);

	draw(value, object, member);
	mdw_stat(html, label, value->str);
}

static void
mdw_share(
	GString		*html,
	JsonObject	*object,
	const gchar	*member
){
	mdw_percent(html, object, member, 100.0);
}

static void
mdw_pct(
	GString		*html,
	JsonObject	*object,
	const gchar	*member
){
	mdw_percent(html, object, member, 1.0);
}

static gboolean
mdw_instrument_query(
	VentureWebServer			 *self,
	HtmxRequest				 *request,
	GHashTable				 *params,
	VentureMarketdataInstrumentQuery	 *query,
	gchar					**out_key,
	GError					**error
){
	memset(query, 0, sizeof(*query));

	if (!venture_web_request_organization(self, request, NULL, &query->organization_id, error))
		return FALSE;

	if (!mdw_path_id(params, "source", &query->data_source_id, error))
		return FALSE;

	*out_key = mdw_path_key(request, error);

	if (NULL == *out_key)
		return FALSE;

	query->key = *out_key;
	query->venue = mdw_param(request, "venue");

	return mdw_number(request, "units", 0, VENTURE_MARKETDATA_BULK_MAX_UNITS, &query->units, error);
}

static HtmxResponse *
venture_marketdata_web_instrument(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GString) html = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *self_path = NULL;
	g_autofree gchar *escaped_key = NULL;
	VentureMarketdataInstrumentQuery query;
	JsonObject *root;
	JsonObject *instrument;
	JsonObject *base;
	JsonObject *reference;
	const gchar *name;
	const gchar *currency;
	const gchar *venue;
	gboolean api;
	HtmxResponse *gate;
	guint i;

	api = mdw_is_api(request);
	gate = mdw_gate(self, request, api, VENTURE_USER_ROLE_VIEWER, VENTURE_TYPE_DATA_SOURCE);

	if (NULL != gate)
		return gate;

	if (!mdw_instrument_query(self, request, params, &query, &key, &error))
		return venture_web_error_response(error);

	answer = venture_marketdata_instrument(self->context, &query, &error);

	if (NULL == answer)
		return venture_web_error_response(error);

	if (api)
		return venture_web_json_response(answer, 200);

	root = json_node_get_object(answer);
	html = g_string_new(NULL);

	if (!json_object_get_boolean_member(root, "available"))
	{
		mdw_head(html, "Trading", key, NULL);
		mdw_notes(html, root);

		return venture_web_html_response(venture_web_page(self, request, "/market/browse", key,
		                                                  html->str), 200);
	}

	instrument = json_object_get_object_member(root, "instrument");
	base = json_object_has_member(root, "base") ? json_object_get_object_member(root, "base") : NULL;
	reference = json_object_has_member(root, "reference")
		? json_object_get_object_member(root, "reference") : NULL;
	name = (NULL != mdw_text(instrument, "name")) ? mdw_text(instrument, "name") : key;
	currency = mdw_text(root, "currency");
	venue = mdw_text(root, "venue");
	escaped_key = g_uri_escape_string(key, NULL, FALSE);
	self_path = g_strdup_printf("/market/i/%" G_GINT64_FORMAT "/%s", query.data_source_id, escaped_key);

	g_string_append(html, "<div class=\"page-head\"><div class=\"page-title\"><nav class=\"crumbs\" "
	                      "aria-label=\"Breadcrumb\"><a href=\"/market/browse\">Browse</a> / ");
	venture_html_escape_append(html, mdw_text(root, "source_name"));
	g_string_append(html, "</nav><h1>");
	venture_html_escape_append(html, name);
	g_string_append(html, "</h1><p class=\"muted\">");
	venture_html_escape_append(html, key);

	if (NULL != mdw_text(instrument, "category"))
	{
		g_string_append(html, " \xc2\xb7 ");
		venture_html_escape_append(html, mdw_text(instrument, "category"));
	}

	if (NULL != venue)
	{
		g_string_append(html, " \xc2\xb7 at ");
		mdw_name(html, root, "venue_name", "venue");
	}

	g_string_append(html, "</p></div></div>");

	/* At the top: the whole page is the source's data, and a line at the
	 * foot of the charts would be anything but conspicuous. */
	mdw_attribution(html, root);
	mdw_notes(html, root);

	/* Where to look at it, and what to do with it. */
	{
		JsonArray *venues = json_object_get_array_member(root, "venues");
		JsonArray *lists = json_object_get_array_member(root, "watchlists");
		gint64 record_id = json_object_get_int_member(root, "record_id");

		g_string_append(html, "<div class=\"card market-actions\"><div class=\"card-head\"><h2>Act</h2></div>"
		                      "<div class=\"card-body market-action-row\">");
		g_string_append(html, "<form method=\"get\" action=\"");
		venture_html_escape_append(html, self_path);
		g_string_append(html, "\" class=\"form-inline\"><label>View at <select name=\"venue\">");

		for (i = 0; (NULL != venues) && (i < json_array_get_length(venues)); i++)
		{
			JsonObject *row = json_array_get_object_element(venues, i);

			g_string_append(html, "<option value=\"");
			venture_html_escape_append(html, mdw_text(row, "venue_key"));
			g_string_append_printf(html, "\"%s>",
			                       (0 == g_strcmp0(mdw_text(row, "venue_key"), venue)) ? " selected" : "");
			mdw_name(html, row, "venue_name", "venue_key");
			g_string_append(html, "</option>");
		}

		g_string_append(html, "</select></label> <button class=\"btn btn-sm\" type=\"submit\">View</button></form>");

		/* Promote, watch and alert are writes, so they are posts. */
		if (0 == record_id)
		{
			g_string_append(html, "<form method=\"post\" action=\"");
			venture_html_escape_append(html, self_path);
			g_string_append(html, "\" class=\"form-inline\"><input type=\"hidden\" name=\"action\" "
			                      "value=\"promote\"><button class=\"btn btn-sm\" type=\"submit\">"
			                      "Promote to a record</button></form>");
		}
		else
			g_string_append_printf(html, "<a class=\"btn btn-sm\" href=\"/e/instrument/%" G_GINT64_FORMAT
			                       "\">Instrument record</a>", record_id);

		g_string_append(html, "<form method=\"post\" action=\"");
		venture_html_escape_append(html, self_path);
		g_string_append(html, "\" class=\"form-inline\"><input type=\"hidden\" name=\"action\" value=\"watch\">"
		                      "<label>Watch on <select name=\"watchlist_id\">"
		                      "<option value=\"\">choose on the next page</option>");

		for (i = 0; (NULL != lists) && (i < json_array_get_length(lists)); i++)
		{
			JsonObject *list = json_array_get_object_element(lists, i);

			g_string_append_printf(html, "<option value=\"%" G_GINT64_FORMAT "\">",
			                       json_object_get_int_member(list, "id"));
			venture_html_escape_append(html, mdw_text(list, "name"));
			g_string_append(html, "</option>");
		}

		g_string_append(html, "</select></label> <button class=\"btn btn-sm\" type=\"submit\">"
		                      "Add to watchlist</button></form>");
		g_string_append(html, "<form method=\"post\" action=\"");
		venture_html_escape_append(html, self_path);
		g_string_append(html, "\" class=\"form-inline\"><input type=\"hidden\" name=\"action\" value=\"alert\">");

		if (NULL != venue)
		{
			g_string_append(html, "<input type=\"hidden\" name=\"venue\" value=\"");
			venture_html_escape_append(html, venue);
			g_string_append(html, "\">");
		}

		g_string_append(html, "<button class=\"btn btn-sm\" type=\"submit\">Create an alert</button></form>");
		g_string_append(html, "</div></div>");
	}

	if (NULL != base)
	{
		g_autoptr(GString) value = g_string_new(NULL);

		g_string_append(html, "<div class=\"card\"><div class=\"card-head\"><h2>Now</h2></div>"
		                      "<div class=\"stat-row market-stats\">");
		mdw_stat_of(html, "Lowest price", mdw_money, base, "min_price");
		mdw_stat_of(html, "Quantity", mdw_int, base, "quantity");
		mdw_stat_of(html, "Last seen", mdw_when, base, "last_seen");
		mdw_stat_of(html, "Median, 14 days", mdw_money, base, "median_14d");
		mdw_stat_of(html, "Region median", mdw_money, base, "region_median");
		mdw_stat_of(html, "Deal price", mdw_money, base, "deal_price");
		mdw_stat_of(html, "Of region", mdw_pct, base, "pct_vs_region");
		mdw_stat_of(html, "Market value", mdw_money, base, "market_value");
		g_string_append(html, "</div></div>");

		g_string_append(html, "<div class=\"card\"><div class=\"card-head\"><h2>Sales</h2></div>"
		                      "<div class=\"stat-row market-stats\">");
		mdw_stat_of(html, "Sold a day", mdw_decimal, reference, "sold_per_day");
		mdw_stat_of(html, "Sale rate", mdw_share, reference, "sale_rate");
		mdw_stat_of(html, "Sold for", mdw_money, reference, "sale_avg");
		mdw_stat_of(html, "Market, 14 days", mdw_money, reference, "market_14d");
		mdw_stat_of(html, "Historical, 60 days", mdw_money, reference, "historical_60d");
		g_string_append(html, "</div></div>");

		/* The pictures, each with its numbers in a table beside it. */
		{
			g_autofree gchar *chart = mdw_snapshots_chart(self, root, currency, name);

			g_string_append(html, "<div class=\"card\"><div class=\"card-head\"><h2>Snapshots</h2></div>"
			                      "<div class=\"card-body\">");
			g_string_append(html, chart);
			g_string_append(html, "</div></div>");
		}

		if (json_object_has_member(root, "heat"))
		{
			JsonObject *heat = json_object_get_object_member(root, "heat");
			g_autofree gchar *price_title = NULL;
			g_autofree gchar *quantity_title = NULL;
			g_autofree gchar *price_map = NULL;
			g_autofree gchar *quantity_map = NULL;

			price_title = g_strdup_printf("%s: lowest price by weekday and hour (%s), last 7 days",
			                              name, mdw_text(heat, "zone"));
			quantity_title = g_strdup_printf("%s: quantity by weekday and hour (%s), last 7 days",
			                                 name, mdw_text(heat, "zone"));
			price_map = mdw_heat_chart(heat, "price", price_title, TRUE, TRUE);
			quantity_map = mdw_heat_chart(heat, "quantity", quantity_title, FALSE, FALSE);
			g_string_append(html, "<div class=\"card\"><div class=\"card-head\"><h2>Heat map</h2></div>"
			                      "<div class=\"card-body market-heat\">");
			g_string_append(html, price_map);
			g_string_append(html, quantity_map);
			g_string_append(html, "</div></div>");
		}

		{
			g_autofree gchar *chart = mdw_daily_chart(self, root, currency, name);
			JsonArray *days = json_object_get_array_member(root, "daily");
			guint n = (NULL != days) ? json_array_get_length(days) : 0;

			g_string_append(html, "<div class=\"card\"><div class=\"card-head\"><h2>Daily history</h2></div>"
			                      "<div class=\"card-body\">");
			g_string_append(html, chart);
			g_string_append(html, "<details class=\"market-days\"><summary>Every day</summary>"
			                      "<table class=\"data\"><thead><tr><th scope=\"col\">Day</th>"
			                      "<th scope=\"col\" class=\"num\">Most offered</th>"
			                      "<th scope=\"col\" class=\"num\">Lowest then</th>"
			                      "<th scope=\"col\" class=\"num\">Lowest all day</th>"
			                      "<th scope=\"col\" class=\"num\">Sold</th>"
			                      "<th scope=\"col\" class=\"num\">Expired</th></tr></thead><tbody>");

			/* Newest first: the day somebody asks about is today. */
			for (i = n; i > 0; i--)
			{
				JsonObject *day = json_array_get_object_element(days, i - 1);
				g_autofree gchar *label = mdw_time_label(self, mdw_text(day, "day"), "%a %b %d");

				g_string_append(html, "<tr><td>");
				venture_html_escape_append(html, label);
				g_string_append(html, "</td><td class=\"num\">");
				mdw_int(html, day, "max_quantity");
				g_string_append(html, "</td><td class=\"num\">");
				mdw_minor(html, day, "price_at_max", mdw_text(day, "currency"));
				g_string_append(html, "</td><td class=\"num\">");
				mdw_minor(html, day, "min_price", mdw_text(day, "currency"));
				g_string_append(html, "</td><td class=\"num\">");
				mdw_int(html, day, "sold");
				g_string_append(html, "</td><td class=\"num\">");
				mdw_int(html, day, "expired");
				g_string_append(html, "</td></tr>");
			}

			g_string_append(html, "</tbody></table></details></div></div>");
		}

		/* Bulk pricing: what buying N would cost, walking the book. */
		{
			JsonArray *tiers = json_object_get_array_member(root, "tiers");
			JsonNode *bulk = json_object_get_member(root, "bulk");

			g_string_append(html, "<div class=\"card\"><div class=\"card-head\"><h2>Bulk pricing</h2></div>"
			                      "<div class=\"card-body\"><form method=\"get\" action=\"");
			venture_html_escape_append(html, self_path);
			g_string_append(html, "\" class=\"form-inline\">");

			if (NULL != venue)
			{
				g_string_append(html, "<input type=\"hidden\" name=\"venue\" value=\"");
				venture_html_escape_append(html, venue);
				g_string_append(html, "\">");
			}

			g_string_append(html, "<label>Units to buy <input class=\"num\" name=\"units\" inputmode=\"numeric\" value=\"");

			if (query.units > 0)
				g_string_append_printf(html, "%" G_GINT64_FORMAT, query.units);

			g_string_append(html, "\"></label> <button class=\"btn btn-sm\" type=\"submit\">Price it</button></form>");

			if ((NULL != bulk) && JSON_NODE_HOLDS_OBJECT(bulk))
			{
				JsonObject *cost = json_node_get_object(bulk);

				g_string_append(html, "<div class=\"stat-row market-stats market-bulk\">");
				mdw_stat_of(html, "Units filled", mdw_int, cost, "filled");
				mdw_stat_of(html, "Total cost", mdw_money, cost, "cost");
				mdw_stat_of(html, "Average", mdw_money, cost, "average");
				mdw_stat_of(html, "Last unit at", mdw_money, cost, "worst_price");
				g_string_append(html, "</div>");

				if (!json_object_get_boolean_member(cost, "complete"))
					g_string_append_printf(html, "<p class=\"muted\">The book holds only %" G_GINT64_FORMAT
					                       " of the %" G_GINT64_FORMAT " units asked for.</p>",
					                       json_object_get_int_member(cost, "filled"),
					                       json_object_get_int_member(cost, "requested"));
			}

			g_string_append(html, "<table class=\"data\"><caption class=\"muted\">The cheapest prices on offer"
			                      "</caption><thead><tr><th scope=\"col\" class=\"num\">Price</th>"
			                      "<th scope=\"col\" class=\"num\">Units</th></tr></thead><tbody>");

			for (i = 0; (NULL != tiers) && (i < json_array_get_length(tiers)) && (i < 20); i++)
			{
				JsonObject *tier = json_array_get_object_element(tiers, i);

				g_string_append(html, "<tr><td class=\"num\">");
				mdw_minor(html, tier, "price", currency);
				g_string_append(html, "</td><td class=\"num\">");
				mdw_int(html, tier, "quantity");
				g_string_append(html, "</td></tr>");
			}

			if ((NULL == tiers) || (0 == json_array_get_length(tiers)))
				g_string_append(html, "<tr><td colspan=\"2\" class=\"muted\">Nothing on offer.</td></tr>");

			g_string_append(html, "</tbody></table></div></div>");
		}
	}

	/* Other venues: Undermine's "other realms". */
	{
		JsonArray *venues = json_object_get_array_member(root, "venues");

		g_string_append(html, "<div class=\"card\"><div class=\"card-head\"><h2>Every venue</h2></div>"
		                      "<div class=\"table-wrap\"><table class=\"data market-table\"><thead><tr>"
		                      "<th scope=\"col\">Venue</th><th scope=\"col\">Group</th>"
		                      "<th scope=\"col\" class=\"num\">Lowest</th><th scope=\"col\" class=\"num\">Quantity</th>"
		                      "<th scope=\"col\" class=\"num\">Market value</th>"
		                      "<th scope=\"col\" class=\"num\">Of region</th><th scope=\"col\">Updated</th>"
		                      "</tr></thead><tbody>");

		for (i = 0; (NULL != venues) && (i < json_array_get_length(venues)); i++)
		{
			JsonObject *row = json_array_get_object_element(venues, i);
			gboolean current = (0 == g_strcmp0(mdw_text(row, "venue_key"), venue));

			g_string_append_printf(html, "<tr%s><td><a href=\"", current ? " aria-current=\"true\" class=\"selected\"" : "");
			venture_html_escape_append(html, mdw_text(row, "url"));
			g_string_append(html, "\">");
			mdw_name(html, row, "venue_name", "venue_key");
			g_string_append(html, "</a></td><td>");
			venture_html_escape_append(html, mdw_text(row, "group_key"));
			g_string_append(html, "</td><td class=\"num\">");
			mdw_money(html, row, "min_price");
			g_string_append(html, "</td><td class=\"num\">");
			mdw_int(html, row, "quantity");
			g_string_append(html, "</td><td class=\"num\">");
			mdw_money(html, row, "market_value");
			g_string_append(html, "</td><td class=\"num\">");
			mdw_percent(html, row, "pct_vs_region", 1.0);
			g_string_append(html, "</td><td>");
			mdw_when(html, row, "taken_at");
			g_string_append(html, "</td></tr>");
		}

		g_string_append(html, "</tbody></table></div></div>");
	}

	return venture_web_html_response(venture_web_page(self, request, "/market/browse", name, html->str),
	                                 200);
}

/*
 * POST to an instrument page, with action=promote, watch or alert. Each
 * promotes the instrument (idempotent: an existing record is found, a
 * deleted one restored) and then goes where the person asked: back to the
 * page, to the generic watchlist-entry form, or to the generic alert-rule
 * form, prefilled through the query string with the records just found.
 * The action is a form field rather than a path segment because the key
 * is the whole rest of the path and may itself end in "/watch".
 */
static HtmxResponse *
venture_marketdata_web_instrument_post(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) instrument = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *escaped_key = NULL;
	g_autofree gchar *target = NULL;
	VentureActor actor;
	const gchar *action;
	gint64 source_id;
	gint64 organization_id;
	HtmxResponse *gate;

	gate = mdw_gate(self, request, FALSE, VENTURE_USER_ROLE_EDITOR, VENTURE_TYPE_INSTRUMENT);

	if (NULL != gate)
		return gate;

	if (!venture_web_request_organization(self, request, NULL, &organization_id, &error))
		return venture_web_error_response(error);

	if (!mdw_path_id(params, "source", &source_id, &error))
		return venture_web_error_response(error);

	key = mdw_path_key(request, &error);

	if (NULL == key)
		return venture_web_error_response(error);

	action = htmx_request_get_form_value(request, "action");

	if ((0 != g_strcmp0(action, "promote")) && (0 != g_strcmp0(action, "watch")) &&
	    (0 != g_strcmp0(action, "alert")))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		                    "The action is promote, watch or alert");
		return venture_web_error_response(error);
	}

	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);

	if (!venture_marketdata_promote_instrument(self->context, organization_id, source_id, key, &actor,
	                                           &instrument, &error))
		return venture_web_error_response(error);

	escaped_key = g_uri_escape_string(key, NULL, FALSE);

	if (0 == g_strcmp0(action, "watch"))
	{
		const gchar *list = htmx_request_get_form_value(request, "watchlist_id");
		gint64 list_id = 0;

		if (!venture_string_is_empty(list) &&
		    !g_ascii_string_to_signed(list, 10, 1, G_MAXINT64, &list_id, NULL))
			list_id = 0;

		if (list_id > 0)
			target = g_strdup_printf("/e/watchlist_entry/new?instrument_id=%" G_GINT64_FORMAT
			                         "&watchlist_id=%" G_GINT64_FORMAT,
			                         venture_entity_get_id(instrument), list_id);
		else
			target = g_strdup_printf("/e/watchlist_entry/new?instrument_id=%" G_GINT64_FORMAT,
			                         venture_entity_get_id(instrument));
	}
	else if (0 == g_strcmp0(action, "alert"))
	{
		const gchar *venue = htmx_request_get_form_value(request, "venue");
		g_autoptr(GString) link = g_string_new(NULL);

		/* The rule's scope is the instrument; its venue's group is the
		 * likeliest narrowing, and the kind a buyer wants first is
		 * "below". All of it stays editable on the form. */
		g_string_append_printf(link, "/e/alert_rule/new?instrument_id=%" G_GINT64_FORMAT
		                       "&data_source_id=%" G_GINT64_FORMAT "&kind=below",
		                       venture_entity_get_id(instrument), source_id);

		if (!venture_string_is_empty(venue))
		{
			VentureMarketdataInstrumentQuery query;
			g_autoptr(JsonNode) answer = NULL;

			memset(&query, 0, sizeof(query));
			query.organization_id = organization_id;
			query.data_source_id = source_id;
			query.key = key;
			query.venue = venue;
			answer = venture_marketdata_instrument(self->context, &query, NULL);

			if ((NULL != answer) && (NULL != mdw_text(json_node_get_object(answer), "group_key")) &&
			    ('\0' != mdw_text(json_node_get_object(answer), "group_key")[0]))
			{
				g_string_append(link, "&group_key=");
				g_string_append_uri_escaped(link, mdw_text(json_node_get_object(answer), "group_key"),
				                            NULL, FALSE);
			}
		}

		target = g_string_free(g_steal_pointer(&link), FALSE);
	}
	else
		target = g_strdup_printf("/market/i/%" G_GINT64_FORMAT "/%s", source_id, escaped_key);

	return venture_web_redirect_to(target);
}

/* --- /market/deals ---------------------------------------------------------- */

static HtmxResponse *
venture_marketdata_web_deals(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GString) html = NULL;
	g_autoptr(VentureMoney) min_value = NULL;
	VentureMarketdataDealsQuery query;
	const gchar *text;
	JsonObject *root;
	JsonArray *rows;
	gboolean api;
	gint64 value;
	HtmxResponse *gate;
	gint64 organization_id;
	guint i;

	(void)params;

	api = mdw_is_api(request);
	gate = mdw_gate(self, request, api, VENTURE_USER_ROLE_VIEWER, VENTURE_TYPE_DATA_SOURCE);

	if (NULL != gate)
		return gate;

	if (!venture_web_request_organization(self, request, NULL, &organization_id, &error))
		return venture_web_error_response(error);

	venture_marketdata_deals_query_init(&query);
	query.organization_id = organization_id;

	if (!mdw_number(request, "source", 0, G_MAXINT64, &query.data_source_id, &error) ||
	    !mdw_number(request, "top", 50, VENTURE_MARKETDATA_DEALS_MAX, &value, &error))
		return venture_web_error_response(error);

	query.count = (guint)value;
	query.venue = mdw_param(request, "venue");
	query.group_key = mdw_param(request, "group");
	query.category = mdw_param(request, "category");
	text = mdw_param(request, "min_value");

	if (NULL != text)
	{
		min_value = venture_marketdata_parse_amount(text, &error);

		if (NULL == min_value)
			return venture_web_error_response(error);

		query.min_value = min_value;
	}

	text = mdw_param(request, "max_pct");

	if (NULL != text)
	{
		gchar *end = NULL;

		query.max_pct = g_ascii_strtod(text, &end);

		if ((NULL == end) || ('\0' != *end) || !isfinite(query.max_pct))
		{
			g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "max_pct is a percent of the region median, e.g. 80");
			return venture_web_error_response(error);
		}
	}

	answer = venture_marketdata_deals(self->context, &query, &error);

	if (NULL == answer)
		return venture_web_error_response(error);

	if (api)
		return venture_web_json_response(answer, 200);

	root = json_node_get_object(answer);
	rows = json_object_get_array_member(root, "rows");
	html = g_string_new(NULL);
	mdw_head(html, "Trading", "Deals",
	         "In stock at a price at or under the group's deal price: the median of the group's "
	         "lowest prices, or the 33rd percentile when 15 or more venues offer it.");

	g_string_append(html, "<div class=\"card\"><form method=\"get\" action=\"/market/deals\" "
	                      "class=\"form-grid market-filters\" role=\"search\" aria-label=\"Deal filters\">");
	mdw_source_picker(html, root, query.data_source_id, TRUE);
	mdw_input(html, request, "Venue", "venue", "a venue's key");
	mdw_input(html, request, "Group", "group", "a group, e.g. eu");
	mdw_input(html, request, "Category", "category", "path, e.g. Trade Goods/Herb");
	mdw_input(html, request, "Worth at least", "min_value", "e.g. 10.00 GOLD");
	mdw_input(html, request, "At most % of region", "max_pct", "e.g. 80");
	mdw_input(html, request, "How many", "top", "50");
	g_string_append(html, "<button class=\"btn\" type=\"submit\">Show</button></form></div>");
	mdw_notes(html, root);

	g_string_append(html, "<div class=\"card\"><div class=\"table-wrap\"><table class=\"data market-table\">"
	                      "<thead><tr><th scope=\"col\">Instrument</th><th scope=\"col\">Venue</th>"
	                      "<th scope=\"col\" class=\"num\">Lowest</th><th scope=\"col\" class=\"num\">Deal price</th>"
	                      "<th scope=\"col\" class=\"num\">Region median</th>"
	                      "<th scope=\"col\" class=\"num\">Of region</th><th scope=\"col\" class=\"num\">Under by</th>"
	                      "<th scope=\"col\" class=\"num\">Quantity</th><th scope=\"col\" class=\"num\">Sale rate</th>"
	                      "</tr></thead><tbody>");

	for (i = 0; (NULL != rows) && (i < json_array_get_length(rows)); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);

		g_string_append(html, "<tr><td><a href=\"");
		venture_html_escape_append(html, mdw_text(row, "url"));
		g_string_append(html, "\">");
		mdw_name(html, row, "instrument_name", "instrument_key");
		g_string_append(html, "</a></td><td>");
		mdw_name(html, row, "venue_name", "venue_key");
		g_string_append(html, " <span class=\"muted\">");
		venture_html_escape_append(html, mdw_text(row, "source_name"));
		g_string_append(html, "</span></td><td class=\"num\">");
		mdw_money(html, row, "min_price");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_money(html, row, "deal_price");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_money(html, row, "region_median");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_percent(html, row, "pct_vs_region", 1.0);
		g_string_append(html, "</td><td class=\"num\">");
		mdw_money(html, row, "discount");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_int(html, row, "quantity");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_percent(html, row, "sale_rate", 100.0);
		g_string_append(html, "</td></tr>");
	}

	if ((NULL == rows) || (0 == json_array_get_length(rows)))
		g_string_append(html, "<tr><td colspan=\"9\" class=\"muted\">No deals right now.</td></tr>");

	g_string_append(html, "</tbody></table></div>");

	if (json_object_get_boolean_member(root, "truncated"))
		g_string_append_printf(html, "<p class=\"muted market-more\">The best %u deals; narrow the "
		                       "question or ask for more to see the rest.</p>", query.count);

	g_string_append(html, "</div>");
	mdw_attribution(html, root);

	return venture_web_html_response(venture_web_page(self, request, "/market/deals", "Deals", html->str),
	                                 200);
}

/* --- /market/venues ------------------------------------------------------ */

static void
mdw_seconds(
	GString		*html,
	JsonObject	*object,
	const gchar	*member
){
	JsonNode *node;
	gint64 seconds;

	node = json_object_get_member(object, member);

	if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node))
	{
		mdw_dash(html);
		return;
	}

	seconds = json_node_get_int(node);

	if (seconds < 3600)
		g_string_append_printf(html, "%" G_GINT64_FORMAT " min", seconds / 60);
	else if (seconds < 172800)
		g_string_append_printf(html, "%" G_GINT64_FORMAT " h %02" G_GINT64_FORMAT " min",
		                       seconds / 3600, (seconds % 3600) / 60);
	else
		g_string_append_printf(html, "%" G_GINT64_FORMAT " days", seconds / 86400);
}

static HtmxResponse *
venture_marketdata_web_venues(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GString) html = NULL;
	JsonObject *root;
	JsonArray *venues;
	JsonArray *groups;
	const gchar *group;
	gint64 source_id;
	gboolean api;
	HtmxResponse *gate;
	gint64 organization_id;
	guint i;

	(void)params;

	api = mdw_is_api(request);
	gate = mdw_gate(self, request, api, VENTURE_USER_ROLE_VIEWER, VENTURE_TYPE_DATA_SOURCE);

	if (NULL != gate)
		return gate;

	if (!venture_web_request_organization(self, request, NULL, &organization_id, &error))
		return venture_web_error_response(error);

	if (!mdw_number(request, "source", 0, G_MAXINT64, &source_id, &error))
		return venture_web_error_response(error);

	group = mdw_param(request, "group");
	answer = venture_marketdata_venue_index(self->context, organization_id, source_id, group, 0, &error);

	if (NULL == answer)
		return venture_web_error_response(error);

	if (api)
		return venture_web_json_response(answer, 200);

	root = json_node_get_object(answer);
	venues = json_object_get_array_member(root, "venues");
	groups = json_object_get_array_member(root, "groups");
	html = g_string_new(NULL);
	mdw_head(html, "Trading", "Venues",
	         "How cheap each venue is against its group, and how often it updates: the shares of "
	         "instruments under, at and over the region median, and the upload timer the store "
	         "has learned.");

	g_string_append(html, "<div class=\"card\"><form method=\"get\" action=\"/market/venues\" "
	                      "class=\"form-grid market-filters\" aria-label=\"Venue filters\">");
	mdw_source_picker(html, root, source_id, TRUE);
	g_string_append(html, "<div class=\"field\"><label>Group<select name=\"group\">"
	                      "<option value=\"\">every group</option>");

	for (i = 0; (NULL != groups) && (i < json_array_get_length(groups)); i++)
	{
		const gchar *one = json_array_get_string_element(groups, i);

		g_string_append(html, "<option value=\"");
		venture_html_escape_append(html, one);
		g_string_append_printf(html, "\"%s>", (0 == g_strcmp0(one, group)) ? " selected" : "");
		venture_html_escape_append(html, one);
		g_string_append(html, "</option>");
	}

	g_string_append(html, "</select></label></div><button class=\"btn\" type=\"submit\">Show</button>"
	                      "</form></div>");
	mdw_notes(html, root);

	g_string_append(html, "<div class=\"card\"><div class=\"table-wrap\"><table class=\"data market-table\">"
	                      "<thead><tr><th scope=\"col\">Venue</th><th scope=\"col\">Group</th>"
	                      "<th scope=\"col\" class=\"num\">Compared</th><th scope=\"col\" class=\"num\">Cheaper</th>"
	                      "<th scope=\"col\" class=\"num\">Equal</th><th scope=\"col\" class=\"num\">Dearer</th>"
	                      "<th scope=\"col\" class=\"num\">Price to region</th>"
	                      "<th scope=\"col\" class=\"num\">Listings</th><th scope=\"col\">Last snapshot</th>"
	                      "<th scope=\"col\" class=\"num\">Updates every</th><th scope=\"col\">Next expected</th>"
	                      "<th scope=\"col\" class=\"num\">Data age</th></tr></thead><tbody>");

	for (i = 0; (NULL != venues) && (i < json_array_get_length(venues)); i++)
	{
		JsonObject *venue = json_array_get_object_element(venues, i);

		g_string_append(html, "<tr><td>");
		mdw_name(html, venue, "venue_name", "venue_key");
		g_string_append(html, " <span class=\"muted\">");
		venture_html_escape_append(html, mdw_text(venue, "source_name"));
		g_string_append(html, "</span></td><td>");
		venture_html_escape_append(html, mdw_text(venue, "group_key"));
		g_string_append(html, "</td><td class=\"num\">");
		mdw_int(html, venue, "instruments");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_percent(html, venue, "pct_cheaper", 1.0);
		g_string_append(html, "</td><td class=\"num\">");
		mdw_percent(html, venue, "pct_equal", 1.0);
		g_string_append(html, "</td><td class=\"num\">");
		mdw_percent(html, venue, "pct_dearer", 1.0);
		g_string_append(html, "</td><td class=\"num\">");
		mdw_percent(html, venue, "avg_ratio", 100.0);
		g_string_append(html, "</td><td class=\"num\">");
		mdw_int(html, venue, "listings");
		g_string_append(html, "</td><td>");
		mdw_when(html, venue, "last_taken_at");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_seconds(html, venue, "interval_seconds");
		g_string_append(html, "</td><td>");
		mdw_when(html, venue, "next_expected");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_seconds(html, venue, "age_seconds");

		if (json_object_get_boolean_member(venue, "overdue"))
			g_string_append(html, " <span class=\"badge warning\">late</span>");

		g_string_append(html, "</td></tr>");
	}

	if ((NULL == venues) || (0 == json_array_get_length(venues)))
		g_string_append(html, "<tr><td colspan=\"12\" class=\"muted\">No venues yet.</td></tr>");

	g_string_append(html, "</tbody></table></div></div>");
	mdw_attribution(html, root);

	return venture_web_html_response(venture_web_page(self, request, "/market/venues", "Venues", html->str),
	                                 200);
}

/* --- /market/watchlists ------------------------------------------------------ */

static HtmxResponse *
venture_marketdata_web_watchlists(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GString) html = NULL;
	JsonArray *lists;
	gboolean api;
	HtmxResponse *gate;
	gint64 organization_id;
	guint i;

	(void)params;

	api = mdw_is_api(request);
	gate = mdw_gate(self, request, api, VENTURE_USER_ROLE_VIEWER, VENTURE_TYPE_WATCHLIST);

	if (NULL != gate)
		return gate;

	if (!venture_web_request_organization(self, request, NULL, &organization_id, &error))
		return venture_web_error_response(error);

	answer = venture_marketdata_watchlists(self->context, organization_id, &error);

	if (NULL == answer)
		return venture_web_error_response(error);

	if (api)
		return venture_web_json_response(answer, 200);

	lists = json_object_get_array_member(json_node_get_object(answer), "watchlists");
	html = g_string_new("<div class=\"page-head\"><div class=\"page-title\"><div class=\"eyebrow\">Trading"
	                    "</div><h1>Watchlists</h1><p class=\"muted\">Instruments you follow, priced across "
	                    "a group of venues against your buy and sell targets.</p></div>"
	                    "<div class=\"page-actions\"><a class=\"btn btn-primary\" href=\"/e/watchlist/new\">"
	                    "New watchlist</a></div></div>");
	g_string_append(html, "<div class=\"card\"><table class=\"data\"><thead><tr><th scope=\"col\">Name</th>"
	                      "<th scope=\"col\">Group</th><th scope=\"col\" class=\"num\">Instruments</th>"
	                      "</tr></thead><tbody>");

	for (i = 0; (NULL != lists) && (i < json_array_get_length(lists)); i++)
	{
		JsonObject *list = json_array_get_object_element(lists, i);
		const gchar *group = mdw_text(list, "group_key");

		g_string_append_printf(html, "<tr><td><a href=\"/market/watchlists/%" G_GINT64_FORMAT "\">",
		                       json_object_get_int_member(list, "id"));
		venture_html_escape_append(html, mdw_text(list, "name"));
		g_string_append(html, "</a></td><td>");

		if (venture_string_is_empty(group))
			g_string_append(html, "<span class=\"muted\">every venue</span>");
		else
			venture_html_escape_append(html, group);

		g_string_append(html, "</td><td class=\"num\">");
		mdw_int(html, list, "entries");
		g_string_append(html, "</td></tr>");
	}

	if ((NULL == lists) || (0 == json_array_get_length(lists)))
		g_string_append(html, "<tr><td colspan=\"3\" class=\"muted\">No watchlists yet.</td></tr>");

	g_string_append(html, "</tbody></table></div>");

	return venture_web_html_response(venture_web_page(self, request, "/market/watchlists", "Watchlists",
	                                                  html->str), 200);
}

static HtmxResponse *
venture_marketdata_web_watchlist(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GString) html = NULL;
	JsonObject *root;
	JsonObject *list;
	JsonArray *entries;
	const gchar *group;
	gint64 id;
	gboolean api;
	HtmxResponse *gate;
	gint64 organization_id;
	guint i;

	api = mdw_is_api(request);
	gate = mdw_gate(self, request, api, VENTURE_USER_ROLE_VIEWER, VENTURE_TYPE_WATCHLIST);

	if (NULL != gate)
		return gate;

	if (!venture_web_request_organization(self, request, NULL, &organization_id, &error))
		return venture_web_error_response(error);

	if (!mdw_path_id(params, "id", &id, &error))
		return venture_web_error_response(error);

	answer = venture_marketdata_watchlist_view(self->context, organization_id, id, 0, &error);

	if (NULL == answer)
		return venture_web_error_response(error);

	if (api)
		return venture_web_json_response(answer, 200);

	root = json_node_get_object(answer);
	list = json_object_get_object_member(root, "watchlist");
	entries = json_object_get_array_member(root, "entries");
	group = mdw_text(list, "group_key");
	html = g_string_new("<div class=\"page-head\"><div class=\"page-title\"><nav class=\"crumbs\" "
	                    "aria-label=\"Breadcrumb\"><a href=\"/market/watchlists\">Watchlists</a></nav><h1>");
	venture_html_escape_append(html, mdw_text(list, "name"));
	g_string_append(html, "</h1><p class=\"muted\">");

	if (venture_string_is_empty(group))
		g_string_append(html, "Priced at every venue.");
	else
	{
		g_string_append(html, "Priced across the ");
		venture_html_escape_append(html, group);
		g_string_append(html, " group.");
	}

	g_string_append_printf(html, "</p></div><div class=\"page-actions\"><a class=\"btn\" href=\"/e/watchlist/%"
	                       G_GINT64_FORMAT "\">Edit list</a><a class=\"btn btn-primary\" href=\"/e/watchlist_entry/new?"
	                       "watchlist_id=%" G_GINT64_FORMAT "\">Add an instrument</a></div></div>", id, id);
	mdw_notes(html, root);

	g_string_append(html, "<div class=\"card\"><div class=\"table-wrap\"><table class=\"data market-table\">"
	                      "<thead><tr><th scope=\"col\">Instrument</th><th scope=\"col\">Cheapest at</th>"
	                      "<th scope=\"col\" class=\"num\">Lowest</th><th scope=\"col\" class=\"num\">Quantity</th>"
	                      "<th scope=\"col\" class=\"num\">Buy at</th><th scope=\"col\" class=\"num\">Against buy</th>"
	                      "<th scope=\"col\" class=\"num\">Sell at</th><th scope=\"col\" class=\"num\">Against sell</th>"
	                      "<th scope=\"col\">14 days</th><th scope=\"col\">Now</th></tr></thead><tbody>");

	for (i = 0; (NULL != entries) && (i < json_array_get_length(entries)); i++)
	{
		JsonObject *entry = json_array_get_object_element(entries, i);
		JsonNode *best_node = json_object_get_member(entry, "best");
		JsonObject *best = ((NULL != best_node) && JSON_NODE_HOLDS_OBJECT(best_node))
			? json_node_get_object(best_node) : NULL;
		JsonArray *trend = json_object_get_array_member(entry, "trend");
		g_autoptr(GArray) values = g_array_new(FALSE, FALSE, sizeof(gdouble));
		g_autofree gchar *spark = NULL;
		g_autofree gchar *spark_label = NULL;
		const gchar *name;
		guint j;

		name = (NULL != mdw_text(entry, "instrument_name")) ? mdw_text(entry, "instrument_name")
		                                                    : mdw_text(entry, "key");
		g_string_append(html, "<tr><td>");

		/* The prices page when the store knows it, else the record. */
		if (NULL != mdw_text(entry, "url"))
		{
			g_string_append(html, "<a href=\"");
			venture_html_escape_append(html, mdw_text(entry, "url"));
			g_string_append(html, "\">");
		}
		else
			g_string_append_printf(html, "<a href=\"/e/instrument/%" G_GINT64_FORMAT "\">",
			                       json_object_get_int_member(entry, "instrument_id"));

		venture_html_escape_append(html, (NULL != name) ? name : "Instrument");
		g_string_append(html, "</a>");
		g_string_append(html, "</td><td>");

		if (NULL != best)
			mdw_name(html, best, "venue_name", "venue_key");
		else if (NULL != mdw_text(entry, "problem"))
		{
			g_string_append(html, "<span class=\"muted\">");
			venture_html_escape_append(html, mdw_text(entry, "problem"));
			g_string_append(html, "</span>");
		}
		else
			g_string_append(html, "<span class=\"muted\">not in stock</span>");

		g_string_append(html, "</td><td class=\"num\">");
		mdw_money(html, best, "min_price");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_int(html, best, "quantity");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_money(html, entry, "target_buy");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_money(html, entry, "buy_delta");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_money(html, entry, "target_sell");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_money(html, entry, "sell_delta");
		g_string_append(html, "</td><td>");

		for (j = 0; (NULL != trend) && (j < json_array_get_length(trend)); j++)
		{
			JsonNode *cell = json_array_get_element(trend, j);
			gdouble value = ((NULL != cell) && JSON_NODE_HOLDS_VALUE(cell))
				? (gdouble)json_node_get_int(cell) : NAN;

			g_array_append_val(values, value);
		}

		spark_label = g_strdup_printf("%s: lowest price each day, 14 days", (NULL != name) ? name : "");
		spark = venture_web_chart_sparkline(spark_label, (const gdouble *)(gpointer)values->data,
		                                    values->len);
		g_string_append(html, spark);
		g_string_append(html, "</td><td>");

		if (json_object_get_boolean_member(entry, "buy_now"))
			g_string_append(html, "<span class=\"badge positive\">buy</span>");
		else if (json_object_get_boolean_member(entry, "sell_now"))
			g_string_append(html, "<span class=\"badge info\">sell</span>");

		g_string_append(html, "</td></tr>");
	}

	if ((NULL == entries) || (0 == json_array_get_length(entries)))
		g_string_append(html, "<tr><td colspan=\"10\" class=\"muted\">Nothing on this list yet.</td></tr>");

	g_string_append(html, "</tbody></table></div></div>");
	mdw_attribution(html, root);

	return venture_web_html_response(venture_web_page(self, request, "/market/watchlists",
	                                                  mdw_text(list, "name"), html->str), 200);
}

/* --- /market/alerts ---------------------------------------------------------- */

/* The alerts page, with an evaluation's answer above it when there is one. */
static HtmxResponse *
mdw_alerts_page(
	VentureWebServer	*self,
	HtmxRequest		*request,
	JsonObject		*root,
	JsonObject		*evaluated,
	const gchar		*evaluated_name
){
	g_autoptr(GString) html = NULL;
	JsonArray *hits;
	JsonArray *rules;
	guint i;

	hits = json_object_get_array_member(root, "hits");
	rules = json_object_get_array_member(root, "rules");
	html = g_string_new("<div class=\"page-head\"><div class=\"page-title\"><div class=\"eyebrow\">Trading"
	                    "</div><h1>Alerts</h1><p class=\"muted\">What the rules caught, newest first, and "
	                    "the rules themselves. Every feed run evaluates them; a rule can be evaluated "
	                    "now from here.</p></div><div class=\"page-actions\"><a class=\"btn btn-primary\" "
	                    "href=\"/e/alert_rule/new\">New rule</a></div></div>");

	if (NULL != evaluated)
	{
		JsonArray *notes = json_object_get_array_member(evaluated, "notes");
		JsonArray *matches = json_object_get_array_member(evaluated, "matches");

		/* One child: a notice is a flex row, and loose text beside a
		 * <strong> would be laid out as a column of its own. */
		g_string_append(html, "<div class=\"notice info market-evaluated\" role=\"status\"><span><strong>");
		venture_html_escape_append(html, evaluated_name);
		g_string_append_printf(html, "</strong>: %" G_GINT64_FORMAT " matched, %" G_GINT64_FORMAT
		                       " recorded, %" G_GINT64_FORMAT " held back by the cooldown.",
		                       json_object_get_int_member(evaluated, "candidates"),
		                       json_object_get_int_member(evaluated, "written"),
		                       json_object_get_int_member(evaluated, "cooled"));

		for (i = 0; (NULL != matches) && (i < json_array_get_length(matches)) && (i < 10); i++)
		{
			g_string_append(html, "<br>");
			venture_html_escape_append(html,
				mdw_text(json_array_get_object_element(matches, i), "message"));
		}

		for (i = 0; (NULL != notes) && (i < json_array_get_length(notes)); i++)
		{
			g_string_append(html, "<br><span class=\"muted\">");
			venture_html_escape_append(html, json_array_get_string_element(notes, i));
			g_string_append(html, "</span>");
		}

		g_string_append(html, "</span></div>");
	}

	g_string_append(html, "<div class=\"card\"><div class=\"card-head\"><h2>Recent hits</h2></div>"
	                      "<div class=\"table-wrap\"><table class=\"data market-table\"><thead><tr>"
	                      "<th scope=\"col\">When</th><th scope=\"col\">Rule</th><th scope=\"col\">What happened</th>"
	                      "<th scope=\"col\" class=\"num\">Seen</th><th scope=\"col\" class=\"num\">Against</th>"
	                      "</tr></thead><tbody>");

	for (i = 0; (NULL != hits) && (i < json_array_get_length(hits)); i++)
	{
		JsonObject *hit = json_array_get_object_element(hits, i);
		const gchar *page = mdw_text(hit, "page");
		const gchar *url = mdw_text(hit, "url");

		g_string_append(html, "<tr><td>");
		mdw_when(html, hit, "observed_at");
		g_string_append_printf(html, "</td><td><a href=\"/e/alert_rule/%" G_GINT64_FORMAT "\">",
		                       json_object_get_int_member(hit, "rule_id"));
		venture_html_escape_append(html, (NULL != mdw_text(hit, "rule_name")) ? mdw_text(hit, "rule_name")
		                                                                      : "a deleted rule");
		g_string_append_printf(html, "</a></td><td><a href=\"/e/alert_hit/%" G_GINT64_FORMAT "\">",
		                       json_object_get_int_member(hit, "id"));
		venture_html_escape_append(html, mdw_text(hit, "message"));
		g_string_append(html, "</a>");

		if (NULL != page)
		{
			g_string_append(html, " <a class=\"muted\" href=\"");
			venture_html_escape_append(html, page);
			g_string_append(html, "\">prices</a>");
		}

		/* An entry's link came from a feed: only a web address is one. */
		if ((NULL != url) && venture_web_url_is_web(url))
		{
			g_string_append(html, " <a class=\"muted\" rel=\"noopener noreferrer\" href=\"");
			venture_html_escape_append(html, url);
			g_string_append(html, "\">source</a>");
		}

		g_string_append(html, "</td><td class=\"num\">");

		if (json_object_has_member(hit, "observed") &&
		    JSON_NODE_HOLDS_OBJECT(json_object_get_member(hit, "observed")))
			mdw_money(html, hit, "observed");
		else
			mdw_decimal(html, hit, "observed_number");

		g_string_append(html, "</td><td class=\"num\">");

		if (json_object_has_member(hit, "reference") &&
		    JSON_NODE_HOLDS_OBJECT(json_object_get_member(hit, "reference")))
			mdw_money(html, hit, "reference");
		else
			mdw_decimal(html, hit, "reference_number");

		g_string_append(html, "</td></tr>");
	}

	if ((NULL == hits) || (0 == json_array_get_length(hits)))
		g_string_append(html, "<tr><td colspan=\"5\" class=\"muted\">Nothing has fired yet.</td></tr>");

	g_string_append(html, "</tbody></table></div></div>");

	/* Under the hits: the overview's sources, and any an evaluation just
	 * read for the matches shown above, each once. */
	{
		g_auto(GStrv) shown = venture_marketdata_attribution_dup(root);
		g_auto(GStrv) matched = venture_marketdata_attribution_dup(evaluated);
		g_autoptr(GStrvBuilder) lines = g_strv_builder_new();
		g_auto(GStrv) all = NULL;

		g_strv_builder_addv(lines, (const gchar **)shown);

		for (i = 0; NULL != matched[i]; i++)
		{
			if (!g_strv_contains((const gchar *const *)shown, matched[i]))
				g_strv_builder_add(lines, matched[i]);
		}

		all = g_strv_builder_end(lines);
		venture_marketdata_attribution_append_html(html, (const gchar *const *)all);
	}

	g_string_append(html, "<div class=\"card\"><div class=\"card-head\"><h2>Rules</h2></div>"
	                      "<div class=\"table-wrap\"><table class=\"data\"><thead><tr><th scope=\"col\">Rule</th>"
	                      "<th scope=\"col\">Kind</th><th scope=\"col\">On</th><th scope=\"col\">Last fired</th>"
	                      "<th scope=\"col\"><span class=\"visually-hidden\">Evaluate</span></th>"
	                      "</tr></thead><tbody>");

	for (i = 0; (NULL != rules) && (i < json_array_get_length(rules)); i++)
	{
		JsonObject *rule = json_array_get_object_element(rules, i);
		gint64 id = json_object_get_int_member(rule, "id");
		gboolean enabled = json_object_has_member(rule, "enabled") &&
		                   json_object_get_boolean_member(rule, "enabled");

		g_string_append_printf(html, "<tr><td><a href=\"/e/alert_rule/%" G_GINT64_FORMAT "\">", id);
		venture_html_escape_append(html, mdw_text(rule, "name"));
		g_string_append(html, "</a></td><td>");
		venture_html_escape_append(html, mdw_text(rule, "kind"));
		g_string_append_printf(html, "</td><td>%s</td><td>",
		                       enabled ? "<span class=\"badge positive\">on</span>"
		                               : "<span class=\"badge\">off</span>");
		mdw_when(html, rule, "last_hit_at");
		g_string_append_printf(html, "</td><td><form method=\"post\" action=\"/market/alerts/%"
		                       G_GINT64_FORMAT "/evaluate\" class=\"form-inline\"><button class=\"btn btn-sm\" "
		                       "type=\"submit\">Evaluate now</button></form></td></tr>", id);
	}

	if ((NULL == rules) || (0 == json_array_get_length(rules)))
		g_string_append(html, "<tr><td colspan=\"5\" class=\"muted\">No rules yet.</td></tr>");

	g_string_append(html, "</tbody></table></div></div>");

	return venture_web_html_response(venture_web_page(self, request, "/market/alerts", "Alerts", html->str),
	                                 200);
}

static HtmxResponse *
venture_marketdata_web_alerts(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) answer = NULL;
	gboolean api;
	gint64 count;
	HtmxResponse *gate;
	gint64 organization_id;

	(void)params;

	api = mdw_is_api(request);
	gate = mdw_gate(self, request, api, VENTURE_USER_ROLE_VIEWER, VENTURE_TYPE_ALERT_HIT);

	if (NULL != gate)
		return gate;

	if (!venture_web_request_organization(self, request, NULL, &organization_id, &error))
		return venture_web_error_response(error);

	if (!mdw_number(request, "count", 50, 500, &count, &error))
		return venture_web_error_response(error);

	answer = venture_marketdata_alerts_overview(self->context, organization_id, (guint)count, &error);

	if (NULL == answer)
		return venture_web_error_response(error);

	if (api)
		return venture_web_json_response(answer, 200);

	return mdw_alerts_page(self, request, json_node_get_object(answer), NULL, NULL);
}

/*
 * POST /market/alerts/:id/evaluate and its API twin: the "evaluate now"
 * button. Records what fires (the cooldown and the cap hold as in a feed
 * run) unless ?record=false asks only what would.
 */
static HtmxResponse *
venture_marketdata_web_evaluate(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) rule = NULL;
	g_autoptr(JsonNode) report = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autofree gchar *name = NULL;
	gboolean api;
	gboolean record;
	gint64 id;
	HtmxResponse *gate;
	gint64 organization_id;

	api = mdw_is_api(request);
	gate = mdw_gate(self, request, api, VENTURE_USER_ROLE_EDITOR, VENTURE_TYPE_ALERT_RULE);

	if (NULL != gate)
		return gate;

	if (!venture_web_request_organization(self, request, NULL, &organization_id, &error))
		return venture_web_error_response(error);

	if (!mdw_path_id(params, "id", &id, &error))
		return venture_web_error_response(error);

	/* Another organization's rule is NOT_FOUND, never FORBIDDEN. */
	rule = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_ALERT_RULE, id,
	                            NULL);

	if ((NULL == rule) || venture_entity_is_deleted(rule) ||
	    (venture_entity_get_organization_id(rule) != organization_id))
	{
		g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "No alert rule #%" G_GINT64_FORMAT " in this organization", id);
		return venture_web_error_response(error);
	}

	record = (0 != g_strcmp0(mdw_param(request, "record"), "false"));

	if (!venture_marketdata_alerts_evaluate(self->context, rule, record, &report, &error))
		return venture_web_error_response(error);

	/* The matches are readings of their sources' data: name them, for the
	 * API and for the page that lists the matches. */
	{
		g_autoptr(GPtrArray) attributions = g_ptr_array_new_with_free_func(g_free);

		venture_marketdata_attribution_collect(self->context, organization_id,
		                                       json_object_get_member(json_node_get_object(report),
		                                                              "matches"),
		                                       attributions);
		venture_marketdata_attribution_set(json_node_get_object(report), attributions);
	}

	if (api)
		return venture_web_json_response(report, 200);

	answer = venture_marketdata_alerts_overview(self->context, organization_id, 50, &error);

	if (NULL == answer)
		return venture_web_error_response(error);

	g_object_get(rule, "name", &name, NULL);

	return mdw_alerts_page(self, request, json_node_get_object(answer), json_node_get_object(report), name);
}

/* --- Report controls ------------------------------------------------------- */

/*
 * The market reports' questions, as forms: which source, venue, group and
 * category, the bounds, and which watchlist. Every one is a query
 * parameter the report API takes too.
 */
static void
venture_marketdata_web_report_controls(
	VentureWebServer	*self,
	HtmxRequest		*request,
	VentureReport		*report,
	GString			*content
){
	const gchar *name;
	const gchar *organization;

	name = venture_report_get_name(report);

	if ((0 != g_strcmp0(name, "market_deals")) && (0 != g_strcmp0(name, "venue_index")) &&
	    (0 != g_strcmp0(name, "watchlist")) && (0 != g_strcmp0(name, "accounts")) &&
	    (0 != g_strcmp0(name, "account_holdings")) && (0 != g_strcmp0(name, "external_pnl")))
		return;

	g_string_append(content, "<form method=\"get\" class=\"form-grid\">");
	organization = htmx_request_get_query_param(request, "organization_id");

	if (!venture_string_is_empty(organization))
	{
		g_string_append(content, "<input type=\"hidden\" name=\"organization_id\" value=\"");
		venture_html_escape_append(content, organization);
		g_string_append(content, "\">");
	}

	if (0 == g_strcmp0(name, "watchlist"))
	{
		g_autoptr(JsonNode) lists = NULL;
		JsonArray *array = NULL;
		const gchar *chosen;
		gint64 organization_id;
		guint i;

		organization_id = venture_string_is_empty(organization)
			? venture_context_get_default_organization_id(self->context)
			: g_ascii_strtoll(organization, NULL, 10);
		lists = venture_marketdata_watchlists(self->context, organization_id, NULL);

		if (NULL != lists)
			array = json_object_get_array_member(json_node_get_object(lists), "watchlists");

		chosen = htmx_request_get_query_param(request, "watchlist_id");
		g_string_append(content, "<div class=\"field\"><label>Watchlist<select name=\"watchlist_id\">");

		for (i = 0; (NULL != array) && (i < json_array_get_length(array)); i++)
		{
			JsonObject *list = json_array_get_object_element(array, i);
			gint64 id = json_object_get_int_member(list, "id");

			g_string_append_printf(content, "<option value=\"%" G_GINT64_FORMAT "\"%s>", id,
			                       ((NULL != chosen) && (g_ascii_strtoll(chosen, NULL, 10) == id))
			                       ? " selected" : "");
			venture_html_escape_append(content, mdw_text(list, "name"));
			g_string_append(content, "</option>");
		}

		g_string_append(content, "</select></label></div>");
	}
	else if (0 == g_strcmp0(name, "accounts"))
	{
		venture_web_goals_input(request, content, "Data source", "data_source_id", "id; every source");
		venture_web_goals_input(request, content, "Realm", "group_key", "every realm");
		venture_web_goals_input(request, content, "Login", "login", "every login, by key");
		venture_web_goals_input(request, content, "Group by", "group_by", "login, or a row per account");
		venture_web_goals_input(request, content, "Value holdings at", "basis", "conservative");
		venture_web_goals_input(request, content, "Listings expiring within (hours)", "expiring_hours", "12");
		venture_web_goals_input(request, content, "Mail expiring within (days)", "mail_days", "3");
		venture_web_goals_input(request, content, "Unvisited after (days)", "stale_days", "14");
		venture_web_goals_input(request, content, "Order", "sort", "attention");
	}
	else if (0 == g_strcmp0(name, "account_holdings"))
	{
		venture_web_goals_input(request, content, "Data source", "data_source_id", "id; the first with accounts");
		venture_web_goals_input(request, content, "Account", "account_key", "every account");
		venture_web_goals_input(request, content, "Login", "login", "every login, by key");
		venture_web_goals_input(request, content, "Group by", "group_by", "login, or a row per item");
		venture_web_goals_input(request, content, "Place", "place", "everywhere but currencies");
		venture_web_goals_input(request, content, "Category", "category_path", "a store category path");
		venture_web_goals_input(request, content, "Value at", "basis", "conservative");
		venture_web_goals_input(request, content, "Worth at least", "min_value", "e.g. 100.00 GOLD");
		venture_web_goals_input(request, content, "Dead stock: no sale for (days)", "dead_days", "every item");
		venture_web_goals_input(request, content, "Order", "sort", "value");
		venture_web_goals_input(request, content, "How many", "top", "50");
	}
	else if (0 == g_strcmp0(name, "external_pnl"))
	{
		venture_web_goals_input(request, content, "Data source", "data_source_id", "id; the first with accounts");
		venture_web_goals_input(request, content, "Group by", "group_by", "day, week, month, account, venue, "
		                        "instrument, source or login");
		venture_web_goals_input(request, content, "Account", "account_key", "every account");
		venture_web_goals_input(request, content, "Login", "login", "every login, by key");
		venture_web_goals_input(request, content, "Venue", "venue", "every venue");
		venture_web_goals_input(request, content, "Item", "instrument", "every item");
		venture_web_goals_input(request, content, "Source label", "source", "e.g. Auction");
	}
	else
	{
		venture_web_goals_input(request, content, "Data source", "data_source_id", "id; every source");
		venture_web_goals_input(request, content, "Group", "group_key", "every group");

		if (0 == g_strcmp0(name, "market_deals"))
		{
			venture_web_goals_input(request, content, "Venue", "venue", "a venue's key");
			venture_web_goals_input(request, content, "Category", "category_path", "a store category path");
			venture_web_goals_input(request, content, "Worth at least", "min_value", "e.g. 10.00 GOLD");
			venture_web_goals_input(request, content, "At most % of region", "max_pct", "e.g. 80");
			venture_web_goals_input(request, content, "How many", "top", "50");
		}
	}

	g_string_append(content, "<button class=\"btn\" type=\"submit\">Run report</button></form>");
}

/* --- Routes ---------------------------------------------------------------- */

static void
venture_marketdata_pages_register(VentureWebServer *self)
{
	static const struct
	{
		HtmxMethod		 method;
		const gchar		*path;
		HtmxRouteCallback	 handler;
	} routes[] = {
		{ HTMX_METHOD_GET, "/market/browse", venture_marketdata_web_browse },
		{ HTMX_METHOD_GET, "/market/deals", venture_marketdata_web_deals },
		{ HTMX_METHOD_GET, "/market/venues", venture_marketdata_web_venues },
		{ HTMX_METHOD_GET, "/market/watchlists", venture_marketdata_web_watchlists },
		{ HTMX_METHOD_GET, "/market/watchlists/:id", venture_marketdata_web_watchlist },
		{ HTMX_METHOD_GET, "/market/alerts", venture_marketdata_web_alerts },
		{ HTMX_METHOD_POST, "/market/alerts/:id/evaluate", venture_marketdata_web_evaluate },
		/* The key is the rest of the path: it may hold a slash. */
		{ HTMX_METHOD_GET, "/market/i/:source/*", venture_marketdata_web_instrument },
		{ HTMX_METHOD_POST, "/market/i/:source/*", venture_marketdata_web_instrument_post },
		/* The twins: the same handlers, answering JSON for /api/. */
		{ HTMX_METHOD_GET, "/api/v1/market/browse", venture_marketdata_web_browse },
		{ HTMX_METHOD_GET, "/api/v1/market/deals", venture_marketdata_web_deals },
		{ HTMX_METHOD_GET, "/api/v1/market/venues", venture_marketdata_web_venues },
		{ HTMX_METHOD_GET, "/api/v1/market/watchlists", venture_marketdata_web_watchlists },
		{ HTMX_METHOD_GET, "/api/v1/market/watchlists/:id", venture_marketdata_web_watchlist },
		{ HTMX_METHOD_GET, "/api/v1/market/alerts", venture_marketdata_web_alerts },
		{ HTMX_METHOD_POST, "/api/v1/market/alerts/:id/evaluate", venture_marketdata_web_evaluate },
		{ HTMX_METHOD_GET, "/api/v1/market/i/:source/*", venture_marketdata_web_instrument },
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(routes); i++)
		venture_web_server_add_classified_route(self, routes[i].method, routes[i].path,
		                                        VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE,
		                                        routes[i].handler, self);
}
