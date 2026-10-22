/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Included beside the existing calendar helpers: one cadence implementation. */
static GPtrArray *
household_rows(VentureDatabase *db, GType type, gint64 org, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autoptr(GPtrArray) rows = NULL;
	venture_query_set_organization(query, org);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, (guint)venture_aggregate_get_max_rows() + 1);
	rows = venture_database_find(db, query, error);
	if (rows != NULL && rows->len > (guint)venture_aggregate_get_max_rows())
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"Too many records to detect subscriptions completely");
		return NULL;
	}
	return g_steal_pointer(&rows);
}

static gint
charge_date_compare(gconstpointer a, gconstpointer b)
{
	g_autoptr(GDateTime) x = when(*(VentureEntity *const *)a, "date");
	g_autoptr(GDateTime) y = when(*(VentureEntity *const *)b, "date");
	return g_date_time_compare(x, y);
}

static gchar *
merchant_key(const gchar *value)
{
	gchar *key = g_utf8_casefold(value ? value : "", -1);
	return g_strstrip(key);
}

static gboolean
repeat_expand(VentureMoneyCalendarSource *self, VentureContext *context, gint64 org,
	const VentureDateRange *range, GDateTime *today, GPtrArray *events, GError **error)
{
	VentureDatabase *db = venture_context_get_database(context);
	g_autoptr(GPtrArray) rows = household_rows(db, VENTURE_TYPE_BANK_TRANSACTION, org, error);
	g_autoptr(GPtrArray) schedules = NULL;
	g_autoptr(GHashTable) groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
	g_autoptr(GHashTable) tracked = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GList) keys = NULL;
	GList *link;
	guint i;
	(void)self;
	if (rows == NULL) return FALSE;
	if (venture_context_module_enabled(context, "recurring"))
	{
		schedules = household_rows(db, VENTURE_TYPE_RECURRING_SCHEDULE, org, error);
		if (schedules == NULL) return FALSE;
		for (i = 0; i < schedules->len; i++)
		{
			VentureEntity *s = g_ptr_array_index(schedules, i);
			g_autofree gchar *merchant = text(s, "merchant");
			g_autofree gchar *normalized = merchant_key(merchant);
			if (*normalized && number(s, "bank-account-id") > 0)
				g_hash_table_add(tracked, g_strdup_printf("%" G_GINT64_FORMAT "|%s", number(s, "bank-account-id"), normalized));
		}
	}
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_autoptr(VentureMoney) amount = money(row, "amount");
		g_autoptr(GDateTime) at = when(row, "date");
		g_autofree gchar *state = text(row, "state");
		g_autofree gchar *description = text(row, "description");
		g_autofree gchar *merchant = merchant_key(description);
		g_autofree gchar *base = g_strdup_printf("%" G_GINT64_FORMAT "|%s", number(row, "bank-account-id"), merchant);
		g_autofree gchar *key = NULL;
		g_autoptr(VentureEntity) match = NULL, bank = NULL;
		g_autofree gchar *type = NULL, *match_kind = NULL;
		GPtrArray *group;
		if (at == NULL || amount == NULL || !venture_money_is_negative(amount) || *merchant == '\0' ||
			g_strcmp0(state, "excluded") == 0 || g_hash_table_contains(tracked, base) ||
			before_today(today, at)) continue;
		bank = load(db, VENTURE_TYPE_BANK_ACCOUNT, number(row, "bank-account-id"), venture_entity_get_organization_id(row));
		if (bank == NULL) continue;
		match = load(db, VENTURE_TYPE_BANK_MATCH, number(row, "match-id"), venture_entity_get_organization_id(row));
		if (match != NULL) { type = text(match, "record-type"); match_kind = text(match, "kind"); }
		if (g_strcmp0(type, "bank_transfer") == 0 || g_strcmp0(match_kind, "transfer") == 0) continue;
		/* Group by merchant and currency, not amount: a changed price must
		 * invalidate the old estimate instead of predicting both prices. */
		key = g_strdup_printf("%s|%s", base, venture_money_get_currency(amount));
		group = g_hash_table_lookup(groups, key);
		if (group == NULL)
		{
			group = g_ptr_array_new();
			g_hash_table_insert(groups, g_strdup(key), group);
		}
		g_ptr_array_add(group, row);
	}
	keys = g_hash_table_get_keys(groups);
	keys = g_list_sort(keys, (GCompareFunc)g_strcmp0);
	for (link = keys; link != NULL; link = link->next)
	{
		GPtrArray *group = g_hash_table_lookup(groups, link->data);
		VentureEntity *latest;
		g_autoptr(GDateTime) anchor = NULL, next = NULL;
		g_autoptr(VentureMoney) amount = NULL, positive = NULL;
		g_autofree gchar *description = NULL, *title = NULL;
		gint frequencies[] = { FREQUENCY_MONTHLY, FREQUENCY_WEEKLY, FREQUENCY_YEARLY };
		gint frequency = -1;
		guint f, n, offset;
		if (group->len < 3) continue;
		g_ptr_array_sort(group, charge_date_compare);
		latest = g_ptr_array_index(group, group->len - 1);
		amount = money(latest, "amount");
		for (f = 0; f < G_N_ELEMENTS(frequencies); f++)
		{
			gboolean matches = TRUE;
			gint month_day = 1;
			g_autoptr(GDateTime) first_date = when(g_ptr_array_index(group, group->len - 3), "date");
			g_clear_pointer(&anchor, g_date_time_unref);
			/* Infer the billing day from observed dates, then clamp each
			 * month independently (Jan 31, Feb 28, Mar 31). */
			for (n = 0; n < 3; n++)
			{
				g_autoptr(GDateTime) at = when(g_ptr_array_index(group, group->len - 3 + n), "date");
				month_day = MAX(month_day, g_date_time_get_day_of_month(at));
			}
			if (frequencies[f] == FREQUENCY_MONTHLY && month_day > g_date_time_get_day_of_month(first_date))
			{
				/* Use a January anchor in the same year, which can represent
				 * every possible billing day even if the first charge is February. */
				anchor = g_date_time_new(g_date_time_get_timezone(first_date), g_date_time_get_year(first_date), 1, month_day, 0, 0, 0);
			}
			else anchor = g_date_time_ref(first_date);
			for (n = 0; n < 3; n++)
			{
				VentureEntity *prior = g_ptr_array_index(group, group->len - 3 + n);
				g_autoptr(GDateTime) at = when(prior, "date");
				gint initial_offset = frequencies[f] == FREQUENCY_MONTHLY ? g_date_time_get_month(first_date) - g_date_time_get_month(anchor) : 0;
				g_autoptr(GDateTime) expected = cycle_date(anchor, frequencies[f], initial_offset + n);
				g_autoptr(VentureMoney) paid = money(prior, "amount");
				g_autofree gchar *actual_day = day_text(at), *expected_day = expected ? day_text(expected) : NULL;
				if (!venture_money_equal(amount, paid) || g_strcmp0(actual_day, expected_day)) matches = FALSE;
			}
			if (matches) { frequency = frequencies[f]; break; }
		}
		if (frequency < 0) continue;
		{
			g_autoptr(GDateTime) first_date = when(g_ptr_array_index(group, group->len - 3), "date");
			offset = 3 + (frequency == FREQUENCY_MONTHLY ? g_date_time_get_month(first_date) - g_date_time_get_month(anchor) : 0);
		}
		next = cycle_date(anchor, frequency, offset);
		if (next == NULL || before_today(next, today)) continue;
		positive = venture_money_abs(amount);
		description = text(latest, "description");
		title = g_strdup_printf("Possible repeat charge: %s", description);
		for (n = 1; n <= 5000; n++)
		{
			g_autoptr(GDateTime) at = cycle_date(anchor, frequency, offset + n - 1);
			VentureMoneyCalendarEvent *event;
			if (at == NULL || (venture_date_range_get_end(range) != NULL && g_date_time_compare(at, venture_date_range_get_end(range)) >= 0)) break;
			if (!in_range(at, range)) continue;
			event = venture_money_calendar_event_new("repeat_charge", at, title, positive, "out", "bank_transaction", venture_entity_get_id(latest));
			g_object_set(event, "counts-in-net", FALSE, NULL);
			g_ptr_array_add(events, event);
		}
	}
	return TRUE;
}

DEFINE_SOURCE(RepeatSource, repeat_source, "repeat_charge", "banking", repeat_expand)

VentureReportResult *
venture_report_household_subscriptions(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	g_autoptr(GDateTime) start = NULL, end = NULL;
	g_autoptr(VentureDateRange) year = NULL;
	g_autoptr(GPtrArray) events = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GHashTable) totals = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)venture_money_free);
	g_autoptr(GHashTable) first = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	g_autoptr(GList) keys = NULL;
	GList *link;
	gint64 org = options ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	guint i;
	if (org <= 0) org = venture_context_get_default_organization_id(context);
	start = venture_date_range_get_start(period) ? g_date_time_ref(venture_date_range_get_start(period)) : venture_time_now();
	end = g_date_time_add_years(start, 1);
	if (end == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "The subscription forecast must fit in the calendar range");
		return NULL;
	}
	year = venture_date_range_new(start, end);
	events = g_ptr_array_new_with_free_func(g_object_unref);
	if (!venture_context_module_enabled(context, "money_calendar"))
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Money calendar is disabled");
		return NULL;
	}
	if (venture_context_module_enabled(context, "recurring") && !recurring_expand_full(NULL, context, org, year, start, events, TRUE, error)) return NULL;
	if (venture_context_module_enabled(context, "banking") && !repeat_expand(NULL, context, org, year, start, events, error)) return NULL;
	result = venture_report_result_new("Subscriptions you pay for", year);
	venture_report_result_add_column(result, "name", "Subscription", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "status", "Status", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "next_due", "Next due", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "amount", "Each payment", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "yearly_cost", "Next 12 months", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "record", "Record", VENTURE_REPORT_COLUMN_TEXT);
	for (i = 0; i < events->len; i++)
	{
		VentureMoneyCalendarEvent *e = g_ptr_array_index(events, i);
		const gchar *kind = venture_money_calendar_event_get_kind(e);
		g_autofree gchar *key = g_strdup_printf("%s:%" G_GINT64_FORMAT, venture_money_calendar_event_get_record_type(e), venture_money_calendar_event_get_record_id(e));
		VentureMoney *total;
		VentureMoney *sum;
		if (g_strcmp0(kind, "recurring") == 0)
		{
			g_autoptr(VentureEntity) s = load(venture_context_get_database(context), VENTURE_TYPE_RECURRING_SCHEDULE,
				venture_money_calendar_event_get_record_id(e), org);
			if (s == NULL || !flag(s, "subscription")) continue;
		}
		else if (g_strcmp0(kind, "repeat_charge")) continue;
		if (g_strcmp0(venture_money_calendar_event_get_direction(e), "out")) continue;
		total = g_hash_table_lookup(totals, key);
		sum = total ? venture_money_add(total, venture_money_calendar_event_get_amount(e), error)
			: venture_money_copy(venture_money_calendar_event_get_amount(e));
		if (sum == NULL) return NULL;
		g_hash_table_replace(totals, g_strdup(key), sum);
		if (!g_hash_table_contains(first, key)) g_hash_table_insert(first, g_strdup(key), e);
	}
	keys = g_hash_table_get_keys(first);
	keys = g_list_sort(keys, (GCompareFunc)g_strcmp0);
	for (link = keys; link != NULL; link = link->next)
	{
		VentureMoneyCalendarEvent *e = g_hash_table_lookup(first, link->data);
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "name", venture_money_calendar_event_get_title(e));
		venture_report_result_set_text(result, "status", g_strcmp0(venture_money_calendar_event_get_kind(e), "repeat_charge") == 0 ? "Possible subscription" : "Confirmed");
		venture_report_result_set_text(result, "next_due", venture_money_calendar_event_get_day(e));
		venture_report_result_set_money(result, "amount", venture_money_calendar_event_get_amount(e));
		venture_report_result_set_money(result, "yearly_cost", g_hash_table_lookup(totals, link->data));
		venture_report_result_set_text(result, "record", link->data);
	}
	venture_report_result_set_note(result, "Estimated remaining payments over 12 months from the period start, separately in each currency. Possible subscriptions require three equal weekly, monthly or yearly charges; they are not bills or spending. Confirm in a recurring schedule, or pause/end a confirmed schedule to stop forecasts.");
	return g_steal_pointer(&result);
}

/* Lazy, opt-in inbox reminders. Notify and stamp share one transaction. */
gint
venture_money_calendar_remind(VentureContext *context, gint64 org, GDateTime *now, GError **error)
{
	VentureDatabase *db = venture_context_get_database(context);
	g_autoptr(GDateTime) clock = now ? g_date_time_ref(now) : venture_time_now();
	g_autoptr(GDateTime) end = g_date_time_add_days(clock, 29);
	g_autoptr(VentureDateRange) range = venture_date_range_new(clock, end);
	g_autoptr(GPtrArray) events = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	guint i;
	gint sent = 0;
	if (!venture_context_module_enabled(context, "money_calendar") || !venture_context_module_enabled(context, "recurring")) return 0;
	if (g_object_get_data(G_OBJECT(db), "household-reminder-writing") != NULL) return 0;
	/* All due dates come from the same source as the page, report and feed. */
	if (!recurring_expand(NULL, context, org, range, clock, events, error)) return -1;
	for (i = 0; i < events->len; i++)
	{
		VentureMoneyCalendarEvent *e = g_ptr_array_index(events, i);
		gint64 id = venture_money_calendar_event_get_record_id(e), user, lead;
		g_autoptr(VentureEntity) s = venture_database_get(db, VENTURE_TYPE_RECURRING_SCHEDULE, id, error);
		g_autoptr(GDateTime) told = NULL, threshold = NULL;
		g_autofree gchar *told_day = NULL, *threshold_day = NULL;
		g_autofree gchar *amount = NULL, *body = NULL;
		gint64 *key;
		g_autoptr(VentureAccessScope) internal = NULL;
		const gchar *due = venture_money_calendar_event_get_day(e);
		if (s == NULL) return -1;
		if (g_hash_table_contains(seen, &id)) continue;
		key = g_new(gint64, 1); *key = id; g_hash_table_add(seen, key);
		lead = number(s, "reminder-days"); user = number(s, "reminder-user-id");
		if (lead <= 0 || user <= 0 || g_strcmp0(venture_money_calendar_event_get_direction(e), "out")) continue;
		threshold = g_date_time_add_days(clock, (gint)lead);
		threshold_day = day_text(threshold);
		if (g_strcmp0(due, threshold_day) > 0 || !venture_comment_user_can_read(context, user, s)) continue;
		told = when(s, "reminder-date");
		if (told != NULL) told_day = day_text(told);
		if (told_day != NULL && g_strcmp0(told_day, due) >= 0) continue;
		amount = venture_money_to_string(venture_money_calendar_event_get_amount(e));
		body = g_strdup_printf("Due %s: %s. This reminder does not record a payment.", due, amount);
		internal = venture_access_policy_enter(venture_database_get_access_policy(db), NULL);
		if (!venture_database_begin(db, error)) return -1;
		g_object_set_data(G_OBJECT(db), "household-reminder-writing", s);
		g_object_set(s, "reminder-date", venture_money_calendar_event_get_date(e), NULL);
		if (!venture_notify_send(context, user, VENTURE_NOTIFICATION_KIND_REMINDER,
			venture_money_calendar_event_get_title(e), body, "recurring_schedule", id,
			venture_money_calendar_event_get_title(e), NULL, error) ||
			!venture_database_save(db, s, NULL, error) || !venture_database_commit(db, error))
		{
			venture_database_rollback(db);
			g_object_set_data(G_OBJECT(db), "household-reminder-writing", NULL);
			return -1;
		}
		g_object_set_data(G_OBJECT(db), "household-reminder-writing", NULL);
		sent++;
	}
	return sent;
}
