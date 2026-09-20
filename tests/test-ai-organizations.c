/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include "venture-test-accounting.h"

/* Provider ownership, platform grants and usage must use the same metadata
 * registry as every other record, not disappear into a process-only cache. */
static void test_records(void)
{
	VentureEntityRegistry *registry = venture_entity_registry_get_default();
	g_assert_cmpuint(venture_entity_registry_lookup(registry, "ai_configuration"), !=, G_TYPE_INVALID);
	g_assert_cmpuint(venture_entity_registry_lookup(registry, "ai_platform_offer"), !=, G_TYPE_INVALID);
	g_assert_cmpuint(venture_entity_registry_lookup(registry, "ai_usage"), !=, G_TYPE_INVALID);
}
static void test_organization_scope(void)
{
	g_autoptr(VentureDatabase) db = venture_test_accounting_database(NULL);
	g_autoptr(VentureAccessScope) scope = NULL, nested = NULL, internal = NULL;
	g_autoptr(VentureEntity) first = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "First", "slug", "first", NULL);
	g_autoptr(VentureEntity) second = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Second", "slug", "second", NULL);
	g_autoptr(VentureEntity) company = g_object_new(VENTURE_TYPE_COMPANY, "name", "Second secret", NULL);
	g_autoptr(VentureEntity) found = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_COMPANY);
	g_autoptr(GError) error = NULL;
	VentureAccessPolicy *policy = venture_database_get_access_policy(db);
	g_autofree gchar *name = g_strdup("fixture-owner");
	VentureAuthPrincipal actor;
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error)); g_assert_no_error(error);
	g_assert_true(venture_database_save(db, first, NULL, &error));
	g_assert_true(venture_database_save(db, second, NULL, &error));
	venture_entity_set_organization_id(company, venture_entity_get_id(second));
	g_assert_true(venture_database_save(db, company, NULL, &error)); g_assert_no_error(error);
	actor.user_id = 0; actor.token_id = 0; actor.role = VENTURE_USER_ROLE_OWNER; actor.name = name; actor.authenticated = TRUE;
	scope = venture_access_policy_enter_organization(policy, &actor, venture_entity_get_id(first));
	g_assert_cmpint(venture_database_count(db, query, &error), ==, 0); g_assert_no_error(error);
	found = venture_database_get(db, VENTURE_TYPE_COMPANY, venture_entity_get_id(company), &error);
	g_assert_null(found); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND); g_clear_error(&error);
	g_object_set(company, "name", "Refused mutation", NULL);
	g_assert_false(venture_database_save(db, company, NULL, &error)); g_clear_error(&error);
	nested = venture_access_policy_enter(policy, &actor);
	g_assert_false(venture_access_policy_can(policy, &actor, "read", second, NULL));
	g_clear_object(&nested);
	nested = venture_access_policy_enter_organization(policy, &actor, venture_entity_get_id(second));
	g_assert_false(venture_access_policy_can(policy, &actor, "read", first, NULL));
	g_assert_false(venture_access_policy_can(policy, &actor, "read", second, NULL));
	g_clear_object(&nested);
	internal = venture_access_policy_enter(policy, NULL);
	g_assert_cmpint(venture_database_count(db, query, &error), ==, 1); g_assert_no_error(error);
	g_clear_object(&internal);
	g_assert_cmpint(venture_database_count(db, query, &error), ==, 0); g_assert_no_error(error);
	g_clear_object(&scope);
	g_assert_true(venture_access_policy_can(policy, &actor, "read", second, NULL));
	/* Trusted internal work still honors an explicitly paid data scope. */
	scope = venture_access_policy_enter_organization(policy, NULL, venture_entity_get_id(first));
	g_assert_cmpint(venture_database_count(db, query, &error), ==, 0); g_assert_no_error(error);
	g_clear_object(&found);
	found = venture_database_get(db, VENTURE_TYPE_COMPANY, venture_entity_get_id(company), &error);
	g_assert_null(found); g_assert_nonnull(error); g_clear_error(&error);
	g_clear_object(&scope);
	venture_test_accounting_database_cleanup(db);
}
#include "venture-test-ai-provider.h"
#include "venture-test-ai-corpus.h"
int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/ai-organizations/records", test_records);
	g_test_add_func("/ai-organizations/organization-scope", test_organization_scope);
	g_test_add("/ai-organizations/owned-isolation", AiFixture, NULL, ai_fixture_setup, test_ai_owned_isolation, ai_fixture_teardown);
	g_test_add("/ai-organizations/quota", AiFixture, NULL, ai_fixture_setup, test_ai_quota, ai_fixture_teardown);
	g_test_add("/ai-organizations/missing-rotation", AiFixture, NULL, ai_fixture_setup, test_ai_missing_and_rotation, ai_fixture_teardown);
	g_test_add("/ai-organizations/grants", AiFixture, NULL, ai_fixture_setup, test_ai_grants, ai_fixture_teardown);
	g_test_add("/ai-organizations/purpose-isolation", AiFixture, NULL, ai_fixture_setup, test_ai_purpose_isolation, ai_fixture_teardown);
	g_test_add("/ai-organizations/local-revocation", AiFixture, NULL, ai_fixture_setup, test_ai_local_revocation, ai_fixture_teardown);
	g_test_add("/ai-organizations/worker-dispatch", AiFixture, NULL, ai_fixture_setup, test_ai_worker_dispatch, ai_fixture_teardown);
	g_test_add("/ai-organizations/embedding-request", AiFixture, NULL, ai_fixture_setup, test_ai_embedding_request, ai_fixture_teardown);
	g_test_add("/ai-organizations/platform-credential-owner", AiFixture, NULL, ai_fixture_setup, test_ai_platform_credential_owner, ai_fixture_teardown);
	g_test_add("/ai-organizations/corpus-scope", AiFixture, NULL, ai_fixture_setup, test_ai_corpus_scope, ai_fixture_teardown);
	g_test_add("/ai-organizations/deadline", AiFixture, NULL, ai_fixture_setup, test_ai_deadline, ai_fixture_teardown);
	g_test_add("/ai-organizations/failed-no-fallback", AiFixture, NULL, ai_fixture_setup, test_ai_failed_request_no_fallback, ai_fixture_teardown);
	g_test_add("/ai-organizations/tool-scope", AiFixture, NULL, ai_fixture_setup, test_ai_tool_scope, ai_fixture_teardown);
	g_test_add("/ai-organizations/reservation-observer", AiFixture, NULL, ai_fixture_setup, test_ai_reservation_observer, ai_fixture_teardown);
	g_test_add("/ai-organizations/service-scope", AiFixture, NULL, ai_fixture_setup, test_ai_service_scope, ai_fixture_teardown);
	g_test_add("/ai-organizations/restart", AiFixture, NULL, ai_fixture_setup, test_ai_restart, ai_fixture_teardown);
	return g_test_run();
}
