/*
 * test-ticket-relation.c - Pointing a ticket at anything
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A relation names its subject by type and id rather than by a foreign key,
 * so there is no database constraint behind it. Nothing stops a row naming
 * a type that was never registered or an id that was never there, and such
 * a row is not a broken link so much as a link to nothing: it cannot be
 * rendered, followed or explained. Everything here is about the check that
 * happens instead.
 */

#include <venture.h>

#include <glib.h>

typedef struct
{
	VentureConfig	*config;
	VentureDatabase	*database;
	VentureContext	*context;
	gint64		 ticket_id;
	gint64		 invoice_id;
} Fixture;

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureTicket) ticket = NULL;
	g_autoptr(VentureInvoice) invoice = NULL;

	(void)user_data;

	fixture->config = venture_config_new();
	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	fixture->context = venture_context_new(fixture->config, fixture->database);

	ticket = venture_ticket_new();
	g_object_set(ticket, "title", "March invoice looks wrong", NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(ticket), NULL, NULL));
	fixture->ticket_id = venture_entity_get_id(VENTURE_ENTITY(ticket));

	invoice = venture_invoice_new();
	g_object_set(invoice, "number", "INV-42", NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(invoice), NULL, NULL));
	fixture->invoice_id = venture_entity_get_id(VENTURE_ENTITY(invoice));
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

/*
 * A ticket can point at a record of any registered type.
 *
 * What breaks if this regresses: the whole point. Tickets are about invoices
 * whose totals look wrong and expenses missing a receipt at least as often
 * as they are about code, and adding a foreign key per type would grow the
 * ticket table a column for every record type that ever exists.
 */
static void
test_relation_links_to_any_type(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureTicketRelation) relation = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *label = NULL;
	g_autofree gchar *subject_type = NULL;

	(void)user_data;

	relation = venture_ticket_relation_create(fixture->database,
		fixture->ticket_id, "invoice", fixture->invoice_id,
		"the total in question", &error);
	g_assert_no_error(error);
	g_assert_nonnull(relation);

	g_object_get(relation, "subject-type", &subject_type,
	             "subject-label", &label, NULL);
	g_assert_cmpstr(subject_type, ==, "invoice");

	/* The label is captured now, not looked up later. */
	g_assert_false(venture_string_is_empty(label));
}

/*
 * A type nobody registered is refused, and the message says what is known.
 *
 * What breaks if this regresses: a row naming "invoic" is a link to nothing,
 * and it is written without complaint. The name is usually a near miss --
 * a singular for a plural, or a plugin type that is not loaded -- so a bare
 * "unknown type" leaves the caller guessing which.
 */
static void
test_relation_refuses_an_unknown_type(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureTicketRelation) relation = NULL;
	g_autoptr(GError) error = NULL;

	(void)user_data;

	relation = venture_ticket_relation_create(fixture->database,
		fixture->ticket_id, "invoic", 1, NULL, &error);

	g_assert_null(relation);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);

	/* The alternatives, so a near miss is fixable from the message. */
	g_assert_nonnull(strstr(error->message, "invoice"));
}

/*
 * A record that is not there is refused.
 *
 * What breaks if this regresses: the relation renders as a subject that
 * cannot be opened, and there is no way to tell it from one whose subject
 * was deleted afterwards -- which is a different thing and legitimately
 * happens.
 */
static void
test_relation_refuses_a_missing_record(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureTicketRelation) relation = NULL;
	g_autoptr(GError) error = NULL;

	(void)user_data;

	relation = venture_ticket_relation_create(fixture->database,
		fixture->ticket_id, "invoice", 9999, NULL, &error);

	g_assert_null(relation);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
}

/*
 * The same pair twice is refused, and a ticket cannot be related to itself.
 *
 * What breaks if this regresses: two identical rows cannot be told apart
 * afterwards, so neither can be the one to remove. And a self-relation
 * renders as a link back to the page it is on -- "part of" already exists
 * for the real ticket-to-ticket relationship.
 */
static void
test_relation_refuses_duplicates_and_self(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureTicketRelation) first = NULL;
	g_autoptr(VentureTicketRelation) again = NULL;
	g_autoptr(VentureTicketRelation) itself = NULL;
	g_autoptr(GError) error = NULL;

	(void)user_data;

	first = venture_ticket_relation_create(fixture->database,
		fixture->ticket_id, "invoice", fixture->invoice_id, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(first), NULL, &error));
	g_assert_no_error(error);

	again = venture_ticket_relation_create(fixture->database,
		fixture->ticket_id, "invoice", fixture->invoice_id, NULL, &error);
	g_assert_null(again);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
	g_clear_error(&error);

	itself = venture_ticket_relation_create(fixture->database,
		fixture->ticket_id, "ticket", fixture->ticket_id, NULL, &error);
	g_assert_null(itself);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/*
 * The reverse direction finds it, which is the useful direction.
 *
 * What breaks if this regresses: venture_web_append_related() finds records
 * pointing at this one by walking declared references, and a polymorphic
 * pair is invisible to it. Without this lookup the relation is only visible
 * from the ticket -- half a link, and the wrong half. Standing on an invoice
 * and asking what is outstanding about it is the usual question.
 */
static void
test_relation_is_found_from_the_subject(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureTicketRelation) relation = NULL;
	g_autoptr(GPtrArray) found = NULL;
	g_autoptr(GError) error = NULL;
	gint64 ticket_id = 0;

	(void)user_data;

	relation = venture_ticket_relation_create(fixture->database,
		fixture->ticket_id, "invoice", fixture->invoice_id, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(relation), NULL, NULL));

	found = venture_ticket_relation_find_for_subject(fixture->database,
		"invoice", fixture->invoice_id, &error);
	g_assert_no_error(error);
	g_assert_nonnull(found);
	g_assert_cmpuint(found->len, ==, 1);

	g_object_get(g_ptr_array_index(found, 0), "ticket-id", &ticket_id, NULL);
	g_assert_cmpint(ticket_id, ==, fixture->ticket_id);

	/* And nothing is found for a record nobody linked. */
	{
		g_autoptr(GPtrArray) none = NULL;

		none = venture_ticket_relation_find_for_subject(fixture->database,
			"invoice", 9999, &error);
		g_assert_no_error(error);
		g_assert_cmpuint(none->len, ==, 0);
	}
}

/*
 * A deleted subject leaves the relation readable.
 *
 * What breaks if this regresses: the label is stored rather than looked up
 * for exactly this case. Deleting the thing a ticket was about is precisely
 * when somebody needs to know what it was, and a relation that renders as
 * "invoice #7" once the invoice is gone has thrown that away.
 */
static void
test_relation_survives_a_deleted_subject(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureTicketRelation) relation = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) resolved = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *label = NULL;

	(void)user_data;

	relation = venture_ticket_relation_create(fixture->database,
		fixture->ticket_id, "invoice", fixture->invoice_id, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(relation), NULL, NULL));

	invoice = venture_database_get(fixture->database, VENTURE_TYPE_INVOICE,
	                               fixture->invoice_id, &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_delete(fixture->database, invoice, NULL,
	                                      &error));
	g_assert_no_error(error);

	/* The label is still there... */
	g_object_get(relation, "subject-label", &label, NULL);
	g_assert_false(venture_string_is_empty(label));

	/*
	 * ...and the subject still resolves, because a soft delete stamps
	 * rather than removes. The caller decides what that means -- the web
	 * panel checks the stamp and drops the link, which is why it must be
	 * reachable rather than filtered out here.
	 */
	resolved = venture_ticket_relation_resolve(fixture->database, relation,
	                                           &error);
	g_assert_no_error(error);
	g_assert_nonnull(resolved);
	g_assert_true(venture_entity_is_deleted(resolved));
}

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	ADD("/ticket-relation/links-to-any-type",
	    test_relation_links_to_any_type);
	ADD("/ticket-relation/refuses-an-unknown-type",
	    test_relation_refuses_an_unknown_type);
	ADD("/ticket-relation/refuses-a-missing-record",
	    test_relation_refuses_a_missing_record);
	ADD("/ticket-relation/refuses-duplicates-and-self",
	    test_relation_refuses_duplicates_and_self);
	ADD("/ticket-relation/is-found-from-the-subject",
	    test_relation_is_found_from_the_subject);
	ADD("/ticket-relation/survives-a-deleted-subject",
	    test_relation_survives_a_deleted_subject);

#undef ADD

	return g_test_run();
}
