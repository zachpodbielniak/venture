/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include "venture-test-util.h"
#include "venture-test-accounting.h"

typedef struct
{
	VentureDatabase *db;
	VentureContext *context;
	VentureConfig *config;
	gint64 org;
	gint64 period;
} Fixture;

static void
save(Fixture *f, VentureEntity *e)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, e, NULL, &error));
	g_assert_no_error(error);
}

static VentureActor
actor_named(const gchar *name)
{
	VentureActor actor;
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = name;
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	return actor;
}

static void
setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) year = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) period = NULL;
	g_autoptr(VentureUser) owner = venture_user_new();
	f->config = venture_config_new();
	f->db = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	g_object_set(owner, "username", "owner", "role", VENTURE_USER_ROLE_OWNER, "active", TRUE, NULL);
	save(f, VENTURE_ENTITY(owner));
	year = g_object_new(VENTURE_TYPE_FISCAL_YEAR, "organization-id", f->org, "name", "2026", NULL);
	g_assert_true(venture_entity_set_field_from_string(year, "start-at", "2026-01-01", &error));
	save(f, year);
	query = venture_query_new(VENTURE_TYPE_FISCAL_PERIOD);
	venture_query_set_organization(query, f->org);
	venture_query_add_order(query, "start-at", VENTURE_SORT_ASCENDING, NULL);
	period = venture_database_find_one(f->db, query, &error);
	g_assert_no_error(error);
	f->period = venture_entity_get_id(period);
}

static void
teardown(Fixture *f, gconstpointer unused)
{
	g_clear_object(&f->context);
	venture_test_accounting_database_cleanup(f->db);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static void
test_records(void)
{
	static const gchar *const names[] = {
		"close_workspace", "close_task", "close_workpaper",
		"close_discrepancy", "close_signoff"
	};
	guint i;
	for (i = 0; i < G_N_ELEMENTS(names); i++)
		g_assert_cmpuint(venture_entity_registry_lookup(
			venture_entity_registry_get_default(), names[i]), !=, G_TYPE_INVALID);
}

static gint64
count_type(Fixture *f, const gchar *name)
{
	g_autoptr(VentureQuery) q = venture_query_new(
		venture_entity_registry_lookup(venture_entity_registry_get_default(), name));
	venture_query_set_organization(q, f->org);
	return venture_database_count(f->db, q, NULL);
}

static void
test_open_and_checklist(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	VentureActor actor = actor_named("closer");
	g_autoptr(VentureEntity) workspace = NULL;
	g_autoptr(VentureQuery) q = NULL;
	g_autoptr(GPtrArray) tasks = NULL;
	workspace = venture_close_service_open(venture_close_service_get(f->db),
		f->period, "USD", &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(workspace);
	g_assert_cmpint(count_type(f, "close_workspace"), ==, 1);
	/* Nine checklist items, each with a preparer and a reviewer task. */
	g_assert_cmpint(count_type(f, "close_task"), ==, 18);
	q = venture_query_new(VENTURE_TYPE_CLOSE_TASK);
	venture_query_set_organization(q, f->org);
	tasks = venture_database_find(f->db, q, &error);
	g_assert_cmpuint(tasks->len, ==, 18);
	{
		g_autoptr(VentureEntity) again = venture_close_service_open(
			venture_close_service_get(f->db), f->period, "USD", &actor, &error);
		g_assert_null(again);
		g_assert_nonnull(error);
	}
}

static void
test_empty_books_tie_out(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	VentureActor actor = actor_named("closer");
	g_autoptr(VentureEntity) workspace = venture_close_service_open(
		venture_close_service_get(f->db), f->period, "USD", &actor, &error);
	g_assert_true(venture_close_service_run_checks(venture_close_service_get(f->db),
		workspace, &actor, &error));
	g_assert_no_error(error);
	{
		gboolean tb = FALSE;
		gboolean sub = FALSE;
		g_object_get(workspace, "tb-balanced", &tb, "subledger-tied", &sub, NULL);
		g_assert_true(tb);
		g_assert_true(sub);
	}
	g_assert_cmpint(count_type(f, "close_discrepancy"), ==, 0);
}

static void
test_signoff_and_complete(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	VentureActor closer = actor_named("closer");
	VentureActor owner = actor_named("owner");
	g_autoptr(VentureEntity) workspace = NULL;
	g_autoptr(VentureQuery) q = NULL;
	g_autoptr(GPtrArray) tasks = NULL;
	g_autoptr(JsonNode) pack = NULL;
	g_autofree gchar *status = NULL;
	gint state = VENTURE_PERIOD_OPEN;
	guint i;
	workspace = venture_close_service_open(venture_close_service_get(f->db),
		f->period, "USD", &closer, &error);
	g_assert_true(venture_close_service_run_checks(venture_close_service_get(f->db),
		workspace, &closer, &error));
	q = venture_query_new(VENTURE_TYPE_CLOSE_TASK);
	venture_query_set_organization(q, f->org);
	tasks = venture_database_find(f->db, q, &error);
	for (i = 0; i < tasks->len; i++)
		g_assert_true(venture_close_service_complete_task(venture_close_service_get(f->db),
			g_ptr_array_index(tasks, i), FALSE, "tied", &closer, &error));
	g_assert_false(venture_close_service_complete(venture_close_service_get(f->db),
		workspace, &closer, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_true(venture_close_service_sign(venture_close_service_get(f->db),
		workspace, "preparer", &closer, &error));
	g_assert_no_error(error);
	g_assert_false(venture_close_service_sign(venture_close_service_get(f->db),
		workspace, "reviewer", &closer, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	g_clear_error(&error);
	g_assert_true(venture_close_service_sign(venture_close_service_get(f->db),
		workspace, "reviewer", &owner, &error));
	g_assert_no_error(error);
	pack = venture_close_service_pack(venture_close_service_get(f->db), workspace, &error);
	g_assert_no_error(error);
	g_assert_true(JSON_NODE_HOLDS_OBJECT(pack));
	g_assert_true(json_object_has_member(json_node_get_object(pack), "trial_balance"));
	g_assert_true(venture_close_service_complete(venture_close_service_get(f->db),
		workspace, &closer, &error));
	g_assert_no_error(error);
	{
		g_autoptr(VentureEntity) stored = venture_database_get(f->db,
			VENTURE_TYPE_CLOSE_WORKSPACE, venture_entity_get_id(workspace), NULL);
		g_object_get(stored, "status", &status, NULL);
		g_assert_cmpstr(status, ==, "completed");
	}
	{
		g_autoptr(VentureEntity) period = venture_database_get(f->db,
			VENTURE_TYPE_FISCAL_PERIOD, f->period, NULL);
		g_object_get(period, "state", &state, NULL);
		g_assert_cmpint(state, ==, VENTURE_PERIOD_CLOSED);
	}
	g_assert_true(venture_close_service_reopen(venture_close_service_get(f->db),
		workspace, &owner, &error));
	g_assert_no_error(error);
	/* Historical signatures remain evidence, not approval of a reopened cycle. */
	g_assert_false(venture_close_service_sign(venture_close_service_get(f->db),
		workspace, "reviewer", &owner, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_assert_true(venture_close_service_run_checks(venture_close_service_get(f->db),
		workspace, &closer, &error));
	g_assert_no_error(error);
	g_assert_false(venture_close_service_complete(venture_close_service_get(f->db),
		workspace, &closer, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_assert_true(venture_close_service_sign(venture_close_service_get(f->db),
		workspace, "preparer", &closer, &error));
	g_assert_true(venture_close_service_sign(venture_close_service_get(f->db),
		workspace, "reviewer", &owner, &error));
	g_assert_true(venture_close_service_complete(venture_close_service_get(f->db),
		workspace, &closer, &error));
	g_assert_no_error(error);
}

static void
test_unmatched_bank_blocks(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	VentureActor actor = actor_named("closer");
	g_autoptr(VentureEntity) workspace = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ACCOUNT);
	g_autoptr(GPtrArray) accounts = NULL;
	g_autoptr(VentureEntity) bank = VENTURE_ENTITY(venture_bank_account_new());
	g_autoptr(JsonObject) args = json_object_new();
	g_autoptr(VentureEntity) imported = NULL;
	venture_query_set_organization(query, f->org);
	g_assert_true(venture_query_add_filter_string(query, "code", VENTURE_FILTER_OP_EQ, "1000", &error));
	accounts = venture_database_find(f->db, query, &error);
	g_assert_cmpuint(accounts->len, ==, 1);
	venture_entity_set_organization_id(bank, f->org);
	g_object_set(bank, "name", "Checking", "currency", "USD",
		"account-id", venture_entity_get_id(g_ptr_array_index(accounts, 0)),
		"date-column", "date", "amount-column", "amount", "description-column", "memo",
		"reference-column", "ref", "external-id-column", "id", "date-format", "%Y-%m-%d",
		"sign-convention", "normal", NULL);
	save(f, bank);
	json_object_set_string_member(args, "format", "csv");
	json_object_set_string_member(args, "data", "id,date,amount,memo,ref\n1,2026-01-15,10.00,Rent,r1\n");
	json_object_set_string_member(args, "period_start", "2026-01-01");
	json_object_set_string_member(args, "period_end", "2026-01-31");
	json_object_set_string_member(args, "opening_balance", "0 USD");
	json_object_set_string_member(args, "closing_balance", "10 USD");
	imported = venture_bank_match_service_execute(venture_database_get_bank_match_service(f->db),
		"import", venture_entity_get_id(bank), args, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(imported);
	workspace = venture_close_service_open(venture_close_service_get(f->db),
		f->period, "USD", &actor, &error);
	g_assert_false(venture_close_service_run_checks(venture_close_service_get(f->db),
		workspace, &actor, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "bank"));
	g_assert_cmpint(count_type(f, "close_discrepancy"), >, 0);
	/* Flags from an earlier run cannot override accounting that now fails. */
	g_clear_error(&error);
	g_object_set(workspace, "tb-balanced", TRUE, "subledger-tied", TRUE, "status", "signed_off", NULL);
	g_assert_false(venture_close_service_complete(venture_close_service_get(f->db), workspace, &actor, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "bank"));
}

static void
test_generic_signoff_refused(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	VentureActor actor = actor_named("closer");
	g_autoptr(VentureEntity) workspace = venture_close_service_open(
		venture_close_service_get(f->db), f->period, "USD", &actor, &error);
	g_autoptr(VentureEntity) sign = g_object_new(VENTURE_TYPE_CLOSE_SIGNOFF,
		"organization-id", f->org, "workspace-id", venture_entity_get_id(workspace),
		"role", "preparer", "actor", "sneak", NULL);
	g_assert_true(venture_entity_set_field_from_string(sign, "signed-at", "2026-01-31", &error));
	g_assert_false(venture_database_save(f->db, sign, &actor, &error));
	g_clear_error(&error);
	/* Even the editable-looking preparing state cannot mint check results. */
	g_object_set(workspace, "status", "preparing", "tb-balanced", TRUE, "subledger-tied", TRUE, NULL);
	g_assert_false(venture_database_save(f->db, workspace, &actor, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "VentureCloseService"));
}

static void
test_module_off(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	VentureActor actor = actor_named("closer");
	g_autoptr(VentureEntity) workspace = NULL;
	venture_config_set_module_enabled(f->config, "close", FALSE);
	workspace = venture_close_service_open(venture_close_service_get(f->db),
		f->period, "USD", &actor, &error);
	g_assert_null(workspace);
	g_assert_nonnull(error);
	venture_config_set_module_enabled(f->config, "close", TRUE);
}

static void
test_tax_control_is_sales_tax(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	VentureActor actor = actor_named("closer");
	g_autoptr(VentureEntity) workspace = venture_close_service_open(
		venture_close_service_get(f->db), f->period, "USD", &actor, &error);
	g_autoptr(VentureQuery) q = NULL;
	g_autoptr(GPtrArray) tasks = NULL;
	gboolean saw = FALSE;
	guint i;

	(void)unused;
	g_assert_true(venture_close_service_run_checks(venture_close_service_get(f->db),
		workspace, &actor, &error));
	g_assert_no_error(error);
	q = venture_query_new(VENTURE_TYPE_CLOSE_TASK);
	venture_query_set_organization(q, f->org);
	tasks = venture_database_find(f->db, q, &error);
	g_assert_nonnull(tasks);
	for (i = 0; i < tasks->len; i++)
	{
		g_autofree gchar *kind = NULL;
		g_autofree gchar *notes = NULL;
		g_object_get(g_ptr_array_index(tasks, i), "kind", &kind, "notes", &notes, NULL);
		if (g_strcmp0(kind, "tax") != 0)
			continue;
		saw = TRUE;
		g_assert_nonnull(notes);
		g_assert_nonnull(strstr(notes, "2100"));
		g_assert_null(strstr(notes, "2200"));
	}
	g_assert_true(saw);
}

static void
test_tax_control_uses_mapped_account(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	VentureActor actor = actor_named("closer");
	g_autoptr(VentureAccount) tax = venture_account_new();
	g_autoptr(VentureEntity) map = NULL;
	g_autoptr(VentureEntity) workspace = NULL;
	g_autoptr(VentureQuery) q = NULL;
	g_autoptr(GPtrArray) tasks = NULL;
	gboolean saw = FALSE;
	guint i;

	(void)unused;
	venture_entity_set_organization_id(VENTURE_ENTITY(tax), f->org);
	g_object_set(tax, "code", "2150", "name", "VAT payable",
		"kind", VENTURE_ACCOUNT_KIND_LIABILITY, "active", TRUE, NULL);
	save(f, VENTURE_ENTITY(tax));
	q = venture_query_new(VENTURE_TYPE_ACCOUNTING_CONTROL_MAP);
	venture_query_set_organization(q, f->org);
	g_assert_true(venture_query_add_filter_string(q, "classification", VENTURE_FILTER_OP_EQ, "tax", NULL));
	map = venture_database_find_one(f->db, q, NULL);
	if (map == NULL)
	{
		map = g_object_new(VENTURE_TYPE_ACCOUNTING_CONTROL_MAP,
			"organization-id", f->org, "classification", "tax",
			"subject-type", "organization", "subject-id", (gint64)0,
			"account-id", venture_entity_get_id(VENTURE_ENTITY(tax)), NULL);
	}
	else
		g_object_set(map, "account-id", venture_entity_get_id(VENTURE_ENTITY(tax)), NULL);
	save(f, map);
	g_clear_object(&q);
	workspace = venture_close_service_open(venture_close_service_get(f->db),
		f->period, NULL, &actor, &error);
	g_assert_no_error(error);
	{
		g_autofree gchar *currency = NULL;
		g_object_get(workspace, "currency", &currency, NULL);
		g_assert_cmpstr(currency, ==, "USD");
	}
	g_assert_true(venture_close_service_run_checks(venture_close_service_get(f->db),
		workspace, &actor, &error));
	g_assert_no_error(error);
	q = venture_query_new(VENTURE_TYPE_CLOSE_TASK);
	venture_query_set_organization(q, f->org);
	tasks = venture_database_find(f->db, q, &error);
	for (i = 0; i < tasks->len; i++)
	{
		g_autofree gchar *kind = NULL;
		g_autofree gchar *notes = NULL;
		g_object_get(g_ptr_array_index(tasks, i), "kind", &kind, "notes", &notes, NULL);
		if (g_strcmp0(kind, "tax") != 0)
			continue;
		saw = TRUE;
		g_assert_nonnull(strstr(notes, "2150"));
		g_assert_null(strstr(notes, "2100"));
	}
	g_assert_true(saw);
}

static void
test_direct_period_close_still_runs_checks(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	VentureActor actor = actor_named("closer");
	g_autoptr(VentureEntity) period = venture_database_get(f->db,
		VENTURE_TYPE_FISCAL_PERIOD, f->period, NULL);
	/* Empty books: the extra subledger checks must not block a close. */
	g_object_set(period, "state", VENTURE_PERIOD_CLOSED, NULL);
	g_assert_true(venture_database_save(f->db, period, &actor, &error));
	g_assert_no_error(error);
}

static VentureEntity *
close_action(Fixture *f, const gchar *type, gint64 id, const gchar *name,
	const gchar *json, const VentureActor *actor, VentureUserRole role, GError **error)
{
	g_autoptr(JsonNode) node = venture_json_parse(json, error);
	g_autoptr(GHashTable) parameters = node ? venture_action_parameters_from_json(node, error) : NULL;
	return parameters ? venture_action_registry_perform(venture_database_get_action_registry(f->db),
		type, id, name, parameters, actor, role, error) : NULL;
}

/* The generated page/API must be able to complete the same checklist as an
 * internal caller. Generic writes deliberately refuse these transitions. */
static void
test_generated_actions(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) workspace = NULL, result = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_CLOSE_TASK);
	g_autoptr(GPtrArray) tasks = NULL;
	g_autoptr(VentureUser) reviewer = venture_user_new();
	VentureActor owner = actor_named("owner"), reviewing = actor_named("reviewer");
	VentureAction *action = venture_action_registry_lookup(venture_database_get_action_registry(f->db), "close_task", "complete");
	gboolean stageable = TRUE;
	guint i;
	g_assert_nonnull(action);
	g_object_get(action, "stageable", &stageable, NULL); g_assert_false(stageable);
	workspace = close_action(f, "fiscal_period", f->period, "open_close", "{\"currency\":\"USD\"}", &owner, VENTURE_USER_ROLE_EDITOR, &error);
	g_assert_no_error(error); g_assert_nonnull(workspace);
	venture_query_set_organization(query, f->org);
	tasks = venture_database_find(f->db, query, &error); g_assert_no_error(error);
	g_assert_cmpuint(tasks->len, ==, 18);
	result = close_action(f, "close_task", venture_entity_get_id(g_ptr_array_index(tasks, 0)), "complete",
		"{\"notes\":\"Reviewed empty synthetic books\"}", &owner, VENTURE_USER_ROLE_VIEWER, &error);
	g_assert_null(result); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	for (i = 0; i < tasks->len; i++) {
		result = close_action(f, "close_task", venture_entity_get_id(g_ptr_array_index(tasks, i)), "complete",
			"{\"notes\":\"Reviewed empty synthetic books\"}", &owner, VENTURE_USER_ROLE_EDITOR, &error);
		g_assert_no_error(error); g_assert_nonnull(result); g_clear_object(&result);
	}
	result = close_action(f, "close_workspace", venture_entity_get_id(workspace), "run_checks", "{}", &owner, VENTURE_USER_ROLE_EDITOR, &error);
	g_assert_no_error(error); g_assert_nonnull(result); g_clear_object(&result);
	result = close_action(f, "close_workspace", venture_entity_get_id(workspace), "sign", "{\"role\":\"preparer\"}", &owner, VENTURE_USER_ROLE_EDITOR, &error);
	g_assert_no_error(error); g_assert_nonnull(result); g_clear_object(&result);
	result = close_action(f, "close_workspace", venture_entity_get_id(workspace), "sign", "{\"role\":\"reviewer\"}", &owner, VENTURE_USER_ROLE_EDITOR, &error);
	g_assert_null(result); g_assert_nonnull(error); g_clear_error(&error);
	g_object_set(reviewer, "username", "reviewer", "role", VENTURE_USER_ROLE_OWNER, "active", TRUE, NULL); save(f, VENTURE_ENTITY(reviewer));
	result = close_action(f, "close_workspace", venture_entity_get_id(workspace), "sign", "{\"role\":\"reviewer\"}", &reviewing, VENTURE_USER_ROLE_EDITOR, &error);
	g_assert_no_error(error); g_assert_nonnull(result); g_clear_object(&result);
	result = close_action(f, "close_workspace", venture_entity_get_id(workspace), "complete", "{}", &owner, VENTURE_USER_ROLE_EDITOR, &error);
	g_assert_no_error(error); g_assert_nonnull(result); g_clear_object(&result);
	result = close_action(f, "close_task", venture_entity_get_id(g_ptr_array_index(tasks, 0)), "waive",
		"{\"notes\":\"Must not change closed evidence\"}", &owner, VENTURE_USER_ROLE_EDITOR, &error);
	g_assert_null(result); g_assert_nonnull(error); g_clear_error(&error);
	venture_config_set_module_enabled(f->config, "close", FALSE);
	result = close_action(f, "fiscal_period", f->period, "open_close", "{\"currency\":\"USD\"}", &owner, VENTURE_USER_ROLE_EDITOR, &error);
	g_assert_null(result); g_assert_nonnull(error); g_clear_error(&error);
	venture_config_set_module_enabled(f->config, "close", TRUE);
}

/* Editor access alone cannot authorize financial signoff; revoking a
 * finance membership must also revoke the generated action immediately. */
static void
test_action_authority(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) user = g_object_new(VENTURE_TYPE_USER,
		"username", "close-finance", "role", VENTURE_USER_ROLE_EDITOR, "active", TRUE, NULL);
	g_autoptr(VentureEntity) member = NULL, workspace = NULL, result = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	VentureAuthPrincipal principal;
	VentureActor actor = actor_named("close-finance");
	(void)unused;
	save(f, user);
	member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP,
		"organization-id", f->org, "user-id", venture_entity_get_id(user),
		"role", VENTURE_ORGANIZATION_ROLE_EDITOR, "active", TRUE, NULL);
	save(f, member);
	principal.user_id = venture_entity_get_id(user);
	principal.token_id = 0;
	principal.name = (gchar *)"close-finance";
	principal.role = VENTURE_USER_ROLE_EDITOR;
	principal.authenticated = TRUE;
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &principal);
	result = close_action(f, "fiscal_period", f->period, "open_close", "{}", &actor, principal.role, &error);
	g_assert_null(result); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	g_clear_object(&scope);
	g_object_set(member, "role", VENTURE_ORGANIZATION_ROLE_FINANCE, NULL); save(f, member);
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &principal);
	result = close_action(f, "fiscal_period", f->period, "open_close", "{\"currency\":\"not-a-currency\"}", &actor, principal.role, &error);
	g_assert_null(result); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	workspace = close_action(f, "fiscal_period", f->period, "open_close", "{}", &actor, principal.role, &error);
	g_assert_no_error(error); g_assert_nonnull(workspace);
	result = close_action(f, "close_workspace", venture_entity_get_id(workspace), "run_checks", "{}", &actor, principal.role, &error);
	g_assert_no_error(error); g_assert_nonnull(result); g_clear_object(&result);
	g_clear_object(&scope);
	g_object_set(member, "active", FALSE, NULL); save(f, member);
	scope = venture_access_policy_enter(venture_database_get_access_policy(f->db), &principal);
	/* A second open on the same period is refused for every caller, so the
	 * revocation is proven on an action that succeeded a moment ago. */
	result = close_action(f, "close_workspace", venture_entity_get_id(workspace), "run_checks", "{}", &actor, principal.role, &error);
	g_assert_null(result); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
	result = close_action(f, "fiscal_period", f->period, "open_close", "{}", &actor, principal.role, &error);
	g_assert_null(result); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED); g_clear_error(&error);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	venture_entity_registry_register_builtins(venture_entity_registry_get_default());
	g_test_add_func("/close/records", test_records);
	g_test_add("/close/action-authority", Fixture, NULL, setup, test_action_authority, teardown);
	g_test_add("/close/generated-actions", Fixture, NULL, setup, test_generated_actions, teardown);
	g_test_add("/close/open", Fixture, NULL, setup, test_open_and_checklist, teardown);
	g_test_add("/close/empty-tie-out", Fixture, NULL, setup, test_empty_books_tie_out, teardown);
	g_test_add("/close/signoff-complete", Fixture, NULL, setup, test_signoff_and_complete, teardown);
	g_test_add("/close/unmatched-bank", Fixture, NULL, setup, test_unmatched_bank_blocks, teardown);
	g_test_add("/close/generic-signoff", Fixture, NULL, setup, test_generic_signoff_refused, teardown);
	g_test_add("/close/module-off", Fixture, NULL, setup, test_module_off, teardown);
	g_test_add("/close/tax-control", Fixture, NULL, setup, test_tax_control_is_sales_tax, teardown);
	g_test_add("/close/mapped-tax", Fixture, NULL, setup, test_tax_control_uses_mapped_account, teardown);
	g_test_add("/close/period-close", Fixture, NULL, setup, test_direct_period_close_still_runs_checks, teardown);
	return g_test_run();
}
