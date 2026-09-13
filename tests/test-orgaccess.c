/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

/* Access records must participate in the generated model, not a private
 * authorization table that the forms and CLI cannot manage. */
static void
test_records(void)
{
	VentureEntityRegistry *registry;
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureEntity) user = NULL;
	g_autoptr(VentureEntity) membership = NULL;
	g_autoptr(VentureEntity) duplicate = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(VentureEntity) org = NULL;
	g_autoptr(GError) error = NULL;
	GType type;
	GType owned[5];
	guint i;

	registry = venture_entity_registry_get_default();
	type = venture_entity_registry_lookup(registry, "organization_membership");
	g_assert_cmpuint(type, !=, G_TYPE_INVALID);
	g_assert_cmpuint(venture_entity_registry_lookup(registry, "team"), !=, G_TYPE_INVALID);
	g_assert_cmpuint(venture_entity_registry_lookup(registry, "team_membership"), !=, G_TYPE_INVALID);
	owned[0] = VENTURE_TYPE_COMPANY;
	owned[1] = VENTURE_TYPE_CONTACT;
	owned[2] = VENTURE_TYPE_DEAL;
	owned[3] = VENTURE_TYPE_TICKET;
	owned[4] = VENTURE_TYPE_VENTURE;
	for (i = 0; i < G_N_ELEMENTS(owned); i++)
	{
		g_autoptr(VentureEntity) entity = g_object_new(owned[i], NULL);
		g_assert_nonnull(g_object_class_find_property(G_OBJECT_GET_CLASS(entity), "owner-user-id"));
		g_assert_nonnull(g_object_class_find_property(G_OBJECT_GET_CLASS(entity), "team-id"));
	}
	db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(db, registry, &error));
	g_assert_no_error(error);
	query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
	org = venture_database_find_one(db, query, &error);
	g_assert_nonnull(org);
	user = g_object_new(VENTURE_TYPE_USER, "username", "member", "active", TRUE, NULL);
	g_assert_true(venture_database_save(db, user, NULL, &error));
	membership = g_object_new(type, "user-id", venture_entity_get_id(user),
		"organization-id", venture_entity_get_id(org), "active", TRUE, NULL);
	g_assert_true(venture_database_save(db, membership, NULL, &error));
	g_assert_no_error(error);
	duplicate = g_object_new(type, "user-id", venture_entity_get_id(user),
		"organization-id", venture_entity_get_id(org), "active", TRUE, NULL);
	g_assert_false(venture_database_save(db, duplicate, NULL, &error));
	g_assert_nonnull(error);
}

static void
test_module(void)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureContext) context = venture_context_new(config, db);
	g_assert_true(venture_context_module_enabled(context, "orgaccess"));
	venture_config_set_module_enabled(config, "orgaccess", FALSE);
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), "team"), ==, G_TYPE_INVALID);
	venture_config_set_module_enabled(config, "orgaccess", TRUE);
}

/* Build only the two historical tables: this fixture has no access schema
 * or migration history and contains two legal entities before upgrade. */
static void
test_upgrade(void)
{
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	GType type;
	guint i;
	g_assert_true(venture_schema_create_table(venture_database_get_connection(db), VENTURE_TYPE_USER, &error));
	g_assert_true(venture_schema_create_table(venture_database_get_connection(db), VENTURE_TYPE_ORGANIZATION, &error));
	g_assert_true(venture_database_execute(db,
		"INSERT INTO users (id, uuid, username, role, active) VALUES (17, 'old-owner', 'old-owner', 'owner', 1), (18, 'old-editor', 'old-editor', 'editor', 1);"
		"INSERT INTO organizations (id, uuid, name, slug, active) VALUES (31, 'old-a', 'Entity A', 'a', 1), (32, 'old-b', 'Entity B', 'b', 1)", NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	type = venture_entity_registry_lookup(venture_entity_registry_get_default(), "organization_membership");
	query = venture_query_new(type);
	rows = venture_database_find(db, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(rows->len, ==, 2);
	for (i = 0; i < rows->len; i++)
	{
		gint64 user_id;
		gboolean active;
		gint role;
		VentureEntity *row = g_ptr_array_index(rows, i);
		g_object_get(row, "user-id", &user_id, "active", &active, NULL);
		g_object_get(row, "role", &role, NULL);
		g_assert_cmpint(user_id, ==, 17);
		g_assert_true(active);
		g_assert_cmpint(role, ==, VENTURE_ORGANIZATION_ROLE_OWNER);
	}
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_cmpint(venture_database_count(db, query, &error), ==, 2);
	g_assert_no_error(error);
}

static void
test_bootstrap(void)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureAuth) auth = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *password = NULL;
	g_setenv("ORGACCESS_TEST_SECRET", "test-only-secret", TRUE);
	g_object_set(config, "security-session-secret-env", "ORGACCESS_TEST_SECRET", "security-password-iterations", (gint64)100000, NULL);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	context = venture_context_new(config, db);
	auth = venture_auth_new(context);
	password = venture_auth_ensure_owner(auth, "test-password", &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_database_count(db, query, &error), ==, 1);
	g_unsetenv("ORGACCESS_TEST_SECRET");
}

static void
test_token_scope(void)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureEntity) user = NULL;
	g_autoptr(VentureEntity) org = NULL;
	g_autoptr(VentureEntity) member = NULL;
	g_autoptr(VentureEntity) company = NULL;
	g_autoptr(VentureApiToken) token = venture_api_token_new();
	g_autofree gchar *secret = NULL;
	VentureAuthPrincipal actor;
	gchar actor_name[] = "token";
	gint64 first;
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), NULL));
	context = venture_context_new(config, db);
	first = venture_context_get_default_organization_id(context);
	user = g_object_new(VENTURE_TYPE_USER, "username", "token-minter", "role", VENTURE_USER_ROLE_EDITOR, "active", TRUE, NULL);
	g_assert_true(venture_database_save(db, user, NULL, NULL));
	member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "organization-id", first,
		"user-id", venture_entity_get_id(user), "active", TRUE, "role", VENTURE_ORGANIZATION_ROLE_EDITOR, NULL);
	g_assert_true(venture_database_save(db, member, NULL, NULL));
	g_object_set(token, "name", "limited", "user-id", venture_entity_get_id(user), "role", VENTURE_USER_ROLE_EDITOR, NULL);
	secret = venture_api_token_generate(token);
	g_assert_true(venture_database_save(db, VENTURE_ENTITY(token), NULL, NULL));
	org = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Later grant", "slug", "later", NULL);
	g_assert_true(venture_database_save(db, org, NULL, NULL));
	g_clear_object(&member);
	member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "organization-id", venture_entity_get_id(org),
		"user-id", venture_entity_get_id(user), "active", TRUE, "role", VENTURE_ORGANIZATION_ROLE_EDITOR, NULL);
	g_assert_true(venture_database_save(db, member, NULL, NULL));
	company = g_object_new(VENTURE_TYPE_COMPANY, "name", "Later data", "organization-id", venture_entity_get_id(org), NULL);
	actor.user_id = venture_entity_get_id(user);
	actor.token_id = venture_entity_get_id(VENTURE_ENTITY(token));
	actor.role = VENTURE_USER_ROLE_EDITOR;
	actor.name = actor_name;
	actor.authenticated = TRUE;
	g_assert_false(venture_access_policy_can(venture_database_get_access_policy(db), &actor, "read", company, NULL));
	venture_entity_set_organization_id(company, first);
	g_assert_true(venture_access_policy_can(venture_database_get_access_policy(db), &actor, "read", company, NULL));
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP);
		g_autoptr(VentureEntity) original_member = NULL;
		g_autoptr(VentureEntity) journal = g_object_new(VENTURE_TYPE_JOURNAL, "organization-id", first, NULL);
		g_autoptr(GError) error = NULL;
		venture_query_add_filter_int(query, "organization-id", VENTURE_FILTER_OP_EQ, first, NULL);
		original_member = venture_database_find_one(db, query, NULL);
		g_object_set(original_member, "role", VENTURE_ORGANIZATION_ROLE_VIEWER, NULL);
		g_assert_true(venture_database_save(db, original_member, NULL, NULL));
		g_clear_object(&token);
		token = venture_api_token_new();
		g_object_set(token, "name", "viewer-token", "user-id", actor.user_id, "role", VENTURE_USER_ROLE_EDITOR, NULL);
		g_clear_pointer(&secret, g_free);
		secret = venture_api_token_generate(token);
		g_assert_true(venture_database_save(db, VENTURE_ENTITY(token), NULL, NULL));
		actor.token_id = venture_entity_get_id(VENTURE_ENTITY(token));
		g_object_set(original_member, "role", VENTURE_ORGANIZATION_ROLE_EDITOR, NULL);
		g_assert_true(venture_database_save(db, original_member, NULL, NULL));
		g_assert_false(venture_access_policy_requires_approval(venture_database_get_access_policy(db), &actor, "post", journal, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED);
	}

}

static void
test_ai_boundary(void)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureAiService) ai = NULL;
	g_autoptr(VentureEntity) user = NULL;
	g_autoptr(VentureEntity) company = NULL;
	g_autoptr(AiToolUse) tool = ai_tool_use_new_from_json_string("probe", "venture_query", "{\"type\":\"company\"}");
	g_autofree gchar *result = NULL;
	g_autoptr(GError) error = NULL;
	VentureAuthPrincipal actor;
	g_object_set(config, "ai-enabled", TRUE, "ai-provider", "ollama", NULL);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	context = venture_context_new(config, db);
	user = g_object_new(VENTURE_TYPE_USER, "username", "ai-caller", "active", TRUE, "role", VENTURE_USER_ROLE_EDITOR, NULL);
	g_assert_true(venture_database_save(db, user, NULL, &error));
	company = g_object_new(VENTURE_TYPE_COMPANY, "name", "PrivateBoundaryMarker",
		"organization-id", venture_context_get_default_organization_id(context), NULL);
	g_assert_true(venture_database_save(db, company, NULL, &error));
	ai = venture_ai_service_new(context, &error);
	g_assert_no_error(error);
	g_assert_nonnull(ai);
	actor.user_id = venture_entity_get_id(user);
	actor.token_id = 0;
	actor.role = VENTURE_USER_ROLE_EDITOR;
	actor.name = NULL;
	actor.authenticated = TRUE;
	result = venture_ai_service_execute_tool(ai, tool, &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_null(strstr(result, "PrivateBoundaryMarker"));
}

static void
test_role_matrix(gconstpointer data)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureEntity) user = g_object_new(VENTURE_TYPE_USER, "username", "matrix", "active", TRUE, NULL);
	g_autoptr(VentureEntity) member = NULL;
	g_autoptr(VentureEntity) company = NULL;
	g_autoptr(VentureEntity) expense = NULL;
	VentureAuthPrincipal actor = { 0 };
	VentureAccessPolicy *policy;
	gint role;
	gint64 org;
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), NULL));
	if (NULL == data)
	{
		context = venture_context_new(config, db);
		org = venture_context_get_default_organization_id(context);
	}
	else
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
		g_autoptr(VentureEntity) first = venture_database_find_one(db, query, NULL);
		org = venture_entity_get_id(first);
	}
	g_assert_true(venture_database_save(db, user, NULL, NULL));
	actor.authenticated = TRUE;
	actor.user_id = venture_entity_get_id(user);
	actor.role = VENTURE_USER_ROLE_EDITOR;
	policy = venture_database_get_access_policy(db);
	member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "user-id", actor.user_id, "organization-id", org, "active", TRUE, NULL);
	company = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", org, "owner-user-id", actor.user_id, NULL);
	expense = g_object_new(VENTURE_TYPE_EXPENSE, "organization-id", org, NULL);
	for (role = VENTURE_ORGANIZATION_ROLE_VIEWER; role <= VENTURE_ORGANIZATION_ROLE_SUPPORT; role++)
	{
		gboolean finance = role == VENTURE_ORGANIZATION_ROLE_OWNER || role == VENTURE_ORGANIZATION_ROLE_ADMIN || role == VENTURE_ORGANIZATION_ROLE_FINANCE;
		gboolean all = finance || role == VENTURE_ORGANIZATION_ROLE_EDITOR;
		g_object_set(member, "role", role, NULL);
		g_assert_true(venture_database_save(db, member, NULL, NULL));
		g_object_set(company, "owner-user-id", actor.user_id, NULL);
		g_assert_true(venture_access_policy_can(policy, &actor, "read", company, NULL));
		g_assert_true(venture_access_policy_can(policy, &actor, "export", company, NULL));
		g_assert_cmpint(venture_access_policy_can(policy, &actor, "write", company, NULL), ==, role != VENTURE_ORGANIZATION_ROLE_VIEWER);
		g_assert_cmpint(venture_access_policy_can(policy, &actor, "delete", company, NULL), ==, role != VENTURE_ORGANIZATION_ROLE_VIEWER);
		g_assert_cmpint(venture_access_policy_can(policy, &actor, "read", expense, NULL), ==, finance);
		g_assert_cmpint(venture_access_policy_can(policy, &actor, "write", expense, NULL), ==, finance);
		g_object_set(company, "owner-user-id", (gint64)0, NULL);
		g_assert_cmpint(venture_access_policy_can(policy, &actor, "read", company, NULL), ==, all);
	}
	g_object_set(company, "owner-user-id", actor.user_id, "organization-id", org + 100, NULL);
	g_assert_false(venture_access_policy_can(policy, &actor, "read", company, NULL));
	actor.role = VENTURE_USER_ROLE_OWNER;
	g_assert_true(venture_access_policy_can(policy, &actor, "delete", company, NULL));
	actor.role = VENTURE_USER_ROLE_ADMIN;
	g_assert_true(venture_access_policy_can(policy, &actor, "export", expense, NULL));
}

static void
test_team_revocation(void)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(VentureEntity) user = g_object_new(VENTURE_TYPE_USER, "username", "team-user", "active", TRUE, NULL);
	g_autoptr(VentureEntity) member = NULL;
	g_autoptr(VentureEntity) team = NULL;
	g_autoptr(VentureEntity) teammate = NULL;
	g_autoptr(VentureEntity) company = NULL;
	VentureAuthPrincipal actor = { 0 };
	VentureAccessPolicy *policy;
	gint64 org;
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), NULL));
	context = venture_context_new(config, db);
	org = venture_context_get_default_organization_id(context);
	g_assert_true(venture_database_save(db, user, NULL, NULL));
	actor.authenticated = TRUE;
	actor.user_id = venture_entity_get_id(user);
	actor.role = VENTURE_USER_ROLE_EDITOR;
	policy = venture_database_get_access_policy(db);
	member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "user-id", actor.user_id, "organization-id", org, "role", VENTURE_ORGANIZATION_ROLE_SALES, "active", TRUE, NULL);
	g_assert_true(venture_database_save(db, member, NULL, NULL));
	team = g_object_new(VENTURE_TYPE_TEAM, "name", "Sales", "organization-id", org, NULL);
	g_assert_true(venture_database_save(db, team, NULL, NULL));
	teammate = g_object_new(VENTURE_TYPE_TEAM_MEMBERSHIP, "user-id", actor.user_id, "organization-id", org, "team-id", venture_entity_get_id(team), "active", TRUE, NULL);
	g_assert_true(venture_database_save(db, teammate, NULL, NULL));
	company = g_object_new(VENTURE_TYPE_COMPANY, "name", "Team account", "organization-id", org, "team-id", venture_entity_get_id(team), NULL);
	g_assert_true(venture_access_policy_can(policy, &actor, "read", company, NULL));
	g_object_set(member, "active", FALSE, NULL);
	g_assert_true(venture_database_save(db, member, NULL, NULL));
	g_assert_false(venture_access_policy_can(policy, &actor, "read", company, NULL));
	/* Revoking organization access must not prevent cleaning up team access. */
	g_object_set(teammate, "active", FALSE, NULL);
	g_assert_true(venture_database_save(db, teammate, NULL, NULL));
	g_object_set(member, "active", TRUE, NULL);
	g_assert_true(venture_database_save(db, member, NULL, NULL));
	g_assert_false(venture_access_policy_can(policy, &actor, "read", company, NULL));
	g_object_set(company, "organization-id", org + 100, NULL);
	g_assert_false(venture_database_save(db, company, NULL, NULL));
	g_object_set(company, "organization-id", org, NULL);
	g_assert_true(venture_database_save(db, company, NULL, NULL));
	g_assert_true(venture_database_delete(db, team, NULL, NULL));
	g_object_set(company, "name", "Historical team reference", NULL);
	g_assert_true(venture_database_save(db, company, NULL, NULL));

}

static gboolean
fail_second_membership(VentureDatabase *db, VentureEntity *entity, VentureEntity *previous, gpointer data, GError **error)
{
	guint *writes = data;
	if (++*writes == 2)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION, "Injected second membership failure");
		return FALSE;
	}
	return TRUE;
}

static void
test_bootstrap_rollback(void)
{
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureEntity) second = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Second", "slug", "second", NULL);
	g_autoptr(VentureUser) user = g_object_new(VENTURE_TYPE_USER, "username", "bootstrap", "role", VENTURE_USER_ROLE_OWNER, "active", TRUE, NULL);
	g_autoptr(VentureQuery) users = venture_query_new(VENTURE_TYPE_USER);
	g_autoptr(VentureQuery) members = venture_query_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP);
	g_autoptr(GError) error = NULL;
	guint writes = 0;
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), NULL));
	g_assert_true(venture_database_save(db, second, NULL, NULL));
	venture_database_add_save_validator(db, VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, fail_second_membership, &writes, NULL);
	g_assert_false(venture_orgaccess_bootstrap_owner(db, user, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_cmpuint(writes, ==, 2);
	g_assert_cmpint(venture_database_count(db, users, NULL), ==, 0);
	g_assert_cmpint(venture_database_count(db, members, NULL), ==, 0);
}

static void
test_pagination_and_picker(void)
{
	g_autoptr(VentureDatabase) db = venture_database_new("sqlite://:memory:", NULL);
	g_autoptr(VentureEntity) user = g_object_new(VENTURE_TYPE_USER, "username", "sales", "active", TRUE, NULL);
	g_autoptr(VentureEntity) member = NULL;
	g_autoptr(VentureEntity) home = NULL;
	g_autoptr(VentureEntity) child = NULL;
	g_autoptr(VentureQuery) orgs = venture_query_new(VENTURE_TYPE_ORGANIZATION);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_COMPANY);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureAccessScope) scope = NULL;
	VentureAccessPolicy *policy;
	VentureAuthPrincipal actor = { 0 };
	guint i;
	gint64 last = 0;
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), NULL));
	home = venture_database_find_one(db, orgs, NULL);
	child = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Hidden child", "slug", "child", "parent-id", venture_entity_get_id(home), NULL);
	g_assert_true(venture_database_save(db, child, NULL, NULL));
	g_assert_true(venture_database_save(db, user, NULL, NULL));
	actor.authenticated = TRUE;
	actor.user_id = venture_entity_get_id(user);
	actor.role = VENTURE_USER_ROLE_EDITOR;
	member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "user-id", actor.user_id,
		"organization-id", venture_entity_get_id(home), "active", TRUE, "role", VENTURE_ORGANIZATION_ROLE_SALES, NULL);
	g_assert_true(venture_database_save(db, member, NULL, NULL));
	for (i = 0; i < 3; i++)
	{
		g_autoptr(VentureEntity) company = g_object_new(VENTURE_TYPE_COMPANY, "name", "Page row",
			"organization-id", venture_entity_get_id(home), "owner-user-id", i == 1 ? (gint64)0 : actor.user_id, NULL);
		g_assert_true(venture_database_save(db, company, NULL, NULL));
		last = venture_entity_get_id(company);
	}
	policy = venture_database_get_access_policy(db);
	scope = venture_access_policy_enter(policy, &actor);
	rows = venture_database_find(db, orgs, NULL);
	g_assert_cmpuint(rows->len, ==, 1);
	g_assert_cmpint(venture_entity_get_id(g_ptr_array_index(rows, 0)), ==, venture_entity_get_id(home));
	g_assert_false(venture_access_policy_can(policy, &actor, "read", child, NULL));
	g_clear_pointer(&rows, g_ptr_array_unref);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 1);
	venture_query_set_offset(query, 1);
	rows = venture_database_find(db, query, NULL);
	g_assert_cmpuint(rows->len, ==, 1);
	g_assert_cmpint(venture_entity_get_id(g_ptr_array_index(rows, 0)), ==, last);
	g_assert_cmpint(venture_database_count(db, query, NULL), ==, 2);
	{
		g_autoptr(VentureAccessScope) nested = NULL;
		VentureAuthPrincipal outsider = actor;
		outsider.user_id += 100;
		nested = venture_access_policy_enter(policy, &outsider);
		g_assert_cmpint(venture_database_count(db, query, NULL), ==, 0);
	}
	g_assert_cmpint(venture_database_count(db, query, NULL), ==, 2);
	g_assert_cmpuint(venture_query_get_limit(query), ==, 1);
	g_assert_cmpuint(venture_query_get_offset(query), ==, 1);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/orgaccess/records", test_records);
	g_test_add_func("/orgaccess/module", test_module);
	g_test_add_func("/orgaccess/upgrade", test_upgrade);
	g_test_add_func("/orgaccess/bootstrap", test_bootstrap);
	g_test_add_func("/orgaccess/token-scope", test_token_scope);
	g_test_add_func("/orgaccess/ai", test_ai_boundary);
	g_test_add_data_func("/orgaccess/role-matrix", NULL, test_role_matrix);
	g_test_add_data_func("/orgaccess/role-without-context", "standalone", test_role_matrix);
	g_test_add_func("/orgaccess/team-revocation", test_team_revocation);
	g_test_add_func("/orgaccess/bootstrap-rollback", test_bootstrap_rollback);
	g_test_add_func("/orgaccess/pagination-and-picker", test_pagination_and_picker);
	return g_test_run();
}
