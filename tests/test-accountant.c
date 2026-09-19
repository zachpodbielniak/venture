/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <libsoup/soup.h>
#include <string.h>
#include <unistd.h>
#ifdef VENTURE_HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif
#include "venture-test-util.h"

/* --- Policy fixtures ------------------------------------------------------ */

typedef struct
{
	VentureConfig *config;
	VentureDatabase *db;
	VentureContext *context;
	VentureEntity *user;
	VentureEntity *member;
	VentureAuthPrincipal actor;
	gint64 org;
} PolicyFixture;

static void
policy_set_up(PolicyFixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->org = venture_context_get_default_organization_id(f->context);
	f->user = g_object_new(VENTURE_TYPE_USER, "username", "cpa", "active", TRUE,
		"role", VENTURE_USER_ROLE_VIEWER, NULL);
	g_assert_true(venture_database_save(f->db, f->user, NULL, &error));
	g_assert_no_error(error);
	f->member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "user-id", venture_entity_get_id(f->user),
		"organization-id", f->org, "active", TRUE, "role", VENTURE_ORGANIZATION_ROLE_ACCOUNTANT, NULL);
	g_assert_true(venture_database_save(f->db, f->member, NULL, &error));
	g_assert_no_error(error);
	f->actor.user_id = venture_entity_get_id(f->user);
	f->actor.token_id = 0;
	f->actor.role = VENTURE_USER_ROLE_VIEWER;
	f->actor.name = NULL;
	f->actor.authenticated = TRUE;
}

static void
policy_tear_down(PolicyFixture *f, gconstpointer unused)
{
	g_clear_object(&f->member);
	g_clear_object(&f->user);
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

/* The role is an ordinary enum value, so the generated membership form,
 * REST body and CLI all accept "accountant" without a special route. */
static void
test_role_nick(PolicyFixture *f, gconstpointer unused)
{
	g_autoptr(GEnumClass) klass = g_type_class_ref(VENTURE_TYPE_ORGANIZATION_ROLE);
	GEnumValue *value = g_enum_get_value_by_nick(klass, "accountant");
	g_assert_nonnull(value);
	g_assert_cmpint(value->value, ==, VENTURE_ORGANIZATION_ROLE_ACCOUNTANT);
}

/* Financial records are readable and exportable; every write is refused
 * with an error that names the role rather than a generic denial. */
static void
test_books_read_only(PolicyFixture *f, gconstpointer unused)
{
	VentureAccessPolicy *policy = venture_database_get_access_policy(f->db);
	GType books[] = {
		VENTURE_TYPE_ACCOUNT, VENTURE_TYPE_JOURNAL, VENTURE_TYPE_JOURNAL_LINE, VENTURE_TYPE_LEDGER_ENTRY,
		VENTURE_TYPE_EXPENSE, VENTURE_TYPE_INVOICE, VENTURE_TYPE_PAYMENT, VENTURE_TYPE_VENDOR_BILL,
		VENTURE_TYPE_BILL_PAYMENT, VENTURE_TYPE_BANK_ACCOUNT, VENTURE_TYPE_BANK_STATEMENT,
		VENTURE_TYPE_TAX_CODE, VENTURE_TYPE_TAX_FILING, VENTURE_TYPE_FIXED_ASSET,
		VENTURE_TYPE_SAVED_REPORT, VENTURE_TYPE_REPORT_PACK, VENTURE_TYPE_FISCAL_YEAR,
		VENTURE_TYPE_COMPANY
	};
	const gchar *writes[] = { "write", "delete" };
	guint i, j;
	for (i = 0; i < G_N_ELEMENTS(books); i++)
	{
		g_autoptr(VentureEntity) row = g_object_new(books[i], "organization-id", f->org, NULL);
		g_autoptr(GError) error = NULL;
		g_assert_true(venture_access_policy_can(policy, &f->actor, "read", row, &error));
		g_assert_no_error(error);
		g_assert_true(venture_access_policy_can(policy, &f->actor, "export", row, NULL));
		for (j = 0; j < G_N_ELEMENTS(writes); j++)
		{
			g_autoptr(GError) refused = NULL;
			g_assert_false(venture_access_policy_can(policy, &f->actor, writes[j], row, &refused));
			g_assert_error(refused, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
			g_assert_nonnull(strstr(refused->message, "accountant"));
		}
	}
}

/* Nothing outside the books exists for the accountant: read or write, the
 * refusal is the not-found a stranger receives, never a 403 confirming the
 * record. */
static void
test_hidden_records(PolicyFixture *f, gconstpointer unused)
{
	VentureAccessPolicy *policy = venture_database_get_access_policy(f->db);
	g_autoptr(VentureEntity) colleague = g_object_new(VENTURE_TYPE_USER, "username", "colleague", "active", TRUE, NULL);
	GType hidden[] = {
		VENTURE_TYPE_LEAD, VENTURE_TYPE_DEAL, VENTURE_TYPE_ACTIVITY, VENTURE_TYPE_TICKET,
		VENTURE_TYPE_MAIL_MESSAGE, VENTURE_TYPE_SEQUENCE, VENTURE_TYPE_CONTACT, VENTURE_TYPE_SALE,
		VENTURE_TYPE_PAYROLL_RUN, VENTURE_TYPE_TEAM, VENTURE_TYPE_ORGANIZATION_MEMBERSHIP,
		VENTURE_TYPE_PLUGIN_CONFIG, VENTURE_TYPE_WEBHOOK, VENTURE_TYPE_SAVED_VIEW
	};
	guint i;
	g_assert_true(venture_database_save(f->db, colleague, NULL, NULL));
	for (i = 0; i < G_N_ELEMENTS(hidden); i++)
	{
		g_autoptr(VentureEntity) row = g_object_new(hidden[i], "organization-id", f->org, NULL);
		g_autoptr(GError) error = NULL;
		g_autoptr(GError) refused = NULL;
		g_assert_false(venture_access_policy_can(policy, &f->actor, "read", row, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
		g_assert_false(venture_access_policy_can(policy, &f->actor, "write", row, &refused));
		g_assert_error(refused, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	}
	/* Other users are settings, not books; the accountant's own row stays
	 * readable so the account page and sign-out work. */
	{
		g_autoptr(GError) error = NULL;
		g_assert_false(venture_access_policy_can(policy, &f->actor, "read", colleague, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
		g_assert_true(venture_access_policy_can(policy, &f->actor, "read", f->user, NULL));
	}
}

/* A document is part of the books only when it is attached to a financial
 * record; a loose contract or manuscript is not. */
static void
test_financial_documents(PolicyFixture *f, gconstpointer unused)
{
	VentureAccessPolicy *policy = venture_database_get_access_policy(f->db);
	g_autoptr(VentureMoney) amount = venture_money_new(1250, "USD", 2);
	g_autoptr(GDateTime) when = venture_time_from_string("2025-03-01", NULL);
	g_autoptr(VentureEntity) expense = g_object_new(VENTURE_TYPE_EXPENSE, "organization-id", f->org,
		"amount", amount, "occurred-at", when, "description", "Printer paper", NULL);
	g_autoptr(VentureEntity) receipt = NULL;
	g_autoptr(VentureEntity) loose = g_object_new(VENTURE_TYPE_DOCUMENT, "organization-id", f->org, "title", "Manuscript", NULL);
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, expense, NULL, NULL));
	receipt = g_object_new(VENTURE_TYPE_DOCUMENT, "organization-id", f->org, "title", "Receipt",
		"expense-id", venture_entity_get_id(expense), NULL);
	g_assert_true(venture_access_policy_can(policy, &f->actor, "read", receipt, NULL));
	g_assert_false(venture_access_policy_can(policy, &f->actor, "write", receipt, NULL));
	g_assert_false(venture_access_policy_can(policy, &f->actor, "read", loose, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
}

/* Generic writes through the repository are refused before any row is
 * touched, and an accountant never proposes a change for approval. */
static void
test_repository_refuses_writes(PolicyFixture *f, gconstpointer unused)
{
	VentureAccessPolicy *policy = venture_database_get_access_policy(f->db);
	g_autoptr(VentureEntity) journal = g_object_new(VENTURE_TYPE_JOURNAL, "organization-id", f->org, "memo", "Year end", NULL);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_JOURNAL);
	g_autoptr(GError) error = NULL;
	g_autoptr(GError) approval = NULL;
	g_autoptr(VentureAccessScope) scope = venture_access_policy_enter(policy, &f->actor);
	g_assert_false(venture_database_save(f->db, journal, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_assert_nonnull(strstr(error->message, "accountant"));
	g_assert_false(venture_access_policy_requires_approval(policy, &f->actor, "write", journal, &approval));
	g_assert_no_error(approval);
	g_assert_false(venture_access_policy_requires_approval(policy, &f->actor, "post", journal, &approval));
	g_assert_no_error(approval);
	g_clear_object(&scope);
	g_assert_cmpint(venture_database_count(f->db, query, NULL), ==, 0);
}

/* A CPA with several clients holds one accountant membership per client
 * organisation; the picker lists exactly those and nothing leaks across. */
static void
test_many_organisations(PolicyFixture *f, gconstpointer unused)
{
	VentureAccessPolicy *policy = venture_database_get_access_policy(f->db);
	g_autoptr(VentureEntity) second = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Client B", "slug", "b", NULL);
	g_autoptr(VentureEntity) third = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Not a client", "slug", "c", NULL);
	g_autoptr(VentureEntity) second_member = NULL;
	g_autoptr(VentureQuery) orgs = venture_query_new(VENTURE_TYPE_ORGANIZATION);
	g_autoptr(VentureQuery) journals = venture_query_new(VENTURE_TYPE_JOURNAL);
	g_autoptr(GPtrArray) visible = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	gint64 ids[3];
	guint i;
	g_assert_true(venture_database_save(f->db, second, NULL, NULL));
	g_assert_true(venture_database_save(f->db, third, NULL, NULL));
	second_member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "user-id", f->actor.user_id,
		"organization-id", venture_entity_get_id(second), "active", TRUE, "role", VENTURE_ORGANIZATION_ROLE_ACCOUNTANT, NULL);
	g_assert_true(venture_database_save(f->db, second_member, NULL, NULL));
	ids[0] = f->org;
	ids[1] = venture_entity_get_id(second);
	ids[2] = venture_entity_get_id(third);
	for (i = 0; i < 3; i++)
	{
		g_autoptr(VentureEntity) journal = g_object_new(VENTURE_TYPE_JOURNAL, "organization-id", ids[i], "memo", "Opening", NULL);
		g_assert_true(venture_database_save(f->db, journal, NULL, NULL));
		g_assert_cmpint(venture_access_policy_can(policy, &f->actor, "read", journal, NULL), ==, i < 2);
	}
	g_assert_true(venture_access_policy_has_membership(policy, &f->actor));
	scope = venture_access_policy_enter(policy, &f->actor);
	visible = venture_database_find(f->db, orgs, NULL);
	g_assert_cmpuint(visible->len, ==, 2);
	g_assert_cmpint(venture_database_count(f->db, journals, NULL), ==, 2);
	/* Switching to one client narrows the books to that client alone. */
	venture_query_set_organization(journals, ids[1]);
	g_assert_cmpint(venture_database_count(f->db, journals, NULL), ==, 1);
	venture_query_set_organization(journals, ids[2]);
	g_assert_cmpint(venture_database_count(f->db, journals, NULL), ==, 0);
	/* Deactivating one client's membership removes only that client. */
	g_clear_object(&scope);
	g_object_set(second_member, "active", FALSE, NULL);
	g_assert_true(venture_database_save(f->db, second_member, NULL, NULL));
	scope = venture_access_policy_enter(policy, &f->actor);
	g_clear_pointer(&visible, g_ptr_array_unref);
	visible = venture_database_find(f->db, orgs, NULL);
	g_assert_cmpuint(visible->len, ==, 1);
	g_assert_cmpint(venture_entity_get_id(g_ptr_array_index(visible, 0)), ==, f->org);
}

/* A token minted while the user was an accountant cannot write even after a
 * promotion; the snapshot intersects with the live role as for every role. */
static void
test_token_snapshot(PolicyFixture *f, gconstpointer unused)
{
	VentureAccessPolicy *policy = venture_database_get_access_policy(f->db);
	g_autoptr(VentureApiToken) token = venture_api_token_new();
	g_autofree gchar *secret = NULL;
	g_autoptr(VentureEntity) journal = g_object_new(VENTURE_TYPE_JOURNAL, "organization-id", f->org, NULL);
	g_autoptr(GError) error = NULL;
	VentureAuthPrincipal bearer = f->actor;
	g_object_set(token, "name", "cpa-token", "user-id", f->actor.user_id, "role", VENTURE_USER_ROLE_EDITOR, NULL);
	secret = venture_api_token_generate(token);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(token), NULL, NULL));
	bearer.token_id = venture_entity_get_id(VENTURE_ENTITY(token));
	bearer.role = VENTURE_USER_ROLE_EDITOR;
	g_assert_true(venture_access_policy_can(policy, &bearer, "read", journal, NULL));
	g_object_set(f->member, "role", VENTURE_ORGANIZATION_ROLE_FINANCE, NULL);
	g_assert_true(venture_database_save(f->db, f->member, NULL, NULL));
	g_assert_false(venture_access_policy_can(policy, &bearer, "write", journal, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_assert_nonnull(strstr(error->message, "accountant"));
}

/* The Books-only surface is for a user whose every membership is the
 * accountant role: one finance membership elsewhere, a global bypass, no
 * membership at all, or an inactive one each puts the ordinary sidebar back. */
static void
test_accountant_only(PolicyFixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) second = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Client B", "slug", "b", NULL);
	g_autoptr(VentureEntity) second_member = NULL;
	VentureAuthPrincipal stranger = f->actor;
	VentureAuthPrincipal owner = f->actor;
	g_assert_true(venture_accountant_role_only(f->db, &f->actor));
	g_assert_false(venture_accountant_role_only(f->db, NULL));
	stranger.user_id = f->actor.user_id + 1000;
	g_assert_false(venture_accountant_role_only(f->db, &stranger));
	owner.role = VENTURE_USER_ROLE_OWNER;
	g_assert_false(venture_accountant_role_only(f->db, &owner));
	g_assert_true(venture_database_save(f->db, second, NULL, NULL));
	second_member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "user-id", f->actor.user_id,
		"organization-id", venture_entity_get_id(second), "active", TRUE, "role", VENTURE_ORGANIZATION_ROLE_ACCOUNTANT, NULL);
	g_assert_true(venture_database_save(f->db, second_member, NULL, NULL));
	g_assert_true(venture_accountant_role_only(f->db, &f->actor));
	g_object_set(second_member, "role", VENTURE_ORGANIZATION_ROLE_FINANCE, NULL);
	g_assert_true(venture_database_save(f->db, second_member, NULL, NULL));
	g_assert_false(venture_accountant_role_only(f->db, &f->actor));
	g_object_set(second_member, "active", FALSE, NULL);
	g_assert_true(venture_database_save(f->db, second_member, NULL, NULL));
	g_assert_true(venture_accountant_role_only(f->db, &f->actor));
	g_object_set(f->member, "active", FALSE, NULL);
	g_assert_true(venture_database_save(f->db, f->member, NULL, NULL));
	g_assert_false(venture_accountant_role_only(f->db, &f->actor));
}

/* --- Year-end pack -------------------------------------------------------- */

static const gchar *const pack_files[] = {
	"trial_balance.csv", "income_statement.csv", "balance_sheet.csv", "cash_flow.csv",
	"receivables_aging.csv", "payables_aging.csv", "general_ledger.csv", "sales_tax.csv",
	"contractor_1099.csv", "fixed_assets.csv", "index.txt", NULL
};

static gboolean
result_has_file(VentureReportResult *result, const gchar *file)
{
	guint i;
	for (i = 0; i < venture_report_result_get_row_count(result); i++)
	{
		const GValue *cell = venture_report_result_get_cell(result, i, "file");
		if (cell != NULL && G_VALUE_HOLDS_STRING(cell) && 0 == g_strcmp0(g_value_get_string(cell), file))
			return TRUE;
	}
	return FALSE;
}

#ifdef VENTURE_HAVE_LIBARCHIVE
static GHashTable *
zip_members(GBytes *zip)
{
	GHashTable *members = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	struct archive *reader = archive_read_new();
	struct archive_entry *entry;
	archive_read_support_format_zip(reader);
	g_assert_cmpint(archive_read_open_memory(reader, (void *)g_bytes_get_data(zip, NULL), g_bytes_get_size(zip)), ==, ARCHIVE_OK);
	while (archive_read_next_header(reader, &entry) == ARCHIVE_OK)
	{
		gsize size = (gsize)archive_entry_size(entry);
		gchar *body = g_malloc0(size + 1);
		if (size > 0)
			g_assert_cmpint(archive_read_data(reader, body, size), ==, (gssize)size);
		g_hash_table_insert(members, g_strdup(archive_entry_pathname(entry)), body);
	}
	archive_read_free(reader);
	return members;
}
#endif

static void
test_year_end_pack(void)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(GBytes) zip = NULL;
	g_autoptr(GError) error = NULL;
	VentureReport *report;
	guint i;
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	context = venture_context_new(config, db);
	report = venture_report_registry_lookup(venture_context_get_report_registry(context), "year_end_pack");
	g_assert_nonnull(report);
	period = venture_context_parse_period(context, "fy_2025", &error);
	g_assert_no_error(error);
	json_object_set_int_member(options, "organization_id", venture_context_get_default_organization_id(context));
	result = venture_report_generate(report, context, period, options, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	for (i = 0; pack_files[i] != NULL; i++)
		g_assert_true(result_has_file(result, pack_files[i]));
	zip = venture_year_end_pack_zip(result, &error);
#ifdef VENTURE_HAVE_LIBARCHIVE
	{
		g_autoptr(GHashTable) members = NULL;
		const gchar *index;
		g_assert_no_error(error);
		g_assert_nonnull(zip);
		g_assert_cmpmem(g_bytes_get_data(zip, NULL), 2, "PK", 2);
		members = zip_members(zip);
		for (i = 0; pack_files[i] != NULL; i++)
			g_assert_true(g_hash_table_contains(members, pack_files[i]));
		g_assert_cmpuint(g_hash_table_size(members), ==, G_N_ELEMENTS(pack_files) - 1);
		index = g_hash_table_lookup(members, "index.txt");
		g_assert_nonnull(strstr(index, "trial_balance.csv"));
		g_assert_nonnull(strstr(index, "Trial balance"));
		g_assert_nonnull(strstr(g_hash_table_lookup(members, "trial_balance.csv"), "\n"));
	}
#else
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED);
#endif
}

/* A module that is off leaves its file out and says so in the index; the
 * pack is never silently incomplete. */
static void
test_year_end_pack_module_off(void)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureDateRange) period = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(GError) error = NULL;
	VentureReport *report;
	guint i;
	gboolean noted = FALSE;
	venture_config_set_module_enabled(config, "assets", FALSE);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	context = venture_context_new(config, db);
	report = venture_report_registry_lookup(venture_context_get_report_registry(context), "year_end_pack");
	g_assert_nonnull(report);
	period = venture_context_parse_period(context, "fy_2025", &error);
	result = venture_report_generate(report, context, period, NULL, &error);
	g_assert_no_error(error);
	g_assert_false(result_has_file(result, "fixed_assets.csv"));
	g_assert_true(result_has_file(result, "trial_balance.csv"));
	for (i = 0; i < venture_report_result_get_row_count(result); i++)
	{
		const GValue *file = venture_report_result_get_cell(result, i, "file");
		const GValue *content = venture_report_result_get_cell(result, i, "content");
		if (0 == g_strcmp0(g_value_get_string(file), "index.txt") &&
			strstr(g_value_get_string(content), "fixed_assets.csv") != NULL &&
			strstr(g_value_get_string(content), "omitted") != NULL)
			noted = TRUE;
	}
	g_assert_true(noted);
	venture_config_set_module_enabled(config, "assets", TRUE);
}

/* The pack rides the scheduled-report path: a saved report in a pack, run
 * by the due sweep, is retained in last_output and the zip is rebuilt from
 * exactly those retained bytes. */
static void
test_year_end_pack_scheduled(void)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureEntity) saved = NULL;
	g_autoptr(VentureEntity) pack = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(GDateTime) at = venture_time_from_string("2026-01-05T08:30:00Z", NULL);
	g_autoptr(GBytes) zip = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *output = NULL;
	VentureReportPackService *service;
	gint64 org;
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	context = venture_context_new(config, db);
	org = venture_context_get_default_organization_id(context);
	service = venture_report_pack_service_get(db);
	saved = venture_report_pack_service_save(service, org, "FY2025 year end", "year_end_pack", "fy_2025", NULL, NULL, NULL, &error);
	g_assert_no_error(error);
	pack = venture_report_pack_service_schedule(service, org, "Year end", "0 8 * * *", venture_entity_get_id(saved), NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_report_pack_service_run_due(service, context, org, at, NULL, &error), ==, 1);
	g_assert_no_error(error);
	stored = venture_database_get(db, VENTURE_TYPE_REPORT_PACK, venture_entity_get_id(pack), &error);
	g_object_get(stored, "last-output", &output, NULL);
	g_assert_nonnull(output);
	g_assert_nonnull(strstr(output, "trial_balance.csv"));
	zip = venture_year_end_pack_zip_from_output(output, &error);
#ifdef VENTURE_HAVE_LIBARCHIVE
	{
		g_autoptr(GHashTable) members = NULL;
		g_assert_no_error(error);
		g_assert_nonnull(zip);
		members = zip_members(zip);
		g_assert_true(g_hash_table_contains(members, "index.txt"));
		g_assert_true(g_hash_table_contains(members, "balance_sheet.csv"));
	}
#else
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED);
#endif
	/* A pack whose retained output holds no year-end result is refused,
	 * not answered with an empty archive. */
	g_clear_error(&error);
	g_clear_pointer(&zip, g_bytes_unref);
	zip = venture_year_end_pack_zip_from_output("[{\"title\":\"Other\",\"rows\":[]}]", &error);
	g_assert_null(zip);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
}

/* --- Web surface ---------------------------------------------------------- */

typedef struct
{
	VentureConfig *config;
	VentureDatabase *db;
	VentureContext *context;
	VentureWebServer *server;
	SoupSession *session;
	gchar *state_dir;
	gchar *cookie;
	guint port;
	gint64 org;
	gint64 second;
} ServerFixture;

typedef struct { gboolean done; GBytes *body; GError *error; } RequestResult;

static void
request_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	RequestResult *outcome = user_data;
	outcome->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &outcome->error);
	outcome->done = TRUE;
}

static guint
server_request(ServerFixture *f, const gchar *method, const gchar *path, const gchar *json, gchar **out_body, gchar **out_location)
{
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u%s", f->port, path);
	RequestResult outcome = { FALSE, NULL, NULL };
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (f->cookie != NULL)
		soup_message_headers_append(soup_message_get_request_headers(message), "Cookie", f->cookie);
	if (json != NULL)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(json, strlen(json));
		soup_message_set_request_body_from_bytes(message, "application/json", bytes);
	}
	soup_session_send_and_read_async(f->session, message, G_PRIORITY_DEFAULT, NULL, request_done, &outcome);
	while (!outcome.done)
		g_main_context_iteration(NULL, TRUE);
	if (outcome.error != NULL)
		g_error("%s %s: %s", method, path, outcome.error->message);
	if (out_body != NULL)
		*out_body = g_strndup(g_bytes_get_data(outcome.body, NULL), g_bytes_get_size(outcome.body));
	if (out_location != NULL)
		*out_location = g_strdup(soup_message_headers_get_one(soup_message_get_response_headers(message), "Location"));
	g_clear_pointer(&outcome.body, g_bytes_unref);
	return soup_message_get_status(message);
}

static void
server_login(ServerFixture *f, const gchar *username, const gchar *password)
{
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u/login", f->port);
	g_autofree gchar *form = g_strdup_printf("username=%s&password=%s", username, password);
	g_autoptr(GBytes) bytes = g_bytes_new(form, strlen(form));
	g_autofree gchar *set_cookie = NULL;
	RequestResult outcome = { FALSE, NULL, NULL };
	gchar *semicolon;
	message = soup_message_new("POST", url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", bytes);
	soup_session_send_and_read_async(f->session, message, G_PRIORITY_DEFAULT, NULL, request_done, &outcome);
	while (!outcome.done)
		g_main_context_iteration(NULL, TRUE);
	g_clear_pointer(&outcome.body, g_bytes_unref);
	g_clear_error(&outcome.error);
	set_cookie = g_strdup(soup_message_headers_get_one(soup_message_get_response_headers(message), "Set-Cookie"));
	g_assert_nonnull(set_cookie);
	semicolon = strchr(set_cookie, ';');
	if (semicolon != NULL)
		*semicolon = '\0';
	g_clear_pointer(&f->cookie, g_free);
	f->cookie = g_steal_pointer(&set_cookie);
}

static void
server_set_up(ServerFixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureUser) owner = venture_user_new();
	g_autoptr(VentureEntity) second = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Client B", "slug", "client-b", NULL);
	g_autoptr(GSocketListener) probe = g_socket_listener_new();
	g_setenv("VENTURE_TEST_SESSION_SECRET", "accountant-test-secret", TRUE);
	f->port = g_socket_listener_add_any_inet_port(probe, NULL, &error);
	g_assert_no_error(error);
	g_clear_object(&probe);
	f->state_dir = g_dir_make_tmp("venture-accountant-XXXXXX", NULL);
	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->state_dir, "server-bind-address", "127.0.0.1",
		"server-port", (gint64)f->port, "security-session-secret-env", "VENTURE_TEST_SESSION_SECRET",
		"security-password-iterations", (gint64)100000, NULL);
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->org = venture_context_get_default_organization_id(f->context);
	g_assert_true(venture_database_save(f->db, second, NULL, &error));
	f->second = venture_entity_get_id(second);
	g_object_set(owner, "username", "owner", "role", VENTURE_USER_ROLE_OWNER, "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(owner, "owner-password-1", 100000, NULL));
	g_assert_true(venture_orgaccess_bootstrap_owner(f->db, owner, &error));
	g_assert_no_error(error);
	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
	f->session = soup_session_new();
}

static void
server_tear_down(ServerFixture *f, gconstpointer unused)
{
	if (f->server != NULL)
		venture_web_server_stop(f->server);
	g_clear_pointer(&f->cookie, g_free);
	g_clear_object(&f->session);
	g_clear_object(&f->server);
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
	if (f->state_dir != NULL)
	{
		venture_test_remove_tree(f->state_dir);
		g_clear_pointer(&f->state_dir, g_free);
	}
}

/* The owner invites the accountant with the existing user and membership
 * records over REST -- no new route -- then the accountant signs in and
 * sees only the Books page, in each client organisation they were given. */
static void
test_books_surface(ServerFixture *f, gconstpointer unused)
{
	g_autofree gchar *body = NULL;
	g_autofree gchar *location = NULL;
	g_autofree gchar *membership = NULL;
	g_autoptr(JsonNode) created = NULL;
	g_autoptr(VentureEntity) lead = g_object_new(VENTURE_TYPE_LEAD, "organization-id", f->org, "name", "Secret prospect", NULL);
	g_autoptr(VentureEntity) journal = g_object_new(VENTURE_TYPE_JOURNAL, "organization-id", f->org, "memo", "Year-end accrual", NULL);
	g_autofree gchar *lead_path = NULL;
	g_autofree gchar *journal_path = NULL;
	gint64 user_id;
	guint status;
	g_assert_true(venture_database_save(f->db, lead, NULL, NULL));
	g_assert_true(venture_database_save(f->db, journal, NULL, NULL));
	lead_path = g_strdup_printf("/api/v1/lead/%" G_GINT64_FORMAT, venture_entity_get_id(lead));
	journal_path = g_strdup_printf("/api/v1/journal/%" G_GINT64_FORMAT, venture_entity_get_id(journal));

	server_login(f, "owner", "owner-password-1");
	status = server_request(f, "POST", "/api/v1/user",
		"{\"username\":\"cpa\",\"role\":\"viewer\",\"password\":\"cpa-password-1\",\"display_name\":\"Outside CPA\"}", &body, NULL);
	g_assert_cmpuint(status, ==, 201);
	created = venture_json_parse(body, NULL);
	user_id = json_object_get_int_member(json_node_get_object(created), "id");
	g_assert_cmpint(user_id, >, 0);
	membership = g_strdup_printf("{\"user_id\":%" G_GINT64_FORMAT ",\"organization_id\":%" G_GINT64_FORMAT ",\"role\":\"accountant\",\"active\":true}", user_id, f->org);
	g_clear_pointer(&body, g_free);
	status = server_request(f, "POST", "/api/v1/organization_membership", membership, &body, NULL);
	g_assert_cmpuint(status, ==, 201);
	g_assert_nonnull(strstr(body, "accountant"));
	g_clear_pointer(&membership, g_free);
	membership = g_strdup_printf("{\"user_id\":%" G_GINT64_FORMAT ",\"organization_id\":%" G_GINT64_FORMAT ",\"role\":\"accountant\",\"active\":true}", user_id, f->second);
	g_clear_pointer(&body, g_free);
	status = server_request(f, "POST", "/api/v1/organization_membership", membership, &body, NULL);
	g_assert_cmpuint(status, ==, 201);

	server_login(f, "cpa", "cpa-password-1");
	/* The landing page is Books, and the sidebar offers nothing else. */
	status = server_request(f, "GET", "/", NULL, NULL, &location);
	g_assert_cmpuint(status, ==, 302);
	g_assert_cmpstr(location, ==, "/books");
	g_clear_pointer(&body, g_free);
	status = server_request(f, "GET", "/books", NULL, &body, NULL);
	g_assert_cmpuint(status, ==, 200);
	g_assert_nonnull(strstr(body, "Year-end pack"));
	g_assert_nonnull(strstr(body, "/reports/balance_sheet"));
	g_assert_nonnull(strstr(body, "/reports/trial_balance"));
	g_assert_nonnull(strstr(body, "/books/year-end-pack.zip"));
	g_assert_nonnull(strstr(body, "href=\"/books\""));
	g_assert_null(strstr(body, "href=\"/e/lead\""));
	g_assert_null(strstr(body, "href=\"/e/ticket\""));
	g_assert_null(strstr(body, "href=\"/settings\""));
	g_assert_null(strstr(body, "href=\"/dashboards\""));
	g_assert_null(strstr(body, "href=\"/e/venture\""));
	g_assert_null(strstr(body, "href=\"/accounting\""));
	g_assert_null(strstr(body, "href=\"/inbox\""));
	/* Both clients are in the picker; a switch keeps the accountant on Books. */
	g_assert_nonnull(strstr(body, "Client B"));
	{
		g_autofree gchar *switch_path = g_strdup_printf("/entity/%" G_GINT64_FORMAT "?back=/books", f->second);
		g_clear_pointer(&location, g_free);
		status = server_request(f, "GET", switch_path, NULL, NULL, &location);
		g_assert_cmpuint(status, ==, 302);
		g_assert_cmpstr(location, ==, "/books");
	}
	/* The pack downloads as a zip. */
	g_clear_pointer(&body, g_free);
	status = server_request(f, "GET", "/books/year-end-pack.zip?period=fy_2025", NULL, &body, NULL);
#ifdef VENTURE_HAVE_LIBARCHIVE
	g_assert_cmpuint(status, ==, 200);
	g_assert_cmpmem(body, 2, "PK", 2);
#else
	g_assert_cmpuint(status, ==, 501);
#endif
	/* CRM is not there: the list is empty and a record is not found. */
	g_clear_pointer(&body, g_free);
	status = server_request(f, "GET", "/api/v1/lead", NULL, &body, NULL);
	g_assert_cmpuint(status, ==, 200);
	g_assert_null(strstr(body, "Secret prospect"));
	status = server_request(f, "GET", lead_path, NULL, NULL, NULL);
	g_assert_cmpuint(status, ==, 404);
	/* The books are readable and exportable, but not writable. */
	g_clear_pointer(&body, g_free);
	status = server_request(f, "GET", journal_path, NULL, &body, NULL);
	g_assert_cmpuint(status, ==, 200);
	g_assert_nonnull(strstr(body, "Year-end accrual"));
	status = server_request(f, "GET", "/api/v1/reports/trial_balance?period=fy_2025&format=csv", NULL, NULL, NULL);
	g_assert_cmpuint(status, ==, 200);
	/* As a global viewer the write falls at the existing global role check
	 * first, which is a refusal too -- but not the one that names the
	 * membership role. */
	g_clear_pointer(&body, g_free);
	status = server_request(f, "PATCH", journal_path, "{\"memo\":\"Tampered\"}", &body, NULL);
	g_assert_cmpuint(status, ==, 403);
	/* A promotion of the global role changes nothing: the accountant
	 * membership still refuses every write, and now says so by name. */
	server_login(f, "owner", "owner-password-1");
	{
		g_autofree gchar *user_path = g_strdup_printf("/api/v1/user/%" G_GINT64_FORMAT, user_id);
		status = server_request(f, "PATCH", user_path, "{\"role\":\"editor\"}", NULL, NULL);
		g_assert_cmpuint(status, ==, 200);
	}
	server_login(f, "cpa", "cpa-password-1");
	g_clear_pointer(&body, g_free);
	status = server_request(f, "PATCH", journal_path, "{\"memo\":\"Tampered\"}", &body, NULL);
	g_assert_cmpuint(status, ==, 403);
	g_assert_nonnull(strstr(body, "accountant"));
	g_clear_pointer(&body, g_free);
	status = server_request(f, "POST", "/api/v1/journal", "{\"memo\":\"Planted\"}", &body, NULL);
	g_assert_cmpuint(status, ==, 403);
	g_assert_nonnull(strstr(body, "accountant"));
	g_clear_pointer(&body, g_free);
	status = server_request(f, "DELETE", journal_path, NULL, &body, NULL);
	g_assert_cmpuint(status, ==, 403);
	g_assert_nonnull(strstr(body, "accountant"));
	/* Still no CRM, and still on Books. */
	status = server_request(f, "GET", lead_path, NULL, NULL, NULL);
	g_assert_cmpuint(status, ==, 404);
	g_clear_pointer(&location, g_free);
	status = server_request(f, "GET", "/", NULL, NULL, &location);
	g_assert_cmpuint(status, ==, 302);
	g_assert_cmpstr(location, ==, "/books");
	{
		g_autoptr(VentureEntity) untouched = venture_database_get(f->db, VENTURE_TYPE_JOURNAL, venture_entity_get_id(journal), NULL);
		g_autofree gchar *memo = NULL;
		g_assert_nonnull(untouched);
		g_object_get(untouched, "memo", &memo, NULL);
		g_assert_cmpstr(memo, ==, "Year-end accrual");
	}
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/accountant/role-nick", PolicyFixture, NULL, policy_set_up, test_role_nick, policy_tear_down);
	g_test_add("/accountant/books-read-only", PolicyFixture, NULL, policy_set_up, test_books_read_only, policy_tear_down);
	g_test_add("/accountant/hidden-records", PolicyFixture, NULL, policy_set_up, test_hidden_records, policy_tear_down);
	g_test_add("/accountant/financial-documents", PolicyFixture, NULL, policy_set_up, test_financial_documents, policy_tear_down);
	g_test_add("/accountant/repository-refuses-writes", PolicyFixture, NULL, policy_set_up, test_repository_refuses_writes, policy_tear_down);
	g_test_add("/accountant/many-organisations", PolicyFixture, NULL, policy_set_up, test_many_organisations, policy_tear_down);
	g_test_add("/accountant/token-snapshot", PolicyFixture, NULL, policy_set_up, test_token_snapshot, policy_tear_down);
	g_test_add("/accountant/accountant-only", PolicyFixture, NULL, policy_set_up, test_accountant_only, policy_tear_down);
	g_test_add_func("/accountant/year-end-pack", test_year_end_pack);
	g_test_add_func("/accountant/year-end-pack-module-off", test_year_end_pack_module_off);
	g_test_add_func("/accountant/year-end-pack-scheduled", test_year_end_pack_scheduled);
	g_test_add("/accountant/books-surface", ServerFixture, NULL, server_set_up, test_books_surface, server_tear_down);
	return g_test_run();
}
