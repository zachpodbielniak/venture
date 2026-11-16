/*
 * test-ticket-external-ref.c - A ticket's id in another system
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A system that mirrors its own work into tickets -- an agent fleet's
 * tasks -- stamps each ticket and comment it makes with its own id for it
 * (`external_ref`). Two things depend on the value being unique within an
 * organization: a create retried after a timeout must find the first
 * ticket rather than make a second, and a mirror deciding whether a
 * comment is its own echo must find exactly one. Neither may stop two
 * organizations using the same reference, nor every ordinary ticket --
 * which has none -- from existing side by side.
 */

#include <venture.h>

#include <glib.h>

typedef struct
{
	VentureConfig	*config;
	VentureDatabase	*database;
	VentureContext	*context;
	gint64		 org_a;
	gint64		 org_b;
} Fixture;

static gint64
make_organization(
	Fixture		*fixture,
	const gchar	*name
){
	g_autoptr(VentureEntity) org = g_object_new(VENTURE_TYPE_ORGANIZATION,
		"name", name, NULL);

	g_assert_true(venture_database_save(fixture->database, org, NULL, NULL));
	return venture_entity_get_id(org);
}

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;

	(void)user_data;

	fixture->config = venture_config_new();
	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->context = venture_context_new(fixture->config, fixture->database);
	fixture->org_a = make_organization(fixture, "Org A");
	fixture->org_b = make_organization(fixture, "Org B");
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

/* Saves a ticket in @org with @ref; returns whether the save took. */
static gboolean
save_ticket(
	Fixture		 *fixture,
	gint64		  org,
	const gchar	 *ref,
	gint64		 *out_id,
	GError		**error
){
	g_autoptr(VentureTicket) ticket = venture_ticket_new();

	g_object_set(ticket, "title", "Mirrored work", "organization-id", org,
	             "external-ref", ref, NULL);

	if (!venture_database_save(fixture->database, VENTURE_ENTITY(ticket), NULL,
	                           error))
		return FALSE;

	if (NULL != out_id)
		*out_id = venture_entity_get_id(VENTURE_ENTITY(ticket));
	return TRUE;
}

/*
 * One reference, one ticket, per organization -- deleted ones included.
 *
 * What breaks if this regresses: a retried create makes a duplicate, and
 * the mirror then tracks one ticket while people work the other.
 */
static void
test_unique_within_organization(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	gint64 first = 0;

	(void)user_data;

	g_assert_true(save_ticket(fixture, fixture->org_a, "clawtilla:x:task-1",
	                          &first, &error));
	g_assert_no_error(error);

	g_assert_false(save_ticket(fixture, fixture->org_a, "clawtilla:x:task-1",
	                           NULL, &error));
	/* "that already exists", which a client reads as 409, not 500 */
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
	g_clear_error(&error);

	/* another organization may use the same reference */
	g_assert_true(save_ticket(fixture, fixture->org_b, "clawtilla:x:task-1",
	                          NULL, &error));
	g_assert_no_error(error);

	/* a deleted ticket still holds its reference */
	{
		g_autoptr(VentureEntity) loaded = venture_database_get(
			fixture->database, VENTURE_TYPE_TICKET, first, NULL);

		g_assert_true(venture_database_delete(fixture->database, loaded, NULL,
		                                      NULL));
	}
	g_assert_false(save_ticket(fixture, fixture->org_a, "clawtilla:x:task-1",
	                           NULL, &error));
	g_clear_error(&error);
}

/*
 * The ordinary ticket has no reference, and any number of them coexist.
 *
 * What breaks if this regresses: the second ticket anybody ever writes by
 * hand is refused as a duplicate of the first.
 */
static void
test_empty_is_not_a_reference(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;

	(void)user_data;

	g_assert_true(save_ticket(fixture, fixture->org_a, NULL, NULL, &error));
	g_assert_true(save_ticket(fixture, fixture->org_a, NULL, NULL, &error));
	g_assert_true(save_ticket(fixture, fixture->org_a, "", NULL, &error));
	g_assert_true(save_ticket(fixture, fixture->org_a, "", NULL, &error));
	g_assert_no_error(error);
}

/*
 * The lookup a mirror makes: a filter on the reference finds the one.
 */
static void
test_filter_finds_it(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_TICKET);
	g_autoptr(GPtrArray) found = NULL;
	gint64 id = 0;

	(void)user_data;

	g_assert_true(save_ticket(fixture, fixture->org_a, "clawtilla:x:task-9",
	                          &id, &error));
	g_assert_true(save_ticket(fixture, fixture->org_a, "clawtilla:x:task-8",
	                          NULL, &error));
	g_assert_true(venture_query_add_filter_string(query, "external-ref",
		VENTURE_FILTER_OP_EQ, "clawtilla:x:task-9", &error));
	found = venture_database_find(fixture->database, query, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(found->len, ==, 1);
	g_assert_cmpint(venture_entity_get_id(g_ptr_array_index(found, 0)), ==, id);
}

/* A comment's reference is held to the same rule. */
static void
test_comment_reference(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	gint64 ticket = 0;
	guint i;

	(void)user_data;

	g_assert_true(save_ticket(fixture, fixture->org_a, NULL, &ticket, &error));

	for (i = 0; i < 2; i++)
	{
		g_autoptr(VentureTicketComment) comment = venture_ticket_comment_new();
		gboolean saved;

		g_object_set(comment, "ticket-id", ticket, "body", "done",
		             "organization-id", fixture->org_a,
		             "external-ref", "clawtilla:x:note-1", NULL);
		saved = venture_database_save(fixture->database,
		                              VENTURE_ENTITY(comment), NULL, &error);
		g_assert_cmpint(saved, ==, i == 0);
		g_clear_error(&error);
	}
}

int
main(
	int	 argc,
	char	*argv[]
){
	g_test_init(&argc, &argv, NULL);

	g_test_add("/ticket-external-ref/unique-within-organization", Fixture,
	           NULL, fixture_set_up, test_unique_within_organization,
	           fixture_tear_down);
	g_test_add("/ticket-external-ref/empty-is-not-a-reference", Fixture, NULL,
	           fixture_set_up, test_empty_is_not_a_reference,
	           fixture_tear_down);
	g_test_add("/ticket-external-ref/filter-finds-it", Fixture, NULL,
	           fixture_set_up, test_filter_finds_it, fixture_tear_down);
	g_test_add("/ticket-external-ref/comment-reference", Fixture, NULL,
	           fixture_set_up, test_comment_reference, fixture_tear_down);

	return g_test_run();
}
