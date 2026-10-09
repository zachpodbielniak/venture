/* SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * venture-marketdata-accounts-pages.inc - /accounts, an account's page, the
 * inventory and the profit and loss, with their /api/v1/accounts twins
 *
 * Included by venture-web-server.c after venture-marketdata-pages.inc,
 * whose mdw_* helpers (the request readers, the gate, the value drawers)
 * these pages share. Every page here is a renderer: it turns the request
 * into the question struct, asks the one implementation in
 * venture-marketdata-accounts.c, and draws the JSON it answers; the
 * /api/v1/accounts twin is the same handler returning that JSON
 * untouched. Nothing here reads a store or computes a figure.
 *
 * The gate is the Trading pages': a session or token, the marketdata
 * module, and read access to data_source records -- a page shows nothing
 * the source's own routes would not. A page reads the sidebar's
 * organization, an API call its organization_id
 * (venture_web_request_organization()).
 */

/* --- Reading the request --------------------------------------------------- */

/*
 * The account key a path names: everything after "/accounts/<source>/",
 * as the instrument pages read theirs -- the server has already undone
 * the escapes, so a key holding a slash arrives as two segments, and
 * undoing them again would misread a key holding "%".
 */
static gchar *
acw_path_key(
	HtmxRequest	 *request,
	GError		**error
){
	const gchar *path;
	const gchar *at;
	const gchar *key;

	path = htmx_request_get_path(request);
	at = (NULL != path) ? strstr(path, "/accounts/") : NULL;
	key = (NULL != at) ? strchr(at + strlen("/accounts/"), '/') : NULL;

	if ((NULL == key) || ('\0' == key[1]) || !g_utf8_validate(key + 1, -1, NULL))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		                    "That is not an account key");
		return NULL;
	}

	return g_strdup(key + 1);
}

/* A whole-number parameter in [1, @max]; absent or blank is 0 (the
 * answer's default). */
static gboolean
acw_threshold(
	HtmxRequest	 *request,
	const gchar	 *name,
	gint64		  max,
	gint64		 *out,
	GError		**error
){
	return mdw_number(request, name, 0, max, out, error);
}

/* The query string with one thing changed: the sort, or the page. */
static gchar *
acw_link(
	HtmxRequest		*request,
	const gchar		*path,
	const gchar *const	*kept,
	const gchar		*sort,
	gboolean		 descending,
	gint64			 page
){
	g_autoptr(GString) link = NULL;
	guint i;

	link = g_string_new(path);
	g_string_append_c(link, '?');

	for (i = 0; NULL != kept[i]; i++)
	{
		const gchar *value = mdw_param(request, kept[i]);

		if (NULL == value)
			continue;

		g_string_append_printf(link, "%s=", kept[i]);
		g_string_append_uri_escaped(link, value, NULL, FALSE);
		g_string_append_c(link, '&');
	}

	g_string_append_printf(link, "sort=%s&dir=%s", sort, descending ? "desc" : "asc");

	if (page > 0)
		g_string_append_printf(link, "&page=%" G_GINT64_FORMAT, page);

	return g_string_free(g_steal_pointer(&link), FALSE);
}

static void
acw_sort_header(
	GString			*html,
	HtmxRequest		*request,
	const gchar		*path,
	const gchar *const	*kept,
	const gchar		*current,
	gboolean		 descending,
	const gchar		*sort,
	const gchar		*label,
	gboolean		 numeric,
	gboolean		 paged
){
	g_autofree gchar *link = NULL;
	gboolean active;

	active = (0 == g_strcmp0(current, sort));

	/* A first click on a figure shows the largest first; a second turns
	 * it round. */
	link = acw_link(request, path, kept, sort, active ? !descending : numeric, paged ? 1 : 0);
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

/* --- Drawing values ---------------------------------------------------------- */

static gchar *acw_money_display(const VentureMoney *money);

/* An array of money objects, one per currency, as "12g 3s · 4.00 USD". */
static void
acw_money_list(
	GString		*html,
	JsonObject	*object,
	const gchar	*member
){
	JsonArray *amounts;
	guint i;

	amounts = ((NULL != object) && json_object_has_member(object, member))
		? json_object_get_array_member(object, member) : NULL;

	if ((NULL == amounts) || (0 == json_array_get_length(amounts)))
	{
		mdw_dash(html);
		return;
	}

	for (i = 0; i < json_array_get_length(amounts); i++)
	{
		g_autoptr(VentureMoney) money = NULL;
		g_autofree gchar *text = NULL;

		money = venture_money_from_json(json_array_get_element(amounts, i), NULL, NULL);

		if (NULL == money)
			continue;

		if (i > 0)
			g_string_append(html, "<span class=\"muted\"> \xc2\xb7 </span>");

		text = acw_money_display(money);
		g_string_append(html, "<span class=\"nowrap\">");
		venture_html_escape_append(html, text);
		g_string_append(html, "</span>");
	}
}

/*
 * Appends already-escaped money text with its leading figure grouped in
 * thousands when its currency's display does not group it (a currency of
 * coins reads "1214276g 97s"): seven digits of gold are a number to
 * count, not to read. Anything already grouped, or markup, goes as is.
 */
static void
acw_grouped(
	GString		*html,
	const gchar	*text
){
	const gchar *digits;
	gsize run;
	gsize i;

	if ((NULL != strchr(text, ',')) || ('<' == text[0]))
	{
		g_string_append(html, text);
		return;
	}

	digits = text + (('-' == text[0]) ? 1 : 0);
	run = strspn(digits, "0123456789");

	if (run < 5)
	{
		g_string_append(html, text);
		return;
	}

	g_string_append_len(html, text, digits - text);

	for (i = 0; i < run; i++)
	{
		if ((i > 0) && (0 == (run - i) % 3))
			g_string_append_c(html, ',');

		g_string_append_c(html, digits[i]);
	}

	g_string_append(html, digits + run);
}

/*
 * Money as these pages show it: whole units once a figure passes a
 * thousand of them -- seven digits of gold and then silver and copper are
 * noise, not precision a person reads -- with the leading figure grouped.
 * Display only: every answer keeps the exact minor units.
 */
static gchar *
acw_money_display(const VentureMoney *money)
{
	g_autoptr(VentureMoney) rounded = NULL;
	g_autoptr(GString) grouped = NULL;
	g_autofree gchar *text = NULL;
	gint64 minor;
	gint64 unit = 1;
	guint exponent;
	guint i;

	minor = venture_money_get_amount(money);
	exponent = venture_money_get_exponent(money);

	for (i = 0; (i < exponent) && (unit < G_GINT64_CONSTANT(1000000000000)); i++)
		unit *= 10;

	if ((unit > 1) && ((minor >= 1000 * unit) || (minor <= -1000 * unit)))
	{
		gint64 rest = minor % unit;

		minor = (minor / unit) * unit + ((rest * 2 >= unit) ? unit : ((rest * 2 <= -unit) ? -unit : 0));
		rounded = venture_money_new_for_currency(minor, venture_money_get_currency(money));
	}

	text = venture_money_to_display_string((NULL != rounded) ? rounded : money, TRUE);
	grouped = g_string_new(NULL);
	acw_grouped(grouped, text);

	return g_string_free(g_steal_pointer(&grouped), FALSE);
}

/* A money member of an answer, as acw_money_display() reads it. */
static void
acw_money(
	GString		*html,
	JsonObject	*object,
	const gchar	*member
){
	g_autoptr(VentureMoney) money = NULL;
	g_autofree gchar *text = NULL;
	JsonNode *node;

	node = (NULL != object) ? json_object_get_member(object, member) : NULL;
	money = ((NULL != node) && JSON_NODE_HOLDS_OBJECT(node)) ? venture_money_from_json(node, NULL, NULL) : NULL;

	if (NULL == money)
	{
		mdw_dash(html);
		return;
	}

	text = acw_money_display(money);
	venture_html_escape_append(html, text);
}

/* A chart's money label, the same way. */
static gchar *
acw_format_money(
	gdouble		 value,
	gpointer	 user_data
){
	g_autoptr(VentureMoney) money = NULL;
	const gchar *currency = user_data;

	if (!isfinite(value) || (fabs(value) > 9.0e15) || venture_string_is_empty(currency) ||
	    !venture_currency_is_valid(currency))
		return venture_web_chart_format_number(value, NULL);

	money = venture_money_new_for_currency((gint64)llround(value), currency);

	if (NULL == money)
		return venture_web_chart_format_number(value, NULL);

	return acw_money_display(money);
}

/*
 * A deadline: "in 3 hours" or "2 hours ago" in a <time>, coloured by how
 * close it is -- expired and soon are the two a person acts on.
 */
static void
acw_expiry(
	GString		*html,
	JsonObject	*object,
	const gchar	*member,
	const gchar	*urgency
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
	g_string_append_printf(html, "<time class=\"expiry expiry-%s\" datetime=\"",
	                       (NULL != urgency) ? urgency : "later");
	venture_html_escape_append(html, text);
	g_string_append(html, "\">");
	venture_html_escape_append(html, relative);
	g_string_append(html, "</time>");
}

/* How fresh a source's word on an account is: a badge once it is a day
 * old, since everything else on the row is as of then. */
static void
acw_freshness(
	GString		*html,
	JsonObject	*row
){
	gint64 age;

	age = json_object_get_int_member_with_default(row, "freshness_seconds", 0);
	mdw_when(html, row, "synced_at");

	if (age > 86400)
		g_string_append(html, " <span class=\"badge warning\">stale data</span>");
}

/* A trend of minor units as a sparkline, NAN where nothing was known. */
static void
acw_sparkline(
	GString		*html,
	JsonObject	*row,
	const gchar	*label
){
	g_autoptr(GArray) values = NULL;
	g_autofree gchar *drawn = NULL;
	JsonNode *node;
	JsonArray *points;
	guint i;

	node = json_object_get_member(row, "trend");

	if ((NULL == node) || !JSON_NODE_HOLDS_OBJECT(node))
		return;

	points = json_object_get_array_member(json_node_get_object(node), "values");
	values = g_array_new(FALSE, FALSE, sizeof(gdouble));

	for (i = 0; (NULL != points) && (i < json_array_get_length(points)); i++)
	{
		JsonNode *point = json_array_get_element(points, i);
		gdouble value = JSON_NODE_HOLDS_VALUE(point) ? (gdouble)json_node_get_int(point) : NAN;

		g_array_append_val(values, value);
	}

	drawn = venture_web_chart_sparkline(label, (const gdouble *)(gpointer)values->data, values->len);
	g_string_append(html, drawn);
}

/* The account's level and class, "80 Warrior", from its attributes. */
static void
acw_level_class(
	GString		*html,
	JsonObject	*row
){
	JsonNode *level;
	const gchar *klass;

	level = json_object_get_member(row, "level");
	klass = mdw_text(row, "class");

	if (((NULL == level) || !JSON_NODE_HOLDS_VALUE(level)) && (NULL == klass))
	{
		mdw_dash(html);
		return;
	}

	if ((NULL != level) && JSON_NODE_HOLDS_VALUE(level))
		g_string_append_printf(html, "%" G_GINT64_FORMAT " ", json_node_get_int(level));

	if (NULL == klass)
		return;

	/* In its class colour, on a chip of its own so the colour reads on
	 * either theme (a white or pale class vanishes on a light page). The
	 * colour was checked as #rrggbb when the answer was built. */
	if (NULL != mdw_text(row, "class_color"))
	{
		g_string_append_printf(html, "<span class=\"class-chip\" style=\"--class-color:%s\">",
		                       mdw_text(row, "class_color"));
		venture_html_escape_append(html, klass);
		g_string_append(html, "</span>");
	}
	else
		venture_html_escape_append(html, klass);
}

/* An account's name, in its class colour where the theme is dark enough
 * for it (see .class-name); plain otherwise. */
static void
acw_account_name(
	GString		*html,
	JsonObject	*account,
	const gchar	*member
){
	const gchar *color = mdw_text(account, "class_color");

	if (NULL != color)
		g_string_append_printf(html, "<span class=\"class-name\" style=\"--class-color:%s\">", color);

	venture_html_escape_append(html, mdw_text(account, member));

	if (NULL != color)
		g_string_append(html, "</span>");
}

static const gchar *
acw_severity_label(const gchar *severity)
{
	if (0 == g_strcmp0(severity, "overdue"))
		return "Overdue";

	if (0 == g_strcmp0(severity, "soon"))
		return "Soon";

	if (0 == g_strcmp0(severity, "waiting"))
		return "Waiting";

	return "Unvisited";
}

static const gchar *
acw_severity_tone(const gchar *severity)
{
	if (0 == g_strcmp0(severity, "overdue"))
		return "negative";

	if (0 == g_strcmp0(severity, "soon"))
		return "warning";

	if (0 == g_strcmp0(severity, "waiting"))
		return "info";

	return "badge-muted";
}

/*
 * The "needs attention" list: one row per place to log in to, the most
 * urgent first, each naming its accounts and every reason. Shared by the
 * overview and its dashboard card, so the two cannot disagree.
 */
static void
acw_attention_list(
	GString		*html,
	JsonArray	*rows,
	guint		 limit
){
	g_autofree gchar *last_group = NULL;
	gboolean by_login = FALSE;
	guint i;

	if ((NULL == rows) || (0 == json_array_get_length(rows)))
	{
		g_string_append(html, "<p class=\"accounts-clear\">Nothing needs a login. Every listing has "
		                      "time left, nothing waits in the mail and every account was seen "
		                      "recently.</p>");
		return;
	}

	/* Rows arrive grouped by login; with any login named, each group gets
	 * a heading, since which login to sign in with is the first thing to
	 * know. Rows with none are reachable from any login. */
	for (i = 0; i < json_array_get_length(rows); i++)
	{
		JsonNode *login = json_object_get_member(json_array_get_object_element(rows, i), "login");

		if ((NULL != login) && JSON_NODE_HOLDS_OBJECT(login))
			by_login = TRUE;
	}

	g_string_append(html, "<ol class=\"accounts-attention\">");

	for (i = 0; (i < json_array_get_length(rows)) && ((0 == limit) || (i < limit)); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);
		JsonArray *accounts = json_object_get_array_member(row, "accounts");
		JsonArray *reasons = json_object_get_array_member(row, "reasons");
		JsonNode *login = json_object_get_member(row, "login");
		const gchar *severity = mdw_text(row, "severity");
		g_autofree gchar *group = NULL;
		guint j;

		if (by_login)
		{
			const gchar *name = ((NULL != login) && JSON_NODE_HOLDS_OBJECT(login))
				? mdw_text(json_node_get_object(login), "name") : NULL;
			const gchar *key = ((NULL != login) && JSON_NODE_HOLDS_OBJECT(login))
				? mdw_text(json_node_get_object(login), "key") : NULL;

			/* By key: two logins may share a name. */
			group = g_strdup_printf("%" G_GINT64_FORMAT "\x1f%s",
			                        json_object_get_int_member(row, "data_source_id"),
			                        (NULL != key) ? key : "");

			if ((NULL == last_group) || (0 != g_strcmp0(last_group, group)))
			{
				g_string_append(html, "<li class=\"attention-login\" role=\"presentation\">");
				venture_html_escape_append(html, (NULL != name) ? name : "Any login");
				g_string_append(html, "</li>");
				g_free(last_group);
				last_group = g_strdup(group);
			}
		}

		g_string_append_printf(html, "<li class=\"attention-row is-%s\"><div class=\"attention-head\">"
		                             "<span class=\"badge %s\">%s</span> <span class=\"attention-title\">",
		                       (NULL != severity) ? severity : "stale", acw_severity_tone(severity),
		                       acw_severity_label(severity));
		venture_html_escape_append(html, mdw_text(row, "title"));
		g_string_append(html, "</span><span class=\"attention-accounts\">");

		for (j = 0; (NULL != accounts) && (j < json_array_get_length(accounts)); j++)
		{
			JsonObject *account = json_array_get_object_element(accounts, j);

			if (j > 0)
				g_string_append(html, ", ");

			g_string_append(html, "<a href=\"");
			venture_html_escape_append(html, mdw_text(account, "url"));
			g_string_append(html, "\">");
			acw_account_name(html, account, "name");
			g_string_append(html, "</a>");
		}

		g_string_append(html, "</span></div><ul class=\"attention-reasons\">");

		for (j = 0; (NULL != reasons) && (j < json_array_get_length(reasons)); j++)
		{
			JsonObject *reason = json_array_get_object_element(reasons, j);

			g_string_append_printf(html, "<li class=\"reason-%s\"><span class=\"attention-who\">",
			                       mdw_text(reason, "kind"));
			venture_html_escape_append(html, mdw_text(reason, "account_name"));
			g_string_append(html, "</span> ");
			venture_html_escape_append(html, mdw_text(reason, "text"));
			g_string_append(html, "</li>");
		}

		g_string_append(html, "</ul></li>");
	}

	g_string_append(html, "</ol>");

	if ((limit > 0) && (json_array_get_length(rows) > limit))
		g_string_append_printf(html, "<p class=\"muted\">%u more place%s to visit.</p>",
		                       json_array_get_length(rows) - limit,
		                       (1 == json_array_get_length(rows) - limit) ? "" : "s");
}

/* The page, inside the wrapper the accounts stylesheet rules hang from. */
static HtmxResponse *
acw_page(
	VentureWebServer	*self,
	HtmxRequest		*request,
	const gchar		*active,
	const gchar		*title,
	GString			*html
){
	g_autofree gchar *body = NULL;

	body = g_strdup_printf("<div class=\"accounts-page\">%s</div>", html->str);

	return venture_web_html_response(venture_web_page(self, request, active, title, body), 200);
}

/*
 * A period's label: its boundary is midnight UTC (every period in VENTURE
 * is), so it is the date it is in UTC -- read in a zone west of UTC, the
 * Monday a week starts on would be labelled Sunday.
 */
static gchar *
acw_period_label(
	const gchar	*iso,
	const gchar	*format
){
	g_autoptr(GDateTime) moment = NULL;

	moment = (NULL != iso) ? g_date_time_new_from_iso8601(iso, NULL) : NULL;

	return (NULL != moment) ? g_date_time_format(moment, format) : g_strdup("");
}

/* --- Ignoring accounts ----------------------------------------------------------- */

/* Where a switch sends the reader back to: a path on this site, or the
 * overview -- never somewhere a form field could point elsewhere. */
static const gchar *
acw_back(HtmxRequest *request)
{
	const gchar *back = htmx_request_get_form_value(request, "back");

	if ((NULL == back) || ('/' != back[0]) || ('/' == back[1]) || (NULL != strchr(back, '\\')) ||
	    (NULL != strpbrk(back, "\r\n")))
		return "/accounts";

	return back;
}

/* A button that posts one ignore switch. @id > 0 turns that record off;
 * otherwise it writes one for @kind and @key, shown as @name. */
static void
acw_ignore_button(
	GString		*html,
	gint64		 id,
	const gchar	*kind,
	const gchar	*key,
	const gchar	*name,
	const gchar	*back,
	const gchar	*label
){
	if (id > 0)
		g_string_append_printf(html, "<form method=\"post\" action=\"/accounts/ignores/%" G_GINT64_FORMAT
		                             "/delete\" class=\"form-inline\">", id);
	else
	{
		g_string_append(html, "<form method=\"post\" action=\"/accounts/ignores\" class=\"form-inline\">"
		                      "<input type=\"hidden\" name=\"kind\" value=\"");
		venture_html_escape_append(html, kind);
		g_string_append(html, "\"><input type=\"hidden\" name=\"key\" value=\"");
		venture_html_escape_append(html, key);
		g_string_append(html, "\"><input type=\"hidden\" name=\"name\" value=\"");
		venture_html_escape_append(html, name);
		g_string_append(html, "\">");
	}

	g_string_append(html, "<input type=\"hidden\" name=\"back\" value=\"");
	venture_html_escape_append(html, back);
	g_string_append(html, "\"><button class=\"btn btn-sm\" type=\"submit\">");
	venture_html_escape_append(html, label);
	g_string_append(html, "</button></form>");
}

/*
 * The overview's card of what is ignored: each record with its switch
 * off, and the link that shows or hides the ignored accounts. Drawn only
 * when something is ignored.
 */
static void
acw_ignores_card(
	GString		*html,
	HtmxRequest	*request,
	JsonObject	*root,
	gboolean	 show
){
	JsonArray *records = json_object_has_member(root, "ignores")
		? json_object_get_array_member(root, "ignores") : NULL;
	JsonObject *summary = json_object_get_object_member(root, "summary");
	g_autoptr(GString) back = g_string_new("/accounts");
	g_autofree gchar *toggle = NULL;
	gint64 hidden = json_object_get_int_member(summary, "ignored");
	guint i;

	if ((NULL == records) || (0 == json_array_get_length(records)))
		return;

	if (show)
		g_string_append(back, "?show_ignored=1");

	/* The page as it is, the switch flipped. */
	{
		static const gchar *const kept[] = {
			"source", "basis", "group", "expiring_hours", "mail_days", "stale_days", "by_realm",
			"login", "by_login", "sort", "dir", NULL
		};
		GString *link = g_string_new("/accounts?");

		for (i = 0; NULL != kept[i]; i++)
		{
			const gchar *value = mdw_param(request, kept[i]);

			if (NULL == value)
				continue;

			g_string_append_printf(link, "%s=", kept[i]);
			g_string_append_uri_escaped(link, value, NULL, FALSE);
			g_string_append_c(link, '&');
		}

		g_string_append(link, show ? "show_ignored=0" : "show_ignored=1");
		toggle = g_string_free(link, FALSE);
	}

	g_string_append(html, "<section class=\"card accounts-ignored-card\" aria-labelledby=\"ignored-h\">"
	                      "<div class=\"card-head\"><h2 id=\"ignored-h\">Ignored</h2><a class=\"btn btn-sm\" "
	                      "href=\"");
	venture_html_escape_append(html, toggle);
	g_string_append_printf(html, "\">%s</a></div><div class=\"card-body\"><p class=\"muted\">",
	                       show ? "Hide ignored accounts" : "Show ignored accounts");

	if (show)
		g_string_append_printf(html, "%" G_GINT64_FORMAT " ignored account%s %s shown below, marked, and "
		                             "counted in the figures above.", hidden, (1 == hidden) ? "" : "s",
		                       (1 == hidden) ? "is" : "are");
	else
		g_string_append_printf(html, "%" G_GINT64_FORMAT " account%s left out of this page, its totals "
		                             "and \"My characters\".", hidden, (1 == hidden) ? "" : "s");

	g_string_append(html, "</p><ul class=\"accounts-ignored\">");

	for (i = 0; i < json_array_get_length(records); i++)
	{
		JsonObject *record = json_array_get_object_element(records, i);
		gboolean realm = (0 == g_strcmp0(mdw_text(record, "kind"), "realm"));

		g_string_append(html, "<li><span class=\"badge\">");
		g_string_append(html, realm ? "realm" : "character");
		g_string_append(html, "</span> ");
		venture_html_escape_append(html, mdw_text(record, "name"));
		g_string_append(html, " ");
		acw_ignore_button(html, json_object_get_int_member(record, "id"), NULL, NULL, NULL, back->str,
		                  "Stop ignoring");
		g_string_append(html, "</li>");
	}

	g_string_append(html, "</ul></div></section>");
}

/*
 * A line saying how many ignored accounts a page leaves out, with the
 * page again -- @kept's parameters as they are -- the other way round.
 * Nothing when none is ignored.
 */
static void
acw_ignored_note(
	GString			*html,
	HtmxRequest		*request,
	JsonObject		*root,
	const gchar		*path,
	const gchar *const	*kept,
	gboolean		 show
){
	g_autoptr(GString) link = NULL;
	gint64 count = json_object_has_member(root, "ignored") ? json_object_get_int_member(root, "ignored") : 0;
	guint i;

	if (count <= 0)
		return;

	link = g_string_new(path);
	g_string_append_c(link, '?');

	for (i = 0; NULL != kept[i]; i++)
	{
		const gchar *value = mdw_param(request, kept[i]);

		if ((NULL == value) || (0 == g_strcmp0(kept[i], "show_ignored")))
			continue;

		g_string_append_printf(link, "%s=", kept[i]);
		g_string_append_uri_escaped(link, value, NULL, FALSE);
		g_string_append_c(link, '&');
	}

	g_string_append(link, show ? "show_ignored=0" : "show_ignored=1");
	g_string_append_printf(html, "<p class=\"muted accounts-ignored-note\">%" G_GINT64_FORMAT
	                             " ignored account%s %s. <a href=\"", count, (1 == count) ? "" : "s",
	                       show ? "included" : "left out");
	venture_html_escape_append(html, link->str);
	g_string_append_printf(html, "\">%s</a> \xc2\xb7 <a href=\"/accounts?show_ignored=1\">manage</a></p>",
	                       show ? "Leave them out" : "Include them");
}

/* --- /accounts ---------------------------------------------------------------- */

static gboolean
acw_overview_query(
	VentureWebServer		 *self,
	HtmxRequest			 *request,
	VentureMarketdataAccountsQuery	 *query,
	GError				**error
){
	venture_marketdata_accounts_query_init(query);

	if (!venture_web_request_organization(self, request, NULL, &query->organization_id, error) ||
	    !mdw_number(request, "source", 0, G_MAXINT64, &query->data_source_id, error) ||
	    !acw_threshold(request, "expiring_hours", VENTURE_MARKETDATA_ACCOUNTS_MAX_EXPIRING_HOURS,
	                   &query->expiring_hours, error) ||
	    !acw_threshold(request, "mail_days", VENTURE_MARKETDATA_ACCOUNTS_MAX_MAIL_DAYS,
	                   &query->mail_days, error) ||
	    !acw_threshold(request, "stale_days", VENTURE_MARKETDATA_ACCOUNTS_MAX_STALE_DAYS,
	                   &query->stale_days, error))
		return FALSE;

	query->basis = mdw_param(request, "basis");
	query->group_key = mdw_param(request, "group");
	query->login = mdw_param(request, "login");
	query->sort = mdw_param(request, "sort");
	query->descending = (0 == g_strcmp0(mdw_param(request, "dir"), "desc"));
	query->show_ignored = mdw_flag(request, "show_ignored");

	return TRUE;
}

/* A <select> of an answer's string list, the chosen one selected. */
static void
acw_select(
	GString		*html,
	const gchar	*label,
	const gchar	*name,
	JsonArray	*choices,
	const gchar	*chosen
){
	guint i;

	g_string_append(html, "<div class=\"field\"><label>");
	venture_html_escape_append(html, label);
	g_string_append_printf(html, "<select name=\"%s\">", name);

	for (i = 0; (NULL != choices) && (i < json_array_get_length(choices)); i++)
	{
		const gchar *choice = json_array_get_string_element(choices, i);
		g_autofree gchar *shown = g_strdup(choice);

		g_strdelimit(shown, "_", ' ');
		g_string_append(html, "<option value=\"");
		venture_html_escape_append(html, choice);
		g_string_append_printf(html, "\"%s>", (0 == g_strcmp0(choice, chosen)) ? " selected" : "");
		venture_html_escape_append(html, shown);
		g_string_append(html, "</option>");
	}

	g_string_append(html, "</select></label></div>");
}

/* The "Gold on hand" label: what a game calls its money, else "Money". */
static gchar *
acw_money_label(
	const gchar	*currency,
	const gchar	*what
){
	if (0 == g_strcmp0(currency, "GOLD"))
		return g_strdup_printf("Gold %s", what);

	return g_strdup_printf("Money %s", what);
}

static void
acw_stat_money_list(
	GString		*html,
	const gchar	*label,
	JsonObject	*object,
	const gchar	*member,
	const gchar	*note
){
	g_autoptr(GString) value = g_string_new(NULL);

	acw_money_list(value, object, member);
	g_string_append(html, "<div class=\"stat\"><span class=\"stat-label\">");
	venture_html_escape_append(html, label);
	g_string_append(html, "</span><span class=\"stat-value market-figure\">");
	g_string_append(html, value->str);
	g_string_append(html, "</span>");

	if (NULL != note)
	{
		g_string_append(html, "<span class=\"stat-note\">");
		venture_html_escape_append(html, note);
		g_string_append(html, "</span>");
	}

	g_string_append(html, "</div>");
}

/*
 * A <select> of the logins an answer offers ({key, name} objects), the
 * chosen one selected; nothing at all for a source that sends no logins,
 * so a one-login operator's form is as it was.
 */
static void
acw_login_picker(
	GString		*html,
	JsonArray	*choices,
	const gchar	*chosen
){
	guint i;

	if ((NULL == choices) || (0 == json_array_get_length(choices)))
		return;

	g_string_append(html, "<div class=\"field\"><label>Login<select name=\"login\">"
	                      "<option value=\"\">every login</option>");

	for (i = 0; i < json_array_get_length(choices); i++)
	{
		JsonObject *choice = json_array_get_object_element(choices, i);
		const gchar *key = mdw_text(choice, "key");

		g_string_append(html, "<option value=\"");
		venture_html_escape_append(html, key);
		g_string_append_printf(html, "\"%s>", (0 == g_strcmp0(key, chosen)) ? " selected" : "");
		venture_html_escape_append(html, mdw_text(choice, "name"));
		g_string_append(html, "</option>");
	}

	g_string_append(html, "</select></label></div>");
}

/*
 * The per-login cards: each login's characters, money, stock, listings
 * and thirty days, with how many of its places want a visit. Drawn only
 * when an account has a login; the figures are the answer's sums.
 */
static void
acw_login_cards(
	GString		*html,
	JsonArray	*logins,
	const gchar	*currency
){
	g_autofree gchar *gold_label = acw_money_label(currency, "on hand");
	guint i;

	if ((NULL == logins) || (0 == json_array_get_length(logins)))
		return;

	g_string_append(html, "<section class=\"accounts-logins\" aria-labelledby=\"logins-h\">"
	                      "<h2 class=\"section-title\" id=\"logins-h\">By login</h2>"
	                      "<div class=\"login-cards\">");

	for (i = 0; i < json_array_get_length(logins); i++)
	{
		JsonObject *login = json_array_get_object_element(logins, i);
		const gchar *url = mdw_text(login, "url");
		gint64 needs = json_object_get_int_member(login, "needs_login");
		gint64 expired = json_object_get_int_member(login, "positions_expired");

		g_string_append(html, "<article class=\"card login-card\"><div class=\"card-head\"><h3>");

		if (NULL != url)
		{
			g_string_append(html, "<a href=\"");
			venture_html_escape_append(html, url);
			g_string_append(html, "\">");
			venture_html_escape_append(html, mdw_text(login, "name"));
			g_string_append(html, "</a>");
		}
		else
			venture_html_escape_append(html, mdw_text(login, "name"));

		g_string_append(html, "</h3>");

		if (needs > 0)
			g_string_append_printf(html, "<span class=\"badge warning\">%" G_GINT64_FORMAT
			                             " to visit</span>", needs);

		/* Characters on realms, then the banks; a card with no
		 * character (the shared banks) says only what it holds. */
		{
			gint64 characters = json_object_get_int_member(login, "characters");
			gint64 banks = json_object_get_int_member(login, "accounts") - characters;
			gint64 realms = json_object_get_int_member(login, "realms");

			g_string_append(html, "</div><div class=\"card-body\"><p class=\"login-sub\">");

			if (characters > 0)
				g_string_append_printf(html, "%" G_GINT64_FORMAT " character%s on %" G_GINT64_FORMAT
				                             " realm%s", characters, (1 == characters) ? "" : "s",
				                       realms, (1 == realms) ? "" : "s");

			if (banks > 0)
				g_string_append_printf(html, "%s%" G_GINT64_FORMAT " bank%s",
				                       (characters > 0) ? ", " : "", banks, (1 == banks) ? "" : "s");
		}

		if (NULL != mdw_text(login, "group_key"))
		{
			g_string_append(html, " \xc2\xb7 under ");
			venture_html_escape_append(html, mdw_text(login, "group_key"));
		}

		g_string_append(html, "</p><dl class=\"login-figures\"><div><dt>");
		venture_html_escape_append(html, gold_label);
		g_string_append(html, "</dt><dd class=\"market-figure\">");
		acw_money_list(html, login, "balances");
		g_string_append(html, "</dd></div><div><dt>Inventory</dt><dd class=\"market-figure\">");
		acw_money_list(html, login, "inventory_value");
		g_string_append_printf(html, "</dd></div><div><dt>Listed</dt><dd>%" G_GINT64_FORMAT,
		                       json_object_get_int_member(login, "positions"));

		if (expired > 0)
			g_string_append_printf(html, " <span class=\"badge negative\">%" G_GINT64_FORMAT
			                             " expired</span>", expired);

		g_string_append(html, "</dd></div><div><dt>Net, 30 days</dt><dd class=\"market-figure\">");
		acw_money_list(html, login, "net_30d");
		g_string_append(html, "</dd></div></dl></div></article>");
	}

	g_string_append(html, "</div></section>");
}

/* A profession's ranks by expansion, folded: each a link to Crafting
 * narrowed to the character, the profession and that expansion; an entry
 * the source wrote in another shape is shown as it came. */
static void
acw_profession_tiers(
	GString		*html,
	JsonObject	*profession
){
	JsonArray *tiers;
	guint i;

	tiers = json_object_has_member(profession, "tiers") ? json_object_get_array_member(profession, "tiers") : NULL;

	if ((NULL == tiers) || (0 == json_array_get_length(tiers)))
		return;

	g_string_append(html, " <details class=\"account-tiers\"><summary>by expansion</summary>");

	for (i = 0; i < json_array_get_length(tiers); i++)
	{
		JsonObject *tier = json_array_get_object_element(tiers, i);
		const gchar *url = mdw_text(tier, "crafting_url");

		if (i > 0)
			g_string_append(html, " \xc2\xb7 ");

		if (NULL != url)
		{
			g_string_append(html, "<a href=\"");
			venture_html_escape_append(html, url);
			g_string_append(html, "\">");
		}

		venture_html_escape_append(html, mdw_text(tier, "text"));

		if (NULL != url)
			g_string_append(html, "</a>");
	}

	g_string_append(html, "</details>");
}

/* A character's professions, "Tailoring 65/100 · Cooking 1/100", each a
 * link to Crafting narrowed to it and the character, after a link to
 * Crafting narrowed to the character alone. Nothing for an account the
 * source names no profession for. */
static void
acw_professions(
	GString		*html,
	JsonObject	*row
){
	JsonArray *professions;
	guint i;

	professions = json_object_has_member(row, "professions") ? json_object_get_array_member(row, "professions")
	                                                         : NULL;

	if ((NULL == professions) || (0 == json_array_get_length(professions)))
		return;

	g_string_append(html, "<div class=\"account-sub account-professions\"><a href=\"");
	venture_html_escape_append(html, mdw_text(row, "crafting_url"));
	g_string_append(html, "\">Crafting</a>: ");

	for (i = 0; i < json_array_get_length(professions); i++)
	{
		JsonObject *profession = json_array_get_object_element(professions, i);

		if (i > 0)
			g_string_append(html, " \xc2\xb7 ");

		g_string_append(html, "<a href=\"");
		venture_html_escape_append(html, mdw_text(profession, "crafting_url"));
		g_string_append(html, "\">");
		venture_html_escape_append(html, mdw_text(profession, "name"));
		g_string_append_printf(html, " %" G_GINT64_FORMAT, json_object_get_int_member(profession, "skill"));

		if (JSON_NODE_HOLDS_VALUE(json_object_get_member(profession, "max")))
			g_string_append_printf(html, "/%" G_GINT64_FORMAT, json_object_get_int_member(profession, "max"));

		g_string_append(html, "</a>");
		acw_profession_tiers(html, profession);
	}

	g_string_append(html, "</div>");
}

static HtmxResponse *
venture_marketdata_web_accounts(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	static const gchar *const kept[] = {
		"source", "basis", "group", "expiring_hours", "mail_days", "stale_days", "by_realm",
		"login", "by_login", "show_ignored", NULL
	};
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GString) html = NULL;
	VentureMarketdataAccountsQuery query;
	JsonObject *root;
	JsonObject *summary;
	JsonObject *echo;
	JsonArray *rows;
	const gchar *currency;
	const gchar *sort;
	const gchar *last_realm = NULL;
	const gchar *last_login = NULL;
	JsonArray *logins;
	gboolean api;
	gboolean by_realm;
	gboolean by_login;
	gboolean has_logins;
	guint columns;
	gboolean descending;
	HtmxResponse *gate;
	guint i;

	(void)params;

	api = mdw_is_api(request);
	gate = mdw_gate(self, request, api, VENTURE_USER_ROLE_VIEWER, VENTURE_TYPE_DATA_SOURCE);

	if (NULL != gate)
		return gate;

	if (!acw_overview_query(self, request, &query, &error))
		return venture_web_error_response(error);

	by_realm = mdw_flag(request, "by_realm");
	by_login = mdw_flag(request, "by_login");

	/* Grouped by realm, the realm is the first key whatever else sorts;
	 * with no sort asked, the realms' accounts go by name. Grouped by
	 * login, the login comes before the realm. */
	query.realm_first = by_realm || by_login;
	query.login_first = by_login;

	if ((by_realm || by_login) && (NULL == query.sort))
		query.sort = "realm";

	answer = venture_marketdata_accounts(self->context, &query, &error);

	if (NULL == answer)
		return venture_web_error_response(error);

	if (api)
		return venture_web_json_response(answer, 200);

	root = json_node_get_object(answer);
	summary = json_object_get_object_member(root, "summary");
	echo = json_object_get_object_member(root, "query");
	rows = json_object_get_array_member(root, "accounts");
	logins = json_object_get_array_member(root, "logins");
	has_logins = (json_array_get_length(json_object_get_array_member(root, "login_choices")) > 0);
	by_login = by_login && has_logins;
	columns = has_logins ? 9 : 8;
	currency = mdw_text(summary, "currency");
	sort = mdw_text(echo, "sort");
	descending = json_object_get_boolean_member(echo, "descending");
	html = g_string_new(NULL);
	mdw_head(html, "Trading", "Accounts",
	         "Every character and bank the data sources report: what they hold, what they have "
	         "listed, what waits in the mail, and where to log in next.");

	/* The question, as a form that works without script. */
	g_string_append(html, "<details class=\"card accounts-controls\"><summary>Show and value"
	                      "</summary><form method=\"get\" action=\"/accounts\" class=\"form-grid "
	                      "market-filters\" aria-label=\"Account filters\">");
	mdw_source_picker(html, root, json_object_get_int_member(root, "data_source_id"), TRUE);
	mdw_input(html, request, "Realm", "group", "every realm");
	acw_login_picker(html, json_object_get_array_member(root, "login_choices"), mdw_text(echo, "login"));
	acw_select(html, "Value holdings at", "basis", json_object_get_array_member(root, "bases"),
	           mdw_text(echo, "basis"));
	mdw_input(html, request, "Listings expiring within (hours)", "expiring_hours", "12");
	mdw_input(html, request, "Mail expiring within (days)", "mail_days", "3");
	mdw_input(html, request, "Unvisited after (days)", "stale_days", "14");
	g_string_append_printf(html, "<div class=\"field\"><label><input type=\"checkbox\" name=\"by_realm\" "
	                             "value=\"1\"%s> Group the table by realm</label></div>",
	                       by_realm ? " checked" : "");

	if (has_logins)
		g_string_append_printf(html, "<div class=\"field\"><label><input type=\"checkbox\" "
		                             "name=\"by_login\" value=\"1\"%s> Group the table by login, "
		                             "then realm</label></div>",
		                       by_login ? " checked" : "");

	g_string_append_printf(html, "<div class=\"field\"><label><input type=\"checkbox\" "
	                             "name=\"show_ignored\" value=\"1\"%s> Show ignored accounts</label></div>",
	                       query.show_ignored ? " checked" : "");
	g_string_append(html, "<button class=\"btn\" type=\"submit\">Show</button></form></details>");
	mdw_notes(html, root);

	/* The headline figures. */
	{
		g_autofree gchar *gold_label = acw_money_label(currency, "on hand");
		g_autofree gchar *inventory_note = NULL;
		g_autofree gchar *listing_note = NULL;
		g_autofree gchar *mail_note = NULL;
		g_autofree gchar *account_note = NULL;
		g_autofree gchar *basis = g_strdup(mdw_text(echo, "basis"));

		g_strdelimit(basis, "_", ' ');
		inventory_note = g_strdup_printf("%" G_GINT64_FORMAT " units at %s value",
		                                 json_object_get_int_member(summary, "inventory_units"), basis);
		listing_note = g_strdup_printf("%" G_GINT64_FORMAT " listed, %" G_GINT64_FORMAT " expired, %"
		                               G_GINT64_FORMAT " expiring soon",
		                               json_object_get_int_member(summary, "positions"),
		                               json_object_get_int_member(summary, "positions_expired"),
		                               json_object_get_int_member(summary, "positions_expiring"));
		mail_note = g_strdup_printf("%" G_GINT64_FORMAT " mail, %" G_GINT64_FORMAT " with items",
		                            json_object_get_int_member(summary, "inbound"),
		                            json_object_get_int_member(summary, "inbound_items"));
		account_note = g_strdup_printf("%" G_GINT64_FORMAT " characters on %" G_GINT64_FORMAT
		                               " realms; %" G_GINT64_FORMAT " need a login",
		                               json_object_get_int_member(summary, "characters"),
		                               json_object_get_int_member(summary, "realms"),
		                               json_object_get_int_member(summary, "needs_login"));

		g_string_append(html, "<div class=\"stat-row market-stats accounts-stats\">");
		acw_stat_money_list(html, gold_label, summary, "balances", account_note);
		acw_stat_money_list(html, "Inventory value", summary, "inventory_value", inventory_note);
		acw_stat_money_list(html, "Listed, at buyout", summary, "positions_value", listing_note);
		acw_stat_money_list(html, "Waiting in the mail", summary, "inbound_money", mail_note);
		acw_stat_money_list(html, "Net, 30 days", summary, "net_30d", "sales and income, less purchases and expenses");
		g_string_append(html, "</div>");
	}

	acw_login_cards(html, logins, currency);

	/* What to do next, first. */
	g_string_append(html, "<section class=\"card accounts-attention-card\" aria-labelledby=\"attention-h\">"
	                      "<div class=\"card-head\"><h2 id=\"attention-h\">Needs attention</h2>"
	                      "<a class=\"btn btn-sm\" href=\"/market/alerts\">Alerts</a></div>"
	                      "<div class=\"card-body\">");
	acw_attention_list(html, json_object_get_array_member(root, "attention"), 0);
	g_string_append(html, "</div></section>");

	/* Every account. */
	g_string_append(html, "<section class=\"card\" aria-labelledby=\"accounts-h\"><div class=\"card-head\">"
	                      "<h2 id=\"accounts-h\">Every account</h2>"
	                      "<a class=\"btn btn-sm\" href=\"/accounts/inventory\">Inventory</a> "
	                      "<a class=\"btn btn-sm\" href=\"/accounts/pnl\">Profit and loss</a></div>"
	                      "<div class=\"table-wrap\"><table class=\"data market-table accounts-table\">"
	                      "<thead><tr>");
	acw_sort_header(html, request, "/accounts", kept, sort, descending, "name", "Character", FALSE, FALSE);

	if (has_logins)
		acw_sort_header(html, request, "/accounts", kept, sort, descending, "login", "Login", FALSE, FALSE);

	acw_sort_header(html, request, "/accounts", kept, sort, descending, "realm", "Realm", FALSE, FALSE);
	acw_sort_header(html, request, "/accounts", kept, sort, descending, "gold",
	                (0 == g_strcmp0(currency, "GOLD")) ? "Gold" : "Money", TRUE, FALSE);
	acw_sort_header(html, request, "/accounts", kept, sort, descending, "positions", "Listings", TRUE, FALSE);
	acw_sort_header(html, request, "/accounts", kept, sort, descending, "expiry", "Soonest expiry", FALSE,
	                FALSE);
	acw_sort_header(html, request, "/accounts", kept, sort, descending, "inbound", "Mail", TRUE, FALSE);
	acw_sort_header(html, request, "/accounts", kept, sort, descending, "last_seen", "Last seen", FALSE,
	                FALSE);
	acw_sort_header(html, request, "/accounts", kept, sort, descending, "freshness", "Data from", FALSE,
	                FALSE);
	g_string_append(html, "</tr></thead><tbody>");

	for (i = 0; (NULL != rows) && (i < json_array_get_length(rows)); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);
		const gchar *realm = mdw_text(row, "realm");
		const gchar *kind = mdw_text(row, "kind");
		g_autofree gchar *label = NULL;
		gint64 expired = json_object_get_int_member(row, "positions_expired");
		gint64 expiring = json_object_get_int_member(row, "positions_expiring");

		/* A login's heading, then each realm's inside it: a new login
		 * starts its realms over, even one the last login also had. */
		if (by_login)
		{
			const gchar *login = mdw_text(row, "login_name");

			if ((0 == i) || (0 != g_strcmp0(last_login, (NULL != login) ? login : "")))
			{
				g_string_append_printf(html, "<tr class=\"accounts-group-row accounts-login-row\">"
				                             "<th scope=\"rowgroup\" colspan=\"%u\">", columns);
				venture_html_escape_append(html, (NULL != login) ? login : "No login");
				g_string_append(html, "</th></tr>");
				last_login = (NULL != login) ? login : "";
				last_realm = NULL;
			}
		}

		if ((by_realm || by_login) && ((NULL == last_realm) || (0 != g_strcmp0(last_realm, realm))))
		{
			g_string_append_printf(html, "<tr class=\"accounts-group-row\"><th scope=\"rowgroup\" "
			                             "colspan=\"%u\">", columns);
			venture_html_escape_append(html, venture_string_is_empty(realm) ? "No realm" : realm);
			g_string_append(html, "</th></tr>");
			last_realm = realm;
		}

		g_string_append_printf(html, "<tr%s><td class=\"row-head\"><a href=\"",
		                       json_object_get_boolean_member(row, "needs_login")
		                       ? " class=\"needs-login\"" : "");
		venture_html_escape_append(html, mdw_text(row, "url"));
		g_string_append(html, "\">");
		acw_account_name(html, row, "display_name");
		g_string_append(html, "</a>");

		if ((NULL != kind) && (0 != g_strcmp0(kind, "character")))
		{
			g_string_append(html, " <span class=\"badge\">");
			venture_html_escape_append(html, kind);
			g_string_append(html, "</span>");
		}

		/* Only there with the ignored shown. */
		if (NULL != mdw_text(row, "ignored"))
			g_string_append_printf(html, " <span class=\"badge\" title=\"Ignored %s\">ignored</span>",
			                       (0 == g_strcmp0(mdw_text(row, "ignored"), "realm"))
			                       ? "with its realm" : "on its own");

		if (JSON_NODE_HOLDS_VALUE(json_object_get_member(row, "level")) || (NULL != mdw_text(row, "class")))
		{
			g_string_append(html, "<div class=\"account-sub\">");
			acw_level_class(html, row);
			g_string_append(html, "</div>");
		}

		acw_professions(html, row);
		g_string_append(html, "</td>");

		if (has_logins)
		{
			const gchar *login = mdw_text(row, "login_name");

			g_string_append(html, "<td class=\"nowrap\">");

			if (NULL != login)
				venture_html_escape_append(html, login);
			else
				g_string_append(html, "<span class=\"muted\">any</span>");

			g_string_append(html, "</td>");
		}

		g_string_append(html, "<td class=\"nowrap\">");
		venture_html_escape_append(html, venture_string_is_empty(realm) ? "\xe2\x80\x94" : realm);
		g_string_append(html, "</td><td class=\"num account-gold\"><span class=\"market-figure\">");
		acw_money(html, row, "gold");
		g_string_append(html, "</span>");
		label = g_strdup_printf("%s: closing balance, last 30 days", mdw_text(row, "display_name"));
		acw_sparkline(html, row, label);
		g_string_append_printf(html, "</td><td class=\"num\">%" G_GINT64_FORMAT,
		                       json_object_get_int_member(row, "positions"));

		if (expired > 0)
			g_string_append_printf(html, " <span class=\"badge negative\">%" G_GINT64_FORMAT " expired</span>",
			                       expired);
		else if (expiring > 0)
			g_string_append_printf(html, " <span class=\"badge warning\">%" G_GINT64_FORMAT " soon</span>",
			                       expiring);

		g_string_append(html, "</td><td>");
		acw_expiry(html, row, "soonest_expiry", (expired > 0) ? "expired" : ((expiring > 0) ? "soon" : "later"));
		g_string_append_printf(html, "</td><td class=\"num\">%" G_GINT64_FORMAT,
		                       json_object_get_int_member(row, "inbound"));

		if (json_array_get_length(json_object_get_array_member(row, "inbound_money")) > 0)
		{
			g_string_append(html, "<div class=\"muted\">");
			acw_money_list(html, row, "inbound_money");
			g_string_append(html, "</div>");
		}

		g_string_append(html, "</td><td>");
		mdw_when(html, row, "last_seen");

		if (json_object_get_boolean_member(row, "stale"))
			g_string_append(html, " <span class=\"badge\">unvisited</span>");

		g_string_append(html, "</td><td>");
		acw_freshness(html, row);
		g_string_append(html, "</td></tr>");
	}

	if ((NULL == rows) || (0 == json_array_get_length(rows)))
		g_string_append_printf(html, "<tr><td colspan=\"%u\"><div class=\"empty\"><h3>No accounts yet</h3><p>"
		                       "Accounts arrive with a data source that reports them: push them with "
		                       "<code>venturectl feeds push</code>, or sync a source whose plugin "
		                       "exports characters.</p></div></td></tr>", columns);

	g_string_append(html, "</tbody></table></div></section>");
	acw_ignores_card(html, request, root, query.show_ignored);
	mdw_attribution(html, root);

	return acw_page(self, request, "/accounts", "Accounts", html);
}

/* --- /accounts/:source/:key ------------------------------------------------------- */

/* The balance history as a line, in its currency, labelled by day. */
static gchar *
acw_balance_chart(
	VentureWebServer	*self,
	JsonObject		*root,
	const gchar		*name
){
	g_autoptr(GPtrArray) labels = NULL;
	g_autoptr(GArray) values = NULL;
	g_autofree gchar *title = NULL;
	VentureWebChartSeries series;
	VentureWebChart chart;
	JsonObject *history;
	JsonArray *points;
	const gchar *currency;
	guint i;

	history = json_object_get_object_member(root, "balance_history");
	points = json_object_get_array_member(history, "points");
	currency = mdw_text(history, "currency");
	labels = g_ptr_array_new_with_free_func(g_free);
	values = g_array_new(FALSE, FALSE, sizeof(gdouble));

	for (i = 0; (NULL != points) && (i < json_array_get_length(points)); i++)
	{
		JsonObject *point = json_array_get_object_element(points, i);
		JsonNode *amount = json_object_get_member(point, "amount");
		gdouble value = ((NULL != amount) && JSON_NODE_HOLDS_VALUE(amount))
			? (gdouble)json_node_get_int(amount) : NAN;

		g_ptr_array_add(labels, acw_period_label(mdw_text(point, "day"), "%b %d"));
		g_array_append_val(values, value);
	}

	g_ptr_array_add(labels, NULL);
	title = g_strdup_printf("%s: closing balance each day, 90 days", name);
	memset(&series, 0, sizeof(series));
	series.name = "Balance";
	series.values = (const gdouble *)(gpointer)values->data;
	series.format = acw_format_money;
	series.format_data = (gpointer)currency;
	memset(&chart, 0, sizeof(chart));
	chart.title = title;
	chart.summary = "The last balance the source reported on each day, carried forward over days "
	                "with no change.";
	chart.labels = (const gchar *const *)labels->pdata;
	chart.n_points = values->len;
	chart.series = &series;
	chart.n_series = 1;

	return venture_web_chart_line(&chart);
}

static HtmxResponse *
venture_marketdata_web_account(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GString) html = NULL;
	g_autofree gchar *key = NULL;
	VentureMarketdataAccountQuery query;
	JsonObject *root;
	JsonObject *account;
	JsonArray *array;
	JsonArray *reasons;
	const gchar *name;
	const gchar *realm;
	const gchar *currency;
	gboolean api;
	HtmxResponse *gate;
	gint64 value;
	guint i;

	api = mdw_is_api(request);
	gate = mdw_gate(self, request, api, VENTURE_USER_ROLE_VIEWER, VENTURE_TYPE_DATA_SOURCE);

	if (NULL != gate)
		return gate;

	memset(&query, 0, sizeof(query));

	if (!venture_web_request_organization(self, request, NULL, &query.organization_id, &error) ||
	    !mdw_path_id(params, "source", &query.data_source_id, &error) ||
	    !acw_threshold(request, "expiring_hours", VENTURE_MARKETDATA_ACCOUNTS_MAX_EXPIRING_HOURS,
	                   &query.expiring_hours, &error) ||
	    !mdw_number(request, "ledger", 0, VENTURE_MARKETDATA_ACCOUNTS_MAX_PAGE, &value, &error))
		return venture_web_error_response(error);

	key = acw_path_key(request, &error);

	if (NULL == key)
		return venture_web_error_response(error);

	query.key = key;
	query.basis = mdw_param(request, "basis");
	query.ledger_count = (guint)value;
	answer = venture_marketdata_account(self->context, &query, &error);

	if (NULL == answer)
		return venture_web_error_response(error);

	if (api)
		return venture_web_json_response(answer, 200);

	root = json_node_get_object(answer);
	html = g_string_new(NULL);

	if (!json_object_get_boolean_member(root, "available"))
	{
		mdw_head(html, "Accounts", key, NULL);
		mdw_notes(html, root);

		return acw_page(self, request, "/accounts", key, html);
	}

	account = json_object_get_object_member(root, "account");
	name = mdw_text(account, "display_name");
	realm = mdw_text(account, "realm");
	currency = mdw_text(root, "currency");

	g_string_append(html, "<div class=\"page-head\"><div class=\"page-title\"><nav class=\"crumbs\" "
	                      "aria-label=\"Breadcrumb\"><a href=\"/accounts\">Accounts</a> / ");
	venture_html_escape_append(html, mdw_text(root, "source_name"));
	g_string_append(html, "</nav><h1>");
	acw_account_name(html, account, "display_name");
	g_string_append(html, "</h1><p class=\"muted account-identity\">");

	if (JSON_NODE_HOLDS_VALUE(json_object_get_member(account, "level")) ||
	    (NULL != mdw_text(account, "class")))
	{
		acw_level_class(html, account);
		g_string_append(html, " \xc2\xb7 ");
	}

	if (!venture_string_is_empty(realm))
	{
		venture_html_escape_append(html, realm);
		g_string_append(html, " \xc2\xb7 ");
	}

	/* The login first among the facts that say how to reach it. */
	if (NULL != mdw_text(account, "login_name"))
	{
		g_autoptr(GString) link = g_string_new("/accounts?login=");

		g_string_append_uri_escaped(link, mdw_text(account, "login"), NULL, FALSE);
		g_string_append_printf(link, "&source=%" G_GINT64_FORMAT,
		                       json_object_get_int_member(root, "data_source_id"));
		g_string_append(html, "login <a class=\"account-login\" href=\"");
		venture_html_escape_append(html, link->str);
		g_string_append(html, "\">");
		venture_html_escape_append(html, mdw_text(account, "login_name"));
		g_string_append(html, "</a> \xc2\xb7 ");
	}

	venture_html_escape_append(html, mdw_text(account, "kind"));
	g_string_append(html, " \xc2\xb7 last seen ");
	mdw_when(html, account, "last_seen");
	g_string_append(html, " \xc2\xb7 data from ");
	acw_freshness(html, account);
	g_string_append(html, "</p></div>");

	/* The ignore switches: this character, and its whole realm. */
	if (json_object_has_member(root, "ignore"))
	{
		JsonObject *ignore = json_object_get_object_member(root, "ignore");
		g_autofree gchar *escaped = g_uri_escape_string(key, NULL, FALSE);
		g_autofree gchar *back = g_strdup_printf("/accounts/%" G_GINT64_FORMAT "/%s",
		                                         json_object_get_int_member(root, "data_source_id"), escaped);
		const gchar *realm_key = mdw_text(ignore, "realm_key");
		const gchar *realm_name = mdw_text(ignore, "realm_name");
		gint64 own = json_object_get_int_member(ignore, "account_id");
		gint64 realm_id = json_object_get_int_member(ignore, "realm_id");

		g_string_append(html, "<div class=\"page-actions account-ignore\">");

		if ((0 != own) || (0 != realm_id))
			g_string_append(html, "<span class=\"badge warning\">Ignored</span>");

		acw_ignore_button(html, own, "character", key, name, back,
		                  (0 != own) ? "Stop ignoring this character" : "Ignore this character");

		if (NULL != realm_key)
		{
			g_autofree gchar *label = g_strdup_printf((0 != realm_id) ? "Stop ignoring %s" : "Ignore %s",
			                                          (NULL != realm_name) ? realm_name : realm_key);

			acw_ignore_button(html, realm_id, "realm", realm_key,
			                  (NULL != realm_name) ? realm_name : realm_key, back, label);
		}

		g_string_append(html, "</div>");
	}

	g_string_append(html, "</div>");
	mdw_attribution(html, root);
	mdw_notes(html, root);

	/* Why to log in, said once at the top. */
	reasons = json_object_get_array_member(account, "reasons");

	if ((NULL != reasons) && (json_array_get_length(reasons) > 0))
	{
		g_string_append(html, "<div class=\"card accounts-attention-card\"><div class=\"card-head\">"
		                      "<h2>Log in for</h2></div><div class=\"card-body\"><ul class=\"attention-reasons\">");

		for (i = 0; i < json_array_get_length(reasons); i++)
		{
			g_string_append(html, "<li>");
			venture_html_escape_append(html, json_array_get_string_element(reasons, i));
			g_string_append(html, "</li>");
		}

		g_string_append(html, "</ul></div></div>");
	}

	/* The figures. */
	{
		g_autofree gchar *gold_label = acw_money_label(currency, "on hand");
		g_autofree gchar *listed = NULL;
		g_autofree gchar *mail = NULL;

		listed = g_strdup_printf("%" G_GINT64_FORMAT " listings, %" G_GINT64_FORMAT " expired",
		                         json_object_get_int_member(account, "positions"),
		                         json_object_get_int_member(account, "positions_expired"));
		mail = g_strdup_printf("%" G_GINT64_FORMAT " mail, %" G_GINT64_FORMAT " with items",
		                       json_object_get_int_member(account, "inbound"),
		                       json_object_get_int_member(account, "inbound_items"));
		g_string_append(html, "<div class=\"stat-row market-stats accounts-stats\">");
		acw_stat_money_list(html, gold_label, account, "balances", NULL);
		mdw_stat_of(html, "Holdings value", acw_money, root, "holdings_value");
		acw_stat_money_list(html, "Listed, at buyout", root, "positions_value", listed);
		acw_stat_money_list(html, "Waiting in the mail", account, "inbound_money", mail);
		acw_stat_money_list(html, "Net, 30 days", json_object_get_object_member(root, "ledger_30d"), "net",
		                    NULL);
		g_string_append(html, "</div>");
	}

	/* Money over time. */
	if (json_object_has_member(root, "balance_history") &&
	    JSON_NODE_HOLDS_OBJECT(json_object_get_member(root, "balance_history")))
	{
		g_autofree gchar *chart = acw_balance_chart(self, root, name);

		g_string_append(html, "<section class=\"card\"><div class=\"card-head\"><h2>");
		venture_html_escape_append(html, (0 == g_strcmp0(currency, "GOLD")) ? "Gold" : "Balance");
		g_string_append(html, "</h2></div><div class=\"card-body\">");
		g_string_append(html, chart);
		g_string_append(html, "</div></section>");
	}

	/* Listings: what is up, against the market where it is. */
	array = json_object_get_array_member(root, "positions");
	g_string_append(html, "<section class=\"card\" aria-labelledby=\"positions-h\"><div class=\"card-head\">"
	                      "<h2 id=\"positions-h\">Listings</h2>");

	if (!json_object_get_null_member(root, "listings_url"))
	{
		g_string_append(html, "<a class=\"btn btn-sm\" href=\"");
		venture_html_escape_append(html, mdw_text(root, "listings_url"));
		g_string_append(html, "\">As listing records</a>");
	}

	g_string_append(html, "</div><div class=\"table-wrap\"><table class=\"data market-table\"><thead><tr>"
	                      "<th scope=\"col\">Item</th><th scope=\"col\" class=\"num\">Units</th>"
	                      "<th scope=\"col\" class=\"num\">Each</th><th scope=\"col\" class=\"num\">Total</th>"
	                      "<th scope=\"col\" class=\"num\">Of market</th><th scope=\"col\">Market</th>"
	                      "<th scope=\"col\">Time left</th></tr></thead><tbody>");

	for (i = 0; (NULL != array) && (i < json_array_get_length(array)); i++)
	{
		JsonObject *row = json_array_get_object_element(array, i);

		g_string_append(html, "<tr><td class=\"row-head\"><a href=\"");
		venture_html_escape_append(html, mdw_text(row, "url"));
		g_string_append(html, "\">");
		mdw_name(html, row, "instrument_name", "instrument_key");
		g_string_append(html, "</a> <span class=\"muted\">");
		mdw_name(html, row, "venue_name", "venue_key");
		g_string_append(html, "</span></td><td class=\"num\">");
		mdw_int(html, row, "quantity");
		g_string_append(html, "</td><td class=\"num\">");
		acw_money(html, row, "unit_price");
		g_string_append(html, "</td><td class=\"num\">");
		acw_money(html, row, "total");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_percent(html, row, "vs_market_pct", 1.0);
		g_string_append(html, "</td><td>");

		if (json_object_get_boolean_member(row, "undercut"))
		{
			g_string_append(html, "<span class=\"badge negative\">undercut</span> <span class=\"muted\">");
			acw_money(html, row, "venue_min");
			g_string_append(html, " lowest</span>");
		}
		else if (JSON_NODE_HOLDS_OBJECT(json_object_get_member(row, "venue_min")))
			g_string_append(html, "<span class=\"badge positive\">lowest</span>");
		else
			mdw_dash(html);

		g_string_append(html, "</td><td>");
		acw_expiry(html, row, "expires_at", mdw_text(row, "urgency"));
		g_string_append(html, "</td></tr>");
	}

	if ((NULL == array) || (0 == json_array_get_length(array)))
		g_string_append(html, "<tr><td colspan=\"7\" class=\"muted\">Nothing listed.</td></tr>");

	g_string_append(html, "</tbody></table></div></section>");

	/* Mail. */
	array = json_object_get_array_member(root, "inbound");
	g_string_append(html, "<section class=\"card\" aria-labelledby=\"inbound-h\"><div class=\"card-head\">"
	                      "<h2 id=\"inbound-h\">Mail</h2></div><div class=\"table-wrap\">"
	                      "<table class=\"data market-table\"><thead><tr><th scope=\"col\">From</th>"
	                      "<th scope=\"col\">Subject</th><th scope=\"col\">Attached</th>"
	                      "<th scope=\"col\" class=\"num\">Money</th><th scope=\"col\">Expires</th>"
	                      "</tr></thead><tbody>");

	for (i = 0; (NULL != array) && (i < json_array_get_length(array)); i++)
	{
		JsonObject *row = json_array_get_object_element(array, i);

		g_string_append(html, "<tr><td>");
		venture_html_escape_append(html, (NULL != mdw_text(row, "sender")) ? mdw_text(row, "sender") : "");

		if (json_object_get_boolean_member(row, "returned"))
			g_string_append(html, " <span class=\"badge\">returned</span>");

		g_string_append(html, "</td><td>");
		venture_html_escape_append(html, (NULL != mdw_text(row, "subject")) ? mdw_text(row, "subject") : "");
		g_string_append(html, "</td><td>");

		if (NULL != mdw_text(row, "instrument_key"))
		{
			mdw_int(html, row, "quantity");
			g_string_append(html, " \xc3\x97 <a href=\"");
			venture_html_escape_append(html, mdw_text(row, "url"));
			g_string_append(html, "\">");
			mdw_name(html, row, "instrument_name", "instrument_key");
			g_string_append(html, "</a>");
		}
		else
			mdw_dash(html);

		g_string_append(html, "</td><td class=\"num\">");
		acw_money(html, row, "money");

		if (JSON_NODE_HOLDS_OBJECT(json_object_get_member(row, "cod")))
		{
			g_string_append(html, "<div class=\"muted\">C.O.D. ");
			acw_money(html, row, "cod");
			g_string_append(html, "</div>");
		}

		g_string_append(html, "</td><td>");
		acw_expiry(html, row, "expires_at", mdw_text(row, "urgency"));
		g_string_append(html, "</td></tr>");
	}

	if ((NULL == array) || (0 == json_array_get_length(array)))
		g_string_append(html, "<tr><td colspan=\"5\" class=\"muted\">Nothing in the mail.</td></tr>");

	g_string_append(html, "</tbody></table></div></section>");

	/* Holdings, place by place. */
	{
		JsonArray *places = json_object_get_array_member(root, "places");
		JsonArray *holdings = json_object_get_array_member(root, "holdings");
		g_autofree gchar *basis = g_strdup(mdw_text(root, "basis"));

		g_strdelimit(basis, "_", ' ');
		g_string_append(html, "<section class=\"card\" aria-labelledby=\"holdings-h\"><div class=\"card-head\">"
		                      "<h2 id=\"holdings-h\">Holdings</h2><span class=\"muted\">valued at ");
		venture_html_escape_append(html, basis);
		g_string_append(html, "</span></div><div class=\"card-body\">");

		if ((NULL == places) || (0 == json_array_get_length(places)))
			g_string_append(html, "<p class=\"muted\">The source has reported nothing held.</p>");
		else
		{
			guint p;

			g_string_append(html, "<div class=\"accounts-places\">");

			for (p = 0; p < json_array_get_length(places); p++)
			{
				JsonObject *place = json_array_get_object_element(places, p);

				g_string_append(html, "<a class=\"accounts-place\" href=\"#place-");
				venture_html_escape_append(html, mdw_text(place, "place"));
				g_string_append(html, "\"><span class=\"stat-label\">");
				venture_html_escape_append(html, mdw_text(place, "label"));
				g_string_append(html, "</span><span class=\"market-figure\">");
				acw_money(html, place, "value");
				g_string_append_printf(html, "</span><span class=\"muted\">%" G_GINT64_FORMAT " units</span></a>",
				                       json_object_get_int_member(place, "units"));
			}

			g_string_append(html, "</div>");

			for (p = 0; p < json_array_get_length(places); p++)
			{
				JsonObject *place = json_array_get_object_element(places, p);
				const gchar *code = mdw_text(place, "place");

				g_string_append(html, "<details class=\"accounts-holdings\" id=\"place-");
				venture_html_escape_append(html, code);
				g_string_append_printf(html, "\"%s><summary>", (p < 3) ? " open" : "");
				venture_html_escape_append(html, mdw_text(place, "label"));
				g_string_append_printf(html, " <span class=\"muted\">%" G_GINT64_FORMAT " lines</span></summary>"
				                             "<table class=\"data market-table\"><thead><tr>"
				                             "<th scope=\"col\">Item</th><th scope=\"col\" class=\"num\">Units</th>"
				                             "<th scope=\"col\" class=\"num\">Each</th>"
				                             "<th scope=\"col\" class=\"num\">Value</th></tr></thead><tbody>",
				                       json_object_get_int_member(place, "lines"));

				for (i = 0; (NULL != holdings) && (i < json_array_get_length(holdings)); i++)
				{
					JsonObject *line = json_array_get_object_element(holdings, i);

					if (0 != g_strcmp0(mdw_text(line, "place"), code))
						continue;

					g_string_append(html, "<tr><td><a href=\"");
					venture_html_escape_append(html, mdw_text(line, "url"));
					g_string_append(html, "\">");
					mdw_name(html, line, "instrument_name", "instrument_key");
					g_string_append(html, "</a></td><td class=\"num\">");
					mdw_int(html, line, "quantity");
					g_string_append(html, "</td><td class=\"num\">");
					acw_money(html, line, "unit_value");
					g_string_append(html, "</td><td class=\"num\">");
					acw_money(html, line, "value");
					g_string_append(html, "</td></tr>");
				}

				g_string_append(html, "</tbody></table></details>");
			}
		}

		g_string_append(html, "</div></section>");
	}

	/* The ledger. */
	array = json_object_get_array_member(root, "ledger");
	g_string_append(html, "<section class=\"card\" aria-labelledby=\"ledger-h\"><div class=\"card-head\">"
	                      "<h2 id=\"ledger-h\">Recent trades</h2><a class=\"btn btn-sm\" href=\"/accounts/pnl?account=");
	{
		g_autofree gchar *escaped = g_uri_escape_string(key, NULL, FALSE);

		venture_html_escape_append(html, escaped);
	}
	g_string_append_printf(html, "&amp;source=%" G_GINT64_FORMAT "\">Profit and loss</a></div>"
	                             "<div class=\"table-wrap\"><table class=\"data market-table\"><thead><tr>"
	                             "<th scope=\"col\">When</th><th scope=\"col\">What</th><th scope=\"col\">Item</th>"
	                             "<th scope=\"col\" class=\"num\">Units</th><th scope=\"col\" class=\"num\">Amount</th>"
	                             "<th scope=\"col\">With</th></tr></thead><tbody>",
	                       query.data_source_id);

	for (i = 0; (NULL != array) && (i < json_array_get_length(array)); i++)
	{
		JsonObject *row = json_array_get_object_element(array, i);
		const gchar *kind = mdw_text(row, "kind");

		g_string_append(html, "<tr><td>");
		mdw_when(html, row, "at");
		g_string_append_printf(html, "</td><td><span class=\"badge %s\">",
		                       ((0 == g_strcmp0(kind, "sale")) || (0 == g_strcmp0(kind, "income"))) ? "positive"
		                       : (((0 == g_strcmp0(kind, "buy")) || (0 == g_strcmp0(kind, "expense"))) ? "info"
		                                                                                               : ""));
		venture_html_escape_append(html, kind);
		g_string_append(html, "</span></td><td>");

		if (NULL != mdw_text(row, "instrument_key"))
			mdw_name(html, row, "instrument_name", "instrument_key");
		else
			mdw_dash(html);

		g_string_append(html, "</td><td class=\"num\">");
		mdw_int(html, row, "quantity");
		g_string_append(html, "</td><td class=\"num\">");
		acw_money(html, row, "amount");
		g_string_append(html, "</td><td>");
		venture_html_escape_append(html, (NULL != mdw_text(row, "counterparty")) ? mdw_text(row, "counterparty")
		                                                                          : "");
		g_string_append(html, "</td></tr>");
	}

	if ((NULL == array) || (0 == json_array_get_length(array)))
		g_string_append(html, "<tr><td colspan=\"6\" class=\"muted\">No trades recorded.</td></tr>");

	g_string_append(html, "</tbody></table></div></section>");

	/* Where it lives in the books, and what the source says about it. */
	{
		JsonObject *attrs = json_object_get_object_member(account, "attrs");
		g_autoptr(GList) members = (NULL != attrs) ? json_object_get_members(attrs) : NULL;
		GList *l;

		g_string_append(html, "<section class=\"card\" aria-labelledby=\"identity-h\"><div class=\"card-head\">"
		                      "<h2 id=\"identity-h\">About this account</h2>");

		if (JSON_NODE_HOLDS_OBJECT(json_object_get_member(root, "location")))
		{
			JsonObject *location = json_object_get_object_member(root, "location");

			g_string_append(html, "<a class=\"btn btn-sm\" href=\"");
			venture_html_escape_append(html, mdw_text(location, "url"));
			g_string_append(html, "\">Location record</a>");
		}

		g_string_append(html, "</div><div class=\"card-body\"><dl class=\"accounts-attrs\"><dt>Key</dt><dd><code>");
		venture_html_escape_append(html, key);
		g_string_append(html, "</code></dd><dt>Kind</dt><dd>");
		venture_html_escape_append(html, mdw_text(account, "kind"));
		g_string_append(html, "</dd>");

		if (NULL != mdw_text(account, "venue_key"))
		{
			g_string_append(html, "<dt>Venue</dt><dd>");
			venture_html_escape_append(html, mdw_text(account, "venue_key"));
			g_string_append(html, "</dd>");
		}

		for (l = members; NULL != l; l = l->next)
		{
			JsonNode *member = json_object_get_member(attrs, l->data);
			g_autofree gchar *label = g_strdup(l->data);

			if (!JSON_NODE_HOLDS_VALUE(member))
				continue;

			g_strdelimit(label, "_", ' ');
			g_string_append(html, "<dt>");
			venture_html_escape_append(html, label);
			g_string_append(html, "</dt><dd>");

			if (G_TYPE_STRING == json_node_get_value_type(member))
				venture_html_escape_append(html, json_node_get_string(member));
			else if (G_TYPE_BOOLEAN == json_node_get_value_type(member))
				g_string_append(html, json_node_get_boolean(member) ? "yes" : "no");
			else if (G_TYPE_DOUBLE == json_node_get_value_type(member))
				g_string_append_printf(html, "%g", json_node_get_double(member));
			else
				g_string_append_printf(html, "%" G_GINT64_FORMAT, json_node_get_int(member));

			g_string_append(html, "</dd>");
		}

		g_string_append(html, "<dt>First seen</dt><dd>");
		mdw_when(html, account, "first_seen");
		g_string_append(html, "</dd></dl></div></section>");
	}

	return acw_page(self, request, "/accounts", name, html);
}

/* --- /accounts/inventory ------------------------------------------------------------ */

static HtmxResponse *
venture_marketdata_web_inventory(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	static const gchar *const kept[] = {
		"source", "basis", "account", "place", "category", "search", "min_value", "dead", "dead_days",
		"per_page", "login", "group_by", "show_ignored", NULL
	};
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GString) html = NULL;
	VentureMarketdataInventoryQuery query;
	JsonObject *root;
	JsonObject *echo;
	JsonObject *totals;
	JsonArray *rows;
	JsonArray *list;
	const gchar *sort;
	gboolean descending;
	gboolean api;
	HtmxResponse *gate;
	gint64 value;
	gint64 page;
	gint64 pages;
	guint i;

	(void)params;

	api = mdw_is_api(request);
	gate = mdw_gate(self, request, api, VENTURE_USER_ROLE_VIEWER, VENTURE_TYPE_DATA_SOURCE);

	if (NULL != gate)
		return gate;

	venture_marketdata_inventory_query_init(&query);

	if (!venture_web_request_organization(self, request, NULL, &query.organization_id, &error) ||
	    !mdw_number(request, "source", 0, G_MAXINT64, &query.data_source_id, &error) ||
	    !acw_threshold(request, "dead_days", 3650, &query.dead_days, &error) ||
	    !mdw_number(request, "page", 1, 1000000, &value, &error))
		return venture_web_error_response(error);

	query.page = (guint)value;

	if (!mdw_number(request, "per_page", 50, VENTURE_MARKETDATA_ACCOUNTS_MAX_PAGE, &value, &error))
		return venture_web_error_response(error);

	query.per_page = (guint)value;
	query.basis = mdw_param(request, "basis");
	query.account = mdw_param(request, "account");
	query.login = mdw_param(request, "login");
	query.group_by = mdw_param(request, "group_by");
	query.show_ignored = mdw_flag(request, "show_ignored");
	query.place = mdw_param(request, "place");
	query.category = mdw_param(request, "category");
	query.search = mdw_param(request, "search");
	query.min_value = mdw_param(request, "min_value");
	query.dead = mdw_flag(request, "dead");
	query.sort = mdw_param(request, "sort");
	query.descending = (NULL == mdw_param(request, "dir")) || (0 == g_strcmp0(mdw_param(request, "dir"), "desc"));
	answer = venture_marketdata_inventory(self->context, &query, &error);

	if (NULL == answer)
		return venture_web_error_response(error);

	if (api)
		return venture_web_json_response(answer, 200);

	root = json_node_get_object(answer);
	echo = json_object_get_object_member(root, "query");
	totals = json_object_get_object_member(root, "totals");
	rows = json_object_get_array_member(root, "rows");
	sort = mdw_text(echo, "sort");
	descending = json_object_get_boolean_member(echo, "descending");
	html = g_string_new(NULL);
	mdw_head(html, "Accounts", "Inventory",
	         "What every account holds, item by item and place by place, valued on the basis you "
	         "choose, with how long it would take to sell at today's pace.");

	/* Folded unless a filter is in force, so the table is what greets a
	 * person; open, it says what narrowed the list. */
	{
		gboolean filtered = (NULL != query.account) || (NULL != query.place) || (NULL != query.category) ||
		                    (NULL != query.search) || (NULL != query.min_value) || query.dead ||
		                    (NULL != query.basis) || (NULL != query.login) || (NULL != query.group_by);

		g_string_append_printf(html, "<details class=\"card accounts-controls\"%s><summary>Filter and value"
		                             "</summary><form method=\"get\" action=\"/accounts/inventory\" "
		                             "class=\"form-grid market-filters\" role=\"search\" "
		                             "aria-label=\"Inventory filters\">",
		                       filtered ? " open" : "");
	}
	mdw_source_picker(html, root, json_object_get_int_member(root, "data_source_id"), FALSE);
	mdw_input(html, request, "Search", "search", "name or key");

	g_string_append(html, "<div class=\"field\"><label>Account<select name=\"account\">"
	                      "<option value=\"\">every account</option>");
	list = json_object_get_array_member(root, "accounts");

	for (i = 0; (NULL != list) && (i < json_array_get_length(list)); i++)
	{
		JsonObject *account = json_array_get_object_element(list, i);

		g_string_append(html, "<option value=\"");
		venture_html_escape_append(html, mdw_text(account, "key"));
		g_string_append_printf(html, "\"%s>",
		                       (0 == g_strcmp0(mdw_text(account, "key"), query.account)) ? " selected" : "");
		venture_html_escape_append(html, mdw_text(account, "name"));

		if (!venture_string_is_empty(mdw_text(account, "realm")))
		{
			g_string_append(html, " \xc2\xb7 ");
			venture_html_escape_append(html, mdw_text(account, "realm"));
		}

		g_string_append(html, "</option>");
	}

	g_string_append(html, "</select></label></div>");
	acw_login_picker(html, json_object_get_array_member(root, "logins"), query.login);

	if (json_array_get_length(json_object_get_array_member(root, "logins")) > 0)
		g_string_append_printf(html, "<div class=\"field\"><label><input type=\"checkbox\" "
		                             "name=\"group_by\" value=\"login\"%s> Value by login</label></div>",
		                       (0 == g_strcmp0(query.group_by, "login")) ? " checked" : "");

	g_string_append(html, "<div class=\"field\"><label>Place<select name=\"place\">"
	                      "<option value=\"\">everywhere but currencies</option>");
	list = json_object_get_array_member(root, "places");

	for (i = 0; (NULL != list) && (i < json_array_get_length(list)); i++)
	{
		JsonObject *place = json_array_get_object_element(list, i);

		g_string_append(html, "<option value=\"");
		venture_html_escape_append(html, mdw_text(place, "place"));
		g_string_append_printf(html, "\"%s>",
		                       (0 == g_strcmp0(mdw_text(place, "place"), query.place)) ? " selected" : "");
		venture_html_escape_append(html, mdw_text(place, "label"));
		g_string_append(html, "</option>");
	}

	g_string_append(html, "</select></label></div>");
	mdw_input(html, request, "Category", "category", "path, e.g. Trade Goods/Herb");
	mdw_input(html, request, "Worth at least", "min_value", "e.g. 100.00 GOLD");
	acw_select(html, "Value at", "basis", json_object_get_array_member(root, "bases"), mdw_text(root, "basis"));
	g_string_append_printf(html, "<div class=\"field\"><label><input type=\"checkbox\" name=\"dead\" value=\"1\"%s>"
	                             " Dead stock only</label></div>",
	                       query.dead ? " checked" : "");
	mdw_input(html, request, "No sale for (days)", "dead_days", "30");

	if (NULL != query.sort)
	{
		g_string_append(html, "<input type=\"hidden\" name=\"sort\" value=\"");
		venture_html_escape_append(html, query.sort);
		g_string_append_printf(html, "\"><input type=\"hidden\" name=\"dir\" value=\"%s\">",
		                       query.descending ? "desc" : "asc");
	}

	if (query.show_ignored)
		g_string_append(html, "<input type=\"hidden\" name=\"show_ignored\" value=\"1\">");

	g_string_append(html, "<button class=\"btn\" type=\"submit\">Show</button></form></details>");
	mdw_notes(html, root);
	acw_ignored_note(html, request, root, "/accounts/inventory", kept, query.show_ignored);

	{
		g_autofree gchar *units = g_strdup_printf("%" G_GINT64_FORMAT " units in %" G_GINT64_FORMAT " lines",
		                                          json_object_get_int_member(totals, "units"),
		                                          json_object_get_int_member(totals, "lines"));

		g_string_append(html, "<div class=\"stat-row market-stats accounts-stats\">");
		mdw_stat_of(html, "Items", mdw_int, totals, "instruments");
		mdw_stat_of(html, "Value of these", acw_money, totals, "value");
		mdw_stat_of(html, "Whole inventory", acw_money, root, "portfolio_value");
		mdw_stat(html, "Held", units);
		g_string_append(html, "</div>");
	}

	/* The same holdings login by login, when asked: what each login's
	 * characters and banks hold, valued line by line as the rows are. */
	list = json_object_get_array_member(root, "by_login");

	if ((NULL != list) && (json_array_get_length(list) > 0))
	{
		g_string_append(html, "<section class=\"card\" aria-labelledby=\"by-login-h\"><div class=\"card-head\">"
		                      "<h2 id=\"by-login-h\">By login</h2></div><div class=\"table-wrap\">"
		                      "<table class=\"data market-table\"><thead><tr><th scope=\"col\">Login</th>"
		                      "<th scope=\"col\" class=\"num\">Lines</th><th scope=\"col\" class=\"num\">"
		                      "Units</th><th scope=\"col\" class=\"num\">Value</th><th scope=\"col\" "
		                      "class=\"num\">Of these</th></tr></thead><tbody>");

		for (i = 0; i < json_array_get_length(list); i++)
		{
			JsonObject *group = json_array_get_object_element(list, i);

			g_string_append(html, "<tr><td class=\"row-head\">");
			venture_html_escape_append(html, mdw_text(group, "name"));
			g_string_append(html, "</td><td class=\"num\">");
			mdw_int(html, group, "lines");
			g_string_append(html, "</td><td class=\"num\">");
			mdw_int(html, group, "units");
			g_string_append(html, "</td><td class=\"num\">");
			acw_money(html, group, "value");
			g_string_append(html, "</td><td class=\"num\">");
			mdw_percent(html, group, "share", 100.0);
			g_string_append(html, "</td></tr>");
		}

		g_string_append(html, "</tbody></table></div></section>");
	}

	/* Paged on the server, so sorted there (data-server-sort). */
	g_string_append(html, "<div class=\"card\"><div class=\"table-wrap\"><table class=\"data market-table "
	                      "accounts-inventory\" data-server-sort><thead><tr>");
	acw_sort_header(html, request, "/accounts/inventory", kept, sort, descending, "name", "Item", FALSE, TRUE);
	acw_sort_header(html, request, "/accounts/inventory", kept, sort, descending, "quantity", "Units", TRUE, TRUE);
	acw_sort_header(html, request, "/accounts/inventory", kept, sort, descending, "unit_value", "Each", TRUE, TRUE);
	acw_sort_header(html, request, "/accounts/inventory", kept, sort, descending, "value", "Value", TRUE, TRUE);
	g_string_append(html, "<th scope=\"col\" class=\"num\">Of inventory</th>");
	acw_sort_header(html, request, "/accounts/inventory", kept, sort, descending, "days_of_supply",
	                "Days to sell", TRUE, TRUE);
	acw_sort_header(html, request, "/accounts/inventory", kept, sort, descending, "accounts", "Held by", TRUE,
	                TRUE);
	g_string_append(html, "<th scope=\"col\">Last sold</th></tr></thead><tbody>");

	for (i = 0; (NULL != rows) && (i < json_array_get_length(rows)); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);
		JsonArray *breakdown = json_object_get_array_member(row, "breakdown");
		JsonNode *share = json_object_get_member(row, "share");
		guint j;

		g_string_append(html, "<tr><td class=\"row-head\"><a href=\"");
		venture_html_escape_append(html, mdw_text(row, "url"));
		g_string_append(html, "\">");
		mdw_name(html, row, "instrument_name", "instrument_key");
		g_string_append(html, "</a>");

		if (NULL != mdw_text(row, "category"))
		{
			g_string_append(html, "<div class=\"account-sub\">");
			venture_html_escape_append(html, mdw_text(row, "category"));
			g_string_append(html, "</div>");
		}

		g_string_append(html, "</td><td class=\"num\">");
		mdw_int(html, row, "quantity");
		g_string_append(html, "</td><td class=\"num\">");
		acw_money(html, row, "unit_value");
		g_string_append(html, "</td><td class=\"num\">");
		acw_money(html, row, "value");
		g_string_append(html, "</td><td class=\"num\">");

		if ((NULL != share) && JSON_NODE_HOLDS_VALUE(share))
			g_string_append_printf(html, "<span class=\"share-bar\" aria-hidden=\"true\"><span class=\"bar-fill\" "
			                             "style=\"width:%.1f%%\"></span></span> ",
			                       CLAMP(json_node_get_double(share) * 100.0, 0.0, 100.0));

		mdw_percent(html, row, "share", 100.0);
		g_string_append(html, "</td><td class=\"num\">");
		mdw_decimal(html, row, "days_of_supply");
		g_string_append_printf(html, "</td><td class=\"num\"><details class=\"accounts-breakdown\"><summary>%"
		                             G_GINT64_FORMAT " account%s</summary><ul>",
		                       json_object_get_int_member(row, "accounts"),
		                       (1 == json_object_get_int_member(row, "accounts")) ? "" : "s");

		for (j = 0; (NULL != breakdown) && (j < json_array_get_length(breakdown)); j++)
		{
			JsonObject *part = json_array_get_object_element(breakdown, j);

			g_string_append(html, "<li>");
			venture_html_escape_append(html, mdw_text(part, "account_name"));

			if (NULL != mdw_text(part, "login_name"))
			{
				g_string_append(html, " <span class=\"muted\">(");
				venture_html_escape_append(html, mdw_text(part, "login_name"));
				g_string_append(html, ")</span>");
			}

			g_string_append(html, " <span class=\"muted\">");
			venture_html_escape_append(html, mdw_text(part, "place_label"));
			g_string_append(html, "</span> ");
			mdw_int(html, part, "quantity");
			g_string_append(html, "</li>");
		}

		g_string_append(html, "</ul></details></td><td>");
		mdw_when(html, row, "last_sale");
		g_string_append(html, "</td></tr>");
	}

	if ((NULL == rows) || (0 == json_array_get_length(rows)))
		g_string_append(html, "<tr><td colspan=\"8\" class=\"muted\">Nothing held matches.</td></tr>");

	g_string_append(html, "</tbody></table></div>");

	page = json_object_get_int_member(root, "page");
	pages = json_object_get_int_member(root, "pages");

	if (pages > 1)
	{
		g_string_append(html, "<nav class=\"pager\" aria-label=\"Pages\">");

		if (page > 1)
		{
			g_autofree gchar *link = acw_link(request, "/accounts/inventory", kept, sort, descending, page - 1);

			g_string_append(html, "<a class=\"btn btn-sm\" href=\"");
			venture_html_escape_append(html, link);
			g_string_append(html, "\">\xe2\x80\xb9 Prev</a>");
		}

		g_string_append_printf(html, "<span class=\"pager-state\">Page %" G_GINT64_FORMAT " of %" G_GINT64_FORMAT
		                             "</span>", page, pages);

		if (page < pages)
		{
			g_autofree gchar *link = acw_link(request, "/accounts/inventory", kept, sort, descending, page + 1);

			g_string_append(html, "<a class=\"btn btn-sm\" href=\"");
			venture_html_escape_append(html, link);
			g_string_append(html, "\">Next \xe2\x80\xba</a>");
		}

		g_string_append(html, "</nav>");
	}

	g_string_append(html, "</div>");
	mdw_attribution(html, root);

	return acw_page(self, request, "/accounts/inventory", "Inventory", html);
}

/* --- /accounts/pnl -------------------------------------------------------------------- */

/* The window a page or API call names: period=, else the last 30 days. */
static gboolean
acw_pnl_window(
	VentureWebServer	 *self,
	HtmxRequest		 *request,
	VentureMarketdataPnlQuery *query,
	gchar			**out_label,
	GError			**error
){
	g_autoptr(VentureDateRange) range = NULL;
	const gchar *period;
	GDateTime *start;
	GDateTime *end;

	period = mdw_param(request, "period");
	*out_label = g_strdup((NULL != period) ? period : "last_30_days");

	if (NULL == period)
		return TRUE;

	range = venture_context_parse_period(self->context, period, error);

	if (NULL == range)
		return FALSE;

	start = venture_date_range_get_start(range);
	end = venture_date_range_get_end(range);
	query->since = (NULL != start) ? g_date_time_to_unix(start) : -1;
	query->until = (NULL != end) ? g_date_time_to_unix(end) : -1;

	return TRUE;
}

/* The trend's net as bars, and sales against purchases as lines. */
static void
acw_pnl_charts(
	VentureWebServer	*self,
	GString			*html,
	JsonObject		*root
){
	g_autoptr(GPtrArray) labels = NULL;
	g_autoptr(GArray) net = NULL;
	g_autoptr(GArray) sales = NULL;
	g_autoptr(GArray) purchases = NULL;
	g_autofree gchar *bars = NULL;
	g_autofree gchar *lines = NULL;
	VentureWebChartSeries series[2];
	VentureWebChart chart;
	JsonNode *node;
	JsonObject *trend;
	JsonArray *points;
	const gchar *currency;
	const gchar *group;
	guint i;

	node = json_object_get_member(root, "trend");

	if ((NULL == node) || !JSON_NODE_HOLDS_OBJECT(node))
		return;

	trend = json_node_get_object(node);
	points = json_object_get_array_member(trend, "points");
	currency = mdw_text(trend, "currency");
	group = mdw_text(trend, "group");
	labels = g_ptr_array_new_with_free_func(g_free);
	net = g_array_new(FALSE, FALSE, sizeof(gdouble));
	sales = g_array_new(FALSE, FALSE, sizeof(gdouble));
	purchases = g_array_new(FALSE, FALSE, sizeof(gdouble));

	for (i = 0; (NULL != points) && (i < json_array_get_length(points)); i++)
	{
		JsonObject *point = json_array_get_object_element(points, i);
		gdouble v;

		g_ptr_array_add(labels, acw_period_label(mdw_text(point, "period_start"),
		                                       (0 == g_strcmp0(group, "month")) ? "%b %Y" : "%b %d"));
		v = (gdouble)json_object_get_int_member(point, "net");
		g_array_append_val(net, v);
		v = (gdouble)json_object_get_int_member(point, "sales");
		g_array_append_val(sales, v);
		v = (gdouble)json_object_get_int_member(point, "purchases");
		g_array_append_val(purchases, v);
	}

	g_ptr_array_add(labels, NULL);

	if (0 == net->len)
		return;

	memset(series, 0, sizeof(series));
	series[0].name = "Net";
	series[0].values = (const gdouble *)(gpointer)net->data;
	series[0].format = acw_format_money;
	series[0].format_data = (gpointer)currency;
	memset(&chart, 0, sizeof(chart));
	chart.title = "Net per period";
	chart.summary = "Sales after the cut and other income, less purchases and expenses.";
	chart.labels = (const gchar *const *)labels->pdata;
	chart.n_points = net->len;
	chart.series = series;
	chart.n_series = 1;
	bars = venture_web_chart_bar(&chart);

	series[0].name = "Sales";
	series[0].values = (const gdouble *)(gpointer)sales->data;
	series[1].name = "Purchases and expenses";
	series[1].values = (const gdouble *)(gpointer)purchases->data;
	series[1].format = acw_format_money;
	series[1].format_data = (gpointer)currency;
	chart.title = "Money in and out per period";
	chart.summary = "What sold, after the venue's cut, against what was spent.";
	chart.series = series;
	chart.n_series = 2;
	lines = venture_web_chart_line(&chart);

	g_string_append(html, "<div class=\"grid cols-2 accounts-charts\"><section class=\"card\"><div class=\"card-body\">");
	g_string_append(html, bars);
	g_string_append(html, "</div></section><section class=\"card\"><div class=\"card-body\">");
	g_string_append(html, lines);
	g_string_append(html, "</div></section></div>");
}

/* One currency's totals as the page's headline row. */
static void
acw_stat_money(
	GString		*html,
	const gchar	*label,
	JsonObject	*object,
	const gchar	*member,
	const gchar	*note
){
	g_string_append(html, "<div class=\"stat\"><span class=\"stat-label\">");
	venture_html_escape_append(html, label);
	g_string_append(html, "</span><span class=\"stat-value market-figure\">");
	acw_money(html, object, member);
	g_string_append(html, "</span>");

	if (NULL != note)
	{
		g_string_append(html, "<span class=\"stat-note\">");
		venture_html_escape_append(html, note);
		g_string_append(html, "</span>");
	}

	g_string_append(html, "</div>");
}

/* One currency's totals as the page's headline row. */
static void
acw_pnl_totals(
	GString		*html,
	JsonObject	*total
){
	g_autofree gchar *sold = g_strdup_printf("%" G_GINT64_FORMAT " units in %" G_GINT64_FORMAT " sales",
	                                         json_object_get_int_member(total, "sold_units"),
	                                         json_object_get_int_member(total, "sales"));
	g_autofree gchar *bought = g_strdup_printf("%" G_GINT64_FORMAT " units in %" G_GINT64_FORMAT " buys",
	                                           json_object_get_int_member(total, "bought_units"),
	                                           json_object_get_int_member(total, "buys"));
	g_autofree gchar *expired = g_strdup_printf("%" G_GINT64_FORMAT " units expired, %" G_GINT64_FORMAT
	                                            " cancelled",
	                                            json_object_get_int_member(total, "expired_units"),
	                                            json_object_get_int_member(total, "cancelled_units"));

	g_string_append(html, "<div class=\"stat-row market-stats accounts-stats\">");
	acw_stat_money(html, "Sales, after the cut", total, "sales_amount", sold);
	acw_stat_money(html, "Purchases", total, "buys_amount", bought);
	acw_stat_money(html, "Other income", total, "income", NULL);
	acw_stat_money(html, "Expenses", total, "expense", NULL);
	acw_stat_money(html, "Net", total, "net", expired);
	g_string_append(html, "</div>");
}

static HtmxResponse *
venture_marketdata_web_pnl(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	static const gchar *const periods[] = {
		"last_7_days", "last_30_days", "last_90_days", "this_month", "last_month", "this_year",
		"last_year", "all", NULL
	};
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) answer = NULL;
	g_autoptr(GString) html = NULL;
	g_autofree gchar *period = NULL;
	VentureMarketdataPnlQuery query;
	JsonObject *root;
	JsonObject *flips;
	JsonArray *array;
	gboolean api;
	HtmxResponse *gate;
	gint64 value;
	guint i;

	(void)params;

	api = mdw_is_api(request);
	gate = mdw_gate(self, request, api, VENTURE_USER_ROLE_VIEWER, VENTURE_TYPE_DATA_SOURCE);

	if (NULL != gate)
		return gate;

	venture_marketdata_pnl_query_init(&query);

	if (!venture_web_request_organization(self, request, NULL, &query.organization_id, &error) ||
	    !mdw_number(request, "source", 0, G_MAXINT64, &query.data_source_id, &error) ||
	    !mdw_number(request, "top", 0, VENTURE_MARKETDATA_ACCOUNTS_MAX_PAGE, &value, &error) ||
	    !acw_pnl_window(self, request, &query, &period, &error))
		return venture_web_error_response(error);

	query.top = (guint)value;
	query.group_by = mdw_param(request, "group_by");
	query.account = mdw_param(request, "account");
	query.login = mdw_param(request, "login");
	query.venue = mdw_param(request, "venue");
	query.show_ignored = mdw_flag(request, "show_ignored");
	query.instrument = mdw_param(request, "instrument");
	query.source = mdw_param(request, "label");
	answer = venture_marketdata_external_pnl(self->context, &query, &error);

	if (NULL == answer)
		return venture_web_error_response(error);

	if (api)
		return venture_web_json_response(answer, 200);

	root = json_node_get_object(answer);
	html = g_string_new(NULL);
	mdw_head(html, "Accounts", "Profit and loss",
	         "The source's own ledger summed: sales after the venue's cut, purchases, other income "
	         "and expenses, and the flips -- buys matched to the sales that followed them.");

	{
		g_autofree gchar *shown = g_strdup(period);

		g_strdelimit(shown, "_", ' ');
		g_string_append(html, "<details class=\"card accounts-controls\"><summary>");
		venture_html_escape_append(html, shown);
		g_string_append(html, ", by ");
		venture_html_escape_append(html, mdw_text(root, "group_by"));
		g_string_append(html, "</summary><form method=\"get\" action=\"/accounts/pnl\" "
		                      "class=\"form-grid market-filters\" aria-label=\"Profit and loss filters\">");
	}
	mdw_source_picker(html, root, json_object_get_int_member(root, "data_source_id"), FALSE);
	g_string_append(html, "<div class=\"field\"><label>Period<select name=\"period\">");

	/* A period typed into the address (a span, a month) stays chosen:
	 * without its own option the select fell back to the first preset and
	 * the next Show answered a different window. */
	if (!g_strv_contains(periods, period))
	{
		g_string_append(html, "<option value=\"");
		venture_html_escape_append(html, period);
		g_string_append(html, "\" selected>");
		venture_html_escape_append(html, period);
		g_string_append(html, "</option>");
	}

	for (i = 0; NULL != periods[i]; i++)
	{
		g_autofree gchar *shown = g_strdup(periods[i]);

		g_strdelimit(shown, "_", ' ');
		g_string_append_printf(html, "<option value=\"%s\"%s>%s</option>", periods[i],
		                       (0 == g_strcmp0(periods[i], period)) ? " selected" : "", shown);
	}

	g_string_append(html, "</select></label></div>");
	acw_select(html, "Group by", "group_by", json_object_get_array_member(root, "groups"),
	           mdw_text(root, "group_by"));
	acw_login_picker(html, json_object_get_array_member(root, "logins"), query.login);
	mdw_input(html, request, "Account", "account", "an account's key");
	mdw_input(html, request, "Venue", "venue", "a venue's key");
	mdw_input(html, request, "Item", "instrument", "an item's key");
	mdw_input(html, request, "Source label", "label", "e.g. Auction");

	if (query.show_ignored)
		g_string_append(html, "<input type=\"hidden\" name=\"show_ignored\" value=\"1\">");

	g_string_append(html, "<button class=\"btn\" type=\"submit\">Show</button></form></details>");
	mdw_notes(html, root);
	{
		static const gchar *const kept[] = {
			"source", "period", "since", "until", "group_by", "login", "account", "venue", "instrument",
			"label", "top", NULL
		};

		acw_ignored_note(html, request, root, "/accounts/pnl", kept, query.show_ignored);
	}

	array = json_object_get_array_member(root, "totals");

	for (i = 0; (NULL != array) && (i < json_array_get_length(array)); i++)
		acw_pnl_totals(html, json_array_get_object_element(array, i));

	if ((NULL == array) || (0 == json_array_get_length(array)))
		g_string_append(html, "<div class=\"card\"><div class=\"card-body\"><p class=\"muted\">No trades in "
		                      "this window.</p></div></div>");

	acw_pnl_charts(self, html, root);

	/* The table by the grouping asked for. */
	array = json_object_get_array_member(root, "buckets");
	g_string_append(html, "<section class=\"card\" aria-labelledby=\"buckets-h\"><div class=\"card-head\">"
	                      "<h2 id=\"buckets-h\">By ");
	venture_html_escape_append(html, mdw_text(root, "group_by"));
	g_string_append(html, "</h2></div><div class=\"table-wrap\"><table class=\"data market-table\"><thead><tr>"
	                      "<th scope=\"col\"></th><th scope=\"col\" class=\"num\">Sales</th>"
	                      "<th scope=\"col\" class=\"num\">Units sold</th><th scope=\"col\" class=\"num\">Purchases</th>"
	                      "<th scope=\"col\" class=\"num\">Units bought</th><th scope=\"col\" class=\"num\">Income</th>"
	                      "<th scope=\"col\" class=\"num\">Expenses</th><th scope=\"col\" class=\"num\">Net</th>"
	                      "<th scope=\"col\" class=\"num\">Expired</th></tr></thead><tbody>");

	for (i = 0; (NULL != array) && (i < json_array_get_length(array)); i++)
	{
		JsonObject *row = json_array_get_object_element(array, i);
		const gchar *url = mdw_text(row, "url");

		g_string_append(html, "<tr><td class=\"row-head\">");

		if (NULL != mdw_text(row, "period_start"))
		{
			g_autofree gchar *label = acw_period_label(mdw_text(row, "period_start"),
			                                         (0 == g_strcmp0(mdw_text(root, "group_by"), "month"))
			                                         ? "%B %Y" : "%a %b %d");

			venture_html_escape_append(html, label);
		}
		else
		{
			const gchar *label = mdw_text(row, "label");

			if (NULL != url)
			{
				g_string_append(html, "<a href=\"");
				venture_html_escape_append(html, url);
				g_string_append(html, "\">");
			}

			venture_html_escape_append(html, venture_string_is_empty(label) ? "(none)" : label);

			if (NULL != url)
				g_string_append(html, "</a>");
		}

		g_string_append(html, "</td><td class=\"num\">");
		acw_money(html, row, "sales_amount");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_int(html, row, "sold_units");
		g_string_append(html, "</td><td class=\"num\">");
		acw_money(html, row, "buys_amount");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_int(html, row, "bought_units");
		g_string_append(html, "</td><td class=\"num\">");
		acw_money(html, row, "income");
		g_string_append(html, "</td><td class=\"num\">");
		acw_money(html, row, "expense");
		g_string_append(html, "</td><td class=\"num market-figure\">");
		acw_money(html, row, "net");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_int(html, row, "expired_units");
		g_string_append(html, "</td></tr>");
	}

	if ((NULL == array) || (0 == json_array_get_length(array)))
		g_string_append(html, "<tr><td colspan=\"9\" class=\"muted\">Nothing in this window.</td></tr>");

	g_string_append(html, "</tbody></table></div></section>");

	/* Top items and flips side by side on a wide screen. */
	array = json_object_get_array_member(root, "top_items");
	g_string_append(html, "<section class=\"card\" aria-labelledby=\"top-h\"><div class=\"card-head\">"
	                      "<h2 id=\"top-h\">Top items by profit</h2></div><div class=\"table-wrap\">"
	                      "<table class=\"data market-table\"><thead><tr><th scope=\"col\">Item</th>"
	                      "<th scope=\"col\" class=\"num\">Sold</th><th scope=\"col\" class=\"num\">Sales</th>"
	                      "<th scope=\"col\" class=\"num\">Bought</th><th scope=\"col\" class=\"num\">Purchases</th>"
	                      "<th scope=\"col\" class=\"num\">Profit</th></tr></thead><tbody>");

	for (i = 0; (NULL != array) && (i < json_array_get_length(array)); i++)
	{
		JsonObject *row = json_array_get_object_element(array, i);

		g_string_append(html, "<tr><td class=\"row-head\"><a href=\"");
		venture_html_escape_append(html, mdw_text(row, "url"));
		g_string_append(html, "\">");
		mdw_name(html, row, "instrument_name", "instrument_key");
		g_string_append(html, "</a></td><td class=\"num\">");
		mdw_int(html, row, "sold_units");
		g_string_append(html, "</td><td class=\"num\">");
		acw_money(html, row, "sales_amount");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_int(html, row, "bought_units");
		g_string_append(html, "</td><td class=\"num\">");
		acw_money(html, row, "buys_amount");
		g_string_append(html, "</td><td class=\"num market-figure\">");
		acw_money(html, row, "profit");
		g_string_append(html, "</td></tr>");
	}

	if ((NULL == array) || (0 == json_array_get_length(array)))
		g_string_append(html, "<tr><td colspan=\"6\" class=\"muted\">No items traded.</td></tr>");

	g_string_append(html, "</tbody></table></div></section>");

	flips = json_object_get_object_member(root, "flips");
	array = json_object_get_array_member(flips, "rows");
	g_string_append(html, "<section class=\"card\" aria-labelledby=\"flips-h\"><div class=\"card-head\">"
	                      "<h2 id=\"flips-h\">Flips</h2><span class=\"muted\">buys matched to later sales, "
	                      "first in, first out</span></div>");

	{
		JsonArray *flip_totals = json_object_get_array_member(flips, "totals");

		if ((NULL != flip_totals) && (json_array_get_length(flip_totals) > 0))
		{
			JsonObject *total = json_array_get_object_element(flip_totals, 0);

			g_string_append(html, "<div class=\"stat-row market-stats\">");
			mdw_stat_of(html, "Cost of what sold", acw_money, total, "cost");
			mdw_stat_of(html, "It sold for", acw_money, total, "proceeds");
			mdw_stat_of(html, "Flip profit", acw_money, total, "profit");
			mdw_stat_of(html, "Return", mdw_share, total, "roi");
			g_string_append(html, "</div>");
		}
	}

	g_string_append(html, "<div class=\"table-wrap\"><table class=\"data market-table accounts-flips\"><thead><tr>"
	                      "<th scope=\"col\">Item</th><th scope=\"col\" class=\"num\">Units</th>"
	                      "<th scope=\"col\" class=\"num\">Bought at</th><th scope=\"col\" class=\"num\">Sold at</th>"
	                      "<th scope=\"col\" class=\"num\">Profit</th><th scope=\"col\" class=\"num\">Return</th>"
	                      "<th scope=\"col\" class=\"num\">Still open</th><th scope=\"col\" class=\"num\">Held now</th>"
	                      "</tr></thead><tbody>");

	for (i = 0; (NULL != array) && (i < json_array_get_length(array)); i++)
	{
		JsonObject *row = json_array_get_object_element(array, i);

		g_string_append(html, "<tr><td class=\"row-head\"><a href=\"");
		venture_html_escape_append(html, mdw_text(row, "url"));
		g_string_append(html, "\">");
		mdw_name(html, row, "instrument_name", "instrument_key");
		g_string_append(html, "</a></td><td class=\"num\">");
		mdw_int(html, row, "matched_units");
		g_string_append(html, "</td><td class=\"num\">");
		acw_money(html, row, "avg_buy");
		g_string_append(html, "</td><td class=\"num\">");
		acw_money(html, row, "avg_sell");
		g_string_append(html, "</td><td class=\"num market-figure\">");
		acw_money(html, row, "profit");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_share(html, row, "roi");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_int(html, row, "open_units");
		g_string_append(html, "</td><td class=\"num\">");
		mdw_int(html, row, "held");
		g_string_append(html, "</td></tr>");
	}

	if ((NULL == array) || (0 == json_array_get_length(array)))
		g_string_append(html, "<tr><td colspan=\"8\" class=\"muted\">No flips: nothing bought in this window "
		                      "has sold yet.</td></tr>");

	g_string_append(html, "</tbody></table></div></section>");
	mdw_attribution(html, root);

	return acw_page(self, request, "/accounts/pnl", "Profit and loss", html);
}

/* --- Routes ---------------------------------------------------------------------------- */

/* --- Ignoring accounts: the switches ---------------------------------------------- */

/*
 * POST /accounts/ignores: kind (character or realm), key, name, back.
 * Ignoring what is already ignored is not an error -- the second click of
 * a double click -- so an existing record is left as it is.
 */
static HtmxResponse *
venture_marketdata_web_ignore(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) record = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) existing = NULL;
	const gchar *kind;
	const gchar *key;
	const gchar *name;
	VentureActor actor;
	gint64 organization_id;
	HtmxResponse *gate;

	(void)params;

	gate = mdw_gate(self, request, FALSE, VENTURE_USER_ROLE_EDITOR, VENTURE_TYPE_ACCOUNT_IGNORE);

	if (NULL != gate)
		return gate;

	if (!venture_web_request_organization(self, request, NULL, &organization_id, &error))
		return venture_web_error_response(error);

	kind = htmx_request_get_form_value(request, "kind");
	key = htmx_request_get_form_value(request, "key");
	name = htmx_request_get_form_value(request, "name");

	query = venture_query_new(VENTURE_TYPE_ACCOUNT_IGNORE);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 1);

	if ((NULL != kind) && (NULL != key) &&
	    venture_query_add_filter_string(query, "kind", VENTURE_FILTER_OP_EQ, kind, NULL) &&
	    venture_query_add_filter_string(query, "key", VENTURE_FILTER_OP_EQ, key, NULL))
		existing = venture_database_find(venture_context_get_database(self->context), query, NULL);

	if ((NULL != existing) && (existing->len > 0))
		return venture_web_redirect_to(acw_back(request));

	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);
	record = VENTURE_ENTITY(venture_account_ignore_new());
	venture_entity_set_organization_id(record, organization_id);
	g_object_set(record, "kind", kind, "key", key,
	             "name", !venture_string_is_empty(name) ? name : key, NULL);

	if (!venture_database_save(venture_context_get_database(self->context), record, &actor, &error))
		return venture_web_error_response(error);

	return venture_web_redirect_to(acw_back(request));
}

/* POST /accounts/ignores/:id/delete: the switch off again. */
static HtmxResponse *
venture_marketdata_web_unignore(
	HtmxRequest	*request,
	GHashTable	*params,
	gpointer	 data
){
	VentureWebServer *self = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureAuthPrincipal) principal = NULL;
	g_autoptr(VentureEntity) record = NULL;
	VentureActor actor;
	gint64 organization_id;
	gint64 id;
	HtmxResponse *gate;

	gate = mdw_gate(self, request, FALSE, VENTURE_USER_ROLE_EDITOR, VENTURE_TYPE_ACCOUNT_IGNORE);

	if (NULL != gate)
		return gate;

	if (!venture_web_request_organization(self, request, NULL, &organization_id, &error) ||
	    !mdw_path_id(params, "id", &id, &error))
		return venture_web_error_response(error);

	record = venture_database_get(venture_context_get_database(self->context), VENTURE_TYPE_ACCOUNT_IGNORE,
	                              id, NULL);

	if ((NULL == record) || (venture_entity_get_organization_id(record) != organization_id))
	{
		g_set_error(&error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND,
		            "No ignored account #%" G_GINT64_FORMAT " in this organization", id);
		return venture_web_error_response(error);
	}

	principal = venture_auth_authenticate(self->auth, request);
	venture_auth_to_actor(principal, &actor);

	if (!venture_entity_is_deleted(record) &&
	    !venture_database_delete(venture_context_get_database(self->context), record, &actor, &error))
		return venture_web_error_response(error);

	return venture_web_redirect_to(acw_back(request));
}

static void
venture_marketdata_accounts_pages_register(VentureWebServer *self)
{
	static const struct
	{
		HtmxMethod		 method;
		const gchar		*path;
		HtmxRouteCallback	 handler;
	} routes[] = {
		/* The fixed pages before the account pattern: a router takes the
		 * first match. */
		{ HTMX_METHOD_GET, "/accounts", venture_marketdata_web_accounts },
		{ HTMX_METHOD_GET, "/accounts/inventory", venture_marketdata_web_inventory },
		{ HTMX_METHOD_GET, "/accounts/pnl", venture_marketdata_web_pnl },
		{ HTMX_METHOD_POST, "/accounts/ignores", venture_marketdata_web_ignore },
		{ HTMX_METHOD_POST, "/accounts/ignores/:id/delete", venture_marketdata_web_unignore },
		/* The key is the rest of the path: it may hold a slash. */
		{ HTMX_METHOD_GET, "/accounts/:source/*", venture_marketdata_web_account },
		{ HTMX_METHOD_GET, "/api/v1/accounts", venture_marketdata_web_accounts },
		{ HTMX_METHOD_GET, "/api/v1/accounts/inventory", venture_marketdata_web_inventory },
		{ HTMX_METHOD_GET, "/api/v1/accounts/pnl", venture_marketdata_web_pnl },
		{ HTMX_METHOD_GET, "/api/v1/accounts/:source/*", venture_marketdata_web_account },
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(routes); i++)
		venture_web_server_add_classified_route(self, routes[i].method, routes[i].path,
		                                        VENTURE_DATA_CLASS_TENANT, VENTURE_HOSTED_ROUTE_NONE,
		                                        routes[i].handler, self);
}
