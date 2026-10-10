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
 * The key a path names: everything after "/i/<source>/" (or, for the
 * price history's route, "/history/<source>/"). The server hands
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

	/* By the route's own prefix: a key may hold "/market/i/" itself. */
	if ((NULL != path) && g_str_has_prefix(path, "/api/v1/market/history/"))
		key = strchr(path + strlen("/api/v1/market/history/"), '/');
	else
	{
		at = (NULL != path) ? strstr(path, "/market/i/") : NULL;
		key = (NULL != at) ? strchr(at + strlen("/market/i/"), '/') : NULL;
	}

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

/*
 * The tooltip card of an instrument's display: its icon, its name in its
 * colour, and its lines, each in its own colour with an optional
 * right-hand column -- the way a game draws an item. The display was
 * checked when the answer was built (colours are #rrggbb, the icon is
 * this server's or https); every text is escaped here.
 */
static void
mdw_tooltip_card(
	GString		*html,
	JsonObject	*display,
	const gchar	*name,
	const gchar	*extra_class
){
	JsonArray *lines;
	const gchar *icon = mdw_text(display, "icon");
	const gchar *color = mdw_text(display, "color");
	guint i;

	g_string_append_printf(html, "<span class=\"item-tip-card%s%s\" role=\"tooltip\">",
	                       (NULL != extra_class) ? " " : "", (NULL != extra_class) ? extra_class : "");

	if (NULL != icon)
	{
		g_string_append(html, "<img class=\"item-tip-icon\" src=\"");
		venture_html_escape_append(html, icon);
		g_string_append(html, "\" width=\"56\" height=\"56\" alt=\"\" loading=\"lazy\">");
	}

	g_string_append(html, "<span class=\"item-tip-body\"><span class=\"item-tip-name\"");
	if (NULL != color)
		g_string_append_printf(html, " style=\"color:%s\"", color);
	g_string_append(html, ">");
	venture_html_escape_append(html, name);
	g_string_append(html, "</span>");

	lines = json_object_has_member(display, "lines") ? json_object_get_array_member(display, "lines") : NULL;

	for (i = 0; (NULL != lines) && (i < json_array_get_length(lines)); i++)
	{
		JsonObject *line = json_array_get_object_element(lines, i);

		g_string_append(html, "<span class=\"item-tip-line\"");
		if (NULL != mdw_text(line, "color"))
			g_string_append_printf(html, " style=\"color:%s\"", mdw_text(line, "color"));
		g_string_append(html, "><span>");
		venture_html_escape_append(html, mdw_text(line, "text"));
		g_string_append(html, "</span>");

		if (NULL != mdw_text(line, "right"))
		{
			g_string_append(html, "<span class=\"item-tip-right\">");
			venture_html_escape_append(html, mdw_text(line, "right"));
			g_string_append(html, "</span>");
		}

		g_string_append(html, "</span>");
	}

	g_string_append(html, "</span></span>");
}

/* A name, or the key when the store gave none; an instrument's variant
 * (its bonuses, a pet's breed) after it, since variants share a name.
 * An instrument with a display is drawn with its icon, in its colour, and
 * shows its tooltip card on hover or focus. */
static void
mdw_name(
	GString		*html,
	JsonObject	*object,
	const gchar	*name_member,
	const gchar	*key_member
){
	const gchar *name;
	const gchar *variant;
	JsonObject *display;

	name = mdw_text(object, name_member);
	display = ((NULL != name) && json_object_has_member(object, "display"))
		? json_object_get_object_member(object, "display") : NULL;

	if (NULL != display)
	{
		g_string_append(html, "<span class=\"item-tip\">");

		if (NULL != mdw_text(display, "icon"))
		{
			g_string_append(html, "<img class=\"item-icon\" src=\"");
			venture_html_escape_append(html, mdw_text(display, "icon"));
			g_string_append(html, "\" width=\"18\" height=\"18\" alt=\"\" loading=\"lazy\">");
		}

		g_string_append(html, "<span class=\"item-name\"");
		if (NULL != mdw_text(display, "color"))
			g_string_append_printf(html, " style=\"color:%s\"", mdw_text(display, "color"));
		g_string_append(html, ">");
		venture_html_escape_append(html, name);
		g_string_append(html, "</span>");
		mdw_tooltip_card(html, display, name, NULL);
		g_string_append(html, "</span>");
	}
	else
		venture_html_escape_append(html, (NULL != name) ? name : mdw_text(object, key_member));

	variant = mdw_text(object, "variant");

	if ((NULL != name) && (NULL != variant))
	{
		g_string_append(html, " <span class=\"muted\">(");
		venture_html_escape_append(html, variant);
		g_string_append(html, ")</span>");
	}
}

/*
 * The venue group picker: every venue, "My characters" when a push source
 * exists, and the organization's saved groups (the answer's venue_groups).
 */
static void
mdw_venue_group_picker(
	GString		*html,
	JsonObject	*root,
	const gchar	*selected
){
	JsonArray *list;
	guint i;

	g_string_append(html, "<div class=\"field\"><label>Venue group<select name=\"venue_group\">"
	                      "<option value=\"\">every venue</option>");
	list = json_object_has_member(root, "venue_groups")
		? json_object_get_array_member(root, "venue_groups") : NULL;

	for (i = 0; (NULL != list) && (i < json_array_get_length(list)); i++)
	{
		JsonObject *group = json_array_get_object_element(list, i);

		g_string_append(html, "<option value=\"");
		venture_html_escape_append(html, mdw_text(group, "value"));
		g_string_append_printf(html, "\"%s>",
		                       (0 == g_strcmp0(mdw_text(group, "value"), selected)) ? " selected" : "");
		venture_html_escape_append(html, mdw_text(group, "name"));
		g_string_append(html, "</option>");
	}

	g_string_append(html, "</select></label> <a class=\"muted\" href=\"/e/venue_group\">edit groups</a></div>");
}

/*
 * The category picker: the answer's "categories" (the store's paths, as
 * the provider files instruments -- an auction house's item classes and
 * subclasses), grouped by their first part the way an auction house lists
 * them: "All Armor", then each kind of armor. A category matches itself
 * and everything beneath it, so "All Armor" is the path "Armor". A
 * category asked for that is not listed (typed into the address) is kept
 * selected rather than silently dropped.
 */
static void
mdw_category_picker(
	GString		*html,
	JsonObject	*root,
	const gchar	*selected
){
	JsonArray *list;
	g_autofree gchar *open_group = NULL;
	gboolean found = FALSE;
	guint i;

	list = (NULL != root) && json_object_has_member(root, "categories")
		? json_object_get_array_member(root, "categories") : NULL;

	g_string_append(html, "<div class=\"field\"><label>Category<select name=\"category\">"
	                      "<option value=\"\">every category</option>");

	for (i = 0; (NULL != list) && (i < json_array_get_length(list)); i++)
	{
		const gchar *path = mdw_text(json_array_get_object_element(list, i), "path");
		const gchar *slash;
		g_autofree gchar *top = NULL;

		if (NULL == path)
			continue;

		slash = strchr(path, '/');
		top = (NULL != slash) ? g_strndup(path, slash - path) : g_strdup(path);

		if (0 != g_strcmp0(top, open_group))
		{
			if (NULL != open_group)
				g_string_append(html, "</optgroup>");

			g_string_append(html, "<optgroup label=\"");
			venture_html_escape_append(html, top);
			g_string_append(html, "\"><option value=\"");
			venture_html_escape_append(html, top);
			g_string_append_printf(html, "\"%s>All ", (0 == g_strcmp0(top, selected)) ? " selected" : "");
			venture_html_escape_append(html, top);
			g_string_append(html, "</option>");
			found = found || (0 == g_strcmp0(top, selected));
			g_free(open_group);
			open_group = g_strdup(top);
		}

		if (NULL == slash)
			continue;

		g_string_append(html, "<option value=\"");
		venture_html_escape_append(html, path);
		g_string_append_printf(html, "\"%s>", (0 == g_strcmp0(path, selected)) ? " selected" : "");
		venture_html_escape_append(html, slash + 1);
		g_string_append(html, "</option>");
		found = found || (0 == g_strcmp0(path, selected));
	}

	if (NULL != open_group)
		g_string_append(html, "</optgroup>");

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

/*
 * Links to other sites (an answer's "links": {label, url}), opened in a
 * new tab and sending no referrer: they leave VENTURE, and the page they
 * leave from names an organization's data.
 */
static void
mdw_links(
	GString		*html,
	JsonObject	*object,
	gboolean	 buttons
){
	JsonArray *links;
	guint i;

	if ((NULL == object) || !json_object_has_member(object, "links"))
		return;

	links = json_object_get_array_member(object, "links");

	for (i = 0; (NULL != links) && (i < json_array_get_length(links)); i++)
	{
		JsonObject *link = json_array_get_object_element(links, i);

		g_string_append_printf(html, "%s<a %shref=\"", (i > 0) ? " " : "",
		                       buttons ? "class=\"btn\" " : "");
		venture_html_escape_append(html, mdw_text(link, "url"));
		g_string_append(html, "\" target=\"_blank\" rel=\"noopener noreferrer\">");
		if (buttons)
			g_string_append(html, "View on ");
		venture_html_escape_append(html, mdw_text(link, "label"));
		g_string_append(html, " \xe2\x86\x97</a>");
	}
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

/* Whether the answer marks @object's @prefix price stale (see mdw_age()). */
static gboolean
mdw_stale(
	JsonObject	*object,
	const gchar	*prefix
){
	g_autofree gchar *name = g_strconcat(prefix, "stale", NULL);
	JsonNode *node = (NULL != object) ? json_object_get_member(object, name) : NULL;

	return (NULL != node) && JSON_NODE_HOLDS_VALUE(node) &&
	       (G_TYPE_BOOLEAN == json_node_get_value_type(node)) && json_node_get_boolean(node);
}

/* " class=\"is-stale\"" for a cell whose price is stale, else nothing. */
static void
mdw_stale_class(
	GString		*html,
	JsonObject	*object,
	const gchar	*prefix
){
	if (mdw_stale(object, prefix))
		g_string_append(html, " class=\"is-stale\"");
}

/*
 * How old a price is, compactly, under the venue it was taken at: "12m",
 * "3h", "2d", in a <time> whose title is the exact moment in the
 * configured zone. The answer judges staleness (@prefix "stale", against
 * series.stale_minutes); a stale one is flagged in words as well as in
 * colour, since colour alone says nothing to a screen reader. The answer
 * computed the age at its own "now", so a page and its JSON twin agree.
 */
static void
mdw_age(
	VentureWebServer	*self,
	GString			*html,
	JsonObject		*object,
	const gchar		*prefix
){
	g_autofree gchar *at_name = g_strconcat(prefix, "taken_at", NULL);
	g_autofree gchar *age_name = g_strconcat(prefix, "age_seconds", NULL);
	g_autofree gchar *exact = NULL;
	g_autofree gchar *compact = NULL;
	const gchar *at = mdw_text(object, at_name);
	gboolean stale = mdw_stale(object, prefix);
	JsonNode *node = (NULL != object) ? json_object_get_member(object, age_name) : NULL;
	gint64 age;

	if ((NULL == at) || (NULL == node) || !JSON_NODE_HOLDS_VALUE(node))
	{
		g_string_append(html, "<span class=\"price-age is-stale\">no time"
		                      "<span class=\"price-age-flag\">stale</span></span>");
		return;
	}

	age = json_node_get_int(node);

	if (age < 60)
		compact = g_strdup("<1m");
	else if (age < 3600)
		compact = g_strdup_printf("%" G_GINT64_FORMAT "m", age / 60);
	else if (age < 2 * 86400)
		compact = g_strdup_printf("%" G_GINT64_FORMAT "h", age / 3600);
	else
		compact = g_strdup_printf("%" G_GINT64_FORMAT "d", age / 86400);

	exact = mdw_time_label(self, at, "%Y-%m-%d %H:%M %Z");
	g_string_append_printf(html, "<span class=\"price-age%s\"><time datetime=\"", stale ? " is-stale" : "");
	venture_html_escape_append(html, at);
	g_string_append(html, "\" title=\"Price as of ");
	venture_html_escape_append(html, exact);
	g_string_append(html, "\">");
	venture_html_escape_append(html, compact);
	g_string_append(html, "</time>");

	if (stale)
		g_string_append(html, "<span class=\"price-age-flag\">stale</span>");

	g_string_append(html, "</span>");
}

/* --- /market/browse ------------------------------------------------------- */

/* --- A row's last seven days ------------------------------------------------- */

/* --- Lists on a row: tags, "Add to list", the notice after ----------------- */

/*
 * The query-string names a list's notice travels in. A page carries them
 * only on the GET after an add or a remove, and every link and form that
 * keeps the question leaves them out (mdw_here()), so a notice is said
 * once.
 */
static const gchar *const mdw_list_notice_names[] = { "list_added", "list_removed", "list_new", NULL };

/* Every value a query string gives @name, joined with commas: a <select
 * multiple> sends one per choice, and the parsed parameters keep one. */
static gchar *
mdw_query_all(
	HtmxRequest	*request,
	const gchar	*name
){
	g_auto(GStrv) pairs = NULL;
	g_autoptr(GString) joined = NULL;
	const gchar *query;
	guint i;

	query = htmx_request_get_query(request);

	if (venture_string_is_empty(query))
		return NULL;

	pairs = g_strsplit(query, "&", -1);
	joined = g_string_new(NULL);

	for (i = 0; NULL != pairs[i]; i++)
	{
		g_auto(GStrv) pair = g_strsplit(pairs[i], "=", 2);
		g_autofree gchar *field = NULL;
		g_autofree gchar *text = NULL;

		if ((NULL == pair[0]) || (NULL == pair[1]))
			continue;

		/* A form encodes a space as '+'. */
		g_strdelimit(pair[0], "+", ' ');
		g_strdelimit(pair[1], "+", ' ');
		field = g_uri_unescape_string(pair[0], NULL);
		text = g_uri_unescape_string(pair[1], NULL);

		if ((0 != g_strcmp0(field, name)) || venture_string_is_empty(text))
			continue;

		if (joined->len > 0)
			g_string_append_c(joined, ',');

		g_string_append(joined, text);
	}

	return (joined->len > 0) ? g_string_free(g_steal_pointer(&joined), FALSE) : NULL;
}

/* @path and @query without the list notices' parameters. */
static gchar *
mdw_without_notice(
	const gchar	*path,
	const gchar	*query
){
	g_autoptr(GString) out = g_string_new(path);
	g_auto(GStrv) pairs = NULL;
	gboolean first = TRUE;
	guint i;

	if (venture_string_is_empty(query))
		return g_string_free(g_steal_pointer(&out), FALSE);

	pairs = g_strsplit(query, "&", -1);

	for (i = 0; NULL != pairs[i]; i++)
	{
		g_auto(GStrv) pair = g_strsplit(pairs[i], "=", 2);

		if (('\0' == pairs[i][0]) || g_strv_contains(mdw_list_notice_names, pair[0]))
			continue;

		g_string_append_c(out, first ? '?' : '&');
		g_string_append(out, pairs[i]);
		first = FALSE;
	}

	return g_string_free(g_steal_pointer(&out), FALSE);
}

/* Where this page is, as a list form sends the person back to it. */
static gchar *
mdw_here(HtmxRequest *request)
{
	return mdw_without_notice(htmx_request_get_path(request), htmx_request_get_query(request));
}

/*
 * Where a list form goes back to: the page it was posted from when that
 * is one of the Trading pages, else @fallback -- never another site, and
 * never a path the form made up. The notice is appended.
 */
static gchar *
mdw_list_back(
	const gchar	*asked,
	const gchar	*fallback,
	const gchar	*notice,
	gint64		 list_id,
	gboolean	 created
){
	g_autofree gchar *base = NULL;
	const gchar *target = fallback;

	if ((NULL != asked) && (g_str_has_prefix(asked, "/market/") || g_str_has_prefix(asked, "/arbitrage/")) &&
	    (NULL == strstr(asked, "//")) && (NULL == strchr(asked, '\\')) && (NULL == strpbrk(asked, "\r\n")))
		target = asked;

	{
		const gchar *mark = strchr(target, '?');
		g_autofree gchar *path = (NULL != mark) ? g_strndup(target, mark - target) : g_strdup(target);

		base = mdw_without_notice(path, (NULL != mark) ? mark + 1 : NULL);
	}

	return g_strdup_printf("%s%c%s=%" G_GINT64_FORMAT "%s", base, (NULL != strchr(base, '?')) ? '&' : '?',
	                       notice, list_id, created ? "&list_new=1" : "");
}

/* A 303 to @location: a form's POST answered with the page to GET. */
static HtmxResponse *
mdw_see_other(const gchar *location)
{
	HtmxResponse *redirect = htmx_response_new();

	htmx_response_set_status(redirect, 303);
	htmx_response_add_header(redirect, "Location", location);

	return redirect;
}

/*
 * After an add or a remove, what happened, named from @choices (the
 * answer's lists): "Added to Bag flips." A list nobody can name (deleted
 * since) is "the list".
 */
static void
mdw_list_notice(
	GString		*html,
	HtmxRequest	*request,
	JsonArray	*choices
){
	const gchar *added = mdw_param(request, "list_added");
	const gchar *removed = mdw_param(request, "list_removed");
	const gchar *which = (NULL != added) ? added : removed;
	const gchar *name = NULL;
	gint64 id = 0;
	guint i;

	if ((NULL == which) || !g_ascii_string_to_signed(which, 10, 1, G_MAXINT64, &id, NULL))
		return;

	for (i = 0; (NULL != choices) && (i < json_array_get_length(choices)); i++)
	{
		JsonObject *choice = json_array_get_object_element(choices, i);

		if (json_object_get_int_member_with_default(choice, "id", 0) == id)
			name = mdw_text(choice, "name");
	}

	g_string_append(html, "<div class=\"notice positive\" role=\"status\">");

	if (NULL != added)
		g_string_append(html, (NULL != mdw_param(request, "list_new")) ? "Made the list " : "Added to ");
	else
		g_string_append(html, "Removed from ");

	if (NULL != name)
	{
		g_string_append(html, "<strong>");
		venture_html_escape_append(html, name);
		g_string_append(html, "</strong>");
	}
	else
		g_string_append(html, "the list");

	g_string_append(html, ((NULL != added) && (NULL != mdw_param(request, "list_new"))) ? " and added it.</div>"
	                                                                                     : ".</div>");
}

/* The names a row's "Add to list" box offers: every list, by name. */
static void
mdw_list_datalist(
	GString		*html,
	const gchar	*id,
	JsonArray	*choices
){
	guint i;

	g_string_append_printf(html, "<datalist id=\"%s\">", id);

	for (i = 0; (NULL != choices) && (i < json_array_get_length(choices)); i++)
	{
		g_string_append(html, "<option value=\"");
		venture_html_escape_append(html, mdw_text(json_array_get_object_element(choices, i), "name"));
		g_string_append(html, "\">");
	}

	g_string_append(html, "</datalist>");
}

/*
 * A row's lists, each a tag linking to the list (@link_prefix and its
 * id) with a remove button that posts to @remove_action, and the "Add to
 * list" box posting to @add_action with @identity (the row's hidden
 * fields). One text box: a name the list offers is that list, a new name
 * makes one. A person who may not write sees the tags alone.
 */
static void
mdw_list_cell(
	GString		*html,
	JsonArray	*memberships,
	const gchar	*link_prefix,
	const gchar	*remove_action,
	const gchar	*add_action,
	const gchar	*identity,
	const gchar	*here,
	const gchar	*datalist_id,
	const gchar	*item_label,
	gboolean	 can_write
){
	guint i;

	g_string_append(html, "<div class=\"list-tags\">");

	for (i = 0; (NULL != memberships) && (i < json_array_get_length(memberships)); i++)
	{
		JsonObject *on = json_array_get_object_element(memberships, i);

		g_string_append_printf(html, "<span class=\"list-tag\"><a href=\"%s%" G_GINT64_FORMAT "\">", link_prefix,
		                       json_object_get_int_member_with_default(on, "id", 0));
		venture_html_escape_append(html, mdw_text(on, "name"));
		g_string_append(html, "</a>");

		if (can_write)
		{
			g_string_append_printf(html, "<form method=\"post\" action=\"%s\" class=\"list-tag-remove\">"
			                             "<input type=\"hidden\" name=\"entry_id\" value=\"%" G_GINT64_FORMAT "\">"
			                             "<input type=\"hidden\" name=\"return\" value=\"", remove_action,
			                       json_object_get_int_member_with_default(on, "entry_id", 0));
			venture_html_escape_append(html, here);
			g_string_append(html, "\"><button class=\"btn btn-sm btn-ghost\" type=\"submit\" aria-label=\"Remove ");
			venture_html_escape_append(html, (NULL != item_label) ? item_label : "it");
			g_string_append(html, " from ");
			venture_html_escape_append(html, mdw_text(on, "name"));
			g_string_append(html, "\">\xc3\x97</button></form>");
		}

		g_string_append(html, "</span>");
	}

	g_string_append(html, "</div>");

	if (!can_write)
		return;

	g_string_append_printf(html, "<form method=\"post\" action=\"%s\" class=\"list-add\">%s"
	                             "<input type=\"hidden\" name=\"return\" value=\"", add_action, identity);
	venture_html_escape_append(html, here);
	g_string_append_printf(html, "\"><input type=\"text\" name=\"list\" list=\"%s\" required maxlength=\"%d\" "
	                             "placeholder=\"list\" aria-label=\"Add ", datalist_id,
	                       VENTURE_MARKETDATA_LIST_NAME_MAX);
	venture_html_escape_append(html, (NULL != item_label) ? item_label : "it");
	g_string_append(html, " to a list: pick one or type a new name\"><button class=\"btn btn-sm\" type=\"submit\">"
	                      "Add</button></form>");
}

/*
 * A field of several choices as chips: a checkbox each, ticked for what
 * is chosen. The query is the one a <select multiple> sends -- one
 * name=value per choice -- without a box that is mostly empty, needs a
 * modifier key to pick two, and hides what is picked once it scrolls.
 */
static void
mdw_chips_open(
	GString		*html,
	const gchar	*label
){
	g_string_append(html, "<fieldset class=\"field chips\"><legend class=\"field-label\">");
	venture_html_escape_append(html, label);
	g_string_append(html, "</legend><div class=\"chips-row\">");
}

static void
mdw_chip(
	GString		*html,
	const gchar	*name,
	const gchar	*value,
	const gchar	*text,
	gboolean	 checked
){
	g_string_append(html, "<label class=\"chip\"><input type=\"checkbox\" name=\"");
	venture_html_escape_append(html, name);
	g_string_append(html, "\" value=\"");
	venture_html_escape_append(html, value);
	g_string_append_printf(html, "\"%s><span>", checked ? " checked" : "");
	venture_html_escape_append(html, text);
	g_string_append(html, "</span></label>");
}

static void
mdw_chips_close(GString *html)
{
	g_string_append(html, "</div></fieldset>");
}

/*
 * The filters a page keeps folded away: "More filters", open when the
 * question already sets any of @names -- so a filter in use is never
 * hidden -- and saying how many it sets. A name may carry the value a
 * form always sends for it, "only_known=0", which is not setting it.
 */
static void
mdw_more_open(
	GString			*html,
	HtmxRequest		*request,
	const gchar *const	*names
){
	guint set = 0;
	guint i;

	for (i = 0; NULL != names[i]; i++)
	{
		const gchar *equals = strchr(names[i], '=');
		g_autofree gchar *name = (NULL != equals) ? g_strndup(names[i], (gsize)(equals - names[i]))
		                                          : g_strdup(names[i]);
		g_autofree gchar *value = mdw_query_all(request, name);

		if (!venture_string_is_empty(value) && ((NULL == equals) || (0 != g_strcmp0(value, equals + 1))))
			set++;
	}

	g_string_append_printf(html, "<details class=\"form-more\"%s><summary>More filters", (set > 0) ? " open" : "");

	if (set > 0)
		g_string_append_printf(html, " <span class=\"badge\">%u set</span>", set);

	g_string_append(html, "</summary><div class=\"form-grid form-more-grid\">");
}

static void
mdw_more_close(GString *html)
{
	g_string_append(html, "</div></details>");
}

/* The Deals filters folded under "More filters". */
static const gchar *const mdw_deals_more[] = {
	"venue_group", "group", "min_value", "max_pct", "top", "cut", "min_sold_per_day", "horizon_days", NULL
};

/* The Deals "List" picker: several may be chosen, their union shown.
 * None at all, no field. */
static void
mdw_watchlist_picker(
	GString		*html,
	JsonObject	*root,
	const gchar	*chosen_text
){
	JsonArray *choices = json_object_has_member(root, "watchlist_choices")
		? json_object_get_array_member(root, "watchlist_choices") : NULL;
	g_auto(GStrv) chosen = g_strsplit((NULL != chosen_text) ? chosen_text : "", ",", -1);
	guint i;

	if ((NULL == choices) || (0 == json_array_get_length(choices)))
		return;

	mdw_chips_open(html, "List");

	for (i = 0; i < json_array_get_length(choices); i++)
	{
		JsonObject *choice = json_array_get_object_element(choices, i);
		g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT,
		                                       json_object_get_int_member_with_default(choice, "id", 0));

		mdw_chip(html, "watchlist", id, mdw_text(choice, "name"),
		         g_strv_contains((const gchar *const *)chosen, id));
	}

	mdw_chips_close(html);
}

/* A store row's lists and its "Add to list", as Deals and Browse draw
 * them: the instrument named by its source and key. */
static void
mdw_row_list_cell(
	GString		*html,
	JsonObject	*row,
	const gchar	*here,
	gboolean	 can_write
){
	g_autoptr(GString) identity = g_string_new(NULL);
	const gchar *key = mdw_text(row, "instrument_key");
	const gchar *label = (NULL != mdw_text(row, "instrument_name")) ? mdw_text(row, "instrument_name") : key;

	if (NULL == key)
	{
		mdw_dash(html);
		return;
	}

	g_string_append_printf(identity, "<input type=\"hidden\" name=\"data_source_id\" value=\"%" G_GINT64_FORMAT
	                                 "\"><input type=\"hidden\" name=\"key\" value=\"",
	                       json_object_get_int_member_with_default(row, "data_source_id", 0));
	venture_html_escape_append(identity, key);
	g_string_append(identity, "\">");
	mdw_list_cell(html, json_object_has_member(row, "watchlists") ? json_object_get_array_member(row, "watchlists")
	                                                              : NULL,
	              "/market/watchlists/", "/market/watchlists/remove", "/market/watchlists/add", identity->str, here,
	              "watchlist-names", label, can_write);
}

/* A field of a form, or of a JSON body, as text; NULL when absent. */
static gchar *
mdw_body_field(
	HtmxRequest	*request,
	JsonObject	*body,
	const gchar	*name
){
	if (NULL != body)
	{
		JsonNode *node = json_object_get_member(body, name);

		if ((NULL == node) || !JSON_NODE_HOLDS_VALUE(node))
			return NULL;

		if (G_TYPE_STRING == json_node_get_value_type(node))
			return g_strdup(json_node_get_string(node));

		if (G_TYPE_INT64 == json_node_get_value_type(node))
			return g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(node));

		return NULL;
	}

	return venture_string_is_empty(htmx_request_get_form_value(request, name))
		? NULL : g_strdup(htmx_request_get_form_value(request, name));
}

/* The first of @names a form or JSON body gives, as an id; 0 for none. */
static gboolean
mdw_body_id(
	HtmxRequest	 *request,
	JsonObject	 *body,
	const gchar *const *names,
	gint64		 *out,
	GError		**error
){
	guint i;

	*out = 0;

	for (i = 0; NULL != names[i]; i++)
	{
		g_autofree gchar *text = mdw_body_field(request, body, names[i]);

		if (NULL == text)
			continue;

		if (!g_ascii_string_to_signed(text, 10, 1, G_MAXINT64, out, NULL))
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "%s is a record's id", names[i]);
			return FALSE;
		}

		return TRUE;
	}

	return TRUE;
}

/* A JSON body when the request is not a form: the API's door. */
static JsonObject *
mdw_json_body(
	HtmxRequest	 *request,
	GError		**error
){
	g_autoptr(JsonNode) node = NULL;

	if (g_str_has_prefix((NULL != htmx_request_get_content_type(request))
	                     ? htmx_request_get_content_type(request) : "",
	                     "application/x-www-form-urlencoded"))
		return NULL;

	node = htmx_request_get_json(request, NULL);

	if ((NULL == node) || !JSON_NODE_HOLDS_OBJECT(node))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Send a JSON object, or a form");
		return NULL;
	}

	return json_object_ref(json_node_get_object(node));
}

/* What a list add or remove answers an API caller. */
static HtmxResponse *
mdw_list_answer(
	VentureEntity	*entry,
	gint64		 list_id,
	gboolean	 created_list,
	gboolean	 created_entry,
	gint64		 removed
){
	g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT);
	JsonObject *object = json_object_new();

	json_object_set_int_member(object, "list_id", list_id);

	if (NULL != entry)
	{
		json_object_set_member(object, "entry", venture_serializable_to_json(VENTURE_SERIALIZABLE(entry), FALSE));
		json_object_set_boolean_member(object, "created_list", created_list);
		json_object_set_boolean_member(object, "created_entry", created_entry);
	}
	else
		json_object_set_int_member(object, "removed", removed);

	json_node_take_object(node, object);

	return venture_web_json_response(node, ((NULL != entry) && created_entry) ? 201 : 200);
}

static gdouble mdw_double (JsonObject *object, const gchar *member);
static const gchar *mdw_tone (gdouble value, gdouble good_at, gdouble bad_at);
static void mdw_chip_open (GString *html, const gchar *tone);
static void mdw_sort_value (GString *html, JsonObject *object, const gchar *member);

/* The headings of the two cells mdw_recent_cells() draws. A sparkline
 * has nothing to sort by, so its heading opts out; so does the figure's
 * in a table paged on the server (@server_sorted), where sorting the rows
 * on screen would pass for an order of every page. */
static void
mdw_recent_headings(
	GString		*html,
	gboolean	 server_sorted
){
	g_string_append_printf(html, "<th scope=\"col\" data-no-sort>7 days</th>"
	                             "<th scope=\"col\" class=\"num\" %s "
	                             "title=\"The price against the median of the last seven days' hourly "
	                             "lowest prices at this venue\">vs 7-day median</th>",
	                       server_sorted ? "data-no-sort" : "data-sort-type=\"num\" data-sort-default=\"asc\"");
}

/*
 * A row's last seven days, as two cells: its lowest price in four-hour
 * blocks as a sparkline, and its price against the seven-day median -- a
 * dip is well under it, a new normal is at it. The figure is the
 * answer's (venture_marketdata_deals(), _browse()); the page only draws.
 */
static void
mdw_recent_cells(
	GString		*html,
	JsonObject	*row
){
	g_autoptr(GArray) values = NULL;
	g_autofree gchar *spark = NULL;
	JsonArray *trend;
	gdouble vs;
	guint i;

	trend = json_object_has_member(row, "trend_7d") ? json_object_get_array_member(row, "trend_7d") : NULL;
	values = g_array_new(FALSE, FALSE, sizeof(gdouble));

	for (i = 0; (NULL != trend) && (i < json_array_get_length(trend)); i++)
	{
		JsonNode *node = json_array_get_element(trend, i);
		gdouble value = ((NULL != node) && JSON_NODE_HOLDS_VALUE(node)) ? (gdouble)json_node_get_int(node) : NAN;

		g_array_append_val(values, value);
	}

	spark = venture_web_chart_sparkline("Lowest price every four hours, last 7 days",
	                                    (const gdouble *)(gpointer)values->data, values->len);
	g_string_append(html, "<td class=\"market-trend\">");
	g_string_append(html, spark);
	g_string_append(html, "</td><td class=\"num\"");
	mdw_sort_value(html, row, "vs_median_7d_pct");
	g_string_append(html, ">");
	vs = mdw_double(row, "vs_median_7d_pct");

	if (isnan(vs))
		g_string_append(html, "<span class=\"muted\" title=\"Fewer than twelve hours of prices "
		                      "in the last seven days\">too new</span>");
	else
	{
		gchar number[G_ASCII_DTOSTR_BUF_SIZE];

		/* Under the median by 15% or more reads as a dip; at or over it
		 * is no dip at all. */
		mdw_chip_open(html, mdw_tone(vs, -15.0, 0.0));
		g_string_append(html, g_ascii_formatd(number, sizeof(number), "%+.1f", vs));
		g_string_append(html, "%</span><div class=\"deal-sub muted\">median ");
		mdw_money(html, row, "median_7d");
		g_string_append(html, "</div>");
	}

	g_string_append(html, "</td>");
}

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
	query->venue_group = mdw_param(request, "venue_group");
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
		"source", "search", "category", "venue", "group", "venue_group", "stock", "per_page", NULL
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
	g_autofree gchar *here = NULL;
	gboolean can_list;
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
	mdw_category_picker(html, root, mdw_param(request, "category"));

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

	g_string_append(html, "</select></label></div>");
	mdw_venue_group_picker(html, root, query.venue_group);
	g_string_append_printf(html, "<div class=\"field\"><label><input type=\"checkbox\" name=\"stock\" value=\"1\"%s>"
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
	mdw_list_notice(html, request, json_object_get_array_member(root, "watchlist_choices"));
	mdw_notes(html, root);
	here = mdw_here(request);
	mdw_list_datalist(html, "watchlist-names", json_object_get_array_member(root, "watchlist_choices"));

	{
		g_autoptr(VentureAuthPrincipal) principal = venture_auth_authenticate(self->auth, request);

		can_list = venture_web_require_for_type(self, principal, VENTURE_TYPE_WATCHLIST_ENTRY,
		                                        VENTURE_USER_ROLE_EDITOR, NULL);
	}

	/* Paged on the server: a heading sorts every page there, not this one
	 * here (data-server-sort keeps the client-side sorter off it). */
	g_string_append(html, "<div class=\"card\"><div class=\"table-wrap\"><table class=\"data market-table\" "
	                      "data-server-sort><thead><tr>");
	mdw_sort_header(html, request, echo, "name", "Instrument", FALSE);
	mdw_sort_header(html, request, echo, "venue", "Venue", FALSE);
	mdw_sort_header(html, request, echo, "min_price", "Lowest", TRUE);
	mdw_sort_header(html, request, echo, "quantity", "Quantity", TRUE);
	mdw_sort_header(html, request, echo, "market_value", "Market value", TRUE);
	mdw_sort_header(html, request, echo, "region_median", "Region median", TRUE);
	mdw_sort_header(html, request, echo, "pct_vs_region", "Of region", TRUE);
	mdw_recent_headings(html, TRUE);
	mdw_sort_header(html, request, echo, "sale_rate", "Sale rate", TRUE);
	mdw_sort_header(html, request, echo, "updated", "Updated", FALSE);
	g_string_append(html, "<th scope=\"col\">Lists</th></tr></thead><tbody>");

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
		g_string_append(html, "</td>");
		mdw_recent_cells(html, row);
		g_string_append(html, "<td class=\"num\">");
		mdw_percent(html, row, "sale_rate", 100.0);
		g_string_append(html, "</td><td>");
		mdw_when(html, row, "taken_at");
		g_string_append(html, "</td><td class=\"list-cell\">");

		/* One source a page: its rows name it here. */
		if (!json_object_has_member(row, "data_source_id"))
			json_object_set_int_member(row, "data_source_id", source_id);

		mdw_row_list_cell(html, row, here, can_list);
		g_string_append(html, "</td></tr>");
	}

	if ((NULL == rows) || (0 == json_array_get_length(rows)))
		g_string_append(html, "<tr><td colspan=\"12\" class=\"muted\">Nothing matches.</td></tr>");

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

/* What a history's range is called on the page. */
static const gchar *
mdw_range_label(const gchar *range)
{
	if (0 == g_strcmp0(range, "24h"))
		return "24 hours";
	if (0 == g_strcmp0(range, "7d"))
		return "7 days";
	if (0 == g_strcmp0(range, "14d"))
		return "14 days";
	if (0 == g_strcmp0(range, "90d"))
		return "90 days";
	if (0 == g_strcmp0(range, "all"))
		return "everything stored";

	return range;
}

/* A link to the instrument's page at @venue with @range and @compare. */
static gchar *
mdw_history_link(
	const gchar	*self_path,
	const gchar	*venue,
	const gchar	*range,
	const gchar	*compare
){
	GString *link;

	link = g_string_new(self_path);
	g_string_append(link, "?range=");
	g_string_append_uri_escaped(link, range, NULL, FALSE);

	if (NULL != venue)
	{
		g_string_append(link, "&venue=");
		g_string_append_uri_escaped(link, venue, NULL, FALSE);
	}

	if (NULL != compare)
	{
		g_string_append(link, "&compare=");
		g_string_append_uri_escaped(link, compare, NULL, FALSE);
	}

	return g_string_free(link, FALSE);
}

/* A figure of a history point, or NAN for null or absent. */
static gdouble
mdw_point_figure(
	JsonObject	*point,
	const gchar	*member
){
	JsonNode *node = json_object_get_member(point, member);

	return ((NULL != node) && JSON_NODE_HOLDS_VALUE(node)) ? (gdouble)json_node_get_int(node) : NAN;
}

/*
 * The price history card: the charted venue's lowest price and market
 * value over the range asked for, its quantity against the right-hand
 * axis, and a second line to compare with when one was asked for (the
 * region's median, or another venue's lowest). The ranges are links and
 * the comparison a form, so it all works without script. The figures are
 * the answer's history (venture_marketdata_history()); the page draws.
 */
static void
mdw_history_card(
	VentureWebServer	*self,
	GString			*html,
	JsonObject		*root,
	const gchar		*self_path,
	const gchar		*name
){
	g_autoptr(GPtrArray) labels = NULL;
	g_autoptr(GArray) prices = NULL;
	g_autoptr(GArray) markets = NULL;
	g_autoptr(GArray) quantities = NULL;
	g_autoptr(GArray) others = NULL;
	g_autoptr(GHashTable) compared = NULL;
	g_autofree gchar *title = NULL;
	g_autofree gchar *other_name = NULL;
	g_autofree gchar *chart_html = NULL;
	VentureWebChartSeries series[4];
	VentureWebChart chart;
	JsonNode *node;
	JsonObject *history;
	JsonObject *compare;
	JsonArray *points;
	JsonArray *ranges;
	JsonArray *venues;
	const gchar *range;
	const gchar *currency;
	const gchar *venue;
	const gchar *compare_key;
	gboolean hourly;
	gsize n_series;
	guint i;

	node = json_object_get_member(root, "history");

	if ((NULL == node) || !JSON_NODE_HOLDS_OBJECT(node))
		return;

	history = json_node_get_object(node);
	range = mdw_text(history, "range");
	currency = mdw_text(history, "currency");
	venue = mdw_text(root, "venue");
	hourly = (0 == g_strcmp0(mdw_text(history, "resolution"), "hour"));
	points = json_object_get_array_member(history, "points");
	ranges = json_object_get_array_member(history, "ranges");
	node = json_object_get_member(history, "compare");
	compare = ((NULL != node) && JSON_NODE_HOLDS_OBJECT(node)) ? json_node_get_object(node) : NULL;
	compare_key = (NULL == compare) ? NULL
		: (0 == g_strcmp0(mdw_text(compare, "kind"), "region")) ? "region" : mdw_text(compare, "venue");

	g_string_append(html, "<div class=\"card market-history\"><div class=\"card-head\"><h2>Price history</h2>"
	                      "</div><div class=\"card-body\"><nav class=\"market-ranges\" aria-label=\"Range\">");

	for (i = 0; (NULL != ranges) && (i < json_array_get_length(ranges)); i++)
	{
		const gchar *one = json_array_get_string_element(ranges, i);
		g_autofree gchar *link = mdw_history_link(self_path, venue, one, compare_key);

		g_string_append(html, "<a class=\"market-range\" href=\"");
		venture_html_escape_append(html, link);
		g_string_append_printf(html, "\"%s>", (0 == g_strcmp0(one, range)) ? " aria-current=\"page\"" : "");
		venture_html_escape_append(html, one);
		g_string_append(html, "</a>");
	}

	/* The comparison: the region, or any other venue that lists it. */
	g_string_append(html, "<form method=\"get\" action=\"");
	venture_html_escape_append(html, self_path);
	g_string_append(html, "\" class=\"form-inline\"><input type=\"hidden\" name=\"range\" value=\"");
	venture_html_escape_append(html, range);
	g_string_append(html, "\">");

	if (NULL != venue)
	{
		g_string_append(html, "<input type=\"hidden\" name=\"venue\" value=\"");
		venture_html_escape_append(html, venue);
		g_string_append(html, "\">");
	}

	g_string_append_printf(html, "<label>Compare with <select name=\"compare\"><option value=\"\">nothing"
	                             "</option><option value=\"region\"%s>the region's median</option>",
	                       (0 == g_strcmp0(compare_key, "region")) ? " selected" : "");
	venues = json_object_get_array_member(root, "venues");

	for (i = 0; (NULL != venues) && (i < json_array_get_length(venues)); i++)
	{
		JsonObject *row = json_array_get_object_element(venues, i);
		const gchar *key = mdw_text(row, "venue_key");

		if (0 == g_strcmp0(key, venue))
			continue;

		g_string_append(html, "<option value=\"");
		venture_html_escape_append(html, key);
		g_string_append_printf(html, "\"%s>", (0 == g_strcmp0(key, compare_key)) ? " selected" : "");
		mdw_name(html, row, "venue_name", "venue_key");
		g_string_append(html, "</option>");
	}

	g_string_append(html, "</select></label> <button class=\"btn btn-sm\" type=\"submit\">Compare</button>"
	                      "</form></nav>");

	labels = g_ptr_array_new_with_free_func(g_free);
	prices = g_array_new(FALSE, FALSE, sizeof(gdouble));
	markets = g_array_new(FALSE, FALSE, sizeof(gdouble));
	quantities = g_array_new(FALSE, FALSE, sizeof(gdouble));
	others = g_array_new(FALSE, FALSE, sizeof(gdouble));

	/* The comparison by slot, so a line is never drawn a slot out. */
	if (NULL != compare)
	{
		JsonArray *other_points = json_object_get_array_member(compare, "points");

		compared = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);

		for (i = 0; (NULL != other_points) && (i < json_array_get_length(other_points)); i++)
		{
			JsonObject *point = json_array_get_object_element(other_points, i);
			gdouble *value = g_new(gdouble, 1);

			*value = mdw_point_figure(point, "min_price");

			if (NULL != mdw_text(point, "at"))
				g_hash_table_insert(compared, (gpointer)mdw_text(point, "at"), value);
			else
				g_free(value);
		}

		other_name = (0 == g_strcmp0(compare_key, "region"))
			? g_strdup("Region median")
			: g_strdup_printf("Lowest at %s", (NULL != mdw_text(compare, "venue_name"))
			                                  ? mdw_text(compare, "venue_name") : compare_key);
	}

	for (i = 0; (NULL != points) && (i < json_array_get_length(points)); i++)
	{
		JsonObject *point = json_array_get_object_element(points, i);
		gdouble value;

		g_ptr_array_add(labels, mdw_time_label(self, mdw_text(point, "at"), hourly ? "%b %d %H:00" : "%b %d"));
		value = mdw_point_figure(point, "min_price");
		g_array_append_val(prices, value);
		value = mdw_point_figure(point, "quantity");
		g_array_append_val(quantities, value);
		value = mdw_point_figure(point, "market_value");
		g_array_append_val(markets, value);

		if (NULL != compared)
		{
			const gdouble *other = (NULL != mdw_text(point, "at"))
				? g_hash_table_lookup(compared, mdw_text(point, "at")) : NULL;

			value = (NULL != other) ? *other : NAN;
			g_array_append_val(others, value);
		}
	}

	g_ptr_array_add(labels, NULL);
	title = g_strdup_printf("%s: lowest price, market value and quantity, %s", name, mdw_range_label(range));
	memset(series, 0, sizeof(series));
	series[0].name = "Lowest price";
	series[0].values = (const gdouble *)(gpointer)prices->data;
	series[0].format = venture_web_chart_format_minor;
	series[0].format_data = (gpointer)currency;
	series[1].name = "Quantity";
	series[1].values = (const gdouble *)(gpointer)quantities->data;
	series[1].secondary = TRUE;
	series[2].name = "Market value";
	series[2].values = (const gdouble *)(gpointer)markets->data;
	series[2].format = venture_web_chart_format_minor;
	series[2].format_data = (gpointer)currency;
	n_series = 3;

	if (NULL != compared)
	{
		series[3].name = other_name;
		series[3].values = (const gdouble *)(gpointer)others->data;
		series[3].format = venture_web_chart_format_minor;
		series[3].format_data = (gpointer)currency;
		n_series = 4;
	}

	memset(&chart, 0, sizeof(chart));
	chart.title = title;
	chart.summary = hourly
		? "The last snapshot of each hour at this venue. An hour nothing was stored for is a gap."
		: "Each day's lowest price at this venue, and the market value and quantity of its fullest "
		  "snapshot. A day nothing was stored for is a gap.";
	chart.labels = (const gchar *const *)labels->pdata;
	chart.n_points = prices->len;
	chart.series = series;
	chart.n_series = n_series;
	chart_html = venture_web_chart_line(&chart);
	g_string_append(html, chart_html);
	g_string_append(html, "</div></div>");
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
	query->venue_group = mdw_param(request, "venue_group");
	query->venues_descending = (0 == g_strcmp0(mdw_param(request, "venues_dir"), "desc"));
	query->range = mdw_param(request, "range");
	query->compare = mdw_param(request, "compare");

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
	g_string_append(html, "</nav><h1 class=\"item-title\">");

	{
		JsonObject *display = json_object_has_member(instrument, "display")
			? json_object_get_object_member(instrument, "display") : NULL;

		if ((NULL != display) && (NULL != mdw_text(display, "icon")))
		{
			g_string_append(html, "<img class=\"item-title-icon\" src=\"");
			venture_html_escape_append(html, mdw_text(display, "icon"));
			g_string_append(html, "\" width=\"40\" height=\"40\" alt=\"\">");
		}

		g_string_append(html, "<span");
		if ((NULL != display) && (NULL != mdw_text(display, "color")))
			g_string_append_printf(html, " style=\"color:%s\"", mdw_text(display, "color"));
		g_string_append(html, ">");
		venture_html_escape_append(html, name);
		g_string_append(html, "</span>");
	}

	g_string_append(html, "</h1><p class=\"muted\">");
	venture_html_escape_append(html, key);

	if (NULL != mdw_text(instrument, "variant"))
	{
		g_string_append(html, " \xc2\xb7 ");
		venture_html_escape_append(html, mdw_text(instrument, "variant"));
	}

	/* The plain item's level: a variant's bonuses move it by an amount the
	 * API does not publish, so it is labelled as the base. */
	if (json_object_has_member(instrument, "level"))
		g_string_append_printf(html, " \xc2\xb7 base item level %" G_GINT64_FORMAT,
		                       json_object_get_int_member(instrument, "level"));

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

	g_string_append(html, "</p>");

	if (json_object_has_member(root, "links") &&
	    (json_array_get_length(json_object_get_array_member(root, "links")) > 0))
	{
		g_string_append(html, "<p class=\"external-links\">");
		mdw_links(html, root, TRUE);
		g_string_append(html, "</p>");
	}

	g_string_append(html, "</div></div>");

	/* The item as the game shows it, drawn in place on its own page. */
	if (json_object_has_member(instrument, "display"))
	{
		g_string_append(html, "<div class=\"item-tip-static\">");
		mdw_tooltip_card(html, json_object_get_object_member(instrument, "display"), name, "is-static");
		g_string_append(html, "</div>");
	}

	/* At the top: the whole page is the source's data, and a line at the
	 * foot of the charts would be anything but conspicuous. */
	mdw_attribution(html, root);
	mdw_list_notice(html, request, json_object_get_array_member(root, "watchlists"));
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

		/* Its lists, and straight onto another: a name the box
		 * offers is that list, a new name makes one. */
		{
			g_autoptr(VentureAuthPrincipal) principal = venture_auth_authenticate(self->auth, request);
			JsonArray *on = json_object_has_member(root, "listed_on")
				? json_object_get_array_member(root, "listed_on") : NULL;
			gboolean can_write = venture_web_require_for_type(self, principal, VENTURE_TYPE_WATCHLIST_ENTRY,
			                                                  VENTURE_USER_ROLE_EDITOR, NULL);
			g_autofree gchar *here = mdw_here(request);

			g_string_append(html, "<div class=\"market-lists\"><span class=\"muted\">Lists</span>");
			mdw_list_datalist(html, "watchlist-names", lists);
			mdw_list_cell(html, on, "/market/watchlists/", "/market/watchlists/remove", self_path,
			              "<input type=\"hidden\" name=\"action\" value=\"watch\">", here, "watchlist-names",
			              name, can_write);
			g_string_append(html, "</div>");
		}
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

		/* A deal alert: told when this sells somewhere for enough more
		 * than it costs somewhere else, as Deals reckons it. */
		g_string_append(html, "<form method=\"post\" action=\"");
		venture_html_escape_append(html, self_path);
		g_string_append(html, "\" class=\"form-inline\"><input type=\"hidden\" name=\"action\" value=\"alert\">"
		                      "<input type=\"hidden\" name=\"kind\" value=\"deal\">");

		if (NULL != venue)
		{
			g_string_append(html, "<input type=\"hidden\" name=\"venue\" value=\"");
			venture_html_escape_append(html, venue);
			g_string_append(html, "\">");
		}

		g_string_append(html, "<button class=\"btn btn-sm\" type=\"submit\">Create a deal alert</button></form>");
		g_string_append(html, "</div></div>");
	}

	if (NULL != base)
	{
		g_autoptr(GString) value = g_string_new(NULL);

		g_string_append(html, "<div class=\"card\"><div class=\"card-head\"><h2>Now</h2></div>"
		                      "<div class=\"stat-row market-stats\">");
		mdw_stat_of(html, "Lowest price", mdw_money, base, "min_price");
		mdw_age(self, value, base, "");
		mdw_stat(html, "Price as of", value->str);
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

		/* Another source's view of the same item (TSM's AuctionDB, as
		 * tsmctl pushes it): drawn only when it has one, so an item
		 * it does not know shows nothing rather than a zero. */
		if (json_object_has_member(root, "ref"))
		{
			JsonObject *ref = json_object_get_object_member(root, "ref");
			g_autoptr(GString) head = g_string_new(NULL);

			g_string_append(head, "<div class=\"card\"><div class=\"card-head\"><h2>From ");
			venture_html_escape_append(head, mdw_text(ref, "source_name"));
			g_string_append(head, "</h2></div><div class=\"stat-row market-stats\">");
			g_string_append(html, head->str);
			mdw_stat_of(html, "Region market value", mdw_money, ref, "region_market");
			mdw_stat_of(html, "Lowest price against it", mdw_pct, ref, "buy_vs_region_pct");
			mdw_stat_of(html, "Region historical", mdw_money, ref, "region_historical");
			mdw_stat_of(html, "Region sold a day", mdw_decimal, ref, "region_sold_per_day");
			mdw_stat_of(html, "Region sale rate", mdw_share, ref, "region_sale_rate");
			mdw_stat_of(html, "This realm's market value", mdw_money, ref, "buy_realm_market");
			mdw_stat_of(html, "Lowest price against the realm's", mdw_pct, ref, "buy_vs_realm_pct");
			g_string_append(html, "</div></div>");
		}

		/* The pictures, each with its numbers in a table beside it. */
		mdw_history_card(self, html, root, self_path, name);

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

		const gchar *group_name = mdw_text(root, "venue_group_name");
		gboolean dearest = json_object_has_member(root, "venues_descending") &&
		                   json_object_get_boolean_member(root, "venues_descending");

		g_string_append(html, "<div class=\"card\"><div class=\"card-head\"><h2>");
		venture_html_escape_append(html, (NULL != group_name) ? group_name : "Every venue");
		g_string_append(html, dearest ? ", dearest first" : ", cheapest first");
		g_string_append(html, "</h2></div><form method=\"get\" action=\"");
		venture_html_escape_append(html, self_path);
		g_string_append(html, "\" class=\"form-grid market-filters\" role=\"search\" "
		                      "aria-label=\"Venue filters\">");

		if (NULL != query.venue)
		{
			g_string_append(html, "<input type=\"hidden\" name=\"venue\" value=\"");
			venture_html_escape_append(html, query.venue);
			g_string_append(html, "\">");
		}

		mdw_venue_group_picker(html, root, query.venue_group);
		g_string_append_printf(html, "<div class=\"field\"><label>Order<select name=\"venues_dir\">"
		                       "<option value=\"\"%s>cheapest first</option>"
		                       "<option value=\"desc\"%s>dearest first</option></select></label></div>"
		                       "<button class=\"btn\" type=\"submit\">Show</button></form>",
		                       dearest ? "" : " selected", dearest ? " selected" : "");
		g_string_append(html, "<div class=\"table-wrap\"><table class=\"data market-table\"><thead><tr>"
		                      "<th scope=\"col\">Venue</th><th scope=\"col\">Group</th>"
		                      "<th scope=\"col\" class=\"num\">Lowest</th><th scope=\"col\" class=\"num\">Quantity</th>"
		                      "<th scope=\"col\" class=\"num\">Market value</th>"
		                      "<th scope=\"col\" class=\"num\">Of region</th><th scope=\"col\">Updated</th>"
		                      "<th scope=\"col\">Elsewhere</th>"
		                      "</tr></thead><tbody>");

		for (i = 0; (NULL != venues) && (i < json_array_get_length(venues)); i++)
		{
			JsonObject *row = json_array_get_object_element(venues, i);
			gboolean current = (0 == g_strcmp0(mdw_text(row, "venue_key"), venue));

			g_string_append_printf(html, "<tr%s><td", current ? " aria-current=\"true\" class=\"selected\"" : "");
			mdw_stale_class(html, row, "");
			g_string_append(html, "><a href=\"");
			venture_html_escape_append(html, mdw_text(row, "url"));
			g_string_append(html, "\">");
			mdw_name(html, row, "venue_name", "venue_key");
			g_string_append(html, "</a></td><td>");
			venture_html_escape_append(html, mdw_text(row, "group_key"));
			g_string_append(html, "</td><td class=\"num");
			g_string_append(html, mdw_stale(row, "") ? " is-stale\">" : "\">");
			mdw_money(html, row, "min_price");
			g_string_append(html, "</td><td class=\"num\">");
			mdw_int(html, row, "quantity");
			g_string_append(html, "</td><td class=\"num\">");
			mdw_money(html, row, "market_value");
			g_string_append(html, "</td><td class=\"num\">");
			mdw_percent(html, row, "pct_vs_region", 1.0);
			g_string_append(html, "</td><td>");
			mdw_age(self, html, row, "");
			g_string_append(html, "</td><td>");
			mdw_links(html, row, FALSE);
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
 * page; onto a watchlist (watchlist_id, or `list` -- a name found or
 * made) and back to the page with a notice; or to the generic alert-rule
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
		/* Straight onto the list -- one named by id, or by a name
		 * found or made -- and back to the page with a notice; the
		 * targets are the list page's to set, entry by entry. */
		const gchar *list = htmx_request_get_form_value(request, "watchlist_id");
		const gchar *name = htmx_request_get_form_value(request, "list");
		g_autoptr(VentureEntity) entry = NULL;
		g_autofree gchar *here = NULL;
		gboolean created_list = FALSE;
		gint64 list_id = 0;
		gint64 entry_list = 0;

		if (!venture_string_is_empty(list) &&
		    !g_ascii_string_to_signed(list, 10, 1, G_MAXINT64, &list_id, NULL))
			list_id = 0;

		if (!venture_marketdata_list_put(self->context, venture_marketdata_watchlist_kind(), organization_id,
		                                 list_id, name, venture_entity_get_id(instrument), &actor, &entry,
		                                 &created_list, NULL, &error))
			return venture_web_error_response(error);

		g_object_get(entry, "watchlist-id", &entry_list, NULL);
		here = g_strdup_printf("/market/i/%" G_GINT64_FORMAT "/%s", source_id, escaped_key);
		target = mdw_list_back(here, here, "list_added", entry_list, created_list);
	}
	else if (0 == g_strcmp0(action, "alert"))
	{
		const gchar *venue = htmx_request_get_form_value(request, "venue");
		const gchar *kind = htmx_request_get_form_value(request, "kind");
		g_autoptr(GString) link = g_string_new(NULL);

		/* The rule's scope is the instrument; its venue's group is the
		 * likeliest narrowing, and the kind a buyer wants first is
		 * "below" -- or "deal", from a Deals row, with a fifth over the
		 * price paid as the return to start from. All of it stays
		 * editable on the form. */
		if (0 == g_strcmp0(kind, "deal"))
			g_string_append_printf(link, "/e/alert_rule/new?instrument_id=%" G_GINT64_FORMAT
			                       "&data_source_id=%" G_GINT64_FORMAT "&kind=deal&threshold_number=20",
			                       venture_entity_get_id(instrument), source_id);
		else
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

/* A number of the answer, or NAN when it is absent or null. */
static gdouble
mdw_double(
	JsonObject	*object,
	const gchar	*member
){
	JsonNode *node = (NULL != object) ? json_object_get_member(object, member) : NULL;

	return ((NULL != node) && JSON_NODE_HOLDS_VALUE(node)) ? json_node_get_double(node) : NAN;
}

/*
 * A figure as a chip coloured by how good it is for the reader: "good"
 * green, "meh" yellow, "bad" red. @good_at and @bad_at are the thresholds,
 * either way round: good_at > bad_at means more is better.
 */
static const gchar *
mdw_tone(
	gdouble	value,
	gdouble	good_at,
	gdouble	bad_at
){
	if (isnan(value))
		return NULL;

	if (good_at > bad_at)
		return (value >= good_at) ? "good" : (value > bad_at) ? "meh" : "bad";

	return (value <= good_at) ? "good" : (value < bad_at) ? "meh" : "bad";
}

static void
mdw_chip_open(
	GString		*html,
	const gchar	*tone
){
	g_string_append_printf(html, "<span class=\"tone-chip%s%s\">", (NULL != tone) ? " tone-" : "",
	                       (NULL != tone) ? tone : "");
}

/* A venue's name, short, its whole name on hover: a connected realm is
 * named for all its realms. */
/* What a venue is called on Deals: its connected realm, else its name,
 * else its key. */
static const gchar *
mdw_venue_label(JsonObject *row)
{
	if (NULL != mdw_text(row, "realm"))
		return mdw_text(row, "realm");

	return (NULL != mdw_text(row, "venue_name")) ? mdw_text(row, "venue_name") : mdw_text(row, "venue_key");
}

static void
mdw_venue_chip(
	GString		*html,
	JsonObject	*row
){
	const gchar *name = (NULL != mdw_text(row, "venue_name")) ? mdw_text(row, "venue_name")
	                                                          : mdw_text(row, "venue_key");
	/* The connected realm as the pickers name it, where the answer
	 * says; the source's own name stays in the title. */
	const gchar *shown = mdw_venue_label(row);

	g_string_append(html, "<span class=\"venue-chip\" title=\"");
	venture_html_escape_append(html, name);
	g_string_append(html, "\">");
	venture_html_escape_append(html, shown);
	g_string_append(html, "</span>");
}

/* A cell's sort value for the client-side sorter: a figure's raw number
 * (money in minor units), or nothing when it has none -- which sorts last.
 * The value is a number written here, never page text. */
static void
mdw_sort_value(
	GString		*html,
	JsonObject	*object,
	const gchar	*member
){
	JsonNode *node = (NULL != object) ? json_object_get_member(object, member) : NULL;

	if (NULL == node)
		return;

	if (JSON_NODE_HOLDS_OBJECT(node) && json_object_has_member(json_node_get_object(node), "amount"))
		g_string_append_printf(html, " data-sort-value=\"%" G_GINT64_FORMAT "\"",
		                       json_object_get_int_member(json_node_get_object(node), "amount"));
	else if (JSON_NODE_HOLDS_VALUE(node) && (G_TYPE_INT64 == json_node_get_value_type(node)))
		g_string_append_printf(html, " data-sort-value=\"%" G_GINT64_FORMAT "\"", json_node_get_int(node));
	else if (JSON_NODE_HOLDS_VALUE(node) && (G_TYPE_DOUBLE == json_node_get_value_type(node)))
	{
		gchar number[G_ASCII_DTOSTR_BUF_SIZE];

		g_string_append_printf(html, " data-sort-value=\"%s\"",
		                       g_ascii_dtostr(number, sizeof(number), json_node_get_double(node)));
	}
}

/* A cell's sort value as text (a name), escaped. */
static void
mdw_sort_text(
	GString		*html,
	const gchar	*text
){
	if (NULL == text)
		return;

	g_string_append(html, " data-sort-value=\"");
	venture_html_escape_append(html, text);
	g_string_append(html, "\"");
}

/*
 * "Add to plan" for one deal: the arbitrage scan's spread question that
 * finds exactly this flip -- its item, bought at its realm and sold at the
 * deal's sell realm, at the lowest listing -- and the spread's key, posted
 * to the arbitrage module's plan. The scan prices it again with each
 * venue's own fee model (where Deals took a flat cut), so what is planned
 * is what the books will see; a flip those fees turn into a loss is
 * refused as no longer there.
 */
static void
mdw_deal_plan_form(
	GString		*html,
	JsonObject	*row,
	JsonObject	*sell
){
	g_autofree gchar *key = NULL;
	gint64 source = json_object_get_int_member_with_default(row, "data_source_id", 0);
	gint64 available = json_object_get_int_member_with_default(row, "quantity", 0);
	const gchar *const fields[][2] = {
		{ "strategy", "spread" },
		{ "sell_basis", "min" },
		{ "instrument", mdw_text(row, "instrument_key") },
		{ "buy_venues", mdw_text(row, "venue_key") },
		{ "sell_venues", mdw_text(sell, "venue_key") },
	};
	guint i;

	key = g_strdup_printf("spread:%" G_GINT64_FORMAT ":%s>%" G_GINT64_FORMAT ":%s:%s", source,
	                      mdw_text(row, "venue_key"), source, mdw_text(sell, "venue_key"),
	                      mdw_text(row, "instrument_key"));
	g_string_append_printf(html, "<form method=\"post\" action=\"/arbitrage/plan/add\" class=\"deal-plan\">"
	                       "<input type=\"hidden\" name=\"data_source_id\" value=\"%" G_GINT64_FORMAT "\">",
	                       source);

	for (i = 0; i < G_N_ELEMENTS(fields); i++)
	{
		g_string_append_printf(html, "<input type=\"hidden\" name=\"%s\" value=\"", fields[i][0]);
		venture_html_escape_append(html, (NULL != fields[i][1]) ? fields[i][1] : "");
		g_string_append(html, "\">");
	}

	g_string_append(html, "<input type=\"hidden\" name=\"key\" value=\"");
	venture_html_escape_append(html, key);
	g_string_append(html, "\"><input type=\"number\" name=\"units\" value=\"1\" min=\"1\"");

	if (available > 0)
		g_string_append_printf(html, " max=\"%" G_GINT64_FORMAT "\"", available);

	g_string_append(html, " aria-label=\"Units to plan\"><button class=\"btn\" type=\"submit\">Add to plan</button>"
	                      "</form>");
}

/* Whether a deals column reads most-first unless asked otherwise (names,
 * realms, a buy price and a share of the region read smallest first). */
static gboolean
mdw_deals_descends(const gchar *sort)
{
	static const gchar *const ascending[] = { "deal", "name", "buy_at", "sell_at", "buy", "region", NULL };

	return (NULL != sort) && !g_strv_contains(ascending, sort);
}

/*
 * A deals column heading: a link sorting by it, keeping every other part
 * of the question; on the column already sorted by, the link turns it
 * round. aria-sort marks the active one.
 */
static void
mdw_deals_header(
	GString		*html,
	HtmxRequest	*request,
	const gchar	*current,
	const gchar	*current_dir,
	const gchar	*key,
	const gchar	*label,
	gboolean	 numeric
){
	static const gchar *const kept[] = {
		"source", "search", "venue_group", "venue", "sell_venue", "group", "category", "min_value", "max_pct", "top",
		"cut", "min_sold_per_day", "horizon_days", NULL
	};
	g_autoptr(GString) link = g_string_new("/market/deals?");
	g_autofree gchar *watchlists = mdw_query_all(request, "watchlist");
	gboolean active = (0 == g_strcmp0(current, key));
	gboolean descending = active
		? ((NULL != current_dir) ? (0 == g_strcmp0(current_dir, "desc")) : mdw_deals_descends(key))
		: FALSE;
	guint i;

	for (i = 0; NULL != kept[i]; i++)
	{
		const gchar *value = mdw_param(request, kept[i]);

		if (NULL == value)
			continue;

		g_string_append_printf(link, "%s=", kept[i]);
		g_string_append_uri_escaped(link, value, NULL, FALSE);
		g_string_append_c(link, '&');
	}

	/* The lists, joined: every value the page was given. */
	if (NULL != watchlists)
	{
		g_string_append(link, "watchlist=");
		g_string_append_uri_escaped(link, watchlists, ",", FALSE);
		g_string_append_c(link, '&');
	}

	g_string_append_printf(link, "sort=%s", key);

	/* Active: the other way. Not: its natural way, which needs no dir. */
	if (active)
		g_string_append_printf(link, "&dir=%s", descending ? "asc" : "desc");

	g_string_append_printf(html, "<th scope=\"col\"%s%s data-sort-type=\"%s\" data-sort-default=\"%s\">"
	                       "<a href=\"", numeric ? " class=\"num\"" : "",
	                       active ? (descending ? " aria-sort=\"descending\"" : " aria-sort=\"ascending\"") : "",
	                       numeric ? "num" : "text", mdw_deals_descends(key) ? "desc" : "asc");
	venture_html_escape_append(html, link->str);
	g_string_append(html, "\">");
	venture_html_escape_append(html, label);

	/* The arrow is drawn from aria-sort (CSS), so it follows the
	 * client-side sorter as well as the server's order. */
	g_string_append(html, "</a></th>");
}

/* A figure of the answer as a signed percent ("+12%", "-8%"), or a dash. */
static void
mdw_signed_pct(
	GString		*html,
	JsonObject	*object,
	const gchar	*member
){
	gdouble value = mdw_double(object, member);

	if (isnan(value))
	{
		mdw_dash(html);
		return;
	}

	g_string_append_printf(html, "%s%.0f%%", (value >= 0.5) ? "+" : (value <= -0.5) ? "\xe2\x88\x92" : "",
	                       fabs(value));
}

/* A query-string number that must be finite, or NAN when it is absent. */
static gboolean
mdw_real(
	HtmxRequest	 *request,
	const gchar	 *name,
	const gchar	 *what,
	gdouble		 *out,
	GError		**error
){
	const gchar *text = mdw_param(request, name);
	gchar *end = NULL;

	*out = NAN;

	if (NULL == text)
		return TRUE;

	*out = g_ascii_strtod(text, &end);

	if ((NULL == end) || ('\0' != *end) || !isfinite(*out))
	{
		g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "%s is %s", name, what);
		return FALSE;
	}

	return TRUE;
}

/*
 * A deals picker of where to buy or to sell: "anywhere", then the
 * answer's venue_choices -- only the venue group's venues when a group is
 * picked, so the list never offers a realm the group leaves out.
 */
static void
mdw_deal_venue_picker(
	GString		*html,
	JsonObject	*root,
	const gchar	*label,
	const gchar	*name,
	const gchar	*selected
){
	JsonArray *choices = json_object_has_member(root, "venue_choices")
		? json_object_get_array_member(root, "venue_choices") : NULL;
	guint i;

	g_string_append_printf(html, "<div class=\"field\"><label>%s<select name=\"%s\">"
	                             "<option value=\"\">%s</option>", label, name,
	                       (0 == g_strcmp0(name, "venue")) ? "any venue" : "the dearest venue");

	for (i = 0; (NULL != choices) && (i < json_array_get_length(choices)); i++)
	{
		const gchar *choice = json_object_get_string_member(json_array_get_object_element(choices, i), "name");

		g_string_append(html, "<option value=\"");
		venture_html_escape_append(html, choice);
		g_string_append_printf(html, "\"%s>", (0 == g_strcmp0(choice, selected)) ? " selected" : "");
		venture_html_escape_append(html, choice);
		g_string_append(html, "</option>");
	}

	g_string_append(html, "</select></label></div>");
}

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
	g_autofree gchar *watchlists = NULL;
	g_autofree gchar *here = NULL;
	gboolean api;
	gboolean can_plan;
	gboolean can_list = FALSE;
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

	/* A deal goes on the flip planner as a planned arbitrage trade: the
	 * column is offered only where the arbitrage module is on and the
	 * person may write trades. */
	{
		g_autoptr(VentureAuthPrincipal) principal = venture_auth_authenticate(self->auth, request);

		can_plan = !api && venture_context_module_enabled(self->context, "arbitrage") &&
		           venture_web_require_for_type(self, principal, VENTURE_TYPE_ARBITRAGE_TRADE,
		                                        VENTURE_USER_ROLE_EDITOR, NULL);

		/* And onto a list, where the person may write entries. */
		can_list = !api && venture_web_require_for_type(self, principal, VENTURE_TYPE_WATCHLIST_ENTRY,
		                                                VENTURE_USER_ROLE_EDITOR, NULL);
	}

	venture_marketdata_deals_query_init(&query);
	query.organization_id = organization_id;

	if (!mdw_number(request, "source", 0, G_MAXINT64, &query.data_source_id, &error) ||
	    !mdw_number(request, "top", 50, VENTURE_MARKETDATA_DEALS_MAX, &value, &error))
		return venture_web_error_response(error);

	query.count = (guint)value;
	query.venue = mdw_param(request, "venue");
	query.sell_venue = mdw_param(request, "sell_venue");
	query.group_key = mdw_param(request, "group");
	query.venue_group = mdw_param(request, "venue_group");
	query.search = mdw_param(request, "search");

	/* Lists: one parameter each, or one comma separated -- their union. */
	watchlists = mdw_query_all(request, "watchlist");
	query.watchlists = watchlists;
	query.category = mdw_param(request, "category");
	query.sort = mdw_param(request, "sort");
	query.dir = mdw_param(request, "dir");
	text = mdw_param(request, "cut");

	if (NULL != text)
	{
		gchar *end = NULL;

		query.cut_pct = g_ascii_strtod(text, &end);

		if ((NULL == end) || ('\0' != *end))
		{
			g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			            "cut is the marketplace's cut of a sale, a percent, e.g. 5");
			return venture_web_error_response(error);
		}
	}

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

	{
		gdouble horizon;

		if (!mdw_real(request, "min_sold_per_day", "the fewest units the sell venue sells a day, e.g. 2",
		              &query.min_sold_per_day, &error) ||
		    !mdw_real(request, "horizon_days", "the days a realisable profit counts sales over, e.g. 7",
		              &horizon, &error))
			return venture_web_error_response(error);

		if (!isnan(horizon))
			query.horizon_days = horizon;
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
	         "Where to buy and where to sell: in stock at a price at or under the group's deal price "
	         "(the median of the group's lowest prices, or the 33rd percentile when 15 or more venues "
	         "offer it), and the dearest venue with stock -- in the venue group, else the region -- to "
	         "sell it at, after the marketplace's cut.");

	g_string_append(html, "<div class=\"card\"><form method=\"get\" action=\"/market/deals\" "
	                      "class=\"form-grid market-filters\" role=\"search\" aria-label=\"Deal filters\">");
	mdw_source_picker(html, root, query.data_source_id, TRUE);
	mdw_input(html, request, "Search", "search", "name or key");
	mdw_deal_venue_picker(html, root, "Buy at", "venue", mdw_text(root, "buy_venue"));
	mdw_deal_venue_picker(html, root, "Sell at", "sell_venue", mdw_text(root, "sell_venue"));
	mdw_category_picker(html, root, mdw_param(request, "category"));
	mdw_watchlist_picker(html, root, watchlists);
	mdw_more_open(html, request, mdw_deals_more);
	mdw_venue_group_picker(html, root, query.venue_group);
	mdw_input(html, request, "Group", "group", "a group, e.g. eu");
	mdw_input(html, request, "Worth at least", "min_value", "e.g. 10.00 GOLD");
	mdw_input(html, request, "At most % of region", "max_pct", "e.g. 80");
	mdw_input(html, request, "How many", "top", "50");
	mdw_input(html, request, "Sale cut %", "cut", "5");
	mdw_input(html, request, "Sells at least, a day", "min_sold_per_day", "e.g. 2");
	mdw_input(html, request, "Over days", "horizon_days", "7");
	mdw_more_close(html);
	{
		const gchar *sort = query.sort;

		static const gchar *const sorts[][2] = {
			{ "deal", "best deal" }, { "realisable", "most realisable" }, { "profit", "most profit" },
			{ "roi", "best return" }, { "sold", "sells fastest" },
			{ "ilvl", "item level" }, { "name", "name" }, { "buy", "buy price" }, { "sell", "sell price" },
			{ "region", "of region" }, { "rate", "sale rate" }, { "qty", "quantity" },
			{ "buy_at", "buy realm" }, { "sell_at", "sell realm" },
		};
		guint k;

		g_string_append(html, "<div class=\"field\"><label>Sort by<select name=\"sort\">");

		for (k = 0; k < G_N_ELEMENTS(sorts); k++)
			g_string_append_printf(html, "<option value=\"%s\"%s>%s</option>", sorts[k][0],
			                       ((NULL == sort) ? (0 == k) : (0 == g_strcmp0(sort, sorts[k][0])))
			                       ? " selected" : "", sorts[k][1]);

		g_string_append(html, "</select></label></div>");

		if (NULL != query.dir)
		{
			g_string_append(html, "<input type=\"hidden\" name=\"dir\" value=\"");
			venture_html_escape_append(html, query.dir);
			g_string_append(html, "\">");
		}
	}
	g_string_append(html, "<button class=\"btn\" type=\"submit\">Show</button></form></div>");
	mdw_list_notice(html, request, json_object_get_array_member(root, "watchlist_choices"));
	mdw_notes(html, root);
	here = mdw_here(request);
	mdw_list_datalist(html, "watchlist-names", json_object_get_array_member(root, "watchlist_choices"));

	g_string_append(html, "<div class=\"card\"><div class=\"table-wrap\"><table class=\"data market-table deals-table\" data-client-sort>"
	                      "<thead><tr>");
	mdw_deals_header(html, request, query.sort, query.dir, "name", "Item", FALSE);
	mdw_deals_header(html, request, query.sort, query.dir, "ilvl", "ilvl", TRUE);
	mdw_deals_header(html, request, query.sort, query.dir, "buy_at", "Buy at", FALSE);
	mdw_deals_header(html, request, query.sort, query.dir, "buy", "Buy price", TRUE);
	mdw_recent_headings(html, FALSE);
	mdw_deals_header(html, request, query.sort, query.dir, "sell_at", "Sell at", FALSE);
	mdw_deals_header(html, request, query.sort, query.dir, "sell", "Sell price", TRUE);
	mdw_deals_header(html, request, query.sort, query.dir, "profit", "Profit", TRUE);
	mdw_deals_header(html, request, query.sort, query.dir, "region", "Of region", TRUE);
	mdw_deals_header(html, request, query.sort, query.dir, "rate", "Sale rate", TRUE);
	mdw_deals_header(html, request, query.sort, query.dir, "sold", "Sold a day", TRUE);
	mdw_deals_header(html, request, query.sort, query.dir, "realisable", "Realisable", TRUE);
	/* Another source's figure: sorted on the page only, by its own
	 * share of the buy price. */
	g_string_append(html, "<th scope=\"col\" class=\"num\" data-sort-type=\"num\" "
	                      "title=\"The region's market value from another source of the same items "
	                      "(TSM's AuctionDB, pushed by tsmctl), and the buy and sell prices against it\">"
	                      "Ref. value</th>");
	mdw_deals_header(html, request, query.sort, query.dir, "qty", "Qty", TRUE);
	g_string_append(html, "<th scope=\"col\"><span class=\"visually-hidden\">Alert</span></th>"
	                      "<th scope=\"col\">Lists</th>");

	if (can_plan)
		g_string_append(html, "<th scope=\"col\">Plan</th>");

	g_string_append(html, "</tr></thead><tbody>");

	for (i = 0; (NULL != rows) && (i < json_array_get_length(rows)); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);
		JsonObject *sell = json_object_has_member(row, "sell") ? json_object_get_object_member(row, "sell") : NULL;
		gdouble roi = mdw_double(row, "roi_pct");

		g_string_append(html, "<tr><td");
		mdw_sort_text(html, (NULL != mdw_text(row, "instrument_name")) ? mdw_text(row, "instrument_name")
		                                                               : mdw_text(row, "instrument_key"));
		g_string_append(html, "><a href=\"");
		venture_html_escape_append(html, mdw_text(row, "url"));
		g_string_append(html, "\">");
		mdw_name(html, row, "instrument_name", "instrument_key");
		g_string_append(html, "</a>");

		if (NULL != mdw_text(row, "category"))
		{
			g_string_append(html, "<div class=\"muted deal-sub\">");
			venture_html_escape_append(html, mdw_text(row, "category"));
			g_string_append(html, "</div>");
		}

		g_string_append(html, "</td><td class=\"num\"");
		mdw_sort_value(html, row, "level");
		g_string_append(html, ">");
		if (json_object_has_member(row, "level"))
			g_string_append_printf(html, "%" G_GINT64_FORMAT, json_object_get_int_member(row, "level"));
		g_string_append(html, "</td><td");
		mdw_stale_class(html, row, "buy_");
		mdw_sort_text(html, mdw_venue_label(row));
		g_string_append(html, ">");
		mdw_venue_chip(html, row);
		mdw_age(self, html, row, "buy_");
		g_string_append(html, "</td><td class=\"num");
		g_string_append(html, mdw_stale(row, "buy_") ? " is-stale\"" : "\"");
		mdw_sort_value(html, row, "min_price");
		g_string_append(html, ">");
		mdw_money(html, row, "min_price");
		g_string_append(html, "</td>");
		mdw_recent_cells(html, row);
		g_string_append(html, "<td");

		if (NULL != sell)
		{
			mdw_stale_class(html, row, "sell_");
			mdw_sort_text(html, mdw_venue_label(sell));
			g_string_append(html, ">");
			mdw_venue_chip(html, sell);
			mdw_age(self, html, row, "sell_");
			g_string_append(html, "</td><td class=\"num");
			g_string_append(html, mdw_stale(row, "sell_") ? " is-stale\"" : "\"");
			mdw_sort_value(html, sell, "min_price");
			g_string_append(html, ">");
			mdw_money(html, sell, "min_price");
			g_string_append(html, "</td><td class=\"num\"");
			mdw_sort_value(html, row, "profit");
			g_string_append(html, ">");
			mdw_chip_open(html, mdw_tone(roi, 20.0, 0.0));
			mdw_money(html, row, "profit");
			g_string_append(html, "</span><div class=\"deal-sub\">");
			mdw_chip_open(html, mdw_tone(roi, 20.0, 0.0));
			mdw_percent(html, row, "roi_pct", 1.0);
			g_string_append(html, "</span></div>");
		}
		else
			g_string_append(html, "><span class=\"muted\">nowhere it sells</span></td><td class=\"num\">"
			                      "&mdash;</td><td class=\"num\">&mdash;");

		g_string_append(html, "</td><td class=\"num\"");
		mdw_sort_value(html, row, "pct_vs_region");
		g_string_append(html, ">");
		mdw_chip_open(html, mdw_tone(mdw_double(row, "pct_vs_region"), 70.0, 90.0));
		mdw_percent(html, row, "pct_vs_region", 1.0);
		g_string_append(html, "</span></td><td class=\"num\"");
		mdw_sort_value(html, NULL != sell ? sell : row, "sale_rate");
		g_string_append(html, ">");
		mdw_chip_open(html, mdw_tone(mdw_double(NULL != sell ? sell : row, "sale_rate"), 0.3, 0.1));
		mdw_percent(html, NULL != sell ? sell : row, "sale_rate", 100.0);
		g_string_append(html, "</span></td>");

		/* How fast it sells where it is sold: the store's own estimate
		 * over fourteen days, sold vanished listings less expired ones. */
		g_string_append(html, "<td class=\"num\"");
		mdw_sort_value(html, row, "sell_sold_per_day");
		g_string_append(html, ">");

		if (NULL != sell)
		{
			mdw_chip_open(html, mdw_tone(mdw_double(row, "sell_sold_per_day"), 5.0, 1.0));
			if (!isnan(mdw_double(row, "sell_sold_per_day")))
				g_string_append(html, "~");
			mdw_decimal(html, row, "sell_sold_per_day");
			g_string_append(html, "</span>");
		}
		else
			mdw_dash(html);

		/* What it can make over the horizon: the profitable units, no
		 * more than are expected to sell. */
		g_string_append(html, "</td><td class=\"num\"");
		mdw_sort_value(html, row, "realisable_profit");
		g_string_append(html, ">");

		if ((NULL != sell) && json_object_has_member(row, "realisable_profit") &&
		    !JSON_NODE_HOLDS_NULL(json_object_get_member(row, "realisable_profit")))
		{
			mdw_money(html, row, "realisable_profit");
			g_string_append(html, "<div class=\"muted deal-sub\">");
			mdw_int(html, row, "realisable_units");
			g_string_append(html, " of ");
			mdw_int(html, row, "expected_sales");
			g_string_append(html, " sold</div>");
		}
		else
			mdw_dash(html);

		/* Another source's view (TSM): nothing at all when it has none. */
		g_string_append(html, "</td><td class=\"num\"");

		{
			JsonObject *ref = json_object_has_member(row, "ref") ? json_object_get_object_member(row, "ref")
			                                                     : NULL;
			gboolean region = (NULL != ref) && json_object_has_member(ref, "region_market") &&
			                  JSON_NODE_HOLDS_OBJECT(json_object_get_member(ref, "region_market"));

			if (NULL != ref)
				mdw_sort_value(html, ref, region ? "region_market" : "buy_realm_market");
			g_string_append(html, ">");

			if (NULL != ref)
			{
				g_string_append(html, "<span title=\"");
				venture_html_escape_append(html, mdw_text(ref, "source_name"));
				g_string_append(html, region ? ": region market value\">" : ": the buy realm's market value\">");
				mdw_money(html, ref, region ? "region_market" : "buy_realm_market");
				g_string_append(html, "</span><div class=\"muted deal-sub\">buy ");
				mdw_signed_pct(html, ref, region ? "buy_vs_region_pct" : "buy_vs_realm_pct");

				if (NULL != sell)
				{
					g_string_append(html, " \xc2\xb7 sell ");
					mdw_signed_pct(html, ref, region ? "sell_vs_region_pct" : "sell_vs_realm_pct");
				}

				g_string_append(html, "</div>");
			}
		}

		g_string_append(html, "</td><td class=\"num\"");
		mdw_sort_value(html, row, "quantity");
		g_string_append(html, ">");
		mdw_int(html, row, "quantity");
		g_string_append(html, "</td><td>");

		/* Told the next time this is a deal, on any realm: the item page's
		 * own "alert" action, which promotes the item and opens a deal
		 * rule on it. A post, because promoting is a write. */
		if (json_object_has_member(row, "data_source_id") && (NULL != mdw_text(row, "instrument_key")))
		{
			g_autofree gchar *item_path = venture_marketdata_instrument_path(
				json_object_get_int_member(row, "data_source_id"), mdw_text(row, "instrument_key"), NULL);

			g_string_append(html, "<form method=\"post\" action=\"");
			venture_html_escape_append(html, item_path);
			g_string_append(html, "\" class=\"form-inline\"><input type=\"hidden\" name=\"action\" "
			                      "value=\"alert\"><input type=\"hidden\" name=\"kind\" value=\"deal\">"
			                      "<input type=\"hidden\" name=\"venue\" value=\"");
			venture_html_escape_append(html, mdw_text(row, "venue_key"));
			g_string_append(html, "\"><button class=\"btn btn-sm\" type=\"submit\" title=\"Tell me when this "
			                      "is a deal on any realm\">Alert</button></form>");
		}

		/* The lists it is on, and "Add to list". */
		g_string_append(html, "</td><td class=\"list-cell\">");
		mdw_row_list_cell(html, row, here, can_list);
		g_string_append(html, "</td>");

		if (can_plan)
		{
			g_string_append(html, "<td>");

			if (NULL != sell)
				mdw_deal_plan_form(html, row, sell);
			else
				mdw_dash(html);

			g_string_append(html, "</td>");
		}

		g_string_append(html, "</tr>");
	}

	if ((NULL == rows) || (0 == json_array_get_length(rows)))
		g_string_append_printf(html, "<tr><td colspan=\"%d\" class=\"muted\">%s</td></tr>",
		                       can_plan ? 18 : 17,
		                       (NULL != watchlists) ? "Nothing on the list is a deal right now." : "No deals right now.");

	g_string_append(html, "</tbody></table></div>");

	/* Buying every row once, selling each where it sells dearest. */
	if (json_object_has_member(root, "totals"))
	{
		JsonObject *totals = json_object_get_object_member(root, "totals");

		g_string_append(html, "<div class=\"deal-totals\"><div class=\"deal-total tone-bad\"><span>Total investment</span>");
		mdw_money(html, totals, "investment");
		g_string_append(html, "</div><div class=\"deal-total tone-meh\"><span>Estimated sale, after the cut</span>");
		mdw_money(html, totals, "sale");
		g_string_append_printf(html, "</div><div class=\"deal-total tone-good\"><span>Profit, one of each of %"
		                       G_GINT64_FORMAT " deals</span>", json_object_get_int_member(totals, "rows"));
		mdw_money(html, totals, "profit");

		if (JSON_NODE_HOLDS_OBJECT(json_object_get_member(totals, "realisable_profit")))
		{
			g_string_append_printf(html, "</div><div class=\"deal-total tone-good\"><span>Realisable over %g "
			                       "days</span>", json_object_get_double_member(root, "horizon_days"));
			mdw_money(html, totals, "realisable_profit");
		}

		g_string_append(html, "</div></div>");
	}

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
	                      "<th scope=\"col\"><span class=\"visually-hidden\">Deals</span></th>"
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
		g_string_append_printf(html, "</td><td><a href=\"/market/deals?watchlist=%" G_GINT64_FORMAT "\">"
		                       "Open in Deals</a></td></tr>", json_object_get_int_member(list, "id"));
	}

	if ((NULL == lists) || (0 == json_array_get_length(lists)))
		g_string_append(html, "<tr><td colspan=\"4\" class=\"muted\">No watchlists yet. Add an item to "
		                      "a new list from a Deals or Browse row, or from the item's page.</td></tr>");

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
	g_autofree gchar *here = NULL;
	const gchar *group;
	gint64 id;
	gboolean api;
	gboolean can_write = FALSE;
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

	g_string_append_printf(html, "</p></div><div class=\"page-actions\"><a class=\"btn btn-primary\" "
	                       "href=\"/market/deals?watchlist=%" G_GINT64_FORMAT "\">Open in Deals</a>"
	                       "<a class=\"btn\" href=\"/e/watchlist/%" G_GINT64_FORMAT "\">Edit list</a>"
	                       "<a class=\"btn\" href=\"/e/watchlist_entry/new?watchlist_id=%" G_GINT64_FORMAT "\">"
	                       "Add an instrument</a></div></div>", id, id, id);
	mdw_list_notice(html, request, NULL);
	mdw_notes(html, root);

	{
		g_autoptr(VentureAuthPrincipal) principal = venture_auth_authenticate(self->auth, request);

		can_write = venture_web_require_for_type(self, principal, VENTURE_TYPE_WATCHLIST_ENTRY,
		                                         VENTURE_USER_ROLE_EDITOR, NULL);
	}

	here = mdw_here(request);

	g_string_append(html, "<div class=\"card\"><div class=\"table-wrap\"><table class=\"data market-table\">"
	                      "<thead><tr><th scope=\"col\">Instrument</th><th scope=\"col\">Cheapest at</th>"
	                      "<th scope=\"col\" class=\"num\">Lowest</th><th scope=\"col\" class=\"num\">Quantity</th>"
	                      "<th scope=\"col\" class=\"num\">Buy at</th><th scope=\"col\" class=\"num\">Against buy</th>"
	                      "<th scope=\"col\" class=\"num\">Sell at</th><th scope=\"col\" class=\"num\">Against sell</th>"
	                      "<th scope=\"col\">14 days</th><th scope=\"col\">Now</th>");

	if (can_write)
		g_string_append(html, "<th scope=\"col\"><span class=\"visually-hidden\">Remove</span></th>");

	g_string_append(html, "</tr></thead><tbody>");

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

		g_string_append(html, "</td>");

		/* Off the list: the entry goes, the instrument record stays. */
		if (can_write)
		{
			g_string_append_printf(html, "<td><form method=\"post\" action=\"/market/watchlists/remove\" "
			                             "class=\"list-tag-remove\"><input type=\"hidden\" name=\"entry_id\" "
			                             "value=\"%" G_GINT64_FORMAT "\"><input type=\"hidden\" name=\"return\" "
			                             "value=\"", json_object_get_int_member(entry, "id"));
			venture_html_escape_append(html, here);
			g_string_append(html, "\"><button class=\"btn btn-sm\" type=\"submit\" aria-label=\"Remove ");
			venture_html_escape_append(html, (NULL != name) ? name : "it");
			g_string_append(html, " from this list\">Remove</button></form></td>");
		}

		g_string_append(html, "</tr>");
	}

	if ((NULL == entries) || (0 == json_array_get_length(entries)))
		g_string_append_printf(html, "<tr><td colspan=\"%d\" class=\"muted\">Nothing on this list yet: add "
		                       "items from a Deals or Browse row, or from the item's page.</td></tr>",
		                       can_write ? 11 : 10);

	g_string_append(html, "</tbody></table></div></div>");
	mdw_attribution(html, root);

	return venture_web_html_response(venture_web_page(self, request, "/market/watchlists",
	                                                  mdw_text(list, "name"), html->str), 200);
}

/*
 * POST /market/watchlists/add: an instrument onto a list, from a row's
 * "Add to list" -- data_source_id and key name the store's instrument,
 * list_id (or watchlist_id) a list, or `list` a name, found ignoring
 * case or made. Back to `return` (the page it came from, its filters
 * kept) with a notice; the API twin answers the entry.
 */
static HtmxResponse *
venture_marketdata_web_watchlist_add(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	static const gchar *const list_ids[] = { "list_id", "watchlist_id", NULL };
	static const gchar *const source_ids[] = { "data_source_id", "source", NULL };
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) body = NULL;
	g_autoptr(VentureEntity) entry = NULL;
	g_autofree gchar *key = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *back = NULL;
	g_autofree gchar *asked = NULL;
	g_autofree gchar *fallback = NULL;
	VentureActor actor;
	gboolean api;
	gboolean created_list = FALSE;
	gboolean created_entry = FALSE;
	gint64 organization_id;
	gint64 list_id;
	gint64 source_id;
	gint64 entry_list = 0;
	HtmxResponse *gate;

	(void)params;

	api = mdw_is_api(request);
	gate = mdw_gate(self, request, api, VENTURE_USER_ROLE_EDITOR, VENTURE_TYPE_WATCHLIST_ENTRY);

	if (NULL != gate)
		return gate;

	body = mdw_json_body(request, &error);

	if (NULL != error)
		return venture_web_error_response(error);

	if (!venture_web_request_organization(self, request, body, &organization_id, &error))
		return venture_web_error_response(error);

	if (!mdw_body_id(request, body, list_ids, &list_id, &error) ||
	    !mdw_body_id(request, body, source_ids, &source_id, &error))
		return venture_web_error_response(error);

	key = mdw_body_field(request, body, "key");
	name = mdw_body_field(request, body, "list");

	if (NULL == name)
		name = mdw_body_field(request, body, "watchlist_name");

	if ((source_id <= 0) || venture_string_is_empty(key))
	{
		g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
		                    "Name the instrument: data_source_id and its key");
		return venture_web_error_response(error);
	}

	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);

	if (!venture_marketdata_watchlist_add(self->context, organization_id, list_id, name, source_id, key, &actor,
	                                      &entry, &created_list, &created_entry, &error))
		return venture_web_error_response(error);

	g_object_get(entry, "watchlist-id", &entry_list, NULL);

	if (api)
		return mdw_list_answer(entry, entry_list, created_list, created_entry, 0);

	asked = mdw_body_field(request, body, "return");
	fallback = venture_marketdata_instrument_path(source_id, key, NULL);
	back = mdw_list_back(asked, fallback, "list_added", entry_list, created_list);

	return mdw_see_other(back);
}

/* POST /market/watchlists/remove: entry_id off its list, back to `return`. */
static HtmxResponse *
venture_marketdata_web_watchlist_remove(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	static const gchar *const entry_ids[] = { "entry_id", NULL };
	VentureWebServer *self = data;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonObject) body = NULL;
	g_autofree gchar *asked = NULL;
	g_autofree gchar *back = NULL;
	g_autofree gchar *fallback = NULL;
	VentureActor actor;
	gboolean api;
	gint64 organization_id;
	gint64 entry_id;
	gint64 list_id = 0;
	HtmxResponse *gate;

	(void)params;

	api = mdw_is_api(request);
	gate = mdw_gate(self, request, api, VENTURE_USER_ROLE_EDITOR, VENTURE_TYPE_WATCHLIST_ENTRY);

	if (NULL != gate)
		return gate;

	body = mdw_json_body(request, &error);

	if (NULL != error)
		return venture_web_error_response(error);

	if (!venture_web_request_organization(self, request, body, &organization_id, &error) ||
	    !mdw_body_id(request, body, entry_ids, &entry_id, &error))
		return venture_web_error_response(error);

	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);

	if (!venture_marketdata_list_take(self->context, venture_marketdata_watchlist_kind(), organization_id, entry_id,
	                                  &actor, &list_id, &error))
		return venture_web_error_response(error);

	if (api)
		return mdw_list_answer(NULL, list_id, FALSE, FALSE, entry_id);

	asked = mdw_body_field(request, body, "return");
	fallback = g_strdup_printf("/market/watchlists/%" G_GINT64_FORMAT, list_id);
	back = mdw_list_back(asked, fallback, "list_removed", list_id, FALSE);

	return mdw_see_other(back);
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
	                    "href=\"/e/alert_rule/new\">New rule</a><a class=\"btn\" "
	                    "href=\"/e/alert_rule/new?kind=below\">New price alert</a><a class=\"btn\" "
	                    "href=\"/e/alert_rule/new?kind=deal&amp;threshold_number=20\">New deal alert</a>"
	                    "</div></div>");

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

/* --- /market/find ---------------------------------------------------------- */

/*
 * The item finder: every item whose name contains the search, each variant
 * on its own row, with where it is cheapest and dearest to buy -- which is
 * where it is cheapest and dearest to sell against -- in every venue or a
 * venue group. Each row links to the instrument page, whose venue table
 * lists every venue in either order.
 */
static HtmxResponse *
venture_marketdata_web_find(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GString) html = NULL;
	VentureMarketdataFindQuery query;
	JsonObject *root = NULL;
	JsonArray *items;
	gboolean api;
	HtmxResponse *gate;
	guint i;

	(void)params;

	api = mdw_is_api(request);
	gate = mdw_gate(self, request, api, VENTURE_USER_ROLE_VIEWER, VENTURE_TYPE_DATA_SOURCE);

	if (NULL != gate)
		return gate;

	memset(&query, 0, sizeof(query));

	if (!venture_web_request_organization(self, request, NULL, &query.organization_id, &error) ||
	    !mdw_number(request, "source", 0, G_MAXINT64, &query.data_source_id, &error))
		return venture_web_error_response(error);

	query.search = mdw_param(request, "search");
	query.venue_group = mdw_param(request, "venue_group");
	query.category = mdw_param(request, "category");

	/* No search yet is a form to fill in, not an error -- except for the
	 * API, where a question without its subject is a bad request. */
	if ((NULL != query.search) || (NULL != query.category) || api)
	{
		answer = venture_marketdata_find(self->context, &query, &error);

		if (NULL == answer)
			return venture_web_error_response(error);

		if (api)
			return venture_web_json_response(answer, 200);

		root = json_node_get_object(answer);
	}

	html = g_string_new(NULL);
	mdw_head(html, "Trading", "Find an item",
	         "Every item whose name contains what you type, each variant on its own row, with the "
	         "venue where it is cheapest and the one where it is dearest right now. Open one for "
	         "every venue in order.");

	g_string_append(html, "<div class=\"card\"><form method=\"get\" action=\"/market/find\" "
	                      "class=\"form-grid market-filters\" role=\"search\" aria-label=\"Find an item\">");

	if (NULL != root)
		mdw_source_picker(html, root, json_object_get_int_member(root, "data_source_id"), FALSE);

	mdw_input(html, request, "Item", "search", "part of a name, e.g. Sunfire Silk");

	if (NULL != root)
	{
		mdw_category_picker(html, root, query.category);
		mdw_venue_group_picker(html, root, query.venue_group);
	}
	else
	{
		/* Before the first search there is no answer to list the
		 * choices from, so they are asked for on their own. */
		g_autoptr(JsonNode) choices = venture_marketdata_picker_choices(self->context,
		                                                              query.organization_id,
		                                                              query.data_source_id);

		mdw_category_picker(html, json_node_get_object(choices), query.category);
		mdw_venue_group_picker(html, json_node_get_object(choices), query.venue_group);
	}

	g_string_append(html, "<button class=\"btn\" type=\"submit\">Find</button></form></div>");

	if (NULL == root)
		return venture_web_html_response(venture_web_page(self, request, "/market/find",
		                                                  "Find an item", html->str), 200);

	mdw_notes(html, root);
	items = json_object_get_array_member(root, "items");

	g_string_append(html, "<div class=\"card\"><div class=\"table-wrap\"><table class=\"data market-table\">"
	                      "<thead><tr><th scope=\"col\">Item</th><th scope=\"col\" class=\"num\">Base ilvl</th>"
	                      "<th scope=\"col\" class=\"num\">Venues</th>"
	                      "<th scope=\"col\">Cheapest at</th><th scope=\"col\" class=\"num\">Lowest</th>"
	                      "<th scope=\"col\">Dearest at</th><th scope=\"col\" class=\"num\">Lowest</th>"
	                      "</tr></thead><tbody>");

	for (i = 0; (NULL != items) && (i < json_array_get_length(items)); i++)
	{
		JsonObject *item = json_array_get_object_element(items, i);
		JsonObject *cheapest = json_object_get_object_member(item, "cheapest");
		JsonObject *dearest = json_object_get_object_member(item, "dearest");
		g_autofree gchar *link = NULL;

		/* The instrument page in the same venue group, dearest first --
		 * the order a seller wants. */
		link = g_strdup_printf("%s?venues_dir=desc%s%s", mdw_text(item, "url"),
		                       (NULL != query.venue_group) ? "&venue_group=" : "",
		                       (NULL != query.venue_group) ? query.venue_group : "");

		g_string_append(html, "<tr><td><a href=\"");
		venture_html_escape_append(html, link);
		g_string_append(html, "\">");
		mdw_name(html, item, "name", "key");
		g_string_append(html, "</a></td><td class=\"num\">");

		if (json_object_has_member(item, "level"))
			g_string_append_printf(html, "%" G_GINT64_FORMAT, json_object_get_int_member(item, "level"));

		g_string_append(html, "</td><td class=\"num\">");
		mdw_int(html, item, "venues");
		g_string_append(html, "</td><td>");
		mdw_name(html, cheapest, "venue_name", "venue_key");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_money(html, cheapest, "min_price");
		g_string_append(html, "</td><td>");
		mdw_name(html, dearest, "venue_name", "venue_key");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_money(html, dearest, "min_price");
		g_string_append(html, "</td></tr>");
	}

	if ((NULL == items) || (0 == json_array_get_length(items)))
		g_string_append(html, "<tr><td colspan=\"7\" class=\"muted\">Nothing in stock matches.</td></tr>");

	g_string_append(html, "</tbody></table></div></div>");

	return venture_web_html_response(venture_web_page(self, request, "/market/find", "Find an item",
	                                                  html->str), 200);
}

/* --- /api/v1/market/history/:source/KEY ------------------------------------ */

/*
 * An instrument's price history alone, as JSON: the chart on its page,
 * for a client that draws its own. ?venue= (the page's default without
 * it), ?range= (24h, 7d, 14d, 90d, all) and ?compare= (region, or a
 * venue's key). API only: the page that draws it is the instrument's.
 */
static HtmxResponse *
venture_marketdata_web_history(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autofree gchar *key = NULL;
	VentureMarketdataInstrumentQuery query;
	HtmxResponse *gate;

	gate = mdw_gate(self, request, TRUE, VENTURE_USER_ROLE_VIEWER, VENTURE_TYPE_DATA_SOURCE);

	if (NULL != gate)
		return gate;

	if (!mdw_instrument_query(self, request, params, &query, &key, &error))
		return venture_web_error_response(error);

	answer = venture_marketdata_history(self->context, &query, &error);

	if (NULL == answer)
		return venture_web_error_response(error);

	return venture_web_json_response(answer, 200);
}

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
		{ HTMX_METHOD_GET, "/market/find", venture_marketdata_web_find },
		{ HTMX_METHOD_GET, "/market/venues", venture_marketdata_web_venues },
		{ HTMX_METHOD_GET, "/market/watchlists", venture_marketdata_web_watchlists },
		{ HTMX_METHOD_GET, "/market/watchlists/:id", venture_marketdata_web_watchlist },
		{ HTMX_METHOD_POST, "/market/watchlists/add", venture_marketdata_web_watchlist_add },
		{ HTMX_METHOD_POST, "/market/watchlists/remove", venture_marketdata_web_watchlist_remove },
		{ HTMX_METHOD_GET, "/market/alerts", venture_marketdata_web_alerts },
		{ HTMX_METHOD_POST, "/market/alerts/:id/evaluate", venture_marketdata_web_evaluate },
		/* The key is the rest of the path: it may hold a slash. */
		{ HTMX_METHOD_GET, "/market/i/:source/*", venture_marketdata_web_instrument },
		{ HTMX_METHOD_POST, "/market/i/:source/*", venture_marketdata_web_instrument_post },
		/* The twins: the same handlers, answering JSON for /api/. */
		{ HTMX_METHOD_GET, "/api/v1/market/browse", venture_marketdata_web_browse },
		{ HTMX_METHOD_GET, "/api/v1/market/deals", venture_marketdata_web_deals },
		{ HTMX_METHOD_GET, "/api/v1/market/find", venture_marketdata_web_find },
		{ HTMX_METHOD_GET, "/api/v1/market/venues", venture_marketdata_web_venues },
		{ HTMX_METHOD_GET, "/api/v1/market/watchlists", venture_marketdata_web_watchlists },
		{ HTMX_METHOD_GET, "/api/v1/market/watchlists/:id", venture_marketdata_web_watchlist },
		{ HTMX_METHOD_POST, "/api/v1/market/watchlists/add", venture_marketdata_web_watchlist_add },
		{ HTMX_METHOD_POST, "/api/v1/market/watchlists/remove", venture_marketdata_web_watchlist_remove },
		{ HTMX_METHOD_GET, "/api/v1/market/alerts", venture_marketdata_web_alerts },
		{ HTMX_METHOD_POST, "/api/v1/market/alerts/:id/evaluate", venture_marketdata_web_evaluate },
		{ HTMX_METHOD_GET, "/api/v1/market/i/:source/*", venture_marketdata_web_instrument },
		/* The price history alone: no page of its own, the instrument's
		 * page draws it. */
		{ HTMX_METHOD_GET, "/api/v1/market/history/:source/*", venture_marketdata_web_history },
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(routes); i++)
		venture_web_server_add_classified_route(self, routes[i].method, routes[i].path,
		                                        VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE,
		                                        routes[i].handler, self);
}
