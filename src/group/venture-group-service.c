/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include <string.h>
#include <stdio.h>

struct _VentureGroupService
{
	GObject parent_instance;
	VentureDatabase *database;
};
G_DEFINE_FINAL_TYPE(VentureGroupService, venture_group_service, G_TYPE_OBJECT)

static void
set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
	if (id == 1)
		VENTURE_GROUP_SERVICE(object)->database = g_value_get_object(value);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
	if (id == 1)
		g_value_set_object(value, VENTURE_GROUP_SERVICE(object)->database);
	else
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}

static void
venture_group_service_class_init(VentureGroupServiceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	object_class->get_property = get_property;
	object_class->set_property = set_property;
	g_object_class_install_property(object_class, 1,
		g_param_spec_object("database", "Database", "Owning repository", VENTURE_TYPE_DATABASE,
			G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

static void
venture_group_service_init(VentureGroupService *self)
{
	(void)self;
}

VentureGroupService *
venture_group_service_get(VentureDatabase *database)
{
	VentureGroupService *self;
	g_return_val_if_fail(VENTURE_IS_DATABASE(database), NULL);
	self = g_object_get_data(G_OBJECT(database), "venture-group-service");
	if (self == NULL)
	{
		self = g_object_new(VENTURE_TYPE_GROUP_SERVICE, "database", database, NULL);
		g_object_set_data_full(G_OBJECT(database), "venture-group-service", self, g_object_unref);
	}
	return self;
}

static gboolean
enabled(GError **error)
{
	if (venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "intercompany_link"))
		return TRUE;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED, "The group module is disabled");
	return FALSE;
}

gboolean
venture_group_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error)
{
	(void)database;
	(void)removal;
	if (!VENTURE_IS_INTERCOMPANY_LINK(record) && !VENTURE_IS_ELIMINATION(record))
		return TRUE;
	return enabled(error);
}

VentureEntity *
venture_group_service_eliminate(VentureGroupService *self, gint64 parent_id, gint64 contra_id,
	gint64 debit_account_id, gint64 credit_account_id, const VentureMoney *amount,
	const gchar *period, const gchar *memo, const VentureActor *actor, GError **error)
{
	g_autoptr(VentureEntity) row = NULL;
	g_return_val_if_fail(VENTURE_IS_GROUP_SERVICE(self), NULL);
	if (!enabled(error))
		return NULL;
	if (parent_id <= 0 || debit_account_id <= 0 || credit_account_id <= 0 || amount == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION,
			"An elimination names parent accounts and an amount");
		return NULL;
	}
	row = VENTURE_ENTITY(venture_elimination_new());
	g_object_set(row, "organization-id", parent_id, "contra-organization-id", contra_id,
		"debit-account-id", debit_account_id, "credit-account-id", credit_account_id,
		"amount", amount, "period", period, "memo", memo, NULL);
	if (!venture_database_save(self->database, row, actor, error))
		return NULL;
	return g_steal_pointer(&row);
}

static GPtrArray *
members(VentureGroupService *self, gint64 parent_id, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INTERCOMPANY_LINK);
	g_autoptr(GPtrArray) links = NULL;
	GPtrArray *orgs = g_ptr_array_new();
	guint i;
	g_ptr_array_add(orgs, GSIZE_TO_POINTER((gsize)parent_id));
	if (!enabled(error))
	{
		g_ptr_array_unref(orgs);
		return NULL;
	}
	venture_query_set_organization(query, parent_id);
	venture_query_set_limit(query, 0);
	links = venture_database_find(self->database, query, error);
	if (links == NULL)
	{
		g_ptr_array_unref(orgs);
		return NULL;
	}
	for (i = 0; i < links->len; i++)
	{
		gint64 child = 0;
		g_object_get(g_ptr_array_index(links, i), "child-organization-id", &child, NULL);
		if (child > 0 && !g_ptr_array_find(orgs, GSIZE_TO_POINTER((gsize)child), NULL))
			g_ptr_array_add(orgs, GSIZE_TO_POINTER((gsize)child));
	}
	return orgs;
}

static const gchar *
org_currency(VentureDatabase *database, gint64 org_id)
{
	g_autoptr(VentureEntity) org = venture_database_get(database, VENTURE_TYPE_ORGANIZATION, org_id, NULL);
	g_autofree gchar *currency = NULL;
	if (org == NULL)
		return "USD";
	g_object_get(org, "default-currency", &currency, NULL);
	return g_intern_string(currency != NULL && currency[0] != '\0' ? currency : "USD");
}

static VentureMoney *
convert(VentureGroupService *self, gint64 parent_id, const VentureMoney *amount,
	const gchar *currency, GDateTime *when, GError **error)
{
	g_autoptr(VentureExchangePolicy) policy = NULL;
	if (amount == NULL)
		return venture_money_new_zero(currency);
	if (g_strcmp0(amount->currency, currency) == 0)
		return venture_money_copy(amount);
	policy = venture_rate_table_policy_new(self->database, parent_id);
	return venture_exchange_policy_convert(policy, amount, currency, when, error);
}

static gint64
money_cell(VentureReportResult *result, guint row, const gchar *column)
{
	const GValue *value = venture_report_result_get_cell(result, row, column);
	const VentureMoney *money;
	if (value == NULL || !G_VALUE_HOLDS(value, VENTURE_TYPE_MONEY))
		return 0;
	money = g_value_get_boxed(value);
	return money != NULL ? money->amount : 0;
}

static const gchar *
text_cell(VentureReportResult *result, guint row, const gchar *column)
{
	const GValue *value = venture_report_result_get_cell(result, row, column);
	return value != NULL && G_VALUE_HOLDS_STRING(value) ? g_value_get_string(value) : "";
}

static gboolean
add_money(VentureMoney **total, const VentureMoney *amount, GError **error)
{
	VentureMoney *next = venture_money_add(*total, amount, error);
	if (next == NULL)
		return FALSE;
	venture_money_free(*total);
	*total = next;
	return TRUE;
}

static gboolean
period_matches(const gchar *elim_period, VentureDateRange *period)
{
	const gchar *label;
	g_autoptr(VentureDateRange) named = NULL;
	if (elim_period == NULL || elim_period[0] == '\0' || period == NULL)
		return TRUE;
	label = venture_date_range_get_label(period);
	if (label != NULL && g_str_equal(elim_period, label))
		return TRUE;
	named = venture_date_range_parse(elim_period, NULL, 1, NULL);
	if (named == NULL)
		return FALSE;
	return g_date_time_compare(venture_date_range_get_start(named),
		venture_date_range_get_end(period)) < 0 &&
		g_date_time_compare(venture_date_range_get_start(period),
		venture_date_range_get_end(named)) < 0;
}

VentureReportResult *
venture_group_service_consolidated(VentureGroupService *self, gint64 parent_id,
	const gchar *report_name, VentureDateRange *period, const gchar *currency, GError **error)
{
	g_autoptr(GPtrArray) orgs = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureMoney) income = NULL;
	g_autoptr(VentureMoney) expenses = NULL;
	g_autoptr(VentureLedgerBalances) books = NULL;
	g_autoptr(GDateTime) when = NULL;
	gboolean trial = g_strcmp0(report_name, "consolidated_trial_balance") == 0;
	gboolean sheet = g_strcmp0(report_name, "consolidated_balance_sheet") == 0;
	guint o;
	g_return_val_if_fail(VENTURE_IS_GROUP_SERVICE(self), NULL);
	if (!enabled(error))
		return NULL;
	if (period == NULL || currency == NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT,
			"Consolidation needs a period and book currency");
		return NULL;
	}
	orgs = members(self, parent_id, error);
	if (orgs == NULL)
		return NULL;
	when = g_date_time_add(venture_date_range_get_end(period), -1);
	books = venture_ledger_balances_new(self->database);
	result = venture_report_result_new(trial ? "Consolidated trial balance" :
		(sheet ? "Consolidated balance sheet" : "Consolidated income statement"), period);
	venture_report_result_add_column(result, "key", "Code", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "name", "Account", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "organization", "Organization", VENTURE_REPORT_COLUMN_TEXT);
	venture_report_result_add_column(result, "current", "Current", VENTURE_REPORT_COLUMN_MONEY);
	if (trial)
	{
		venture_report_result_add_column(result, "debits", "Debits", VENTURE_REPORT_COLUMN_MONEY);
		venture_report_result_add_column(result, "credits", "Credits", VENTURE_REPORT_COLUMN_MONEY);
	}
	income = venture_money_new_zero(currency);
	expenses = venture_money_new_zero(currency);
	for (o = 0; o < orgs->len; o++)
	{
		gint64 org = (gint64)GPOINTER_TO_SIZE(g_ptr_array_index(orgs, o));
		g_autoptr(VentureReportResult) balances = NULL;
		g_autofree gchar *org_label = g_strdup_printf("%" G_GINT64_FORMAT, org);
		guint i;
		balances = venture_ledger_balances_query(books, org, org_currency(self->database, org),
			period, NULL, FALSE, error);
		if (balances == NULL)
			return NULL;
		for (i = 0; i < venture_report_result_get_row_count(balances); i++)
		{
			gint64 account_id = g_ascii_strtoll(text_cell(balances, i, "account_id"), NULL, 10);
			g_autoptr(VentureEntity) account = NULL;
			g_autoptr(VentureMoney) native = NULL;
			g_autoptr(VentureMoney) converted = NULL;
			gint kind;
			gint64 amount;
			if (account_id <= 0)
				continue;
			account = venture_database_get(self->database, VENTURE_TYPE_ACCOUNT, account_id, error);
			if (account == NULL)
				return NULL;
			g_object_get(account, "kind", &kind, NULL);
			if (trial)
				amount = money_cell(balances, i, "closing");
			else if (sheet)
				amount = kind == VENTURE_ACCOUNT_KIND_ASSET
					? money_cell(balances, i, "closing") : -money_cell(balances, i, "closing");
			else if (kind == VENTURE_ACCOUNT_KIND_INCOME)
				amount = money_cell(balances, i, "credits") - money_cell(balances, i, "debits");
			else if (kind == VENTURE_ACCOUNT_KIND_EXPENSE)
				amount = money_cell(balances, i, "debits") - money_cell(balances, i, "credits");
			else
				continue;
			if (amount == 0)
				continue;
			native = venture_money_new_for_currency(amount, org_currency(self->database, org));
			converted = convert(self, parent_id, native, currency, when, error);
			if (converted == NULL)
				return NULL;
			venture_report_result_begin_row(result);
			/* Unclosed income and expense balances belong to retained earnings on the sheet. */
			venture_report_result_set_text(result, "key", sheet &&
				(kind == VENTURE_ACCOUNT_KIND_INCOME || kind == VENTURE_ACCOUNT_KIND_EXPENSE)
				? "equity" : text_cell(balances, i, "key"));
			venture_report_result_set_text(result, "name", sheet &&
				(kind == VENTURE_ACCOUNT_KIND_INCOME || kind == VENTURE_ACCOUNT_KIND_EXPENSE)
				? "Retained earnings" : text_cell(balances, i, "name"));
			venture_report_result_set_text(result, "organization", org_label);
			venture_report_result_set_money(result, "current", converted);
			if (trial)
			{
				g_autoptr(VentureMoney) zero = venture_money_new_zero(currency);
				g_autoptr(VentureMoney) opposite = venture_money_negate(converted);
				venture_report_result_set_money(result, "debits", amount > 0 ? converted : zero);
				venture_report_result_set_money(result, "credits", amount < 0 ? opposite : zero);
			}
			if (!sheet && !trial && kind == VENTURE_ACCOUNT_KIND_INCOME &&
				!add_money(&income, converted, error))
				return NULL;
			if (!sheet && !trial && kind == VENTURE_ACCOUNT_KIND_EXPENSE &&
				!add_money(&expenses, converted, error))
				return NULL;
		}
	}
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ELIMINATION);
		g_autoptr(GPtrArray) rows = NULL;
		guint i;
		venture_query_set_organization(query, parent_id);
		venture_query_set_limit(query, 0);
		rows = venture_database_find(self->database, query, error);
		if (rows == NULL)
			return NULL;
		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *row = g_ptr_array_index(rows, i);
			g_autofree gchar *elim_period = NULL;
			g_autoptr(VentureMoney) amount = NULL;
			gint64 debit_id = 0, credit_id = 0;
			g_autoptr(VentureEntity) debit = NULL;
			g_autoptr(VentureEntity) credit = NULL;
			gint kind;
			g_object_get(row, "period", &elim_period, "amount", &amount,
				"debit-account-id", &debit_id, "credit-account-id", &credit_id, NULL);
			if (!period_matches(elim_period, period))
				continue;
			if (amount == NULL)
				continue;
			{
				g_autoptr(VentureMoney) converted = convert(self, parent_id, amount, currency, when, error);
				if (converted == NULL)
					return NULL;
				g_clear_pointer(&amount, venture_money_free);
				amount = g_steal_pointer(&converted);
			}
			debit = venture_database_get(self->database, VENTURE_TYPE_ACCOUNT, debit_id, error);
			credit = venture_database_get(self->database, VENTURE_TYPE_ACCOUNT, credit_id, error);
			if (debit == NULL || credit == NULL)
				return NULL;
			if (!trial && !sheet)
			{
				gint credit_kind;
				g_object_get(debit, "kind", &kind, NULL);
				g_object_get(credit, "kind", &credit_kind, NULL);
				if ((kind == VENTURE_ACCOUNT_KIND_INCOME) != (credit_kind == VENTURE_ACCOUNT_KIND_INCOME))
				{
					VentureMoney *next = kind == VENTURE_ACCOUNT_KIND_INCOME
						? venture_money_subtract(income, amount, error) : venture_money_add(income, amount, error);
					if (next == NULL)
						return NULL;
					venture_money_free(income);
					income = next;
				}
				if ((kind == VENTURE_ACCOUNT_KIND_EXPENSE) != (credit_kind == VENTURE_ACCOUNT_KIND_EXPENSE))
				{
					VentureMoney *next = kind == VENTURE_ACCOUNT_KIND_EXPENSE
						? venture_money_add(expenses, amount, error) : venture_money_subtract(expenses, amount, error);
					if (next == NULL)
						return NULL;
					venture_money_free(expenses);
					expenses = next;
				}
			}
			if (trial || sheet)
			{
				g_autoptr(VentureMoney) credit_amount = NULL;
				g_autoptr(VentureMoney) debit_amount = NULL;
				g_autofree gchar *debit_code = NULL;
				g_autofree gchar *debit_name = NULL;
				g_autofree gchar *credit_code = NULL;
				g_autofree gchar *credit_name = NULL;
				gint debit_kind, credit_kind;
				gboolean debit_bs, credit_bs;
				credit_amount = venture_money_negate(amount);
				if (credit_amount == NULL)
					return NULL;
				g_object_get(debit, "code", &debit_code, "name", &debit_name, "kind", &debit_kind, NULL);
				g_object_get(credit, "code", &credit_code, "name", &credit_name, "kind", &credit_kind, NULL);
				debit_amount = sheet && debit_kind != VENTURE_ACCOUNT_KIND_ASSET
					? venture_money_negate(amount) : venture_money_copy(amount);
				if (sheet && credit_kind != VENTURE_ACCOUNT_KIND_ASSET)
				{
					g_clear_pointer(&credit_amount, venture_money_free);
					credit_amount = venture_money_copy(amount);
				}
				debit_bs = debit_kind == VENTURE_ACCOUNT_KIND_ASSET ||
					debit_kind == VENTURE_ACCOUNT_KIND_LIABILITY ||
					debit_kind == VENTURE_ACCOUNT_KIND_EQUITY;
				credit_bs = credit_kind == VENTURE_ACCOUNT_KIND_ASSET ||
					credit_kind == VENTURE_ACCOUNT_KIND_LIABILITY ||
					credit_kind == VENTURE_ACCOUNT_KIND_EQUITY;
				if (trial || debit_bs || (sheet && !debit_bs))
				{
					venture_report_result_begin_row(result);
					if (sheet && !debit_bs)
					{
						venture_report_result_set_text(result, "key", "equity");
						venture_report_result_set_text(result, "name", "Retained earnings (eliminations)");
					}
					else
					{
						venture_report_result_set_text(result, "key", debit_code);
						venture_report_result_set_text(result, "name", debit_name);
					}
					venture_report_result_set_text(result, "organization", "elimination");
					venture_report_result_set_money(result, "current", debit_amount);
					if (trial)
					{
						g_autoptr(VentureMoney) zero = venture_money_new_zero(amount->currency);
						venture_report_result_set_money(result, "debits", amount);
						venture_report_result_set_money(result, "credits", zero);
					}
				}
				if (trial || credit_bs || (sheet && !credit_bs))
				{
					venture_report_result_begin_row(result);
					if (sheet && !credit_bs)
					{
						venture_report_result_set_text(result, "key", "equity");
						venture_report_result_set_text(result, "name", "Retained earnings (eliminations)");
					}
					else
					{
						venture_report_result_set_text(result, "key", credit_code);
						venture_report_result_set_text(result, "name", credit_name);
					}
					venture_report_result_set_text(result, "organization", "elimination");
					venture_report_result_set_money(result, "current", credit_amount);
					if (trial)
					{
						g_autoptr(VentureMoney) zero = venture_money_new_zero(amount->currency);
						venture_report_result_set_money(result, "debits", zero);
						venture_report_result_set_money(result, "credits", amount);
					}
				}
			}
		}
		if (!trial && !sheet)
		{
			venture_report_result_begin_row(result);
			venture_report_result_set_text(result, "key", "income");
			venture_report_result_set_text(result, "name", "Total income");
			venture_report_result_set_text(result, "organization", "group");
			venture_report_result_set_money(result, "current", income);
			venture_report_result_begin_row(result);
			venture_report_result_set_text(result, "key", "expenses");
			venture_report_result_set_text(result, "name", "Total expenses");
			venture_report_result_set_text(result, "organization", "group");
			venture_report_result_set_money(result, "current", expenses);
		}
	}
	return g_steal_pointer(&result);
}

static VentureReportResult *
report_named(const gchar *name, VentureContext *context, VentureDateRange *period, JsonObject *options, GError **error)
{
	gint64 org = options != NULL ? venture_json_object_get_int(options, "organization_id", 0) : 0;
	const gchar *currency = options != NULL ? venture_json_object_get_string(options, "currency", "USD") : "USD";
	if (org == 0)
		org = venture_context_get_default_organization_id(context);
	return venture_group_service_consolidated(venture_group_service_get(venture_context_get_database(context)),
		org, name, period, currency, error);
}

static VentureReportResult *
report_tb(VentureContext *c, VentureDateRange *p, JsonObject *o, GError **e)
{ return report_named("consolidated_trial_balance", c, p, o, e); }
static VentureReportResult *
report_is(VentureContext *c, VentureDateRange *p, JsonObject *o, GError **e)
{ return report_named("consolidated_income_statement", c, p, o, e); }
static VentureReportResult *
report_bs(VentureContext *c, VentureDateRange *p, JsonObject *o, GError **e)
{ return report_named("consolidated_balance_sheet", c, p, o, e); }

void
venture_group_register_reports(VentureReportRegistry *registry)
{
	VentureReport *tb = VENTURE_REPORT(venture_func_report_new("consolidated_trial_balance",
		"Consolidated trial balance", "Combined trial balance with org dimension and FX.", report_tb));
	VentureReport *income = VENTURE_REPORT(venture_func_report_new("consolidated_income_statement",
		"Consolidated income statement", "Combined income with eliminations and FX.", report_is));
	VentureReport *sheet = VENTURE_REPORT(venture_func_report_new("consolidated_balance_sheet",
		"Consolidated balance sheet", "Combined balance sheet with org dimension and FX.", report_bs));
	g_object_set(tb, "financial", TRUE, NULL);
	g_object_set(income, "financial", TRUE, NULL);
	g_object_set(sheet, "financial", TRUE, NULL);
	venture_report_registry_add(registry, tb);
	venture_report_registry_add(registry, income);
	venture_report_registry_add(registry, sheet);
}
