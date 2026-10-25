/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Both sources are existing authorities: direct posted assets or envelope left. */
static VentureMoney *
savings_balance(VentureContext *context, VentureEntity *goal, VentureDateRange *range,
	const gchar *month, const gchar *currency, GError **error)
{
	VentureDatabase *db = venture_context_get_database(context);
	gint64 account_id = 0, envelope_id = 0;
	g_autoptr(VentureReportResult) balances = NULL;
	g_autoptr(VentureEntity) account = NULL, envelope = NULL;
	g_autofree gchar *code = NULL;
	guint i;
	g_object_get(goal, "savings-account-id", &account_id, "savings-envelope-id", &envelope_id, NULL);
	if (account_id > 0)
	{
		g_autoptr(VentureLedgerBalances) books = venture_ledger_balances_new(db);
		balances = venture_ledger_balances_query(books, venture_entity_get_organization_id(goal), currency, range, NULL, FALSE, error);
	}
	else
	{
		envelope = venture_database_get(db, VENTURE_TYPE_BUDGET_LINE, envelope_id, error);
		if (envelope == NULL) return NULL;
		g_object_get(envelope, "account-id", &account_id, NULL);
		balances = venture_budget_service_envelopes(venture_budget_service_get(db), venture_entity_get_organization_id(goal), month, error);
	}
	if (balances == NULL) return NULL;
	account = venture_database_get(db, VENTURE_TYPE_ACCOUNT, account_id, error);
	if (account == NULL) return NULL;
	g_object_get(account, "code", &code, NULL);
	for (i = 0; i < venture_report_result_get_row_count(balances); i++)
	{
		const GValue *key = venture_report_result_get_cell(balances, i, "key");
		const GValue *value;
		if (key == NULL || g_strcmp0(g_value_get_string(key), code) != 0) continue;
		value = venture_report_result_get_cell(balances, i, envelope_id > 0 ? "left" : "closing");
		if (value != NULL) return venture_money_copy(g_value_get_boxed(value));
	}
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		"Savings source is unavailable at this month end; check the envelope start month and active budget");
	return NULL;
}

static VentureReportResult *
savings_report(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	VentureDatabase *db = venture_context_get_database(context);
	g_autoptr(JsonObject) defaults = json_object_new();
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_GOAL);
	g_autoptr(GPtrArray) goals = NULL;
	g_autoptr(VentureReportResult) r = venture_report_result_new("Savings goals", period);
	g_autoptr(GDateTime) start = NULL, end = NULL, cutoff = NULL;
	g_autoptr(VentureDateRange) range = NULL;
	g_autofree gchar *month = NULL;
	GDateTime *requested_start = venture_date_range_get_start(period), *requested_end = venture_date_range_get_end(period);
	const gchar *currency;
	gint64 org, goal_id;
	guint i;
	if (options == NULL) options = defaults;
	if (!goals_report_organization(context, options, &org, error)) return NULL;
	goal_id = venture_json_object_get_int(options, "goal_id", 0);
	currency = venture_json_object_get_string(options, "currency", NULL);
	if ((currency != NULL && !venture_currency_is_normalised(currency)) ||
		json_object_has_member(options, "as_of") ||
		g_date_time_get_day_of_month(requested_start) != 1 || g_date_time_get_day_of_month(requested_end) != 1 ||
		(g_date_time_get_year(requested_end) - g_date_time_get_year(requested_start)) * 12 +
		g_date_time_get_month(requested_end) - g_date_time_get_month(requested_start) != 1)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"Savings goals require one calendar month, no as_of override, and an uppercase currency if supplied");
		return NULL;
	}
	month = g_date_time_format(requested_start, "%Y-%m");
	start = g_date_time_new_utc(g_date_time_get_year(requested_start), g_date_time_get_month(requested_start), 1, 0, 0, 0);
	end = g_date_time_add_months(start, 1);
	if (end == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Month exceeds supported calendar");
		return NULL;
	}
	cutoff = g_date_time_add_days(end, -1);
	range = venture_date_range_new(start, end);
	venture_query_set_organization(query, org);
	/* Count only savings in the requested currency, not unrelated goals.
	 * Invalid linked sources still reach savings_check() and refuse. */
	if (!venture_query_add_filter(query, "savings-target", VENTURE_FILTER_OP_NOT_NULL, NULL, error)) return NULL;
	if (currency != NULL && !venture_query_add_money_currency(query, "savings-target", currency, error)) return NULL;
	venture_query_set_limit(query, venture_aggregate_get_max_rows() + 1);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	if (goal_id != 0)
	{
		if (!goals_report_require_goal(db, org, goal_id, error)) return NULL;
		venture_query_add_filter_int(query, "id", VENTURE_FILTER_OP_EQ, goal_id, NULL);
	}
	goals = venture_database_find(db, query, error);
	if (goals == NULL) return NULL;
	if (goals->len > (guint)venture_aggregate_get_max_rows())
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Too many goals; narrow by goal_id");
		return NULL;
	}
	venture_report_result_add_column(r, "key", "Goal ID", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(r, "name", "Goal", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(r, "source", "Source", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(r, "currency", "Currency", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(r, "saved", "Saved / envelope left", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(r, "target", "Target", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(r, "remaining", "Still needed", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(r, "progress", "Progress", VENTURE_REPORT_COLUMN_PERCENT);
	venture_report_result_add_column(r, "monthly_needed", "Monthly amount needed", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(r, "monthly", "Planned monthly contribution", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(r, "forecast", "Estimated target date", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(r, "status", "Forecast status", VENTURE_REPORT_COLUMN_TEXT);
	for (i = 0; i < goals->len; i++)
	{
		VentureEntity *goal = g_ptr_array_index(goals, i);
		g_autoptr(VentureMoney) target = NULL, monthly = NULL, saved = NULL, remaining = NULL, needed = NULL;
		g_autoptr(GDateTime) due = NULL, forecast = NULL;
		g_autofree gchar *key = NULL, *name = NULL, *date = NULL;
		gint64 envelope_id = 0, months = 0;
		gint status = 0;
		const gchar *reason = "Set a monthly contribution";
		g_object_get(goal, "savings-target", &target, "savings-monthly", &monthly, "savings-envelope-id", &envelope_id,
			"due-on", &due, "status", &status, "name", &name, NULL);
		if (!savings_check(db, goal, error)) return NULL;
		saved = savings_balance(context, goal, range, month, target->currency, error);
		if (saved == NULL) return NULL;
		remaining = venture_money_subtract(target, saved, error);
		if (remaining == NULL) return NULL;
		remaining->amount = MAX((gint64)0, remaining->amount);
		if (monthly == NULL) monthly = venture_money_new_zero(target->currency);
		if (due != NULL)
		{
			g_autoptr(GDateTime) last = NULL;
			months = MAX(0, (g_date_time_get_year(due) - g_date_time_get_year(cutoff)) * 12 +
				g_date_time_get_month(due) - g_date_time_get_month(cutoff));
			last = g_date_time_add_months(cutoff, months);
			if (months > 0 && last != NULL && g_date_time_compare(last, due) > 0) months--;
			needed = venture_money_copy(remaining);
			/* Round upward: a rounded-down payment would miss the target. */
			if (months > 0) needed->amount = remaining->amount / months + (remaining->amount % months != 0);
		}
		if (remaining->amount == 0)
		{
			forecast = g_date_time_ref(cutoff);
			reason = "Target covered at cutoff";
		}
		else if (status == VENTURE_GOAL_STATUS_PAUSED || status == VENTURE_GOAL_STATUS_ABANDONED || status == VENTURE_GOAL_STATUS_ACHIEVED)
			reason = "Goal is not active; no contribution forecast";
		else if (monthly->amount > 0)
		{
			gint64 deposits = remaining->amount / monthly->amount + (remaining->amount % monthly->amount != 0);
			if (deposits <= 1200) forecast = g_date_time_add_months(cutoff, (gint)deposits);
			reason = forecast == NULL ? "Beyond the 1200-month forecast horizon" :
				due == NULL ? "At the planned contribution" :
				g_date_time_compare(forecast, due) <= 0 ? "On track" : "After the due date";
		}
		if (remaining->amount > 0 && due != NULL && months == 0)
			reason = "Due before the next contribution; remaining amount needed now";
		key = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(goal));
		venture_report_result_begin_row(r);
		venture_report_result_set_text(r, "key", key);
		venture_report_result_set_text(r, "name", name);
		venture_report_result_set_text(r, "source", envelope_id > 0 ? "Rollover envelope" : "Asset account (direct balance)");
		venture_report_result_set_text(r, "currency", target->currency);
		venture_report_result_set_money(r, "saved", saved);
		venture_report_result_set_money(r, "target", target);
		venture_report_result_set_money(r, "remaining", remaining);
		venture_report_result_set_number(r, "progress", (gdouble)saved->amount / (gdouble)target->amount);
		if (needed != NULL) venture_report_result_set_money(r, "monthly_needed", needed);
		venture_report_result_set_money(r, "monthly", monthly);
		if (forecast != NULL)
		{
			date = g_date_time_format(forecast, "%Y-%m-%d");
			venture_report_result_set_text(r, "forecast", date);
		}
		venture_report_result_set_text(r, "status", reason);
	}
	venture_report_result_append_note(r, "Balances are at the end of the selected UTC calendar month. Asset accounts use direct posted balances in the target currency; envelopes use the existing rollover calculation, not cash held in a bank. Each goal reads its whole source; shared sources can back several goals and are not reserved or summed. Contributions start one month after the cutoff, anchored to its day and clamped in shorter months. Monthly needed rounds up over contributions falling on or before the due date; with none left, the remaining amount is needed now. Forecasts assume the configured net monthly contribution, no further withdrawals, interest, fees or currency changes, and stop at 1200 months. No due date means monthly needed is blank; no contribution means no future forecast. Current settings apply to historical cutoffs; no transfers or goal-status changes are made.");
	return g_steal_pointer(&r);
}

static VentureReportResult *
savings_generate(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	VentureDatabase *db = venture_context_get_database(context);
	g_autoptr(VentureReportResult) result = NULL;
	if (!venture_database_begin(db, error)) return NULL;
	result = savings_report(context, period, options, error);
	if (result == NULL) { venture_database_rollback(db); return NULL; }
	if (!venture_database_commit(db, error)) return NULL;
	return g_steal_pointer(&result);
}
