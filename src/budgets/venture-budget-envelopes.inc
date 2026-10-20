/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Shared by generic writes and reports: an envelope has one expense account,
 * so cash transfers cannot consume it and overlapping plans cannot count twice. */
static gboolean
envelope_check(VentureDatabase *database, VentureEntity *record, GError **error)
{
	gboolean envelope = FALSE, rollover = FALSE;
	gint64 account_id = 0, budget_id = 0;
	gint kind = 0;
	g_autofree gchar *period = NULL, *dimension = NULL, *currency = NULL;
	g_autoptr(VentureMoney) amount = NULL;
	g_autoptr(VentureDateRange) range = NULL;
	g_autoptr(VentureEntity) account = NULL, plan = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	g_object_get(record, "envelope", &envelope, "rollover", &rollover,
		"account-id", &account_id, "budget-id", &budget_id, "period", &period,
		"dimension", &dimension, "amount", &amount, NULL);
	if (!envelope && !rollover) return TRUE;
	if (!envelope || period == NULL || strlen(period) != 7 ||
		(dimension != NULL && *dimension) || amount == NULL || amount->amount < 0)
		goto invalid;
	range = period_range(period, error);
	if (range == NULL) return FALSE;
	currency = venture_database_get_book_currency(database, venture_entity_get_organization_id(record));
	if (g_strcmp0(amount->currency, currency) != 0) goto invalid;
	account = venture_database_get(database, VENTURE_TYPE_ACCOUNT, account_id, error);
	if (account == NULL) return FALSE;
	plan = venture_database_get(database, VENTURE_TYPE_BUDGET, budget_id, error);
	if (plan == NULL) return FALSE;
	g_object_get(account, "kind", &kind, NULL);
	if (kind != VENTURE_ACCOUNT_KIND_EXPENSE || venture_entity_is_deleted(account) ||
		venture_entity_is_deleted(plan) ||
		venture_entity_get_organization_id(account) != venture_entity_get_organization_id(record) ||
		venture_entity_get_organization_id(plan) != venture_entity_get_organization_id(record))
		goto invalid;
	query = venture_query_new(VENTURE_TYPE_BUDGET_LINE);
	venture_query_set_organization(query, venture_entity_get_organization_id(record));
	venture_query_set_limit(query, 0);
	if (!venture_query_add_filter_int(query, "account-id", VENTURE_FILTER_OP_EQ, account_id, error)) return FALSE;
	rows = venture_database_find(database, query, error);
	if (rows == NULL) return FALSE;
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *other = g_ptr_array_index(rows, i);
		gboolean other_envelope = FALSE;
		g_object_get(other, "envelope", &other_envelope, NULL);
		if (other_envelope && venture_entity_get_id(other) != venture_entity_get_id(record))
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
				"This category already has a monthly envelope; edit that line instead");
			return FALSE;
		}
	}
	return TRUE;
invalid:
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
		"A monthly envelope needs a YYYY-MM start, a nonnegative amount in the book currency, "
		"an expense account and budget in the same organization, and no dimension; rollover needs an envelope");
	return FALSE;
}

VentureReportResult *
venture_budget_service_envelopes(VentureBudgetService *self, gint64 organization_id,
	const gchar *period, GError **error)
{
	g_autoptr(VentureDateRange) range = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(VentureReportResult) result = NULL, current = NULL;
	g_autofree gchar *currency = NULL;
	g_autoptr(VentureMoney) total = NULL;
	guint i;
	g_return_val_if_fail(VENTURE_IS_BUDGET_SERVICE(self), NULL);
	if (!enabled(error)) return NULL;
	if (organization_id <= 0 || period == NULL || strlen(period) != 7)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"Choose one organization and a month in YYYY-MM form");
		return NULL;
	}
	range = period_range(period, error);
	if (range == NULL) return NULL;
	query = venture_query_new(VENTURE_TYPE_BUDGET_LINE);
	venture_query_set_organization(query, organization_id);
	venture_query_set_limit(query, 0);
	lines = venture_database_find(self->database, query, error);
	if (lines == NULL) return NULL;
	currency = venture_database_get_book_currency(self->database, organization_id);
	current = balances_for(self, organization_id, period, currency, NULL, error);
	if (current == NULL) return NULL;
	result = venture_report_result_new("Monthly envelopes", range);
	venture_report_result_add_column(result, "key", "Code", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "name", "Category", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "budget", "Monthly amount", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "carried", "Rolled over", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "available", "Available", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "actual", "Spent", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "left", "Left to spend", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "warning", "Status", VENTURE_REPORT_COLUMN_TEXT);
	total = venture_money_new_zero(currency);
	for (i = 0; i < lines->len; i++)
	{
		VentureEntity *line = g_ptr_array_index(lines, i);
		gboolean envelope = FALSE, rollover = FALSE;
		gint64 account_id = 0, budget_id = 0;
		g_autofree gchar *start_month = NULL, *status = NULL, *code = NULL, *name = NULL;
		g_autoptr(VentureEntity) plan = NULL, account = NULL;
		g_autoptr(VentureDateRange) start_range = NULL;
		g_autoptr(VentureMoney) planned = NULL, carried = NULL, available = NULL, actual = NULL, left = NULL, next = NULL;
		const gchar *warning;
		g_object_get(line, "envelope", &envelope, "rollover", &rollover,
			"account-id", &account_id, "budget-id", &budget_id, "period", &start_month, "amount", &planned, NULL);
		if (!envelope) continue;
		plan = venture_database_get(self->database, VENTURE_TYPE_BUDGET, budget_id, error);
		if (plan == NULL) return NULL;
		g_object_get(plan, "status", &status, NULL);
		if (venture_entity_is_deleted(plan) || g_strcmp0(status, "active") != 0) continue;
		if (!envelope_check(self->database, line, error)) return NULL;
		if (strcmp(start_month, period) > 0) continue;
		start_range = period_range(start_month, error);
		if (start_range == NULL) return NULL;
		carried = venture_money_new_zero(currency);
		if (rollover && strcmp(start_month, period) < 0)
		{
			GDateTime *start = venture_date_range_get_start(start_range);
			GDateTime *end = venture_date_range_get_start(range);
			gint months = (g_date_time_get_year(end) - g_date_time_get_year(start)) * 12 +
				g_date_time_get_month(end) - g_date_time_get_month(start);
			g_autoptr(VentureDateRange) prior_range = venture_date_range_new(start, end);
			g_autoptr(VentureLedgerBalances) books = venture_ledger_balances_new(self->database);
			g_autoptr(VentureReportResult) prior = NULL;
			g_autoptr(VentureMoney) allocated = NULL, spent = NULL;
			prior = venture_ledger_balances_query(books, organization_id, currency, prior_range, NULL, FALSE, error);
			if (prior == NULL) return NULL;
			allocated = venture_money_multiply_int(planned, months, error);
			if (allocated == NULL) return NULL;
			spent = venture_money_new_for_currency(actual_for(prior, account_id, VENTURE_ACCOUNT_KIND_EXPENSE), currency);
			g_clear_pointer(&carried, venture_money_free);
			carried = venture_money_subtract(allocated, spent, error);
			if (carried == NULL) return NULL;
		}
		available = venture_money_add(planned, carried, error);
		if (available == NULL) return NULL;
		actual = venture_money_new_for_currency(actual_for(current, account_id, VENTURE_ACCOUNT_KIND_EXPENSE), currency);
		left = venture_money_subtract(available, actual, error);
		if (left == NULL) return NULL;
		next = venture_money_add(total, left, error);
		if (next == NULL) return NULL;
		g_clear_pointer(&total, venture_money_free);
		total = g_steal_pointer(&next);
		warning = left->amount < 0 ? "Over budget" : left->amount == 0 ? "Used up" :
			(available->amount > 0 && actual->amount >= available->amount - available->amount / 5) ? "Running low" : "On track";
		account = venture_database_get(self->database, VENTURE_TYPE_ACCOUNT, account_id, error);
		if (account == NULL) return NULL;
		g_object_get(account, "code", &code, "name", &name, NULL);
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "key", code);
		venture_report_result_set_text(result, "name", name);
		venture_report_result_set_money(result, "budget", planned);
		venture_report_result_set_money(result, "carried", carried);
		venture_report_result_set_money(result, "available", available);
		venture_report_result_set_money(result, "actual", actual);
		venture_report_result_set_money(result, "left", left);
		venture_report_result_set_text(result, "warning", warning);
	}
	venture_report_result_add_metric(result, venture_metric_new_money("left", "Left to spend", total));
	return g_steal_pointer(&result);
}
