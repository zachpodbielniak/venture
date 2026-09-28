/*
 * test-forms.c - The forms module: records, the renderer, the public door
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * A form is the one place VENTURE takes writes from strangers with no
 * session at all, so most of these tests are about what the public door
 * refuses, and what it must never say. The rest pin the embed contract:
 * class names a customer's stylesheet was written against cannot move.
 */

#include <venture.h>
#include <string.h>
#include <libsoup/soup.h>
#include "venture-test-util.h"
#include <libxml/HTMLparser.h>
#include <libxml/tree.h>

typedef struct {
	VentureDatabase *db;
	VentureConfig *config;
	VentureContext *context;
	gint64 org;
	VentureWebServer *server;
	SoupSession *session;
	gchar *state_dir;
	guint16 port;
	gboolean open;
} Fixture;

static void
setup(Fixture *f, gconstpointer data)
{
	g_autoptr(GError) error = NULL;
	(void)data;
	f->config = venture_config_new();
	f->db = venture_database_new("sqlite://:memory:", &error);
	g_assert_no_error(error);
	g_assert_true(venture_database_migrate(f->db, venture_entity_registry_get_default(), &error));
	g_assert_no_error(error);
	f->context = venture_context_new(f->config, f->db);
	f->org = venture_context_get_default_organization_id(f->context);
}

static void
teardown(Fixture *f, gconstpointer data)
{
	(void)data;
	if (f->server != NULL) venture_web_server_stop(f->server);
	g_clear_object(&f->server);
	g_clear_object(&f->session);
	if (f->state_dir != NULL) { venture_test_remove_tree(f->state_dir); g_free(f->state_dir); }
	g_clear_object(&f->context);
	g_clear_object(&f->db);
	g_clear_object(&f->config);
}

static void
save(Fixture *f, gpointer entity)
{
	g_autoptr(GError) error = NULL;
	gboolean ok = venture_database_save(f->db, VENTURE_ENTITY(entity), NULL, &error);
	g_assert_no_error(error);
	g_assert_true(ok);
}

static void
refuse(Fixture *f, gpointer entity, const gchar *needle)
{
	g_autoptr(GError) error = NULL;
	g_assert_false(venture_database_save(f->db, VENTURE_ENTITY(entity), NULL, &error));
	g_assert_nonnull(error);
	if (needle != NULL && strstr(error->message, needle) == NULL)
		g_error("expected \"%s\" in \"%s\"", needle, error->message);
}

static VentureEntity *
reread(Fixture *f, VentureEntity *entity)
{
	return venture_database_get(f->db, G_OBJECT_TYPE(entity), venture_entity_get_id(entity), NULL);
}

static gint64
count(Fixture *f, GType type)
{
	g_autoptr(VentureQuery) q = venture_query_new(type);
	venture_query_set_organization(q, f->org);
	return venture_database_count(f->db, q, NULL);
}

/* A live form with a token the test can type. */
static VentureEntity *
make_form(Fixture *f, const gchar *token, VentureFormState state)
{
	VentureEntity *form = VENTURE_ENTITY(venture_form_new());
	venture_entity_set_organization_id(form, f->org);
	g_object_set(form, "name", "Internal: website contact", "title", "Contact us",
		"description", "We answer within a day.", "public-token", token, "state", state,
		"notes", "INTERNAL-NOTE-DO-NOT-LEAK", NULL);
	save(f, form);
	return form;
}

static VentureEntity *
make_field(Fixture *f, VentureEntity *form, const gchar *key, const gchar *label,
	VentureFormFieldKind kind, gboolean required, gint64 position)
{
	VentureEntity *field = VENTURE_ENTITY(venture_form_field_new());
	venture_entity_set_organization_id(field, f->org);
	g_object_set(field, "form-id", venture_entity_get_id(form), "key", key, "label", label,
		"kind", kind, "required", required, "position", position, NULL);
	return field;
}

static void
add_field(Fixture *f, VentureEntity *form, const gchar *key, const gchar *label,
	VentureFormFieldKind kind, gboolean required, gint64 position)
{
	g_autoptr(VentureEntity) field = make_field(f, form, key, label, kind, required, position);
	save(f, field);
}

/* Freezes the form's questions as its next version. */
static VentureEntity *
publish(Fixture *f, VentureEntity *form)
{
	g_autoptr(GError) error = NULL;
	VentureEntity *version = venture_forms_publish(f->db, form, NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(version);
	/* The form object the test holds is a version behind the publish. */
	{
		g_autoptr(VentureEntity) fresh = reread(f, form);
		gint64 published = 0, number = 0;
		g_object_get(fresh, "published-version-id", &published, "published-number", &number, NULL);
		g_object_set(form, "published-version-id", published, "published-number", number,
			"version", venture_entity_get_version(fresh), NULL);
	}
	return version;
}

/* The contact form most tests submit to: a name, an email, a message, a
 * choice, a rating and a consent box, in that order. */
static VentureEntity *
contact_form(Fixture *f, const gchar *token)
{
	VentureEntity *form = make_form(f, token, VENTURE_FORM_LIVE);
	g_autoptr(VentureEntity) topic = make_field(f, form, "topic", "Topic", VENTURE_FORM_FIELD_SINGLE_CHOICE, TRUE, 40);
	g_autoptr(VentureEntity) score = make_field(f, form, "score", "How did we do?", VENTURE_FORM_FIELD_RATING, FALSE, 50);
	add_field(f, form, "name", "Your name", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 10);
	add_field(f, form, "email", "Email", VENTURE_FORM_FIELD_EMAIL, TRUE, 20);
	add_field(f, form, "message", "Message", VENTURE_FORM_FIELD_LONG_TEXT, FALSE, 30);
	g_object_set(topic, "choices", "Sales\nSupport\nsomething_else | Something else", NULL);
	save(f, topic);
	g_object_set(score, "min-value", 1.0, "max-value", 5.0, NULL);
	save(f, score);
	add_field(f, form, "consent", "You may contact me", VENTURE_FORM_FIELD_CHECKBOX, FALSE, 60);
	g_object_unref(publish(f, form));
	return form;
}

/* ==========================================================================
 * Records
 * ========================================================================== */

/* Every forms type is registered and belongs to the forms module; a type
 * outside a module is refused at startup. */
static void
test_records(void)
{
	static const gchar *const names[] = { "form", "form_field", "form_submission" };
	g_autoptr(VentureModuleRegistry) modules = venture_module_registry_new();
	guint i;
	venture_module_registry_register_builtins(modules);
	for (i = 0; i < G_N_ELEMENTS(names); i++)
	{
		VentureModule *module = venture_module_registry_get_module_for_type(modules, names[i]);
		g_assert_cmpuint(venture_entity_registry_lookup(venture_entity_registry_get_default(), names[i]), !=, G_TYPE_INVALID);
		g_assert_nonnull(module);
		g_assert_cmpstr(venture_module_get_name(module), ==, "forms");
	}
}

/* A form gets a capability token and a private ticket key on its first
 * save; a token already used by another form is refused, because a
 * shared capability would select someone else's form. */
static void
test_form_defaults(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = VENTURE_ENTITY(venture_form_new());
	g_autoptr(VentureEntity) other = VENTURE_ENTITY(venture_form_new());
	g_autoptr(VentureEntity) stored = NULL;
	g_autofree gchar *token = NULL, *key = NULL, *slug = NULL;
	(void)data;
	venture_entity_set_organization_id(form, f->org);
	g_object_set(form, "name", "Newsletter signup", NULL);
	save(f, form);
	stored = reread(f, form);
	g_object_get(stored, "public-token", &token, "ticket-key", &key, "slug", &slug, NULL);
	g_assert_cmpuint(strlen(token), >=, 32);
	g_assert_cmpuint(strlen(key), >=, 32);
	g_assert_cmpstr(slug, ==, "newsletter-signup");
	venture_entity_set_organization_id(other, f->org);
	g_object_set(other, "name", "Copy", "public-token", token, NULL);
	refuse(f, other, "token");
}

/* What a site is allowed to be told to do after a submission, and which
 * sites may embed the form, are both checked when they are written. */
static void
test_form_validation(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "validation-form", VENTURE_FORM_DRAFT);
	(void)data;
	g_object_set(form, "redirect-url", "javascript:alert(1)", NULL);
	refuse(f, form, "Redirect");
	g_object_set(form, "redirect-url", "https://example.com/thanks", "allowed-origins", "https://example.com/path", NULL);
	refuse(f, form, "origin");
	g_object_set(form, "allowed-origins", "https://example.com\nhttp://localhost:8080", NULL);
	save(f, form);
	g_object_set(form, "response-limit", (gint64)-1, NULL);
	refuse(f, form, "Response limit");
	g_object_set(form, "privacy-url", "ftp://example.com/p", NULL);
	refuse(f, form, "Privacy");
	g_object_set(form, "privacy-url", "https://example.com/privacy", "retention-action", "shred", NULL);
	refuse(f, form, "anonymise");
	g_object_set(form, "retention-action", "purge", "response-limit", (gint64)0, NULL);
	save(f, form);
	g_object_set(form, "response-limit", (gint64)0, "min-fill-seconds", (gint64)-2, NULL);
	refuse(f, form, "fill");
	g_object_set(form, "min-fill-seconds", (gint64)0, "on-duplicate", "sometimes", NULL);
	refuse(f, form, "duplicate");
}

/* A field's key is its stable identity: stored answers point at it, so it
 * is validated, unique within its form and never changed. */
static void
test_field_keys(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "keys-form", VENTURE_FORM_DRAFT);
	g_autoptr(VentureEntity) bad = make_field(f, form, "Bad Key", "Bad", VENTURE_FORM_FIELD_SHORT_TEXT, FALSE, 1);
	g_autoptr(VentureEntity) reserved = make_field(f, form, "_vf_hp", "Reserved", VENTURE_FORM_FIELD_SHORT_TEXT, FALSE, 1);
	g_autoptr(VentureEntity) first = make_field(f, form, "email", "Email", VENTURE_FORM_FIELD_EMAIL, FALSE, 1);
	g_autoptr(VentureEntity) twin = make_field(f, form, "email", "Email again", VENTURE_FORM_FIELD_EMAIL, FALSE, 2);
	(void)data;
	refuse(f, bad, "Key");
	refuse(f, reserved, "Key");
	save(f, first);
	refuse(f, twin, "already");
	g_object_set(first, "key", "mail", NULL);
	refuse(f, first, "cannot be changed");
}

/* Choices are written as lines; each gets a stable id the first time it
 * is saved, and relabelling keeps the id, so old answers still read. */
static void
test_field_choices(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "choices-form", VENTURE_FORM_DRAFT);
	g_autoptr(VentureEntity) field = make_field(f, form, "colour", "Colour", VENTURE_FORM_FIELD_SINGLE_CHOICE, FALSE, 1);
	g_autoptr(VentureEntity) stored = NULL;
	g_autoptr(VentureEntity) empty = make_field(f, form, "size", "Size", VENTURE_FORM_FIELD_MULTIPLE_CHOICE, FALSE, 2);
	g_autofree gchar *choices = NULL;
	(void)data;
	g_object_set(field, "choices", "Red\nDark blue\n\nRed", NULL);
	save(f, field);
	stored = reread(f, field);
	g_object_get(stored, "choices", &choices, NULL);
	g_assert_cmpstr(choices, ==, "red | Red\ndark_blue | Dark blue\nred_2 | Red");
	g_clear_pointer(&choices, g_free);
	g_object_set(stored, "choices", "red | Crimson\ndark_blue | Dark blue\nred_2 | Red", NULL);
	save(f, stored);
	g_object_get(stored, "choices", &choices, NULL);
	g_assert_cmpstr(choices, ==, "red | Crimson\ndark_blue | Dark blue\nred_2 | Red");
	refuse(f, empty, "choice");
}

/* Per-kind settings that cannot mean anything are refused at the save
 * rather than discovered by the first person to fill the form in. */
static void
test_field_settings(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "settings-form", VENTURE_FORM_DRAFT);
	g_autoptr(VentureEntity) pattern = make_field(f, form, "code", "Code", VENTURE_FORM_FIELD_SHORT_TEXT, FALSE, 1);
	g_autoptr(VentureEntity) range = make_field(f, form, "age", "Age", VENTURE_FORM_FIELD_NUMBER, FALSE, 2);
	g_autoptr(VentureEntity) mapped = make_field(f, form, "who", "Who", VENTURE_FORM_FIELD_SHORT_TEXT, FALSE, 3);
	g_autoptr(VentureEntity) lengths = make_field(f, form, "bio", "Bio", VENTURE_FORM_FIELD_LONG_TEXT, FALSE, 4);
	(void)data;
	g_object_set(pattern, "pattern", "([a-z", NULL);
	refuse(f, pattern, "Pattern");
	g_object_set(range, "min-value", 10.0, "max-value", 1.0, NULL);
	refuse(f, range, "Maximum");
	g_object_set(mapped, "maps-to", "owner", NULL);
	refuse(f, mapped, "Maps to");
	g_object_set(lengths, "min-length", (gint64)10, "max-length", (gint64)5, NULL);
	refuse(f, lengths, "length");
}

/* A response is evidence of what a stranger sent. Nobody writes one by
 * hand, and the answers cannot be edited afterwards. */
static void
test_submission_guarded(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "guarded-form");
	g_autoptr(VentureEntity) forged = VENTURE_ENTITY(venture_form_submission_new());
	g_autoptr(GHashTable) answers = venture_forms_answers_new();
	g_autoptr(VentureEntity) submission = NULL;
	g_autoptr(JsonObject) errors = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	VentureFormsOutcome outcome;
	(void)data;
	venture_entity_set_organization_id(forged, f->org);
	g_object_set(forged, "form-id", venture_entity_get_id(form), "name", "Forged", NULL);
	refuse(f, forged, "public address");
	venture_forms_answers_add(answers, "name", "Alice");
	venture_forms_answers_add(answers, "email", "alice@example.com");
	venture_forms_answers_add(answers, "topic", "sales");
	g_assert_true(venture_forms_submit(f->db, form, answers, NULL, now, &outcome, &submission, &errors, &error));
	g_assert_no_error(error);
	g_assert_cmpint(outcome, ==, VENTURE_FORMS_ACCEPTED);
	g_object_set(submission, "answers", "{\"name\":\"Mallory\"}", NULL);
	refuse(f, submission, "cannot be changed");
	g_clear_object(&submission);
}

/* ==========================================================================
 * The renderer: one function, one contract
 * ========================================================================== */

static gchar *
render(Fixture *f, VentureEntity *form, VentureFormsRenderMode mode)
{
	VentureFormsRender options;
	g_autoptr(GError) error = NULL;
	gchar *html;
	options.mode = mode;
	options.action = "/pub/form/contract-form";
	options.ticket = NULL;
	options.values = NULL;
	options.errors = NULL;
	options.version = NULL;
	html = venture_forms_render(f->db, form, &options, &error);
	g_assert_no_error(error);
	g_assert_nonnull(html);
	return html;
}

/* The class names below are a public contract: a customer's stylesheet was
 * written against them. Renaming one breaks every site that embeds a form,
 * silently, so this test is the rename's alarm. */
static void
test_class_contract(Fixture *f, gconstpointer data)
{
	static const gchar *const contract[] = {
		"class=\"vf-form\"", "class=\"vf-title\"", "class=\"vf-description\"",
		"vf-field vf-field--short-text", "vf-field vf-field--email",
		"vf-field vf-field--long-text", "vf-field vf-field--single-choice",
		"vf-field vf-field--rating", "vf-field vf-field--checkbox",
		"class=\"vf-label\"", "class=\"vf-input\"", "class=\"vf-required\"",
		"class=\"vf-choices\"", "class=\"vf-choice\"", "class=\"vf-submit\"",
		"class=\"vf-hp\"", "data-vf-form=\"contract-form\"", "data-vf-field=\"email\"",
		"data-vf-kind=\"email\"", "<fieldset", "<legend", "<label class=\"vf-label\" for=\"vf-contract-form-email\"",
		"aria-describedby=\"vf-contract-form-email-error\"", NULL
	};
	g_autoptr(VentureEntity) form = contact_form(f, "contract-form");
	g_autofree gchar *html = NULL;
	guint i;
	(void)data;
	html = render(f, form, VENTURE_FORMS_RENDER_FRAGMENT);
	for (i = 0; contract[i] != NULL; i++)
		if (strstr(html, contract[i]) == NULL)
			g_error("the embed contract lost %s:\n%s", contract[i], html);
	/* Host CSS always applies: nothing inline, nothing of ours. */
	g_assert_null(strstr(html, "style="));
	g_assert_null(strstr(html, "<style"));
	g_assert_null(strstr(html, "<link"));
	g_assert_null(strstr(html, "class=\"btn"));
	g_assert_null(strstr(html, "class=\"field"));
	g_assert_null(strstr(html, "var(--"));
	/* The fields come out in their declared order. */
	g_assert_true(strstr(html, "data-vf-field=\"name\"") < strstr(html, "data-vf-field=\"email\""));
	g_assert_true(strstr(html, "data-vf-field=\"email\"") < strstr(html, "data-vf-field=\"consent\""));
}

/* Nothing internal reaches the public: the form's internal name, its notes,
 * its organization, its mappings, its ticket key, the origins it trusts. */
static void
test_no_internal_config(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "private-form");
	g_autoptr(JsonNode) schema = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *key = NULL, *html = NULL, *json = NULL, *hosted = NULL;
	VentureFormsRenderMode modes[] = { VENTURE_FORMS_RENDER_FRAGMENT, VENTURE_FORMS_RENDER_HOSTED, VENTURE_FORMS_RENDER_SNIPPET };
	guint i;
	(void)data;
	g_object_set(form, "allowed-origins", "https://trusted.example", "create-lead", TRUE,
		"lead-source", "SECRET-SOURCE", NULL);
	save(f, form);
	g_object_get(form, "ticket-key", &key, NULL);
	schema = venture_forms_schema(f->db, form, "/pub/form/private-form", now, &error);
	g_assert_no_error(error);
	json = json_to_string(schema, FALSE);
	for (i = 0; i < G_N_ELEMENTS(modes); i++)
	{
		g_free(html);
		html = render(f, form, modes[i]);
		g_assert_null(strstr(html, "Internal: website contact"));
		g_assert_null(strstr(html, "INTERNAL-NOTE"));
		g_assert_null(strstr(html, key));
		g_assert_null(strstr(html, "trusted.example"));
		g_assert_null(strstr(html, "SECRET-SOURCE"));
		g_assert_null(strstr(html, "organization"));
	}
	g_assert_null(strstr(json, "Internal: website contact"));
	g_assert_null(strstr(json, "INTERNAL-NOTE"));
	g_assert_null(strstr(json, key));
	g_assert_null(strstr(json, "trusted.example"));
	g_assert_null(strstr(json, "SECRET-SOURCE"));
	g_assert_null(strstr(json, "organization"));
	g_assert_null(strstr(json, "maps_to"));
	g_assert_nonnull(strstr(json, "\"key\":\"email\""));
	g_assert_nonnull(strstr(json, "\"id\":\"something_else\""));
	g_assert_nonnull(strstr(json, "\"ticket\":"));
	/* The hosted page is a whole document; the others are fragments. */
	hosted = render(f, form, VENTURE_FORMS_RENDER_HOSTED);
	g_assert_true(g_str_has_prefix(hosted, "<!DOCTYPE html>"));
	g_assert_false(g_str_has_prefix(html, "<!DOCTYPE html>"));
}

/* ==========================================================================
 * Answers and validation, without HTTP
 * ========================================================================== */

/* A form post repeats a name for every box ticked; a parser that keeps
 * only the last one loses every other choice. */
static void
test_urlencoded_repeats(void)
{
	static const gchar body[] = "size=s&size=m&note=a+b%26c&empty=";
	g_autoptr(GError) error = NULL;
	g_autoptr(GHashTable) answers = venture_forms_answers_from_urlencoded(body, strlen(body), &error);
	GPtrArray *size, *note, *empty;
	g_assert_no_error(error);
	size = g_hash_table_lookup(answers, "size");
	note = g_hash_table_lookup(answers, "note");
	empty = g_hash_table_lookup(answers, "empty");
	g_assert_cmpuint(size->len, ==, 2);
	g_assert_cmpstr(g_ptr_array_index(size, 0), ==, "s");
	g_assert_cmpstr(g_ptr_array_index(size, 1), ==, "m");
	g_assert_cmpstr(g_ptr_array_index(note, 0), ==, "a b&c");
	g_assert_cmpstr(g_ptr_array_index(empty, 0), ==, "");
	g_clear_pointer(&answers, g_hash_table_unref);
	answers = venture_forms_answers_from_urlencoded("bad=%ZZ", 7, &error);
	g_assert_null(answers);
	g_assert_nonnull(error);
}

static VentureFormsOutcome
submit_pairs(Fixture *f, VentureEntity *form, const gchar *const *pairs, JsonObject **errors_out)
{
	g_autoptr(GHashTable) answers = venture_forms_answers_new();
	g_autoptr(VentureEntity) submission = NULL;
	g_autoptr(JsonObject) errors = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	VentureFormsOutcome outcome;
	guint i;
	for (i = 0; pairs[i] != NULL; i += 2)
		venture_forms_answers_add(answers, pairs[i], pairs[i + 1]);
	g_assert_true(venture_forms_submit(f->db, form, answers, NULL, now, &outcome, &submission, &errors, &error));
	g_assert_no_error(error);
	if (errors_out != NULL) *errors_out = g_steal_pointer(&errors);
	return outcome;
}

/* Returns the message the server gave @key, or fails if it gave none. */
static void
assert_invalid(Fixture *f, VentureEntity *form, const gchar *const *pairs, const gchar *key)
{
	g_autoptr(JsonObject) errors = NULL;
	g_assert_cmpint(submit_pairs(f, form, pairs, &errors), ==, VENTURE_FORMS_INVALID);
	g_assert_nonnull(errors);
	if (!json_object_has_member(errors, key))
		g_error("expected an error for %s", key);
}

/* The server is the authority on every kind. Each case is one way a
 * browser without our script, or a script that is not ours, can send
 * something the form never offered. */
static void
test_kind_validation(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "kinds-form");
	g_autoptr(VentureEntity) website = make_field(f, form, "website", "Website", VENTURE_FORM_FIELD_URL, FALSE, 70);
	g_autoptr(VentureEntity) when = make_field(f, form, "when", "When", VENTURE_FORM_FIELD_DATE, FALSE, 80);
	g_autoptr(VentureEntity) seats = make_field(f, form, "seats", "Seats", VENTURE_FORM_FIELD_NUMBER, FALSE, 90);
	g_autoptr(VentureEntity) extras = make_field(f, form, "extras", "Extras", VENTURE_FORM_FIELD_MULTIPLE_CHOICE, FALSE, 100);
	g_autoptr(VentureEntity) code = make_field(f, form, "code", "Code", VENTURE_FORM_FIELD_SHORT_TEXT, FALSE, 110);
	g_autoptr(VentureEntity) phone = make_field(f, form, "phone", "Phone", VENTURE_FORM_FIELD_PHONE, FALSE, 120);
	g_autoptr(JsonObject) errors = NULL;
	(void)data;
	save(f, website);
	save(f, when);
	g_object_set(seats, "min-value", 1.0, "max-value", 10.0, NULL);
	save(f, seats);
	g_object_set(extras, "choices", "Lunch\nParking", NULL);
	save(f, extras);
	g_object_set(code, "pattern", "[A-Z]{3}-[0-9]{2}", "max-length", (gint64)6, NULL);
	save(f, code);
	save(f, phone);
	g_object_unref(publish(f, form));
	{
		const gchar *const missing[] = { "email", "a@example.com", "topic", "sales", NULL };
		const gchar *const email[] = { "name", "A", "email", "not-an-email", "topic", "sales", NULL };
		const gchar *const choice[] = { "name", "A", "email", "a@example.com", "topic", "Sales", NULL };
		const gchar *const rating[] = { "name", "A", "email", "a@example.com", "topic", "sales", "score", "6", NULL };
		const gchar *const fraction[] = { "name", "A", "email", "a@example.com", "topic", "sales", "score", "2.5", NULL };
		const gchar *const url[] = { "name", "A", "email", "a@example.com", "topic", "sales", "website", "ftp://x", NULL };
		const gchar *const date[] = { "name", "A", "email", "a@example.com", "topic", "sales", "when", "2026-02-30", NULL };
		const gchar *const number[] = { "name", "A", "email", "a@example.com", "topic", "sales", "seats", "11", NULL };
		const gchar *const nan[] = { "name", "A", "email", "a@example.com", "topic", "sales", "seats", "nan", NULL };
		const gchar *const multiple[] = { "name", "A", "email", "a@example.com", "topic", "sales", "extras", "lunch", "extras", "boat", NULL };
		const gchar *const pattern[] = { "name", "A", "email", "a@example.com", "topic", "sales", "code", "abc-12", NULL };
		const gchar *const phones[] = { "name", "A", "email", "a@example.com", "topic", "sales", "phone", "call me", NULL };
		const gchar *const twice[] = { "name", "A", "name", "B", "email", "a@example.com", "topic", "sales", NULL };
		const gchar *const line[] = { "name", "A\nB", "email", "a@example.com", "topic", "sales", NULL };
		const gchar *const unknown[] = { "name", "A", "email", "a@example.com", "topic", "sales", "fax", "1", NULL };
		const gchar *const good[] = { "name", " Alice ", "email", "Alice@Example.com", "topic", "something_else",
			"score", "5", "website", "https://example.com", "when", "2026-02-28", "seats", "2.5",
			"extras", "lunch", "extras", "parking", "code", "ABC-12", "phone", "+1 (555) 010-0000",
			"consent", "on", "message", "Line one\nLine two", NULL };
		assert_invalid(f, form, missing, "name");
		assert_invalid(f, form, email, "email");
		assert_invalid(f, form, choice, "topic");
		assert_invalid(f, form, rating, "score");
		assert_invalid(f, form, fraction, "score");
		assert_invalid(f, form, url, "website");
		assert_invalid(f, form, date, "when");
		assert_invalid(f, form, number, "seats");
		assert_invalid(f, form, nan, "seats");
		assert_invalid(f, form, multiple, "extras");
		assert_invalid(f, form, pattern, "code");
		assert_invalid(f, form, phones, "phone");
		assert_invalid(f, form, twice, "name");
		assert_invalid(f, form, line, "name");
		/* Unknown fields are refused, never silently dropped. */
		assert_invalid(f, form, unknown, "fax");
		g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 0);
		g_assert_cmpint(submit_pairs(f, form, good, &errors), ==, VENTURE_FORMS_ACCEPTED);
		g_assert_null(errors);
	}
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_SUBMISSION);
		g_autoptr(VentureEntity) stored = NULL;
		g_autofree gchar *json = NULL, *name = NULL, *summary = NULL;
		stored = venture_database_find_one(f->db, query, NULL);
		g_assert_nonnull(stored);
		g_object_get(stored, "answers", &json, "name", &name, "summary", &summary, NULL);
		/* Stable keys and choice ids, typed values, trimmed text. */
		g_assert_nonnull(strstr(json, "\"name\":\"Alice\""));
		g_assert_nonnull(strstr(json, "\"email\":\"Alice@Example.com\""));
		g_assert_nonnull(strstr(json, "\"topic\":\"something_else\""));
		g_assert_nonnull(strstr(json, "\"score\":5"));
		g_assert_nonnull(strstr(json, "\"seats\":2.5"));
		g_assert_nonnull(strstr(json, "\"extras\":[\"lunch\",\"parking\"]"));
		g_assert_nonnull(strstr(json, "\"consent\":true"));
		g_assert_cmpstr(name, ==, "Alice");
		/* The summary is what search reads: labels, not keys. */
		g_assert_nonnull(strstr(summary, "Topic: Something else"));
	}
}

/* ==========================================================================
 * The public door
 * ========================================================================== */

static void
start_http(Fixture *f)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocketListener) reservation = g_socket_listener_new();
	f->state_dir = g_dir_make_tmp("venture-forms-XXXXXX", NULL);
	f->port = g_socket_listener_add_any_inet_port(reservation, NULL, &error);
	g_assert_no_error(error);
	g_socket_listener_close(reservation);
	g_object_set(f->config, "state-dir", f->state_dir, "server-port", (gint64)f->port,
		"server-bind-address", "127.0.0.1", "security-require-auth", !f->open, NULL);
	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
	f->session = soup_session_new();
}

typedef struct {
	guint status;
	gchar *body;
	gchar *location;
	gchar *allow_origin;
	gchar *content_type;
} Reply;

static void
reply_clear(Reply *reply)
{
	g_clear_pointer(&reply->body, g_free);
	g_clear_pointer(&reply->location, g_free);
	g_clear_pointer(&reply->allow_origin, g_free);
	g_clear_pointer(&reply->content_type, g_free);
}

typedef struct { gboolean done; GBytes *body; GError *error; } Pending;

static void
received(GObject *source, GAsyncResult *result, gpointer data)
{
	Pending *pending = data;
	pending->body = soup_session_send_and_read_finish(SOUP_SESSION(source), result, &pending->error);
	pending->done = TRUE;
}

/* One request; @content_type NULL means a GET. */
static void
request(Fixture *f, const gchar *path, const gchar *content_type, const gchar *body,
	const gchar *origin, const gchar *accept, Reply *reply)
{
	g_autofree gchar *uri = g_strdup_printf("http://127.0.0.1:%u%s", f->port, path);
	g_autoptr(SoupMessage) msg = soup_message_new(content_type != NULL ? "POST" : "GET", uri);
	Pending pending = { FALSE, NULL, NULL };
	SoupMessageHeaders *headers;
	gsize size = 0;
	const gchar *data;
	soup_message_set_flags(msg, SOUP_MESSAGE_NO_REDIRECT);
	if (content_type != NULL)
	{
		g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
		soup_message_set_request_body_from_bytes(msg, content_type, bytes);
	}
	if (origin != NULL) soup_message_headers_replace(soup_message_get_request_headers(msg), "Origin", origin);
	if (accept != NULL) soup_message_headers_replace(soup_message_get_request_headers(msg), "Accept", accept);
	soup_session_send_and_read_async(f->session, msg, G_PRIORITY_DEFAULT, NULL, received, &pending);
	while (!pending.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(pending.error);
	headers = soup_message_get_response_headers(msg);
	data = g_bytes_get_data(pending.body, &size);
	reply->status = soup_message_get_status(msg);
	reply->body = g_strndup(data != NULL ? data : "", size);
	reply->location = g_strdup(soup_message_headers_get_one(headers, "Location"));
	reply->allow_origin = g_strdup(soup_message_headers_get_one(headers, "Access-Control-Allow-Origin"));
	reply->content_type = g_strdup(soup_message_headers_get_one(headers, "Content-Type"));
	g_bytes_unref(pending.body);
}

static guint
post_form(Fixture *f, const gchar *path, const gchar *body)
{
	Reply reply = { 0, NULL, NULL, NULL, NULL };
	guint status;
	request(f, path, "application/x-www-form-urlencoded", body, NULL, NULL, &reply);
	status = reply.status;
	reply_clear(&reply);
	return status;
}

/* A ticket issued far enough in the past that the fill-time guard passes. */
static gchar *
old_ticket(VentureEntity *form)
{
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) then = g_date_time_add_seconds(now, -60);
	g_autofree gchar *ticket = venture_forms_ticket_new(form, then);
	g_autofree gchar *escaped = g_uri_escape_string(ticket, NULL, TRUE);
	return g_strdup_printf("_vf_t=%s", escaped);
}

/* The done-when: a form submits from a plain HTML page with no script at
 * all, and lands as a response. The success is a page, not JSON, because
 * a browser that posted a form shows whatever comes back. */
static void
test_http_plain_post(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "plain-form");
	g_autofree gchar *ticket = old_ticket(form);
	g_autofree gchar *body = g_strdup_printf("%s&name=Alice&email=alice%%40example.com&topic=sales&_vf_hp=", ticket);
	Reply reply = { 0, NULL, NULL, NULL, NULL };
	(void)data;
	start_http(f);
	request(f, "/pub/form/plain-form", "application/x-www-form-urlencoded", body, NULL, "text/html", &reply);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_nonnull(strstr(reply.body, "class=\"vf-success\""));
	g_assert_nonnull(strstr(reply.body, "Thank you"));
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 1);
	reply_clear(&reply);
	/* The same form redirects when told to, with 303 so the browser GETs. */
	g_object_set(form, "redirect-url", "https://example.com/thanks", NULL);
	save(f, form);
	request(f, "/pub/form/plain-form", "application/x-www-form-urlencoded", body, NULL, "text/html", &reply);
	g_assert_cmpuint(reply.status, ==, 303);
	g_assert_cmpstr(reply.location, ==, "https://example.com/thanks");
	reply_clear(&reply);
	/* An invalid post re-renders the form with the error beside the field
	 * and the person's answers still in it. */
	{
		g_autofree gchar *invalid = g_strdup_printf("%s&name=Alice&email=nope&topic=sales", ticket);

		request(f, "/pub/form/plain-form", "application/x-www-form-urlencoded", invalid, NULL, "text/html", &reply);
	}
	g_assert_cmpuint(reply.status, ==, 422);
	g_assert_nonnull(strstr(reply.body, "id=\"vf-plain-form-email-error\""));
	g_assert_nonnull(strstr(reply.body, "value=\"nope\""));
	g_assert_nonnull(strstr(reply.body, "aria-invalid=\"true\""));
	reply_clear(&reply);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 2);
}

/* The script loader and a site's own framework post the same answers and
 * read JSON back. text/plain is accepted because it is the one JSON body a
 * cross-origin fetch can send without a preflight this server does not
 * answer. */
static void
test_http_json(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "json-form");
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) then = g_date_time_add_seconds(now, -60);
	g_autofree gchar *ticket = venture_forms_ticket_new(form, then);
	g_autofree gchar *body = g_strdup_printf("{\"_vf_t\":\"%s\",\"name\":\"Bob\",\"email\":\"bob@example.com\","
		"\"topic\":\"support\",\"score\":4,\"consent\":true}", ticket);
	Reply reply = { 0, NULL, NULL, NULL, NULL };
	(void)data;
	start_http(f);
	request(f, "/pub/form/json-form", "text/plain;charset=UTF-8", body, NULL, "application/json", &reply);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_nonnull(strstr(reply.body, "\"ok\":true"));
	reply_clear(&reply);
	{
		g_autofree gchar *invalid = g_strdup_printf("{\"_vf_t\":\"%s\",\"name\":\"Bob\",\"email\":\"x\","
			"\"topic\":\"support\"}", ticket);

		request(f, "/pub/form/json-form", "application/json", invalid, NULL, "application/json", &reply);
	}
	g_assert_cmpuint(reply.status, ==, 422);
	g_assert_nonnull(strstr(reply.body, "\"errors\":{"));
	g_assert_nonnull(strstr(reply.body, "\"email\":"));
	reply_clear(&reply);
	/* A JSON value of the wrong shape is refused, not coerced. */
	request(f, "/pub/form/json-form", "application/json", "{\"name\":{\"first\":\"Bob\"}}", NULL, "application/json", &reply);
	g_assert_cmpuint(reply.status, ==, 422);
	reply_clear(&reply);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 1);
}

/* Draft, closed, unknown, past its close date and over its cap: all five
 * answer exactly the same 404, so the door says nothing about which. */
static void
test_http_refusals(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) draft = make_form(f, "draft-form", VENTURE_FORM_DRAFT);
	g_autoptr(VentureEntity) closed = make_form(f, "closed-form", VENTURE_FORM_CLOSED);
	g_autoptr(VentureEntity) capped = contact_form(f, "capped-form");
	g_autoptr(VentureEntity) expired = make_form(f, "expired-form", VENTURE_FORM_LIVE);
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) past = g_date_time_add_hours(now, -1);
	const gchar *paths[] = { "/pub/form/draft-form", "/pub/form/closed-form", "/pub/form/no-such-form",
		"/pub/form/expired-form", "/pub/form/capped-form" };
	g_autofree gchar *reference = NULL;
	g_autofree gchar *ticket = old_ticket(capped);
	g_autofree gchar *body = g_strdup_printf("%s&name=A&email=a%%40example.com&topic=sales", ticket);
	guint i;
	(void)data;
	g_object_set(expired, "closes-at", past, NULL);
	save(f, expired);
	g_object_set(capped, "response-limit", (gint64)1, NULL);
	save(f, capped);
	start_http(f);
	g_assert_cmpuint(post_form(f, "/pub/form/capped-form", body), ==, 200);
	for (i = 0; i < G_N_ELEMENTS(paths); i++)
	{
		const gchar *suffixes[] = { "", "/fragment", "/schema" };
		guint j;
		{
			Reply reply = { 0, NULL, NULL, NULL, NULL };
			request(f, paths[i], "application/x-www-form-urlencoded", body, NULL, NULL, &reply);
			g_assert_cmpuint(reply.status, ==, 404);
			if (reference == NULL) reference = g_strdup(reply.body);
			g_assert_cmpstr(reply.body, ==, reference);
			reply_clear(&reply);
		}
		for (j = 0; j < G_N_ELEMENTS(suffixes); j++)
		{
			g_autofree gchar *path = g_strconcat(paths[i], suffixes[j], NULL);
			Reply reply = { 0, NULL, NULL, NULL, NULL };
			request(f, path, NULL, NULL, NULL, NULL, &reply);
			g_assert_cmpuint(reply.status, ==, 404);
			reply_clear(&reply);
		}
	}
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 1);
}

/* An oversize body is refused before it is parsed; a flood from one client
 * is refused before it is saved. */
static void
test_http_limits(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "limits-form");
	g_autofree gchar *ticket = old_ticket(form);
	g_autofree gchar *body = g_strdup_printf("%s&name=A&email=a%%40example.com&topic=sales", ticket);
	GString *big = g_string_new("name=");
	guint i, status = 0;
	(void)data;
	while (big->len < 70 * 1024) g_string_append_c(big, 'x');
	g_object_set(form, "hourly-limit", (gint64)2, NULL);
	save(f, form);
	start_http(f);
	g_assert_cmpuint(post_form(f, "/pub/form/limits-form", big->str), ==, 413);
	g_string_free(big, TRUE);
	for (i = 0; i < 3; i++) status = post_form(f, "/pub/form/limits-form", body);
	g_assert_cmpuint(status, ==, 429);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 2);
}

/* A bot that fills the honeypot, or answers faster than a person could,
 * is told it succeeded and nothing is saved: telling it why teaches it. */
static void
test_http_spam(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "spam-form");
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *fresh = venture_forms_ticket_new(form, now);
	g_autofree gchar *escaped = g_uri_escape_string(fresh, NULL, TRUE);
	g_autofree gchar *ticket = old_ticket(form);
	g_autofree gchar *honeypot = g_strdup_printf("%s&name=A&email=a%%40example.com&topic=sales&_vf_hp=buy+now", ticket);
	g_autofree gchar *fast = g_strdup_printf("_vf_t=%s&name=A&email=a%%40example.com&topic=sales", escaped);
	g_autofree gchar *forged = g_strdup("_vf_t=1.deadbeef&name=A&email=a%40example.com&topic=sales");
	g_autofree gchar *missing = g_strdup("name=A&email=a%40example.com&topic=sales");
	(void)data;
	start_http(f);
	g_assert_cmpuint(post_form(f, "/pub/form/spam-form", honeypot), ==, 200);
	g_assert_cmpuint(post_form(f, "/pub/form/spam-form", fast), ==, 200);
	g_assert_cmpuint(post_form(f, "/pub/form/spam-form", forged), ==, 200);
	g_assert_cmpuint(post_form(f, "/pub/form/spam-form", missing), ==, 200);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 0);
}

/* A form limited to named sites refuses a browser on any other, and tells
 * the allowed one it may read the answer. */
static void
test_http_origins(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "origin-form");
	g_autofree gchar *ticket = old_ticket(form);
	g_autofree gchar *body = g_strdup_printf("%s&name=A&email=a%%40example.com&topic=sales", ticket);
	Reply reply = { 0, NULL, NULL, NULL, NULL };
	(void)data;
	g_object_set(form, "allowed-origins", "https://shop.example", NULL);
	save(f, form);
	start_http(f);
	request(f, "/pub/form/origin-form", "application/x-www-form-urlencoded", body, "https://evil.example", NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 403);
	g_assert_null(reply.allow_origin);
	reply_clear(&reply);
	request(f, "/pub/form/origin-form/fragment", NULL, NULL, "https://evil.example", NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 403);
	reply_clear(&reply);
	request(f, "/pub/form/origin-form", "application/x-www-form-urlencoded", body, "https://shop.example", "application/json", &reply);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_cmpstr(reply.allow_origin, ==, "https://shop.example");
	reply_clear(&reply);
	request(f, "/pub/form/origin-form/fragment", NULL, NULL, "https://shop.example", NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_cmpstr(reply.allow_origin, ==, "https://shop.example");
	g_assert_nonnull(strstr(reply.body, "class=\"vf-form\""));
	reply_clear(&reply);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 1);
}

/* The four ways in: the hosted page, the fragment the loader fetches, the
 * loader itself and the JSON schema. All of them are public; none of them
 * needs or honours a session. */
static void
test_http_ways_in(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "ways-form");
	Reply reply = { 0, NULL, NULL, NULL, NULL };
	(void)data;
	start_http(f);
	request(f, "/pub/form/ways-form", NULL, NULL, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_true(g_str_has_prefix(reply.body, "<!DOCTYPE html>"));
	g_assert_nonnull(strstr(reply.body, "action=\"/pub/form/ways-form\""));
	g_assert_null(strstr(reply.body, "<style"));
	reply_clear(&reply);
	/* The optional stylesheet is opt-in, and even then only structural. */
	request(f, "/pub/form/ways-form?style=basic", NULL, NULL, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_nonnull(strstr(reply.body, "<style"));
	reply_clear(&reply);
	request(f, "/pub/form/ways-form/fragment", NULL, NULL, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_true(g_str_has_prefix(reply.body, "<form"));
	g_assert_nonnull(strstr(reply.body, "name=\"_vf_t\""));
	reply_clear(&reply);
	request(f, "/pub/form/ways-form/schema", NULL, NULL, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_nonnull(strstr(reply.content_type, "application/json"));
	g_assert_nonnull(strstr(reply.body, "\"fields\":["));
	reply_clear(&reply);
	request(f, "/pub/forms.js", NULL, NULL, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_nonnull(strstr(reply.content_type, "javascript"));
	g_assert_nonnull(strstr(reply.body, "data-venture-form"));
	/* No shadow DOM, or the page's CSS could not reach the form. */
	g_assert_null(strstr(reply.body, "attachShadow"));
	reply_clear(&reply);
}

/* The embed codes the builder offers are generated from the same renderer
 * and name the real public addresses. */
static void
test_embed_codes(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "embed-form");
	g_autoptr(GError) error = NULL;
	g_autofree gchar *snippet = NULL, *script = NULL;
	(void)data;
	g_object_set(f->config, "server-base-url", "https://crm.example", NULL);
	snippet = venture_forms_embed_code(f->db, form, "https://crm.example", VENTURE_FORMS_EMBED_HTML, &error);
	g_assert_no_error(error);
	g_assert_nonnull(strstr(snippet, "action=\"https://crm.example/pub/form/embed-form\""));
	g_assert_nonnull(strstr(snippet, "class=\"vf-form\""));
	script = venture_forms_embed_code(f->db, form, "https://crm.example", VENTURE_FORMS_EMBED_SCRIPT, &error);
	g_assert_no_error(error);
	g_assert_nonnull(strstr(script, "src=\"https://crm.example/pub/forms.js\""));
	g_assert_nonnull(strstr(script, "data-venture-form=\"https://crm.example/pub/form/embed-form/fragment\""));
}

/* The form's own page is the builder: its questions in order, how many
 * responses, a preview from the public renderer, and a copy button for
 * each way onto a site. */
static void
test_builder_page(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "builder-form");
	g_autofree gchar *path = g_strdup_printf("/e/form/%" G_GINT64_FORMAT, venture_entity_get_id(form));
	Reply reply = { 0, NULL, NULL, NULL, NULL };
	(void)data;
	f->open = TRUE;
	start_http(f);
	request(f, path, NULL, NULL, NULL, "text/html", &reply);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_nonnull(strstr(reply.body, "Questions"));
	g_assert_nonnull(strstr(reply.body, "action=\"/forms/"));
	g_assert_nonnull(strstr(reply.body, "Publish</button>"));
	g_assert_nonnull(strstr(reply.body, "/e/form_field/new?form_id="));
	g_assert_nonnull(strstr(reply.body, "class=\"form-preview\""));
	g_assert_nonnull(strstr(reply.body, "data-vf-form=\"builder-form\""));
	g_assert_nonnull(strstr(reply.body, "id=\"form-embed-html\""));
	g_assert_nonnull(strstr(reply.body, "id=\"form-embed-script\""));
	g_assert_nonnull(strstr(reply.body, "id=\"form-embed-iframe\""));
	g_assert_nonnull(strstr(reply.body, "id=\"form-embed-schema\""));
	g_assert_nonnull(strstr(reply.body, "/reports/form_summary?form_id="));
	/* The preview posts nowhere and cannot be sent. */
	g_assert_nonnull(strstr(reply.body, "<button class=\"vf-submit\" type=\"submit\" disabled>"));
	reply_clear(&reply);
}

/* ==========================================================================
 * Accessibility: the rules the embed contract promises, checked
 *
 * A structural checker over the parsed markup, in the suite so it runs on
 * every build. It checks what the renderer is responsible for on any host
 * page: every control named, ids unique, every description reference
 * real, groups labelled, required said in text and to assistive
 * technology, errors tied to their fields and summarised with working
 * links, focus order left alone, no styling of ours. Colour contrast is
 * the host page's, so it is not ours to check; tools/venture-forms-axe.sh
 * runs axe-core over the same renderings when node is available.
 * ========================================================================== */

typedef struct
{
	GHashTable	*ids;		/* id -> element */
	GHashTable	*label_for;	/* ids named by a <label for> */
	GPtrArray	*elements;
} A11yIndex;

static gchar *
a11y_attr(xmlNode *node, const gchar *name)
{
	xmlChar *value = xmlGetProp(node, (const xmlChar *)name);
	gchar *copy = value != NULL ? g_strdup((const gchar *)value) : NULL;
	xmlFree(value);
	return copy;
}

static gboolean
a11y_has(xmlNode *node, const gchar *name)
{
	return xmlHasProp(node, (const xmlChar *)name) != NULL;
}

static gboolean
a11y_is(xmlNode *node, const gchar *tag)
{
	return node->type == XML_ELEMENT_NODE && g_ascii_strcasecmp((const gchar *)node->name, tag) == 0;
}

static gchar *
a11y_text(xmlNode *node)
{
	xmlChar *text = xmlNodeGetContent(node);
	gchar *copy = g_strstrip(g_strdup(text != NULL ? (const gchar *)text : ""));
	xmlFree(text);
	return copy;
}

static void
a11y_collect(xmlNode *node, A11yIndex *index, GString *problems)
{
	for (; node != NULL; node = node->next)
	{
		if (node->type != XML_ELEMENT_NODE)
			continue;
		g_ptr_array_add(index->elements, node);
		{
			g_autofree gchar *id = a11y_attr(node, "id");
			if (id != NULL)
			{
				if (g_hash_table_contains(index->ids, id))
					g_string_append_printf(problems, "duplicate id %s\n", id);
				g_hash_table_insert(index->ids, g_strdup(id), node);
			}
		}
		if (a11y_is(node, "label"))
		{
			gchar *target = a11y_attr(node, "for");
			if (target != NULL)
				g_hash_table_add(index->label_for, target);
		}
		a11y_collect(node->children, index, problems);
	}
}

static gboolean
a11y_inside(xmlNode *node, const gchar *tag)
{
	for (node = node->parent; node != NULL; node = node->parent)
		if (a11y_is(node, tag))
			return TRUE;
	return FALSE;
}

static xmlNode *
a11y_enclosing(xmlNode *node, const gchar *tag)
{
	for (node = node->parent; node != NULL; node = node->parent)
		if (a11y_is(node, tag))
			return node;
	return NULL;
}

/* Every problem found, one per line; empty when the markup keeps every
 * rule. */
static gchar *
a11y_check(const gchar *html)
{
	GString *problems = g_string_new(NULL);
	htmlDocPtr doc;
	A11yIndex index;
	gboolean any_invalid = FALSE, any_required = FALSE;
	guint i;

	doc = htmlReadMemory(html, (int)strlen(html), NULL, "utf-8",
		HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING | HTML_PARSE_NONET);
	g_assert_nonnull(doc);
	index.ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	index.label_for = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	index.elements = g_ptr_array_new();
	a11y_collect(xmlDocGetRootElement(doc), &index, problems);

	for (i = 0; i < index.elements->len; i++)
	{
		xmlNode *node = g_ptr_array_index(index.elements, i);
		g_autofree gchar *type = a11y_attr(node, "type");
		g_autofree gchar *id = a11y_attr(node, "id");
		g_autofree gchar *tabindex = a11y_attr(node, "tabindex");
		g_autofree gchar *invalid = a11y_attr(node, "aria-invalid");
		g_autofree gchar *described = a11y_attr(node, "aria-describedby");
		g_autofree gchar *role = a11y_attr(node, "role");

		/* Ours never styles. */
		if (a11y_has(node, "style"))
			g_string_append_printf(problems, "<%s> has a style attribute\n", node->name);
		/* Focus order is the host page's: nothing jumps the queue. */
		if (tabindex != NULL && g_ascii_strtoll(tabindex, NULL, 10) > 0)
			g_string_append_printf(problems, "<%s> has a positive tabindex\n", node->name);
		/* Every description a control points at exists. */
		if (described != NULL)
		{
			g_auto(GStrv) refs = g_strsplit(described, " ", -1);
			guint j;
			for (j = 0; refs[j] != NULL; j++)
				if (*refs[j] != '\0' && !g_hash_table_contains(index.ids, refs[j]))
					g_string_append_printf(problems, "aria-describedby names missing id %s\n", refs[j]);
		}
		/* A control is named: wrapped by its label or named by one. */
		if ((a11y_is(node, "input") && g_strcmp0(type, "hidden") != 0 && g_strcmp0(type, "submit") != 0) ||
		    a11y_is(node, "textarea") || a11y_is(node, "select"))
		{
			if (!a11y_inside(node, "label") && (id == NULL || !g_hash_table_contains(index.label_for, id)))
				g_string_append_printf(problems, "control %s has no label\n", id != NULL ? id : "(no id)");
			if (a11y_has(node, "required"))
			{
				any_required = TRUE;
				if (g_strcmp0(type, "radio") == 0)
				{
					xmlNode *group = a11y_enclosing(node, "fieldset");
					g_autofree gchar *said = group != NULL ? a11y_attr(group, "aria-required") : NULL;
					if (g_strcmp0(said, "true") != 0)
						g_string_append_printf(problems, "required radio %s: its group does not say so\n", id);
				}
				else
				{
					g_autofree gchar *said = a11y_attr(node, "aria-required");
					if (g_strcmp0(said, "true") != 0)
						g_string_append_printf(problems, "required control %s lacks aria-required\n", id);
				}
			}
		}
		/* A group is labelled by its first child. */
		if (a11y_is(node, "fieldset"))
		{
			xmlNode *first = node->children;
			while (first != NULL && first->type != XML_ELEMENT_NODE)
				first = first->next;
			if (first == NULL || !a11y_is(first, "legend"))
				g_string_append_printf(problems, "fieldset %s has no legend first\n", id != NULL ? id : "");
			else
			{
				g_autofree gchar *text = a11y_text(first);
				if (*text == '\0')
					g_string_append_printf(problems, "fieldset %s has an empty legend\n", id != NULL ? id : "");
			}
		}
		/* A refusal is said in words, beside the answer, not in colour. */
		if (g_strcmp0(invalid, "true") == 0)
		{
			gboolean told = FALSE;
			any_invalid = TRUE;
			if (described != NULL)
			{
				g_auto(GStrv) refs = g_strsplit(described, " ", -1);
				guint j;
				for (j = 0; refs[j] != NULL; j++)
				{
					xmlNode *target = g_hash_table_lookup(index.ids, refs[j]);
					g_autofree gchar *text = target != NULL ? a11y_text(target) : NULL;
					if (target != NULL && !a11y_has(target, "hidden") && text != NULL && *text != '\0')
						told = TRUE;
				}
			}
			if (!told)
				g_string_append_printf(problems, "%s is invalid with no visible message tied to it\n",
					id != NULL ? id : (const gchar *)node->name);
		}
		/* Every link in the error summary goes somewhere. */
		if (a11y_is(node, "a"))
		{
			g_autofree gchar *href = a11y_attr(node, "href");
			if (href != NULL && href[0] == '#' && !g_hash_table_contains(index.ids, href + 1))
				g_string_append_printf(problems, "link to missing %s\n", href);
		}
		if (a11y_is(node, "button"))
		{
			g_autofree gchar *text = a11y_text(node);
			if (*text == '\0')
				g_string_append(problems, "a button has no text\n");
		}
		(void)role;
	}

	/* A failed submit is summarised at the top, where focus goes. */
	if (any_invalid)
	{
		gboolean summary = FALSE;
		for (i = 0; i < index.elements->len; i++)
		{
			xmlNode *node = g_ptr_array_index(index.elements, i);
			g_autofree gchar *role = a11y_attr(node, "role");
			g_autofree gchar *tabindex = a11y_attr(node, "tabindex");
			if (g_strcmp0(role, "alert") == 0 && !a11y_has(node, "hidden") && g_strcmp0(tabindex, "-1") == 0)
				summary = TRUE;
		}
		if (!summary)
			g_string_append(problems, "errors with no visible, focusable summary\n");
	}
	/* Required is said in words for those who read the page. */
	if (any_required && strstr(html, "class=\"vf-required-note\"") == NULL)
		g_string_append(problems, "required questions with no note saying what marks them\n");
	/* A whole page says its language and its title. */
	{
		xmlNode *root = xmlDocGetRootElement(doc);
		if (strstr(html, "<!DOCTYPE") != NULL)
		{
			g_autofree gchar *lang = root != NULL ? a11y_attr(root, "lang") : NULL;
			if (lang == NULL || *lang == '\0')
				g_string_append(problems, "the page has no lang\n");
			if (strstr(html, "<title>") == NULL || strstr(html, "<title></title>") != NULL)
				g_string_append(problems, "the page has no title\n");
		}
	}

	g_hash_table_unref(index.ids);
	g_hash_table_unref(index.label_for);
	g_ptr_array_unref(index.elements);
	xmlFreeDoc(doc);
	return g_string_free(problems, FALSE);
}

/* A form with every kind, alternating required, some with help. */
static VentureEntity *
every_kind_form(Fixture *f, const gchar *token)
{
	VentureEntity *form = make_form(f, token, VENTURE_FORM_LIVE);
	GEnumClass *klass = g_type_class_ref(VENTURE_TYPE_FORM_FIELD_KIND);
	guint i;
	for (i = 0; i < klass->n_values; i++)
	{
		GEnumValue *value = &klass->values[i];
		g_autofree gchar *key = g_strdup(value->value_nick);
		g_autoptr(VentureEntity) field = make_field(f, form, key, value->value_nick,
			(VentureFormFieldKind)value->value, i % 2 == 0, (gint64)i);
		if (i % 3 == 0)
			g_object_set(field, "help", "A line of help.", NULL);
		if (value->value == VENTURE_FORM_FIELD_SINGLE_CHOICE || value->value == VENTURE_FORM_FIELD_MULTIPLE_CHOICE)
			g_object_set(field, "choices", "One\nTwo\nThree", NULL);
		save(f, field);
	}
	g_type_class_unref(klass);
	g_object_unref(publish(f, form));
	return form;
}

static gchar *
render_with(Fixture *f, VentureEntity *form, VentureFormsRenderMode mode, JsonObject *values, JsonObject *errors)
{
	VentureFormsRender options;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureEntity) version = venture_forms_published_version(f->db, form, NULL);
	gchar *html;
	options.mode = mode;
	options.action = NULL;
	options.ticket = "1.1.x";
	options.values = values;
	options.errors = errors;
	options.version = version;
	html = venture_forms_render(f->db, form, &options, &error);
	g_assert_no_error(error);
	return html;
}

/* Writes a rendering where tools/venture-forms-axe.sh will find it, when
 * asked to; the suite itself only checks. */
static void
a11y_keep(const gchar *name, const gchar *html)
{
	const gchar *directory = g_getenv("VENTURE_FORMS_A11Y_DIR");
	g_autofree gchar *path = NULL;
	g_autofree gchar *page = NULL;
	if (directory == NULL)
		return;
	path = g_build_filename(directory, name, NULL);
	page = g_str_has_prefix(html, "<!DOCTYPE") ? g_strdup(html) :
		g_strdup_printf("<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"utf-8\"><title>%s</title>"
		"</head><body><main>%s</main></body></html>", name, html);
	g_assert_true(g_file_set_contents(path, page, -1, NULL));
}

/* Every field kind, in every state a stranger sees -- blank, refused and
 * thanked, bare and as a whole page -- keeps every rule. */
static void
test_a11y_contract(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = every_kind_form(f, "a11y-form");
	g_object_set(form, "privacy-url", "https://example.com/privacy", NULL);
	save(f, form);
	g_autoptr(JsonObject) errors = json_object_new();
	g_autoptr(JsonObject) values = json_object_new();
	g_autoptr(GPtrArray) fields = venture_forms_fields(f->db, form, NULL);
	g_autofree gchar *blank = NULL, *page = NULL, *refused = NULL, *thanks = NULL, *bare_thanks = NULL;
	guint i;
	(void)data;
	for (i = 0; i < fields->len; i++)
	{
		g_autofree gchar *key = NULL;
		g_object_get(g_ptr_array_index(fields, i), "key", &key, NULL);
		if (g_strcmp0(key, "hidden") != 0)
			json_object_set_string_member(errors, key, "This answer needs attention.");
		json_object_set_string_member(values, key, "x");
	}
	json_object_set_string_member(errors, "fax", "This form has no such question.");
	blank = render_with(f, form, VENTURE_FORMS_RENDER_FRAGMENT, NULL, NULL);
	page = render_with(f, form, VENTURE_FORMS_RENDER_HOSTED, NULL, NULL);
	refused = render_with(f, form, VENTURE_FORMS_RENDER_HOSTED, values, errors);
	thanks = venture_forms_render_success(form, TRUE);
	bare_thanks = venture_forms_render_success(form, FALSE);
	{
		const gchar *names[] = { "blank.html", "page.html", "refused.html", "thanks.html", "bare-thanks.html" };
		const gchar *pages[] = { blank, page, refused, thanks, bare_thanks };
		for (i = 0; i < G_N_ELEMENTS(pages); i++)
		{
			g_autofree gchar *problems = a11y_check(pages[i]);
			a11y_keep(names[i], pages[i]);
			if (*problems != '\0')
				g_error("%s breaks the accessibility contract:\n%s", names[i], problems);
		}
	}
	/* The refused page's summary links to every refused question and
	 * takes focus; the success says so to a screen reader. */
	g_assert_nonnull(strstr(refused, "class=\"vf-error-list\""));
	g_assert_nonnull(strstr(refused, "href=\"#vf-a11y-form-email\""));
	g_assert_nonnull(strstr(refused, "href=\"#vf-a11y-form-single_choice--one\""));
	g_assert_nonnull(strstr(refused, "autofocus"));
	g_assert_nonnull(strstr(thanks, "role=\"status\""));
	g_assert_nonnull(strstr(blank, "autocomplete=\"email\""));
	g_assert_nonnull(strstr(blank, "<p class=\"vf-privacy\"><a href=\"https://example.com/privacy\">Privacy notice</a></p>"));
	g_assert_nonnull(strstr(blank, "autocomplete=\"tel\""));
}

/* The checker is only worth something if it fails bad markup. */
static void
test_a11y_checker_catches(void)
{
	static const gchar *const bad[] = {
		"<form><input id=\"a\" name=\"a\"></form>",
		"<form><label for=\"a\">A</label><input id=\"a\" required></form>",
		"<form><fieldset><div></div><input type=\"radio\" id=\"r\"></fieldset></form>",
		"<form><label for=\"a\">A</label><input id=\"a\" aria-invalid=\"true\"></form>",
		"<form><label for=\"a\">A</label><input id=\"a\" aria-describedby=\"nothing\"></form>",
		"<form><a href=\"#nowhere\">x</a></form>",
		"<form><label for=\"a\">A</label><input id=\"a\" tabindex=\"3\"></form>",
		"<form><p style=\"color:red\">x</p></form>",
		"<form><button></button></form>",
		"<!DOCTYPE html><html><head></head><body></body></html>",
		NULL
	};
	guint i;
	for (i = 0; bad[i] != NULL; i++)
	{
		g_autofree gchar *problems = a11y_check(bad[i]);
		if (*problems == '\0')
			g_error("the checker passed bad markup: %s", bad[i]);
	}
}

/* ==========================================================================
 * Versions
 * ========================================================================== */

static gdouble summary_value(JsonNode *json, const gchar *answer, const gchar *column);

/* Whether the summary has a row for @question on @versions whose @answer
 * row averages @average. */
static gboolean
summary_has(JsonNode *json, const gchar *question, const gchar *versions, const gchar *answer, gdouble average)
{
	JsonArray *rows = json_object_get_array_member(json_node_get_object(json), "rows");
	guint i;
	for (i = 0; i < json_array_get_length(rows); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);
		if (g_strcmp0(json_object_get_string_member_with_default(row, "question", ""), question) == 0 &&
		    g_strcmp0(json_object_get_string_member_with_default(row, "versions", ""), versions) == 0 &&
		    g_strcmp0(json_object_get_string_member_with_default(row, "answer", ""), answer) == 0)
			return json_object_get_double_member(row, "average") == average;
	}
	return FALSE;
}

/* The field whose key is @key on @form. */
static VentureEntity *
field_by_key(Fixture *f, VentureEntity *form, const gchar *key)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_FIELD);
	venture_query_add_filter_int(query, "form-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(form), NULL);
	venture_query_add_filter_string(query, "key", VENTURE_FILTER_OP_EQ, key, NULL);
	return venture_database_find_one(f->db, query, NULL);
}

static gchar *
public_fragment(Fixture *f, const gchar *token)
{
	g_autofree gchar *path = g_strdup_printf("/pub/form/%s/fragment", token);
	Reply reply = { 0, NULL, NULL, NULL, NULL };
	gchar *body;
	request(f, path, NULL, NULL, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 200);
	body = g_steal_pointer(&reply.body);
	reply_clear(&reply);
	return body;
}

/* Editing a live form's questions changes the draft, not what strangers
 * see; publishing is what makes it public. */
static void
test_versions_freeze(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "freeze-form");
	g_autoptr(VentureEntity) name = field_by_key(f, form, "name");
	g_autoptr(VentureEntity) v2 = NULL;
	g_autofree gchar *before = NULL, *after = NULL;
	gint64 number = 0;
	(void)data;
	start_http(f);
	g_object_set(name, "label", "Full legal name", NULL);
	save(f, name);
	add_field(f, form, "company", "Company", VENTURE_FORM_FIELD_SHORT_TEXT, FALSE, 70);
	g_assert_true(venture_forms_has_unpublished_changes(f->db, form));
	before = public_fragment(f, "freeze-form");
	g_assert_nonnull(strstr(before, "Your name"));
	g_assert_null(strstr(before, "Full legal name"));
	g_assert_null(strstr(before, "data-vf-field=\"company\""));
	v2 = publish(f, form);
	g_object_get(v2, "number", &number, NULL);
	g_assert_cmpint(number, ==, 2);
	g_assert_false(venture_forms_has_unpublished_changes(f->db, form));
	after = public_fragment(f, "freeze-form");
	g_assert_nonnull(strstr(after, "Full legal name"));
	g_assert_nonnull(strstr(after, "data-vf-field=\"company\""));
}

/* A second publish with nothing changed is not a second version, and a
 * form with no questions has nothing to publish. A live form never
 * published is not available at all. */
static void
test_versions_publish_rules(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "rules-form");
	g_autoptr(VentureEntity) empty = make_form(f, "empty-form", VENTURE_FORM_LIVE);
	g_autoptr(VentureEntity) again = NULL;
	g_autoptr(GError) error = NULL;
	gint64 number = 0;
	(void)data;
	again = publish(f, form);
	g_object_get(again, "number", &number, NULL);
	g_assert_cmpint(number, ==, 1);
	g_assert_null(venture_forms_publish(f->db, empty, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
	g_clear_error(&error);
	g_assert_null(venture_forms_find_live(f->db, "empty-form", NULL, &error));
}

/* A version is what the form asked: nothing creates one but publishing and
 * nothing edits one, from any door. */
static void
test_versions_read_only(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "readonly-form");
	g_autoptr(VentureEntity) version = venture_forms_published_version(f->db, form, NULL);
	g_autoptr(VentureEntity) forged = VENTURE_ENTITY(venture_form_version_new());
	g_autoptr(VentureEntity) other = contact_form(f, "other-form");
	g_autoptr(VentureEntity) foreign = venture_forms_published_version(f->db, other, NULL);
	Reply reply = { 0, NULL, NULL, NULL, NULL };
	g_autofree gchar *path = NULL;
	(void)data;
	g_assert_nonnull(version);
	g_object_set(version, "definition", "{\"fields\":[]}", NULL);
	refuse(f, version, "cannot be changed");
	venture_entity_set_organization_id(forged, f->org);
	g_object_set(forged, "name", "Forged", "form-id", venture_entity_get_id(form), "number", (gint64)9,
		"definition", "{\"fields\":[]}", NULL);
	refuse(f, forged, "publishing");
	/* Pointing a form at another form's version is refused. */
	g_object_set(form, "published-version-id", venture_entity_get_id(foreign), NULL);
	refuse(f, form, "not a version of this form");
	f->open = TRUE;
	start_http(f);
	path = g_strdup_printf("/api/v1/form_version/%" G_GINT64_FORMAT, venture_entity_get_id(version));
	request(f, "/api/v1/form_version", "application/json", "{\"name\":\"x\"}", NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 403);
	reply_clear(&reply);
}

/* Going back: point the form at an earlier version and the public sees
 * it again, with its number, and its tickets name it. */
static void
test_versions_rollback(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "rollback-form");
	g_autoptr(VentureEntity) v1 = venture_forms_published_version(f->db, form, NULL);
	g_autoptr(VentureEntity) v2 = NULL;
	g_autoptr(VentureEntity) stored = NULL;
	gint64 number = 0;
	(void)data;
	add_field(f, form, "company", "Company", VENTURE_FORM_FIELD_SHORT_TEXT, FALSE, 70);
	v2 = publish(f, form);
	g_object_set(form, "published-version-id", venture_entity_get_id(v1), NULL);
	save(f, form);
	stored = reread(f, form);
	g_object_get(stored, "published-number", &number, NULL);
	g_assert_cmpint(number, ==, 1);
}

/* Whoever started on version 1 finishes on version 1: a ticket issued
 * before version 2 was published checks the answers against the questions
 * the person could see, and files them under version 1. */
static void
test_versions_mid_draft(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "draft-mid-form");
	g_autoptr(VentureEntity) message = field_by_key(f, form, "message");
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) then = g_date_time_add_seconds(now, -60);
	g_autofree gchar *v1_ticket = venture_forms_ticket_new(form, then);
	g_autofree gchar *escaped = g_uri_escape_string(v1_ticket, NULL, TRUE);
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_SUBMISSION);
	g_autoptr(VentureEntity) response = NULL;
	gint64 number = 0;
	(void)data;
	/* Version 2 drops the message and asks for a company. */
	g_assert_true(venture_database_delete(f->db, message, NULL, &error));
	g_assert_no_error(error);
	add_field(f, form, "company", "Company", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 70);
	g_object_unref(publish(f, form));
	start_http(f);
	{
		g_autofree gchar *old = g_strdup_printf("_vf_t=%s&name=A&email=a%%40example.com&topic=sales&message=Hi", escaped);
		g_autofree gchar *mixed = g_strdup_printf("_vf_t=%s&name=A&email=a%%40example.com&topic=sales&company=Acme", escaped);
		Reply reply = { 0, NULL, NULL, NULL, NULL };

		request(f, "/pub/form/draft-mid-form", "application/x-www-form-urlencoded", old, NULL, "application/json", &reply);
		g_assert_cmpuint(reply.status, ==, 200);
		reply_clear(&reply);
		/* Version 1 never asked for a company. */
		request(f, "/pub/form/draft-mid-form", "application/x-www-form-urlencoded", mixed, NULL, "application/json", &reply);
		g_assert_cmpuint(reply.status, ==, 422);
		g_assert_nonnull(strstr(reply.body, "\"company\""));
		reply_clear(&reply);
	}
	response = venture_database_find_one(f->db, query, NULL);
	g_object_get(response, "version-number", &number, NULL);
	g_assert_cmpint(number, ==, 1);
}

/* Two responses, one on each version, read side by side: each with the
 * labels it was answered under. The summary counts a relabelled choice as
 * one row and splits a scale whose meaning changed. */
static void
test_versions_side_by_side(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "side-form");
	g_autoptr(VentureEntity) score = field_by_key(f, form, "score");
	g_autoptr(VentureEntity) topic = field_by_key(f, form, "topic");
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_SUBMISSION);
	g_autoptr(GPtrArray) rows = NULL;
	const gchar *const first[] = { "name", "A", "email", "a@example.com", "topic", "sales", "score", "5", NULL };
	const gchar *const second[] = { "name", "B", "email", "b@example.com", "topic", "sales", "score", "9", NULL };
	VentureReportRegistry *registry = venture_context_get_report_registry(f->context);
	g_autoptr(VentureDateRange) period = venture_date_range_new_all_time();
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(JsonNode) json = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL;
	guint i;
	(void)data;
	g_assert_cmpint(submit_pairs(f, form, first, NULL), ==, VENTURE_FORMS_ACCEPTED);
	g_object_set(score, "label", "How likely are you to recommend us?", "min-value", 0.0, "max-value", 10.0, NULL);
	save(f, score);
	g_object_set(topic, "choices", "sales | Buying something\nsupport | Support\nsomething_else | Something else", NULL);
	save(f, topic);
	g_object_unref(publish(f, form));
	g_assert_cmpint(submit_pairs(f, form, second, NULL), ==, VENTURE_FORMS_ACCEPTED);

	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(f->db, query, NULL);
	g_assert_cmpuint(rows->len, ==, 2);
	for (i = 0; i < 2; i++)
	{
		g_autofree gchar *html = venture_forms_render_answers(f->db, g_ptr_array_index(rows, i), &error);
		g_assert_no_error(error);
		if (0 == i)
		{
			g_assert_nonnull(strstr(html, "How did we do?"));
			g_assert_nonnull(strstr(html, "<dd><p>Sales</p></dd>"));
			g_assert_nonnull(strstr(html, "version 1"));
		}
		else
		{
			g_assert_nonnull(strstr(html, "How likely are you to recommend us?"));
			g_assert_nonnull(strstr(html, "<dd><p>Buying something</p></dd>"));
			g_assert_nonnull(strstr(html, "version 2"));
		}
	}

	json_object_set_int_member(options, "form_id", venture_entity_get_id(form));
	result = venture_report_generate(venture_report_registry_lookup(registry, "form_summary"),
		f->context, period, options, &error);
	g_assert_no_error(error);
	json = venture_report_result_to_json(result);
	text = json_to_string(json, FALSE);
	/* The relabelled choice: one row, both responses, the newer label. */
	g_assert_cmpfloat(summary_value(json, "Buying something", "count"), ==, 2);
	g_assert_null(strstr(text, "\"answer\":\"Sales\""));
	/* The changed scale: two questions, each averaging its own answers. */
	g_assert_true(summary_has(json, "How did we do?", "1", "Average", 5));
	g_assert_true(summary_has(json, "How likely are you to recommend us?", "2", "Average", 9));
}

/* The publish action is the builder's button for every other door. */
static void
test_versions_action(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "action-form");
	g_autofree gchar *path = g_strdup_printf("/api/v1/form/%" G_GINT64_FORMAT "/actions/publish",
		venture_entity_get_id(form));
	Reply reply = { 0, NULL, NULL, NULL, NULL };
	g_autoptr(VentureEntity) stored = NULL;
	gint64 number = 0;
	(void)data;
	add_field(f, form, "company", "Company", VENTURE_FORM_FIELD_SHORT_TEXT, FALSE, 70);
	f->open = TRUE;
	start_http(f);
	request(f, path, "application/json", "{}", NULL, "application/json", &reply);
	g_assert_cmpuint(reply.status, ==, 200);
	reply_clear(&reply);
	stored = reread(f, form);
	g_object_get(stored, "published-number", &number, NULL);
	g_assert_cmpint(number, ==, 2);
}

/* ==========================================================================
 * Privacy
 * ========================================================================== */

/* Consent is the words it was given to; an unticked box records nothing,
 * so consent is never made up from a default. */
static void
test_consent_recorded(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "consent-form");
	g_autoptr(VentureEntity) consent = make_field(f, form, "news", "Send me the monthly newsletter",
		VENTURE_FORM_FIELD_CONSENT, FALSE, 70);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_SUBMISSION);
	g_autoptr(GPtrArray) rows = NULL;
	const gchar *const yes[] = { "name", "A", "email", "a@example.com", "topic", "sales", "news", "on", NULL };
	const gchar *const no[] = { "name", "B", "email", "b@example.com", "topic", "sales", NULL };
	g_autofree gchar *first = NULL, *second = NULL;
	(void)data;
	g_object_set(consent, "help", "Unsubscribe any time.", NULL);
	save(f, consent);
	g_object_unref(publish(f, form));
	g_assert_cmpint(submit_pairs(f, form, yes, NULL), ==, VENTURE_FORMS_ACCEPTED);
	g_assert_cmpint(submit_pairs(f, form, no, NULL), ==, VENTURE_FORMS_ACCEPTED);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(f->db, query, NULL);
	g_object_get(g_ptr_array_index(rows, 0), "answers", &first, NULL);
	g_object_get(g_ptr_array_index(rows, 1), "answers", &second, NULL);
	g_assert_nonnull(strstr(first, "\"news\":{\"given\":true,\"wording\":\"Send me the monthly newsletter\",\"detail\":\"Unsubscribe any time.\"}"));
	g_assert_null(strstr(second, "news"));
}

/* A sensitive answer stays on its record: not in the summary search and
 * webhooks read, not in the record's JSON (the API, the assistant), not
 * in the audit log, and it cannot be copied into a lead. The response's
 * own page shows it. */
static void
test_sensitive_kept_apart(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "sensitive-form");
	g_autoptr(VentureEntity) health = make_field(f, form, "health", "Anything we should know?",
		VENTURE_FORM_FIELD_LONG_TEXT, FALSE, 70);
	g_autoptr(VentureEntity) submission = NULL;
	g_autoptr(GHashTable) answers = venture_forms_answers_new();
	g_autoptr(JsonObject) errors = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(JsonNode) json = NULL;
	g_autoptr(VentureQuery) audits = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
	g_autoptr(GPtrArray) entries = NULL;
	g_autofree gchar *summary = NULL, *text = NULL, *page = NULL;
	VentureFormsOutcome outcome;
	guint i;
	(void)data;
	g_object_set(health, "sensitive", TRUE, NULL);
	save(f, health);
	g_object_unref(publish(f, form));
	venture_forms_answers_add(answers, "name", "Alice Private");
	venture_forms_answers_add(answers, "email", "alice@example.com");
	venture_forms_answers_add(answers, "topic", "sales");
	venture_forms_answers_add(answers, "health", "SECRET-CONDITION");
	g_assert_true(venture_forms_submit(f->db, form, answers, NULL, now, &outcome, &submission, &errors, &error));
	g_assert_no_error(error);
	g_assert_cmpint(outcome, ==, VENTURE_FORMS_ACCEPTED);
	g_object_get(submission, "summary", &summary, NULL);
	g_assert_null(strstr(summary, "SECRET-CONDITION"));
	g_assert_nonnull(strstr(summary, "Anything we should know?: (sensitive)"));
	json = venture_serializable_to_json(VENTURE_SERIALIZABLE(submission), FALSE);
	text = json_to_string(json, FALSE);
	g_assert_null(strstr(text, "SECRET-CONDITION"));
	page = venture_forms_render_answers(f->db, submission, &error);
	g_assert_no_error(error);
	g_assert_nonnull(strstr(page, "SECRET-CONDITION"));
	/* The audit log names it by number and holds none of what was sent. */
	venture_query_set_limit(audits, 0);
	entries = venture_database_find(f->db, audits, NULL);
	for (i = 0; i < entries->len; i++)
	{
		g_autofree gchar *type = NULL, *label = NULL, *diff = NULL;
		g_object_get(g_ptr_array_index(entries, i), "target-type", &type, "target-label", &label, "diff", &diff, NULL);
		if (g_strcmp0(type, "form_submission") != 0)
			continue;
		g_assert_true(g_str_has_prefix(label, "Form response #"));
		g_assert_true(diff == NULL || strstr(diff, "Alice") == NULL);
	}
	g_object_set(health, "maps-to", "notes", NULL);
	refuse(f, health, "sensitive");
}

/* The sweep anonymises what a form no longer keeps -- the count stays, the
 * answers go -- or purges it, and never more than it is allowed. */
static void
test_retention_sweep(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "retention-form");
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_SUBMISSION);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) later = g_date_time_add_days(now, 40);
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL, *answers = NULL, *name = NULL;
	const gchar *const pairs[] = { "name", "Alice", "email", "a@example.com", "topic", "sales", NULL };
	guint i;
	(void)data;
	for (i = 0; i < 3; i++)
		g_assert_cmpint(submit_pairs(f, form, pairs, NULL), ==, VENTURE_FORMS_ACCEPTED);
	g_object_set(form, "retention-days", (gint64)30, NULL);
	save(f, form);
	/* Nothing is old yet. */
	result = venture_forms_retention_sweep(f->db, f->org, 10, now, NULL, &error);
	g_assert_no_error(error);
	text = json_to_string(result, FALSE);
	g_assert_nonnull(strstr(text, "\"anonymised\":0"));
	g_clear_pointer(&result, json_node_unref);
	g_clear_pointer(&text, g_free);
	/* Forty days on, two at a time. */
	result = venture_forms_retention_sweep(f->db, f->org, 2, later, NULL, &error);
	g_assert_no_error(error);
	text = json_to_string(result, FALSE);
	g_assert_nonnull(strstr(text, "\"anonymised\":2"));
	g_assert_nonnull(strstr(text, "\"limit_reached\":true"));
	g_clear_pointer(&result, json_node_unref);
	result = venture_forms_retention_sweep(f->db, f->org, 10, later, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 3);
	rows = venture_database_find(f->db, query, NULL);
	g_object_get(g_ptr_array_index(rows, 0), "answers", &answers, "name", &name, NULL);
	g_assert_cmpstr(answers, ==, "{}");
	g_assert_null(strstr(name, "Alice"));
	/* Purging deletes. */
	g_object_set(form, "retention-action", "purge", NULL);
	save(f, form);
	g_clear_pointer(&result, json_node_unref);
	result = venture_forms_retention_sweep(f->db, f->org, 10, later, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 0);
}

/* ==========================================================================
 * After a submission
 * ========================================================================== */

/* A form told to create leads does so through the lead service, so its
 * duplicate rules apply, and the response points at the lead it made. */
static void
test_lead_mapping(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "lead-form");
	g_autoptr(VentureEntity) name = NULL;
	g_autoptr(VentureQuery) fields = venture_query_new(VENTURE_TYPE_FORM_FIELD);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_LEAD);
	g_autoptr(VentureEntity) lead = NULL;
	g_autoptr(VentureQuery) responses = venture_query_new(VENTURE_TYPE_FORM_SUBMISSION);
	g_autoptr(VentureEntity) response = NULL;
	g_autofree gchar *email = NULL, *source = NULL, *notes = NULL;
	const gchar *const first[] = { "name", "Alice", "email", "alice@example.com", "topic", "sales", "message", "Call me", NULL };
	const gchar *const again[] = { "name", "Alice", "email", "ALICE@example.com", "topic", "support", NULL };
	gint64 lead_id = 0;
	guint i;
	(void)data;
	g_object_set(form, "create-lead", TRUE, "lead-source", "Website form", NULL);
	save(f, form);
	venture_query_set_limit(fields, 0);
	rows = venture_database_find(f->db, fields, NULL);
	for (i = 0; i < rows->len; i++)
	{
		VentureEntity *field = g_ptr_array_index(rows, i);
		g_autofree gchar *key = NULL;
		g_object_get(field, "key", &key, NULL);
		if (g_strcmp0(key, "name") == 0 || g_strcmp0(key, "email") == 0 || g_strcmp0(key, "message") == 0)
		{
			g_object_set(field, "maps-to", g_strcmp0(key, "message") == 0 ? "notes" : key, NULL);
			save(f, field);
		}
	}
	g_object_unref(publish(f, form));
	g_assert_cmpint(submit_pairs(f, form, first, NULL), ==, VENTURE_FORMS_ACCEPTED);
	g_assert_cmpint(submit_pairs(f, form, again, NULL), ==, VENTURE_FORMS_ACCEPTED);
	/* Merged, not duplicated: the lead service's rule, not ours. */
	g_assert_cmpint(count(f, VENTURE_TYPE_LEAD), ==, 1);
	lead = venture_database_find_one(f->db, query, NULL);
	g_object_get(lead, "email", &email, "source", &source, "notes", &notes, NULL);
	g_assert_cmpstr(email, ==, "alice@example.com");
	g_assert_cmpstr(source, ==, "Website form");
	g_assert_nonnull(strstr(notes, "Call me"));
	response = venture_database_find_one(f->db, responses, NULL);
	g_object_get(response, "lead-id", &lead_id, NULL);
	g_assert_cmpint(lead_id, ==, venture_entity_get_id(lead));
	(void)name;
}

/* A response is saved through the ordinary path, so it is audited and the
 * rest of the system hears about it with no form code involved. */
static void
test_audited(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "audit-form");
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
	g_autoptr(GPtrArray) rows = NULL;
	const gchar *const pairs[] = { "name", "Alice", "email", "alice@example.com", "topic", "sales", NULL };
	gboolean found = FALSE;
	guint i;
	(void)data;
	g_assert_cmpint(submit_pairs(f, form, pairs, NULL), ==, VENTURE_FORMS_ACCEPTED);
	venture_query_set_limit(query, 0);
	rows = venture_database_find(f->db, query, NULL);
	for (i = 0; i < rows->len; i++)
	{
		g_autofree gchar *type = NULL;
		g_object_get(g_ptr_array_index(rows, i), "target-type", &type, NULL);
		if (g_strcmp0(type, "form_submission") == 0) found = TRUE;
	}
	g_assert_true(found);
}

/* The @column of the summary row whose answer is @answer. */
static gdouble
summary_value(JsonNode *json, const gchar *answer, const gchar *column)
{
	JsonArray *rows = json_object_get_array_member(json_node_get_object(json), "rows");
	guint i;
	for (i = 0; i < json_array_get_length(rows); i++)
	{
		JsonObject *row = json_array_get_object_element(rows, i);
		if (g_strcmp0(json_object_get_string_member_with_default(row, "answer", ""), answer) == 0)
			return json_object_get_double_member(row, column);
	}
	g_error("no summary row for %s", answer);
	return -1;
}

/* The survey summary counts every choice, including the ones nobody
 * picked, and averages scales; a relabelled choice keeps its count. */
static void
test_summary_report(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "summary-form");
	VentureReportRegistry *registry = venture_context_get_report_registry(f->context);
	g_autoptr(VentureDateRange) period = venture_date_range_new_all_time();
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(JsonNode) json = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *const a[] = { "name", "A", "email", "a@example.com", "topic", "sales", "score", "5", NULL };
	const gchar *const b[] = { "name", "B", "email", "b@example.com", "topic", "sales", "score", "3", NULL };
	const gchar *const c[] = { "name", "C", "email", "c@example.com", "topic", "support", NULL };
	(void)data;
	g_assert_cmpint(submit_pairs(f, form, a, NULL), ==, VENTURE_FORMS_ACCEPTED);
	g_assert_cmpint(submit_pairs(f, form, b, NULL), ==, VENTURE_FORMS_ACCEPTED);
	g_assert_cmpint(submit_pairs(f, form, c, NULL), ==, VENTURE_FORMS_ACCEPTED);
	json_object_set_int_member(options, "form_id", venture_entity_get_id(form));
	json_object_set_int_member(options, "organization_id", f->org);
	result = venture_report_generate(venture_report_registry_lookup(registry, "form_summary"),
		f->context, period, options, &error);
	g_assert_no_error(error);
	g_assert_nonnull(result);
	json = venture_report_result_to_json(result);
	g_assert_cmpfloat(summary_value(json, "Sales", "count"), ==, 2);
	g_assert_cmpfloat(summary_value(json, "Support", "count"), ==, 1);
	g_assert_cmpfloat(summary_value(json, "Something else", "count"), ==, 0);
	g_assert_cmpfloat(summary_value(json, "Average", "average"), ==, 4);
	/* No form, no report: a summary of every form at once means nothing. */
	g_clear_object(&result);
	g_clear_error(&error);
	json_object_remove_member(options, "form_id");
	result = venture_report_generate(venture_report_registry_lookup(registry, "form_summary"),
		f->context, period, options, &error);
	g_assert_null(result);
	g_assert_nonnull(error);
}

/* The module switch hides the whole door: with forms off, the public
 * address of a live form is a 404 like any other. */
static void
test_module_off(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "off-form");
	g_autofree gchar *ticket = old_ticket(form);
	g_autofree gchar *body = g_strdup_printf("%s&name=A&email=a%%40example.com&topic=sales", ticket);
	(void)data;
	venture_config_set_module_enabled(f->config, "forms", FALSE);
	start_http(f);
	g_assert_cmpuint(post_form(f, "/pub/form/off-form", body), ==, 404);
	venture_config_set_module_enabled(f->config, "forms", TRUE);
}

int
main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/forms/records", test_records);
	g_test_add_func("/forms/urlencoded-repeats", test_urlencoded_repeats);
	g_test_add_func("/forms/a11y-checker-catches", test_a11y_checker_catches);
	g_test_add("/forms/form-defaults", Fixture, NULL, setup, test_form_defaults, teardown);
	g_test_add("/forms/form-validation", Fixture, NULL, setup, test_form_validation, teardown);
	g_test_add("/forms/field-keys", Fixture, NULL, setup, test_field_keys, teardown);
	g_test_add("/forms/field-choices", Fixture, NULL, setup, test_field_choices, teardown);
	g_test_add("/forms/field-settings", Fixture, NULL, setup, test_field_settings, teardown);
	g_test_add("/forms/submission-guarded", Fixture, NULL, setup, test_submission_guarded, teardown);
	g_test_add("/forms/class-contract", Fixture, NULL, setup, test_class_contract, teardown);
	g_test_add("/forms/no-internal-config", Fixture, NULL, setup, test_no_internal_config, teardown);
	g_test_add("/forms/kind-validation", Fixture, NULL, setup, test_kind_validation, teardown);
	g_test_add("/forms/http-plain-post", Fixture, NULL, setup, test_http_plain_post, teardown);
	g_test_add("/forms/http-json", Fixture, NULL, setup, test_http_json, teardown);
	g_test_add("/forms/http-refusals", Fixture, NULL, setup, test_http_refusals, teardown);
	g_test_add("/forms/http-limits", Fixture, NULL, setup, test_http_limits, teardown);
	g_test_add("/forms/http-spam", Fixture, NULL, setup, test_http_spam, teardown);
	g_test_add("/forms/http-origins", Fixture, NULL, setup, test_http_origins, teardown);
	g_test_add("/forms/http-ways-in", Fixture, NULL, setup, test_http_ways_in, teardown);
	g_test_add("/forms/embed-codes", Fixture, NULL, setup, test_embed_codes, teardown);
	g_test_add("/forms/builder-page", Fixture, NULL, setup, test_builder_page, teardown);
	g_test_add("/forms/consent-recorded", Fixture, NULL, setup, test_consent_recorded, teardown);
	g_test_add("/forms/sensitive-kept-apart", Fixture, NULL, setup, test_sensitive_kept_apart, teardown);
	g_test_add("/forms/retention-sweep", Fixture, NULL, setup, test_retention_sweep, teardown);
	g_test_add("/forms/a11y-contract", Fixture, NULL, setup, test_a11y_contract, teardown);
	g_test_add("/forms/versions-freeze", Fixture, NULL, setup, test_versions_freeze, teardown);
	g_test_add("/forms/versions-publish-rules", Fixture, NULL, setup, test_versions_publish_rules, teardown);
	g_test_add("/forms/versions-read-only", Fixture, NULL, setup, test_versions_read_only, teardown);
	g_test_add("/forms/versions-rollback", Fixture, NULL, setup, test_versions_rollback, teardown);
	g_test_add("/forms/versions-mid-draft", Fixture, NULL, setup, test_versions_mid_draft, teardown);
	g_test_add("/forms/versions-side-by-side", Fixture, NULL, setup, test_versions_side_by_side, teardown);
	g_test_add("/forms/versions-action", Fixture, NULL, setup, test_versions_action, teardown);
	g_test_add("/forms/lead-mapping", Fixture, NULL, setup, test_lead_mapping, teardown);
	g_test_add("/forms/audited", Fixture, NULL, setup, test_audited, teardown);
	g_test_add("/forms/summary-report", Fixture, NULL, setup, test_summary_report, teardown);
	g_test_add("/forms/module-off", Fixture, NULL, setup, test_module_off, teardown);
	return g_test_run();
}
