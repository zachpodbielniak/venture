/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include "venture-test-util.h"

typedef struct
{
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	gint64 parent;
	gint64 child;
} Fixture;

static void
chart(Fixture *f, gint64 org)
{
	static const struct { const gchar *code; const gchar *name; VentureAccountKind kind; } rows[] = {
		{ "1000", "Cash", VENTURE_ACCOUNT_KIND_ASSET },
		{ "1100", "Accounts receivable", VENTURE_ACCOUNT_KIND_ASSET },
		{ "2000", "Accounts payable", VENTURE_ACCOUNT_KIND_LIABILITY },
		{ "4000", "Sales", VENTURE_ACCOUNT_KIND_INCOME },
		{ "6900", "General expenses", VENTURE_ACCOUNT_KIND_EXPENSE }
	};
	gsize i;
	for (i = 0; i < G_N_ELEMENTS(rows); i++)
	{
		g_autoptr(VentureEntity) account = VENTURE_ENTITY(venture_account_new());
		g_autoptr(GError) error = NULL;
		g_object_set(account, "organization-id", org, "code", rows[i].code, "name", rows[i].name,
			"kind", rows[i].kind, "active", TRUE, NULL);
		g_assert_true(venture_database_save(f->db, account, NULL, &error));
		g_assert_no_error(error);
	}
}

static gint64
make_org(Fixture *f, const gchar *name, const gchar *currency)
{
	g_autoptr(VentureEntity) org = VENTURE_ENTITY(venture_organization_new());
	g_autoptr(GError) error = NULL;
	g_object_set(org, "name", name, "default-currency", currency, NULL);
	g_assert_true(venture_database_save(f->db, org, NULL, &error));
	g_assert_no_error(error);
	chart(f, venture_entity_get_id(org));
	return venture_entity_get_id(org);
}

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	(void)data;
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->config = venture_config_new();
	g_object_set(f->config, "locale-timezone", "UTC", "group-enabled", TRUE, NULL);
	f->context = venture_context_new(f->config, f->db);
	f->parent = venture_context_get_default_organization_id(f->context);
	f->child = make_org(f, "Subsidiary", "EUR");
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	g_clear_object(&f->context);
	g_clear_object(&f->config);
	g_clear_object(&f->db);
}

static gint64
account_in(Fixture *f, gint64 org, const gchar *code)
{
	g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(VentureEntity) row = NULL;
	venture_query_set_organization(q, org);
	venture_query_add_filter_string(q, "code", VENTURE_FILTER_OP_EQ, code, NULL);
	row = venture_database_find_one(f->db, q, NULL);
	g_assert_nonnull(row);
	return venture_entity_get_id(row);
}

static void
post_in(Fixture *f, gint64 org, const gchar *currency, const gchar *when,
	const gchar *debit, const gchar *credit, gint64 amount)
{
	g_autoptr(VentureJournal) header = venture_journal_new();
	g_autoptr(VentureJournal) posted = NULL;
	g_autoptr(GPtrArray) lines = g_ptr_array_new_with_free_func(g_object_unref);
	g_autoptr(GDateTime) date = g_date_time_new_from_iso8601(when, NULL);
	g_autoptr(VentureMoney) money = venture_money_new_for_currency(amount, currency);
	g_autoptr(GError) error = NULL;
	VentureJournalLine *line;
	g_object_set(header, "organization-id", org, "source-type", "organization",
		"source-id", org, "occurred-at", date, "currency", currency, NULL);
	line = venture_journal_line_new();
	g_object_set(line, "account-id", account_in(f, org, debit), "side", VENTURE_LEDGER_SIDE_DEBIT, "amount", money, NULL);
	g_ptr_array_add(lines, line);
	line = venture_journal_line_new();
	g_object_set(line, "account-id", account_in(f, org, credit), "side", VENTURE_LEDGER_SIDE_CREDIT, "amount", money, NULL);
	g_ptr_array_add(lines, line);
	posted = venture_posting_service_post(venture_database_get_posting_service(f->db), header, lines, NULL, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(posted);
}

static gint64
cell_org(VentureReportResult *r, const gchar *code, const gchar *org_label, const gchar *column)
{
	guint i;
	for (i = 0; i < venture_report_result_get_row_count(r); i++)
	{
		const GValue *key = venture_report_result_get_cell(r, i, "key");
		const GValue *org = venture_report_result_get_cell(r, i, "organization");
		if (key != NULL && org != NULL && g_strcmp0(g_value_get_string(key), code) == 0 &&
			g_strcmp0(g_value_get_string(org), org_label) == 0)
		{
			const GValue *value = venture_report_result_get_cell(r, i, column);
			g_assert_nonnull(value);
			g_assert_true(G_VALUE_HOLDS(value, VENTURE_TYPE_MONEY));
			return ((const VentureMoney *)g_value_get_boxed(value))->amount;
		}
	}
	g_error("Missing consolidated row %s/%s", code, org_label);
	return 0;
}

static gint64
cell_key(VentureReportResult *r, const gchar *key, const gchar *column)
{
	guint i;
	for (i = 0; i < venture_report_result_get_row_count(r); i++)
	{
		const GValue *name = venture_report_result_get_cell(r, i, "key");
		if (name != NULL && g_strcmp0(g_value_get_string(name), key) == 0)
		{
			const GValue *value = venture_report_result_get_cell(r, i, column);
			g_assert_nonnull(value);
			g_assert_true(G_VALUE_HOLDS(value, VENTURE_TYPE_MONEY));
			return ((const VentureMoney *)g_value_get_boxed(value))->amount;
		}
	}
	g_error("Missing row %s", key);
	return 0;
}

static void
test_off_by_default(void)
{
	g_autoptr(VentureModuleRegistry) registry = venture_module_registry_new();
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(GError) error = NULL;
	VentureModule *module;
	venture_module_registry_register_builtins(registry);
	g_assert_true(venture_module_registry_configure(registry, config, &error));
	module = venture_module_registry_lookup(registry, "group");
	g_assert_nonnull(module);
	g_assert_false(venture_module_is_enabled(module));
}

static void
test_records(Fixture *f, gconstpointer data)
{
	(void)data;
	g_assert_true(venture_module_registry_is_enabled(venture_context_get_modules(f->context), "group"));
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(),
		"intercompany_link"), !=, G_TYPE_INVALID);
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(),
		"elimination"), !=, G_TYPE_INVALID);
}

static void
test_consolidated_and_fx(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) link = VENTURE_ENTITY(venture_intercompany_link_new());
	g_autoptr(VentureEntity) rate = VENTURE_ENTITY(venture_exchange_rate_new());
	g_autoptr(GDateTime) effective = g_date_time_new_utc(2026, 1, 1, 0, 0, 0);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureReportResult) income = NULL;
	g_autoptr(VentureDateRange) period = NULL;
	(void)data;
	g_object_set(link, "organization-id", f->parent, "child-organization-id", f->child, NULL);
	g_assert_true(venture_database_save(f->db, link, NULL, &error));
	g_assert_no_error(error);
	g_object_set(rate, "organization-id", f->parent, "from-currency", "EUR", "to-currency", "USD",
		"rate-numerator", (gint64)2, "rate-denominator", (gint64)1, "effective-at", effective,
		"source", "manual", "reason", "test", NULL);
	g_assert_true(venture_database_save(f->db, rate, NULL, &error));
	g_assert_no_error(error);
	post_in(f, f->parent, "USD", "2026-08-10T00:00:00Z", "1000", "4000", 10000);
	post_in(f, f->child, "EUR", "2026-08-10T00:00:00Z", "1000", "4000", 5000);
	period = venture_context_parse_period(f->context, "2026-08", &error);
	g_assert_no_error(error);
	income = venture_group_service_consolidated(venture_group_service_get(f->db),
		f->parent, "consolidated_income_statement", period, "USD", &error);
	g_assert_no_error(error);
	g_assert_nonnull(income);
	{
		g_autofree gchar *parent_label = g_strdup_printf("%" G_GINT64_FORMAT, f->parent);
		g_autofree gchar *child_label = g_strdup_printf("%" G_GINT64_FORMAT, f->child);
		g_assert_cmpint(cell_org(income, "4000", parent_label, "current"), ==, 10000);
		g_assert_cmpint(cell_org(income, "4000", child_label, "current"), ==, 10000);
	}
	g_assert_cmpint(cell_key(income, "income", "current"), ==, 20000);
}

static void
test_elimination(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) link = VENTURE_ENTITY(venture_intercompany_link_new());
	g_autoptr(VentureMoney) amount = venture_money_new_for_currency(10000, "USD");
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureReportResult) sheet = NULL;
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(VentureEntity) elimination = NULL;
	(void)data;
	g_object_set(link, "organization-id", f->parent, "child-organization-id", f->child, NULL);
	g_assert_true(venture_database_save(f->db, link, NULL, &error));
	{
		g_autoptr(VentureEntity) rate = VENTURE_ENTITY(venture_exchange_rate_new());
		g_autoptr(GDateTime) effective = g_date_time_new_utc(2026, 1, 1, 0, 0, 0);
		g_object_set(rate, "organization-id", f->parent, "from-currency", "EUR", "to-currency", "USD",
			"rate-numerator", (gint64)1, "rate-denominator", (gint64)1, "effective-at", effective,
			"source", "manual", "reason", "test", NULL);
		g_assert_true(venture_database_save(f->db, rate, NULL, &error));
		g_assert_no_error(error);
	}
	post_in(f, f->parent, "USD", "2026-08-10T00:00:00Z", "1100", "4000", 10000);
	post_in(f, f->child, "EUR", "2026-08-10T00:00:00Z", "6900", "2000", 10000);
	elimination = venture_group_service_eliminate(venture_group_service_get(f->db), f->parent, f->child,
		account_in(f, f->parent, "4000"), account_in(f, f->parent, "1100"), amount, "2026-08",
		"Intercompany revenue", NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(elimination);
	period = venture_context_parse_period(f->context, "2026-08", &error);
	sheet = venture_group_service_consolidated(venture_group_service_get(f->db),
		f->parent, "consolidated_income_statement", period, "USD", &error);
	g_assert_no_error(error);
	g_assert_cmpint(cell_key(sheet, "income", "current"), ==, 0);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/group/off-by-default", test_off_by_default);
	g_test_add("/group/records", Fixture, NULL, setup, test_records, teardown);
	g_test_add("/group/consolidated-fx", Fixture, NULL, setup, test_consolidated_and_fx, teardown);
	g_test_add("/group/elimination", Fixture, NULL, setup, test_elimination, teardown);
	return g_test_run();
}
