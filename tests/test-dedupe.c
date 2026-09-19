/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * Duplicate companies and contacts that already exist: the scan proposes,
 * a person merges, and a merge is one transaction that re-points every
 * reference the field tables declare. Each test names the rule that breaks
 * if it regresses.
 */
#include <venture.h>
#include <string.h>
#include "venture-test-util.h"

typedef struct {
	VentureDatabase *db;
	VentureContext *context;
	VentureConfig *config;
	gint64 org;
} Fixture;

static VentureEntity *record(Fixture *f, const gchar *name)
{
	GType type = venture_entity_registry_lookup(venture_entity_registry_get_default(), name);
	g_assert_cmpuint(type, !=, G_TYPE_INVALID);
	return g_object_new(type, "organization-id", f->org, NULL);
}
static void save(Fixture *f, VentureEntity *row)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_save(f->db, row, NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}
static void field(VentureEntity *row, const gchar *name, const gchar *value)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_entity_set_field_from_string(row, name, value, &error));
	g_assert_no_error(error);
}
static GPtrArray *rows(Fixture *f, const gchar *name)
{
	g_autoptr(VentureQuery) query = venture_query_new(venture_entity_registry_lookup(venture_entity_registry_get_default(), name));
	venture_query_set_organization(query, f->org);
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	return venture_database_find(f->db, query, NULL);
}
static GPtrArray *all_rows(Fixture *f, const gchar *name)
{
	g_autoptr(VentureQuery) query = venture_query_new(venture_entity_registry_lookup(venture_entity_registry_get_default(), name));
	venture_query_set_organization(query, f->org);
	venture_query_set_include_deleted(query, TRUE);
	venture_query_set_limit(query, 0);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	return venture_database_find(f->db, query, NULL);
}
static gint64 company(Fixture *f, const gchar *name, const gchar *email, const gchar *phone, const gchar *website)
{
	g_autoptr(VentureEntity) row = record(f, "company");
	g_object_set(row, "name", name, "email", email, "phone", phone, "website", website, NULL);
	save(f, row);
	return venture_entity_get_id(row);
}
static gint64 contact(Fixture *f, const gchar *name, const gchar *email, const gchar *phone, gint64 company_id)
{
	g_autoptr(VentureEntity) row = record(f, "contact");
	g_object_set(row, "name", name, "email", email, "phone", phone, "company-id", company_id, NULL);
	save(f, row);
	return venture_entity_get_id(row);
}
static gint64 ref_id(Fixture *f, const gchar *type, gint64 id, const gchar *property)
{
	g_autoptr(VentureEntity) row = venture_database_get(f->db,
		venture_entity_registry_lookup(venture_entity_registry_get_default(), type), id, NULL);
	gint64 value = -1;
	g_assert_nonnull(row);
	g_object_get(row, property, &value, NULL);
	return value;
}
static gchar *text_of(Fixture *f, const gchar *type, gint64 id, const gchar *property)
{
	g_autoptr(VentureEntity) row = venture_database_get(f->db,
		venture_entity_registry_lookup(venture_entity_registry_get_default(), type), id, NULL);
	gchar *value = NULL;
	g_assert_nonnull(row);
	g_object_get(row, property, &value, NULL);
	return value;
}
static gint scan(Fixture *f, const gchar *kind)
{
	g_autoptr(GError) error = NULL;
	gint n = venture_dedupe_service_scan(venture_dedupe_service_get(f->db), f->org, kind, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(n, >=, 0);
	return n;
}
static VentureEntity *candidate_for(Fixture *f, gint64 a, gint64 b)
{
	g_autoptr(GPtrArray) candidates = rows(f, "duplicate_candidate");
	guint i;
	for (i = 0; i < candidates->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(candidates, i);
		gint64 ra = 0, rb = 0;
		g_object_get(row, "record-a", &ra, "record-b", &rb, NULL);
		if ((ra == MIN(a, b)) && (rb == MAX(a, b)))
			return g_object_ref(row);
	}
	return NULL;
}
static void setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	(void)unused;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
}
static void teardown(Fixture *f, gconstpointer unused)
{
	(void)unused;
	g_clear_object(&f->context); g_clear_object(&f->db); g_clear_object(&f->config);
}

/* The documented name similarity: case, punctuation and whitespace are
 * ignored, legal suffixes are dropped, and the score is the share of the
 * longer token list that the shorter one covers. */
static void test_name_similarity(Fixture *f, gconstpointer unused)
{
	(void)f; (void)unused;
	g_assert_cmpint(venture_dedupe_name_similarity("Acme, Inc.", "ACME Inc"), ==, 100);
	g_assert_cmpint(venture_dedupe_name_similarity("Acme Widgets", "Acme  gadgets"), ==, 50);
	g_assert_cmpint(venture_dedupe_name_similarity("Initech LLC", "The Initech Corporation"), ==, 50);
	g_assert_cmpint(venture_dedupe_name_similarity("Foo", "Bar"), ==, 0);
	g_assert_cmpint(venture_dedupe_name_similarity(NULL, "Bar"), ==, 0);
	g_assert_cmpint(venture_dedupe_name_similarity("Inc", "Ltd"), ==, 0);
}

/* Exact normalised identifiers, a shared domain with a similar name and a
 * similar name alone each propose a pair; unrelated rows do not. */
static void test_scan_companies(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) ab = NULL, ac = NULL, de = NULL, fg = NULL, ah = NULL;
	g_autoptr(GPtrArray) before = NULL, after = NULL;
	g_autofree gchar *reasons = NULL;
	gint64 a, b, c, d, e, g, h, i;
	gint64 score = 0;
	(void)unused;
	a = company(f, "Acme Inc", "Sales@Acme.com", "", "");
	b = company(f, "ACME, Inc.", "sales+web@acme.com", "", "");
	c = company(f, "Acme Widgets", "bob@acme.com", "", "");
	d = company(f, "Globex", "", "+1 (555) 010-2000", "");
	e = company(f, "Globex Corporation", "", "15550102000", "");
	g = company(f, "Initech", "", "", "https://www.initech.com/");
	h = company(f, "Initech LLC", "", "", "initech.com");
	i = company(f, "Unrelated Ltd", "hi@unrelated.example", "999", "unrelated.example");
	(void)i;
	g_assert_cmpint(scan(f, "company"), ==, 5);
	ab = candidate_for(f, a, b); g_assert_nonnull(ab);
	g_object_get(ab, "score", &score, "reasons", &reasons, NULL);
	g_assert_cmpint(score, ==, 100);
	g_assert_nonnull(strstr(reasons, "email"));
	g_assert_nonnull(strstr(reasons, "name"));
	g_clear_pointer(&reasons, g_free);
	ac = candidate_for(f, a, c); g_assert_nonnull(ac);
	g_object_get(ac, "score", &score, "reasons", &reasons, NULL);
	g_assert_cmpint(score, ==, 80);
	g_assert_nonnull(strstr(reasons, "domain+name"));
	g_clear_pointer(&reasons, g_free);
	de = candidate_for(f, d, e); g_assert_nonnull(de);
	g_object_get(de, "reasons", &reasons, NULL);
	g_assert_nonnull(strstr(reasons, "phone"));
	g_clear_pointer(&reasons, g_free);
	fg = candidate_for(f, g, h); g_assert_nonnull(fg);
	g_object_get(fg, "reasons", &reasons, NULL);
	g_assert_nonnull(strstr(reasons, "website"));
	g_assert_null(candidate_for(f, a, i));
	g_assert_null(candidate_for(f, c, d));

	/* A rescan is idempotent: no new rows, the same ids. */
	before = all_rows(f, "duplicate_candidate");
	g_assert_cmpint(scan(f, "company"), ==, 5);
	after = all_rows(f, "duplicate_candidate");
	g_assert_cmpuint(after->len, ==, before->len);
	g_assert_cmpint(venture_entity_get_id(g_ptr_array_index(after, after->len - 1)), ==,
		venture_entity_get_id(g_ptr_array_index(before, before->len - 1)));

	/* A pair that stops matching is dropped on the next scan. */
	{
		g_autoptr(VentureEntity) row = venture_database_get(f->db, VENTURE_TYPE_COMPANY, b, NULL);
		g_object_set(row, "name", "Zeta Holdings", "email", "zeta@zeta.example", NULL);
		save(f, row);
	}
	g_assert_cmpint(scan(f, "company"), ==, 3);
	g_assert_null(candidate_for(f, a, b));
	g_assert_null(candidate_for(f, b, c));
	ah = candidate_for(f, a, c); g_assert_nonnull(ah);
}

/* Contacts are scanned by the same rules, and each kind only sees its own. */
static void test_scan_contacts(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) pair = NULL;
	g_autofree gchar *kind = NULL;
	gint64 a, b;
	(void)unused;
	a = contact(f, "Alice Example", "Alice@Example.org", "", 0);
	b = contact(f, "A. Example", "alice+news@example.org", "", 0);
	contact(f, "Bob Other", "bob@other.example", "", 0);
	company(f, "Alice Example", "alice@example.org", "", "");
	g_assert_cmpint(scan(f, "contact"), ==, 1);
	pair = candidate_for(f, a, b);
	g_assert_nonnull(pair);
	g_object_get(pair, "kind", &kind, NULL);
	g_assert_cmpstr(kind, ==, "contact");
	g_assert_cmpint(scan(f, "company"), ==, 0);
}

/* A merge re-points every declared reference, unions the fields with the
 * survivor winning, notes what was not carried, soft-deletes the loser with
 * its forwarding pointer, closes the candidate and writes an audit row. */
static void test_merge(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) candidate = NULL, other = NULL, survivor = NULL, loser = NULL;
	g_autoptr(VentureEntity) interaction = record(f, "interaction");
	g_autoptr(VentureEntity) deal = record(f, "deal");
	g_autoptr(VentureEntity) invoice = record(f, "invoice");
	g_autoptr(VentureEntity) ticket = record(f, "ticket");
	g_autoptr(VentureEntity) activity = record(f, "activity");
	g_autoptr(VentureEntity) bill = record(f, "vendor_bill");
	g_autoptr(GPtrArray) notes = NULL, audits = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *phone = NULL, *industry = NULL, *name = NULL, *status = NULL, *notes_text = NULL;
	gint64 a, b, c, person, merged_into = 0, survivor_id = 0;
	(void)unused;
	a = company(f, "Acme Inc", "sales@acme.com", "", "acme.com");
	b = company(f, "ACME, Inc.", "sales@acme.com", "555 0100", "");
	c = company(f, "Acme Widgets", "bob@acme.com", "", "");
	{
		g_autoptr(VentureEntity) row = venture_database_get(f->db, VENTURE_TYPE_COMPANY, b, NULL);
		g_object_set(row, "industry", "Software", "notes", "Met at the fair", NULL);
		save(f, row);
	}
	person = contact(f, "Pat", "pat@acme.com", "", b);
	g_object_set(interaction, "company-id", b, "subject", "Call", NULL);
	field(interaction, "occurred-at", "2026-01-05"); save(f, interaction);
	g_object_set(deal, "name", "Big deal", "company-id", b, NULL); save(f, deal);
	g_object_set(invoice, "number", "INV-B", "company-id", b, "contact-id", person, NULL); save(f, invoice);
	g_object_set(ticket, "title", "Help", "company-id", b, NULL); save(f, ticket);
	g_object_set(activity, "subject", "Follow up", "company-id", b, NULL); save(f, activity);
	g_object_set(bill, "number", "BILL-B", "company-id", b, "currency", "USD", "status", "draft", NULL);
	field(bill, "bill-date", "2026-01-01"); field(bill, "due-date", "2026-01-31"); save(f, bill);

	g_assert_cmpint(scan(f, "company"), ==, 3);
	candidate = candidate_for(f, a, b); g_assert_nonnull(candidate);
	other = candidate_for(f, b, c); g_assert_nonnull(other);
	survivor = venture_dedupe_service_merge(venture_dedupe_service_get(f->db), candidate, a, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(survivor);
	g_assert_cmpint(venture_entity_get_id(survivor), ==, a);
	g_assert_false(venture_database_has_transaction(f->db));

	/* Every reference the field tables declare now names the survivor. */
	g_assert_cmpint(ref_id(f, "contact", person, "company-id"), ==, a);
	g_assert_cmpint(ref_id(f, "interaction", venture_entity_get_id(interaction), "company-id"), ==, a);
	g_assert_cmpint(ref_id(f, "deal", venture_entity_get_id(deal), "company-id"), ==, a);
	g_assert_cmpint(ref_id(f, "invoice", venture_entity_get_id(invoice), "company-id"), ==, a);
	g_assert_cmpint(ref_id(f, "ticket", venture_entity_get_id(ticket), "company-id"), ==, a);
	g_assert_cmpint(ref_id(f, "activity", venture_entity_get_id(activity), "company-id"), ==, a);
	g_assert_cmpint(ref_id(f, "vendor_bill", venture_entity_get_id(bill), "company-id"), ==, a);

	/* Union: empty survivor fields take the loser's value; conflicts keep
	 * the survivor's and are noted on its timeline. */
	phone = text_of(f, "company", a, "phone"); g_assert_cmpstr(phone, ==, "555 0100");
	industry = text_of(f, "company", a, "industry"); g_assert_cmpstr(industry, ==, "Software");
	notes_text = text_of(f, "company", a, "notes"); g_assert_cmpstr(notes_text, ==, "Met at the fair");
	name = text_of(f, "company", a, "name"); g_assert_cmpstr(name, ==, "Acme Inc");
	notes = rows(f, "interaction");
	g_assert_cmpuint(notes->len, ==, 2);
	{
		g_autofree gchar *body = NULL;
		gint64 on = 0;
		g_object_get(g_ptr_array_index(notes, 1), "body", &body, "company-id", &on, NULL);
		g_assert_cmpint(on, ==, a);
		g_assert_nonnull(strstr(body, "ACME, Inc."));
		g_assert_nonnull(strstr(body, "name"));
	}

	/* The loser is soft-deleted and forwards to the survivor. */
	loser = venture_database_get(f->db, VENTURE_TYPE_COMPANY, b, NULL);
	g_assert_true(venture_entity_is_deleted(loser));
	g_object_get(loser, "merged-into-id", &merged_into, NULL);
	g_assert_cmpint(merged_into, ==, a);
	g_assert_cmpint(venture_dedupe_merged_into(f->db, loser), ==, a);

	/* The candidate closes; any other candidate naming the loser is dropped. */
	g_clear_object(&candidate);
	candidate = venture_database_get(f->db, VENTURE_TYPE_DUPLICATE_CANDIDATE, venture_entity_get_id(other), NULL);
	g_assert_true(venture_entity_is_deleted(candidate));
	g_clear_object(&candidate);
	candidate = candidate_for(f, a, b);
	g_assert_nonnull(candidate);
	g_object_get(candidate, "status", &status, "survivor-id", &survivor_id, NULL);
	g_assert_cmpstr(status, ==, "merged");
	g_assert_cmpint(survivor_id, ==, a);

	/* One audit row records the merge against the survivor. */
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
		venture_query_add_filter_string(query, "source", VENTURE_FILTER_OP_EQ, "dedupe", NULL);
		venture_query_set_limit(query, 0);
		audits = venture_database_find(f->db, query, NULL);
		g_assert_cmpuint(audits->len, ==, 1);
		{
			g_autofree gchar *diff = NULL;
			gint64 target = 0;
			g_object_get(g_ptr_array_index(audits, 0), "diff", &diff, "target-id", &target, NULL);
			g_assert_cmpint(target, ==, a);
			g_assert_nonnull(strstr(diff, "invoice.company_id"));
		}
	}
	/* A second scan does not resurrect the merged pair. */
	g_assert_cmpint(scan(f, "company"), ==, 1);
}

static gboolean refuse_note(VentureDatabase *db, VentureEntity *entity, VentureEntity *previous, gpointer data, GError **error)
{
	(void)db; (void)entity; (void)previous; (void)data;
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_DATABASE, "disk full");
	return FALSE;
}
/* A failure after the references moved leaves nothing changed. */
static void test_merge_atomic(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) candidate = NULL, result = NULL, loser = NULL;
	g_autoptr(VentureEntity) deal = record(f, "deal");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *phone = NULL, *status = NULL;
	gint64 a, b;
	(void)unused;
	a = company(f, "Acme Inc", "sales@acme.com", "", "");
	b = company(f, "ACME, Inc.", "sales@acme.com", "555 0100", "");
	g_object_set(deal, "name", "Big deal", "company-id", b, NULL); save(f, deal);
	g_assert_cmpint(scan(f, "company"), ==, 1);
	candidate = candidate_for(f, a, b);
	venture_database_add_save_validator(f->db, VENTURE_TYPE_INTERACTION, refuse_note, NULL, NULL);
	result = venture_dedupe_service_merge(venture_dedupe_service_get(f->db), candidate, a, NULL, &error);
	g_assert_null(result);
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "disk full"));
	g_assert_false(venture_database_has_transaction(f->db));
	g_assert_cmpint(ref_id(f, "deal", venture_entity_get_id(deal), "company-id"), ==, b);
	phone = text_of(f, "company", a, "phone");
	g_assert_true(venture_string_is_empty(phone));
	loser = venture_database_get(f->db, VENTURE_TYPE_COMPANY, b, NULL);
	g_assert_false(venture_entity_is_deleted(loser));
	g_clear_object(&candidate);
	candidate = candidate_for(f, a, b);
	g_object_get(candidate, "status", &status, NULL);
	g_assert_cmpstr(status, ==, "open");
}

/* Refusals: another organisation, itself, and posted history in another
 * currency. A survivor with no posted history accepts any currency. */
static void test_refusals(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) org = g_object_new(VENTURE_TYPE_ORGANIZATION, "name", "Elsewhere", "slug", "elsewhere", NULL);
	g_autoptr(VentureEntity) foreign = NULL, result = NULL;
	g_autoptr(GError) error = NULL;
	VentureDedupeService *service = venture_dedupe_service_get(f->db);
	gint64 a, b, c, other;
	(void)unused;
	save(f, org);
	foreign = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", venture_entity_get_id(org), "name", "Acme Inc", "email", "sales@acme.com", NULL);
	save(f, foreign);
	other = venture_entity_get_id(foreign);
	a = company(f, "Acme Inc", "sales@acme.com", "", "");
	b = company(f, "ACME, Inc.", "sales@acme.com", "", "");
	c = company(f, "Acme Widgets", "bob@acme.com", "", "");

	result = venture_dedupe_service_merge_records(service, "company", a, other, NULL, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "organization"));
	g_clear_error(&error);

	result = venture_dedupe_service_merge_records(service, "company", a, a, NULL, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_assert_nonnull(strstr(error->message, "itself"));
	g_clear_error(&error);

	/* Issued invoices in two currencies. */
	{
		const gchar *amounts[] = { "40 USD", "40 EUR" };
		gint64 owners[] = { a, b };
		guint i;
		for (i = 0; i < 2; i++)
		{
			g_autoptr(VentureEntity) invoice = record(f, "invoice");
			g_autoptr(VentureEntity) line = record(f, "invoice_line");
			g_autofree gchar *number = g_strdup_printf("INV-%u", i);
			g_object_set(invoice, "number", number, "company-id", owners[i], NULL);
			field(invoice, "issued-at", "2026-01-10"); save(f, invoice);
			g_object_set(line, "invoice-id", venture_entity_get_id(invoice), "description", "Work", "quantity", 1.0, NULL);
			field(line, "unit-price", amounts[i]); save(f, line);
			g_object_set(invoice, "status", VENTURE_INVOICE_STATUS_SENT, NULL); save(f, invoice);
		}
	}
	result = venture_dedupe_service_merge_records(service, "company", a, b, NULL, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_assert_nonnull(strstr(error->message, "EUR"));
	g_clear_error(&error);
	/* The other direction is refused too: USD history into a EUR record. */
	result = venture_dedupe_service_merge_records(service, "company", b, a, NULL, &error);
	g_assert_null(result);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT);
	g_clear_error(&error);
	/* No posted history on the survivor: the EUR record folds into it. */
	result = venture_dedupe_service_merge_records(service, "company", c, b, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	g_assert_cmpint(venture_entity_get_id(result), ==, c);
}

/* Candidates and the forwarding pointer are written by the service only. */
static void test_generic_writes(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) candidate = record(f, "duplicate_candidate");
	g_autoptr(VentureEntity) row = NULL;
	g_autoptr(GError) error = NULL;
	gint64 a, b;
	(void)unused;
	a = company(f, "Acme Inc", "", "", "");
	b = company(f, "Beta", "", "", "");
	g_object_set(candidate, "kind", "company", "record-a", a, "record-b", b, "pair-key", "company:1:2", "status", "open", NULL);
	g_assert_false(venture_database_save(f->db, candidate, NULL, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "VentureDedupeService"));
	g_clear_error(&error);
	row = venture_database_get(f->db, VENTURE_TYPE_COMPANY, b, NULL);
	g_object_set(row, "merged-into-id", a, NULL);
	g_assert_false(venture_database_save(f->db, row, NULL, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "VentureDedupeService"));
}

/* Switching the module off hides the candidates and refuses the scan. */
static void test_module_off(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	(void)unused;
	company(f, "Acme Inc", "sales@acme.com", "", "");
	company(f, "ACME, Inc.", "sales@acme.com", "", "");
	venture_config_set_module_enabled(f->config, "dedupe", FALSE);
	g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), "duplicate_candidate"), ==, G_TYPE_INVALID);
	g_assert_cmpint(venture_dedupe_service_scan(venture_dedupe_service_get(f->db), f->org, "company", NULL, &error), ==, -1);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	venture_config_set_module_enabled(f->config, "dedupe", TRUE);
	g_assert_cmpint(scan(f, "company"), ==, 1);
}

/* REST, the page and the CLI reach the same service; a merged id forwards. */
typedef struct { gboolean done; GError *error; GBytes *bytes; gchar *out, *err; } Result;
typedef struct { Fixture base; VentureWebServer *server; gchar *directory; } ServerFixture;
static void server_setup(ServerFixture *s, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocketListener) listener = g_socket_listener_new();
	guint16 port = g_socket_listener_add_any_inet_port(listener, NULL, &error);
	g_assert_no_error(error);
	g_socket_listener_close(listener);
	s->directory = g_dir_make_tmp("venture-dedupe-XXXXXX", &error);
	g_assert_no_error(error);
	setup(&s->base, unused);
	g_object_set(s->base.config, "state-dir", s->directory, "server-bind-address", "127.0.0.1", "server-port", (gint64)port, "security-require-auth", FALSE, NULL);
	s->server = venture_web_server_new(s->base.context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(s->server, &error));
	g_assert_no_error(error);
}
static void server_teardown(ServerFixture *s, gconstpointer unused)
{
	venture_web_server_stop(s->server);
	g_clear_object(&s->server);
	teardown(&s->base, unused);
	venture_test_remove_tree(s->directory); g_free(s->directory);
}
static void http_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Result *r = data;
	r->bytes = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &r->error);
	r->done = TRUE;
}
static guint request(ServerFixture *s, const gchar *method, const gchar *path, const gchar *body, gchar **out, gchar **location)
{
	g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 15, NULL);
	g_autofree gchar *url = g_strconcat(venture_web_server_get_base_url(s->server), path, NULL);
	g_autoptr(SoupMessage) message = soup_message_new(method, url);
	Result result;
	memset(&result, 0, sizeof(result));
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (body)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(message,
			body[0] == '{' ? "application/json" : "application/x-www-form-urlencoded", bytes);
	}
	soup_session_send_and_read_async(session, message, G_PRIORITY_DEFAULT, NULL, http_done, &result);
	while (!result.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(result.error);
	if (out) *out = g_strndup(g_bytes_get_data(result.bytes, NULL), g_bytes_get_size(result.bytes));
	if (location) *location = g_strdup(soup_message_headers_get_one(soup_message_get_response_headers(message), "Location"));
	g_bytes_unref(result.bytes);
	return soup_message_get_status(message);
}
static void cli_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Result *r = data;
	g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &r->out, &r->err, &r->error);
	r->done = TRUE;
}
static gboolean cli_timeout(gpointer process) { g_subprocess_force_exit(process); return G_SOURCE_CONTINUE; }
static gchar *cli(ServerFixture *s, const gchar *const *args, gboolean expect_success)
{
	g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
	g_autoptr(GSubprocess) process = NULL;
	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GError) error = NULL;
	Result result;
	guint i, timeout;
	memset(&result, 0, sizeof(result));
	g_ptr_array_add(argv, g_canonicalize_filename("build/debug/venturectl", NULL));
	g_ptr_array_add(argv, g_strdup("--server")); g_ptr_array_add(argv, g_strdup(venture_web_server_get_base_url(s->server)));
	g_ptr_array_add(argv, g_strdup("-f")); g_ptr_array_add(argv, g_strdup("json"));
	for (i = 0; args[i]; i++) g_ptr_array_add(argv, g_strdup(args[i]));
	g_ptr_array_add(argv, NULL);
	g_subprocess_launcher_setenv(launcher, "VENTURE_TOKEN", "dedupe-fixture", TRUE);
	process = g_subprocess_launcher_spawnv(launcher, (const gchar *const *)argv->pdata, &error);
	g_assert_no_error(error);
	timeout = g_timeout_add_seconds(30, cli_timeout, process);
	g_subprocess_communicate_utf8_async(process, NULL, NULL, cli_done, &result);
	while (!result.done) g_main_context_iteration(NULL, TRUE);
	g_source_remove(timeout);
	g_assert_no_error(result.error);
	if (g_subprocess_get_successful(process) != expect_success) g_test_message("CLI: %s%s", result.out, result.err);
	g_assert_true(g_subprocess_get_successful(process) == expect_success);
	g_free(result.err);
	return result.out;
}
static void test_surfaces(ServerFixture *s, gconstpointer unused)
{
	Fixture *f = &s->base;
	g_autoptr(VentureEntity) ab = NULL, cd = NULL, ef = NULL;
	g_autofree gchar *page = NULL, *body = NULL, *location = NULL, *out = NULL, *bad = NULL;
	g_autofree gchar *api_path = NULL, *ui_path = NULL, *merge_path = NULL, *ui_merge = NULL, *form = NULL;
	g_autofree gchar *candidate_id = NULL, *survivor_arg = NULL;
	const gchar *scan_args[] = { "dedupe", "scan", "kind=company", NULL };
	const gchar *bad_args[] = { "dedupe", "nope", NULL };
	const gchar *merge_args[] = { "dedupe", "merge", NULL, NULL, NULL };
	gint64 a, b, c, d, e, g;
	(void)unused;
	a = company(f, "Acme Inc", "sales@acme.com", "", "");
	b = company(f, "ACME, Inc.", "sales@acme.com", "", "");
	c = company(f, "Globex", "", "555 0100", "");
	d = company(f, "Globex Corp", "", "555-0100", "");
	e = company(f, "Initech", "", "", "initech.com");
	g = company(f, "Initech LLC", "", "", "www.initech.com");

	/* The scan action over REST and the CLI. */
	g_assert_cmpuint(request(s, "POST", "/api/v1/duplicate_candidate/0/actions/scan", "{\"kind\":\"company\"}", &body, NULL), ==, 200);
	ab = candidate_for(f, a, b); g_assert_nonnull(ab);
	cd = candidate_for(f, c, d); g_assert_nonnull(cd);
	ef = candidate_for(f, e, g); g_assert_nonnull(ef);
	out = cli(s, scan_args, TRUE);
	g_assert_nonnull(strstr(out, "\"open\""));
	bad = cli(s, bad_args, FALSE);

	/* The page lists both sides of a pair with a merge button each way. */
	g_assert_cmpuint(request(s, "GET", "/customers/duplicates", NULL, &page, NULL), ==, 200);
	g_assert_nonnull(strstr(page, "Acme Inc"));
	g_assert_nonnull(strstr(page, "ACME, Inc."));
	g_assert_nonnull(strstr(page, "sales@acme.com"));
	g_assert_nonnull(strstr(page, "/merge"));

	/* REST merge of the first pair; the loser then forwards. */
	merge_path = g_strdup_printf("/api/v1/duplicate_candidate/%" G_GINT64_FORMAT "/actions/merge", venture_entity_get_id(ab));
	form = g_strdup_printf("{\"survivor\":%" G_GINT64_FORMAT "}", a);
	g_clear_pointer(&body, g_free);
	g_assert_cmpuint(request(s, "POST", merge_path, form, &body, NULL), ==, 200);
	g_assert_nonnull(strstr(body, "\"merged\""));
	api_path = g_strdup_printf("/api/v1/company/%" G_GINT64_FORMAT, b);
	g_assert_cmpuint(request(s, "GET", api_path, NULL, NULL, &location), ==, 301);
	g_clear_pointer(&page, g_free);
	page = g_strdup_printf("/api/v1/company/%" G_GINT64_FORMAT, a);
	g_assert_cmpstr(location, ==, page);
	ui_path = g_strdup_printf("/e/company/%" G_GINT64_FORMAT, b);
	g_clear_pointer(&location, g_free);
	g_assert_cmpuint(request(s, "GET", ui_path, NULL, NULL, &location), ==, 301);
	g_clear_pointer(&page, g_free);
	page = g_strdup_printf("/e/company/%" G_GINT64_FORMAT, a);
	g_assert_cmpstr(location, ==, page);

	/* The page's own merge button, then the CLI verb for the third pair. */
	ui_merge = g_strdup_printf("/customers/duplicates/%" G_GINT64_FORMAT "/merge", venture_entity_get_id(cd));
	g_clear_pointer(&form, g_free);
	form = g_strdup_printf("survivor=%" G_GINT64_FORMAT, d);
	g_clear_pointer(&location, g_free);
	g_assert_cmpuint(request(s, "POST", ui_merge, form, NULL, &location), ==, 302);
	g_assert_cmpstr(location, ==, "/customers/duplicates");
	g_assert_cmpint(venture_dedupe_merged_into(f->db, NULL), ==, 0);
	{
		g_autoptr(VentureEntity) loser = venture_database_get(f->db, VENTURE_TYPE_COMPANY, c, NULL);
		g_assert_cmpint(venture_dedupe_merged_into(f->db, loser), ==, d);
	}
	candidate_id = g_strdup_printf("%" G_GINT64_FORMAT, venture_entity_get_id(ef));
	survivor_arg = g_strdup_printf("survivor=%" G_GINT64_FORMAT, e);
	merge_args[2] = candidate_id; merge_args[3] = survivor_arg;
	g_clear_pointer(&out, g_free);
	out = cli(s, merge_args, TRUE);
	{
		g_autoptr(VentureEntity) loser = venture_database_get(f->db, VENTURE_TYPE_COMPANY, g, NULL);
		g_assert_true(venture_entity_is_deleted(loser));
	}
	/* Nothing is left to propose, and the page says so. */
	g_clear_pointer(&page, g_free);
	g_assert_cmpuint(request(s, "GET", "/customers/duplicates", NULL, &page, NULL), ==, 200);
	g_assert_nonnull(strstr(page, "No duplicates"));
}

/* Upgrading with the module disabled keeps the CRM tables and adds nothing. */
static void test_migrate_disabled(void)
{
	g_autoptr(VentureConfig) config = venture_config_new();
	g_autoptr(VentureDatabase) db = NULL;
	g_autoptr(VentureContext) context = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(OrmResult) result = NULL;
	g_autoptr(VentureConfig) everything = NULL;
	g_autoptr(VentureModuleRegistry) registry = NULL;

	venture_config_set_module_enabled(config, "dedupe", FALSE);
	db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	context = venture_context_new(config, db);
	g_assert_true(venture_database_migrate(db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	result = venture_database_query_raw(db,
		"SELECT CAST(COUNT(*) AS BIGINT) FROM sqlite_master WHERE type = 'table' AND name = 'duplicate_candidates'",
		NULL, &error);
	g_assert_no_error(error);
	g_assert_true(orm_result_next(result));
	g_assert_cmpint(orm_row_get_integer(orm_result_get_row(result), 0), ==, 0);
	g_clear_object(&result);
	result = venture_database_query_raw(db,
		"SELECT CAST(COUNT(*) AS BIGINT) FROM pragma_table_info('companies') WHERE name = 'merged_into_id'",
		NULL, &error);
	g_assert_no_error(error);
	g_assert_true(orm_result_next(result));
	g_assert_cmpint(orm_row_get_integer(orm_result_get_row(result), 0), ==, 1);
	g_clear_object(&context);
	g_clear_object(&db);
	everything = venture_config_new();
	registry = venture_module_registry_new();
	venture_module_registry_register_builtins(registry);
	g_assert_true(venture_module_registry_configure(registry, everything, NULL));
	venture_module_registry_apply(registry, venture_entity_registry_get_default());
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add("/dedupe/name-similarity", Fixture, NULL, setup, test_name_similarity, teardown);
	g_test_add("/dedupe/scan-companies", Fixture, NULL, setup, test_scan_companies, teardown);
	g_test_add("/dedupe/scan-contacts", Fixture, NULL, setup, test_scan_contacts, teardown);
	g_test_add("/dedupe/merge", Fixture, NULL, setup, test_merge, teardown);
	g_test_add("/dedupe/merge-atomic", Fixture, NULL, setup, test_merge_atomic, teardown);
	g_test_add("/dedupe/refusals", Fixture, NULL, setup, test_refusals, teardown);
	g_test_add("/dedupe/generic-writes", Fixture, NULL, setup, test_generic_writes, teardown);
	g_test_add("/dedupe/module-off", Fixture, NULL, setup, test_module_off, teardown);
	g_test_add("/dedupe/surfaces", ServerFixture, NULL, server_setup, test_surfaces, server_teardown);
	g_test_add_func("/dedupe/migrate-disabled", test_migrate_disabled);
	return g_test_run();
}
