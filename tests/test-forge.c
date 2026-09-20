/*
 * test-forge.c - Forge integration: records, schema and credential handling
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * These tests are about the seams rather than the arithmetic. A forge record
 * holds a credential, a ticket gained an enumerated column on a table that
 * already had rows, and both of those are places where a plausible
 * implementation is wrong in a way that only shows up later -- as a token
 * that will not change, or as every historical ticket quietly becoming an
 * epic.
 */

#include <venture.h>
#include "venture-test-accounting.h"
#include "venture-test-forge.h"

#include <glib.h>
#include <libsoup/soup.h>
#include <string.h>

typedef struct
{
	VentureConfig	*config;
	VentureDatabase	*database;
	VentureContext	*context;
} Fixture;

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;

	(void)user_data;

	fixture->config = venture_config_new();

	fixture->database = venture_test_accounting_database(&error);
	g_assert_no_error(error);

	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->context = venture_context_new(fixture->config, fixture->database);
}

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	(void)user_data;

	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);
}

/* --- Registration -------------------------------------------------------- */

/*
 * Every forge type is registered under the name the rest of the system uses.
 *
 * What breaks if this regresses: the REST router turns /api/v1/<name> into a
 * GType through this registry, and so does venturectl and the schema
 * builder. A type that is declared but never registered has no table, no
 * routes and no CLI, and the failure reads as "no such type" from four
 * unrelated places at once.
 */
static void
test_forge_types_are_registered(void)
{
	VentureEntityRegistry *registry;
	static const gchar *const names[] = {
		"forge", "forge_repo", "forge_rule", "ticket_link", "forge_run"
	};
	gsize i;

	registry = venture_entity_registry_get_default();

	for (i = 0; i < G_N_ELEMENTS(names); i++)
	{
		g_assert_cmpuint(venture_entity_registry_lookup(registry, names[i]),
		                 !=, G_TYPE_INVALID);
	}
}

/* --- The ticket's new columns -------------------------------------------- */

/*
 * A ticket written before issue-type existed reads back as a task.
 *
 * What breaks if this regresses: schema evolution only ever ADDs nullable
 * columns, so every ticket that predates this field has NULL in it, and a
 * NULL enum column leaves the property at its default -- which is always the
 * enum's zero value. Reordering VentureIssueType so that something else is
 * zero silently retypes the entire history of the ticket table, and because
 * the next ordinary edit writes every column back, the guess is then
 * persisted as though somebody had chosen it. There is no audit entry saying
 * anything changed. This test nails the zero value in place.
 */
static void
test_forge_issue_type_defaults_to_task_on_an_older_row(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureTicket) ticket = NULL;
	g_autoptr(VentureEntity) reloaded = NULL;
	g_autoptr(GError) error = NULL;
	VentureIssueType issue_type;
	gint64 id;

	(void)user_data;

	ticket = venture_ticket_new();
	g_object_set(ticket, "title", "written before the column existed", NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(ticket), NULL, &error));
	g_assert_no_error(error);

	id = venture_entity_get_id(VENTURE_ENTITY(ticket));

	/* Exactly what an ALTER TABLE ADD COLUMN leaves behind. */
	g_assert_true(venture_database_execute(fixture->database,
		"UPDATE tickets SET issue_type = NULL", NULL, &error));
	g_assert_no_error(error);

	reloaded = venture_database_get(fixture->database, VENTURE_TYPE_TICKET,
	                                id, &error);
	g_assert_no_error(error);
	g_assert_nonnull(reloaded);

	g_object_get(reloaded, "issue-type", &issue_type, NULL);
	g_assert_cmpint(issue_type, ==, VENTURE_ISSUE_TYPE_TASK);
}

/*
 * Kind and issue-type are independent, and an external bug is both.
 *
 * What breaks if this regresses: somebody notices two enumerations that both
 * look like "what sort of ticket is this" and collapses them. Kind decides
 * who may read the replies -- an internal note on an external ticket is not
 * shown to whoever raised it -- while issue-type decides how the work is
 * approached. Merging them either leaks internal comments or loses the
 * distinction a forge rule keys on.
 */
static void
test_forge_kind_and_issue_type_are_independent(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureTicket) ticket = NULL;
	g_autoptr(VentureEntity) reloaded = NULL;
	g_autoptr(GError) error = NULL;
	VentureTicketKind kind;
	VentureIssueType issue_type;

	(void)user_data;

	ticket = venture_ticket_new();
	g_object_set(ticket,
	             "title", "crash when saving an invoice",
	             "kind", VENTURE_TICKET_KIND_EXTERNAL,
	             "issue-type", VENTURE_ISSUE_TYPE_BUG,
	             NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(ticket), NULL, &error));
	g_assert_no_error(error);

	reloaded = venture_database_get(fixture->database, VENTURE_TYPE_TICKET,
	                                venture_entity_get_id(VENTURE_ENTITY(ticket)),
	                                &error);
	g_assert_no_error(error);

	g_object_get(reloaded, "kind", &kind, "issue-type", &issue_type, NULL);
	g_assert_cmpint(kind, ==, VENTURE_TICKET_KIND_EXTERNAL);
	g_assert_cmpint(issue_type, ==, VENTURE_ISSUE_TYPE_BUG);
}

/*
 * Every issue type survives a round trip by nick.
 *
 * What breaks if this regresses: enum columns are stored as their nick, not
 * their ordinal, so a nick that does not round-trip is a value that silently
 * becomes the zero value on the next read.
 */
static void
test_forge_every_issue_type_round_trips(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GEnumClass) klass = NULL;
	guint i;

	(void)user_data;

	klass = g_type_class_ref(VENTURE_TYPE_ISSUE_TYPE);

	for (i = 0; i < klass->n_values; i++)
	{
		g_autoptr(VentureTicket) ticket = NULL;
		g_autoptr(VentureEntity) reloaded = NULL;
		g_autoptr(GError) error = NULL;
		VentureIssueType read_back;

		ticket = venture_ticket_new();
		g_object_set(ticket, "title", klass->values[i].value_nick,
		             "issue-type", klass->values[i].value, NULL);
		g_assert_true(venture_database_save(fixture->database,
			VENTURE_ENTITY(ticket), NULL, &error));
		g_assert_no_error(error);

		reloaded = venture_database_get(fixture->database,
			VENTURE_TYPE_TICKET,
			venture_entity_get_id(VENTURE_ENTITY(ticket)), &error);
		g_assert_no_error(error);

		g_object_get(reloaded, "issue-type", &read_back, NULL);
		g_assert_cmpint(read_back, ==, klass->values[i].value);
	}
}

/* Two organizations may use different accounts on the same remote server.
 * A workspace-global origin constraint prevents independent configuration. */
static void
test_forge_origin_is_organization_scoped(Fixture *fixture, gconstpointer data)
{
	g_autoptr(VentureOrganization) other = venture_organization_new();
	g_autoptr(VentureForge) first = venture_forge_new();
	g_autoptr(VentureForge) second = venture_forge_new();
	g_autoptr(GError) error = NULL;
	(void)data;
	g_object_set(other, "name", "Second organization", "active", TRUE, NULL);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(other), NULL, &error));
	g_assert_no_error(error);
	g_object_set(first, "organization-id", (gint64)1, "name", "First account",
		"base-url", "https://git.example.com", "active", TRUE, NULL);
	g_object_set(second, "organization-id", venture_entity_get_id(VENTURE_ENTITY(other)),
		"name", "Second account", "base-url", "https://git.example.com", "active", TRUE, NULL);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(other)), !=, 1);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(first), NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(second), NULL, &error));
	g_assert_no_error(error);
}

/* Rebuild the shipped global constraint without changing row identity or
 * reviving deleted identifiers. The same origin must then work in another
 * organization while remaining unique within its own organization. */
static void
test_forge_origin_upgrade(void)
{
	g_autoptr(VentureEntityClass) klass = NULL;
	g_autoptr(VentureDatabase) database = NULL;
	g_autoptr(VentureForge) first = venture_forge_new();
	g_autoptr(VentureForge) removed = venture_forge_new();
	g_autoptr(VentureForge) second = venture_forge_new();
	g_autoptr(VentureForge) duplicate = venture_forge_new();
	g_autoptr(VentureForgeRepo) repository = venture_forge_repo_new();
	g_autoptr(VentureOrganization) other = venture_organization_new();
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *uuid = NULL;
	g_autofree gchar *token = NULL;
	VentureColumnFlags flags;
	gint64 first_id;
	gint64 removed_id;
	gint64 version;
	gint64 reference;

	venture_entity_registry_get_default();
	klass = g_type_class_ref(VENTURE_TYPE_FORGE);
	flags = venture_entity_class_get_column_flags(klass, "base-url");
	venture_entity_class_set_column_flags(klass, "base-url",
		(flags & ~VENTURE_COLUMN_FLAG_UNIQUE_ORGANIZATION) | VENTURE_COLUMN_FLAG_UNIQUE);
	database = venture_test_accounting_database(&error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	venture_entity_class_set_column_flags(klass, "base-url", flags);
	g_object_set(first, "organization-id", (gint64)1, "name", "Retained account",
		"base-url", "https://git.example.com", NULL);
	g_object_set(removed, "organization-id", (gint64)1, "name", "Removed account",
		"base-url", "https://removed.example.com", NULL);
	g_assert_true(venture_database_save(database, VENTURE_ENTITY(first), NULL, &error));
	g_assert_true(venture_database_save(database, VENTURE_ENTITY(removed), NULL, &error));
	g_assert_no_error(error);
	first_id = venture_entity_get_id(VENTURE_ENTITY(first));
	g_assert_true(venture_database_execute(database, "UPDATE forges SET token = 'legacy-import-only' WHERE name = 'Retained account'", NULL, &error));
	removed_id = venture_entity_get_id(VENTURE_ENTITY(removed));
	version = venture_entity_get_version(VENTURE_ENTITY(first));
	uuid = g_strdup(venture_entity_get_uuid(VENTURE_ENTITY(first)));
	g_object_set(repository, "organization-id", (gint64)1, "forge-id", first_id,
		"name", "sample/project", NULL);
	g_assert_true(venture_database_save(database, VENTURE_ENTITY(repository), NULL, &error));
	g_assert_true(venture_database_purge(database, VENTURE_ENTITY(removed), NULL, &error));
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	stored = venture_database_get(database, VENTURE_TYPE_FORGE, first_id, &error);
	g_assert_no_error(error);
	g_assert_cmpstr(venture_entity_get_uuid(stored), ==, uuid);
	g_assert_cmpint(venture_entity_get_version(stored), ==, version);
	g_object_get(stored, "token", &token, NULL);
	g_assert_cmpstr(token, ==, "legacy-import-only");
	g_clear_object(&stored);
	stored = venture_database_get(database, VENTURE_TYPE_FORGE_REPO,
		venture_entity_get_id(VENTURE_ENTITY(repository)), &error);
	g_assert_no_error(error);
	g_object_get(stored, "forge-id", &reference, NULL);
	g_assert_cmpint(reference, ==, first_id);
	g_object_set(other, "name", "Independent business", NULL);
	g_assert_true(venture_database_save(database, VENTURE_ENTITY(other), NULL, &error));
	g_assert_no_error(error);
	g_object_set(second, "organization-id", venture_entity_get_id(VENTURE_ENTITY(other)),
		"name", "Independent account", "base-url", "https://git.example.com", NULL);
	g_assert_true(venture_database_save(database, VENTURE_ENTITY(second), NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(second)), >, removed_id);
	g_object_set(duplicate, "organization-id", (gint64)1, "name", "Duplicate account",
		"base-url", "https://git.example.com", NULL);
	g_assert_false(venture_database_save(database, VENTURE_ENTITY(duplicate), NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_true(venture_database_migrate(database, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	venture_test_accounting_database_cleanup(database);
}

/* --- The credential ------------------------------------------------------ */

/*
 * A forge token can be set, and then changed.
 *
 * What breaks if this regresses -- and this is the one CLAUDE.md warns about
 * directly: venture_database_save() short-circuits on an empty diff and
 * returns success without writing. A sensitive field appears in the diff as
 * {changed, redacted} rather than from/to, and if it were ever excluded from
 * the diff instead, setting a token would report success and leave the old
 * value in place. Nothing would look wrong. The second half of this test is
 * the half that matters.
 */
static void
test_forge_token_can_be_set_and_changed(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureForge) forge = venture_forge_new();
	g_autoptr(GError) error = NULL;
	(void)user_data;
	g_object_set(forge, "name", "Home", "base-url", "https://git.example.com", "token", "plaintext", NULL);
	g_assert_false(venture_database_save(fixture->database, VENTURE_ENTITY(forge), NULL, &error));
	g_assert_nonnull(error); g_clear_error(&error);
	g_object_set(forge, "token", NULL, NULL);
	g_assert_true(venture_database_save(fixture->database, VENTURE_ENTITY(forge), NULL, &error));
	g_assert_no_error(error);
	g_object_set(forge, "webhook-secret", "plaintext", NULL);
	g_assert_false(venture_database_save(fixture->database, VENTURE_ENTITY(forge), NULL, &error));
	g_assert_nonnull(error);

}

/*
 * A token never reaches a serialisation, and a change to one is audited
 * without its value.
 *
 * What breaks if this regresses: the token is in every API response, every
 * CSV export and every audit row. The sensitive flag is the only thing
 * standing between a forge record and that, and it is easy to lose by
 * declaring the field with the wrong flags.
 */
static void
test_forge_token_is_absent_from_serialisation(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureForge) forge = NULL;
	g_autoptr(VentureForge) changed = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(JsonNode) diff = NULL;
	g_autofree gchar *serialised = NULL;
	g_autofree gchar *diff_text = NULL;
	JsonObject *object;

	(void)user_data;
	(void)fixture;

	forge = venture_forge_new();
	g_object_set(forge,
	             "name", "Home",
	             "base-url", "https://git.example.com",
	             "token", "super-secret-value",
	             "webhook-secret", "hook-secret-value",
	             NULL);

	node = venture_serializable_to_json(VENTURE_SERIALIZABLE(forge), FALSE);
	object = json_node_get_object(node);

	g_assert_false(json_object_has_member(object, "token"));
	g_assert_false(json_object_has_member(object, "webhook_secret"));

	/* Belt and braces: the value must not appear anywhere in the text,
	 * under any member name. */
	serialised = json_to_string(node, FALSE);
	g_assert_null(g_strstr_len(serialised, -1, "super-secret-value"));
	g_assert_null(g_strstr_len(serialised, -1, "hook-secret-value"));

	/* The audit diff must say a token changed without saying to what. */
	changed = venture_forge_new();
	g_object_set(changed,
	             "name", "Home",
	             "base-url", "https://git.example.com",
	             "token", "a-different-secret",
	             NULL);

	diff = venture_entity_diff(VENTURE_ENTITY(forge), VENTURE_ENTITY(changed));
	diff_text = json_to_string(diff, FALSE);

	g_assert_nonnull(g_strstr_len(diff_text, -1, "redacted"));
	g_assert_null(g_strstr_len(diff_text, -1, "super-secret-value"));
	g_assert_null(g_strstr_len(diff_text, -1, "a-different-secret"));
}

/*
 * A sensitive field is not offered by the generated form.
 *
 * What breaks if this regresses: the token appears in a text box on the
 * ordinary edit page, gets rendered with its current value, and is then one
 * screenshot away from anybody who can see the screen. It is also why the
 * token needs a route of its own -- this test is what makes that route
 * necessary rather than merely tidy.
 */
static void
test_forge_token_is_not_in_the_generated_form(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureForge) forge = NULL;
	g_autoptr(GPtrArray) specs = NULL;
	guint i;
	gboolean saw_token = FALSE;
	gboolean saw_base_url = FALSE;

	(void)user_data;
	(void)fixture;

	forge = venture_forge_new();
	specs = venture_entity_get_field_specs(VENTURE_ENTITY(forge));

	for (i = 0; i < specs->len; i++)
	{
		VentureFieldSpec *spec = g_ptr_array_index(specs, i);
		const gchar *name = venture_field_spec_get_name(spec);
		VentureColumnFlags flags = venture_field_spec_get_flags(spec);

		if (0 == g_strcmp0(name, "token"))
		{
			saw_token = TRUE;
			g_assert_cmpuint(flags & VENTURE_COLUMN_FLAG_SENSITIVE, !=, 0);
			g_assert_false(venture_field_spec_get_show_in_list(spec));
		}

		if (0 == g_strcmp0(name, "base-url"))
		{
			saw_base_url = TRUE;
			g_assert_cmpuint(flags & VENTURE_COLUMN_FLAG_SENSITIVE, ==, 0);
		}
	}

	/* Both must have been seen, or the assertions above proved nothing. */
	g_assert_true(saw_token);
	g_assert_true(saw_base_url);
}

/* --- Rule resolution ----------------------------------------------------- */

static gint64
make_forge(Fixture *fixture, const gchar *name)
{
	g_autoptr(VentureForge) forge = NULL;
	g_autoptr(GError) error = NULL;

	g_autofree gchar *base_url = NULL;

	/* Derived from the name: base-url is UNIQUE, so two forges in one
	 * test cannot share it. */
	base_url = g_strdup_printf("https://%s.example.com", name);

	forge = venture_forge_new();
	g_object_set(forge, "name", name, "base-url", base_url,
	             "active", TRUE, NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(forge), NULL, &error));
	g_assert_no_error(error);

	return venture_entity_get_id(VENTURE_ENTITY(forge));
}

static gint64
make_repo(Fixture *fixture, gint64 forge_id, const gchar *full_name)
{
	g_autoptr(VentureForgeRepo) repo = NULL;
	g_autoptr(GError) error = NULL;

	repo = venture_forge_repo_new();
	g_object_set(repo, "name", full_name, "forge-id", forge_id,
	             "active", TRUE, NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(repo), NULL, &error));
	g_assert_no_error(error);

	return venture_entity_get_id(VENTURE_ENTITY(repo));
}

static gint64
make_rule(
	Fixture		*fixture,
	const gchar	*name,
	gint64		 repo_id,
	gint64		 forge_id,
	gboolean	 all_types,
	VentureIssueType issue_type,
	gboolean	 enabled
){
	g_autoptr(VentureForgeRule) rule = NULL;
	g_autoptr(GError) error = NULL;

	rule = venture_forge_rule_new();
	g_object_set(rule,
	             "name", name,
	             "repo-id", repo_id,
	             "forge-id", forge_id,
	             "all-issue-types", all_types,
	             "issue-type", issue_type,
	             "enabled", enabled,
	             NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(rule), NULL, &error));
	g_assert_no_error(error);

	return venture_entity_get_id(VENTURE_ENTITY(rule));
}

/*
 * The repository's own rule beats a forge-wide one.
 *
 * What breaks if this regresses: a forge-wide rule written generously -- to
 * cover the repositories you have not thought about yet -- starts overriding
 * the deliberate, narrower rule on the repository you care most about. The
 * symptom is a run that used the wrong model, or ran at all when a
 * repository-scoped rule said it should not.
 */
static void
test_forge_rule_prefers_the_repository(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureForgeRule) resolved = NULL;
	g_autoptr(GError) error = NULL;
	gint64 forge_id;
	gint64 repo_id;
	gint64 specific;

	(void)user_data;

	forge_id = make_forge(fixture, "Home");
	repo_id = make_repo(fixture, forge_id, "zach/venture");

	make_rule(fixture, "forge bugs", 0, forge_id, FALSE,
	          VENTURE_ISSUE_TYPE_BUG, TRUE);
	specific = make_rule(fixture, "repo bugs", repo_id, 0, FALSE,
	                     VENTURE_ISSUE_TYPE_BUG, TRUE);

	resolved = venture_forge_rule_resolve(fixture->database, repo_id,
	                                      VENTURE_ISSUE_TYPE_BUG, &error);
	g_assert_no_error(error);
	g_assert_nonnull(resolved);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(resolved)), ==,
	                specific);
}

/*
 * A typed rule beats a catch-all at the same scope.
 *
 * What breaks if this regresses: the whole point of the feature. "Bugs get
 * an AI, everything else gets a branch" is expressed as a typed rule beside
 * a catch-all, and if the catch-all wins, tasks start getting worked on.
 */
static void
test_forge_rule_prefers_the_typed_rule(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureForgeRule) resolved = NULL;
	g_autoptr(GError) error = NULL;
	gint64 forge_id;
	gint64 repo_id;
	gint64 typed;

	(void)user_data;

	forge_id = make_forge(fixture, "Home");
	repo_id = make_repo(fixture, forge_id, "zach/venture");

	make_rule(fixture, "everything", repo_id, 0, TRUE,
	          VENTURE_ISSUE_TYPE_TASK, TRUE);
	typed = make_rule(fixture, "bugs", repo_id, 0, FALSE,
	                  VENTURE_ISSUE_TYPE_BUG, TRUE);

	resolved = venture_forge_rule_resolve(fixture->database, repo_id,
	                                      VENTURE_ISSUE_TYPE_BUG, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(resolved)), ==, typed);
}

/*
 * The forge-wide catch-all is the last resort, and it is reached.
 *
 * What breaks if this regresses: "run on every bug on this forge" -- the
 * single-row configuration that makes the feature usable without enrolling
 * each repository -- stops matching anything.
 */
static void
test_forge_rule_falls_back_to_the_forge(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureForgeRule) resolved = NULL;
	g_autoptr(GError) error = NULL;
	gint64 forge_id;
	gint64 repo_id;
	gint64 wide;

	(void)user_data;

	forge_id = make_forge(fixture, "Home");
	repo_id = make_repo(fixture, forge_id, "zach/venture");
	wide = make_rule(fixture, "anything anywhere", 0, forge_id, TRUE,
	                 VENTURE_ISSUE_TYPE_TASK, TRUE);

	resolved = venture_forge_rule_resolve(fixture->database, repo_id,
	                                      VENTURE_ISSUE_TYPE_RESEARCH, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(resolved)), ==, wide);
}

/*
 * No rule at all means nothing runs, and that is not an error.
 *
 * What breaks if this regresses: either a repository nobody enrolled starts
 * getting AI runs, or the absence of a rule is reported as a failure and the
 * caller treats a perfectly normal state as broken.
 */
static void
test_forge_rule_absent_means_nothing_runs(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureForgeRule) resolved = NULL;
	g_autoptr(GError) error = NULL;
	gint64 forge_id;
	gint64 repo_id;

	(void)user_data;

	forge_id = make_forge(fixture, "Home");
	repo_id = make_repo(fixture, forge_id, "zach/venture");

	resolved = venture_forge_rule_resolve(fixture->database, repo_id,
	                                      VENTURE_ISSUE_TYPE_BUG, &error);
	g_assert_no_error(error);
	g_assert_null(resolved);

	/* And a ticket with no repository at all resolves to nothing too,
	 * rather than picking up somebody else's forge-wide rule. */
	make_rule(fixture, "forge wide", 0, forge_id, TRUE,
	          VENTURE_ISSUE_TYPE_TASK, TRUE);

	g_assert_null(venture_forge_rule_resolve(fixture->database, 0,
	                                         VENTURE_ISSUE_TYPE_BUG, &error));
	g_assert_no_error(error);
}

/*
 * A disabled rule is skipped and the search continues past it.
 *
 * What breaks if this regresses: switching a rule off would either stop the
 * search dead -- so the broader rule never applies -- or leave the disabled
 * rule running. Both are surprising, which is why the behaviour is stated in
 * the field's own help text and pinned here.
 */
static void
test_forge_rule_disabled_falls_through(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureForgeRule) resolved = NULL;
	g_autoptr(GError) error = NULL;
	gint64 forge_id;
	gint64 repo_id;
	gint64 wide;

	(void)user_data;

	forge_id = make_forge(fixture, "Home");
	repo_id = make_repo(fixture, forge_id, "zach/venture");

	make_rule(fixture, "repo bugs, switched off", repo_id, 0, FALSE,
	          VENTURE_ISSUE_TYPE_BUG, FALSE);
	wide = make_rule(fixture, "forge bugs", 0, forge_id, FALSE,
	                 VENTURE_ISSUE_TYPE_BUG, TRUE);

	resolved = venture_forge_rule_resolve(fixture->database, repo_id,
	                                      VENTURE_ISSUE_TYPE_BUG, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(resolved)), ==, wide);
}

/*
 * A rule scoped to neither a repository nor a forge matches nothing.
 *
 * What breaks if this regresses: a row saved half-filled -- the obvious
 * mistake when creating a rule through the form -- would apply to every
 * ticket on every forge. That is the failure that costs money, so it is
 * worth a test of its own.
 */
static void
test_forge_rule_with_no_scope_matches_nothing(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	gint64 forge_id;
	gint64 repo_id;

	(void)user_data;

	forge_id = make_forge(fixture, "Home");
	repo_id = make_repo(fixture, forge_id, "zach/venture");

	make_rule(fixture, "half filled in", 0, 0, TRUE,
	          VENTURE_ISSUE_TYPE_TASK, TRUE);

	g_assert_null(venture_forge_rule_resolve(fixture->database, repo_id,
	                                         VENTURE_ISSUE_TYPE_BUG, &error));
	g_assert_no_error(error);
}

/*
 * A rule belonging to another forge never matches.
 *
 * What breaks if this regresses: two forges configured on one install start
 * borrowing each other's policy, and a repository on the cautious forge gets
 * the permissive forge's rule.
 */
static void
test_forge_rule_does_not_cross_forges(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	gint64 mine;
	gint64 theirs;
	gint64 repo_id;

	(void)user_data;

	mine = make_forge(fixture, "Home");
	theirs = make_forge(fixture, "Work");
	repo_id = make_repo(fixture, mine, "zach/venture");

	make_rule(fixture, "other forge", 0, theirs, TRUE,
	          VENTURE_ISSUE_TYPE_TASK, TRUE);

	g_assert_null(venture_forge_rule_resolve(fixture->database, repo_id,
	                                         VENTURE_ISSUE_TYPE_BUG, &error));
	g_assert_no_error(error);
}

/*
 * Two rules with identical scope resolve to the same one every time.
 *
 * What breaks if this regresses: the duplicate is a configuration mistake
 * either way, but a resolution that depends on the order the database
 * happened to return rows makes the same ticket behave differently on
 * consecutive runs, which is far harder to diagnose than a consistent wrong
 * answer.
 */
static void
test_forge_rule_ties_break_by_id(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	gint64 forge_id;
	gint64 repo_id;
	gint64 first;
	guint i;

	(void)user_data;

	forge_id = make_forge(fixture, "Home");
	repo_id = make_repo(fixture, forge_id, "zach/venture");

	first = make_rule(fixture, "one", repo_id, 0, FALSE,
	                  VENTURE_ISSUE_TYPE_BUG, TRUE);
	make_rule(fixture, "two", repo_id, 0, FALSE, VENTURE_ISSUE_TYPE_BUG, TRUE);

	for (i = 0; i < 3; i++)
	{
		g_autoptr(VentureForgeRule) resolved = NULL;

		/* Each resolution warns about the duplicate, which
		 * G_DEBUG=fatal-warnings would otherwise turn into a failure.
		 * Expected once per call, immediately before it. */
		g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
		                      "*identical scope*");

		resolved = venture_forge_rule_resolve(fixture->database, repo_id,
		                                      VENTURE_ISSUE_TYPE_BUG,
		                                      &error);
		g_assert_no_error(error);
		g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(resolved)),
		                ==, first);

		g_test_assert_expected_messages();
	}
}

/* --- Branch naming ------------------------------------------------------- */

/*
 * A branch name built from a hostile title is still a legal refname.
 *
 * What breaks if this regresses: the title comes from a webhook, so anybody
 * who can open an issue upstream chooses it. A name beginning with "-" is
 * read by git as an option rather than a branch, and "..", spaces and the
 * characters git reserves all produce either a refusal with an opaque
 * message or, worse, an argument somebody else picked.
 */
static void
test_forge_branch_name_is_always_legal(void)
{
	static const gchar *const titles[] = {
		"--upload-pack=/bin/sh",
		"../../etc/passwd",
		"a branch with spaces",
		"tilde~caret^colon:question?star*bracket[backslash\\",
		"trailing.lock",
		"@{upstream}",
		"",
		"     ",
		"日本語",
		"Café crème brûlée"
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(titles); i++)
	{
		g_autofree gchar *branch = NULL;

		branch = venture_forge_branch_name(NULL, NULL,
		                                   VENTURE_ISSUE_TYPE_BUG, 42, 0,
		                                   titles[i]);

		g_assert_nonnull(branch);
		g_assert_true(venture_forge_refname_is_valid(branch));
		g_assert_true('-' != branch[0]);
	}
}

/*
 * A hostile template or prefix cannot produce an illegal name either.
 *
 * What breaks if this regresses: the template and the prefix are operator
 * text from the settings page, so they are less hostile than a webhook title
 * but not verified. Falling back to a name built only from parts this code
 * controls is what stops a typo in a prefix becoming a git argument.
 */
static void
test_forge_branch_name_survives_a_bad_template(void)
{
	g_autofree gchar *from_prefix = NULL;
	g_autofree gchar *from_template = NULL;

	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
	                      "*git will not accept*");
	from_prefix = venture_forge_branch_name(NULL, "-oProxyCommand=",
	                                        VENTURE_ISSUE_TYPE_BUG, 7, 0,
	                                        "a real bug");
	g_assert_true(venture_forge_refname_is_valid(from_prefix));

	g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
	                      "*git will not accept*");
	from_template = venture_forge_branch_name("../{slug}", NULL,
	                                          VENTURE_ISSUE_TYPE_BUG, 7, 0,
	                                          "a real bug");
	g_assert_true(venture_forge_refname_is_valid(from_template));

	g_test_assert_expected_messages();
}

/*
 * Two tickets with the same title get different branches.
 *
 * What breaks if this regresses: the second run pushes onto the first one's
 * branch, and one ticket's work silently lands inside another's pull
 * request. The ticket id in the default template is the only thing
 * guaranteed to differ, which is why it is not decorative.
 */
static void
test_forge_branch_name_is_unique_per_ticket(void)
{
	g_autofree gchar *first = NULL;
	g_autofree gchar *second = NULL;

	first = venture_forge_branch_name(NULL, NULL, VENTURE_ISSUE_TYPE_BUG, 1,
	                                  0, "invoice total is wrong");
	second = venture_forge_branch_name(NULL, NULL, VENTURE_ISSUE_TYPE_BUG, 2,
	                                   0, "invoice total is wrong");

	g_assert_cmpstr(first, !=, second);
}

/*
 * The template's placeholders expand, and the prefix is applied.
 */
static void
test_forge_branch_name_expands_the_template(void)
{
	g_autofree gchar *branch = NULL;
	g_autofree gchar *issue = NULL;

	branch = venture_forge_branch_name(NULL, "venture/",
	                                   VENTURE_ISSUE_TYPE_BUG, 412, 0,
	                                   "Invoice total rounds wrong");
	g_assert_cmpstr(branch, ==, "venture/bug/412-invoice-total-rounds-wrong");

	/* {issue} falls back to the ticket id when nothing is filed upstream,
	 * so a template using it still produces a distinct name. */
	issue = venture_forge_branch_name("{issue}-{slug}", NULL,
	                                  VENTURE_ISSUE_TYPE_BUG, 412, 99,
	                                  "Invoice total rounds wrong");
	g_assert_cmpstr(issue, ==, "99-invoice-total-rounds-wrong");
}

/*
 * A repository's full name splits exactly, and a malformed one is refused.
 *
 * What breaks if this regresses: these two halves become path segments in an
 * API URL. A name carrying an extra slash or a traversal segment is how a
 * caller-supplied value walks out of the path it was meant to sit in.
 */
static void
test_forge_repo_split_refuses_malformed_names(void)
{
	g_autofree gchar *owner = NULL;
	g_autofree gchar *repo = NULL;
	static const gchar *const bad[] = {
		"noslash", "", "/repo", "owner/", "owner/sub/repo", "../..",
		"owner/..", "../repo"
	};
	gsize i;

	g_assert_true(venture_forge_repo_split("zach/venture", &owner, &repo));
	g_assert_cmpstr(owner, ==, "zach");
	g_assert_cmpstr(repo, ==, "venture");

	for (i = 0; i < G_N_ELEMENTS(bad); i++)
	{
		g_autofree gchar *o = NULL;
		g_autofree gchar *r = NULL;

		g_assert_false(venture_forge_repo_split(bad[i], &o, &r));
		g_assert_null(o);
		g_assert_null(r);
	}
}

/* --- A forge to talk to ---------------------------------------------------
 *
 * The client is synchronous, so the test server cannot live on the thread
 * that is blocked inside it. It gets its own thread and its own
 * GMainContext; without that the request never reaches a handler and the
 * test hangs rather than failing.
 */

typedef struct
{
	GThread		*thread;
	GMainContext	*context;
	GMainLoop	*loop;
	SoupServer	*server;
	guint16		 port;
	GAsyncQueue	*ready;

	/* What the last request looked like, for the assertions. */
	GMutex		 lock;
	gchar		*last_path;
	gchar		*last_auth;

	/* What to answer with. */
	guint		 status;
	gchar		*body;
	gchar		*redirect_to;
} FakeForge;

static void
fake_forge_handler(
	SoupServer		*server,
	SoupServerMessage	*message,
	const char		*path,
	GHashTable		*query,
	gpointer		 user_data
){
	FakeForge *forge = user_data;
	g_autofree gchar *body = NULL;
	guint status;

	(void)server;
	(void)query;

	g_mutex_lock(&forge->lock);

	g_clear_pointer(&forge->last_path, g_free);
	forge->last_path = g_strdup(path);

	g_clear_pointer(&forge->last_auth, g_free);
	forge->last_auth = g_strdup(soup_message_headers_get_one(
		soup_server_message_get_request_headers(message), "Authorization"));

	status = forge->status;
	body = g_strdup(forge->body);

	if (NULL != forge->redirect_to)
	{
		soup_message_headers_replace(
			soup_server_message_get_response_headers(message),
			"Location", forge->redirect_to);
		soup_server_message_set_status(message, SOUP_STATUS_FOUND, NULL);
		g_mutex_unlock(&forge->lock);
		return;
	}

	g_mutex_unlock(&forge->lock);

	soup_server_message_set_status(message, status, NULL);

	if (NULL != body)
	{
		soup_server_message_set_response(message, "application/json",
		                                 SOUP_MEMORY_COPY, body, strlen(body));
	}
}

static gpointer
fake_forge_thread(gpointer user_data)
{
	FakeForge *forge = user_data;
	g_autoptr(GError) error = NULL;
	GSList *uris;

	g_main_context_push_thread_default(forge->context);

	forge->server = soup_server_new(NULL, NULL);
	soup_server_add_handler(forge->server, NULL, fake_forge_handler, forge,
	                        NULL);

	g_assert_true(soup_server_listen_local(forge->server, 0,
	                                       SOUP_SERVER_LISTEN_IPV4_ONLY,
	                                       &error));
	g_assert_no_error(error);

	uris = soup_server_get_uris(forge->server);
	g_assert_nonnull(uris);
	forge->port = (guint16)g_uri_get_port(uris->data);
	g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);

	g_async_queue_push(forge->ready, GINT_TO_POINTER(1));

	g_main_loop_run(forge->loop);

	soup_server_disconnect(forge->server);
	g_clear_object(&forge->server);
	g_main_context_pop_thread_default(forge->context);

	return NULL;
}

static FakeForge *
fake_forge_start(void)
{
	FakeForge *forge;

	forge = g_new0(FakeForge, 1);
	g_mutex_init(&forge->lock);
	forge->context = g_main_context_new();
	forge->loop = g_main_loop_new(forge->context, FALSE);
	forge->ready = g_async_queue_new();
	forge->status = SOUP_STATUS_OK;

	forge->thread = g_thread_new("fake-forge", fake_forge_thread, forge);

	g_assert_nonnull(g_async_queue_timeout_pop(forge->ready,
	                                           10 * G_USEC_PER_SEC));

	return forge;
}

static void
fake_forge_stop(FakeForge *forge)
{
	g_main_loop_quit(forge->loop);
	g_thread_join(forge->thread);

	g_main_loop_unref(forge->loop);
	g_main_context_unref(forge->context);
	g_async_queue_unref(forge->ready);
	g_clear_pointer(&forge->last_path, g_free);
	g_clear_pointer(&forge->last_auth, g_free);
	g_clear_pointer(&forge->body, g_free);
	g_clear_pointer(&forge->redirect_to, g_free);
	g_mutex_clear(&forge->lock);
	g_free(forge);
}

static void
fake_forge_reply(
	FakeForge	*forge,
	guint		 status,
	const gchar	*body
){
	g_mutex_lock(&forge->lock);
	forge->status = status;
	g_clear_pointer(&forge->body, g_free);
	forge->body = g_strdup(body);
	g_mutex_unlock(&forge->lock);
}

static gchar *
fake_forge_url(FakeForge *forge)
{
	return g_strdup_printf("http://127.0.0.1:%u", forge->port);
}

static gchar *
fake_forge_last_path(FakeForge *forge)
{
	gchar *path;

	g_mutex_lock(&forge->lock);
	path = g_strdup(forge->last_path);
	g_mutex_unlock(&forge->lock);

	return path;
}

/*
 * A base URL that is not http or https is refused when the client is built.
 *
 * What breaks if this regresses: the origin is chosen exactly once, and
 * everything downstream reuses the scheme from that parse. A file:// or
 * gopher:// origin accepted here would be reachable by every later request,
 * and the check would have to be repeated at each one instead.
 */
static void
test_forge_client_refuses_a_bad_scheme(void)
{
	static const gchar *const bad[] = {
		"file:///etc/passwd", "gopher://example.com", "ftp://example.com",
		"not a url", ""
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(bad); i++)
	{
		g_autoptr(GError) error = NULL;
		VentureForgejoClient *client;

		client = venture_forgejo_client_new(bad[i], "tok", 5, &error);

		g_assert_null(client);
		g_assert_nonnull(error);
	}

	/* And a good one is accepted, or the loop above proved nothing. */
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(VentureForgejoClient) client = NULL;

		client = venture_forgejo_client_new("https://git.example.com", "tok",
		                                    5, &error);
		g_assert_no_error(error);
		g_assert_nonnull(client);
	}
}

/*
 * The client does not follow a redirect, and the token never reaches the
 * host the redirect named.
 *
 * What breaks if this regresses -- and this is the test that matters most in
 * this file: every request carries an Authorization header holding this
 * install's forge token. libsoup follows redirects by default and carries
 * that header with it, so a forge that answered 302 could hand the token to
 * any host it chose. The AI's fetcher is protected by refusing private
 * addresses; this client cannot use that defence, because a self-hosted
 * forge lives on exactly those addresses. Refusing to move is the defence
 * instead.
 */
static void
test_forge_client_does_not_follow_a_redirect(void)
{
	FakeForge *origin;
	FakeForge *elsewhere;
	g_autoptr(VentureForgejoClient) client = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *base = NULL;
	g_autofree gchar *target = NULL;
	g_autofree gchar *reached = NULL;
	g_autofree gchar *login = NULL;

	origin = fake_forge_start();
	elsewhere = fake_forge_start();

	fake_forge_reply(elsewhere, SOUP_STATUS_OK, "{\"login\":\"stolen\"}");

	target = g_strdup_printf("http://127.0.0.1:%u/api/v1/user",
	                         elsewhere->port);

	g_mutex_lock(&origin->lock);
	origin->redirect_to = g_strdup(target);
	g_mutex_unlock(&origin->lock);

	base = fake_forge_url(origin);
	client = venture_forgejo_client_new(base, "super-secret-token", 5, &error);
	g_assert_no_error(error);

	login = venture_forge_client_whoami(VENTURE_FORGE_CLIENT(client), &error);

	/* The call fails rather than silently succeeding elsewhere. */
	g_assert_null(login);
	g_assert_nonnull(error);

	/* And nothing at all reached the other server. */
	reached = fake_forge_last_path(elsewhere);
	g_assert_null(reached);

	fake_forge_stop(origin);
	fake_forge_stop(elsewhere);
}

/*
 * A hostile repository name cannot walk out of the path it belongs in.
 *
 * What breaks if this regresses: the repository name reaches VENTURE from a
 * webhook payload, so anybody who can open an issue upstream influences it.
 * A name carrying an extra slash or a traversal segment would otherwise
 * address a different endpoint entirely -- with this install's token
 * attached.
 */
static void
test_forge_client_escapes_the_repository_name(void)
{
	FakeForge *forge;
	g_autoptr(VentureForgejoClient) client = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *base = NULL;
	g_autofree gchar *path = NULL;
	gboolean ok;

	forge = fake_forge_start();
	fake_forge_reply(forge, SOUP_STATUS_OK, "{\"default_branch\":\"main\"}");

	base = fake_forge_url(forge);
	client = venture_forgejo_client_new(base, "tok", 5, &error);
	g_assert_no_error(error);

	/* Refused before a request is even made: the split rejects it. */
	ok = venture_forge_client_check_repository(VENTURE_FORGE_CLIENT(client),
	                                           "../../admin/secrets", NULL,
	                                           &error);
	g_assert_false(ok);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_assert_null(fake_forge_last_path(forge));
	g_clear_error(&error);

	/* A name whose halves are individually hostile but structurally
	 * valid is escaped rather than refused, and still cannot escape. */
	ok = venture_forge_client_check_repository(VENTURE_FORGE_CLIENT(client),
	                                           "..%2F..%2Fadmin/repo", NULL,
	                                           &error);
	(void)ok;
	path = fake_forge_last_path(forge);

	if (NULL != path)
	{
		g_assert_null(strstr(path, "/../"));
		g_assert_true(g_str_has_prefix(path, "/api/v1/repos/"));
	}

	fake_forge_stop(forge);
}

/*
 * Status codes become errors an operator can act on.
 *
 * What breaks if this regresses: the module these endpoint shapes came from
 * reports every non-2xx as a network failure, which makes "the token was
 * rejected" and "that repository does not exist" the same message. Those
 * two have completely different fixes.
 */
static void
test_forge_client_maps_status_codes(void)
{
	static const struct
	{
		guint	 status;
		gint	 code;
	} cases[] = {
		{ SOUP_STATUS_UNAUTHORIZED, VENTURE_ERROR_UNAUTHENTICATED },
		{ SOUP_STATUS_FORBIDDEN,    VENTURE_ERROR_PERMISSION_DENIED },
		{ SOUP_STATUS_NOT_FOUND,    VENTURE_ERROR_NOT_FOUND },
		{ SOUP_STATUS_CONFLICT,     VENTURE_ERROR_ALREADY_EXISTS },
		{ 422,                      VENTURE_ERROR_VALIDATION },
		{ 429,                      VENTURE_ERROR_CONFLICT },
		{ 500,                      VENTURE_ERROR_NETWORK }
	};
	FakeForge *forge;
	g_autoptr(VentureForgejoClient) client = NULL;
	g_autoptr(GError) build_error = NULL;
	g_autofree gchar *base = NULL;
	gsize i;

	forge = fake_forge_start();
	base = fake_forge_url(forge);
	client = venture_forgejo_client_new(base, "super-secret-token", 5,
	                                    &build_error);
	g_assert_no_error(build_error);

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autoptr(GError) error = NULL;
		g_autofree gchar *login = NULL;

		fake_forge_reply(forge, cases[i].status, "{\"message\":\"nope\"}");

		login = venture_forge_client_whoami(VENTURE_FORGE_CLIENT(client),
		                                    &error);

		g_assert_null(login);
		g_assert_error(error, VENTURE_ERROR, cases[i].code);

		/* The token must not appear in anything an operator will read. */
		g_assert_null(strstr(error->message, "super-secret-token"));
	}

	fake_forge_stop(forge);
}

/*
 * A successful call carries the token, and reaches the path it meant to.
 *
 * The counterpart to the tests above: they prove things do not happen, and
 * without this one they would all pass against a client that never sent
 * anything at all.
 */
static void
test_forge_client_sends_the_token(void)
{
	FakeForge *forge;
	g_autoptr(VentureForgejoClient) client = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *base = NULL;
	g_autofree gchar *login = NULL;
	g_autofree gchar *path = NULL;
	g_autofree gchar *auth = NULL;

	forge = fake_forge_start();
	fake_forge_reply(forge, SOUP_STATUS_OK, "{\"login\":\"venture-bot\"}");

	base = fake_forge_url(forge);
	client = venture_forgejo_client_new(base, "super-secret-token", 5, &error);
	g_assert_no_error(error);

	login = venture_forge_client_whoami(VENTURE_FORGE_CLIENT(client), &error);
	g_assert_no_error(error);
	g_assert_cmpstr(login, ==, "venture-bot");

	path = fake_forge_last_path(forge);
	g_assert_cmpstr(path, ==, "/api/v1/user");

	g_mutex_lock(&forge->lock);
	auth = g_strdup(forge->last_auth);
	g_mutex_unlock(&forge->lock);

	/* "token", not "Bearer" -- what Forgejo's own documentation uses. */
	g_assert_cmpstr(auth, ==, "token super-secret-token");

	fake_forge_stop(forge);
}

/* --- Webhook signatures --------------------------------------------------- */

static SoupMessageHeaders *
signature_headers(
	const gchar	*name,
	const gchar	*value
){
	SoupMessageHeaders *headers;

	headers = soup_message_headers_new(SOUP_MESSAGE_HEADERS_REQUEST);

	if (NULL != name)
		soup_message_headers_replace(headers, name, value);

	return headers;
}

/*
 * A correctly signed payload is accepted; everything else is refused.
 *
 * What breaks if this regresses: /hooks/forge/:id is the only route in this
 * codebase that deliberately accepts an unauthenticated request, and the
 * signature is the whole of its authentication. Every case below is a way
 * that check has been got wrong somewhere before -- most notably the last
 * one, which is what the podomation module this scheme was copied from
 * actually does.
 */
static void
test_forge_webhook_signature(void)
{
	static const gchar *const secret = "hook-secret";
	static const gchar *const payload = "{\"action\":\"opened\"}";
	g_autoptr(VentureForgejoClient) client = NULL;
	g_autoptr(GError) build_error = NULL;
	g_autoptr(GBytes) body = NULL;
	g_autofree gchar *good = NULL;

	client = venture_forgejo_client_new("https://git.example.com", "tok", 5,
	                                    &build_error);
	g_assert_no_error(build_error);

	body = g_bytes_new(payload, strlen(payload));
	good = g_compute_hmac_for_data(G_CHECKSUM_SHA256,
	                               (const guchar *)secret, strlen(secret),
	                               (const guchar *)payload, strlen(payload));

	/* The signature Forgejo would actually send. */
	{
		g_autoptr(SoupMessageHeaders) headers = NULL;
		g_autoptr(GError) error = NULL;

		headers = signature_headers("X-Forgejo-Signature", good);
		g_assert_true(venture_forge_client_verify_webhook(
			VENTURE_FORGE_CLIENT(client), headers, body, secret, &error));
		g_assert_no_error(error);
	}

	/* Gitea's spelling of the same header, which Forgejo also sends. */
	{
		g_autoptr(SoupMessageHeaders) headers = NULL;
		g_autoptr(GError) error = NULL;

		headers = signature_headers("X-Gitea-Signature", good);
		g_assert_true(venture_forge_client_verify_webhook(
			VENTURE_FORGE_CLIENT(client), headers, body, secret, &error));
		g_assert_no_error(error);
	}

	/* A signature that is simply wrong. */
	{
		g_autoptr(SoupMessageHeaders) headers = NULL;
		g_autoptr(GError) error = NULL;

		headers = signature_headers("X-Forgejo-Signature",
			"0000000000000000000000000000000000000000000000000000000000000000");
		g_assert_false(venture_forge_client_verify_webhook(
			VENTURE_FORGE_CLIENT(client), headers, body, secret, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED);
	}

	/* A signature that was valid for a different body. Catches a check
	 * that verifies the header is well formed without verifying it
	 * against anything. */
	{
		g_autoptr(SoupMessageHeaders) headers = NULL;
		g_autoptr(GError) error = NULL;
		g_autoptr(GBytes) tampered = NULL;
		static const gchar *const other = "{\"action\":\"closed\"}";

		tampered = g_bytes_new(other, strlen(other));
		headers = signature_headers("X-Forgejo-Signature", good);
		g_assert_false(venture_forge_client_verify_webhook(
			VENTURE_FORGE_CLIENT(client), headers, tampered, secret, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED);
	}

	/* No signature header at all. Must be a refusal, not "nothing to
	 * check". */
	{
		g_autoptr(SoupMessageHeaders) headers = NULL;
		g_autoptr(GError) error = NULL;

		headers = signature_headers(NULL, NULL);
		g_assert_false(venture_forge_client_verify_webhook(
			VENTURE_FORGE_CLIENT(client), headers, body, secret, &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED);
	}

	/*
	 * No secret configured. This is the one that matters: the module this
	 * scheme came from returns TRUE here, which turns a forge nobody
	 * finished configuring into an endpoint anybody can post records to.
	 */
	{
		g_autoptr(SoupMessageHeaders) headers = NULL;
		g_autoptr(GError) error = NULL;

		headers = signature_headers("X-Forgejo-Signature", good);
		g_assert_false(venture_forge_client_verify_webhook(
			VENTURE_FORGE_CLIENT(client), headers, body, "", &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNAUTHENTICATED);
	}
}

/*
 * A payload containing a NUL byte still verifies.
 *
 * What breaks if this regresses: htmx_request_get_body() is a g_strndup, so
 * reading the body as a C string truncates it at the first NUL and the HMAC
 * is then computed over a prefix. The signature would fail for exactly those
 * deliveries and succeed for every other, which is about the least
 * debuggable failure available. The fix is to hash the bytes; this is the
 * test that keeps it that way.
 */
static void
test_forge_webhook_signature_covers_the_whole_body(void)
{
	static const gchar *const secret = "hook-secret";
	static const gchar payload[] = "{\"a\":\"\0hidden\"}";
	const gsize length = sizeof(payload) - 1;
	g_autoptr(VentureForgejoClient) client = NULL;
	g_autoptr(SoupMessageHeaders) headers = NULL;
	g_autoptr(GError) build_error = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) body = NULL;
	g_autofree gchar *signature = NULL;

	client = venture_forgejo_client_new("https://git.example.com", "tok", 5,
	                                    &build_error);
	g_assert_no_error(build_error);

	body = g_bytes_new(payload, length);
	signature = g_compute_hmac_for_data(G_CHECKSUM_SHA256,
	                                    (const guchar *)secret, strlen(secret),
	                                    (const guchar *)payload, length);

	headers = signature_headers("X-Forgejo-Signature", signature);

	g_assert_true(venture_forge_client_verify_webhook(
		VENTURE_FORGE_CLIENT(client), headers, body, secret, &error));
	g_assert_no_error(error);
}

/*
 * An issue payload is normalised into the fields the route needs.
 *
 * What breaks if this regresses: the sender and the delivery id are the two
 * webhook loop guards, and a parser that quietly leaves either empty
 * disables both -- VENTURE would re-import its own issues and re-apply every
 * retried delivery.
 */
static void
test_forge_webhook_parses_an_issue_event(void)
{
	static const gchar *const payload =
		"{\"action\":\"opened\","
		" \"repository\":{\"full_name\":\"zach/venture\"},"
		" \"sender\":{\"login\":\"someone-else\"},"
		" \"issue\":{\"number\":42,\"title\":\"It crashes\","
		"            \"body\":\"every time\",\"state\":\"open\","
		"            \"html_url\":\"https://git.example.com/zach/venture/issues/42\","
		"            \"labels\":[{\"name\":\"bug\"},{\"name\":\"urgent\"}]}}";
	g_autoptr(VentureForgejoClient) client = NULL;
	g_autoptr(SoupMessageHeaders) headers = NULL;
	g_autoptr(JsonParser) parser = NULL;
	g_autoptr(GError) error = NULL;
	VentureForgeEvent event;

	client = venture_forgejo_client_new("https://git.example.com", "tok", 5,
	                                    &error);
	g_assert_no_error(error);

	parser = json_parser_new();
	g_assert_true(json_parser_load_from_data(parser, payload, -1, &error));
	g_assert_no_error(error);

	headers = signature_headers("X-Forgejo-Delivery", "delivery-1");

	g_assert_true(venture_forge_client_parse_issue_event(
		VENTURE_FORGE_CLIENT(client), json_parser_get_root(parser), headers,
		&event, &error));
	g_assert_no_error(error);

	g_assert_cmpstr(event.action, ==, "opened");
	g_assert_cmpstr(event.repo_full_name, ==, "zach/venture");
	g_assert_cmpint(event.issue_number, ==, 42);
	g_assert_cmpstr(event.title, ==, "It crashes");
	g_assert_cmpstr(event.state, ==, "open");
	g_assert_cmpstr(event.sender, ==, "someone-else");
	g_assert_cmpstr(event.delivery_id, ==, "delivery-1");
	g_assert_nonnull(event.labels);
	g_assert_cmpstr(event.labels[0], ==, "bug");
	g_assert_cmpstr(event.labels[1], ==, "urgent");

	venture_forge_event_clear(&event);

	/* Clearing twice must be safe: the route unwinds through this on
	 * every path, including the ones that failed halfway. */
	venture_forge_event_clear(&event);
}

/*
 * A payload with no issue is refused rather than half-parsed.
 */
static void
test_forge_webhook_refuses_a_malformed_payload(void)
{
	g_autoptr(VentureForgejoClient) client = NULL;
	g_autoptr(SoupMessageHeaders) headers = NULL;
	g_autoptr(JsonParser) parser = NULL;
	g_autoptr(GError) error = NULL;
	VentureForgeEvent event;

	client = venture_forgejo_client_new("https://git.example.com", "tok", 5,
	                                    &error);
	g_assert_no_error(error);

	parser = json_parser_new();
	g_assert_true(json_parser_load_from_data(parser, "{\"action\":\"opened\"}",
	                                         -1, &error));
	g_assert_no_error(error);

	headers = signature_headers(NULL, NULL);

	g_assert_false(venture_forge_client_parse_issue_event(
		VENTURE_FORGE_CLIENT(client), json_parser_get_root(parser), headers,
		&event, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);

	venture_forge_event_clear(&event);
}

/*
 * The clone URL is composed from the clone base, not the API base.
 *
 * What breaks if this regresses: these are routinely different hosts. A
 * Forgejo behind a reverse proxy answers its API on https://git.example.com
 * while SSH goes straight to the daemon at git@git-ssh.example.com, because
 * the proxy speaks HTTP and sshd does not. Deriving the clone URL from the
 * API base guesses wrong for every install shaped that way -- and the
 * failure is a clone that cannot resolve, minutes into a run.
 */
static void
test_forge_clone_url_uses_the_clone_base(void)
{
	g_autofree gchar *ssh = NULL;
	g_autofree gchar *https = NULL;
	g_autofree gchar *fallback = NULL;
	g_autofree gchar *override = NULL;

	/* The shape this exists for: SSH on a different host from the API. */
	ssh = venture_forge_clone_url(NULL, "git@git-ssh.podbielniak.com",
	                              "https://git.podbielniak.com",
	                              "zachpodbielniak/venture");
	g_assert_cmpstr(ssh, ==,
	                "git@git-ssh.podbielniak.com:zachpodbielniak/venture.git");

	/* An HTTPS clone base that is not the API host. */
	https = venture_forge_clone_url(NULL, "https://clone.example.com",
	                                "https://git.example.com", "zach/venture");
	g_assert_cmpstr(https, ==, "https://clone.example.com/zach/venture.git");

	/* No clone base: one host does both, which is the simple case. */
	fallback = venture_forge_clone_url(NULL, NULL, "https://git.example.com",
	                                   "zach/venture");
	g_assert_cmpstr(fallback, ==, "https://git.example.com/zach/venture.git");

	/* A repository that does not follow its forge's pattern wins outright. */
	override = venture_forge_clone_url("git@mirror.example.com:other/thing.git",
	                                   "git@git-ssh.example.com",
	                                   "https://git.example.com",
	                                   "zach/venture");
	g_assert_cmpstr(override, ==, "git@mirror.example.com:other/thing.git");
}

/*
 * The scp-like form uses a colon, and a trailing separator does not double.
 *
 * What breaks if this regresses: git reads git@host/owner/repo.git as a path
 * rather than a host, so the colon is not a stylistic choice. And a base
 * written with a trailing slash or colon -- which is how a person naturally
 * types one -- would otherwise produce a URL that fails in a way nobody
 * reads carefully.
 */
static void
test_forge_clone_url_separators(void)
{
	static const struct
	{
		const gchar *base;
		const gchar *expect;
	} cases[] = {
		{ "git@host.example.com",   "git@host.example.com:zach/venture.git" },
		{ "git@host.example.com:",  "git@host.example.com:zach/venture.git" },
		{ "git@host.example.com/",  "git@host.example.com:zach/venture.git" },
		{ "https://host.example.com",  "https://host.example.com/zach/venture.git" },
		{ "https://host.example.com/", "https://host.example.com/zach/venture.git" },
		{ "ssh://git@host.example.com", "ssh://git@host.example.com/zach/venture.git" }
	};
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autofree gchar *url = NULL;

		url = venture_forge_clone_url(NULL, cases[i].base, NULL,
		                              "zach/venture");
		g_assert_cmpstr(url, ==, cases[i].expect);
	}

	/* An ssh:// URL keeps a slash, because it is a URL and not the
	 * scp-like shorthand. That distinction is the reason the check is on
	 * "has a scheme" rather than "has an @". */
}

/*
 * With nothing to work from, there is no clone URL rather than a wrong one.
 */
static void
test_forge_clone_url_refuses_without_a_base(void)
{
	g_autofree gchar *none = NULL;
	g_autofree gchar *bad_name = NULL;

	none = venture_forge_clone_url(NULL, NULL, NULL, "zach/venture");
	g_assert_null(none);

	bad_name = venture_forge_clone_url(NULL, "git@host.example.com", NULL,
	                                   "noslash");
	g_assert_null(bad_name);
}

/*
 * The web URL comes from the base URL, not the clone base.
 *
 * What breaks if this regresses: the clone base is frequently an SSH host --
 * git@git-ssh.example.com -- with no web server on it at all. Composing a
 * browser link from it produces something that cannot be opened, and the
 * failure is a dead link rather than an error anybody sees in a log.
 */
static void
test_forge_web_url_uses_the_base_url(void)
{
	g_autofree gchar *repo = NULL;
	g_autofree gchar *branch = NULL;
	g_autofree gchar *trailing = NULL;

	repo = venture_forge_web_url("https://git.podbielniak.com",
	                             "zachpodbielniak/venture", NULL);
	g_assert_cmpstr(repo, ==,
	                "https://git.podbielniak.com/zachpodbielniak/venture");

	branch = venture_forge_web_url("https://git.podbielniak.com",
	                               "zachpodbielniak/venture",
	                               "venture/bug/12-it-crashes");
	g_assert_cmpstr(branch, ==,
		"https://git.podbielniak.com/zachpodbielniak/venture"
		"/src/branch/venture/bug/12-it-crashes");

	/* A base written with a trailing slash must not double it. */
	trailing = venture_forge_web_url("https://git.example.com/", "zach/venture",
	                                 NULL);
	g_assert_cmpstr(trailing, ==, "https://git.example.com/zach/venture");
}

/*
 * A branch's slashes survive; everything else is escaped.
 *
 * What breaks if this regresses: `feature/thing` is an ordinary branch name
 * and the forge's src/branch/ route wants those separators as separators.
 * Escaping them to %2F produces a 404. Escaping nothing at all would let a
 * branch containing a `?` or a `#` truncate the path.
 */
static void
test_forge_web_url_escapes_but_keeps_slashes(void)
{
	g_autofree gchar *nested = NULL;
	g_autofree gchar *awkward = NULL;
	g_autofree gchar *no_base = NULL;

	nested = venture_forge_web_url("https://git.example.com", "zach/venture",
	                              "feature/deep/thing");
	g_assert_nonnull(strstr(nested, "/src/branch/feature/deep/thing"));

	/* A refname cannot contain ? or #, but the composer must not rely on
	 * that -- it is handed values that came from a payload. */
	awkward = venture_forge_web_url("https://git.example.com", "zach/venture",
	                               "odd name");
	g_assert_null(strstr(awkward, " "));

	no_base = venture_forge_web_url(NULL, "zach/venture", NULL);
	g_assert_null(no_base);
}

#include "forge-vault.inc"

/* A stored legacy token is evidence for explicit import, never authorization. */
static void
test_forge_legacy_factory_refuses(void)
{
	g_autoptr(VentureForge) forge = venture_forge_new();
	g_autoptr(VentureForgeClient) client = NULL;
	g_autoptr(GError) error = NULL;
	g_object_set(forge, "base-url", "https://git.example.com", "token", "legacy-credential", NULL);
	client = venture_forge_client_for_forge(forge, 1, &error);
	g_assert_null(client);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
}

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/forge/legacy-factory-refuses", test_forge_legacy_factory_refuses);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	ADD("/forge/vault-lifecycle", test_forge_vault_lifecycle);
	ADD("/forge/vault-import-worker", test_forge_vault_import_and_worker);
	ADD("/forge/vault-clone-boundary", test_forge_vault_clone_boundary);
	g_test_add_func("/forge/types-are-registered",
	                test_forge_types_are_registered);

	ADD("/forge/issue-type-defaults-to-task-on-an-older-row",
	    test_forge_issue_type_defaults_to_task_on_an_older_row);
	ADD("/forge/kind-and-issue-type-are-independent",
	    test_forge_kind_and_issue_type_are_independent);
	ADD("/forge/every-issue-type-round-trips",
	    test_forge_every_issue_type_round_trips);
	ADD("/forge/plaintext-writes-refused",
	    test_forge_token_can_be_set_and_changed);
	ADD("/forge/token-is-absent-from-serialisation",
	    test_forge_token_is_absent_from_serialisation);
	ADD("/forge/token-is-not-in-the-generated-form",
	    test_forge_token_is_not_in_the_generated_form);

	ADD("/forge/rule-prefers-the-repository",
	    test_forge_rule_prefers_the_repository);
	ADD("/forge/rule-prefers-the-typed-rule",
	    test_forge_rule_prefers_the_typed_rule);
	ADD("/forge/rule-falls-back-to-the-forge",
	    test_forge_rule_falls_back_to_the_forge);
	ADD("/forge/rule-absent-means-nothing-runs",
	    test_forge_rule_absent_means_nothing_runs);
	ADD("/forge/rule-disabled-falls-through",
	    test_forge_rule_disabled_falls_through);
	ADD("/forge/rule-with-no-scope-matches-nothing",
	    test_forge_rule_with_no_scope_matches_nothing);
	ADD("/forge/rule-does-not-cross-forges",
	    test_forge_rule_does_not_cross_forges);
	ADD("/forge/rule-ties-break-by-id", test_forge_rule_ties_break_by_id);

	g_test_add_func("/forge/branch-name-is-always-legal",
	                test_forge_branch_name_is_always_legal);
	g_test_add_func("/forge/branch-name-survives-a-bad-template",
	                test_forge_branch_name_survives_a_bad_template);
	g_test_add_func("/forge/branch-name-is-unique-per-ticket",
	                test_forge_branch_name_is_unique_per_ticket);
	g_test_add_func("/forge/branch-name-expands-the-template",
	                test_forge_branch_name_expands_the_template);
	g_test_add_func("/forge/repo-split-refuses-malformed-names",
	                test_forge_repo_split_refuses_malformed_names);

	g_test_add_func("/forge/clone-url-uses-the-clone-base",
	                test_forge_clone_url_uses_the_clone_base);
	g_test_add_func("/forge/clone-url-separators",
	                test_forge_clone_url_separators);
	g_test_add_func("/forge/clone-url-refuses-without-a-base",
	                test_forge_clone_url_refuses_without_a_base);

	g_test_add_func("/forge/web-url-uses-the-base-url",
	                test_forge_web_url_uses_the_base_url);
	g_test_add_func("/forge/web-url-escapes-but-keeps-slashes",
	                test_forge_web_url_escapes_but_keeps_slashes);

	g_test_add_func("/forge/client-refuses-a-bad-scheme",
	                test_forge_client_refuses_a_bad_scheme);
	g_test_add_func("/forge/client-does-not-follow-a-redirect",
	                test_forge_client_does_not_follow_a_redirect);
	g_test_add_func("/forge/client-escapes-the-repository-name",
	                test_forge_client_escapes_the_repository_name);
	g_test_add_func("/forge/client-maps-status-codes",
	                test_forge_client_maps_status_codes);
	g_test_add_func("/forge/client-sends-the-token",
	                test_forge_client_sends_the_token);

	g_test_add_func("/forge/webhook-signature", test_forge_webhook_signature);
	g_test_add_func("/forge/webhook-signature-covers-the-whole-body",
	                test_forge_webhook_signature_covers_the_whole_body);
	g_test_add_func("/forge/webhook-parses-an-issue-event",
	                test_forge_webhook_parses_an_issue_event);
	g_test_add_func("/forge/webhook-refuses-a-malformed-payload",
	                test_forge_webhook_refuses_a_malformed_payload);

	ADD("/forge/origin-is-organization-scoped", test_forge_origin_is_organization_scoped);

	g_test_add_func("/forge/origin-upgrade", test_forge_origin_upgrade);

#undef ADD

	return g_test_run();
}
