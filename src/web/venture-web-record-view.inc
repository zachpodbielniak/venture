/*
 * venture-web-record-view.inc - How a record reads to a person
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Included by venture-web-server.c. Everything a record page, a list and a
 * form decide about presentation is decided here, from the field table,
 * through one classification (venture_field_spec_get_role()):
 *
 *   - the title is the record's display name, and its status -- if it has
 *     one -- is a badge beside it;
 *   - the content a person wrote (long text) is given room to be read;
 *   - the facts that are set are listed under Details; empty ones are not;
 *   - machinery -- replay keys, digests, revisions, the identity spine --
 *     is one click away under "All fields", never on the page by default.
 *
 * The same roles choose a list's default columns and group a form, so the
 * three surfaces cannot disagree about what matters on a record. Nothing
 * here knows a record type by name: a type added tomorrow, or a plugin's,
 * reads the same way the day it registers.
 */

/* How many facts the Details card shows before "All fields" takes over. */
#define VENTURE_WEB_SUMMARY_FACTS	10

/* How many columns a list shows by default. */
#define VENTURE_WEB_LIST_COLUMNS	6

/*
 * A document line's delete, shared by every composer: a bin in the one
 * red, sized to be hit on purpose. A bare "x" was too small to find.
 */
#define VENTURE_WEB_LINE_DELETE \
	"<td class=\"line-remove\"><button type=\"button\" class=\"line-delete\" data-remove-line " \
	"aria-label=\"Remove this line\" title=\"Remove this line\">" \
	"<svg viewBox=\"0 0 24 24\" width=\"18\" height=\"18\" fill=\"none\" stroke=\"currentColor\" " \
	"stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\" aria-hidden=\"true\">" \
	"<path d=\"M3 6h18\"/><path d=\"M8 6V4h8v2\"/><path d=\"M19 6l-1 14H6L5 6\"/>" \
	"<path d=\"M10 11v6\"/><path d=\"M14 11v6\"/></svg></button></td>"

/* Past this many optional fields, a form tucks the rest under "More". */
#define VENTURE_WEB_FORM_OPEN_FIELDS	6

/*
 * What a person calls @type, singular or plural.
 *
 * Returns: (transfer full): the label
 */
static gchar *
venture_web_type_label(
	GType		type,
	gboolean	plural
){
	return venture_entity_type_dup_label(type, plural);
}

/*
 * Whether @spec holds nothing worth a row on @record: an empty string, an
 * unset date, a zero reference, a false flag, a zero amount of money.
 *
 * A false flag is deliberately empty. "No reminders: no" is two words of
 * nothing; the flag says something only when it is set, and the full
 * field list under "All fields" still shows it either way.
 */
static gboolean
venture_web_value_is_empty(
	VentureEntity		*record,
	VentureFieldSpec	*spec
){
	g_auto(GValue) value = G_VALUE_INIT;

	if (!venture_entity_get_field(record, venture_field_spec_get_name(spec),
	                              &value))
		return TRUE;

	if (G_VALUE_HOLDS(&value, VENTURE_TYPE_MONEY))
	{
		const VentureMoney *money;

		money = g_value_get_boxed(&value);

		return (NULL == money) || (0 == venture_money_get_amount(money));
	}

	if (G_VALUE_HOLDS(&value, G_TYPE_DATE_TIME))
		return NULL == g_value_get_boxed(&value);

	if (G_VALUE_HOLDS_BOOLEAN(&value))
		return !g_value_get_boolean(&value);

	if (G_VALUE_HOLDS_STRING(&value))
		return venture_string_is_empty(g_value_get_string(&value));

	if (G_VALUE_HOLDS_INT64(&value) &&
	    (VENTURE_FIELD_KIND_REFERENCE == venture_field_spec_get_kind(spec)))
		return 0 == g_value_get_int64(&value);

	if (G_VALUE_HOLDS_ENUM(&value))
		return FALSE;

	/* Zero is what an unset number reads as: "Estimate 0.0" and
	 * "Points 0" on a ticket nobody has estimated say nothing. */
	if (G_VALUE_HOLDS_INT64(&value))
		return 0 == g_value_get_int64(&value);

	if (G_VALUE_HOLDS_INT(&value))
		return 0 == g_value_get_int(&value);

	if (G_VALUE_HOLDS_DOUBLE(&value))
		return 0.0 == g_value_get_double(&value);

	{
		g_autoptr(JsonNode) node = NULL;

		node = venture_json_node_from_value(&value);

		return (NULL == node) || JSON_NODE_HOLDS_NULL(node);
	}
}

/*
 * An enumeration value as a person reads it: the label a declarative type
 * gave it, else the nick with its underscores taken out.
 *
 * Returns: (transfer full): the label
 */
static gchar *
venture_web_choice_label(
	VentureFieldSpec	*spec,
	const gchar		*nick
){
	const gchar *declared;

	declared = (NULL != spec)
		? venture_field_spec_get_choice_label(spec, nick) : NULL;

	if (!venture_string_is_empty(declared))
		return g_strdup(declared);

	return venture_web_label_from_name(nick);
}

/*
 * Which of the four status tones a lifecycle value reads as. Words, not a
 * table per type: a status that ended well is green whatever the type
 * calls it, and one that needs somebody is amber.
 *
 * Returns: (transfer none): "positive", "negative", "warning" or "info"
 */
static const gchar *
venture_web_status_tone(const gchar *nick)
{
	static const gchar *const positive[] = {
		"active", "won", "closed_won", "paid", "done", "completed",
		"complete", "resolved", "published", "approved", "accepted",
		"sent", "delivered", "succeeded", "success", "released",
		"converted", "qualified", "reconciled", "posted", "live",
		"healthy", "shipped", "fulfilled", "open_for_business", NULL
	};
	static const gchar *const negative[] = {
		"lost", "closed_lost", "overdue", "failed", "failure",
		"cancelled", "canceled", "void", "voided", "rejected",
		"declined", "blocked", "unqualified", "bounced", "disputed",
		"written_off", "churned", "breached", "error", NULL
	};
	static const gchar *const warning[] = {
		"pending", "draft", "in_progress", "open", "triage", "review",
		"partial", "partially_paid", "awaiting", "waiting", "on_hold",
		"paused", "mitigated", "new", "planned", "scheduled", "queued",
		"proposal", "negotiation", "contacted", "nurturing", "at_risk",
		NULL
	};

	if (NULL == nick)
		return "info";

	if (g_strv_contains(positive, nick))
		return "positive";

	if (g_strv_contains(negative, nick))
		return "negative";

	if (g_strv_contains(warning, nick))
		return "warning";

	return "info";
}

/*
 * Whether a string field is an address somebody would click: an email
 * goes to mailto:, a phone to tel:, a web address to itself. By name,
 * because that is what every table calls them.
 */
static void
venture_web_append_string_value(
	GString		*html,
	const gchar	*name,
	const gchar	*text
){
	g_autofree gchar *shown = NULL;

	/* A URI carrying a password never reaches a page. */
	shown = venture_string_redact_uri(text);

	if (g_str_has_suffix(name, "email") && (NULL != strchr(shown, '@')) &&
	    (NULL == strpbrk(shown, " <>\"'")))
	{
		g_string_append(html, "<a href=\"mailto:");
		venture_html_escape_append(html, shown);
		g_string_append(html, "\">");
		venture_html_escape_append(html, shown);
		g_string_append(html, "</a>");
		return;
	}

	if (g_str_has_suffix(name, "phone") &&
	    (strspn(shown, "+0123456789 ()-.") == strlen(shown)))
	{
		g_string_append(html, "<a href=\"tel:");
		venture_html_escape_append(html, shown);
		g_string_append(html, "\">");
		venture_html_escape_append(html, shown);
		g_string_append(html, "</a>");
		return;
	}

	if (venture_web_url_is_web(shown))
	{
		g_string_append(html, "<a rel=\"noreferrer\" href=\"");
		venture_html_escape_append(html, shown);
		g_string_append(html, "\">");
		venture_html_escape_append(html, shown);
		g_string_append(html, "</a>");
		return;
	}

	venture_html_escape_append(html, shown);
}

/*
 * One value, the way a person reads it. Empty is an en dash in the muted
 * colour; the callers that list only what is set never reach it.
 */
static void
venture_web_append_value(
	VentureWebServer	*self,
	GString			*html,
	VentureEntity		*record,
	VentureFieldSpec	*spec
){
	g_auto(GValue) value = G_VALUE_INIT;
	const gchar *name;

	name = venture_field_spec_get_name(spec);

	if (venture_web_value_is_empty(record, spec) &&
	    (VENTURE_FIELD_KIND_BOOLEAN != venture_field_spec_get_kind(spec)))
	{
		g_string_append(html, "<span class=\"muted\">\xe2\x80\x94</span>");
		return;
	}

	if (!venture_entity_get_field(record, name, &value))
		return;

	if (G_VALUE_HOLDS(&value, G_TYPE_DATE_TIME))
	{
		GDateTime *when;
		g_autofree gchar *text = NULL;
		GTimeZone *zone;

		when = g_value_get_boxed(&value);
		zone = venture_context_get_timezone(self->context);

		/* A calendar date is stored as midnight UTC and reads as that
		 * day; an instant reads with its time in the configured zone. */
		if ((VENTURE_FIELD_KIND_DATE == venture_field_spec_get_kind(spec)) ||
		    ((0 == g_date_time_get_hour(when)) &&
		     (0 == g_date_time_get_minute(when)) &&
		     (0 == g_date_time_get_second(when))))
		{
			g_autofree gchar *iso = NULL;

			iso = venture_time_to_date_string(when, zone);
			text = g_strdup(iso);
		}
		else
		{
			g_autoptr(GDateTime) local = NULL;

			/* The same shape as a date, with the time after it, so a
			 * column or a card of both reads as one kind of thing. */
			local = g_date_time_to_timezone(when, zone);
			text = (NULL != local)
				? g_date_time_format(local, "%Y-%m-%d %H:%M") : NULL;
		}

		g_string_append(html, "<time>");
		venture_html_escape_append(html, text);
		g_string_append(html, "</time>");
		return;
	}

	if (G_VALUE_HOLDS_ENUM(&value))
	{
		g_autofree gchar *label = NULL;

		label = venture_web_choice_label(spec,
			venture_enum_to_nick(G_VALUE_TYPE(&value),
			                     g_value_get_enum(&value)));
		venture_html_escape_append(html, label);
		return;
	}

	if (G_VALUE_HOLDS_BOOLEAN(&value))
	{
		g_string_append(html, g_value_get_boolean(&value) ? "Yes" : "No");
		return;
	}

	/* An enumeration declared by a YAML type is a string with choices,
	 * and a status kept as a string is a word all the same. */
	if (G_VALUE_HOLDS_STRING(&value) &&
	    ((VENTURE_FIELD_KIND_ENUM == venture_field_spec_get_kind(spec)) ||
	     (VENTURE_FIELD_ROLE_STATUS == venture_field_spec_get_role(spec))))
	{
		g_autofree gchar *label = NULL;

		label = venture_web_choice_label(spec, g_value_get_string(&value));
		venture_html_escape_append(html, label);
		return;
	}

	if (G_VALUE_HOLDS_STRING(&value) &&
	    (VENTURE_FIELD_KIND_TEXT != venture_field_spec_get_kind(spec)))
	{
		venture_web_append_string_value(html, name,
		                                g_value_get_string(&value));
		return;
	}

	if (VENTURE_FIELD_KIND_REFERENCE == venture_field_spec_get_kind(spec))
	{
		gint64 id;
		g_autoptr(VentureEntity) target = NULL;
		const gchar *type_name;
		GType target_type;

		id = G_VALUE_HOLDS_INT64(&value) ? g_value_get_int64(&value) : 0;
		type_name = venture_field_spec_get_reference_type(spec);
		target_type = venture_entity_registry_lookup(
			venture_context_get_entity_registry(self->context), type_name);

		if (G_TYPE_INVALID != target_type)
			target = venture_database_get(
				venture_context_get_database(self->context),
				target_type, id, NULL);

		if (NULL == target)
		{
			/* Gone, or not the reader's to see; the number would mean
			 * nothing to them either way. */
			g_string_append(html,
				"<span class=\"muted\">Deleted or unavailable</span>");
			return;
		}

		{
			g_autofree gchar *label = NULL;

			label = venture_entity_get_display_name(target);
			g_string_append_printf(html,
				"<a href=\"/e/%s/%" G_GINT64_FORMAT "\">", type_name, id);
			venture_html_escape_append(html, label);
			g_string_append(html, "</a>");
		}
		return;
	}

	/* Money, numbers, prose and the rest: the detail renderer already
	 * knows how each of them reads. */
	venture_web_append_detail_value(self, html, record, spec);
}

/*
 * The field specs a person may see on @record, in the order the type
 * declared them. Sensitive fields are not in it: they are never shown,
 * not even collapsed.
 *
 * Returns: (transfer container): the visible field specs
 */
static GPtrArray *
venture_web_visible_specs(VentureEntity *record)
{
	g_autoptr(GPtrArray) specs = NULL;
	GPtrArray *visible;
	guint i;

	specs = venture_entity_get_field_specs(record);
	g_ptr_array_sort_values(specs, venture_field_spec_compare_display_order);

	visible = g_ptr_array_new_with_free_func(
		(GDestroyNotify)venture_field_spec_free);

	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec;

		spec = g_ptr_array_index(specs, i);

		if (0 != (venture_field_spec_get_flags(spec) &
		          VENTURE_COLUMN_FLAG_SENSITIVE))
			continue;

		g_ptr_array_add(visible, venture_field_spec_copy(spec));
	}

	return visible;
}

/*
 * The record's status field, if it has one.
 *
 * Returns: (transfer none) (nullable): the spec, owned by @specs
 */
static VentureFieldSpec *
venture_web_status_spec(GPtrArray *specs)
{
	guint i;

	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec;

		spec = g_ptr_array_index(specs, i);

		if (VENTURE_FIELD_ROLE_STATUS == venture_field_spec_get_role(spec))
			return spec;
	}

	return NULL;
}

/*
 * The status as a badge: the humanised value in the tone its word reads
 * as. Nothing when the record has no status.
 */
static void
venture_web_append_status_badge(
	GString			*html,
	VentureEntity		*record,
	VentureFieldSpec	*spec
){
	g_auto(GValue) value = G_VALUE_INIT;
	g_autofree gchar *label = NULL;
	const gchar *nick;

	if ((NULL == spec) ||
	    !venture_entity_get_field(record, venture_field_spec_get_name(spec),
	                              &value))
		return;

	if (G_VALUE_HOLDS_ENUM(&value))
		nick = venture_enum_to_nick(G_VALUE_TYPE(&value),
		                            g_value_get_enum(&value));
	else if (G_VALUE_HOLDS_STRING(&value))
		nick = g_value_get_string(&value);
	else
		return;

	if (venture_string_is_empty(nick))
		return;

	label = venture_web_choice_label(spec, nick);
	g_string_append_printf(html, "<span class=\"badge status-badge %s\" "
	                       "data-status=\"", venture_web_status_tone(nick));
	venture_html_escape_append(html, nick);
	g_string_append(html, "\">");
	venture_html_escape_append(html, label);
	g_string_append(html, "</span>");
}

/*
 * Where a list of @type lives in the sidebar, and what the sidebar calls
 * it: "Customers" and "Contacts" for a contact. The breadcrumb and the
 * active sidebar row are both read from here, so the page and the
 * navigation cannot name the same place two ways.
 *
 * Returns: (transfer none) (nullable): the matching link, or %NULL
 */
static const VentureWebNavLink *
venture_web_nav_link_for_type(
	const gchar	 *type_name,
	const gchar	**out_section
){
	const VentureWebNavSection *sections;
	const VentureWebNavLink *links;
	g_autofree gchar *path = NULL;
	const gchar *section = NULL;
	gsize i;

	path = g_strdup_printf("/e/%s", type_name);
	links = venture_web_navigation();
	sections = venture_web_navigation_sections();

	if (NULL != out_section)
		*out_section = NULL;

	for (i = 0; NULL != links[i].path; i++)
	{
		gsize j;

		if (NULL != links[i].section)
			section = links[i].section;

		if (0 != g_strcmp0(links[i].path, path))
			continue;

		/* A row a question gathers is filed under the question. */
		for (j = 0; NULL != sections[j].heading; j++)
		{
			if (g_strv_contains(sections[j].paths, path))
			{
				section = sections[j].heading;
				break;
			}
		}

		if (NULL != out_section)
			*out_section = section;

		return &links[i];
	}

	return NULL;
}

/*
 * The trail above a page's title: the sidebar group, then the list.
 * "Customers / Contacts". A type with no sidebar row gets its plural name,
 * linked to its list, so there is always a way back.
 */
static void
venture_web_append_crumbs(
	GString		*html,
	GType		 type,
	const gchar	*type_name,
	gboolean	 link_list
){
	const VentureWebNavLink *link;
	const gchar *section = NULL;
	g_autofree gchar *plural = NULL;

	link = venture_web_nav_link_for_type(type_name, &section);
	plural = (NULL != link) ? g_strdup(link->label)
	                        : venture_web_type_label(type, TRUE);

	g_string_append(html, "<nav class=\"crumbs\" aria-label=\"Breadcrumb\">");

	if (!venture_string_is_empty(section))
	{
		g_string_append(html, "<span class=\"crumb\">");
		venture_html_escape_append(html, section);
		g_string_append(html, "</span>");
	}

	if (link_list)
	{
		g_string_append_printf(html, "<a class=\"crumb\" href=\"/e/%s\">",
		                       type_name);
		venture_html_escape_append(html, plural);
		g_string_append(html, "</a>");
	}
	else
	{
		g_string_append(html, "<span class=\"crumb\">");
		venture_html_escape_append(html, plural);
		g_string_append(html, "</span>");
	}

	g_string_append(html, "</nav>");
}

/*
 * Whether long-text @spec holds something long enough to be read as
 * prose. "30 days" in a Terms field is a fact, not an essay: it goes in
 * Details with the other short values, and only a paragraph -- or
 * anything with a line break -- gets a card of its own.
 */
static gboolean
venture_web_is_prose(
	VentureEntity		*record,
	VentureFieldSpec	*spec
){
	g_auto(GValue) value = G_VALUE_INIT;
	const gchar *text;

	if ((VENTURE_FIELD_ROLE_CONTENT != venture_field_spec_get_role(spec)) ||
	    !venture_entity_get_field(record, venture_field_spec_get_name(spec),
	                              &value) ||
	    !G_VALUE_HOLDS_STRING(&value))
		return FALSE;

	text = g_value_get_string(&value);

	return !venture_string_is_empty(text) &&
	       ((g_utf8_strlen(text, -1) > 80) || (NULL != strchr(text, '\n')));
}

/*
 * The content somebody wrote -- a message, notes, a description -- each
 * in a card of its own with room to be read. Only what is set; a record
 * with no notes has no Notes card.
 */
static void
venture_web_append_record_content(
	VentureWebServer	*self,
	GString			*html,
	VentureEntity		*record,
	GPtrArray		*specs
){
	guint i;

	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec;
		g_auto(GValue) value = G_VALUE_INIT;

		spec = g_ptr_array_index(specs, i);

		if (!venture_web_is_prose(record, spec))
			continue;

		if (venture_web_value_is_empty(record, spec) ||
		    !venture_entity_get_field(record,
		                              venture_field_spec_get_name(spec),
		                              &value) ||
		    !G_VALUE_HOLDS_STRING(&value))
			continue;

		g_string_append(html, "<section class=\"card record-content\">"
		                      "<div class=\"card-head\"><h2>");
		venture_html_escape_append(html, venture_field_spec_get_label(spec));
		g_string_append(html, "</h2></div><div class=\"card-body prose\">");
		venture_html_escape_append(html, g_value_get_string(&value));
		g_string_append(html, "</div></section>");
	}

	(void)self;
}

/*
 * Whether @spec is the field the page is already titled with. The title
 * says it in the largest type on the page; Details saying it again is the
 * first row anybody reads, spent on nothing.
 */
static gboolean
venture_web_spec_is_title(
	VentureEntity		*record,
	VentureFieldSpec	*spec,
	const gchar		*title
){
	g_auto(GValue) value = G_VALUE_INIT;

	if ((VENTURE_FIELD_KIND_STRING != venture_field_spec_get_kind(spec)) ||
	    !venture_entity_get_field(record, venture_field_spec_get_name(spec),
	                              &value) ||
	    !G_VALUE_HOLDS_STRING(&value))
		return FALSE;

	return 0 == g_strcmp0(g_value_get_string(&value), title);
}

/*
 * One label/value row of a definition list.
 */
static void
venture_web_append_fact_row(
	VentureWebServer	*self,
	GString			*html,
	VentureEntity		*record,
	VentureFieldSpec	*spec
){
	g_string_append(html, "<div class=\"fact\"><dt>");
	venture_html_escape_append(html, venture_field_spec_get_label(spec));
	g_string_append(html, "</dt><dd>");
	venture_web_append_value(self, html, record, spec);
	g_string_append(html, "</dd></div>");
}

/*
 * The side panel: the facts that are set, in the order the type declared
 * them, and under "All fields" everything else -- empty fields, machinery
 * and the record's own identity -- for the person who needs it.
 */
static void
venture_web_append_record_aside(
	VentureWebServer	*self,
	GString			*html,
	VentureEntity		*record,
	GPtrArray		*specs,
	const gchar		*actions
){
	g_autofree gchar *title = NULL;
	guint shown;
	guint rest;
	guint i;

	title = venture_entity_get_display_name(record);

	g_string_append(html, "<aside class=\"record-aside\">");
	g_string_append(html, (NULL != actions) ? actions : "");
	g_string_append(html, "<section class=\"card record-facts\">"
	                      "<div class=\"card-head\"><h2>Details</h2></div>"
	                      "<div class=\"card-body\"><dl class=\"facts\">");

	shown = 0;
	rest = 0;

	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec;
		VentureFieldRole role;

		spec = g_ptr_array_index(specs, i);
		role = venture_field_spec_get_role(spec);

		/* Short text reads as a fact; prose has its own card. */
		if (((VENTURE_FIELD_ROLE_FACT != role) &&
		     ((VENTURE_FIELD_ROLE_CONTENT != role) ||
		      venture_web_is_prose(record, spec))) ||
		    venture_web_value_is_empty(record, spec) ||
		    venture_web_spec_is_title(record, spec, title) ||
		    (shown >= VENTURE_WEB_SUMMARY_FACTS))
		{
			rest++;
			continue;
		}

		venture_web_append_fact_row(self, html, record, spec);
		shown++;
	}

	g_string_append(html, "</dl>");

	if (0 == shown)
		g_string_append(html, "<p class=\"muted small\">Nothing filled in "
		                      "yet.</p>");

	/*
	 * Every field, set or not, and the machinery. A <details>, so it is
	 * one click with no script and the page stays quiet until somebody
	 * asks. The record's identity goes here and nowhere else.
	 */
	g_string_append_printf(html,
		"<details class=\"all-fields\"><summary>All fields "
		"<span class=\"count\">%u</span></summary><dl class=\"facts\">",
		specs->len + 4);

	for (i = 0; i < specs->len; i++)
		venture_web_append_fact_row(self, html, record,
		                            g_ptr_array_index(specs, i));

	g_string_append_printf(html,
		"<div class=\"fact\"><dt>Record number</dt><dd><samp>%" G_GINT64_FORMAT
		"</samp></dd></div>", venture_entity_get_id(record));
	g_string_append(html, "<div class=\"fact\"><dt>Stable identifier</dt>"
	                      "<dd><samp>");
	venture_html_escape_append(html, venture_entity_get_uuid(record));
	g_string_append(html, "</samp></dd></div>");

	{
		g_autofree gchar *created = NULL;
		g_autofree gchar *updated = NULL;

		created = venture_time_to_display_string(
			venture_entity_get_created_at(record),
			venture_context_get_timezone(self->context));
		updated = venture_time_to_display_string(
			venture_entity_get_updated_at(record),
			venture_context_get_timezone(self->context));

		g_string_append(html, "<div class=\"fact\"><dt>Created</dt><dd>");
		venture_html_escape_append(html, (NULL != created) ? created : "");
		g_string_append(html, "</dd></div><div class=\"fact\"><dt>Last "
		                      "changed</dt><dd>");
		venture_html_escape_append(html, (NULL != updated) ? updated : "");
		g_string_append_printf(html, " <span class=\"muted\">(revision %"
			G_GINT64_FORMAT ")</span></dd></div>",
			venture_entity_get_version(record));
	}

	g_string_append(html, "</dl></details></div></section></aside>");

	(void)rest;
}

/*
 * A record's initials in a tile, tinted by its name, so a page is
 * recognisable at a glance and two records never look the same. The tint
 * is one of the four status hues mixed into the ground -- tokens, not a
 * colour of our own.
 */
static void
venture_web_append_avatar(
	GString		*html,
	const gchar	*name
){
	static const gchar *const tones[] = { "accent", "positive", "warning", "info" };
	gchar initials[8] = { 0 };
	const gchar *p;
	guint n = 0;
	guint hash;

	if (venture_string_is_empty(name))
		return;

	/* The first letter of the first two words, ASCII-safe: a name that
	 * starts with anything else gets its first character whole. */
	for (p = name; ('\0' != *p) && (n < 2); p = g_utf8_next_char(p))
	{
		if (((p == name) || (' ' == *(p - 1))) && g_unichar_isalnum(g_utf8_get_char(p)))
		{
			gchar buffer[7] = { 0 };
			gint length = g_unichar_to_utf8(g_unichar_toupper(g_utf8_get_char(p)), buffer);

			if ((strlen(initials) + length) < sizeof(initials))
				strncat(initials, buffer, (gsize)length);
			n++;
		}
	}

	if (0 == n)
		return;

	hash = g_str_hash(name);
	g_string_append_printf(html, "<span class=\"avatar tone-%s\" aria-hidden=\"true\">",
	                       tones[hash % G_N_ELEMENTS(tones)]);
	venture_html_escape_append(html, initials);
	g_string_append(html, "</span>");
}

/*
 * The top of a record page: where it lives, its name, its status, and the
 * one or two things you do to it.
 */
static void
venture_web_append_record_head(
	GString		*html,
	VentureEntity	*record,
	GPtrArray	*specs,
	const gchar	*type_name,
	const gchar	*actions
){
	g_autofree gchar *title = NULL;
	g_autofree gchar *singular = NULL;

	title = venture_entity_get_display_name(record);
	singular = venture_web_type_label(G_OBJECT_TYPE(record), FALSE);

	g_string_append(html, "<div class=\"page-head record-head\">"
	                      "<div class=\"page-title\">");
	venture_web_append_crumbs(html, G_OBJECT_TYPE(record), type_name, TRUE);
	g_string_append(html, "<div class=\"title-line\">");
	venture_web_append_avatar(html, title);
	g_string_append(html, "<h1>");
	venture_html_escape_append(html, title);
	g_string_append(html, "</h1>");
	venture_web_append_status_badge(html, record,
	                                venture_web_status_spec(specs));
	g_string_append(html, "</div><span class=\"subtitle\">");
	venture_html_escape_append(html, singular);
	g_string_append(html, "</span></div><div class=\"page-actions\">");
	g_string_append(html, actions);
	g_string_append(html, "</div></div>");
}

/*
 * One record type as a choice: posts the registered name, reads as what a
 * person calls it. A select of sixty "attribution_submission"-style names
 * is the schema, not a question.
 */
static void
venture_web_append_type_option(
	VentureWebServer	*self,
	GString			*html,
	const gchar		*type_name
){
	GType type;
	g_autofree gchar *label = NULL;

	type = venture_entity_registry_lookup_any(
		venture_context_get_entity_registry(self->context), type_name);
	label = (G_TYPE_INVALID != type)
		? venture_web_type_label(type, FALSE)
		: venture_web_label_from_name(type_name);

	g_string_append(html, "<option value=\"");
	venture_html_escape_append(html, type_name);
	g_string_append(html, "\">");
	venture_html_escape_append(html, label);
	g_string_append(html, "</option>");
}

/*
 * A row of tabs over @n panels, the first one showing. Radio buttons and
 * CSS, so switching needs no script and every panel is still in the page
 * for a test, a search or a screen reader. One panel is drawn as itself,
 * with no tab row over it; none draws nothing.
 */
static void
venture_web_append_tabs(
	GString		*html,
	const gchar	**labels,
	GString		**panels,
	guint		  n
){
	guint i;

	if (0 == n)
		return;

	if (1 == n)
	{
		g_string_append(html, panels[0]->str);
		return;
	}

	g_string_append(html, "<div class=\"record-tabs\">");

	for (i = 0; i < n; i++)
		g_string_append_printf(html, "<input type=\"radio\" class=\"tab-radio\" "
		                       "name=\"record-tab\" id=\"record-tab-%u\"%s>",
		                       i, (0 == i) ? " checked" : "");

	g_string_append(html, "<div class=\"tab-list\" role=\"tablist\">");

	for (i = 0; i < n; i++)
	{
		g_string_append_printf(html, "<label for=\"record-tab-%u\" role=\"tab\">", i);
		venture_html_escape_append(html, labels[i]);
		g_string_append(html, "</label>");
	}

	g_string_append(html, "</div>");

	for (i = 0; i < n; i++)
		g_string_append_printf(html, "<div class=\"tab-panel\" data-tab=\"%u\" "
		                       "role=\"tabpanel\">%s</div>", i, panels[i]->str);

	g_string_append(html, "</div>");
}

/* --- Lists ----------------------------------------------------------------- */

/*
 * A list's columns: what somebody scans a list for, not the schema's
 * first seven fields. The status first after the name, then the facts in
 * the order the type declared them; never prose, never machinery. A
 * column empty on every row of this page is dropped, because a column of
 * dashes is a question nobody asked.
 *
 * Returns: (transfer container): borrowed specs from @specs, in order
 */
static GPtrArray *
venture_web_list_columns(
	GPtrArray	*specs,
	GPtrArray	*records
){
	GPtrArray *columns;
	GPtrArray *candidates;
	guint i;

	columns = g_ptr_array_new();
	candidates = g_ptr_array_new();

	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec;
		VentureFieldRole role;

		spec = g_ptr_array_index(specs, i);
		role = venture_field_spec_get_role(spec);

		if (!venture_field_spec_get_show_in_list(spec))
			continue;

		if ((VENTURE_FIELD_ROLE_FACT != role) &&
		    (VENTURE_FIELD_ROLE_STATUS != role))
			continue;

		/* A column with something in it on this page. */
		if (NULL != records)
		{
			gboolean any = FALSE;
			guint j;

			for (j = 0; (j < records->len) && !any; j++)
				any = !venture_web_value_is_empty(
					g_ptr_array_index(records, j), spec);

			if ((records->len > 0) && !any)
				continue;
		}

		g_ptr_array_add(candidates, spec);
	}

	/* The first declared fact is the record's name in nearly every
	 * table; the status reads best right after it. */
	for (i = 0; (i < candidates->len) && (columns->len < 1); i++)
	{
		VentureFieldSpec *spec = g_ptr_array_index(candidates, i);

		if (VENTURE_FIELD_ROLE_FACT == venture_field_spec_get_role(spec))
			g_ptr_array_add(columns, spec);
	}

	for (i = 0; i < candidates->len; i++)
	{
		VentureFieldSpec *spec = g_ptr_array_index(candidates, i);

		if (VENTURE_FIELD_ROLE_STATUS == venture_field_spec_get_role(spec))
		{
			g_ptr_array_add(columns, spec);
			break;
		}
	}

	for (i = 0; (i < candidates->len) &&
	            (columns->len < VENTURE_WEB_LIST_COLUMNS); i++)
	{
		VentureFieldSpec *spec = g_ptr_array_index(candidates, i);
		guint k;
		gboolean taken = FALSE;

		for (k = 0; k < columns->len; k++)
			taken = taken || (g_ptr_array_index(columns, k) == spec);

		if (!taken)
			g_ptr_array_add(columns, spec);
	}

	g_ptr_array_unref(candidates);

	return columns;
}

/*
 * Drops the column that is the records' display name, which the list
 * already leads with as the link to each record. A column is the name when
 * it matches the display name on every row of the page.
 *
 * Returns: (transfer none) (nullable): the dropped spec, which the Name
 *   heading sorts by, or %NULL
 */
static VentureFieldSpec *
venture_web_list_drop_title(
	GPtrArray	*columns,
	GPtrArray	*records
){
	guint i;

	if ((NULL == records) || (0 == records->len))
		return NULL;

	for (i = 0; i < columns->len; i++)
	{
		VentureFieldSpec *spec = g_ptr_array_index(columns, i);
		gboolean title = TRUE;
		guint j;

		if (VENTURE_FIELD_KIND_STRING != venture_field_spec_get_kind(spec))
			continue;

		for (j = 0; (j < records->len) && title; j++)
		{
			VentureEntity *record = g_ptr_array_index(records, j);
			g_autofree gchar *name = venture_entity_get_display_name(record);

			title = venture_web_spec_is_title(record, spec, name);
		}

		if (title)
		{
			g_ptr_array_remove_index(columns, i);
			return spec;
		}
	}

	return NULL;
}

/*
 * One list cell. The same reading as the record page, shortened: a
 * status is a badge, a reference is the other record's name, and prose
 * is cut to a line.
 */
static void
venture_web_append_list_cell(
	VentureWebServer	*self,
	GString			*html,
	VentureEntity		*record,
	VentureFieldSpec	*spec
){
	VentureFieldKind kind;

	kind = venture_field_spec_get_kind(spec);

	if (VENTURE_FIELD_ROLE_STATUS == venture_field_spec_get_role(spec))
	{
		g_string_append(html, "<td>");
		venture_web_append_status_badge(html, record, spec);
		g_string_append(html, "</td>");
		return;
	}

	if (venture_web_value_is_empty(record, spec))
	{
		g_string_append(html, (VENTURE_FIELD_KIND_MONEY == kind)
			? "<td class=\"num\"></td>" : "<td></td>");
		return;
	}

	if ((VENTURE_FIELD_KIND_MONEY == kind) ||
	    (VENTURE_FIELD_KIND_INTEGER == kind) ||
	    (VENTURE_FIELD_KIND_DOUBLE == kind))
	{
		g_string_append(html, "<td class=\"num\">");
		venture_web_append_value(self, html, record, spec);
		g_string_append(html, "</td>");
		return;
	}

	/* A list is scanned down a column: the day, never the minute. */
	if ((VENTURE_FIELD_KIND_DATE == kind) ||
	    (VENTURE_FIELD_KIND_DATETIME == kind))
	{
		g_auto(GValue) value = G_VALUE_INIT;
		g_autofree gchar *text = NULL;

		venture_entity_get_field(record, venture_field_spec_get_name(spec),
		                         &value);
		text = venture_time_to_date_string(g_value_get_boxed(&value),
			venture_context_get_timezone(self->context));
		g_string_append(html, "<td><time>");
		venture_html_escape_append(html, (NULL != text) ? text : "");
		g_string_append(html, "</time></td>");
		return;
	}

	if (VENTURE_FIELD_KIND_STRING == kind)
	{
		g_auto(GValue) value = G_VALUE_INIT;
		g_autofree gchar *text = NULL;

		venture_entity_get_field(record, venture_field_spec_get_name(spec),
		                         &value);
		text = venture_truncate(g_value_get_string(&value), 60);
		g_string_append(html, "<td>");
		venture_html_escape_append(html, text);
		g_string_append(html, "</td>");
		return;
	}

	g_string_append(html, "<td>");
	venture_web_append_value(self, html, record, spec);
	g_string_append(html, "</td>");
}

/* --- Forms ----------------------------------------------------------------- */

/*
 * Which group of a form a field belongs in.
 *
 *   0  the essentials: what is required, the status, and the first few
 *      facts -- enough to create a useful record without scrolling;
 *   1  what somebody writes (notes, a description), full width;
 *   2  everything else, under "More details", folded when the form is new
 *      and there are many;
 *   3  machinery, under "Advanced", always folded.
 */
static guint
venture_web_form_group(
	VentureFieldSpec	*spec,
	guint			*facts_seen
){
	switch (venture_field_spec_get_role(spec))
	{
	case VENTURE_FIELD_ROLE_TECHNICAL:
	case VENTURE_FIELD_ROLE_STRUCTURED:
		/* A required machinery field is still required, and a folded
		 * required input cannot be focused when the browser refuses
		 * the submit: it stays in view. */
		return venture_field_spec_get_required(spec) ? 0 : 3;

	case VENTURE_FIELD_ROLE_CONTENT:
		return venture_field_spec_get_required(spec) ? 0 : 1;

	case VENTURE_FIELD_ROLE_STATUS:
		return 0;

	case VENTURE_FIELD_ROLE_FACT:
	default:
		if (venture_field_spec_get_required(spec))
			return 0;

		if (*facts_seen < VENTURE_WEB_FORM_OPEN_FIELDS)
		{
			(*facts_seen)++;
			return 0;
		}

		return 2;
	}
}
