/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>

#include <vk-kernels.h>

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

static gboolean envelope_check(VentureDatabase *database, VentureEntity *record, GError **error);

gboolean
venture_budget_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error)
{
	if (!VENTURE_IS_BUDGET(record) && !VENTURE_IS_BUDGET_LINE(record))
		return TRUE;
	if (!enabled(error)) return FALSE;
	return removal || !VENTURE_IS_BUDGET_LINE(record) || envelope_check(database, record, error);
}

/* The UTC range of a budget period; reading "YYYY" or "YYYY-MM" is
 * vk_budget_period. */
static VentureDateRange *
period_range(const gchar *period, GError **error)
{
	g_autoptr(GDateTime) start = NULL;
	g_autoptr(GDateTime) end = NULL;
	g_autoptr(VkBudgetPeriod) read = vk_budget_period(period != NULL ? period : "");
	switch (read->tag)
	{
	case VK_BUDGET_PERIOD_YEAR:
		start = g_date_time_new_utc((gint)read->u.year.year, 1, 1, 0, 0, 0);
		end = g_date_time_new_utc((gint)read->u.year.year + 1, 1, 1, 0, 0, 0);
		break;
	case VK_BUDGET_PERIOD_MONTH:
		start = g_date_time_new_utc((gint)read->u.month.year, (gint)read->u.month.month, 1, 0, 0, 0);
		end = start != NULL ? g_date_time_add_months(start, 1) : NULL;
		break;
	case VK_BUDGET_PERIOD_INVALID:
	default:
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"A budget period must be YYYY or YYYY-MM");
		return NULL;
	}
	if (start == NULL || end == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT, "Invalid budget period");
		return NULL;
	}
	return venture_date_range_new(start, end);
}

/* Whether a line (its own period, else its budget's) covers @requested
 * (vk_budget_covers). */
static gboolean
covers(const gchar *line_period, const gchar *budget_period, const gchar *requested)
{
	return vk_budget_covers(line_period, budget_period, requested);
}

/* The kernel's name for an account kind; an unknown kind reads as the
 * debit-normal default the sign rule always gave it. */
static VkLedgerKind
ledger_kind(VentureAccountKind kind)
{
	switch (kind)
	{
	case VENTURE_ACCOUNT_KIND_ASSET:
		return VK_LEDGER_KIND_ASSET;
	case VENTURE_ACCOUNT_KIND_LIABILITY:
		return VK_LEDGER_KIND_LIABILITY;
	case VENTURE_ACCOUNT_KIND_EQUITY:
		return VK_LEDGER_KIND_EQUITY;
	case VENTURE_ACCOUNT_KIND_INCOME:
		return VK_LEDGER_KIND_INCOME;
	case VENTURE_ACCOUNT_KIND_EXPENSE:
	default:
		return VK_LEDGER_KIND_EXPENSE;
	}
}

/*
 * What @account_id did in @balances, on its normal side, as money in
 * @currency: debits less credits for an asset or expense, credits less
 * debits otherwise (vk_ledger_normal_balance, checked and at the two
 * amounts' common exponent). Zero when the account has no row. It used
 * to subtract the raw amounts unchecked -- a wrapped gint64 at the
 * extremes, and minor units read at whatever exponent @currency has --
 * and to relabel the result with @currency whatever currency the balances
 * were in; another currency is now refused.
 */
static VentureMoney *
actual_for(VentureReportResult *balances, gint64 account_id, VentureAccountKind kind,
	const gchar *currency, GError **error)
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
		TlwOptI64 actual;
		if (id == NULL || g_strcmp0(g_value_get_string(id), wanted) != 0)
			continue;
		debits = venture_report_result_get_cell(balances, i, "debits");
		credits = venture_report_result_get_cell(balances, i, "credits");
		if (debits == NULL || credits == NULL)
			break;
		debit_money = g_value_get_boxed(debits);
		credit_money = g_value_get_boxed(credits);
		if (g_ascii_strcasecmp(debit_money->currency, currency) != 0 ||
			g_ascii_strcasecmp(credit_money->currency, currency) != 0)
		{
			g_set_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				"The ledger's balances for account #%" G_GINT64_FORMAT " are in %s, not %s: "
				"a budget compares one currency", account_id, debit_money->currency, currency);
			return NULL;
		}
		actual = vk_ledger_normal_balance(ledger_kind(kind), debit_money->amount, (gint64)debit_money->exponent,
			credit_money->amount, (gint64)credit_money->exponent);
		if (!actual.has)
		{
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
				"Monetary subtraction overflows a 64-bit minor-unit amount");
			return NULL;
		}
		return venture_money_new(actual.val, currency,
			MAX(debit_money->exponent, credit_money->exponent));
	}
	return venture_money_new_for_currency(0, currency);
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
			gboolean envelope = FALSE;
			g_autofree gchar *line_period = NULL;
			g_autofree gchar *line_dimension = NULL;
			g_object_get(line, "period", &line_period, "dimension", &line_dimension, "envelope", &envelope, NULL);
			if (envelope) continue;
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
	const gchar *currency, const gchar *dimension, GError **error)
{
	g_autoptr(VentureDateRange) range = period_range(period, error);
	g_autoptr(VentureLedgerBalances) books = NULL;
	if (range == NULL)
		return NULL;
	books = venture_ledger_balances_new(self->database);
	venture_ledger_balances_set_dimension(books, dimension);
	return venture_ledger_balances_query(books, organization_id, currency, range, NULL, FALSE, error);
}

#include "venture-budget-envelopes.inc"

VentureReportResult *
venture_budget_service_vs_actual(VentureBudgetService *self, gint64 organization_id,
	const gchar *period, const gchar *dimension, GError **error)
{
	g_autoptr(GPtrArray) lines = NULL;
	g_autoptr(VentureDateRange) range = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureMoney) budget_total = NULL;
	g_autoptr(VentureMoney) actual_total = NULL;
	g_autoptr(GHashTable) cached = NULL;
	g_autofree gchar *book_currency = NULL;
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
	/* The organization's book currency, or the install's when it names
	 * none -- never "USD", which anchored a euro install's budget on
	 * dollars and then refused to add its first euro line. */
	book_currency = venture_database_get_book_currency(self->database, organization_id);
	cached = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
	result = venture_report_result_new("Budget vs actual", range);
	venture_report_result_add_column(result, "key", "Code", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "name", "Account", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "dimension", "Dimension", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "budget", "Budget", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "actual", "Actual", VENTURE_REPORT_COLUMN_MONEY);
	venture_report_result_add_column(result, "variance", "Variance", VENTURE_REPORT_COLUMN_MONEY);
	budget_total = venture_money_new_zero(book_currency);
	actual_total = venture_money_new_zero(book_currency);
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
		VentureReportResult *balances;
		const gchar *scope;
		gint64 account_id = 0;
		gint kind;
		g_object_get(line, "account-id", &account_id, "amount", &planned, "dimension", &line_dimension, NULL);
		account = venture_database_get(self->database, VENTURE_TYPE_ACCOUNT, account_id, error);
		if (account == NULL)
			return NULL;
		g_object_get(account, "code", &code, "name", &name, "kind", &kind, NULL);
		scope = (line_dimension != NULL && line_dimension[0] != '\0') ? line_dimension :
			((dimension != NULL && dimension[0] != '\0') ? dimension : "");
		balances = g_hash_table_lookup(cached, scope);
		if (balances == NULL)
		{
			balances = balances_for(self, organization_id, period, book_currency,
				scope[0] ? scope : NULL, error);
			if (balances == NULL)
				return NULL;
			g_hash_table_insert(cached, g_strdup(scope), balances);
		}
		actual = actual_for(balances, account_id, kind,
			planned != NULL ? planned->currency : book_currency, error);
		if (actual == NULL)
			return NULL;
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
		g_autofree gchar *currency = venture_database_get_book_currency(self->database, organization_id);
		balances = balances_for(self, organization_id, period, currency, NULL, error);
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
	{
		g_autofree gchar *currency = venture_database_get_book_currency(self->database, organization_id);
		ar = venture_money_new_for_currency(ar_id > 0 ? closing_for(balances, ar_id) : 0, currency);
		ap = venture_money_new_for_currency(ap_id > 0 ? -closing_for(balances, ap_id) : 0, currency);
		budget_in = venture_money_new_zero(currency);
		budget_out = venture_money_new_zero(currency);
	}
	for (i = 0; i < lines->len; i++)
	{
		VentureEntity *line = g_ptr_array_index(lines, i);
		g_autoptr(VentureEntity) account = NULL;
		g_autoptr(VentureMoney) planned = NULL;
		g_autoptr(VentureMoney) actual = NULL;
		g_autoptr(VentureMoney) part = NULL;
		gint64 account_id = 0;
		gint kind;
		g_object_get(line, "account-id", &account_id, "amount", &planned, NULL);
		account = venture_database_get(self->database, VENTURE_TYPE_ACCOUNT, account_id, error);
		if (account == NULL)
			return NULL;
		g_object_get(account, "kind", &kind, NULL);
		/* What is left of the plan: checked and exponent-aware, where it
		 * was a raw gint64 subtraction. */
		actual = actual_for(balances, account_id, kind, planned->currency, error);
		if (actual == NULL)
			return NULL;
		part = venture_money_subtract(planned, actual, error);
		if (part == NULL)
			return NULL;
		if (kind == VENTURE_ACCOUNT_KIND_INCOME)
		{
			g_autoptr(VentureMoney) next = venture_money_add(budget_in, part, error);
			if (next == NULL)
				return NULL;
			venture_money_free(budget_in);
			budget_in = g_steal_pointer(&next);
		}
		else if (kind == VENTURE_ACCOUNT_KIND_EXPENSE)
		{
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

static gchar *
report_period(VentureDateRange *period)
{
	g_autoptr(GDateTime) now = period == NULL ? venture_time_now() : NULL;
	GDateTime *start = period != NULL ? venture_date_range_get_start(period) : now;
	g_autoptr(GDateTime) next_year = g_date_time_add_years(start, 1);
	gboolean annual = period != NULL && g_date_time_get_month(start) == 1 &&
		g_date_time_get_day_of_month(start) == 1 && next_year != NULL &&
		g_date_time_compare(next_year, venture_date_range_get_end(period)) == 0;
	/* Labels may say "this_month"; calendar bounds are the reporting contract. */
	return g_date_time_format(start, annual ? "%Y" : "%Y-%m");
}

static VentureReportResult *
report_vs(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	g_autofree gchar *label = report_period(period);
	gint64 org = options != NULL ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	const gchar *dimension = options != NULL ? venture_json_object_get_string(options, "dimension", NULL) : NULL;
	if (org == 0)
		org = venture_context_get_default_organization_id(context);
	return venture_budget_service_vs_actual(venture_budget_service_get(venture_context_get_database(context)),
		org, label, dimension, error);
}

static VentureReportResult *
report_forecast(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	g_autofree gchar *label = report_period(period);
	gint64 org = options != NULL ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	if (org == 0)
		org = venture_context_get_default_organization_id(context);
	return venture_budget_service_cash_forecast(venture_budget_service_get(venture_context_get_database(context)),
		org, label, error);
}

static VentureReportResult *
report_envelopes(VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	g_autofree gchar *label = report_period(period);
	gint64 org = options != NULL ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	if (org == 0) org = venture_context_get_default_organization_id(context);
	return venture_budget_service_envelopes(venture_budget_service_get(venture_context_get_database(context)),
		org, label, error);
}

void
venture_budgets_register_reports(VentureReportRegistry *registry)
{
	VentureReport *vs = VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT, "budget_vs_actual", "Budget vs actual",
		"Planned amounts against posted account movement.", report_vs));
	VentureReport *forecast = VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT, "cash_forecast", "Cash forecast",
		"Unpaid AR and AP plus remaining budget cash movements.", report_forecast));
	VentureReport *envelopes = VENTURE_REPORT(venture_func_report_new_classified(VENTURE_DATA_CLASS_TENANT, "budget_envelopes", "Monthly envelopes",
		"Monthly category budgets, rollover, left to spend and early warnings.", report_envelopes));
	g_object_set(vs, "financial", TRUE, NULL);
	g_object_set(forecast, "financial", TRUE, NULL);
	venture_report_registry_add(registry, vs);
	venture_report_registry_add(registry, forecast);
	g_object_set(envelopes, "financial", TRUE, NULL);
	venture_report_registry_add(registry, envelopes);
}
