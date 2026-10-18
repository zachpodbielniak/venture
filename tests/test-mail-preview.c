/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * Previews of every outgoing email and their pre-send checks.
 *
 * The point of a preview is that it is the message: if a preview built from
 * a record and the row its sender enqueues ever disagree, an operator
 * approves one email and the customer reads another. So the kind tests
 * build a preview, let the real sender enqueue, preview the stored row and
 * compare. Nothing here touches the network: reachability goes through a
 * fake checker, and the default checker is only asked about an address its
 * guard refuses before any request.
 */
#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"

typedef struct {
	VentureDatabase *db;
	VentureContext *context;
	VentureConfig *config;
	VentureLogMailer *mailer;
	gchar *directory;
	gint64 org, company, contact, invoice, policy;
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
static void setup(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) company = NULL, contact = NULL, invoice = NULL, line = NULL, policy = NULL, template = NULL;
	g_autoptr(GDateTime) issued = venture_time_from_string("2026-01-01", NULL);
	g_autofree gchar *steps = NULL;
	(void)unused;
	f->directory = g_dir_make_tmp("venture-mail-preview-XXXXXX", &error);
	g_assert_no_error(error);
	f->config = venture_config_new();
	g_object_set(f->config, "state-dir", f->directory, "server-base-url", "https://books.example.com", NULL);
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
	f->mailer = venture_log_mailer_new();
	venture_context_set_mailer(f->context, VENTURE_MAILER(f->mailer));
	g_object_set(venture_database_get_mail_outbox(f->db), "mailer", f->mailer, NULL);
	company = record(f, "company");
	g_object_set(company, "name", "Acme & Sons", "email", "accounts@example.com", NULL);
	save(f, company); f->company = venture_entity_get_id(company);
	contact = record(f, "contact");
	g_object_set(contact, "name", "Alice", "email", "alice@example.com", "company-id", f->company, NULL);
	save(f, contact); f->contact = venture_entity_get_id(contact);
	template = record(f, "mail_template");
	g_object_set(template, "name", "Reminder", "subject", "Invoice {number}",
		"text-body", "Hello {customer_name}: {number}, due {due_at}, balance {open_balance}. Pay here: {pay_link}",
		"html-body", "<p>Hello {customer_name}</p><p><a href=\"{pay_link}\">Pay now</a></p>", NULL);
	save(f, template);
	policy = record(f, "dunning_policy");
	steps = g_strdup_printf("[{\"offset\":-3,\"template_id\":%" G_GINT64_FORMAT "},{\"offset\":7,\"template_id\":%" G_GINT64_FORMAT "}]",
		venture_entity_get_id(template), venture_entity_get_id(template));
	g_object_set(policy, "name", "Standard", "steps", steps, "is-default", TRUE, "final-escalation", FALSE, NULL);
	field(policy, "adopted-at", "2026-01-01");
	save(f, policy); f->policy = venture_entity_get_id(policy);
	invoice = record(f, "invoice");
	g_object_set(invoice, "number", "INV-PRE", "company-id", f->company, "contact-id", f->contact, "terms", "Net 9", NULL);
	field(invoice, "issued-at", "2026-01-01"); field(invoice, "due-at", "2026-01-10");
	save(f, invoice); f->invoice = venture_entity_get_id(invoice);
	line = record(f, "invoice_line");
	g_object_set(line, "invoice-id", f->invoice, "description", "Bookkeeping", "quantity", 1.0, NULL);
	field(line, "unit-price", "40 USD"); save(f, line);
	g_assert_true(venture_settlement_service_transition(venture_settlement_service_get(f->db), VENTURE_INVOICE(invoice), "sent", issued, NULL, &error));
	g_assert_no_error(error);
}
static void teardown(Fixture *f, gconstpointer unused)
{
	(void)unused;
	g_clear_object(&f->context); g_clear_object(&f->db);
	g_clear_object(&f->config); g_clear_object(&f->mailer);
	venture_test_remove_tree(f->directory);
	g_free(f->directory);
}

/* --- A fake reachability checker ------------------------------------------ */

typedef struct { GObject parent_instance; GPtrArray *asked; } FakeChecker;
typedef struct { GObjectClass parent_class; } FakeCheckerClass;
static GType fake_checker_get_type(void);
static void fake_checker_iface(VentureMailLinkCheckerInterface *iface);
G_DEFINE_TYPE_WITH_CODE(FakeChecker, fake_checker, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_MAIL_LINK_CHECKER, fake_checker_iface))
static gboolean fake_check(VentureMailLinkChecker *checker, const gchar *url, GCancellable *cancellable, GError **error)
{
	FakeChecker *self = (FakeChecker *)checker;
	(void)cancellable;
	g_ptr_array_add(self->asked, g_strdup(url));
	if (strstr(url, "broken") != NULL) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_HOST_NOT_FOUND, "No such host");
		return FALSE;
	}
	return TRUE;
}
static void fake_checker_iface(VentureMailLinkCheckerInterface *iface) { iface->check = fake_check; }
static void fake_checker_finalize(GObject *object)
{
	g_ptr_array_unref(((FakeChecker *)object)->asked);
	G_OBJECT_CLASS(fake_checker_parent_class)->finalize(object);
}
static void fake_checker_class_init(FakeCheckerClass *klass) { G_OBJECT_CLASS(klass)->finalize = fake_checker_finalize; }
static void fake_checker_init(FakeChecker *self) { self->asked = g_ptr_array_new_with_free_func(g_free); }
G_DEFINE_AUTOPTR_CLEANUP_FUNC(FakeChecker, g_object_unref)

/* --- Helpers ----------------------------------------------------------------- */

static VentureMailMessage *message(const gchar *text, const gchar *html)
{
	VentureMailMessage *m = venture_mail_message_new();
	g_object_set(m, "organization-id", (gint64)1, "to", "reader@example.com", "subject", "Hello",
		"text-body", text, "html-body", html, NULL);
	return m;
}
static VentureMailCheckStatus status(VentureMailPreview *preview, const gchar *id)
{
	const VentureMailCheck *check = venture_mail_preview_get_check(preview, id);
	g_assert_nonnull(check);
	g_assert_false(venture_string_is_empty(check->message));
	return check->status;
}
static const gchar good_text[] = "Thank you for your order. Your receipt is attached to this message.";

/* --- The checks ----------------------------------------------------------- */

/* Without a text alternative a text-only reader, a screen reader in plain
 * mode and many spam filters see an empty message. */
static void test_check_plain_text(void)
{
	g_autoptr(VentureMailMessage) none = message("", "<p>Only HTML</p>");
	g_autoptr(VentureMailMessage) tiny = message("Hi", "<p>Only HTML</p>");
	g_autoptr(VentureMailMessage) good = message(good_text, "<p>Thanks</p>");
	g_autoptr(VentureMailPreview) a = venture_mail_preview_render(none, NULL, FALSE, NULL, NULL);
	g_autoptr(VentureMailPreview) b = venture_mail_preview_render(tiny, NULL, FALSE, NULL, NULL);
	g_autoptr(VentureMailPreview) c = venture_mail_preview_render(good, NULL, FALSE, NULL, NULL);
	g_assert_cmpint(status(a, "plain-text"), ==, VENTURE_MAIL_CHECK_FAIL);
	g_assert_cmpint(status(b, "plain-text"), ==, VENTURE_MAIL_CHECK_FAIL);
	g_assert_cmpint(status(c, "plain-text"), ==, VENTURE_MAIL_CHECK_OK);
}

/* Images are off by default in many clients; alt text is what is read. */
static void test_check_image_alt(void)
{
	g_autoptr(VentureMailMessage) missing = message(good_text, "<p><img src=\"https://example.com/logo.png\"></p>");
	g_autoptr(VentureMailMessage) empty = message(good_text, "<p><IMG alt=\"\" src=\"https://example.com/logo.png\"></p>");
	g_autoptr(VentureMailMessage) good = message(good_text, "<p><img src='https://example.com/logo.png' alt='Acme logo'></p>");
	g_autoptr(VentureMailPreview) a = venture_mail_preview_render(missing, NULL, FALSE, NULL, NULL);
	g_autoptr(VentureMailPreview) b = venture_mail_preview_render(empty, NULL, FALSE, NULL, NULL);
	g_autoptr(VentureMailPreview) c = venture_mail_preview_render(good, NULL, FALSE, NULL, NULL);
	g_assert_cmpint(status(a, "image-alt"), ==, VENTURE_MAIL_CHECK_FAIL);
	g_assert_cmpint(status(b, "image-alt"), ==, VENTURE_MAIL_CHECK_FAIL);
	g_assert_cmpint(status(c, "image-alt"), ==, VENTURE_MAIL_CHECK_OK);
}

/* Syntax is always checked; reachability only through a checker. */
static void test_check_links(void)
{
	g_autoptr(VentureMailMessage) bad = message(good_text,
		"<a href=\"javascript:alert(1)\">x</a><a href=\"https://\">y</a><a href=\"ftp://example.com/f\">z</a>");
	g_autoptr(VentureMailMessage) good = message("See https://example.com/news and write to us.",
		"<a href=\"https://example.com/news\">News</a><a href=\"mailto:help@example.com\">Help</a><a href=\"https://broken.example.net/x\">Old</a>");
	g_autoptr(FakeChecker) checker = g_object_new(fake_checker_get_type(), NULL);
	g_autoptr(VentureMailPreview) a = venture_mail_preview_render(bad, NULL, FALSE, NULL, NULL);
	g_autoptr(VentureMailPreview) b = venture_mail_preview_render(good, NULL, FALSE, NULL, NULL);
	g_autoptr(VentureMailPreview) c = NULL;
	const VentureMailCheck *check;
	guint i;
	g_assert_cmpint(status(a, "links"), ==, VENTURE_MAIL_CHECK_FAIL);
	g_assert_cmpint(status(b, "links"), ==, VENTURE_MAIL_CHECK_OK);
	check = venture_mail_preview_get_check(b, "links");
	g_assert_nonnull(strstr(check->message, "not checked"));
	c = venture_mail_preview_render(good, NULL, FALSE, VENTURE_MAIL_LINK_CHECKER(checker), NULL);
	g_assert_cmpint(status(c, "links"), ==, VENTURE_MAIL_CHECK_WARN);
	check = venture_mail_preview_get_check(c, "links");
	g_assert_nonnull(strstr(check->message, "broken.example.net"));
	/* Only http and https are asked about, each once. */
	g_assert_cmpuint(checker->asked->len, ==, 2);
	for (i = 0; i < checker->asked->len; i++)
		g_assert_true(g_str_has_prefix(g_ptr_array_index(checker->asked, i), "https://"));
}

/* Above about 102 KB a major webmail client clips the message. */
static void test_check_size(void)
{
	g_autoptr(GString) big = g_string_new("<p>");
	g_autoptr(VentureMailMessage) small = message(good_text, "<p>Small</p>");
	g_autoptr(VentureMailMessage) large = NULL;
	g_autoptr(VentureMailPreview) a = NULL, b = NULL;
	while (big->len <= VENTURE_MAIL_PREVIEW_CLIP_BYTES) g_string_append(big, "Lorem ipsum dolor sit amet. ");
	g_string_append(big, "</p>");
	large = message(good_text, big->str);
	a = venture_mail_preview_render(small, NULL, FALSE, NULL, NULL);
	b = venture_mail_preview_render(large, NULL, FALSE, NULL, NULL);
	g_assert_cmpint(status(a, "size"), ==, VENTURE_MAIL_CHECK_OK);
	g_assert_cmpint(status(b, "size"), ==, VENTURE_MAIL_CHECK_WARN);
}

/* Bulk mail needs both a body link and the List-Unsubscribe header;
 * transactional mail needs neither. */
static void test_check_unsubscribe(void)
{
	g_autoptr(VentureMailMessage) plain = message(good_text, "<p>Hello</p>");
	g_autoptr(VentureMailMessage) campaign = message(good_text, "<p>Hello</p>");
	g_autoptr(VentureMailPreview) a = venture_mail_preview_render(plain, NULL, FALSE, NULL, NULL);
	g_autoptr(VentureMailPreview) b = venture_mail_preview_render(plain, NULL, TRUE, NULL, NULL);
	g_autoptr(VentureMailPreview) c = NULL;
	g_autofree gchar *json = NULL;
	g_autoptr(JsonNode) node = NULL;
	guint i;
	gboolean header = FALSE;
	g_assert_cmpint(status(a, "unsubscribe"), ==, VENTURE_MAIL_CHECK_OK);
	g_assert_cmpint(status(b, "unsubscribe"), ==, VENTURE_MAIL_CHECK_FAIL);
	g_object_set(campaign,
		"private-text-body", "Hello.\n\nUnsubscribe: https://books.example.com/marketing/u/0123456789abcdef",
		"private-html-body", "<p>Hello</p><p><a href=\"https://books.example.com/marketing/u/0123456789abcdef\">Unsubscribe</a></p>",
		"private-unsubscribe-url", "https://books.example.com/marketing/u/0123456789abcdef", NULL);
	c = venture_mail_preview_render(campaign, "campaign", TRUE, NULL, NULL);
	g_assert_cmpint(status(c, "unsubscribe"), ==, VENTURE_MAIL_CHECK_OK);
	for (i = 0; i < c->headers->len; i++)
		header = header || g_str_has_prefix(g_ptr_array_index(c->headers, i), "List-Unsubscribe:");
	g_assert_true(header);
	/* The token is a credential: it is in no field of the preview. */
	node = venture_mail_preview_to_json(c);
	json = json_to_string(node, FALSE);
	g_assert_null(strstr(json, "0123456789abcdef"));
	g_assert_nonnull(strstr(json, VENTURE_MAIL_PREVIEW_PLACEHOLDER_URL));
}

/* The transport's rule, and the redaction applied to what it would send. */
static void test_private_links(void)
{
	g_autoptr(VentureMailMessage) m = message("A private link is included in this message.", "<p>Public</p>");
	g_autoptr(VentureMailMessage) html_twin = message("Public text", "<p>Public</p>");
	g_autoptr(VentureMailPreview) a = NULL, b = NULL;
	g_autofree gchar *text = NULL, *html = NULL;
	g_object_set(m, "private-text-body", "Open https://books.example.com/portal/SECRETTOKEN and keep it. Help: https://example.com/help", NULL);
	venture_mail_visible_bodies(m, &text, &html);
	g_assert_nonnull(strstr(text, "SECRETTOKEN"));
	g_assert_null(html);
	a = venture_mail_preview_render(m, NULL, FALSE, NULL, NULL);
	g_assert_null(strstr(a->text, "SECRETTOKEN"));
	g_assert_nonnull(strstr(a->text, VENTURE_MAIL_PREVIEW_PLACEHOLDER_URL));
	/* A public link the private body repeats is not a credential. */
	g_object_set(m, "text-body", "Help: https://example.com/help", NULL);
	g_clear_pointer(&a, venture_mail_preview_free);
	a = venture_mail_preview_render(m, NULL, FALSE, NULL, NULL);
	g_assert_nonnull(strstr(a->text, "https://example.com/help"));
	g_assert_null(a->html);
	g_object_set(html_twin, "private-html-body", "<p><a href=\"https://books.example.com/portal/SECRETTOKEN\">Pay</a></p>", NULL);
	g_clear_pointer(&text, g_free);
	venture_mail_visible_bodies(html_twin, &text, &html);
	g_assert_cmpstr(text, ==, "Public text");
	g_assert_nonnull(strstr(html, "SECRETTOKEN"));
	b = venture_mail_preview_render(html_twin, NULL, FALSE, NULL, NULL);
	g_assert_null(strstr(b->html, "SECRETTOKEN"));
	g_assert_nonnull(strstr(b->html, VENTURE_MAIL_PREVIEW_PLACEHOLDER_URL));
}

/* The default checker never reaches a loopback or private address. */
static void test_default_checker_guard(void)
{
	g_autoptr(VentureHttpLinkChecker) checker = venture_http_link_checker_new();
	g_autoptr(GError) error = NULL;
	g_assert_false(venture_mail_link_checker_check(VENTURE_MAIL_LINK_CHECKER(checker), "http://127.0.0.1:1/", NULL, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	g_assert_false(venture_mail_link_checker_check(VENTURE_MAIL_LINK_CHECKER(checker), "ftp://example.com/", NULL, &error));
	g_assert_nonnull(error);
}

/* --- The kinds ------------------------------------------------------------ */

static void test_kinds(void)
{
	const gchar *expected[] = { "invoice", "quote", "reminder", "receipt", "campaign", "sequence",
		"portal-invitation", "account-welcome", NULL };
	g_autoptr(GPtrArray) kinds = venture_mail_preview_kinds();
	guint i;
	for (i = 0; expected[i]; i++) {
		const VentureMailPreviewKind *kind = venture_mail_preview_lookup(expected[i]);
		g_assert_nonnull(kind);
		g_assert_nonnull(kind->build);
		g_assert_false(venture_string_is_empty(kind->label));
	}
	g_assert_true(venture_mail_preview_lookup("campaign")->bulk);
	g_assert_true(venture_mail_preview_lookup("sequence")->bulk);
	g_assert_false(venture_mail_preview_lookup("invoice")->bulk);
	/* Alerts are in-app only: there is no alert email to preview. */
	g_assert_null(venture_mail_preview_lookup("alert"));
	g_assert_null(venture_mail_preview_lookup("message"));
	g_assert_cmpuint(kinds->len, >=, 8);
}

static VentureMailPreview *stored(Fixture *f, VentureEntity *row)
{
	g_autoptr(GError) error = NULL;
	VentureMailPreview *preview = venture_mail_preview_message(f->context, f->org, venture_entity_get_id(row), NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(preview);
	return preview;
}
static VentureMailPreview *built(Fixture *f, const gchar *kind, gint64 id, const gchar *date)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = date ? venture_time_from_string(date, NULL) : NULL;
	VentureMailPreview *preview = venture_mail_preview_build(f->context, kind, f->org, id, now, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(preview);
	return preview;
}
static void assert_same(VentureMailPreview *a, VentureMailPreview *b)
{
	guint i;
	g_assert_cmpstr(a->to, ==, b->to);
	g_assert_cmpstr(a->subject, ==, b->subject);
	g_assert_cmpstr(a->text, ==, b->text);
	g_assert_cmpstr(a->html, ==, b->html);
	g_assert_cmpuint(a->headers->len, ==, b->headers->len);
	for (i = 0; i < a->headers->len; i++)
		g_assert_cmpstr(g_ptr_array_index(a->headers, i), ==, g_ptr_array_index(b->headers, i));
	g_assert_cmpuint(a->attachments->len, ==, b->attachments->len);
	g_assert_cmpuint(a->checks->len, ==, b->checks->len);
}
static void assert_no_mail(Fixture *f)
{
	g_autoptr(GPtrArray) mail = rows(f, "mail_message");
	g_assert_cmpuint(mail->len, ==, 0);
}

/* The invoice the Send button would queue, before it is pressed. */
static void test_invoice(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureMailPreview) before = NULL, after = NULL;
	g_autoptr(VentureMailMessage) queued = NULL;
	g_autoptr(GPtrArray) templates = rows(f, "mail_template");
	g_autoptr(GPtrArray) again = NULL;
	g_autoptr(GError) error = NULL;
	(void)unused;
	before = built(f, "invoice", f->invoice, NULL);
	/* Building wrote nothing: no message, not even the default template. */
	assert_no_mail(f);
	again = rows(f, "mail_template");
	g_assert_cmpuint(again->len, ==, templates->len);
	g_assert_cmpstr(before->to, ==, "accounts@example.com");
	g_assert_nonnull(strstr(before->html, "Bookkeeping"));
	g_assert_cmpuint(before->attachments->len, ==, 1);
	g_assert_cmpint(status(before, "unsubscribe"), ==, VENTURE_MAIL_CHECK_OK);
	queued = venture_mail_send_invoice(f->context, f->org, f->invoice, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(queued);
	after = stored(f, VENTURE_ENTITY(queued));
	assert_same(before, after);
}

static void test_quote(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) q = record(f, "quote"), l = record(f, "quote_line"), action = record(f, "quote_action");
	g_autoptr(VentureEntity) current = NULL;
	g_autoptr(VentureMailPreview) before = NULL, after = NULL;
	g_autoptr(GPtrArray) mail = NULL;
	g_autofree gchar *token = NULL;
	(void)unused;
	g_object_set(q, "number", "Q-PRE", "company-id", f->company, "currency", "USD", NULL);
	save(f, q);
	g_object_set(l, "quote-id", venture_entity_get_id(q), "description", "Setup", NULL);
	field(l, "quantity", "2"); field(l, "unit-price", "15 USD");
	save(f, l);
	before = built(f, "quote", venture_entity_get_id(q), NULL);
	assert_no_mail(f);
	g_assert_null(before->html);
	g_assert_nonnull(strstr(before->text, VENTURE_MAIL_PREVIEW_PLACEHOLDER_URL));
	current = venture_database_get(f->db, VENTURE_TYPE_QUOTE, venture_entity_get_id(q), NULL);
	g_object_set(action, "quote-id", venture_entity_get_id(q), "action", "send",
		"expected-version", venture_entity_get_version(current), NULL);
	save(f, action);
	mail = rows(f, "mail_message");
	g_assert_cmpuint(mail->len, ==, 1);
	g_clear_object(&current);
	current = venture_database_get(f->db, VENTURE_TYPE_QUOTE, venture_entity_get_id(q), NULL);
	g_object_get(current, "acceptance-token", &token, NULL);
	g_assert_false(venture_string_is_empty(token));
	after = stored(f, g_ptr_array_index(mail, 0));
	g_assert_null(strstr(after->text, token));
	assert_same(before, after);
}

static void test_reminder(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) access = NULL;
	g_autoptr(VentureMailPreview) before = NULL, after = NULL;
	g_autoptr(GPtrArray) mail = NULL;
	g_autoptr(GDateTime) at = venture_time_from_string("2026-01-07", NULL);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *token = NULL;
	(void)unused;
	access = venture_portal_service_invite(venture_portal_service_get(f->db), f->org, f->company, "alice@example.com", NULL, &error);
	g_assert_no_error(error);
	g_object_get(access, "token", &token, NULL);
	before = built(f, "reminder", f->invoice, "2026-01-07");
	assert_no_mail(f);
	g_assert_cmpstr(before->to, ==, "alice@example.com");
	g_assert_null(strstr(before->text, token));
	g_assert_null(strstr(before->html, token));
	g_assert_nonnull(strstr(before->html, VENTURE_MAIL_PREVIEW_PLACEHOLDER_URL));
	g_assert_cmpint(venture_dunning_service_sweep(venture_dunning_service_get(f->db), f->org, at, 100, NULL, &error), >=, 0);
	g_assert_no_error(error);
	mail = rows(f, "mail_message");
	g_assert_cmpuint(mail->len, ==, 1);
	after = stored(f, g_ptr_array_index(mail, 0));
	assert_same(before, after);
}

static void test_campaign(Fixture *f, gconstpointer unused)
{
	VentureMarketingService *service = venture_marketing_service_get(f->db);
	g_autoptr(VentureEntity) list = record(f, "marketing_list"), member = record(f, "marketing_member");
	g_autoptr(VentureEntity) send = record(f, "marketing_send");
	g_autoptr(VentureMarketingConsent) consent = NULL;
	g_autoptr(VentureMarketingSend) previewed = NULL;
	g_autoptr(VentureEntity) contact = NULL;
	g_autoptr(GPtrArray) recipients = NULL, mail = NULL;
	g_autoptr(VentureMailPreview) before = NULL, after = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *hash = NULL;
	(void)unused;
	contact = venture_database_get(f->db, VENTURE_TYPE_CONTACT, f->contact, NULL);
	consent = venture_marketing_service_consent(service, f->org, contact, "Signed preference form",
		"I request this organization's marketing email", "preview-permission", now, NULL, &error);
	g_assert_no_error(error);
	g_object_set(list, "name", "Readers", NULL); save(f, list);
	g_object_set(member, "list-id", venture_entity_get_id(list), "contact-id", f->contact, NULL); save(f, member);
	g_object_set(send, "name", "Autumn", "list-id", venture_entity_get_id(list), "subject", "Hello {name}",
		"text-body", "Our autumn news is out: https://example.com/news",
		"html-body", "<p>Hello {name}</p><p><img src=\"https://example.com/leaf.png\" alt=\"A leaf\"></p><a href=\"https://example.com/news\">News</a>",
		"interval-seconds", (gint64)1, NULL);
	save(f, send);
	previewed = venture_marketing_service_preview(service, f->org, venture_entity_get_id(send), "https://books.example.com", NULL, &error);
	g_assert_no_error(error);
	recipients = rows(f, "marketing_recipient");
	g_assert_cmpuint(recipients->len, ==, 1);
	before = built(f, "campaign", venture_entity_get_id(g_ptr_array_index(recipients, 0)), NULL);
	assert_no_mail(f);
	g_assert_cmpint(status(before, "unsubscribe"), ==, VENTURE_MAIL_CHECK_OK);
	g_assert_cmpint(status(before, "image-alt"), ==, VENTURE_MAIL_CHECK_OK);
	g_object_get(g_ptr_array_index(recipients, 0), "unsubscribe-hash", &hash, NULL);
	g_assert_true(venture_marketing_service_transition(service, f->org, venture_entity_get_id(send), "approve", NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(venture_marketing_service_run(service, f->org, venture_entity_get_id(send), 100, now, NULL, &error), ==, 1);
	g_assert_no_error(error);
	mail = rows(f, "mail_message");
	g_assert_cmpuint(mail->len, ==, 1);
	after = stored(f, g_ptr_array_index(mail, 0));
	g_assert_true(after->bulk);
	assert_same(before, after);
}

/* An unknown kind and another organization's record are refused alike. */
static void test_refusals(Fixture *f, gconstpointer unused)
{
	g_autoptr(GError) error = NULL;
	(void)unused;
	g_assert_null(venture_mail_preview_build(f->context, "alert", f->org, 1, NULL, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_null(venture_mail_preview_build(f->context, "invoice", f->org + 1000, f->invoice, NULL, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_null(venture_mail_preview_message(f->context, f->org, 999999, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
}

/* A test send goes to the acting user only, through the outbox, with the
 * bearer links withheld, and once per click. */
static void test_send_test(Fixture *f, gconstpointer unused)
{
	g_autoptr(VentureEntity) user = record(f, "user"), access = NULL;
	g_autoptr(VentureMailMessage) first = NULL, repeat = NULL, other = NULL, refused = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *to = NULL, *subject = NULL, *text = NULL, *html = NULL, *private_text = NULL, *private_html = NULL, *token = NULL;
	g_autofree gchar *cc = NULL, *bcc = NULL;
	const GPtrArray *sent;
	VentureActor actor;
	actor.kind = VENTURE_ACTOR_KIND_USER;
	actor.name = "previewer";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	(void)unused;
	refused = venture_mail_preview_send_test(f->context, "invoice", f->org, f->invoice,
		"6f1c1f3e-8a51-4d55-9a0e-6a54bfbf0a01", &actor, &error);
	g_assert_null(refused);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	/* The address is set after creation: a new user with an email is sent
	 * a welcome message, which would be one more row in the outbox. */
	g_object_set(user, "username", "previewer", "password-hash", "x", NULL);
	save(f, user);
	g_object_set(user, "email", "previewer@example.net", NULL);
	save(f, user);
	access = venture_portal_service_invite(venture_portal_service_get(f->db), f->org, f->company, "alice@example.com", NULL, &error);
	g_assert_no_error(error);
	g_object_get(access, "token", &token, NULL);
	first = venture_mail_preview_send_test(f->context, "reminder", f->org, f->invoice,
		"6f1c1f3e-8a51-4d55-9a0e-6a54bfbf0a01", &actor, &error);
	g_assert_no_error(error);
	g_assert_nonnull(first);
	g_object_get(first, "to", &to, "cc", &cc, "bcc", &bcc, "subject", &subject, "text-body", &text, "html-body", &html,
		"private-text-body", &private_text, "private-html-body", &private_html, NULL);
	g_assert_cmpstr(to, ==, "previewer@example.net");
	g_assert_true(venture_string_is_empty(cc));
	g_assert_true(venture_string_is_empty(bcc));
	g_assert_true(g_str_has_prefix(subject, "[TEST] "));
	g_assert_true(venture_string_is_empty(private_text));
	g_assert_true(venture_string_is_empty(private_html));
	g_assert_null(strstr(text, token));
	g_assert_null(strstr(html, token));
	g_assert_nonnull(strstr(html, VENTURE_MAIL_PREVIEW_PLACEHOLDER_URL));
	repeat = venture_mail_preview_send_test(f->context, "reminder", f->org, f->invoice,
		"6f1c1f3e-8a51-4d55-9a0e-6a54bfbf0a01", &actor, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(repeat)), ==, venture_entity_get_id(VENTURE_ENTITY(first)));
	other = venture_mail_preview_send_test(f->context, "reminder", f->org, f->invoice,
		"0b6e3c7a-2f1d-4a8e-b3c4-9d5e6f7a8b9c", &actor, &error);
	g_assert_no_error(error);
	g_assert_cmpint(venture_entity_get_id(VENTURE_ENTITY(other)), !=, venture_entity_get_id(VENTURE_ENTITY(first)));
	g_assert_null(venture_mail_preview_send_test(f->context, "reminder", f->org, f->invoice, "not-a-uuid", &actor, &error));
	g_assert_nonnull(error);
	g_clear_error(&error);
	/* The real outbox delivers it, to the actor and nobody else. */
	g_assert_cmpint(venture_mail_outbox_deliver_due(venture_database_get_mail_outbox(f->db), f->org, 10, NULL, NULL, &error), ==, 2);
	g_assert_no_error(error);
	sent = venture_log_mailer_get_messages(f->mailer);
	g_assert_cmpuint(sent->len, ==, 2);
	{
		guint i;
		for (i = 0; i < sent->len; i++) {
			g_autofree gchar *recipient = NULL;
			g_object_get(g_ptr_array_index(sent, i), "to", &recipient, NULL);
			g_assert_cmpstr(recipient, ==, "previewer@example.net");
		}
	}
	/* And it decided nothing: no reminder step was recorded. */
	{
		g_autoptr(GPtrArray) events = rows(f, "dunning_event");
		g_assert_cmpuint(events->len, ==, 0);
	}
}

/* --- The page, with real sign-in ------------------------------------------ */

typedef struct {
	Fixture base;
	VentureWebServer *server;
	SoupSession *session;
	guint port;
} ServerFixture;
typedef struct { gboolean done; GBytes *body; GError *error; } RequestResult;
static void request_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
	RequestResult *outcome = user_data;
	outcome->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &outcome->error);
	outcome->done = TRUE;
}
static guint http(ServerFixture *s, const gchar *method, const gchar *path, const gchar *cookie,
	const gchar *form, gchar **out_body, gchar **out_cookie)
{
	g_autoptr(SoupMessage) message = NULL;
	g_autofree gchar *url = g_strdup_printf("http://127.0.0.1:%u%s", s->port, path);
	RequestResult outcome = { FALSE, NULL, NULL };
	message = soup_message_new(method, url);
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	if (cookie != NULL)
		soup_message_headers_append(soup_message_get_request_headers(message), "Cookie", cookie);
	if (form != NULL) {
		g_autoptr(GBytes) bytes = g_bytes_new(form, strlen(form));
		soup_message_set_request_body_from_bytes(message, "application/x-www-form-urlencoded", bytes);
	}
	soup_session_send_and_read_async(s->session, message, G_PRIORITY_DEFAULT, NULL, request_done, &outcome);
	while (!outcome.done)
		g_main_context_iteration(NULL, TRUE);
	if (outcome.error != NULL)
		g_error("%s %s: %s", method, path, outcome.error->message);
	if (out_body != NULL)
		*out_body = g_strndup(g_bytes_get_data(outcome.body, NULL), g_bytes_get_size(outcome.body));
	if (out_cookie != NULL) {
		const gchar *set = soup_message_headers_get_one(soup_message_get_response_headers(message), "Set-Cookie");
		*out_cookie = set ? g_strndup(set, strcspn(set, ";")) : NULL;
	}
	g_clear_pointer(&outcome.body, g_bytes_unref);
	return soup_message_get_status(message);
}
static gchar *login(ServerFixture *s, const gchar *username, const gchar *password)
{
	g_autofree gchar *form = g_strdup_printf("username=%s&password=%s", username, password);
	gchar *cookie = NULL;
	g_assert_cmpuint(http(s, "POST", "/login", NULL, form, NULL, &cookie), ==, SOUP_STATUS_FOUND);
	g_assert_nonnull(cookie);
	return cookie;
}
static void server_setup(ServerFixture *s, gconstpointer unused)
{
	Fixture *f = &s->base;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureUser) owner = venture_user_new();
	g_autoptr(VentureUser) editor = venture_user_new();
	g_autoptr(VentureEntity) member = NULL;
	gboolean ok;
	setup(f, unused);
	g_setenv("VENTURE_TEST_SESSION_SECRET", "mail-preview-test-secret", TRUE);
	g_object_set(f->config, "server-bind-address", "127.0.0.1", "server-port", (gint64)0,
		"security-session-secret-env", "VENTURE_TEST_SESSION_SECRET", "security-password-iterations", (gint64)100000, NULL);
	g_object_set(owner, "username", "owner", "role", VENTURE_USER_ROLE_OWNER, "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(owner, "owner-password-1", 100000, NULL));
	ok = venture_orgaccess_bootstrap_owner(f->db, owner, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
	/* Addresses after creation, so neither user is sent a welcome. */
	g_object_set(owner, "email", "owner@example.net", NULL);
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(owner), NULL, &error));
	g_assert_no_error(error);
	g_object_set(editor, "username", "editor", "role", VENTURE_USER_ROLE_EDITOR, "active", TRUE, NULL);
	g_assert_true(venture_user_set_password(editor, "editor-password-1", 100000, NULL));
	g_assert_true(venture_database_save(f->db, VENTURE_ENTITY(editor), NULL, &error));
	g_assert_no_error(error);
	member = g_object_new(VENTURE_TYPE_ORGANIZATION_MEMBERSHIP, "user-id", venture_entity_get_id(VENTURE_ENTITY(editor)),
		"organization-id", f->org, "role", VENTURE_ORGANIZATION_ROLE_FINANCE, "active", TRUE, NULL);
	g_assert_true(venture_database_save(f->db, member, NULL, &error));
	g_assert_no_error(error);
	s->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(s->server, &error));
	g_assert_no_error(error);
	s->port = venture_web_server_get_port(s->server);
	s->session = soup_session_new();
}
static void server_teardown(ServerFixture *s, gconstpointer unused)
{
	venture_web_server_stop(s->server);
	g_clear_object(&s->session);
	g_clear_object(&s->server);
	teardown(&s->base, unused);
}
static guint count(const gchar *haystack, const gchar *needle)
{
	guint n = 0;
	const gchar *p = haystack;
	while ((p = strstr(p, needle)) != NULL) { n++; p += strlen(needle); }
	return n;
}

/* Owner or admin only; three widths in two schemes; email HTML sandboxed. */
static void test_page(ServerFixture *s, gconstpointer unused)
{
	g_autofree gchar *owner = NULL, *editor = NULL, *page = NULL, *path = NULL, *api = NULL, *json = NULL;
	(void)unused;
	path = g_strdup_printf("/mail/preview/invoice/%" G_GINT64_FORMAT, s->base.invoice);
	api = g_strdup_printf("/api/v1/mail/preview/invoice/%" G_GINT64_FORMAT, s->base.invoice);
	g_assert_cmpuint(http(s, "GET", path, NULL, NULL, NULL, NULL), ==, SOUP_STATUS_FOUND);
	g_assert_cmpuint(http(s, "GET", api, NULL, NULL, NULL, NULL), ==, SOUP_STATUS_UNAUTHORIZED);
	editor = login(s, "editor", "editor-password-1");
	g_assert_cmpuint(http(s, "GET", path, editor, NULL, NULL, NULL), ==, SOUP_STATUS_FORBIDDEN);
	g_assert_cmpuint(http(s, "GET", api, editor, NULL, NULL, NULL), ==, SOUP_STATUS_FORBIDDEN);
	owner = login(s, "owner", "owner-password-1");
	g_assert_cmpuint(http(s, "GET", path, owner, NULL, &page, NULL), ==, SOUP_STATUS_OK);
	g_assert_cmpuint(count(page, "<iframe"), ==, 6);
	g_assert_cmpuint(count(page, "data-width=\"390\""), ==, 2);
	g_assert_cmpuint(count(page, "data-width=\"768\""), ==, 2);
	g_assert_cmpuint(count(page, "data-width=\"1440\""), ==, 2);
	g_assert_cmpuint(count(page, "data-scheme=\"light\""), ==, 3);
	g_assert_cmpuint(count(page, "data-scheme=\"dark\""), ==, 3);
	g_assert_cmpuint(count(page, "sandbox=\"\""), ==, 6);
	g_assert_cmpuint(count(page, "srcdoc=\""), ==, 6);
	g_assert_null(strstr(page, "allow-scripts"));
	g_assert_null(strstr(page, "allow-same-origin"));
	g_assert_nonnull(strstr(page, "color-scheme: dark"));
	g_assert_nonnull(strstr(page, "Plain-text alternative"));
	g_assert_nonnull(strstr(page, "plain-text"));
	g_assert_nonnull(strstr(page, "image-alt"));
	g_assert_nonnull(strstr(page, "unsubscribe"));
	g_assert_nonnull(strstr(page, "Bookkeeping"));
	g_assert_cmpuint(http(s, "GET", api, owner, NULL, &json, NULL), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(json, "\"checks\""));
	g_assert_nonnull(strstr(json, "\"plain-text\""));
	g_clear_pointer(&page, g_free);
	g_assert_cmpuint(http(s, "GET", "/mail/preview/alert/1", owner, NULL, &page, NULL), ==, SOUP_STATUS_NOT_FOUND);
}

/* The button: only to the person pressing it, only through the outbox. */
static void test_page_send_test(ServerFixture *s, gconstpointer unused)
{
	g_autofree gchar *owner = NULL, *editor = NULL, *page = NULL, *path = NULL, *message_path = NULL;
	g_autoptr(GPtrArray) mail = NULL;
	const GPtrArray *sent;
	g_autofree gchar *to = NULL, *subject = NULL;
	(void)unused;
	path = g_strdup_printf("/mail/preview/invoice/%" G_GINT64_FORMAT "/test", s->base.invoice);
	g_assert_cmpuint(http(s, "POST", path, NULL, "token=6f1c1f3e-8a51-4d55-9a0e-6a54bfbf0a01", NULL, NULL), ==, SOUP_STATUS_FOUND);
	editor = login(s, "editor", "editor-password-1");
	g_assert_cmpuint(http(s, "POST", path, editor, "token=6f1c1f3e-8a51-4d55-9a0e-6a54bfbf0a01", NULL, NULL), ==, SOUP_STATUS_FORBIDDEN);
	mail = rows(&s->base, "mail_message");
	g_assert_cmpuint(mail->len, ==, 0);
	g_clear_pointer(&mail, g_ptr_array_unref);
	owner = login(s, "owner", "owner-password-1");
	g_assert_cmpuint(http(s, "POST", path, owner, "token=6f1c1f3e-8a51-4d55-9a0e-6a54bfbf0a01", &page, NULL), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(page, "owner@example.net"));
	g_clear_pointer(&page, g_free);
	/* The same click token twice is one message. */
	g_assert_cmpuint(http(s, "POST", path, owner, "token=6f1c1f3e-8a51-4d55-9a0e-6a54bfbf0a01", &page, NULL), ==, SOUP_STATUS_OK);
	mail = rows(&s->base, "mail_message");
	g_assert_cmpuint(mail->len, ==, 1);
	sent = venture_log_mailer_get_messages(s->base.mailer);
	g_assert_cmpuint(sent->len, ==, 1);
	g_object_get(g_ptr_array_index(sent, 0), "to", &to, "subject", &subject, NULL);
	g_assert_cmpstr(to, ==, "owner@example.net");
	g_assert_cmpstr(subject, ==, "[TEST] Invoice INV-PRE");
	/* A stored row previews by id too. */
	message_path = g_strdup_printf("/mail/preview/message/%" G_GINT64_FORMAT, venture_entity_get_id(g_ptr_array_index(mail, 0)));
	g_clear_pointer(&page, g_free);
	g_assert_cmpuint(http(s, "GET", message_path, owner, NULL, &page, NULL), ==, SOUP_STATUS_OK);
	g_assert_nonnull(strstr(page, "[TEST] Invoice INV-PRE"));
	g_assert_cmpuint(count(page, "<iframe"), ==, 6);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/mail-preview/check/plain-text", test_check_plain_text);
	g_test_add_func("/mail-preview/check/image-alt", test_check_image_alt);
	g_test_add_func("/mail-preview/check/links", test_check_links);
	g_test_add_func("/mail-preview/check/size", test_check_size);
	g_test_add_func("/mail-preview/check/unsubscribe", test_check_unsubscribe);
	g_test_add_func("/mail-preview/private-links", test_private_links);
	g_test_add_func("/mail-preview/default-checker-guard", test_default_checker_guard);
	g_test_add_func("/mail-preview/kinds", test_kinds);
	g_test_add("/mail-preview/invoice", Fixture, NULL, setup, test_invoice, teardown);
	g_test_add("/mail-preview/quote", Fixture, NULL, setup, test_quote, teardown);
	g_test_add("/mail-preview/reminder", Fixture, NULL, setup, test_reminder, teardown);
	g_test_add("/mail-preview/campaign", Fixture, NULL, setup, test_campaign, teardown);
	g_test_add("/mail-preview/refusals", Fixture, NULL, setup, test_refusals, teardown);
	g_test_add("/mail-preview/send-test", Fixture, NULL, setup, test_send_test, teardown);
	g_test_add("/mail-preview/page", ServerFixture, NULL, server_setup, test_page, server_teardown);
	g_test_add("/mail-preview/page-send-test", ServerFixture, NULL, server_setup, test_page_send_test, server_teardown);
	return g_test_run();
}
