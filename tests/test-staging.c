/*
 * test-staging.c - Writes proposed but not yet made
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * The confirmation queue is what lets an agent holding an API token propose
 * a change rather than make one, so the properties worth pinning are the
 * ones somebody relies on when they approve a card without re-reading the
 * request that produced it:
 *
 *   - staging changes nothing, which is asserted by hashing the whole record
 *     set before and after rather than by looking at the one record the test
 *     happens to know about;
 *   - approving applies what was staged, not what the record looks like now,
 *     and a record that moved underneath is a reported conflict rather than
 *     a silent overwrite;
 *   - the audit trail can tell a staged-then-approved change from a direct
 *     one and from the assistant's own, because "who did that" is the whole
 *     reason the trail exists;
 *   - a change nobody answers goes away instead of accumulating.
 *
 * Nothing here opens a socket or spawns anything: the store is a library
 * object over an in-memory database. `unshare -rn ./build/debug/tests/
 * test-staging` demonstrates it.
 */

#include <venture.h>

#include <string.h>

typedef struct
{
	VentureDatabase			*database;
	VentureConfig			*config;
	VentureContext			*context;
	VentureConfirmationStore	*store;
	gint64				 organization_id;
} Fixture;

/* --- Fixture -------------------------------------------------------------- */

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureOrganization) organization = NULL;
	g_autoptr(GError) error = NULL;

	(void)user_data;

	fixture->config = venture_config_new();
	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);

	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	/*
	 * The real context, not a store built by hand: an install with AI
	 * switched off must still stage, and building the store here would
	 * pass whether or not the context creates one.
	 */
	fixture->context = venture_context_new(fixture->config,
	                                       fixture->database);
	fixture->store = venture_context_get_confirmations(fixture->context);
	g_assert_nonnull(fixture->store);

	organization = venture_organization_new();
	g_object_set(organization, "name", "Ironwood", "slug", "ironwood", NULL);
	g_assert_true(venture_database_save(fixture->database,
		VENTURE_ENTITY(organization), NULL, &error));
	g_assert_no_error(error);

	fixture->organization_id =
		venture_entity_get_id(VENTURE_ENTITY(organization));
}

static void
fixture_tear_down(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	(void)user_data;

	fixture->store = NULL;
	g_clear_object(&fixture->context);
	g_clear_object(&fixture->database);
	g_clear_object(&fixture->config);
}

/* --- Helpers -------------------------------------------------------------- */

/*
 * A checksum over every record in the database, soft-deleted ones included.
 *
 * Asserting that one expense did not change would pass a staging bug that
 * wrote a different record, or an audit row, or the organisation. The claim
 * being tested is "nothing changed", so the check has to be over everything;
 * audit entries are in it deliberately, because staging must not write one
 * either.
 */
static gchar *
fixture_fingerprint(Fixture *fixture)
{
	g_autoptr(GChecksum) checksum = NULL;
	g_auto(GStrv) names = NULL;
	guint i;

	checksum = g_checksum_new(G_CHECKSUM_SHA256);
	names = venture_entity_registry_list_names(
		venture_context_get_entity_registry(fixture->context));

	for (i = 0; (NULL != names) && (NULL != names[i]); i++)
	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) records = NULL;
		guint r;

		query = venture_query_new_for_name(
			venture_context_get_entity_registry(fixture->context), names[i],
			NULL);

		if (NULL == query)
			continue;

		venture_query_set_limit(query, 0);
		venture_query_set_include_deleted(query, TRUE);
		venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);

		records = venture_database_find(fixture->database, query, NULL);

		g_checksum_update(checksum, (const guchar *)names[i],
		                  (gssize)strlen(names[i]));

		for (r = 0; (NULL != records) && (r < records->len); r++)
		{
			g_autoptr(JsonNode) node = NULL;
			g_autofree gchar *text = NULL;

			node = venture_serializable_to_json(
				VENTURE_SERIALIZABLE(g_ptr_array_index(records, r)), FALSE);
			text = venture_json_to_string(node, FALSE);

			g_checksum_update(checksum, (const guchar *)text,
			                  (gssize)strlen(text));
		}
	}

	return g_strdup(g_checksum_get_string(checksum));
}

/*
 * How many changes are waiting. Through the listing rather than a counter of
 * its own, so a test cannot pass against a store that counts one thing and
 * lists another.
 */
static guint
confirmations_waiting(VentureConfirmationStore *store)
{
	g_autoptr(GPtrArray) pending = NULL;

	pending = venture_confirmation_store_list_pending(store);

	return (NULL != pending) ? pending->len : 0;
}

static VentureExpense *
fixture_create_expense(
	Fixture		*fixture,
	const gchar	*description,
	const gchar	*amount
){
	g_autoptr(VentureMoney) money = NULL;
	g_autoptr(GError) error = NULL;
	VentureExpense *expense;

	expense = venture_expense_new();
	money = venture_money_from_string(amount, "USD", &error);
	g_assert_no_error(error);

	g_object_set(expense, "description", description, "amount", money, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(expense),
	                                   fixture->organization_id);

	g_assert_true(venture_database_save(fixture->database,
		VENTURE_ENTITY(expense), NULL, &error));
	g_assert_no_error(error);

	return expense;
}

/*
 * The actor an agent holding an API token would arrive as.
 */
static void
fixture_agent_actor(VentureActor *actor)
{
	actor->kind = VENTURE_ACTOR_KIND_IMPORT;
	actor->name = "bookkeeper-token";
	actor->prompt = NULL;
	actor->request_id = NULL;
	actor->approved_by = NULL;
}

static GPtrArray *
fixture_audit_entries(Fixture *fixture)
{
	g_autoptr(VentureQuery) query = NULL;

	query = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
	venture_query_set_limit(query, 0);

	return venture_database_find(fixture->database, query, NULL);
}

/* --- Staging changes nothing ---------------------------------------------- */

/*
 * What breaks if this regresses: the entire proposition. A staged write that
 * writes is a write with a confirmation card beside it, and the card reads
 * as a request for permission that was already taken.
 */
static void
test_staging_creates_nothing(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureExpense) expense = NULL;
	g_autoptr(VentureMoney) money = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autoptr(GError) error = NULL;
	VentureConfirmation *confirmation;
	VentureActor origin;

	(void)user_data;

	before = fixture_fingerprint(fixture);

	expense = venture_expense_new();
	money = venture_money_from_string("12.00", "USD", NULL);
	g_object_set(expense, "description", "Coffee grinder", "amount", money, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(expense),
	                                   fixture->organization_id);

	fixture_agent_actor(&origin);
	confirmation = venture_confirmation_store_stage(fixture->store,
		VENTURE_AUDIT_ACTION_CREATE, VENTURE_ENTITY(expense), NULL, &origin,
		"rest-api", &error);

	g_assert_no_error(error);
	g_assert_nonnull(confirmation);

	after = fixture_fingerprint(fixture);
	g_assert_cmpstr(before, ==, after);

	/* And the record really has no row, rather than a row the
	 * fingerprint happened not to reach. */
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(expense)), ==, 0);
}

static void
test_staging_updates_nothing(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureExpense) expense = NULL;
	g_autoptr(VentureEntity) edited = NULL;
	g_autoptr(VentureEntity) original = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor origin;
	gint64 id;

	(void)user_data;

	expense = fixture_create_expense(fixture, "Cover art", "250.00");
	id = venture_entity_get_id(VENTURE_ENTITY(expense));

	before = fixture_fingerprint(fixture);

	edited = venture_database_get(fixture->database, VENTURE_TYPE_EXPENSE, id,
	                              NULL);
	original = venture_database_get(fixture->database, VENTURE_TYPE_EXPENSE,
	                                id, NULL);
	g_object_set(edited, "description", "Cover art, revised", NULL);

	fixture_agent_actor(&origin);
	g_assert_nonnull(venture_confirmation_store_stage(fixture->store,
		VENTURE_AUDIT_ACTION_UPDATE, edited, original, &origin, "rest-api",
		&error));
	g_assert_no_error(error);

	after = fixture_fingerprint(fixture);
	g_assert_cmpstr(before, ==, after);
}

static void
test_staging_deletes_nothing(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureExpense) expense = NULL;
	g_autoptr(VentureEntity) target = NULL;
	g_autoptr(VentureEntity) original = NULL;
	g_autofree gchar *before = NULL;
	g_autofree gchar *after = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor origin;
	gint64 id;

	(void)user_data;

	expense = fixture_create_expense(fixture, "Duplicate row", "9.99");
	id = venture_entity_get_id(VENTURE_ENTITY(expense));

	before = fixture_fingerprint(fixture);

	target = venture_database_get(fixture->database, VENTURE_TYPE_EXPENSE, id,
	                              NULL);
	original = venture_database_get(fixture->database, VENTURE_TYPE_EXPENSE,
	                                id, NULL);

	fixture_agent_actor(&origin);
	g_assert_nonnull(venture_confirmation_store_stage(fixture->store,
		VENTURE_AUDIT_ACTION_DELETE, target, original, &origin, "rest-api",
		&error));
	g_assert_no_error(error);

	after = fixture_fingerprint(fixture);
	g_assert_cmpstr(before, ==, after);
}

/* --- Approving applies exactly what was staged ---------------------------- */

static void
test_staging_approve_applies_the_create(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureExpense) expense = NULL;
	g_autoptr(VentureMoney) money = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) records = NULL;
	g_autoptr(GError) error = NULL;
	VentureConfirmation *confirmation;
	VentureActor origin;

	(void)user_data;

	expense = venture_expense_new();
	money = venture_money_from_string("12.00", "USD", NULL);
	g_object_set(expense, "description", "Coffee grinder", "amount", money, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(expense),
	                                   fixture->organization_id);

	fixture_agent_actor(&origin);
	confirmation = venture_confirmation_store_stage(fixture->store,
		VENTURE_AUDIT_ACTION_CREATE, VENTURE_ENTITY(expense), NULL, &origin,
		"rest-api", NULL);
	g_assert_nonnull(confirmation);

	g_assert_true(venture_confirmation_store_approve(fixture->store,
		venture_confirmation_get_id(confirmation), "zach", &error));
	g_assert_no_error(error);

	query = venture_query_new(VENTURE_TYPE_EXPENSE);
	records = venture_database_find(fixture->database, query, NULL);

	g_assert_nonnull(records);
	g_assert_cmpuint(records->len, ==, 1);

	{
		g_autofree gchar *description = NULL;

		g_object_get(g_ptr_array_index(records, 0), "description", &description, NULL);
		g_assert_cmpstr(description, ==, "Coffee grinder");
	}

	/* Decided, so it is gone from the queue rather than sitting there
	 * looking like it still needs answering. */
	g_assert_cmpuint(confirmations_waiting(fixture->store),
	                 ==, 0);
}

/*
 * What breaks if this regresses: approval re-reading the record and
 * reapplying the *instruction* rather than the staged object. The two differ
 * whenever anything else touched the row, and the difference is a value
 * nobody was shown being written.
 */
static void
test_staging_approve_applies_the_staged_object(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureExpense) expense = NULL;
	g_autoptr(VentureEntity) edited = NULL;
	g_autoptr(VentureEntity) original = NULL;
	g_autoptr(VentureEntity) applied = NULL;
	g_autoptr(VentureMoney) staged_amount = NULL;
	g_autoptr(GError) error = NULL;
	VentureConfirmation *confirmation;
	VentureActor origin;
	gint64 id;

	(void)user_data;

	expense = fixture_create_expense(fixture, "Cover art", "250.00");
	id = venture_entity_get_id(VENTURE_ENTITY(expense));

	edited = venture_database_get(fixture->database, VENTURE_TYPE_EXPENSE, id,
	                              NULL);
	original = venture_database_get(fixture->database, VENTURE_TYPE_EXPENSE,
	                                id, NULL);

	staged_amount = venture_money_from_string("260.00", "USD", NULL);
	g_object_set(edited, "amount", staged_amount, NULL);

	fixture_agent_actor(&origin);
	confirmation = venture_confirmation_store_stage(fixture->store,
		VENTURE_AUDIT_ACTION_UPDATE, edited, original, &origin, "rest-api",
		NULL);
	g_assert_nonnull(confirmation);

	g_assert_true(venture_confirmation_store_approve(fixture->store,
		venture_confirmation_get_id(confirmation), "zach", &error));
	g_assert_no_error(error);

	applied = venture_database_get(fixture->database, VENTURE_TYPE_EXPENSE, id,
	                              NULL);
	g_assert_nonnull(applied);

	{
		g_autoptr(VentureMoney) amount = NULL;
		g_autofree gchar *text = NULL;

		g_object_get(applied, "amount", &amount, NULL);
		text = venture_money_to_string(amount);
		g_assert_cmpstr(text, ==, "260.00 USD");
	}
}

static void
test_staging_approve_applies_the_delete(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureExpense) expense = NULL;
	g_autoptr(VentureEntity) target = NULL;
	g_autoptr(VentureEntity) original = NULL;
	g_autoptr(VentureEntity) after = NULL;
	g_autoptr(GError) error = NULL;
	VentureConfirmation *confirmation;
	VentureActor origin;
	gint64 id;

	(void)user_data;

	expense = fixture_create_expense(fixture, "Wrong row", "1.00");
	id = venture_entity_get_id(VENTURE_ENTITY(expense));

	target = venture_database_get(fixture->database, VENTURE_TYPE_EXPENSE, id,
	                              NULL);
	original = venture_database_get(fixture->database, VENTURE_TYPE_EXPENSE,
	                                id, NULL);

	fixture_agent_actor(&origin);
	confirmation = venture_confirmation_store_stage(fixture->store,
		VENTURE_AUDIT_ACTION_DELETE, target, original, &origin, "rest-api",
		NULL);
	g_assert_nonnull(confirmation);

	g_assert_true(venture_confirmation_store_approve(fixture->store,
		venture_confirmation_get_id(confirmation), "zach", &error));
	g_assert_no_error(error);

	/* Soft, like every delete here: the row is still fetchable by id and
	 * marked, which is what makes the deletion recoverable. */
	after = venture_database_get(fixture->database, VENTURE_TYPE_EXPENSE, id,
	                             NULL);
	g_assert_nonnull(after);
	g_assert_true(venture_entity_is_deleted(after));

	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) records = NULL;

		query = venture_query_new(VENTURE_TYPE_EXPENSE);
		records = venture_database_find(fixture->database, query, NULL);
		g_assert_cmpuint(records->len, ==, 0);
	}
}

/* --- A record that moved underneath --------------------------------------- */

/*
 * What breaks if this regresses: a change approved an hour after it was
 * proposed silently reverting whatever somebody did to the record in that
 * hour. The staged object carries every field, not only the ones the agent
 * meant to touch, so applying it over a newer row rewrites the lot.
 *
 * The refusal has to name what moved. "Somebody changed it" leaves the
 * person holding the card to go and work out whether their change still
 * makes sense, which is the work the message could have done.
 */
static void
test_staging_stale_approval_is_a_conflict(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureExpense) expense = NULL;
	g_autoptr(VentureEntity) edited = NULL;
	g_autoptr(VentureEntity) original = NULL;
	g_autoptr(VentureEntity) meanwhile = NULL;
	g_autoptr(VentureEntity) after = NULL;
	g_autoptr(VentureMoney) staged_amount = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *confirmation_id = NULL;
	VentureConfirmation *confirmation;
	VentureActor origin;
	gint64 id;

	(void)user_data;

	expense = fixture_create_expense(fixture, "Cover art", "250.00");
	id = venture_entity_get_id(VENTURE_ENTITY(expense));

	edited = venture_database_get(fixture->database, VENTURE_TYPE_EXPENSE, id,
	                              NULL);
	original = venture_database_get(fixture->database, VENTURE_TYPE_EXPENSE,
	                                id, NULL);

	staged_amount = venture_money_from_string("260.00", "USD", NULL);
	g_object_set(edited, "amount", staged_amount, NULL);

	fixture_agent_actor(&origin);
	confirmation = venture_confirmation_store_stage(fixture->store,
		VENTURE_AUDIT_ACTION_UPDATE, edited, original, &origin, "rest-api",
		NULL);
	g_assert_nonnull(confirmation);

	/* Copied, because deciding a confirmation frees it. */
	confirmation_id = g_strdup(venture_confirmation_get_id(confirmation));

	/* Somebody else edits the row while the card is waiting. */
	meanwhile = venture_database_get(fixture->database, VENTURE_TYPE_EXPENSE,
	                                 id, NULL);
	g_object_set(meanwhile, "description", "Cover art (final)", NULL);
	g_assert_true(venture_database_save(fixture->database, meanwhile, NULL,
	                                    NULL));

	g_assert_false(venture_confirmation_store_approve(fixture->store,
		confirmation_id, "zach", &error));

	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	/* Named, not merely reported. */
	g_assert_nonnull(strstr(error->message, "description"));

	/* And the other person's edit survived: the staged amount was not
	 * written and the newer memo is still there. */
	after = venture_database_get(fixture->database, VENTURE_TYPE_EXPENSE, id,
	                            NULL);

	{
		g_autoptr(VentureMoney) amount = NULL;
		g_autofree gchar *text = NULL;
		g_autofree gchar *description = NULL;

		g_object_get(after, "amount", &amount, "description", &description, NULL);
		text = venture_money_to_string(amount);

		g_assert_cmpstr(text, ==, "250.00 USD");
		g_assert_cmpstr(description, ==, "Cover art (final)");
	}

	/*
	 * The confirmation is gone. Its staged object is pinned to a version
	 * the row has moved past, so a second approval could not succeed
	 * either, and a card that can only ever fail is worse than none.
	 */
	g_assert_null(venture_confirmation_store_find(fixture->store,
		confirmation_id));
}

/* --- Rejection ------------------------------------------------------------ */

static void
test_staging_reject_discards_and_records(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureExpense) expense = NULL;
	g_autoptr(VentureMoney) money = NULL;
	g_autoptr(GPtrArray) entries = NULL;
	g_autoptr(GError) error = NULL;
	VentureConfirmation *confirmation;
	VentureActor origin;
	gboolean found;
	guint i;

	(void)user_data;

	expense = venture_expense_new();
	money = venture_money_from_string("400.00", "USD", NULL);
	g_object_set(expense, "description", "Not ours", "amount", money, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(expense),
	                                   fixture->organization_id);

	fixture_agent_actor(&origin);
	confirmation = venture_confirmation_store_stage(fixture->store,
		VENTURE_AUDIT_ACTION_CREATE, VENTURE_ENTITY(expense), NULL, &origin,
		"rest-api", NULL);
	g_assert_nonnull(confirmation);

	g_assert_true(venture_confirmation_store_reject(fixture->store,
		venture_confirmation_get_id(confirmation), "zach", &error));
	g_assert_no_error(error);

	g_assert_cmpuint(confirmations_waiting(fixture->store),
	                 ==, 0);

	{
		g_autoptr(VentureQuery) query = NULL;
		g_autoptr(GPtrArray) records = NULL;

		query = venture_query_new(VENTURE_TYPE_EXPENSE);
		records = venture_database_find(fixture->database, query, NULL);
		g_assert_cmpuint(records->len, ==, 0);
	}

	/*
	 * Knowing what an agent proposed and was denied is as useful as
	 * knowing what it did -- a refusal nobody can find afterwards makes
	 * "has it been trying to do that all week" unanswerable.
	 */
	entries = fixture_audit_entries(fixture);
	found = FALSE;

	for (i = 0; i < entries->len; i++)
	{
		VentureAuditAction action;
		g_autofree gchar *approver = NULL;

		g_object_get(g_ptr_array_index(entries, i), "action", &action,
		             "approved-by", &approver, NULL);

		if ((VENTURE_AUDIT_ACTION_REJECT == action) &&
		    (0 == g_strcmp0(approver, "zach")))
			found = TRUE;
	}

	g_assert_true(found);
}

/* --- The audit trail ------------------------------------------------------ */

/*
 * What breaks if this regresses: "did somebody approve this, or did it just
 * happen" stops being answerable. VENTURE's automation story is that the
 * trail says who did what; a staged change that lands looking exactly like a
 * direct one gives an agent's write the appearance of a person's.
 */
static void
test_staging_audit_distinguishes_staged_from_direct(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureExpense) direct = NULL;
	g_autoptr(VentureExpense) proposed = NULL;
	g_autoptr(VentureMoney) money = NULL;
	g_autoptr(GPtrArray) entries = NULL;
	g_autofree gchar *confirmation_id = NULL;
	VentureConfirmation *confirmation;
	VentureActor origin;
	VentureActor person;
	gboolean saw_direct;
	gboolean saw_staged;
	guint i;

	(void)user_data;

	/* A person writing straight through the API. */
	person.kind = VENTURE_ACTOR_KIND_USER;
	person.name = "zach";
	person.prompt = NULL;
	person.request_id = NULL;
	person.approved_by = NULL;

	direct = venture_expense_new();
	money = venture_money_from_string("5.00", "USD", NULL);
	g_object_set(direct, "description", "Straight through", "amount", money, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(direct),
	                                   fixture->organization_id);
	g_assert_true(venture_database_save(fixture->database,
		VENTURE_ENTITY(direct), &person, NULL));

	/* And an agent proposing one that a person then approves. */
	proposed = venture_expense_new();
	g_object_set(proposed, "description", "Proposed", "amount", money, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(proposed),
	                                   fixture->organization_id);

	fixture_agent_actor(&origin);
	confirmation = venture_confirmation_store_stage(fixture->store,
		VENTURE_AUDIT_ACTION_CREATE, VENTURE_ENTITY(proposed), NULL, &origin,
		"rest-api", NULL);
	g_assert_nonnull(confirmation);

	/* Copied, because deciding a confirmation frees it -- and the id is
	 * exactly what the audit row has to carry. */
	confirmation_id = g_strdup(venture_confirmation_get_id(confirmation));

	g_assert_true(venture_confirmation_store_approve(fixture->store,
		confirmation_id, "zach", NULL));

	entries = fixture_audit_entries(fixture);
	saw_direct = FALSE;
	saw_staged = FALSE;

	for (i = 0; i < entries->len; i++)
	{
		g_autofree gchar *label = NULL;
		g_autofree gchar *approver = NULL;
		g_autofree gchar *actor = NULL;
		g_autofree gchar *request = NULL;
		VentureActorKind kind;

		g_object_get(g_ptr_array_index(entries, i),
		             "target-label", &label,
		             "approved-by", &approver,
		             "actor", &actor,
		             "request-id", &request,
		             "actor-kind", &kind,
		             NULL);

		if ((NULL != label) && (NULL != strstr(label, "Straight through")))
		{
			saw_direct = TRUE;

			/* A direct write has no approver and no confirmation. */
			g_assert_cmpstr(approver, ==, NULL);
			g_assert_cmpstr(request, ==, NULL);
			g_assert_cmpint(kind, ==, VENTURE_ACTOR_KIND_USER);
		}

		if ((NULL != label) && (NULL != strstr(label, "Proposed")))
		{
			saw_staged = TRUE;

			/* Both parties, and the confirmation that joined them. */
			g_assert_cmpstr(approver, ==, "zach");
			g_assert_cmpstr(actor, ==, "bookkeeper-token");
			g_assert_cmpstr(request, ==, confirmation_id);
		}
	}

	g_assert_true(saw_direct);
	g_assert_true(saw_staged);
}

/*
 * And apart from the assistant's own, which is the third case somebody
 * reading the trail has to be able to separate.
 */
static void
test_staging_audit_distinguishes_the_assistant(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureExpense) expense = NULL;
	g_autoptr(VentureMoney) money = NULL;
	g_autoptr(GPtrArray) entries = NULL;
	VentureConfirmation *confirmation;
	VentureActor origin;
	gboolean found;
	guint i;

	(void)user_data;

	expense = venture_expense_new();
	money = venture_money_from_string("18.00", "USD", NULL);
	g_object_set(expense, "description", "Read off a screenshot", "amount", money,
	             NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(expense),
	                                   fixture->organization_id);

	origin.kind = VENTURE_ACTOR_KIND_AI;
	origin.name = "claude-sonnet-5";
	origin.prompt = "file this receipt";
	origin.request_id = NULL;
	origin.approved_by = NULL;

	confirmation = venture_confirmation_store_stage(fixture->store,
		VENTURE_AUDIT_ACTION_CREATE, VENTURE_ENTITY(expense), NULL, &origin,
		"assistant", NULL);
	g_assert_nonnull(confirmation);

	g_assert_true(venture_confirmation_store_approve(fixture->store,
		venture_confirmation_get_id(confirmation), "zach", NULL));

	entries = fixture_audit_entries(fixture);
	found = FALSE;

	for (i = 0; i < entries->len; i++)
	{
		g_autofree gchar *prompt = NULL;
		g_autofree gchar *approver = NULL;
		VentureActorKind kind;

		g_object_get(g_ptr_array_index(entries, i), "prompt", &prompt,
		             "approved-by", &approver, "actor-kind", &kind, NULL);

		if (VENTURE_ACTOR_KIND_AI != kind)
			continue;

		/* The prompt that caused it, and the person who let it through. */
		g_assert_cmpstr(prompt, ==, "file this receipt");
		g_assert_cmpstr(approver, ==, "zach");
		found = TRUE;
	}

	g_assert_true(found);
}

/* --- What a card carries -------------------------------------------------- */

/*
 * What breaks if this regresses: a decision inbox in another program -- the
 * thing this route was built for -- cannot render the card. It has the
 * confirmation and nothing else: not the HTTP call that produced it, not the
 * conversation, not the record.
 */
static void
test_staging_card_stands_on_its_own(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureExpense) expense = NULL;
	g_autoptr(VentureEntity) edited = NULL;
	g_autoptr(VentureEntity) original = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autoptr(VentureMoney) staged_amount = NULL;
	VentureConfirmation *confirmation;
	VentureActor origin;
	JsonObject *object;
	JsonObject *from_origin;
	gint64 id;

	(void)user_data;

	expense = fixture_create_expense(fixture, "Cover art", "250.00");
	id = venture_entity_get_id(VENTURE_ENTITY(expense));

	edited = venture_database_get(fixture->database, VENTURE_TYPE_EXPENSE, id,
	                              NULL);
	original = venture_database_get(fixture->database, VENTURE_TYPE_EXPENSE,
	                                id, NULL);
	staged_amount = venture_money_from_string("260.00", "USD", NULL);
	g_object_set(edited, "amount", staged_amount, NULL);

	fixture_agent_actor(&origin);
	confirmation = venture_confirmation_store_stage(fixture->store,
		VENTURE_AUDIT_ACTION_UPDATE, edited, original, &origin, "rest-api",
		NULL);
	g_assert_nonnull(confirmation);

	node = venture_confirmation_to_json(confirmation);
	g_assert_true(JSON_NODE_HOLDS_OBJECT(node));
	object = json_node_get_object(node);

	/* Which type, which record, what kind of change. */
	g_assert_cmpstr(venture_json_object_get_string(object, "type", NULL), ==,
	                "expense");
	g_assert_cmpstr(venture_json_object_get_string(object, "action", NULL), ==,
	                "update");
	g_assert_cmpint(json_object_get_int_member(object, "record_id"), ==, id);

	/* What would change. */
	g_assert_true(json_object_has_member(object, "diff"));
	g_assert_true(json_object_has_member(
		json_object_get_object_member(object, "diff"), "amount"));

	/* Who asked, and how it reached us. */
	from_origin = json_object_get_object_member(object, "origin");
	g_assert_cmpstr(venture_json_object_get_string(from_origin, "name", NULL),
	                ==, "bookkeeper-token");
	g_assert_cmpstr(venture_json_object_get_string(from_origin, "via", NULL),
	                ==, "rest-api");

	/* When it stops waiting. */
	g_assert_true(json_object_has_member(object, "expires_at"));
}

/*
 * The prompt behind an assistant-staged change stays out of the card.
 *
 * What breaks if this regresses: /api/v1/confirmations is open to any
 * viewer, and a prompt is the text of somebody's private chat thread. The
 * chat routes go to some length to keep a thread to its owner; putting the
 * prompt on a card walks around all of it.
 */
static void
test_staging_card_withholds_the_prompt(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureExpense) expense = NULL;
	g_autoptr(JsonNode) node = NULL;
	g_autofree gchar *text = NULL;
	g_autoptr(VentureMoney) money = NULL;
	VentureConfirmation *confirmation;
	VentureActor origin;

	(void)user_data;

	expense = venture_expense_new();
	money = venture_money_from_string("18.00", "USD", NULL);
	g_object_set(expense, "description", "From a chat", "amount", money, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(expense),
	                                   fixture->organization_id);

	origin.kind = VENTURE_ACTOR_KIND_AI;
	origin.name = "claude-sonnet-5";
	origin.prompt = "the divorce settlement receipts, file them quietly";
	origin.request_id = NULL;
	origin.approved_by = NULL;

	confirmation = venture_confirmation_store_stage(fixture->store,
		VENTURE_AUDIT_ACTION_CREATE, VENTURE_ENTITY(expense), NULL, &origin,
		"assistant", NULL);
	g_assert_nonnull(confirmation);

	node = venture_confirmation_to_json(confirmation);
	text = venture_json_to_string(node, FALSE);

	g_assert_null(strstr(text, "divorce"));
}

/* --- Not accumulating for ever -------------------------------------------- */

/*
 * What breaks if this regresses: a server left running collects every change
 * nobody ever answered. The previous implementation marked an expired
 * confirmation and kept the object, so the table only ever grew -- and it
 * holds a whole record each.
 */
static void
test_staging_expires_and_is_dropped(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureConfirmationStore) store = NULL;
	g_autoptr(VentureExpense) expense = NULL;
	g_autoptr(VentureMoney) money = NULL;
	g_autoptr(GError) error = NULL;
	VentureConfirmation *confirmation;
	VentureActor origin;
	g_autofree gchar *id = NULL;

	(void)user_data;

	/* A lifetime in the past, so the sweep has something to do without
	 * the test sleeping through a real one. */
	store = venture_confirmation_store_new(fixture->database, -1, 0);

	expense = venture_expense_new();
	money = venture_money_from_string("3.00", "USD", NULL);
	g_object_set(expense, "description", "Nobody answered", "amount", money, NULL);
	venture_entity_set_organization_id(VENTURE_ENTITY(expense),
	                                   fixture->organization_id);

	fixture_agent_actor(&origin);
	confirmation = venture_confirmation_store_stage(store,
		VENTURE_AUDIT_ACTION_CREATE, VENTURE_ENTITY(expense), NULL, &origin,
		"rest-api", NULL);
	g_assert_nonnull(confirmation);

	id = g_strdup(venture_confirmation_get_id(confirmation));

	/*
	 * A negative lifetime is clamped to the default hour, so this one is
	 * still waiting: what is being pinned is that the clamp happens at
	 * all, since a store built with a nonsense TTL that expired
	 * everything instantly would discard every change silently.
	 */
	g_assert_cmpuint(confirmations_waiting(store), ==, 1);

	/* Now one that really has run out. */
	{
		g_autoptr(VentureConfirmationStore) expired = NULL;
		g_autoptr(VentureExpense) stale = NULL;

		expired = venture_confirmation_store_new(fixture->database, 1, 0);

		stale = venture_expense_new();
		g_object_set(stale, "description", "Left waiting", "amount", money, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(stale),
		                                   fixture->organization_id);

		confirmation = venture_confirmation_store_stage(expired,
			VENTURE_AUDIT_ACTION_CREATE, VENTURE_ENTITY(stale), NULL,
			&origin, "rest-api", NULL);
		g_assert_nonnull(confirmation);

		g_free(id);
		id = g_strdup(venture_confirmation_get_id(confirmation));

		g_assert_cmpuint(confirmations_waiting(expired),
		                 ==, 1);

		g_usleep(1100000);

		/* Gone, not merely marked: nothing is left holding the record. */
		g_assert_cmpuint(confirmations_waiting(expired),
		                 ==, 0);
		g_assert_null(venture_confirmation_store_find(expired, id));

		g_assert_false(venture_confirmation_store_approve(expired, id, "zach",
		                                                  &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	}
}

/*
 * What breaks if this regresses: the lifetime bounds how long one change
 * waits, and nothing bounds how many an agent in a retry loop piles up
 * inside that window. "It clears itself in an hour" is no comfort to a
 * server that ran out of memory in ten minutes.
 */
static void
test_staging_refuses_past_the_limit(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureConfirmationStore) store = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor origin;
	guint i;

	(void)user_data;

	store = venture_confirmation_store_new(fixture->database, 3600, 3);
	fixture_agent_actor(&origin);

	for (i = 0; i < 3; i++)
	{
		g_autoptr(VentureExpense) expense = NULL;
		g_autoptr(VentureMoney) money = NULL;
		g_autofree gchar *description = NULL;

		expense = venture_expense_new();
		money = venture_money_from_string("1.00", "USD", NULL);
		description = g_strdup_printf("Retry %u", i);
		g_object_set(expense, "description", description, "amount", money, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(expense),
		                                   fixture->organization_id);

		g_assert_nonnull(venture_confirmation_store_stage(store,
			VENTURE_AUDIT_ACTION_CREATE, VENTURE_ENTITY(expense), NULL,
			&origin, "rest-api", NULL));
	}

	{
		g_autoptr(VentureExpense) expense = NULL;
		g_autoptr(VentureMoney) money = NULL;

		expense = venture_expense_new();
		money = venture_money_from_string("1.00", "USD", NULL);
		g_object_set(expense, "description", "One too many", "amount", money, NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(expense),
		                                   fixture->organization_id);

		g_assert_null(venture_confirmation_store_stage(store,
			VENTURE_AUDIT_ACTION_CREATE, VENTURE_ENTITY(expense), NULL,
			&origin, "rest-api", &error));
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	}
}

/*
 * What breaks if this regresses: an agent that retries a call answering
 * "awaiting approval" rather than "ok" -- which they do -- leaves three
 * identical cards for one change, and approving all three makes three
 * records.
 */
static void
test_staging_restaging_keeps_one_card(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	VentureConfirmation *first;
	VentureConfirmation *second;
	VentureActor origin;
	guint i;

	(void)user_data;

	fixture_agent_actor(&origin);
	first = NULL;
	second = NULL;

	for (i = 0; i < 2; i++)
	{
		g_autoptr(VentureExpense) expense = NULL;
		g_autoptr(VentureMoney) money = NULL;
		VentureConfirmation *staged;

		expense = venture_expense_new();
		money = venture_money_from_string("12.00", "USD", NULL);
		g_object_set(expense, "description", "Coffee grinder", "amount", money,
		             NULL);
		venture_entity_set_organization_id(VENTURE_ENTITY(expense),
		                                   fixture->organization_id);

		staged = venture_confirmation_store_stage(fixture->store,
			VENTURE_AUDIT_ACTION_CREATE, VENTURE_ENTITY(expense), NULL,
			&origin, "rest-api", NULL);
		g_assert_nonnull(staged);

		if (0 == i)
			first = staged;
		else
			second = staged;
	}

	g_assert_cmpstr(venture_confirmation_get_id(first), ==,
	                venture_confirmation_get_id(second));
	g_assert_cmpuint(confirmations_waiting(fixture->store),
	                 ==, 1);
}

/* --- Reading the parameter ------------------------------------------------ */

/*
 * What breaks if this regresses: a spelling the server does not recognise,
 * read as "no", applies the write the caller was trying to hold back. The
 * refusal costs a retry; the permissive reading costs the record.
 */
static void
test_staging_flag_refuses_what_it_does_not_know(void)
{
	g_autoptr(GError) error = NULL;
	gboolean stage;

	g_assert_true(venture_confirmation_parse_stage_flag(NULL, &stage, NULL));
	g_assert_false(stage);

	g_assert_true(venture_confirmation_parse_stage_flag("1", &stage, NULL));
	g_assert_true(stage);

	g_assert_true(venture_confirmation_parse_stage_flag("true", &stage, NULL));
	g_assert_true(stage);

	g_assert_true(venture_confirmation_parse_stage_flag("0", &stage, NULL));
	g_assert_false(stage);

	g_assert_false(venture_confirmation_parse_stage_flag("y", &stage, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);

	/* And the refusal says which spellings work, so the caller can fix
	 * it without reading the source. */
	g_assert_nonnull(strstr(error->message, "stage=1"));
}

/* --- An update that changes nothing --------------------------------------- */

/*
 * Refused at staging rather than at approval. A card that says "Update
 * expense" with an empty diff asks somebody to approve nothing, and
 * approving it would short-circuit inside the repository and report success
 * without writing -- which reads as the change having been applied.
 */
static void
test_staging_refuses_an_empty_change(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureExpense) expense = NULL;
	g_autoptr(VentureEntity) edited = NULL;
	g_autoptr(VentureEntity) original = NULL;
	g_autoptr(GError) error = NULL;
	VentureActor origin;
	gint64 id;

	(void)user_data;

	expense = fixture_create_expense(fixture, "Unchanged", "10.00");
	id = venture_entity_get_id(VENTURE_ENTITY(expense));

	edited = venture_database_get(fixture->database, VENTURE_TYPE_EXPENSE, id,
	                              NULL);
	original = venture_database_get(fixture->database, VENTURE_TYPE_EXPENSE,
	                                id, NULL);

	fixture_agent_actor(&origin);

	g_assert_null(venture_confirmation_store_stage(fixture->store,
		VENTURE_AUDIT_ACTION_UPDATE, edited, original, &origin, "rest-api",
		&error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
}

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add("/staging/creates-nothing", Fixture, NULL, fixture_set_up,
	           test_staging_creates_nothing, fixture_tear_down);
	g_test_add("/staging/updates-nothing", Fixture, NULL, fixture_set_up,
	           test_staging_updates_nothing, fixture_tear_down);
	g_test_add("/staging/deletes-nothing", Fixture, NULL, fixture_set_up,
	           test_staging_deletes_nothing, fixture_tear_down);
	g_test_add("/staging/approve-applies-the-create", Fixture, NULL,
	           fixture_set_up, test_staging_approve_applies_the_create,
	           fixture_tear_down);
	g_test_add("/staging/approve-applies-the-staged-object", Fixture, NULL,
	           fixture_set_up, test_staging_approve_applies_the_staged_object,
	           fixture_tear_down);
	g_test_add("/staging/approve-applies-the-delete", Fixture, NULL,
	           fixture_set_up, test_staging_approve_applies_the_delete,
	           fixture_tear_down);
	g_test_add("/staging/stale-approval-is-a-conflict", Fixture, NULL,
	           fixture_set_up, test_staging_stale_approval_is_a_conflict,
	           fixture_tear_down);
	g_test_add("/staging/reject-discards-and-records", Fixture, NULL,
	           fixture_set_up, test_staging_reject_discards_and_records,
	           fixture_tear_down);
	g_test_add("/staging/audit-distinguishes-staged-from-direct", Fixture,
	           NULL, fixture_set_up,
	           test_staging_audit_distinguishes_staged_from_direct,
	           fixture_tear_down);
	g_test_add("/staging/audit-distinguishes-the-assistant", Fixture, NULL,
	           fixture_set_up, test_staging_audit_distinguishes_the_assistant,
	           fixture_tear_down);
	g_test_add("/staging/card-stands-on-its-own", Fixture, NULL,
	           fixture_set_up, test_staging_card_stands_on_its_own,
	           fixture_tear_down);
	g_test_add("/staging/card-withholds-the-prompt", Fixture, NULL,
	           fixture_set_up, test_staging_card_withholds_the_prompt,
	           fixture_tear_down);
	g_test_add("/staging/expires-and-is-dropped", Fixture, NULL,
	           fixture_set_up, test_staging_expires_and_is_dropped,
	           fixture_tear_down);
	g_test_add("/staging/refuses-past-the-limit", Fixture, NULL,
	           fixture_set_up, test_staging_refuses_past_the_limit,
	           fixture_tear_down);
	g_test_add("/staging/restaging-keeps-one-card", Fixture, NULL,
	           fixture_set_up, test_staging_restaging_keeps_one_card,
	           fixture_tear_down);
	g_test_add("/staging/refuses-an-empty-change", Fixture, NULL,
	           fixture_set_up, test_staging_refuses_an_empty_change,
	           fixture_tear_down);

	g_test_add_func("/staging/flag-refuses-what-it-does-not-know",
	                test_staging_flag_refuses_what_it_does_not_know);

	return g_test_run();
}
