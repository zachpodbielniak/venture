/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
#include <string.h>
#include <glib/gstdio.h>
#include "venture-test-util.h"

typedef struct {
	VentureDatabase *db;
	VentureFakeImapClient *imap;
	VentureMailSyncService *service;
	gchar *root;
	gint64 org;
	gint64 company;
	gint64 contact;
} Fixture;

static void save(Fixture *f, VentureEntity *e)
{
	g_autoptr(GError) error = NULL;
	g_assert_true(venture_database_save(f->db, e, NULL, &error));
	g_assert_no_error(error);
}

static gint64 count_type(Fixture *f, const gchar *name)
{
	g_autoptr(VentureQuery) q = venture_query_new(venture_entity_registry_lookup(venture_entity_registry_get_default(), name));
	venture_query_set_organization(q, f->org);
	return venture_database_count(f->db, q, NULL);
}

static void setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) company = NULL, contact = NULL;
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
		g_autoptr(VentureEntity) org = venture_database_find_one(f->db, query, &error);
		f->org = venture_entity_get_id(org);
	}
	f->root = g_dir_make_tmp("venture-mail-sync-XXXXXX", &error);
	g_assert_no_error(error);
	company = g_object_new(VENTURE_TYPE_COMPANY, "organization-id", f->org, "name", "Acme", NULL);
	save(f, company);
	f->company = venture_entity_get_id(company);
	contact = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", f->org, "name", "Ada", "email", "Ada+crm@Example.test", "company-id", f->company, NULL);
	save(f, contact);
	f->contact = venture_entity_get_id(contact);
	f->imap = venture_fake_imap_client_new();
	f->service = venture_mail_sync_service_new(f->db, VENTURE_IMAP_CLIENT(f->imap));
	g_object_set(f->service, "attachment-root", f->root, NULL);
	g_setenv("VENTURE_TEST_IMAP_SECRET", "app-password", TRUE);
}

static void teardown(Fixture *f, gconstpointer data)
{
	g_autofree gchar *real = realpath(f->root, NULL);
	g_clear_object(&f->service);
	g_clear_object(&f->imap);
	g_clear_object(&f->db);
	if (real) venture_test_remove_within(f->root, real);
	g_free(f->root);
}

static VentureEntity *account(Fixture *f, const gchar *secret_env)
{
	VentureEntity *a = g_object_new(VENTURE_TYPE_MAIL_ACCOUNT, "organization-id", f->org,
		"address", "ops@venture.test", "imap-host", "imap.venture.test", "imap-port", (gint64)993, "imap-tls", "tls",
		"username", "ops@venture.test", "secret-env", secret_env, "folders", "INBOX",
		"capture-address", "receipts@venture.test", "capture-folder", "Receipts", "active", TRUE, NULL);
	save(f, a);
	return a;
}

static const gchar *msg_ada =
	"From: Ada <ada@example.test>\r\nTo: ops@venture.test\r\nSubject: Hello\r\nDate: Mon, 14 Sep 2026 10:00:00 +0000\r\n"
	"Message-ID: <one@example.test>\r\nContent-Type: text/plain\r\n\r\nFirst message.\r\n";
static const gchar *msg_reply =
	"From: ops@venture.test\r\nTo: Ada <ada@example.test>\r\nCc: stranger@else.test\r\nSubject: Re: Hello\r\nDate: Mon, 14 Sep 2026 11:00:00 +0000\r\n"
	"Message-ID: <two@venture.test>\r\nIn-Reply-To: <one@example.test>\r\nReferences: <one@example.test>\r\nContent-Type: text/plain\r\n\r\nReply.\r\n";
static const gchar *msg_receipt =
	"From: Shop <shop@store.test>\r\nTo: receipts@venture.test\r\nSubject: Your receipt\r\nDate: Tue, 15 Sep 2026 09:00:00 +0000\r\n"
	"Message-ID: <r1@store.test>\r\nMIME-Version: 1.0\r\nContent-Type: multipart/mixed; boundary=\"b\"\r\n\r\n"
	"--b\r\nContent-Type: text/plain\r\n\r\nThanks for your order.\r\n"
	"--b\r\nContent-Type: text/csv; name=\"receipt.csv\"\r\nContent-Disposition: attachment; filename=\"receipt.csv\"\r\n\r\nitem,amount\r\ntoner,12.50\r\n"
	"--b--\r\n";

static void test_records(void)
{
	VentureEntityRegistry *r = venture_entity_registry_get_default();
	g_assert_cmpuint(venture_entity_registry_lookup(r, "mail_account"), !=, G_TYPE_INVALID);
	g_assert_cmpuint(venture_entity_registry_lookup(r, "mail_inbound"), !=, G_TYPE_INVALID);
	g_assert_cmpuint(venture_entity_registry_lookup(r, "mail_unmatched_sender"), !=, G_TYPE_INVALID);
	{
		g_autoptr(VentureModuleRegistry) modules = venture_module_registry_new();
		venture_module_registry_register_builtins(modules);
		g_assert_nonnull(venture_module_registry_lookup(modules, "mail_sync"));
	}
}

/* Rule 1: a missing secret fails the sync with an error naming the variable. */
static void test_missing_secret(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_TEST_IMAP_MISSING");
	g_autoptr(GError) error = NULL;
	g_unsetenv("VENTURE_TEST_IMAP_MISSING");
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, -1);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG);
	g_assert_nonnull(strstr(error->message, "VENTURE_TEST_IMAP_MISSING"));
	g_assert_cmpint(venture_fake_imap_client_get_connects(f->imap), ==, 0);
}

/* Rules 2 and 3: UID high-water mark, raw documents, matched interactions, threads, unmatched senders. */
static void test_sync_matches_and_threads(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_TEST_IMAP_SECRET");
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) q = NULL;
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *cursors = NULL;
	venture_fake_imap_client_add_message(f->imap, "INBOX", 5, msg_ada);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 7, msg_reply);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 2);
	g_assert_no_error(error);
	g_assert_cmpint(count_type(f, "mail_inbound"), ==, 2);
	g_assert_cmpint(count_type(f, "document"), ==, 2);
	g_assert_cmpint(count_type(f, "interaction"), ==, 2);
	g_assert_cmpint(count_type(f, "mail_unmatched_sender"), ==, 1);
	g_object_get(a, "cursors", &cursors, NULL);
	g_assert_nonnull(strstr(cursors, "\"INBOX\""));
	g_assert_nonnull(strstr(cursors, "7"));
	/* Same fake mailbox again: nothing new. */
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 0);
	g_assert_no_error(error);
	g_assert_cmpint(count_type(f, "mail_inbound"), ==, 2);
	g_assert_cmpint(count_type(f, "interaction"), ==, 2);
	g_assert_cmpint(count_type(f, "mail_unmatched_sender"), ==, 1);
	q = venture_query_new(VENTURE_TYPE_INTERACTION);
	venture_query_set_organization(q, f->org);
	venture_query_add_order(q, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(f->db, q, &error);
	g_assert_cmpuint(rows->len, ==, 2);
	{
		VentureEntity *first = g_ptr_array_index(rows, 0), *second = g_ptr_array_index(rows, 1);
		VentureInteractionKind kind;
		gint64 contact = 0, company = 0;
		gboolean outbound = TRUE;
		g_object_get(first, "kind", &kind, "contact-id", &contact, "company-id", &company, "outbound", &outbound, NULL);
		g_assert_cmpint(kind, ==, VENTURE_INTERACTION_KIND_EMAIL);
		g_assert_cmpint(contact, ==, f->contact);
		g_assert_cmpint(company, ==, f->company);
		g_assert_false(outbound);
		g_object_get(second, "outbound", &outbound, NULL);
		g_assert_true(outbound);
	}
	g_clear_object(&q);
	g_clear_pointer(&rows, g_ptr_array_unref);
	q = venture_query_new(VENTURE_TYPE_MAIL_INBOUND);
	venture_query_set_organization(q, f->org);
	venture_query_add_order(q, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(f->db, q, &error);
	{
		g_autofree gchar *t1 = NULL, *t2 = NULL, *mid = NULL;
		gint64 interaction = 0;
		g_object_get(g_ptr_array_index(rows, 0), "thread-id", &t1, "message-id", &mid, "interaction-id", &interaction, NULL);
		g_object_get(g_ptr_array_index(rows, 1), "thread-id", &t2, NULL);
		g_assert_cmpstr(mid, ==, "one@example.test");
		g_assert_cmpstr(t1, ==, t2);
		g_assert_cmpint(interaction, >, 0);
	}
	g_clear_object(&q);
	g_clear_pointer(&rows, g_ptr_array_unref);
	q = venture_query_new(VENTURE_TYPE_MAIL_UNMATCHED_SENDER);
	venture_query_set_organization(q, f->org);
	rows = venture_database_find(f->db, q, &error);
	{
		g_autofree gchar *address = NULL;
		g_autoptr(VentureEntity) contact = NULL;
		gint64 seen = 0;
		g_object_get(g_ptr_array_index(rows, 0), "address", &address, "seen", &seen, NULL);
		g_assert_cmpstr(address, ==, "stranger@else.test");
		g_assert_cmpint(seen, ==, 1);
		/* One click: the unmatched sender becomes a contact and leaves the list. */
		contact = venture_mail_sync_service_create_contact(f->service, g_ptr_array_index(rows, 0), NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(contact);
		g_assert_cmpint(count_type(f, "mail_unmatched_sender"), ==, 0);
		g_assert_cmpint(count_type(f, "contact"), ==, 2);
	}
}

/* Rule 4: capture-address mail becomes an inbox item with its attachment; same hash is not duplicated. */
static void test_capture(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_TEST_IMAP_SECRET");
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) q = NULL;
	g_autoptr(VentureEntity) item = NULL, document = NULL;
	g_autofree gchar *source = NULL, *hash = NULL, *text = NULL, *title = NULL;
	gint64 document_id = 0;
	/* The receipt was already captured by upload: the same content hash. */
	{
		g_autoptr(VentureEntity) uploaded = g_object_new(VENTURE_TYPE_DOCUMENT, "organization-id", f->org, "title", "receipt.csv",
			"hash", g_compute_checksum_for_string(G_CHECKSUM_SHA256, "item,amount\r\ntoner,12.50", -1), NULL);
		g_autoptr(VentureEntity) prior = NULL;
		save(f, uploaded);
		prior = venture_capture_service_ingest_for_organization(venture_capture_service_get(f->db), f->org, "receipt", "receipt.csv", "upload",
			venture_entity_get_id(uploaded), NULL, NULL, NULL, NULL, NULL, &error);
		g_assert_no_error(error);
	}
	venture_fake_imap_client_add_message(f->imap, "INBOX", 1, msg_receipt);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpint(count_type(f, "capture_item"), ==, 1);
	g_assert_cmpint(count_type(f, "interaction"), ==, 0);
	/* A genuinely new receipt in the capture folder lands in the inbox with its attachment filed. */
	venture_fake_imap_client_add_message(f->imap, "Receipts", 3,
		"From: Shop <shop@store.test>\r\nTo: ops@venture.test\r\nSubject: Invoice 9\r\nMessage-ID: <r2@store.test>\r\nMIME-Version: 1.0\r\nContent-Type: multipart/mixed; boundary=\"b\"\r\n\r\n"
		"--b\r\nContent-Type: text/plain\r\n\r\nInvoice attached.\r\n--b\r\nContent-Type: text/plain; name=\"inv.txt\"\r\nContent-Disposition: attachment; filename=\"inv.txt\"\r\n\r\nTotal 99.00\r\n--b--\r\n");
	g_object_set(a, "folders", "INBOX,Receipts", NULL);
	save(f, a);
	g_assert_cmpint(venture_mail_sync_service_sync(f->service, a, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpint(count_type(f, "capture_item"), ==, 2);
	q = venture_query_new(VENTURE_TYPE_CAPTURE_ITEM);
	venture_query_set_organization(q, f->org);
	venture_query_add_filter_string(q, "source", VENTURE_FILTER_OP_EQ, "email", NULL);
	item = venture_database_find_one(f->db, q, &error);
	g_assert_nonnull(item);
	g_object_get(item, "source", &source, "document-id", &document_id, "title", &title, NULL);
	g_assert_cmpstr(title, ==, "Invoice 9");
	g_assert_cmpint(document_id, >, 0);
	document = venture_database_get(f->db, VENTURE_TYPE_DOCUMENT, document_id, &error);
	g_object_get(document, "hash", &hash, "extracted-text", &text, NULL);
	g_assert_cmpstr(hash, ==, g_compute_checksum_for_string(G_CHECKSUM_SHA256, "Total 99.00", -1));
	g_assert_cmpstr(text, ==, "Total 99.00");
}

/* Rule 5: outbound mail through the outbox is an interaction on the recipient contact. */
static void test_outbound_recorded(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureLogMailer) mailer = venture_log_mailer_new();
	g_autoptr(VentureMailOutbox) outbox = venture_mail_outbox_new(f->db, VENTURE_MAILER(mailer));
	g_autoptr(VentureMailMessage) message = venture_mail_message_new();
	g_autoptr(VentureMailMessage) queued = NULL;
	g_autoptr(VentureQuery) q = NULL;
	g_autoptr(VentureEntity) interaction = NULL;
	g_autoptr(GError) error = NULL;
	gboolean outbound = FALSE;
	gint64 contact = 0;
	g_object_set(message, "organization-id", f->org, "to", "ada+crm@example.test", "subject", "Quote", "text-body", "Attached.", "idempotency-key", "q1", NULL);
	queued = venture_mail_outbox_enqueue(outbox, message, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(count_type(f, "interaction"), ==, 0);
	g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, f->org, 10, NULL, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpint(count_type(f, "interaction"), ==, 1);
	q = venture_query_new(VENTURE_TYPE_INTERACTION);
	venture_query_set_organization(q, f->org);
	interaction = venture_database_find_one(f->db, q, &error);
	g_object_get(interaction, "outbound", &outbound, "contact-id", &contact, NULL);
	g_assert_true(outbound);
	g_assert_cmpint(contact, ==, f->contact);
	/* A second sweep does not deliver or record again. */
	g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, f->org, 10, NULL, NULL, &error), ==, 0);
	g_assert_cmpint(count_type(f, "interaction"), ==, 1);
}

/* Rule 6: the sweep over an organization's accounts, and its refusal of a disabled account. */
static void test_sweep(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) a = account(f, "VENTURE_TEST_IMAP_SECRET");
	g_autoptr(VentureEntity) off = account(f, "VENTURE_TEST_IMAP_MISSING");
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) report = NULL;
	g_object_set(off, "active", FALSE, NULL);
	save(f, off);
	venture_fake_imap_client_add_message(f->imap, "INBOX", 2, msg_ada);
	report = venture_mail_sync_service_sweep(f->service, f->org, 100, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(report);
	g_assert_cmpint(venture_json_object_get_int(json_node_get_object(report), "accounts", 0), ==, 1);
	g_assert_cmpint(venture_json_object_get_int(json_node_get_object(report), "messages", 0), ==, 1);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/mail-sync/records", test_records);
	g_test_add("/mail-sync/missing-secret", Fixture, NULL, setup, test_missing_secret, teardown);
	g_test_add("/mail-sync/matches-and-threads", Fixture, NULL, setup, test_sync_matches_and_threads, teardown);
	g_test_add("/mail-sync/capture", Fixture, NULL, setup, test_capture, teardown);
	g_test_add("/mail-sync/outbound", Fixture, NULL, setup, test_outbound_recorded, teardown);
	g_test_add("/mail-sync/sweep", Fixture, NULL, setup, test_sweep, teardown);
	return g_test_run();
}
