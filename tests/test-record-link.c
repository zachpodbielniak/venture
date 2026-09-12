/*
 * test-record-link.c - Linking any record to any other
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A link names both of its ends by type and id, so there is no database
 * constraint behind it. Everything here is about the checks that stand in
 * for one, and about a link being one row read from either end.
 */

#include <venture.h>

#include <glib.h>
#include <string.h>

typedef struct
{
	VentureConfig	*config;
	VentureDatabase	*database;
	VentureContext	*context;
	gint64		 ticket_id;
	gint64		 invoice_id;
	gint64		 expense_id;
} Fixture;

static void
fixture_set_up(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureTicket) ticket = NULL;
	g_autoptr(VentureInvoice) invoice = NULL;
	g_autoptr(VentureExpense) expense = NULL;

	(void)user_data;

	fixture->config = venture_config_new();
	fixture->database = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);

	/* The context installs the save validator; without it a link is
	 * just a row. */
	fixture->context = venture_context_new(fixture->config, fixture->database);

	g_assert_true(venture_database_migrate(fixture->database,
		venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);

	ticket = venture_ticket_new();
	g_object_set(ticket, "title", "Ship the release", NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(ticket), NULL, NULL));
	fixture->ticket_id = venture_entity_get_id(VENTURE_ENTITY(ticket));

	invoice = venture_invoice_new();
	g_object_set(invoice, "number", "INV-42", NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(invoice), NULL, NULL));
	fixture->invoice_id = venture_entity_get_id(VENTURE_ENTITY(invoice));

	expense = venture_expense_new();
	g_object_set(expense, "description", "Cover art", NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(expense), NULL, NULL));
	fixture->expense_id = venture_entity_get_id(VENTURE_ENTITY(expense));
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
 * The inverse table is total and involutive: every kind has an inverse,
 * and the inverse of the inverse is the kind.
 */
static void
test_link_kind_inverse_is_an_involution(void)
{
	GEnumClass *enum_class;
	guint i;

	enum_class = g_type_class_ref(VENTURE_TYPE_LINK_KIND);

	for (i = 0; i < enum_class->n_values; i++)
	{
		VentureLinkKind kind;

		kind = (VentureLinkKind)enum_class->values[i].value;

		g_assert_cmpint(venture_link_kind_inverse(
			venture_link_kind_inverse(kind)), ==, kind);
		g_assert_nonnull(venture_link_kind_to_label(kind));
	}

	g_type_class_unref(enum_class);

	g_assert_cmpint(venture_link_kind_inverse(VENTURE_LINK_KIND_BLOCKS), ==,
	                VENTURE_LINK_KIND_BLOCKED_BY);
	g_assert_true(venture_link_kind_is_symmetric(VENTURE_LINK_KIND_RELATED));
	g_assert_false(venture_link_kind_is_symmetric(VENTURE_LINK_KIND_PRODUCES));
}

/*
 * Any two records of any types can be linked, and the link reads correctly
 * from either end: the kind from the source, the inverse from the target.
 *
 * What breaks if this regresses: the whole point. One row has to serve
 * both pages, or every link is two rows that drift.
 */
static void
test_link_joins_any_two_records_and_reads_from_both_ends(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureRecordLink) link = NULL;
	g_autoptr(GPtrArray) from_ticket = NULL;
	g_autoptr(GPtrArray) from_invoice = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *other_type = NULL;
	g_autofree gchar *other_label = NULL;
	g_autofree gchar *display = NULL;
	VentureLinkKind kind;
	gint64 other_id = 0;

	(void)user_data;

	link = venture_record_link_create(fixture->database, "ticket",
	                                  fixture->ticket_id,
	                                  VENTURE_LINK_KIND_BLOCKS, "invoice",
	                                  fixture->invoice_id, "cannot bill yet",
	                                  &error);
	g_assert_no_error(error);
	g_assert_nonnull(link);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(link), NULL, &error));
	g_assert_no_error(error);

	/* Labels captured now, and the display name reads as a sentence.
	 * An invoice's display name is whatever the type says it is --
	 * "invoice #1" today -- so the expectation is built, not spelled. */
	{
		g_autoptr(VentureEntity) invoice = NULL;
		g_autofree gchar *invoice_label = NULL;
		g_autofree gchar *expected = NULL;

		invoice = venture_database_get(fixture->database, VENTURE_TYPE_INVOICE,
		                               fixture->invoice_id, NULL);
		invoice_label = venture_entity_get_display_name(invoice);
		expected = g_strdup_printf("Ship the release blocks %s", invoice_label);
		display = venture_entity_get_display_name(VENTURE_ENTITY(link));
		g_assert_cmpstr(display, ==, expected);
	}

	/* From the ticket: it blocks the invoice. */
	from_ticket = venture_record_link_find_for(fixture->database, "ticket",
	                                           fixture->ticket_id, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(from_ticket->len, ==, 1);
	g_assert_true(venture_record_link_other_end(
		g_ptr_array_index(from_ticket, 0), "ticket", fixture->ticket_id,
		&other_type, &other_id, &other_label, &kind));
	g_assert_cmpstr(other_type, ==, "invoice");
	g_assert_cmpint(other_id, ==, fixture->invoice_id);
	g_assert_false(venture_string_is_empty(other_label));
	g_assert_cmpint(kind, ==, VENTURE_LINK_KIND_BLOCKS);

	g_clear_pointer(&other_type, g_free);
	g_clear_pointer(&other_label, g_free);

	/* From the invoice -- by its plural, since a caller may hold a REST
	 * path segment -- it is blocked by the ticket. */
	from_invoice = venture_record_link_find_for(fixture->database, "invoices",
	                                            fixture->invoice_id, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(from_invoice->len, ==, 1);
	g_assert_true(venture_record_link_other_end(
		g_ptr_array_index(from_invoice, 0), "invoices", fixture->invoice_id,
		&other_type, &other_id, &other_label, &kind));
	g_assert_cmpstr(other_type, ==, "ticket");
	g_assert_cmpint(other_id, ==, fixture->ticket_id);
	g_assert_cmpint(kind, ==, VENTURE_LINK_KIND_BLOCKED_BY);

	/* A record it does not touch says so. */
	g_assert_false(venture_record_link_other_end(
		g_ptr_array_index(from_invoice, 0), "expense", fixture->expense_id,
		NULL, NULL, NULL, NULL));
}

/*
 * The checks: unknown type, missing record, self-link, duplicate in either
 * direction, and the audit log.
 *
 * What breaks if this regresses: a row naming a type nobody registered or
 * an id nobody has is a link to nothing, written without complaint.
 */
static void
test_link_refuses_what_cannot_mean_anything(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureRecordLink) link = NULL;
	g_autoptr(VentureRecordLink) reverse = NULL;
	g_autoptr(GError) error = NULL;

	(void)user_data;

	/* Unknown type, with the alternatives listed. */
	link = venture_record_link_create(fixture->database, "ticket",
	                                  fixture->ticket_id,
	                                  VENTURE_LINK_KIND_RELATED, "invoic", 1,
	                                  NULL, &error);
	g_assert_null(link);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_assert_nonnull(strstr(error->message, "Known types"));
	g_clear_error(&error);

	/* Missing record. */
	link = venture_record_link_create(fixture->database, "ticket",
	                                  fixture->ticket_id,
	                                  VENTURE_LINK_KIND_RELATED, "invoice",
	                                  9999, NULL, &error);
	g_assert_null(link);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);

	/* Self, even spelled differently. */
	link = venture_record_link_create(fixture->database, "ticket",
	                                  fixture->ticket_id,
	                                  VENTURE_LINK_KIND_RELATED, "tickets",
	                                  fixture->ticket_id, NULL, &error);
	g_assert_null(link);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	/* The audit log. */
	link = venture_record_link_create(fixture->database, "ticket",
	                                  fixture->ticket_id,
	                                  VENTURE_LINK_KIND_RELATED, "audit_entry",
	                                  1, NULL, &error);
	g_assert_null(link);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
	g_clear_error(&error);

	/* A good one, saved... */
	link = venture_record_link_create(fixture->database, "ticket",
	                                  fixture->ticket_id,
	                                  VENTURE_LINK_KIND_PRODUCES, "invoice",
	                                  fixture->invoice_id, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(link), NULL, &error));

	/* ...cannot be added again, forwards or backwards. */
	reverse = venture_record_link_create(fixture->database, "invoice",
	                                     fixture->invoice_id,
	                                     VENTURE_LINK_KIND_RELATED, "ticket",
	                                     fixture->ticket_id, NULL, &error);
	g_assert_null(reverse);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
	g_clear_error(&error);

	reverse = venture_record_link_create(fixture->database, "ticket",
	                                     fixture->ticket_id,
	                                     VENTURE_LINK_KIND_BLOCKS, "invoice",
	                                     fixture->invoice_id, NULL, &error);
	g_assert_null(reverse);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
}

/*
 * The checks run at the save, not only in the constructor, so a link built
 * any other way -- a generic POST /api/v1/record_link, an approved staged
 * change -- is held to them too, and gets its labels filled in.
 *
 * What breaks if this regresses: the constructor is one door; the save is
 * the only one every writer goes through.
 */
static void
test_link_save_validator_catches_every_writer(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureRecordLink) raw = NULL;
	g_autoptr(VentureRecordLink) loaded = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *source_label = NULL;
	g_autofree gchar *target_label = NULL;

	(void)user_data;

	/* Built by hand, as the generic API would, with a bad target. */
	raw = venture_record_link_new();
	g_object_set(raw,
	             "source-type", "ticket", "source-id", fixture->ticket_id,
	             "kind", VENTURE_LINK_KIND_RELATED,
	             "target-type", "expense", "target-id", (gint64)424242,
	             NULL);

	g_assert_false(venture_database_save(fixture->database,
	                                     VENTURE_ENTITY(raw), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);

	/* Fixed, it saves, and the labels it never set are filled from the
	 * records. */
	g_object_set(raw, "target-id", fixture->expense_id, NULL);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(raw), NULL, &error));
	g_assert_no_error(error);

	loaded = VENTURE_RECORD_LINK(venture_database_get(
		fixture->database, VENTURE_TYPE_RECORD_LINK,
		venture_entity_get_id(VENTURE_ENTITY(raw)), &error));
	g_assert_no_error(error);
	g_object_get(loaded, "source-label", &source_label,
	             "target-label", &target_label, NULL);
	g_assert_cmpstr(source_label, ==, "Ship the release");
	g_assert_cmpstr(target_label, ==, "Cover art");

	/* Moving an end onto itself is caught on the update too. */
	g_object_set(loaded, "target-type", "ticket",
	             "target-id", fixture->ticket_id, NULL);
	g_assert_false(venture_database_save(fixture->database,
	                                     VENTURE_ENTITY(loaded), NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_INVALID_ARGUMENT);
}

/*
 * A deleted end keeps its label and loses its link; a disabled module does
 * the same. Neither is an error.
 */
static void
test_link_survives_a_missing_end(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureRecordLink) link = NULL;
	g_autoptr(VentureEntity) invoice = NULL;
	g_autoptr(VentureEntity) resolved = NULL;
	g_autoptr(JsonNode) described = NULL;
	g_autoptr(GError) error = NULL;
	JsonObject *entry;

	(void)user_data;

	link = venture_record_link_create(fixture->database, "ticket",
	                                  fixture->ticket_id,
	                                  VENTURE_LINK_KIND_REFERENCES, "invoice",
	                                  fixture->invoice_id, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(link), NULL, &error));

	invoice = venture_database_get(fixture->database, VENTURE_TYPE_INVOICE,
	                               fixture->invoice_id, &error);
	g_assert_true(venture_database_delete(fixture->database, invoice, NULL,
	                                      &error));
	g_assert_no_error(error);

	described = venture_record_link_describe_for(fixture->database, "ticket",
	                                             fixture->ticket_id, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(described)),
	                 ==, 1);

	entry = json_array_get_object_element(json_node_get_array(described), 0);
	g_assert_cmpstr(json_object_get_string_member(entry, "kind"), ==,
	                "references");
	g_assert_cmpstr(json_object_get_string_member(entry, "kind_label"), ==,
	                "references");
	g_assert_cmpstr(json_object_get_string_member(entry, "other_type"), ==,
	                "invoice");
	g_assert_false(venture_string_is_empty(
		json_object_get_string_member(entry, "other_label")));
	g_assert_false(json_object_get_boolean_member(entry, "other_exists"));

	/* And the link itself is still there to be removed. */
	resolved = venture_record_link_resolve(fixture->database, "invoice",
	                                       fixture->invoice_id, &error);
	g_assert_no_error(error);
	g_assert_nonnull(resolved);
	g_assert_true(venture_entity_is_deleted(resolved));

	/* A type that is off resolves to nothing, quietly. */
	venture_entity_registry_set_type_module(
		venture_entity_registry_get_default(), "invoice", "invoicing", FALSE);
	g_clear_object(&resolved);
	resolved = venture_record_link_resolve(fixture->database, "invoice",
	                                       fixture->invoice_id, &error);
	g_assert_no_error(error);
	g_assert_null(resolved);
	venture_entity_registry_set_type_module(
		venture_entity_registry_get_default(), "invoice", "invoicing", TRUE);
}

/*
 * Links are records: soft-deleted, restorable, audited, and serialised
 * through the same machinery as everything else -- which is what makes
 * them reachable from the generic API, the CLI and a staged approval.
 */
static void
test_link_is_an_ordinary_record(
	Fixture		*fixture,
	gconstpointer	 user_data
){
	g_autoptr(VentureRecordLink) link = NULL;
	g_autoptr(VentureEntity) loaded = NULL;
	g_autoptr(JsonNode) json = NULL;
	g_autoptr(GPtrArray) links = NULL;
	g_autoptr(GError) error = NULL;
	JsonObject *object;

	(void)user_data;

	g_assert_cmpuint(venture_entity_registry_lookup(
		venture_entity_registry_get_default(), "record_link"), ==,
		VENTURE_TYPE_RECORD_LINK);
	g_assert_cmpstr(venture_entity_registry_get_type_module(
		venture_entity_registry_get_default(), "record_link"), ==, "core");

	link = venture_record_link_create(fixture->database, "expense",
	                                  fixture->expense_id,
	                                  VENTURE_LINK_KIND_CAUSED_BY, "ticket",
	                                  fixture->ticket_id, "the art brief",
	                                  &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_save(fixture->database,
	                                    VENTURE_ENTITY(link), NULL, &error));

	json = venture_serializable_to_json(VENTURE_SERIALIZABLE(link), FALSE);
	object = json_node_get_object(json);
	g_assert_cmpstr(json_object_get_string_member(object, "kind"), ==,
	                "caused_by");
	g_assert_cmpstr(json_object_get_string_member(object, "source_type"), ==,
	                "expense");

	loaded = venture_database_get(fixture->database, VENTURE_TYPE_RECORD_LINK,
	                              venture_entity_get_id(VENTURE_ENTITY(link)),
	                              &error);
	g_assert_true(venture_database_delete(fixture->database, loaded, NULL,
	                                      &error));
	g_assert_no_error(error);

	/* Gone from both ends. */
	links = venture_record_link_find_for(fixture->database, "ticket",
	                                     fixture->ticket_id, &error);
	g_assert_cmpuint(links->len, ==, 0);
	g_clear_pointer(&links, g_ptr_array_unref);
	links = venture_record_link_find_for(fixture->database, "expense",
	                                     fixture->expense_id, &error);
	g_assert_cmpuint(links->len, ==, 0);

	/* Restored, it is back -- and the duplicate check sees it again. */
	g_assert_true(venture_database_restore(fixture->database, loaded, NULL,
	                                       &error));
	g_assert_no_error(error);
	g_clear_object(&link);
	link = venture_record_link_create(fixture->database, "ticket",
	                                  fixture->ticket_id,
	                                  VENTURE_LINK_KIND_CAUSES, "expense",
	                                  fixture->expense_id, NULL, &error);
	g_assert_null(link);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_ALREADY_EXISTS);
}

int
main(
	int	  argc,
	char	**argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/record-link/kind-inverse-is-an-involution",
	                test_link_kind_inverse_is_an_involution);

#define ADD(path, func) \
	g_test_add(path, Fixture, NULL, fixture_set_up, func, fixture_tear_down)

	ADD("/record-link/joins-any-two-records-and-reads-from-both-ends",
	    test_link_joins_any_two_records_and_reads_from_both_ends);
	ADD("/record-link/refuses-what-cannot-mean-anything",
	    test_link_refuses_what_cannot_mean_anything);
	ADD("/record-link/save-validator-catches-every-writer",
	    test_link_save_validator_catches_every_writer);
	ADD("/record-link/survives-a-missing-end",
	    test_link_survives_a_missing_end);
	ADD("/record-link/is-an-ordinary-record",
	    test_link_is_an_ordinary_record);

	return g_test_run();
}
