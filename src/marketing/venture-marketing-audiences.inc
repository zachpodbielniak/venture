/* SPDX-License-Identifier: AGPL-3.0-or-later */
GPtrArray *venture_marketing_service_segment_members(VentureMarketingService *self,
	gint64 organization_id, gint64 list_id, GDateTime *now, GError **error)
{
	g_autoptr(VentureEntity) list = NULL;
	GPtrArray *rows;
	g_return_val_if_fail(VENTURE_IS_MARKETING_SERVICE(self), NULL);
	if (!allowed(self, organization_id, error)) return NULL;
	if (!now) { refuse(error, "A segment needs a clock"); return NULL; }
	list = get_row(self, VENTURE_TYPE_MARKETING_LIST, organization_id, list_id, error);
	if (!list) return NULL;
	if (choice(list, "mode") != 1) { refuse(error, "Only a saved-search segment has members by search"); return NULL; }
	rows = segment_members(self, list, now, error);
	if (rows && rows->len > 10000) { g_ptr_array_unref(rows); refuse(error, "The segment selects more than 10000 records; narrow it"); return NULL; }
	return rows;
}
/* --- A/B tests ------------------------------------------------------------ */

typedef struct { gchar *digest; guint index; } ArmOrder;
static gint compare_order(gconstpointer a, gconstpointer b)
{
	const ArmOrder *left = a, *right = b;
	return strcmp(left->digest, right->digest);
}
gchar **venture_marketing_ab_assign(const gchar *send_uuid, const gchar *const *emails, guint percent)
{
	g_autoptr(GArray) order = g_array_new(FALSE, FALSE, sizeof(ArmOrder));
	gchar **arms;
	guint n = 0, test, i;
	while (emails && emails[n]) n++;
	arms = g_new0(gchar *, n + 1);
	percent = CLAMP(percent, 1, 100);
	/* Rounded, but never fewer than two when there are two: one per arm. */
	test = (n * percent + 50) / 100;
	if (test < 2) test = MIN(2, n);
	for (i = 0; i < n; i++) {
		g_autofree gchar *material = g_strdup_printf("%s\n%s", send_uuid ? send_uuid : "", emails[i]);
		ArmOrder entry;
		entry.digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, material, -1);
		entry.index = i;
		g_array_append_val(order, entry);
	}
	g_array_sort(order, compare_order);
	for (i = 0; i < n; i++) {
		ArmOrder *entry = &g_array_index(order, ArmOrder, i);
		arms[entry->index] = g_strdup(i >= test ? "rest" : (i % 2 == 0 ? "a" : "b"));
		g_free(entry->digest);
	}
	return arms;
}
/* The rate comparison without division: B wins only on strictly more
 * people per recipient; a tie, or nothing measured, is A, the original. */
static gboolean pick_winner(VentureMarketingService *self, VentureEntity *send, GDateTime *now,
	const VentureActor *actor, GError **error)
{
	g_autoptr(GPtrArray) rows = children(self, VENTURE_TYPE_MARKETING_RECIPIENT, venture_entity_get_organization_id(send),
		"send-id", venture_entity_get_id(send), 0, error);
	const gchar *metric = choice(send, "winner-metric") == 1 ? "clicked-at" : "opened-at";
	gint64 people[2] = { 0, 0 }, sent[2] = { 0, 0 };
	const gchar *winner;
	guint i;
	if (!rows) return FALSE;
	for (i = 0; i < rows->len; i++) {
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *arm = string(row, "variant");
		gint side = !g_strcmp0(arm, "a") ? 0 : !g_strcmp0(arm, "b") ? 1 : -1;
		if (side < 0 || !number(row, "mail-id")) continue;
		sent[side]++;
		if (has_stamp(row, metric)) people[side]++;
	}
	winner = people[1] * MAX(sent[0], 1) > people[0] * MAX(sent[1], 1) ? "b" : "a";
	g_object_set(send, "winner", winner, "winner-at", now, NULL);
	if (!save(self, send, actor, error)) return FALSE;
	if (!g_strcmp0(winner, "a")) return TRUE;
	for (i = 0; i < rows->len; i++) {
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *arm = string(row, "variant"), *state = string(row, "state");
		if (g_strcmp0(arm, "rest") || g_strcmp0(state, "eligible")) continue;
		swap_arms(row);
		if (!save(self, row, actor, error)) return FALSE;
	}
	return TRUE;
}
/* Where an A/B send stands before a run queues anything. @hold is set
 * while the rest wait; @arms_only while the test arms are still going. */
static gboolean ab_gate(VentureMarketingService *self, VentureEntity *send, GDateTime *now,
	gboolean *hold, gboolean *arms_only, const VentureActor *actor, GError **error)
{
	g_autofree gchar *winner = string(send, "winner");
	g_autoptr(GDateTime) started = NULL, due = NULL;
	g_autoptr(VentureQuery) query = NULL;
	gint64 waiting, hours = number(send, "winner-wait-hours");
	*hold = FALSE; *arms_only = FALSE;
	if (!ab_test(send) || (winner && *winner)) return TRUE;
	query = venture_query_new(VENTURE_TYPE_MARKETING_RECIPIENT);
	venture_query_set_organization(query, venture_entity_get_organization_id(send));
	venture_query_add_filter_int(query, "send-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(send), NULL);
	venture_query_add_filter_string(query, "state", VENTURE_FILTER_OP_EQ, "eligible", NULL);
	{
		g_autoptr(GPtrArray) arms = g_ptr_array_new_with_free_func(g_free);
		g_ptr_array_add(arms, g_strdup("a")); g_ptr_array_add(arms, g_strdup("b"));
		venture_query_add_filter(query, "variant", VENTURE_FILTER_OP_IN, arms, NULL);
	}
	waiting = venture_database_count(self->database, query, error);
	if (waiting < 0) return FALSE;
	if (waiting > 0) { *arms_only = TRUE; return TRUE; }
	g_object_get(send, "test-sent-at", &started, NULL);
	if (!started) {
		g_object_set(send, "test-sent-at", now, NULL);
		if (!save(self, send, actor, error)) return FALSE;
		started = g_date_time_ref(now);
	}
	due = g_date_time_add_hours(started, (gint)(hours > 0 ? hours : org_default(self, venture_entity_get_organization_id(send),
		"marketing-ab-wait-hours", VENTURE_MARKETING_AB_WAIT_HOURS, 1, 168)));
	if (!due || g_date_time_compare(now, due) < 0) { *hold = TRUE; return TRUE; }
	return pick_winner(self, send, now, actor, error);
}

/* --- Local time ------------------------------------------------------------ */

static GDateTime *local_at(GTimeZone *zone, gint year, gint month, gint day, gint hour)
{
	GDateTime *at = g_date_time_new(zone, year, month, day, hour, 0, 0);
	/* An hour the clocks skip (02:30 on a spring-forward night) does not
	 * exist; GLib may land before the gap. The first moment after it is
	 * the hour the wall clock shows next. */
	if (at && g_date_time_get_hour(at) != hour) {
		gint step;
		for (step = 1; step <= 3 && at && g_date_time_get_hour(at) != hour; step++) {
			GDateTime *later = g_date_time_new(zone, year, month, day, hour + step <= 23 ? hour + step : 23, 0, 0);
			g_date_time_unref(at);
			at = later;
			if (at && g_date_time_get_hour(at) == hour + step) break;
		}
	}
	return at;
}
GDateTime *venture_marketing_local_due(const gchar *zone, gint hour, const gchar *day,
	GDateTime *after, GError **error)
{
	g_autoptr(GTimeZone) tz = NULL;
	g_autoptr(GDateTime) local = NULL, candidate = NULL;
	gint year = 0, month = 0, date = 0;
	if (hour < 0 || hour > 23) { refuse(error, "The local hour is 0 to 23"); return NULL; }
	tz = zone && *zone ? g_time_zone_new_identifier(zone) : NULL;
	if (!tz) { refuse(error, "Not a time zone name, for example Europe/Berlin"); return NULL; }
	if (!after) { refuse(error, "A local time needs a starting moment"); return NULL; }
	if (day && *day) {
		if (sscanf(day, "%d-%d-%d", &year, &month, &date) != 3 || !g_date_valid_dmy(date, month, year)) {
			refuse(error, "A local day is YYYY-MM-DD"); return NULL;
		}
		candidate = local_at(tz, year, month, date, hour);
		if (!candidate) { refuse(error, "That local time is out of range"); return NULL; }
		if (g_date_time_compare(candidate, after) < 0) return g_date_time_to_utc(after);
		return g_date_time_to_utc(candidate);
	}
	local = g_date_time_to_timezone(after, tz);
	if (!local) { refuse(error, "That local time is out of range"); return NULL; }
	candidate = local_at(tz, g_date_time_get_year(local), g_date_time_get_month(local), g_date_time_get_day_of_month(local), hour);
	if (candidate && g_date_time_compare(candidate, after) < 0) {
		g_autoptr(GDateTime) tomorrow = g_date_time_add_days(local, 1);
		g_clear_pointer(&candidate, g_date_time_unref);
		if (tomorrow) candidate = local_at(tz, g_date_time_get_year(tomorrow), g_date_time_get_month(tomorrow), g_date_time_get_day_of_month(tomorrow), hour);
	}
	if (!candidate) { refuse(error, "That local time is out of range"); return NULL; }
	return g_date_time_to_utc(candidate);
}
/* The zone a recipient is reached in: the contact's, else the
 * organization's, else UTC. A name GLib does not know falls through. */
static gchar *recipient_zone(VentureMarketingService *self, VentureEntity *recipient, VentureEntity *organization)
{
	const gchar *candidates[2] = { NULL, NULL };
	g_autofree gchar *mine = NULL, *theirs = NULL;
	guint i;
	if (number(recipient, "contact-id") > 0) {
		g_autoptr(VentureEntity) contact = venture_database_get(self->database, VENTURE_TYPE_CONTACT, number(recipient, "contact-id"), NULL);
		if (contact) mine = string(contact, "timezone");
	}
	if (organization) theirs = string(organization, "timezone");
	candidates[0] = mine; candidates[1] = theirs;
	for (i = 0; i < 2; i++) {
		g_autoptr(GTimeZone) tz = candidates[i] && *candidates[i] ? g_time_zone_new_identifier(candidates[i]) : NULL;
		if (tz) return g_strdup(candidates[i]);
	}
	return g_strdup("UTC");
}
/* At approval: each recipient's due moment. */
static gboolean schedule_local(VentureMarketingService *self, VentureEntity *send, GDateTime *approved,
	const VentureActor *actor, GError **error)
{
	gint64 org = venture_entity_get_organization_id(send);
	g_autoptr(GPtrArray) rows = children(self, VENTURE_TYPE_MARKETING_RECIPIENT, org, "send-id", venture_entity_get_id(send), 0, error);
	g_autoptr(VentureEntity) organization = NULL;
	g_autoptr(GDateTime) day = NULL;
	g_autofree gchar *day_text = NULL;
	guint i;
	if (!rows) return FALSE;
	organization = get_row(self, VENTURE_TYPE_ORGANIZATION, org, org, error);
	if (!organization) return FALSE;
	g_object_get(send, "local-date", &day, NULL);
	if (day) {
		/* A calendar date is stored as midnight UTC on that day. */
		g_autoptr(GDateTime) utc = g_date_time_to_utc(day);
		day_text = g_date_time_format(utc, "%Y-%m-%d");
	}
	for (i = 0; i < rows->len; i++) {
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autofree gchar *state = string(row, "state"), *zone = NULL;
		g_autoptr(GDateTime) due = NULL;
		if (g_strcmp0(state, "eligible")) continue;
		zone = recipient_zone(self, row, organization);
		due = venture_marketing_local_due(zone, (gint)number(send, "local-hour"), day_text, approved, error);
		if (!due) return FALSE;
		g_object_set(row, "due-at", due, NULL);
		if (!save(self, row, actor, error)) return FALSE;
	}
	return TRUE;
}

/* --- Resend to non-openers ------------------------------------------------- */

VentureMarketingSend *venture_marketing_service_resend(VentureMarketingService *self, gint64 organization_id,
	gint64 send_id, gint64 days, const gchar *subject, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) first = NULL;
	g_autoptr(VentureMarketingSend) draft = NULL;
	g_autofree gchar *name = NULL, *title = NULL, *text = NULL, *html = NULL, *label = NULL;
	gint state;
	g_return_val_if_fail(VENTURE_IS_MARKETING_SERVICE(self), NULL);
	if (!allowed(self, organization_id, error)) return NULL;
	if (days < 0 || days > 365) { refuse(error, "Resend after 0 to 365 days"); return NULL; }
	if (subject && (!bounded(subject, 998) || strpbrk(subject, "\r\n"))) { refuse(error, "A subject is one line"); return NULL; }
	first = get_row(self, VENTURE_TYPE_MARKETING_SEND, organization_id, send_id, error);
	if (!first) return NULL;
	state = choice(first, "state");
	if ((state != 2 && state != 3 && state != 5) || !number(first, "last-mail-id")) {
		refuse(error, "Only an approved send that has sent can be resent to its non-openers"); return NULL;
	}
	if (!flag(first, "tracking")) { refuse(error, "Without tracking there are no opens to tell openers from non-openers"); return NULL; }
	g_object_get(first, "name", &name, "subject", &title, "text-body", &text, "html-body", &html, NULL);
	label = g_strdup_printf("Resend: %s", name ? name : "send");
	draft = g_object_new(VENTURE_TYPE_MARKETING_SEND, "organization-id", organization_id, "name", label,
		"list-id", number(first, "list-id"), "subject", subject && *subject ? subject : title, "text-body", text, "html-body", html,
		"tracking", TRUE, "interval-seconds", number(first, "interval-seconds"), "topic-id", number(first, "topic-id"),
		"resend-of-id", send_id, "resend-after-days", days, NULL);
	if (!venture_database_save(self->database, VENTURE_ENTITY(draft), actor, error)) return NULL;
	return g_steal_pointer(&draft);
}
