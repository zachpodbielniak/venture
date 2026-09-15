/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>
#include <stdio.h>

struct _VentureBudgetService
{
	GObject parent_instance;
	VentureDatabase *database;
};
G_DEFINE_FINAL_TYPE(VentureBudgetService, venture_budget_service, G_TYPE_OBJECT)

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	if (id == 1)
		VENTURE_BUDGET_SERVICE(object)->database = g_value_get_object(value);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	if (id == 1)
		g_value_set_object(value, VENTURE_BUDGET_SERVICE(object)->database);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
venture_budget_service_class_init(VentureBudgetServiceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->get_property = get_property;
	object_class->set_property = set_property;
	g_object_class_install_property(object_class, 1,
		g_param_spec_object("database", "Database", "Owning repository", VENTURE_TYPE_DATABASE,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
venture_budget_service_init(VentureBudgetService *self)
{
	(void)self;
}

VentureBudgetService *
venture_budget_service_get(VentureDatabase *database)
{
	VentureBudgetService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-budget-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_BUDGET_SERVICE, "database", database, NULL);
		g_object_set_data_full(G_OBJECT(database), "venture-budget-service", self, g_object_unref);
	}
	return self;
}

static gboolean
enabled(GError **error)
{
	if (venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "budget"))
		return TRUE;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "The budgets module is disabled");
	return FALSE;
}

gboolean
venture_budget_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error)
{
	(void)database;
	(void)removal;
	if (!VENTURE_IS_BUDGET(record) && !VENTURE_IS_BUDGET_LINE(record))
		return TRUE;
	return enabled(error);
}

static VentureDateRange *
period_range(const gchar *period, GError **error)
{
	gint year = 0, month = 0;
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) end = NULL;
	if (period == NULL || sscanf(period, "%d-%d", &year, &month) < 1 || year < 1)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"A budget period must be YYYY or YYYY-MM");
		return NULL;
	}
	if (strchr(period, '-') == NULL)
	{
		start = g_date_time_new_utc(year, 1, 1, 0, 0, 0);
		end = g_date_time_new_utc(year + 1, 1, 1, 0, 0, 0);
	}
	else
	{
		if (month < 1 || month > 12)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				"A budget period must be YYYY or YYYY-MM");
			return NULL;
		}
		start = g_date_time_new_utc(year, month, 1, 0, 0, 0);
		end = g_date_time_add_months(start, 1);
	}
	if (start == NULL || end == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Invalid budget period");
		return NULL;
	}
	return venture_date_range_new(start, end);
}

static gboolean
covers(const gchar *line_period, const gchar *budget_period, const gchar *requested)
{
	const gchar *effective = (line_period != NULL && line_period[0] != '\0') ? line_period : budget_period;
	return effective != NULL && (g_str_equal(effective, requested) ||
		(strlen(requested) == 7 && g_str_has_prefix(effective, requested)) ||
		(strlen(effective) == 4 && g_str_has_prefix(requested, effective)));
}

static gint64
actual_for(VentureReportResult *balances, gint64 account_id, VentureAccountKind kind)
{
	g_autofree gchar *wanted = g_strdup_printf("%" G_GINT64_FORMAT, account_id);
	guint i;
	for (i = 0; i < venture_report_result_get_row_count(balances); i++)
	{
		const GValue *id = venture_report_result_get_cell(balances, i, "account_id");
		const GValue *debits;
		const GValue *credits;
		const VentureMoney *debit_money;
		const VentureMoney *credit_money;
		if (id == NULL || g_strcmp0(g_value_get_string(id), wanted) != 0)
			continue;
		debits = venture_report_result_get_cell(balances, i, "debits");
		credits = venture_report_result_get_cell(balances, i, "credits");
		if (debits == NULL || credits == NULL)
			return 0;
		debit_money = g_value_get_boxed(debits);
		credit_money = g_value_get_boxed(credits);
		if (kind == VENTURE_ACCOUNT_KIND_INCOME)
			return credit_money->amount - debit_money->amount;
		if (kind == VENTURE_ACCOUNT_KIND_LIABILITY || kind == VENTURE_ACCOUNT_KIND_EQUITY)
			return credit_money->amount - debit_money->amount;
		return debit_money->amount - credit_money->amount;
	}
	return 0;
}

static gint64
closing_for(VentureReportResult *balances, gint64 account_id)
{
	g_autofree gchar *wanted = g_strdup_printf("%" G_GINT64_FORMAT, account_id);
	guint i;
	for (i = 0; i < venture_report_result_get_row_count(balances); i++)
	{
		const GValue *id = venture_report_result_get_cell(balances, i, "account_id");
		const GValue *closing;
		const VentureMoney *money;
		if (id == NULL || g_strcmp0(g_value_get_string(id), wanted) != 0)
			continue;
		closing = venture_report_result_get_cell(balances, i, "closing");
		if (closing == NULL)
			return 0;
		money = g_value_get_boxed(closing);
		return money != NULL ? money->amount : 0;
	}
	return 0;
}

static GPtrArray *
active_lines(VentureBudgetService *self, gint64 organization_id, const gchar *period,
	const gchar *dimension, GError **error)
{
	g_autoptr(VentureQuery) budgets = venture_query_new(VENTURE_TYPE_BUDGET);
	g_autoptr(GPtrArray) plans = NULL;
	GPtrArray *lines = g_ptr_array_new_with_free_func(g_object_unref);
	guint i;
	venture_query_set_organization(budgets, organization_id);
	venture_query_set_limit(budgets, 0);
	plans = venture_database_find(self->database, budgets, error);
	if (plans == NULL)
	{
		g_ptr_array_unref(lines);
		return NULL;
	}
	for (i = 0; i < plans->len; i++)
	{
		VentureEntity *plan = g_ptr_array_index(plans, i);
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_BUDGET_LINE);
		g_autoptr(GPtrArray) rows = NULL;
		g_autofree gchar *status = NULL;
		g_autofree gchar *budget_period = NULL;
		guint j;
		g_object_get(plan, "status", &status, "period", &budget_period, NULL);
		if (g_strcmp0(status, "active") != 0)
			continue;
		venture_query_set_organization(query, organization_id);
		venture_query_set_limit(query, 0);
		if (!venture_query_add_filter_int(query, "budget-id", VENTURE_FILTER_OP_EQ,
			venture_entity_get_id(plan), error))
		{
			g_ptr_array_unref(lines);
			return NULL;
		}
		rows = venture_database_find(self->database, query, error);
		if (rows == NULL)
		{
			g_ptr_array_unref(lines);
			return NULL;
		}
		for (j = 0; j < rows->len; j++)
		{
			VentureEntity *line = g_ptr_array_index(rows, j);
			g_autofree gchar *line_period = NULL;
			g_autofree gchar *line_dimension = NULL;
			g_object_get(line, "period", &line_period, "dimension", &line_dimension, NULL);
			if (!covers(line_period, budget_period, period))
				continue;
			if (dimension != NULL && dimension[0] != '\0' && g_strcmp0(line_dimension, dimension) != 0)
				continue;
			g_ptr_array_add(lines, g_object_ref(line));
		}
	}
	return lines;
}

static VentureReportResult *
balances_for(VentureBudgetService *self, gint64 organization_id, const gchar *period,
	const gchar *currency, GError **error)
{
	g_autoptr(VentureDateRange) range = period_range(period, error);
	g_autoptr(VentureLedgerBalances) books = NULL;
	if (range == NULL)
		return NULL;
	books = venture_ledger_balances_new(self->database);
	return venture_ledger_balances_query(books, organization_id, currency, range, NULL, FALSE, error);
}

VentureReportResult *
venture_budget_service_vs_actual(VentureBudgetService *self, gint64 organization_id,
	const gchar *period, const gchar *dimension, GError **error)
{
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(VentureReportResult) balances = NULL;
	g_autoptr(VentureDateRange) range = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureMoney) budget_total = NULL;
	g_autoptr(VentureMoney) actual_total = NULL;
	guint i;
	g_return_val_if_fail(VENTURE_IS_BUDGET_SERVICE(self), NULL);
	if (!enabled(error))
		return NULL;
	range = period_range(period, error);
	if (range == NULL)
		return NULL;
	lines = active_lines(self, organization_id, period, dimension, error);
	if (lines == NULL)
		return NULL;
	{
		g_autoptr(VentureEntity) org = venture_database_get(self->database, VENTURE_TYPE_ORGANIZATION, organization_id, NULL);
		g_autofree gchar *currency = NULL;
		if (org != NULL)
			g_object_get(org, "default-currency", &currency, NULL);
		balances = balances_for(self, organization_id, period, currency && currency[0] ? currency : "USD", error);
	}
	if (balances == NULL)
		return NULL;
	result = venture_report_result_new("Budget vs actual", range);
	venture_report_result_add_column(result, "key", "Code", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "name", "Account", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "dimension", "Dimension", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "budget", "Budget", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "actual", "Actual", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "variance", "Variance", VENTURE_REPORT_COLUMN_MONEY);
	{
		g_autoptr(VentureEntity) org = venture_database_get(self->database, VENTURE_TYPE_ORGANIZATION, organization_id, NULL);
		g_autofree gchar *currency = NULL;
		if (org != NULL)
			g_object_get(org, "default-currency", &currency, NULL);
		budget_total = venture_money_new_zero(currency && currency[0] ? currency : "USD");
		actual_total = venture_money_new_zero(currency && currency[0] ? currency : "USD");
	}
	for (i = 0; i < lines->len; i++)
	{
		VentureEntity *line = g_ptr_array_index(lines, i);
		g_autoptr(VentureEntity) account = NULL;
		g_autoptr(VentureMoney) planned = NULL;
		g_autoptr(VentureMoney) actual = NULL;
		g_autoptr(VentureMoney) variance = NULL;
		g_autoptr(VentureMoney) next_budget = NULL;
		g_autoptr(VentureMoney) next_actual = NULL;
		g_autofree gchar *code = NULL;
		g_autofree gchar *name = NULL;
		g_autofree gchar *line_dimension = NULL;
		gint64 account_id = 0;
		gint kind;
		g_object_get(line, "account-id", &account_id, "amount", &planned, "dimension", &line_dimension, NULL);
		account = venture_database_get(self->database, VENTURE_TYPE_ACCOUNT, account_id, error);
		if (account == NULL)
			return NULL;
		g_object_get(account, "code", &code, "name", &name, "kind", &kind, NULL);
		actual = venture_money_new_for_currency(actual_for(balances, account_id, kind),
			planned != NULL ? planned->currency : "USD");
		variance = venture_money_subtract(actual, planned, error);
		if (variance == NULL)
			return NULL;
		next_budget = venture_money_add(budget_total, planned, error);
		next_actual = venture_money_add(actual_total, actual, error);
		if (next_budget == NULL || next_actual == NULL)
			return NULL;
		venture_money_free(budget_total);
		venture_money_free(actual_total);
		budget_total = g_steal_pointer(&next_budget);
		actual_total = g_steal_pointer(&next_actual);
		venture_report_result_begin_row(result);
		venture_report_result_set_text(result, "key", code);
		venture_report_result_set_text(result, "name", name);
		venture_report_result_set_text(result, "dimension", line_dimension);
		venture_report_result_set_money(result, "budget", planned);
		venture_report_result_set_money(result, "actual", actual);
		venture_report_result_set_money(result, "variance", variance);
	}
	venture_report_result_add_metric(result, venture_metric_new_money("budget", "Budget", budget_total));
	venture_report_result_add_metric(result, venture_metric_new_money("actual", "Actual", actual_total));
	return g_steal_pointer(&result);
}

VentureReportResult *
venture_budget_service_cash_forecast(VentureBudgetService *self, gint64 organization_id,
	const gchar *period, GError **error)
{
	g_autoptr(VentureReportResult) vs = NULL;
	g_autoptr(VentureReportResult) balances = NULL;
	g_autoptr(VentureDateRange) range = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(VentureMoney) ar = NULL;
	g_autoptr(VentureMoney) ap = NULL;
	g_autoptr(VentureMoney) budget_in = NULL;
	g_autoptr(VentureMoney) budget_out = NULL;
	g_autoptr(VentureMoney) forecast = NULL;
	gint64 cash_id, ar_id, ap_id;
	guint i;
	g_return_val_if_fail(VENTURE_IS_BUDGET_SERVICE(self), NULL);
	if (!enabled(error))
		return NULL;
	range = period_range(period, error);
	if (range == NULL)
		return NULL;
	{
		g_autoptr(VentureEntity) org = venture_database_get(self->database, VENTURE_TYPE_ORGANIZATION, organization_id, NULL);
		g_autofree gchar *currency = NULL;
		if (org != NULL)
			g_object_get(org, "default-currency", &currency, NULL);
		balances = balances_for(self, organization_id, period, currency && currency[0] ? currency : "USD", error);
	}
	if (balances == NULL)
		return NULL;
	vs = venture_budget_service_vs_actual(self, organization_id, period, NULL, error);
	if (vs == NULL)
		return NULL;
	lines = active_lines(self, organization_id, period, NULL, error);
	if (lines == NULL)
		return NULL;
	cash_id = venture_setup_resolve_account(self->database, organization_id, "cash", "organization", 0, NULL, error);
	ar_id = venture_setup_resolve_account(self->database, organization_id, "receivables", "organization", 0, NULL, error);
	ap_id = venture_setup_resolve_account(self->database, organization_id, "payables", "organization", 0, NULL, error);
	if (cash_id < 0 || ar_id < 0 || ap_id < 0)
		return NULL;
	ar = venture_money_new_for_currency(ar_id > 0 ? closing_for(balances, ar_id) : 0, "USD");
	ap = venture_money_new_for_currency(ap_id > 0 ? -closing_for(balances, ap_id) : 0, "USD");
	budget_in = venture_money_new_zero("USD");
	budget_out = venture_money_new_zero("USD");
	for (i = 0; i < lines->len; i++)
	{
		VentureEntity *line = g_ptr_array_index(lines, i);
		g_autoptr(VentureEntity) account = NULL;
		g_autoptr(VentureMoney) planned = NULL;
		gint64 account_id = 0;
		gint kind;
		gint64 remaining;
		g_object_get(line, "account-id", &account_id, "amount", &planned, NULL);
		account = venture_database_get(self->database, VENTURE_TYPE_ACCOUNT, account_id, error);
		if (account == NULL)
			return NULL;
		g_object_get(account, "kind", &kind, NULL);
		remaining = planned->amount - actual_for(balances, account_id, kind);
		if (kind == VENTURE_ACCOUNT_KIND_INCOME)
		{
			g_autoptr(VentureMoney) part = venture_money_new_for_currency(remaining, planned->currency);
			g_autoptr(VentureMoney) next = venture_money_add(budget_in, part, error);
			if (next == NULL)
				return NULL;
			venture_money_free(budget_in);
			budget_in = g_steal_pointer(&next);
		}
		else if (kind == VENTURE_ACCOUNT_KIND_EXPENSE)
		{
			g_autoptr(VentureMoney) part = venture_money_new_for_currency(remaining, planned->currency);
			g_autoptr(VentureMoney) next = venture_money_add(budget_out, part, error);
			if (next == NULL)
				return NULL;
			venture_money_free(budget_out);
			budget_out = g_steal_pointer(&next);
		}
	}
	forecast = venture_money_add(ar, budget_in, error);
	if (forecast == NULL)
		return NULL;
	{
		VentureMoney *tmp = venture_money_subtract(forecast, ap, error);
		if (tmp == NULL)
			return NULL;
		venture_money_free(forecast);
		forecast = tmp;
		tmp = venture_money_subtract(forecast, budget_out, error);
		if (tmp == NULL)
			return NULL;
		venture_money_free(forecast);
		forecast = tmp;
	}
	result = venture_report_result_new("Cash forecast", range);
	venture_report_result_add_column(result, "key", "Item", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "amount", "Amount", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_begin_row(result);
	venture_report_result_set_text(result, "key", "Unpaid receivables");
	venture_report_result_set_money(result, "amount", ar);
	venture_report_result_begin_row(result);
	venture_report_result_set_text(result, "key", "Unpaid payables");
	venture_report_result_set_money(result, "amount", ap);
	venture_report_result_begin_row(result);
	venture_report_result_set_text(result, "key", "Remaining budget inflows");
	venture_report_result_set_money(result, "amount", budget_in);
	venture_report_result_begin_row(result);
	venture_report_result_set_text(result, "key", "Remaining budget outflows");
	venture_report_result_set_money(result, "amount", budget_out);
	venture_report_result_add_metric(result, venture_metric_new_money("receivables", "Unpaid AR", ar));
	venture_report_result_add_metric(result, venture_metric_new_money("payables", "Unpaid AP", ap));
	venture_report_result_add_metric(result, venture_metric_new_money("budget_in", "Budget inflows remaining", budget_in));
	venture_report_result_add_metric(result, venture_metric_new_money("budget_out", "Budget outflows remaining", budget_out));
	venture_report_result_add_metric(result, venture_metric_new_money("forecast", "Cash forecast", forecast));
	return g_steal_pointer(&result);
}

static VentureReportResult *
report_vs(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	const gchar *label = period != NULL ? venture_date_range_get_label(period) : NULL;
	gint64 org = options != NULL ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	const gchar *dimension = options != NULL ? venture_json_object_get_string(options, "dimension", NULL) : NULL;
	if (org == 0)
		org = venture_context_get_default_organization_id(context);
	if (label == NULL || label[0] == '\0')
		label = "this_month";
	return venture_budget_service_vs_actual(venture_budget_service_get(venture_context_get_database(context)),
		org, label, dimension, error);
}

static VentureReportResult *
report_forecast(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	const gchar *label = period != NULL ? venture_date_range_get_label(period) : NULL;
	gint64 org = options != NULL ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	if (org == 0)
		org = venture_context_get_default_organization_id(context);
	if (label == NULL || label[0] == '\0')
		label = "this_month";
	return venture_budget_service_cash_forecast(venture_budget_service_get(venture_context_get_database(context)),
		org, label, error);
}

void
venture_budgets_register_reports(VentureReportRegistry *registry)
{
	VentureReport *vs = VENTURE_REPORT(venture_func_report_new("budget_vs_actual", "Budget vs actual",
		"Planned amounts against posted account movement.", report_vs));
	VentureReport *forecast = VENTURE_REPORT(venture_func_report_new("cash_forecast", "Cash forecast",
		"Unpaid AR and AP plus remaining budget cash movements.", report_forecast));
	g_object_set(vs, "financial", TRUE, NULL);
	g_object_set(forecast, "financial", TRUE, NULL);
	venture_report_registry_add(registry, vs);
	venture_report_registry_add(registry, forecast);
}
