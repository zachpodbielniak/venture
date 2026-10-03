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
#include "../src/forms/venture-forms-private.h"
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
	f->state_dir = g_dir_make_tmp("venture-forms-XXXXXX", NULL);
	g_object_set(f->config, "state-dir", f->state_dir, "server-port", (gint64)0,
		"server-bind-address", "127.0.0.1", "security-require-auth", !f->open, NULL);
	f->server = venture_web_server_new(f->context, &error);
	g_assert_no_error(error);
	g_assert_true(venture_web_server_start(f->server, &error));
	g_assert_no_error(error);
	f->port = venture_web_server_get_port(f->server);
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
		if (strlen(body) > VENTURE_FORMS_MAX_BODY) soup_message_headers_set_expectations(soup_message_get_request_headers(msg), SOUP_EXPECTATION_CONTINUE);
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

/* Draft, closed, unknown, scheduled, past its close date and over its cap: all
 * answer exactly the same 404, so the door says nothing about which. */
static void
test_http_refusals(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) draft = make_form(f, "draft-form", VENTURE_FORM_DRAFT);
	g_autoptr(VentureEntity) closed = make_form(f, "closed-form", VENTURE_FORM_CLOSED);
	g_autoptr(VentureEntity) capped = contact_form(f, "capped-form");
	g_autoptr(VentureEntity) scheduled = contact_form(f, "future-form");
	g_autoptr(VentureEntity) expired = make_form(f, "expired-form", VENTURE_FORM_LIVE);
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) past = g_date_time_add_hours(now, -1);
	g_autoptr(GDateTime) future = g_date_time_add_hours(now, 1);
	const gchar *paths[] = { "/pub/form/draft-form", "/pub/form/closed-form", "/pub/form/no-such-form",
		"/pub/form/expired-form", "/pub/form/capped-form", "/pub/form/future-form" };
	g_autofree gchar *reference = NULL;
	g_autofree gchar *ticket = old_ticket(capped);
	g_autofree gchar *body = g_strdup_printf("%s&name=A&email=a%%40example.com&topic=sales", ticket);
	guint i;
	(void)data;
	g_object_set(scheduled, "opens-at", future, NULL);
	save(f, scheduled);
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

static gchar *a11y_check(const gchar *html);
static void a11y_keep(const gchar *name, const gchar *html);

static gchar *
page_token(const gchar *html)
{
	const gchar *start = strstr(html, "name=\"_vf_draft\" value=\"");
	const gchar *end;
	g_autofree gchar *problems = a11y_check(html);
	static guint rendering = 0;
	g_autofree gchar *name = g_strdup_printf("multi-page-%u.html", rendering++);
	g_assert_cmpstr(problems, ==, "");
	a11y_keep(name, html);
	g_assert_nonnull(start);
	start += strlen("name=\"_vf_draft\" value=\"");
	end = strchr(start, '"');
	g_assert_nonnull(end);
	return g_strndup(start, (gsize)(end - start));
}

static void
page_saved(VentureDatabase *db, VentureEntity *entity, gboolean created, gpointer data)
{
	(void)db; (void)entity; (void)created;
	(*(guint *)data)++;
}

static void
page_audit(VentureDatabase *db, VentureEntity *entity, gpointer data)
{
	(void)db; (void)entity;
	(*(guint *)data)++;
}

/* An unfinished page is server-side state, never a submitted response. */
static void
test_multi_page(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "pages-form", VENTURE_FORM_LIVE);
	GEnumClass *kinds = g_type_class_ref(VENTURE_TYPE_FORM_FIELD_KIND);
	GEnumValue *page_break = g_enum_get_value_by_nick(kinds, "page_break");
	g_autofree gchar *ticket = NULL;
	g_autofree gchar *body = NULL, *token = NULL, *old = NULL;
	guint saves = 0, audits = 0;
	Reply reply = { 0, NULL, NULL, NULL, NULL };
	(void)data;
	g_assert_nonnull(page_break);
	add_field(f, form, "name", "Name", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 1);
	add_field(f, form, "page_two", "Contact", page_break->value, FALSE, 2);
	add_field(f, form, "email", "Email", VENTURE_FORM_FIELD_EMAIL, TRUE, 3);
	add_field(f, form, "page_three", "Confirmation", page_break->value, FALSE, 4);
	add_field(f, form, "agree", "I agree", VENTURE_FORM_FIELD_CONSENT, TRUE, 5);
	g_type_class_unref(kinds);
	g_object_unref(publish(f, form));
	start_http(f);
	request(f, "/pub/form/pages-form", NULL, NULL, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_nonnull(strstr(reply.body, "name=\"name\""));
	g_assert_null(strstr(reply.body, "name=\"email\""));
	g_assert_nonnull(strstr(reply.body, "vf-next"));
	reply_clear(&reply);
	ticket = old_ticket(form);
	/* An empty draft parameter cannot bypass the initial fill-time screen. */
	g_assert_cmpuint(post_form(f, "/pub/form/pages-form", "_vf_draft=&name=Robot"), ==, 404);
	g_signal_connect(f->db, "entity-saved", G_CALLBACK(page_saved), &saves);
	g_signal_connect(f->db, "audit", G_CALLBACK(page_audit), &audits);
	body = g_strdup_printf("%s&name=Alice", ticket);
	request(f, "/pub/form/pages-form", "application/x-www-form-urlencoded", body, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_nonnull(strstr(reply.body, "name=\"email\""));
	g_assert_nonnull(strstr(reply.body, "name=\"_vf_draft\""));
	g_assert_null(strstr(reply.body, "Alice"));
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 0);
	g_assert_cmpuint(saves, ==, 0);
	g_assert_cmpuint(audits, ==, 0);
	token = page_token(reply.body);
	reply_clear(&reply);
	old = g_strdup(token);
	old[strlen(old) - 1] = old[strlen(old) - 1] == 'a' ? 'b' : 'a';
	g_free(body); body = g_strdup_printf("_vf_draft=%s&email=alice%%40example.com", old);
	g_assert_cmpuint(post_form(f, "/pub/form/pages-form", body), ==, 404);
	/* The client may not overwrite earlier answers while posting this page. */
	g_free(body); body = g_strdup_printf("_vf_draft=%s&email=alice%%40example.com&name=Mallory", token);
	request(f, "/pub/form/pages-form", "application/x-www-form-urlencoded", body, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 422);
	g_free(token); token = page_token(reply.body);
	reply_clear(&reply);
	g_free(body); body = g_strdup_printf("_vf_draft=%s&email=alice%%40example.com", token);
	request(f, "/pub/form/pages-form", "application/x-www-form-urlencoded", body, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_nonnull(strstr(reply.body, "name=\"agree\""));
	g_free(token); token = page_token(reply.body);
	reply_clear(&reply);
	g_assert_cmpuint(post_form(f, "/pub/form/pages-form", body), ==, 404);
	g_free(body); body = g_strdup_printf("_vf_draft=%s&_vf_move=back", token);
	request(f, "/pub/form/pages-form", "application/x-www-form-urlencoded", body, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_nonnull(strstr(reply.body, "alice@example.com"));
	g_free(token); token = page_token(reply.body);
	reply_clear(&reply);
	g_free(body); body = g_strdup_printf("_vf_draft=%s&email=alice%%40example.com", token);
	request(f, "/pub/form/pages-form", "application/x-www-form-urlencoded", body, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 200);
	g_free(token); token = page_token(reply.body);
	reply_clear(&reply);
	g_assert_cmpuint(saves, ==, 0);
	g_assert_cmpuint(audits, ==, 0);
	g_free(body); body = g_strdup_printf("_vf_draft=%s&agree=on", token);
	g_assert_cmpuint(post_form(f, "/pub/form/pages-form", body), ==, 200);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 1);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_DRAFT_RECORD), ==, 0);
	g_assert_cmpuint(saves, >, 0);
	g_assert_cmpuint(audits, >, 0);
	g_assert_cmpuint(post_form(f, "/pub/form/pages-form", body), ==, 404);
	{
		g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_FORM_SUBMISSION);
		g_autoptr(VentureEntity) response = venture_database_find_one(f->db, q, NULL);
		g_autofree gchar *text = venture_forms_get_string(response, "answers");
		g_assert_nonnull(strstr(text, "Alice"));
		g_assert_null(strstr(text, "Mallory"));
	}
	g_signal_handlers_disconnect_by_data(f->db, &saves);
	g_signal_handlers_disconnect_by_data(f->db, &audits);
}

static void
test_draft_privacy(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "private-draft", VENTURE_FORM_LIVE);
	g_autoptr(GHashTable) answers = venture_forms_answers_new();
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) later = g_date_time_add_hours(now, 2);
	g_autoptr(VentureFormsStep) step = NULL, refused = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) exported = NULL, swept = NULL, erased = NULL;
	g_autofree gchar *ticket = NULL;
	(void)data;
	add_field(f, form, "email", "Email", VENTURE_FORM_FIELD_EMAIL, TRUE, 1);
	add_field(f, form, "next", "Next", VENTURE_FORM_FIELD_PAGE_BREAK, FALSE, 2);
	add_field(f, form, "name", "Name", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 3);
	g_object_unref(publish(f, form));
	{
		g_autoptr(GDateTime) issued = g_date_time_add_seconds(now, -60);
		ticket = venture_forms_ticket_new(form, issued);
	}
	venture_forms_answers_add(answers, "_vf_t", ticket);
	venture_forms_answers_add(answers, "email", "private@example.com");
	step = venture_forms_step(f->db, form, answers, NULL, now, &error);
	g_assert_no_error(error);
	g_assert_nonnull(step);
	exported = venture_forms_export_person(f->db, f->org, "private@example.com", &error);
	g_assert_no_error(error);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(exported), "drafts")), ==, 1);
	{
		g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_FORM_DRAFT_RECORD);
		g_autoptr(VentureEntity) draft = venture_database_find_one(f->db, q, NULL);
		g_autoptr(JsonNode) node = venture_serializable_to_json(VENTURE_SERIALIZABLE(draft), FALSE);
		g_autofree gchar *json = json_to_string(node, FALSE);
		g_assert_null(strstr(json, "private@example.com"));
	}
	g_hash_table_remove_all(answers);
	venture_forms_answers_add(answers, "_vf_draft", step->token);
	venture_forms_answers_add(answers, "name", "Alice");
	refused = venture_forms_step(f->db, form, answers, NULL, later, &error);
	g_assert_null(refused);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);
	/* A generic soft delete must not leave private draft bytes outside cleanup. */
	{
		g_autoptr(VentureQuery) q = venture_query_new(VENTURE_TYPE_FORM_DRAFT_RECORD);
		g_autoptr(VentureEntity) draft = venture_database_find_one(f->db, q, NULL);
		g_assert_true(venture_database_delete(f->db, draft, NULL, &error));
		g_assert_no_error(error);
	}
	swept = venture_forms_retention_sweep(f->db, f->org, 1, later, NULL, &error);
	g_assert_no_error(error);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(swept), "expired_drafts"), ==, 1);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_DRAFT_RECORD), ==, 0);
	g_hash_table_remove_all(answers);
	venture_forms_answers_add(answers, "_vf_t", ticket);
	venture_forms_answers_add(answers, "email", "private@example.com");
	g_clear_pointer(&step, venture_forms_step_free);
	step = venture_forms_step(f->db, form, answers, NULL, now, &error);
	g_assert_no_error(error);
	g_assert_nonnull(step);
	erased = venture_forms_erase_person(f->db, f->org, "private@example.com", NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(erased);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_DRAFT_RECORD), ==, 0);
}

/* Publishing a new questionnaire must not change the contract mid-fill. */
static void
test_draft_version(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "draft-version", VENTURE_FORM_LIVE);
	g_autoptr(VentureEntity) version = NULL, forged = VENTURE_ENTITY(venture_form_draft_new());
	g_autoptr(GHashTable) answers = venture_forms_answers_new();
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(VentureFormsStep) first = NULL, last = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *ticket = NULL;
	(void)data;
	add_field(f, form, "name", "Name", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 1);
	add_field(f, form, "next", "Next", VENTURE_FORM_FIELD_PAGE_BREAK, FALSE, 2);
	/* Empty pages would strand a visitor and are refused at publication. */
	version = venture_forms_publish(f->db, form, NULL, &error);
	g_assert_null(version);
	g_assert_nonnull(error);
	g_clear_error(&error);
	add_field(f, form, "email", "Email", VENTURE_FORM_FIELD_EMAIL, TRUE, 3);
	version = publish(f, form);
	{
		g_autoptr(GDateTime) issued = g_date_time_add_seconds(now, -60);
		ticket = venture_forms_ticket_new(form, issued);
	}
	venture_forms_answers_add(answers, "_vf_t", ticket);
	venture_forms_answers_add(answers, "name", "Alice");
	first = venture_forms_step(f->db, form, answers, NULL, now, &error);
	g_assert_no_error(error);
	g_assert_nonnull(first);
	add_field(f, form, "new_question", "New required question", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 4);
	g_object_unref(publish(f, form));
	g_hash_table_remove_all(answers);
	venture_forms_answers_add(answers, "_vf_draft", first->token);
	venture_forms_answers_add(answers, "email", "alice@example.com");
	last = venture_forms_step(f->db, form, answers, NULL, now, &error);
	g_assert_no_error(error);
	g_assert_nonnull(last);
	g_assert_true(last->complete);
	g_assert_cmpint(venture_entity_get_id(last->version), ==, venture_entity_get_id(version));
	venture_entity_set_organization_id(forged, f->org);
	g_object_set(forged, "name", "Forged draft", "form-id", venture_entity_get_id(form),
		"version-id", venture_entity_get_id(version), NULL);
	g_assert_false(venture_database_save(f->db, forged, NULL, &error));
	g_assert_nonnull(error);
	g_assert_nonnull(strstr(error->message, "public form service"));
}

/* Add/remove are private draft edits. They must work without script and
 * preserve the other row's answers when the first row is removed. */
static void
test_repeat_groups(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "repeat-form", VENTURE_FORM_LIVE);
	g_autoptr(VentureEntity) group = VENTURE_ENTITY(venture_form_group_new());
	g_autoptr(VentureEntity) name = NULL, email = NULL;
	g_autofree gchar *ticket = NULL, *body = NULL, *token = NULL;
	Reply reply = { 0, NULL, NULL, NULL, NULL };
	(void)data;
	venture_entity_set_organization_id(group, f->org);
	g_object_set(group, "label", "Attendee", "key", "attendee", "form-id", venture_entity_get_id(form),
		"min-rows", (gint64)1, "max-rows", (gint64)2, NULL);
	save(f, group);
	name = make_field(f, form, "name", "Name", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 1);
	email = make_field(f, form, "email", "Email", VENTURE_FORM_FIELD_EMAIL, TRUE, 2);
	g_object_set(name, "group-id", venture_entity_get_id(group), NULL); save(f, name);
	g_object_set(email, "group-id", venture_entity_get_id(group), NULL); save(f, email);
	g_object_unref(publish(f, form));
	start_http(f);
	request(f, "/pub/form/repeat-form", NULL, NULL, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_nonnull(strstr(reply.body, "name=\"attendee[0][name]\""));
	g_assert_nonnull(strstr(reply.body, "value=\"add:attendee\""));
	reply_clear(&reply);
	ticket = old_ticket(form);
	body = g_strdup_printf("%s&attendee[0][_row]=1&attendee[0][name]=Alice&_vf_move=add:attendee", ticket);
	request(f, "/pub/form/repeat-form", "application/x-www-form-urlencoded", body, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_nonnull(strstr(reply.body, "name=\"attendee[1][name]\""));
	g_assert_nonnull(strstr(reply.body, "value=\"Alice\""));
	token = page_token(reply.body); reply_clear(&reply);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 0);
	g_free(body); body = g_strdup_printf("_vf_draft=%s&attendee[0][_row]=1&attendee[0][name]=Alice&attendee[0][email]=alice%%40example.com&attendee[1][_row]=1&attendee[1][name]=Bob&attendee[1][email]=bob%%40example.com&_vf_move=add:attendee", token);
	request(f, "/pub/form/repeat-form", "application/x-www-form-urlencoded", body, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 422);
	g_free(token); token = page_token(reply.body); reply_clear(&reply);
	g_free(body); body = g_strdup_printf("_vf_draft=%s&attendee[0][_row]=1&attendee[0][name]=Alice&attendee[0][email]=alice%%40example.com&attendee[1][_row]=1&attendee[1][name]=Bob&attendee[1][email]=bob%%40example.com&_vf_move=remove:attendee:0", token);
	request(f, "/pub/form/repeat-form", "application/x-www-form-urlencoded", body, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_nonnull(strstr(reply.body, "value=\"Bob\""));
	g_assert_null(strstr(reply.body, "Alice"));
	g_assert_null(strstr(reply.body, "name=\"attendee[1][name]\""));
	g_free(token); token = page_token(reply.body); reply_clear(&reply);
	g_free(body); body = g_strdup_printf("_vf_draft=%s&attendee[0][_row]=1&attendee[0][name]=Bob", token);
	request(f, "/pub/form/repeat-form", "application/x-www-form-urlencoded", body, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 422);
	g_free(token); token = page_token(reply.body); reply_clear(&reply);
	g_free(body); body = g_strdup_printf("_vf_draft=%s&attendee[0][_row]=1&attendee[0][name]=Bob&attendee[0][email]=bob%%40example.com", token);
	g_assert_cmpuint(post_form(f, "/pub/form/repeat-form", body), ==, 200);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 1);
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_SUBMISSION);
		g_autoptr(VentureEntity) response = venture_database_find_one(f->db, query, NULL);
		g_autofree gchar *text = venture_forms_get_string(response, "answers");
		g_autoptr(JsonNode) answers = json_from_string(text, NULL);
		JsonArray *rows = json_object_get_array_member(json_node_get_object(answers), "attendee");
		g_assert_cmpuint(json_array_get_length(rows), ==, 1);
		g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(rows, 0), "name"), ==, "Bob");
		g_assert_null(strstr(text, "Alice"));
		g_assert_null(strstr(text, "_row"));
	}
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(VentureDateRange) period = venture_date_range_new_all_time();
		g_autoptr(JsonObject) options = json_object_new();
		g_autoptr(VentureReportResult) result = NULL;
		g_autoptr(JsonNode) json = NULL;
		JsonArray *rows;
		guint i, found = 0;
		json_object_set_int_member(options, "form_id", venture_entity_get_id(form));
		result = venture_report_generate(venture_report_registry_lookup(venture_context_get_report_registry(f->context), "form_summary"), f->context, period, options, &error);
		g_assert_no_error(error);
		json = venture_report_result_to_json(result);
		rows = json_object_get_array_member(json_node_get_object(json), "rows");
		for (i = 0; i < json_array_get_length(rows); i++)
		{
			JsonObject *row = json_array_get_object_element(rows, i);
			const gchar *answer = json_object_get_string_member(row, "answer");
			if (g_strcmp0(answer, "Rows") == 0 || g_strcmp0(answer, "Answered") == 0)
			{
				g_assert_cmpfloat(json_object_get_double_member(row, "count"), ==, 1);
				found++;
			}
		}
		g_assert_cmpuint(found, ==, 4);
	}

	{
		g_autoptr(GError) error = NULL;
		g_autoptr(JsonNode) exported = venture_forms_export_person(f->db, f->org, "BOB@example.com", &error);
		g_autoptr(JsonNode) erased = NULL;
		g_assert_no_error(error);
		g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(exported), "responses")), ==, 1);
		erased = venture_forms_erase_person(f->db, f->org, "bob@example.com", NULL, &error);
		g_assert_no_error(error);
		g_assert_nonnull(erased);
		g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 0);
	}

}

/* A zero-row group's first Add click may precede the fill-time floor.
 * That exception may create a draft, but must never admit final intake. */
static void
test_repeat_early_add(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "early-add", VENTURE_FORM_LIVE);
	g_autoptr(VentureEntity) group = VENTURE_ENTITY(venture_form_group_new());
	g_autoptr(VentureEntity) name = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *ticket = NULL, *body = NULL, *token = NULL;
	Reply reply = { 0, NULL, NULL, NULL, NULL };
	(void)data;
	g_object_set(form, "min-fill-seconds", (gint64)3600, NULL); save(f, form);
	venture_entity_set_organization_id(group, f->org);
	g_object_set(group, "label", "Attendee", "key", "attendee", "form-id", venture_entity_get_id(form), "min-rows", (gint64)0, "max-rows", (gint64)2, NULL); save(f, group);
	name = make_field(f, form, "name", "Name", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 1);
	g_object_set(name, "group-id", venture_entity_get_id(group), NULL); save(f, name);
	g_object_unref(publish(f, form)); start_http(f);
	ticket = venture_forms_ticket_new(form, now);
	body = g_strdup_printf("_vf_t=%s&_vf_move=add:attendee", ticket);
	request(f, "/pub/form/early-add", "application/x-www-form-urlencoded", body, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_nonnull(strstr(reply.body, "attendee[0][name]"));
	token = page_token(reply.body); reply_clear(&reply);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_DRAFT_RECORD), ==, 1);
	g_free(body); body = g_strdup_printf("_vf_draft=%s&attendee[0][_row]=1&attendee[0][name]=Robot", token);
	g_assert_cmpuint(post_form(f, "/pub/form/early-add", body), ==, 200);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 0);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_DRAFT_RECORD), ==, 0);
}

/* Both wire representations must feed identical validation. Ambiguous names
 * must never let object iteration order select the retained answer. */
static void
test_repeat_json(void)
{
	static const gchar *bad[] = {
		"{\"attendee\":[{\"name\":\"A\"}],\"attendee[0][name]\":\"B\"}",
		"{\"attendee[0][_row]\":\"1\",\"attendee\":[{\"name\":\"A\"}]}",
		"{\"attendee\":[{},\"bad\"]}",
		"{\"attendee\":[{\"child\":{\"name\":\"A\"}}]}",
		"{\"attendee\":[{\"child\":[{}]}]}",
		"{\"attendee\":[{\"_row\":\"forged\"}]}"
	};
	g_autoptr(JsonNode) root = json_from_string("{\"attendee\":[{\"name\":\"Alice\",\"choices\":[\"a\",\"b\"]},{}]}", NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(GHashTable) answers = venture_forms_answers_from_json(json_node_get_object(root), &error);
	GPtrArray *values;
	guint i;
	g_assert_no_error(error);
	g_assert_nonnull(answers);
	values = g_hash_table_lookup(answers, "attendee[0][name]");
	g_assert_cmpstr(g_ptr_array_index(values, 0), ==, "Alice");
	values = g_hash_table_lookup(answers, "attendee[0][choices]");
	g_assert_cmpuint(values->len, ==, 2);
	g_assert_true(g_hash_table_contains(answers, "attendee[1][_row]"));
	g_assert_true(json_object_has_member(json_node_get_object(root), "attendee"));
	for (i = 0; i < G_N_ELEMENTS(bad); i++)
	{
		g_autoptr(JsonNode) invalid = json_from_string(bad[i], NULL);
		g_autoptr(GHashTable) refused = venture_forms_answers_from_json(json_node_get_object(invalid), &error);
		g_assert_null(refused);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION);
		g_clear_error(&error);
	}
}

static void
test_rule_vocabulary(void)
{
	static const struct { const gchar *field; const gchar *op; const gchar *wanted; gboolean matches; } cases[] = {
		{ "text", "equals", "alpha beta", TRUE },
		{ "text", "equals", "Alpha beta", FALSE },
		{ "text", "contains", "beta", TRUE },
		{ "multi", "equals", "alpha", TRUE },
		{ "multi", "not_equals", "alpha", FALSE },
		{ "multi", "not_equals", "beta", TRUE },
		{ "multi", "any_of", "beta\ngamma", TRUE },
		{ "number", "greater_than", "11", TRUE },
		{ "number", "less_than", "12", FALSE },
		{ "bad_number", "greater_than", "0", FALSE },
		{ "empty", "is_empty", "", TRUE },
		{ "missing", "is_empty", "", TRUE },
		{ "multi", "is_empty", "", FALSE }
	};
	g_autoptr(JsonNode) values = json_from_string("{\"text\":\"alpha beta\",\"multi\":[\"alpha\",\"gamma\"],\"number\":\"12\",\"bad_number\":\"NaN\",\"empty\":[]}", NULL);
	guint i;
	for (i = 0; i < G_N_ELEMENTS(cases); i++)
	{
		g_autoptr(JsonObject) condition = json_object_new();
		json_object_set_string_member(condition, "field", cases[i].field);
		json_object_set_string_member(condition, "operator", cases[i].op);
		json_object_set_string_member(condition, "value", cases[i].wanted);
		g_assert_cmpint(venture_forms_condition_matches(condition, json_node_get_object(values)), ==, cases[i].matches);
	}
}

static VentureEntity *
last_form_response(Fixture *f)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_SUBMISSION);
	venture_query_set_organization(query, f->org);
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	return venture_database_find_one(f->db, query, NULL);
}

static VentureEntity *
make_rule(Fixture *f, VentureEntity *form, VentureFormRuleAction action,
	const gchar *target, const gchar *conditions)
{
	VentureEntity *rule = VENTURE_ENTITY(venture_form_rule_new());
	venture_entity_set_organization_id(rule, f->org);
	g_object_set(rule, "name", "Test rule", "form-id", venture_entity_get_id(form),
		"action", action, "target-key", target, "conditions", conditions, NULL);
	return rule;
}

/* A condition belongs to its own row. Forged hidden values are discarded,
 * and sensitive row answers remain available only to the owner export. */
static void
test_repeat_rules_privacy(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "row-rules", VENTURE_FORM_LIVE);
	g_autoptr(VentureEntity) group = VENTURE_ENTITY(venture_form_group_new());
	g_autoptr(VentureEntity) kind = NULL, email = NULL, detail = NULL, show = NULL, required = NULL, response = NULL;
	g_autoptr(JsonObject) errors = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *open = NULL, *secret = NULL, *html = NULL;
	g_autoptr(JsonNode) exported = NULL, erased = NULL;
	const gchar *conditions = "[{\"field\":\"kind\",\"operator\":\"equals\",\"value\":\"yes\"}]";
	const gchar *missing[] = { "attendee[0][kind]", "yes", "attendee[0][email]", "one@example.com", "attendee[1][kind]", "no", "attendee[1][email]", "two@example.com", "attendee[1][detail]", "forged", NULL };
	const gchar *valid[] = { "attendee[0][kind]", "yes", "attendee[0][email]", "one@example.com", "attendee[0][detail]", "private health answer", "attendee[1][kind]", "no", "attendee[1][email]", "two@example.com", "attendee[1][detail]", "forged", NULL };
	const gchar *sparse[] = { "attendee[1][kind]", "no", "attendee[1][email]", "two@example.com", NULL };
	const gchar *excess[] = { "attendee[0][kind]", "no", "attendee[1][kind]", "no", "attendee[2][kind]", "no", NULL };
	const gchar *empty[] = { NULL };
	(void)data;
	venture_entity_set_organization_id(group, f->org);
	g_object_set(group, "label", "Attendee", "key", "attendee", "form-id", venture_entity_get_id(form), "min-rows", (gint64)1, "max-rows", (gint64)2, NULL);
	save(f, group);
	kind = make_field(f, form, "kind", "Needs details", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 1);
	email = make_field(f, form, "email", "Address", VENTURE_FORM_FIELD_EMAIL, TRUE, 2);
	detail = make_field(f, form, "detail", "Private details", VENTURE_FORM_FIELD_LONG_TEXT, FALSE, 3);
	g_object_set(kind, "group-id", venture_entity_get_id(group), NULL); save(f, kind);
	g_object_set(email, "group-id", venture_entity_get_id(group), NULL); save(f, email);
	g_object_set(detail, "group-id", venture_entity_get_id(group), "sensitive", TRUE, NULL); save(f, detail);
	show = make_rule(f, form, VENTURE_FORM_RULE_SHOW, "detail", conditions); save(f, show);
	required = make_rule(f, form, VENTURE_FORM_RULE_REQUIRE, "detail", conditions); save(f, required);
	g_object_unref(publish(f, form));
	g_object_set(form, "unique-email-field", "email", NULL); refuse(f, form, "required email");
	g_object_set(form, "unique-email-field", NULL, "confirmation-field", "email", NULL); refuse(f, form, "non-repeated");
	g_object_set(form, "confirmation-field", NULL, NULL);

	g_assert_cmpint(submit_pairs(f, form, excess, &errors), ==, VENTURE_FORMS_INVALID);
	g_assert_true(json_object_has_member(errors, "attendee")); g_clear_pointer(&errors, json_object_unref);
	g_assert_cmpint(submit_pairs(f, form, empty, &errors), ==, VENTURE_FORMS_INVALID);
	g_assert_true(json_object_has_member(errors, "attendee")); g_clear_pointer(&errors, json_object_unref);
	g_assert_cmpint(submit_pairs(f, form, sparse, &errors), ==, VENTURE_FORMS_INVALID);
	g_assert_true(json_object_has_member(errors, "attendee")); g_clear_pointer(&errors, json_object_unref);
	g_assert_cmpint(submit_pairs(f, form, missing, &errors), ==, VENTURE_FORMS_INVALID);
	g_assert_true(json_object_has_member(errors, "attendee[0][detail]"));
	g_assert_false(json_object_has_member(errors, "attendee[1][detail]")); g_clear_pointer(&errors, json_object_unref);
	g_assert_cmpint(submit_pairs(f, form, valid, &errors), ==, VENTURE_FORMS_ACCEPTED);
	response = last_form_response(f);
	open = venture_forms_get_string(response, "answers");
	secret = venture_forms_get_string(response, "sensitive-answers");
	g_assert_null(strstr(open, "private health answer"));
	g_assert_null(strstr(open, "forged"));
	g_assert_nonnull(strstr(secret, "private health answer"));
	g_assert_null(strstr(secret, "forged"));
	html = venture_forms_render_answers(f->db, response, &error); g_assert_no_error(error);
	g_assert_nonnull(strstr(html, "one@example.com"));
	g_assert_nonnull(strstr(html, "two@example.com"));
	g_assert_null(strstr(html, "private health answer"));
	exported = venture_forms_export_person(f->db, f->org, "two@example.com", &error); g_assert_no_error(error);
	{
		g_autofree gchar *text = json_to_string(exported, FALSE);
		g_assert_nonnull(strstr(text, "private health answer"));
		g_assert_nonnull(strstr(text, "one@example.com"));
	}
	erased = venture_forms_erase_person(f->db, f->org, "two@example.com", NULL, &error); g_assert_no_error(error);
	g_assert_nonnull(erased);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 0);
}

/* A link identifies a contact, never a login. Editing a visible email may
 * change the answer but cannot move its binding or evade retained limits. */
static void
test_personal_links(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "personal-form", VENTURE_FORM_LIVE);
	g_autoptr(VentureEntity) other = make_form(f, "other-personal", VENTURE_FORM_LIVE);
	g_autoptr(VentureEntity) contact = VENTURE_ENTITY(venture_contact_new());
	g_autoptr(VentureEntity) name = NULL, email = NULL, version = NULL, found = NULL, response = NULL;
	g_autoptr(GDateTime) now = venture_time_now(), expires = NULL, later = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GHashTable) query = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	g_autoptr(JsonObject) values = NULL, restored = NULL, errors = NULL;
	g_autoptr(JsonNode) erased = NULL;
	g_autofree gchar *url = NULL, *url2 = NULL, *seed = NULL, *text = NULL;
	const gchar *token;
	const gchar *pairs[7];
	(void)data;
	venture_entity_set_organization_id(contact, f->org);
	g_object_set(contact, "name", "Known contact", "email", "known@example.com", "notes", "PRIVATE CRM NOTES", NULL); save(f, contact);
	name = make_field(f, form, "name", "Name", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 1);
	email = make_field(f, form, "email", "Email", VENTURE_FORM_FIELD_EMAIL, TRUE, 2);
	g_object_set(name, "allow-prefill", TRUE, "contact-field", "name", NULL); save(f, name);
	g_object_set(email, "contact-field", "email", NULL); save(f, email);
	{
		g_autoptr(VentureEntity) consent = make_field(f, form, "agree", "Agree", VENTURE_FORM_FIELD_CONSENT, FALSE, 3);
		g_object_set(consent, "allow-prefill", TRUE, NULL); refuse(f, consent, "cannot be prefilled");
		g_object_set(consent, "allow-prefill", FALSE, "contact-field", "notes", NULL); refuse(f, consent, "choose name");
		g_object_set(name, "sensitive", TRUE, NULL); refuse(f, name, "cannot be prefilled");
		g_object_set(name, "sensitive", FALSE, NULL);
	}

	g_object_set(form, "one-per-link", TRUE, NULL); save(f, form);
	version = publish(f, form);
	expires = g_date_time_add_days(now, 1); later = g_date_time_add_days(now, 2);
	url = venture_forms_personal_link(f->db, form, contact, "https://forms.example", expires, now, &error); g_assert_no_error(error);
	g_assert_nonnull(url); token = strstr(url, "personal=") + strlen("personal=");
	found = venture_forms_personal_contact(f->db, form, token, now, &error); g_assert_no_error(error);
	g_assert_cmpint(venture_entity_get_id(found), ==, venture_entity_get_id(contact)); g_clear_object(&found);
	found = venture_forms_personal_contact(f->db, other, token, now, &error); g_assert_null(found);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND); g_clear_error(&error);
	found = venture_forms_personal_contact(f->db, form, token, later, &error); g_assert_null(found);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND); g_clear_error(&error);
	{
		g_autofree gchar *tampered = g_strdup(token);
		tampered[strlen(tampered) - 1] = tampered[strlen(tampered) - 1] == 'a' ? 'b' : 'a';
		found = venture_forms_personal_contact(f->db, form, tampered, now, &error); g_assert_null(found);
		g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND); g_clear_error(&error);
	}
	g_hash_table_insert(query, g_strdup("name"), g_strdup("Query name")); g_hash_table_insert(query, g_strdup("email"), g_strdup("forged@example.com"));
	values = venture_forms_prefill_values(f->db, form, version, query, NULL, now, &error); g_assert_no_error(error);
	g_assert_cmpstr(json_object_get_string_member(values, "name"), ==, "Query name");
	g_assert_false(json_object_has_member(values, "email")); g_clear_pointer(&values, json_object_unref);
	values = venture_forms_prefill_values(f->db, form, version, query, token, now, &error); g_assert_no_error(error);
	g_assert_cmpstr(json_object_get_string_member(values, "name"), ==, "Known contact");
	g_assert_cmpstr(json_object_get_string_member(values, "email"), ==, "known@example.com");
	g_assert_false(json_object_has_member(values, "notes"));
	seed = venture_forms_prefill_pack(form, version, values);
	restored = venture_forms_prefill_unpack(form, version, seed, &error); g_assert_no_error(error);
	g_assert_cmpstr(json_object_get_string_member(restored, "name"), ==, "Known contact");
	pairs[0] = "name"; pairs[1] = "Edited name"; pairs[2] = "email"; pairs[3] = "changed@example.com";
	pairs[4] = VENTURE_FORMS_PERSONAL; pairs[5] = token; pairs[6] = NULL;
	g_assert_cmpint(submit_pairs(f, form, pairs, &errors), ==, VENTURE_FORMS_ACCEPTED);
	response = last_form_response(f);
	g_assert_cmpint(venture_forms_get_int(response, "contact-id"), ==, venture_entity_get_id(contact));
	text = venture_forms_get_string(response, "answers"); g_assert_nonnull(strstr(text, "changed@example.com"));
	g_assert_null(strstr(text, "personal"));
	g_assert_cmpint(submit_pairs(f, form, pairs, &errors), ==, VENTURE_FORMS_INVALID); g_clear_pointer(&errors, json_object_unref);
	url2 = venture_forms_personal_link(f->db, form, contact, "https://forms.example", expires, now, &error); g_assert_no_error(error);
	pairs[5] = strstr(url2, "personal=") + strlen("personal=");
	g_assert_cmpint(submit_pairs(f, form, pairs, &errors), ==, VENTURE_FORMS_ACCEPTED);
	g_object_set(form, "one-per-contact", TRUE, NULL); save(f, form);
	g_assert_cmpint(submit_pairs(f, form, pairs, &errors), ==, VENTURE_FORMS_INVALID); g_clear_pointer(&errors, json_object_unref);
	erased = venture_forms_erase_person(f->db, f->org, "known@example.com", NULL, &error); g_assert_no_error(error);
	g_assert_nonnull(erased); g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 0);
	g_assert_cmpint(submit_pairs(f, form, pairs, &errors), ==, VENTURE_FORMS_ACCEPTED);
	{
		g_autoptr(VentureEntity) organization = VENTURE_ENTITY(venture_organization_new());
		g_autoptr(VentureEntity) outsider = VENTURE_ENTITY(venture_contact_new());
		g_autofree gchar *wrong = NULL;
		g_object_set(organization, "name", "Other organization", NULL); save(f, organization);
		venture_entity_set_organization_id(outsider, venture_entity_get_id(organization));
		g_object_set(outsider, "name", "Other contact", "email", "other@example.com", NULL); save(f, outsider);
		wrong = venture_forms_personal_link(f->db, form, outsider, "https://forms.example", expires, now, &error);
		g_assert_null(wrong); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_VALIDATION); g_clear_error(&error);
	}
	g_assert_true(venture_database_delete(f->db, contact, NULL, &error)); g_assert_no_error(error);
	found = venture_forms_personal_contact(f->db, form, token, now, &error); g_assert_null(found);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);

}

/* Defaults for later pages travel in a signed seed, without creating an
 * anonymous GET-side draft or trusting extra fields in a page POST. */
static void
test_query_prefill_pages(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "query-prefill", VENTURE_FORM_LIVE);
	g_autoptr(VentureEntity) name = NULL, email = NULL, version = NULL;
	g_autoptr(GHashTable) query = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free), answers = venture_forms_answers_new();
	g_autoptr(GDateTime) now = venture_time_now(), issued = NULL;
	g_autoptr(JsonObject) values = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureFormsStep) step = NULL;
	g_autofree gchar *seed = NULL, *ticket = NULL, *html = NULL;
	VentureFormsRender options;
	Reply reply = { 0, NULL, NULL, NULL, NULL };
	(void)data;
	name = make_field(f, form, "name", "Name", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 1);
	add_field(f, form, "next", "Next", VENTURE_FORM_FIELD_PAGE_BREAK, FALSE, 2);
	email = make_field(f, form, "email", "Email", VENTURE_FORM_FIELD_EMAIL, TRUE, 3);
	g_object_set(name, "allow-prefill", TRUE, NULL); save(f, name);
	g_object_set(email, "allow-prefill", TRUE, NULL); save(f, email);
	version = publish(f, form); start_http(f);
	request(f, "/pub/form/query-prefill?name=Alex&email=later%40example.com&notes=FORGED", NULL, NULL, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 200); g_assert_nonnull(strstr(reply.body, "value=\"Alex\""));
	g_assert_null(strstr(reply.body, "FORGED")); g_assert_nonnull(strstr(reply.body, "name=\"_vf_prefill\"")); reply_clear(&reply);
	request(f, "/pub/form/query-prefill", "application/x-www-form-urlencoded", "_vf_personal=invalid&name=Alex", NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 404); reply_clear(&reply);

	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_DRAFT_RECORD), ==, 0);
	g_hash_table_insert(query, g_strdup("email"), g_strdup("not an address"));
	values = venture_forms_prefill_values(f->db, form, version, query, NULL, now, &error); g_assert_no_error(error);
	g_assert_false(json_object_has_member(values, "email")); g_clear_pointer(&values, json_object_unref);

	g_hash_table_insert(query, g_strdup("email"), g_strdup("later@example.com"));
	values = venture_forms_prefill_values(f->db, form, version, query, NULL, now, &error); g_assert_no_error(error);
	seed = venture_forms_prefill_pack(form, version, values);
	issued = g_date_time_add_seconds(now, -60); ticket = venture_forms_ticket_new(form, issued);
	venture_forms_answers_add(answers, VENTURE_FORMS_TICKET, ticket);
	venture_forms_answers_add(answers, VENTURE_FORMS_PREFILL, seed);
	venture_forms_answers_add(answers, "name", "Edited Alex");
	step = venture_forms_step(f->db, form, answers, NULL, now, &error); g_assert_no_error(error);
	g_assert_nonnull(step); g_assert_cmpuint(step->page, ==, 1);
	g_assert_cmpstr(json_object_get_string_member(step->values, "email"), ==, "later@example.com");
	options.mode = VENTURE_FORMS_RENDER_FRAGMENT; options.action = NULL; options.ticket = step->ticket;
	options.values = step->values; options.errors = step->errors; options.version = step->version;
	html = venture_forms_render_step(f->db, form, &options, step, &error); g_assert_no_error(error);
	g_assert_nonnull(strstr(html, "value=\"later@example.com\""));
	g_clear_pointer(&step, venture_forms_step_free); g_hash_table_remove_all(answers);
	seed[strlen(seed)-1] = seed[strlen(seed)-1] == 'a' ? 'b' : 'a';
	venture_forms_answers_add(answers, VENTURE_FORMS_TICKET, ticket);
	venture_forms_answers_add(answers, VENTURE_FORMS_PREFILL, seed);
	venture_forms_answers_add(answers, "name", "Alex");
	step = venture_forms_step(f->db, form, answers, NULL, now, &error); g_assert_null(step);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
}

/* Hidden answers cannot influence either required branches or saved data;
 * navigation and conditions are frozen with the questions they govern. */
static void
test_rules(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "rules-form", VENTURE_FORM_LIVE);
	g_autoptr(VentureEntity) show = NULL, require = NULL, jump = NULL, end = NULL, bad = NULL, combined = NULL;
	g_autoptr(VentureEntity) response = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) schema = NULL;
	g_autoptr(GHashTable) answers = venture_forms_answers_new();
	g_autoptr(VentureFormsStep) first = NULL, back = NULL, again = NULL, done = NULL;
	g_autofree gchar *ticket = NULL;
	const gchar *other = "[{\"field\":\"kind\",\"operator\":\"equals\",\"value\":\"other\"}]";
	const gchar *business = "[{\"field\":\"kind\",\"operator\":\"equals\",\"value\":\"business\"}]";
	const gchar *const hidden[] = { "kind", "business", "detail", "Discard this", "personal", "Forged skipped answer", "company", "Example", NULL };
	const gchar *const missing[] = { "kind", "person", "company", "Cannot skip personal", NULL };
	const gchar *const person[] = { "kind", "person", "personal", "done", NULL };
	const gchar *const required[] = { "kind", "other", NULL };
	const gchar *const ended[] = { "kind", "other", "detail", "Explained", "company", "Discard this too", NULL };
	(void)data;
	add_field(f, form, "kind", "Kind", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 1);
	add_field(f, form, "detail", "Details", VENTURE_FORM_FIELD_SHORT_TEXT, FALSE, 2);
	add_field(f, form, "personal_page", "Personal", VENTURE_FORM_FIELD_PAGE_BREAK, FALSE, 3);
	add_field(f, form, "personal", "Personal answer", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 4);
	add_field(f, form, "business_page", "Business", VENTURE_FORM_FIELD_PAGE_BREAK, FALSE, 5);
	add_field(f, form, "company", "Company", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 6);
	show = make_rule(f, form, VENTURE_FORM_RULE_SHOW, "detail", other); save(f, show);
	require = make_rule(f, form, VENTURE_FORM_RULE_REQUIRE, "detail", other); save(f, require);
	jump = make_rule(f, form, VENTURE_FORM_RULE_JUMP, "business_page", business); save(f, jump);
	end = make_rule(f, form, VENTURE_FORM_RULE_END, "", other); save(f, end);
	combined = make_rule(f, form, VENTURE_FORM_RULE_END, "",
		"[{\"field\":\"kind\",\"operator\":\"equals\",\"value\":\"person\"},{\"field\":\"personal\",\"operator\":\"equals\",\"value\":\"done\"}]");
	save(f, combined);
	bad = make_rule(f, form, VENTURE_FORM_RULE_SHOW, "missing", other); refuse(f, bad, "Rule");
	g_object_set(bad, "target-key", "kind", NULL); refuse(f, bad, "Rule");
	g_object_set(bad, "target-key", "detail", "conditions", "[{\"field\":\"kind\",\"operator\":\"eval\",\"value\":\"1\"}]", NULL);
	refuse(f, bad, "Rule");
	g_object_unref(publish(f, form));
	schema = venture_forms_schema(f->db, form, "/pub/form/rules-form", now, &error);
	g_assert_no_error(error);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(schema), "rules")), ==, 5);
	g_assert_cmpint(submit_pairs(f, form, hidden, NULL), ==, VENTURE_FORMS_ACCEPTED);
	response = last_form_response(f);
	{
		g_autofree gchar *stored = venture_forms_get_string(response, "answers");
		g_autofree gchar *omitted = venture_forms_get_string(response, "not-shown");
		g_assert_null(strstr(stored, "Discard"));
		g_assert_null(strstr(stored, "Forged"));
		g_assert_nonnull(strstr(omitted, "personal"));
	}
	g_clear_object(&response);
	g_assert_cmpint(submit_pairs(f, form, missing, NULL), ==, VENTURE_FORMS_INVALID);
	g_assert_cmpint(submit_pairs(f, form, required, NULL), ==, VENTURE_FORMS_INVALID);
	g_assert_cmpint(submit_pairs(f, form, ended, NULL), ==, VENTURE_FORMS_ACCEPTED);
	response = last_form_response(f);
	{
		g_autofree gchar *stored = venture_forms_get_string(response, "answers");
		g_assert_nonnull(strstr(stored, "Explained"));
		g_assert_null(strstr(stored, "Discard"));
	}
	g_assert_cmpint(submit_pairs(f, form, person, NULL), ==, VENTURE_FORMS_ACCEPTED);
	/* Unpublished rule edits cannot change the public branch. */
	g_object_set(jump, "conditions", other, NULL); save(f, jump);
	{
		g_autoptr(GDateTime) issued = g_date_time_add_seconds(now, -60);
		ticket = venture_forms_ticket_new(form, issued);
	}
	venture_forms_answers_add(answers, "_vf_t", ticket);
	venture_forms_answers_add(answers, "kind", "business");
	first = venture_forms_step(f->db, form, answers, NULL, now, &error);
	g_assert_no_error(error); g_assert_nonnull(first);
	g_assert_cmpuint(first->page, ==, 2);
	g_hash_table_remove_all(answers);
	venture_forms_answers_add(answers, "_vf_draft", first->token);
	venture_forms_answers_add(answers, "_vf_move", "back");
	back = venture_forms_step(f->db, form, answers, NULL, now, &error);
	g_assert_no_error(error); g_assert_nonnull(back);
	g_assert_cmpuint(back->page, ==, 0);
	g_hash_table_remove_all(answers);
	venture_forms_answers_add(answers, "_vf_draft", back->token);
	venture_forms_answers_add(answers, "kind", "business");
	again = venture_forms_step(f->db, form, answers, NULL, now, &error);
	g_assert_no_error(error); g_assert_nonnull(again);
	g_assert_cmpuint(again->page, ==, 2);
	g_hash_table_remove_all(answers);
	venture_forms_answers_add(answers, "_vf_draft", again->token);
	venture_forms_answers_add(answers, "company", "Example");
	done = venture_forms_step(f->db, form, answers, NULL, now, &error);
	g_assert_no_error(error); g_assert_nonnull(done);
	g_assert_true(done->complete);
	{
		g_autoptr(VentureDateRange) period = venture_date_range_new_all_time();
		g_autoptr(JsonObject) options = json_object_new();
		g_autoptr(VentureReportResult) result = NULL;
		g_autoptr(JsonNode) json = NULL;
		JsonArray *rows;
		guint i;
		gboolean found = FALSE;
		json_object_set_int_member(options, "form_id", venture_entity_get_id(form));
		json_object_set_int_member(options, "organization_id", f->org);
		result = venture_report_generate(venture_report_registry_lookup(venture_context_get_report_registry(f->context), "form_summary"),
			f->context, period, options, &error);
		g_assert_no_error(error);
		json = venture_report_result_to_json(result);
		rows = json_object_get_array_member(json_node_get_object(json), "rows");
		for (i = 0; i < json_array_get_length(rows); i++)
		{
			JsonObject *row = json_array_get_object_element(rows, i);
			if (g_strcmp0(json_object_get_string_member(row, "question"), "Personal answer") == 0 &&
			    g_strcmp0(json_object_get_string_member(row, "answer"), "Not shown") == 0)
			{
				g_assert_cmpfloat(json_object_get_double_member(row, "count"), ==, 3);
				found = TRUE;
			}
		}
		g_assert_true(found);
	}
}

/* Enabling the email limit counts responses received before it was enabled,
 * and compares addresses without case. Refused responses trigger no follow-up. */
static void
test_unique_email(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "unique-email");
	g_autoptr(VentureEntity) response = NULL;
	g_autoptr(GHashTable) answers = venture_forms_answers_new();
	g_autoptr(JsonObject) errors = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	VentureFormsOutcome outcome;
	(void)data;
	venture_forms_answers_add(answers, "name", "Alice");
	venture_forms_answers_add(answers, "email", "Alice@example.com");
	venture_forms_answers_add(answers, "topic", "sales");
	g_assert_true(venture_forms_submit(f->db, form, answers, NULL, now, &outcome, &response, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(outcome, ==, VENTURE_FORMS_ACCEPTED);
	g_clear_object(&response);
	g_object_set(form, "unique-email-field", "email", NULL);
	save(f, form);
	g_hash_table_remove(answers, "email");
	venture_forms_answers_add(answers, "email", "alice@EXAMPLE.com");
	g_assert_true(venture_forms_submit(f->db, form, answers, NULL, now, &outcome, &response, &errors, &error));
	g_assert_no_error(error);
	g_assert_cmpint(outcome, ==, VENTURE_FORMS_INVALID);
	g_assert_null(response);
	g_assert_true(json_object_has_member(errors, "email"));
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 1);
	g_clear_pointer(&errors, json_object_unref);
	g_hash_table_remove(answers, "email");
	venture_forms_answers_add(answers, "email", "bob@example.com");
	g_assert_true(venture_forms_submit(f->db, form, answers, NULL, now, &outcome, &response, &errors, &error));
	g_assert_no_error(error);
	g_assert_cmpint(outcome, ==, VENTURE_FORMS_ACCEPTED);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 2);
	g_object_set(form, "unique-email-field", "name", NULL);
	refuse(f, form, "One response per email");
}

/* Sensitive email answers enforce the same limit, and erasure removes the
 * identity used for that limit rather than leaving a hidden deny-list. */
static void
test_unique_email_sensitive(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "private-email", VENTURE_FORM_LIVE);
	g_autoptr(VentureEntity) email = make_field(f, form, "private_email", "Email", VENTURE_FORM_FIELD_EMAIL, TRUE, 1);
	g_autoptr(JsonNode) erased = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *const answers[] = { "private_email", "alice@example.com", NULL };
	(void)data;
	g_object_set(email, "sensitive", TRUE, NULL);
	save(f, email);
	g_object_unref(publish(f, form));
	g_object_set(form, "unique-email-field", "private_email", NULL);
	save(f, form);
	g_assert_cmpint(submit_pairs(f, form, answers, NULL), ==, VENTURE_FORMS_ACCEPTED);
	g_assert_cmpint(submit_pairs(f, form, answers, NULL), ==, VENTURE_FORMS_INVALID);
	erased = venture_forms_erase_person(f->db, f->org, "alice@example.com", NULL, &error);
	g_assert_no_error(error);
	g_assert_nonnull(erased);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 0);
	g_assert_cmpint(submit_pairs(f, form, answers, NULL), ==, VENTURE_FORMS_ACCEPTED);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 1);
}

/* The same instant is judged at the public door and inside the save lock;
 * a cached form object cannot bypass a changed opening or closing time. */
static void
test_schedule(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "scheduled-form");
	g_autoptr(VentureEntity) stale = reread(f, form);
	g_autoptr(VentureEntity) found = NULL;
	g_autoptr(VentureEntity) submission = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) later = g_date_time_add_hours(now, 1);
	g_autoptr(GDateTime) end = g_date_time_add_hours(now, 2);
	g_autoptr(GHashTable) answers = venture_forms_answers_new();
	g_autoptr(GError) error = NULL;
	VentureFormsOutcome outcome;
	(void)data;
	venture_forms_answers_add(answers, "name", "Alice");
	venture_forms_answers_add(answers, "email", "alice@example.com");
	venture_forms_answers_add(answers, "topic", "sales");
	g_object_set(form, "opens-at", later, "closes-at", end, NULL);
	save(f, form);
	found = venture_forms_find_live(f->db, "scheduled-form", now, &error);
	g_assert_null(found);
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);
	g_assert_false(venture_forms_submit(f->db, stale, answers, NULL, now,
		&outcome, &submission, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_clear_error(&error);
	found = venture_forms_find_live(f->db, "scheduled-form", later, &error);
	g_assert_no_error(error);
	g_assert_nonnull(found);
	g_assert_true(venture_forms_submit(f->db, stale, answers, NULL, later,
		&outcome, &submission, NULL, &error));
	g_assert_no_error(error);
	g_assert_cmpint(outcome, ==, VENTURE_FORMS_ACCEPTED);
	g_clear_object(&submission);
	g_assert_false(venture_forms_submit(f->db, stale, answers, NULL, end,
		&outcome, &submission, NULL, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 1);
	g_object_set(form, "opens-at", end, "closes-at", later, NULL);
	refuse(f, form, "Closes at");
}

/* Two requests are in flight for the last slot before either callback runs. */
static void
test_last_slot(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "last-slot");
	g_autofree gchar *ticket = old_ticket(form);
	g_autofree gchar *body = g_strdup_printf("%s&name=Alice&email=alice%%40example.com&topic=sales", ticket);
	g_autofree gchar *uri = NULL;
	g_autoptr(SoupMessage) first = NULL;
	g_autoptr(SoupMessage) second = NULL;
	g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));
	Pending a = { FALSE, NULL, NULL }, b = { FALSE, NULL, NULL };
	gint64 deadline;
	guint sa, sb;
	(void)data;
	g_object_set(form, "response-limit", (gint64)1, NULL);
	save(f, form);
	start_http(f);
	uri = g_strdup_printf("http://127.0.0.1:%u/pub/form/last-slot", f->port);
	first = soup_message_new("POST", uri);
	second = soup_message_new("POST", uri);
	soup_message_set_request_body_from_bytes(first, "application/x-www-form-urlencoded", bytes);
	soup_message_set_request_body_from_bytes(second, "application/x-www-form-urlencoded", bytes);
	soup_session_send_and_read_async(f->session, first, G_PRIORITY_DEFAULT, NULL, received, &a);
	soup_session_send_and_read_async(f->session, second, G_PRIORITY_DEFAULT, NULL, received, &b);
	deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
	while ((!a.done || !b.done) && g_get_monotonic_time() < deadline)
	{
		g_main_context_iteration(NULL, FALSE);
		g_usleep(1000);
	}
	g_assert_true(a.done && b.done);
	g_assert_no_error(a.error);
	g_assert_no_error(b.error);
	sa = soup_message_get_status(first);
	sb = soup_message_get_status(second);
	g_assert_true((sa == 200 && sb == 404) || (sa == 404 && sb == 200));
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 1);
	g_bytes_unref(a.body);
	g_bytes_unref(b.body);
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
	g_object_set(form, "public-origin", "https://forms.example.test", NULL); save(f, form);
	for (i = 0; i < klass->n_values; i++)
	{
		GEnumValue *value = &klass->values[i];
		/* Structural page breaks are exercised by the multi-page walkthrough. */
		if (value->value == VENTURE_FORM_FIELD_PAGE_BREAK)
			continue;
		g_autofree gchar *key = g_strdup(value->value_nick);
		g_autoptr(VentureEntity) field = make_field(f, form, key, value->value_nick,
			(VentureFormFieldKind)value->value, i % 2 == 0, (gint64)i);
		if (value->value == VENTURE_FORM_FIELD_BOOKING)
		{
			g_autoptr(VentureEntity) page = g_object_new(VENTURE_TYPE_BOOKING_PAGE, "organization-id", f->org,
				"title", "Accessible slots", "slug", token, "owner", "owner", "duration-minutes", (gint64)30,
				"timezone", "UTC", "horizon-days", (gint64)2, "active", TRUE,
				"availability", "{\"mon\":\"09:00-17:00\",\"tue\":\"09:00-17:00\",\"wed\":\"09:00-17:00\",\"thu\":\"09:00-17:00\",\"fri\":\"09:00-17:00\",\"sat\":\"09:00-17:00\",\"sun\":\"09:00-17:00\"}", NULL);
			save(f, page);
			g_object_set(field, "booking-page-id", venture_entity_get_id(page), "booking-name-field", "short_text", NULL);
		}
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

/* Old drafts and frozen versions could contain a default-checked consent
 * box. A browser must require a person's choice, not submit that default. */
static void
test_consent_no_default(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "consent-default", VENTURE_FORM_LIVE);
	g_autoptr(VentureEntity) field = make_field(f, form, "permission", "Send news",
		VENTURE_FORM_FIELD_CONSENT, FALSE, 10);
	g_autoptr(GError) error = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(JsonNode) schema = NULL;
	g_autofree gchar *html = NULL;
	JsonObject *question;
	(void)data;

	save(f, field);
	g_object_unref(publish(f, form));
	/* Represent a version written before the default-consent guard. */
	g_assert_true(venture_database_execute(f->db,
		"UPDATE form_fields SET default_value = 'on';"
		"UPDATE form_versions SET definition = replace(definition, "
		"'\"kind\":\"consent\"', '\"kind\":\"consent\",\"default_value\":\"on\"')", NULL, &error));
	g_assert_no_error(error);
	html = render(f, form, VENTURE_FORMS_RENDER_FRAGMENT);
	g_assert_null(strstr(html, " checked"));
	schema = venture_forms_schema(f->db, form, "/pub/form/consent-default", now, &error);
	g_assert_no_error(error);
	question = json_array_get_object_element(json_object_get_array_member(json_node_get_object(schema), "fields"), 0);
	g_assert_cmpstr(json_object_get_string_member(question, "default"), ==, "");
	g_object_set(field, "default-value", "true", NULL);
	refuse(f, field, "consent");
}

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

/* General consent is not marketing permission. Only an explicitly mapped,
 * checked question may create evidence, using the published wording. */
static void
test_marketing_consent(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "marketing-form", VENTURE_FORM_LIVE);
	g_autoptr(VentureEntity) name = make_field(f, form, "name", "Name", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 10);
	g_autoptr(VentureEntity) email = make_field(f, form, "email", "Email", VENTURE_FORM_FIELD_EMAIL, TRUE, 20);
	g_autoptr(VentureEntity) news = make_field(f, form, "news", "Send the newsletter", VENTURE_FORM_FIELD_CONSENT, FALSE, 30);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MARKETING_CONSENT);
	g_autoptr(GPtrArray) rows = NULL;
	g_autofree gchar *evidence = NULL;
	g_autoptr(GDateTime) recorded = NULL;
	const gchar *const yes[] = { "name", "Alice", "email", "alice@example.com", "news", "on", NULL };
	const gchar *const no[] = { "name", "Bob", "email", "bob@example.com", NULL };
	(void)data;
	g_object_set(form, "create-lead", TRUE, NULL);
	save(f, form);
	g_object_set(name, "maps-to", "name", NULL);
	g_object_set(email, "maps-to", "email", NULL);
	save(f, name);
	save(f, email);
	save(f, news);
	g_object_unref(publish(f, form));
	g_assert_cmpint(submit_pairs(f, form, yes, NULL), ==, VENTURE_FORMS_ACCEPTED);
	g_assert_cmpint(count(f, VENTURE_TYPE_MARKETING_CONSENT), ==, 0);
	g_object_set(news, "marketing-consent", TRUE, "help", "You can unsubscribe.", NULL);
	save(f, news);
	g_object_unref(publish(f, form));
	g_object_set(news, "label", "Unpublished wording", NULL);
	save(f, news);
	g_assert_cmpint(submit_pairs(f, form, no, NULL), ==, VENTURE_FORMS_ACCEPTED);
	g_assert_cmpint(count(f, VENTURE_TYPE_MARKETING_CONSENT), ==, 0);
	g_assert_cmpint(submit_pairs(f, form, yes, NULL), ==, VENTURE_FORMS_ACCEPTED);
	rows = venture_database_find(f->db, query, NULL);
	g_assert_cmpuint(rows->len, ==, 1);
	g_object_get(g_ptr_array_index(rows, 0), "evidence", &evidence, "recorded-at", &recorded, NULL);
	g_assert_nonnull(strstr(evidence, "Send the newsletter"));
	g_assert_nonnull(strstr(evidence, "You can unsubscribe."));
	g_assert_nonnull(strstr(evidence, "version 2"));
	g_assert_null(strstr(evidence, "Unpublished wording"));
	g_assert_nonnull(recorded);
	/* Suppression wins over another checked box; the response survives
	 * the failed follow-up without manufacturing permission. */
	{
		g_autoptr(VentureEntity) suppression = g_object_new(VENTURE_TYPE_SUPPRESSION,
			"organization-id", f->org, "email", "alice@example.com", NULL);
		g_autoptr(VentureQuery) responses = venture_query_new(VENTURE_TYPE_FORM_SUBMISSION);
		g_autoptr(VentureEntity) response = NULL;
		g_autofree gchar *note = NULL;
		gint64 lead_id = -1;

		save(f, suppression);
		g_assert_cmpint(submit_pairs(f, form, yes, NULL), ==, VENTURE_FORMS_ACCEPTED);
		g_assert_cmpint(count(f, VENTURE_TYPE_MARKETING_CONSENT), ==, 1);
		venture_query_add_order(responses, "id", VENTURE_SORT_DESCENDING, NULL);
		response = venture_database_find_one(f->db, responses, NULL);
		g_object_get(response, "mapping-note", &note, "lead-id", &lead_id, NULL);
		g_assert_nonnull(strstr(note, "suppression"));
		g_assert_cmpint(lead_id, ==, 0);
	}
	g_object_set(news, "sensitive", TRUE, NULL);
	refuse(f, news, "marketing");
}

/* A sensitive answer stays on its record: not in the summary search reads,
 * not in the record's JSON -- which is exactly what the API, the
 * assistant's record tools and an outbound webhook's data carry
 * (venture-webhook.c serialises the record with the same call) -- and not
 * in the audit log, whose label and diff are what inbox notifications are
 * built from. It cannot be copied into a lead or disclosed by the response page. */
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
	g_assert_null(strstr(page, "SECRET-CONDITION"));
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

/* Erasure deletes every response carrying the address, sensitive answers
 * included, leaves others alone, and leaves one audit entry that says it
 * happened without saying whose. */
static void
test_erase_person(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "erase-form");
	g_autoptr(VentureEntity) note = make_field(f, form, "contact_again", "Where else can we reach you?",
		VENTURE_FORM_FIELD_SHORT_TEXT, FALSE, 70);
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(VentureQuery) audits = venture_query_new(VENTURE_TYPE_AUDIT_ENTRY);
	g_autoptr(GPtrArray) entries = NULL;
	g_autofree gchar *text = NULL;
	const gchar *const hers[] = { "name", "Alice", "email", "Alice@Example.com", "topic", "sales", NULL };
	const gchar *const hidden[] = { "name", "A2", "email", "other@example.com", "topic", "sales", "contact_again", "alice@example.com", NULL };
	const gchar *const his[] = { "name", "Bob", "email", "bob@example.com", "topic", "sales", NULL };
	gboolean recorded = FALSE;
	guint i;
	(void)data;
	g_object_set(note, "sensitive", TRUE, NULL);
	save(f, note);
	g_object_unref(publish(f, form));
	g_assert_cmpint(submit_pairs(f, form, hers, NULL), ==, VENTURE_FORMS_ACCEPTED);
	g_assert_cmpint(submit_pairs(f, form, hidden, NULL), ==, VENTURE_FORMS_ACCEPTED);
	g_assert_cmpint(submit_pairs(f, form, his, NULL), ==, VENTURE_FORMS_ACCEPTED);
	/* An access request first: both of hers, the sensitive one included. */
	{
		g_autoptr(JsonNode) exported = venture_forms_export_person(f->db, f->org, "ALICE@example.com", &error);
		g_autofree gchar *dump = NULL;
		g_assert_no_error(error);
		dump = json_to_string(exported, FALSE);
		g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(exported), "responses")), ==, 2);
		g_assert_nonnull(strstr(dump, "\"contact_again\":\"alice@example.com\""));
		g_assert_null(strstr(dump, "Bob"));
	}
	/* Generated assistant and MCP action tools always stage. Export must
	 * refuse before reading answers or putting them on an approval card. */
	{
		VentureAction *action = venture_action_registry_lookup(
			venture_database_get_action_registry(f->db), "form", "export_person");
		g_autoptr(GHashTable) params = g_hash_table_new_full(g_str_hash, g_str_equal,
			g_free, (GDestroyNotify)json_node_unref);
		VentureActor actor;
		const VentureActorKind kinds[] = { VENTURE_ACTOR_KIND_AI, VENTURE_ACTOR_KIND_USER };
		guint caller;

		g_assert_nonnull(action);
		actor.name = "export-test";
		actor.prompt = NULL;
		actor.request_id = NULL;
		actor.approved_by = NULL;
		for (caller = 0; caller < G_N_ELEMENTS(kinds); caller++)
		{
			actor.kind = kinds[caller];
			actor.name = caller == 0 ? "ai" : "API token #1";
			g_assert_null(venture_confirmation_store_stage_action(
				venture_context_get_confirmations(f->context), action, form, params,
				&actor, VENTURE_USER_ROLE_OWNER, "test", &error));
			g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_UNSUPPORTED);
			g_clear_error(&error);
		}
	}
	result = venture_forms_erase_person(f->db, f->org, " alice@example.com ", NULL, &error);
	g_assert_no_error(error);
	text = json_to_string(result, FALSE);
	g_assert_nonnull(strstr(text, "\"erased\":2"));
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 1);
	venture_query_set_limit(audits, 0);
	entries = venture_database_find(f->db, audits, NULL);
	for (i = 0; i < entries->len; i++)
	{
		g_autofree gchar *label = NULL, *diff = NULL;
		g_object_get(g_ptr_array_index(entries, i), "target-label", &label, "diff", &diff, NULL);
		g_assert_null(strstr(label != NULL ? label : "", "Alice"));
		g_assert_null(strstr(diff != NULL ? diff : "", "alice"));
		if (g_strcmp0(label, "Erasure request") == 0)
			recorded = TRUE;
	}
	g_assert_true(recorded);
	g_clear_pointer(&result, json_node_unref);
	g_assert_null(venture_forms_erase_person(f->db, f->org, "nobody", NULL, &error));
	g_assert_nonnull(error);
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
	g_assert_cmpfloat_with_epsilon(summary_value(json, "Sales", "share"), 2.0 / 3.0, 0.000001);
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

static void
optin_business_saved(VentureDatabase *database, VentureEntity *entity, gboolean created, gpointer data)
{
	(void)database; (void)created;
	if (VENTURE_IS_FORM_PENDING(entity) || VENTURE_IS_FORM_SUBMISSION(entity) ||
	    VENTURE_IS_CONTACT(entity) || VENTURE_IS_MARKETING_CONSENT(entity) || VENTURE_IS_MARKETING_MEMBER(entity))
		(*(guint *)data)++;
}

/* Inbox ownership must precede every CRM/consent/response write. Reading a
 * link (including a mail scanner) cannot count as the person's confirmation. */
static void
test_double_optin(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "optin-form", VENTURE_FORM_LIVE);
	g_autoptr(VentureEntity) news = make_field(f, form, "news", "Send the newsletter", VENTURE_FORM_FIELD_CONSENT, TRUE, 20);
	g_autoptr(VentureEntity) list = g_object_new(VENTURE_TYPE_MARKETING_LIST, "organization-id", f->org, "name", "Readers", NULL);
	g_autoptr(VentureLogMailer) mailer = venture_log_mailer_new();
	g_autoptr(VentureMailOutbox) outbox = venture_mail_outbox_new(f->db, VENTURE_MAILER(mailer));
	g_autoptr(GDateTime) now = venture_time_now(), later = NULL;
	g_autoptr(VentureEntity) pending = NULL, response = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *body = NULL, *token = NULL, *public_body = NULL;
	const gchar *const yes[] = { "email", "Alice@Example.test", "news", "on", NULL };
	const gchar *begin, *end;
	guint business_saves = 0;
	gulong saved_handler;
	(void)data;
	save(f, list);
	add_field(f, form, "email", "Email", VENTURE_FORM_FIELD_EMAIL, TRUE, 10);
	g_object_set(news, "marketing-consent", TRUE, "help", "Unsubscribe any time.", NULL); save(f, news);
	g_object_set(form, "double-opt-in", TRUE, "optin-email-field", "email", "public-origin", "https://forms.example.test",
		"optin-list-id", venture_entity_get_id(list), NULL); save(f, form);
	g_object_unref(publish(f, form));
	{
		g_autoptr(VentureEntity) forged = g_object_new(VENTURE_TYPE_FORM_PENDING,
			"organization-id", f->org, "form-id", venture_entity_get_id(form),
			"version-id", venture_forms_get_int(form, "published-version-id"), "name", "Forged signup", "email", "forged@example.test", NULL);
		refuse(f, forged, "only the forms service");
	}
	saved_handler = g_signal_connect(f->db, "entity-saved", G_CALLBACK(optin_business_saved), &business_saves);
	g_assert_cmpint(submit_pairs(f, form, yes, NULL), ==, VENTURE_FORMS_PENDING);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_PENDING), ==, 1);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 0);
	g_assert_cmpint(count(f, VENTURE_TYPE_CONTACT), ==, 0);
	g_assert_cmpint(count(f, VENTURE_TYPE_LEAD), ==, 0);
	g_assert_cmpint(count(f, VENTURE_TYPE_MARKETING_CONSENT), ==, 0);
	g_assert_cmpint(count(f, VENTURE_TYPE_MARKETING_MEMBER), ==, 0);
	g_assert_cmpuint(business_saves, ==, 0);
	/* A second immediate submit coalesces without another message. */
	g_assert_cmpint(submit_pairs(f, form, yes, NULL), ==, VENTURE_FORMS_PENDING);
	g_assert_cmpint(count(f, VENTURE_TYPE_MAIL_MESSAGE), ==, 1);
	later = g_date_time_add_minutes(now, 1);
	g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, f->org, 10, later, NULL, &error), ==, 1);
	g_assert_no_error(error);
	g_assert_cmpuint(venture_log_mailer_get_messages(mailer)->len, ==, 1);
	g_object_get(g_ptr_array_index((GPtrArray *)venture_log_mailer_get_messages(mailer), 0),
		"private-text-body", &body, "text-body", &public_body, NULL);
	begin = strstr(body, "/confirm/"); g_assert_nonnull(begin); begin += strlen("/confirm/");
	end = strchr(begin, '\n'); g_assert_nonnull(end); token = g_strndup(begin, end - begin);
	g_assert_null(strstr(public_body, token));
	g_clear_pointer(&later, g_date_time_unref); later = venture_time_now();
	pending = venture_forms_confirm_signup(f->db, form, token, later, FALSE, &error);
	g_assert_no_error(error); g_assert_nonnull(pending);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 0);
	{
		g_autoptr(JsonNode) json = venture_serializable_to_json(VENTURE_SERIALIZABLE(pending), FALSE);
		g_autofree gchar *text = json_to_string(json, FALSE);
		g_assert_null(strstr(text, "alice@example.test")); g_assert_null(strstr(text, token));
	}
	response = venture_forms_confirm_signup(f->db, form, token, later, TRUE, &error);
	g_assert_no_error(error); g_assert_nonnull(response);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_PENDING), ==, 0);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 1);
	g_assert_cmpint(count(f, VENTURE_TYPE_CONTACT), ==, 1);
	g_assert_cmpint(count(f, VENTURE_TYPE_MARKETING_CONSENT), ==, 1);
	g_assert_cmpint(count(f, VENTURE_TYPE_MARKETING_MEMBER), ==, 1);
	g_assert_cmpuint(business_saves, ==, 4);
	g_signal_handler_disconnect(f->db, saved_handler);
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MARKETING_CONSENT);
		g_autoptr(VentureEntity) consent = venture_database_find_one(f->db, query, &error);
		g_autoptr(GDateTime) recorded = NULL;
		g_autofree gchar *evidence = NULL;
		g_assert_no_error(error); g_assert_nonnull(consent);
		g_object_get(consent, "recorded-at", &recorded, "evidence", &evidence, NULL);
		g_assert_cmpint(g_date_time_compare(recorded, later), ==, 0);
		g_assert_nonnull(strstr(evidence, "Send the newsletter"));
		g_assert_nonnull(strstr(evidence, "Unsubscribe any time."));
	}
	g_clear_object(&response);
	response = venture_forms_confirm_signup(f->db, form, token, later, TRUE, &error);
	g_assert_null(response); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND); g_clear_error(&error);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 1);
}


static VentureEntity *
optin_form(Fixture *f, const gchar *token)
{
	VentureEntity *form = make_form(f, token, VENTURE_FORM_LIVE);
	g_autoptr(VentureEntity) consent = make_field(f, form, "news", "Send newsletter", VENTURE_FORM_FIELD_CONSENT, TRUE, 20);
	add_field(f, form, "email", "Email", VENTURE_FORM_FIELD_EMAIL, TRUE, 10);
	g_object_set(consent, "marketing-consent", TRUE, NULL); save(f, consent);
	g_object_set(form, "double-opt-in", TRUE, "optin-email-field", "email", "public-origin", "https://forms.example.test", NULL);
	save(f, form); g_object_unref(publish(f, form));
	return form;
}

static gchar *
optin_latest_token(Fixture *f)
{
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
	g_autoptr(VentureEntity) message = NULL;
	g_autofree gchar *body = NULL;
	const gchar *begin, *end;
	venture_query_add_order(query, "id", VENTURE_SORT_DESCENDING, NULL);
	message = venture_database_find_one(f->db, query, NULL); g_assert_nonnull(message);
	body = venture_forms_get_string(message, "private-text-body");
	begin = strstr(body, "/confirm/"); g_assert_nonnull(begin); begin += strlen("/confirm/");
	end = strchr(begin, '\n'); g_assert_nonnull(end);
	return g_strndup(begin, end - begin);
}

static void
optin_submit_at(Fixture *f, VentureEntity *form, const gchar *email, GDateTime *now)
{
	g_autoptr(GHashTable) answers = venture_forms_answers_new();
	g_autoptr(GError) error = NULL;
	VentureFormsOutcome outcome;
	venture_forms_answers_add(answers, "email", email); venture_forms_answers_add(answers, "news", "on");
	g_assert_true(venture_forms_submit(f->db, form, answers, NULL, now, &outcome, NULL, NULL, &error));
	g_assert_no_error(error); g_assert_cmpint(outcome, ==, VENTURE_FORMS_PENDING);
}

/* Resend cannot refresh the retention clock or retain an earlier capability.
 * Expiry and erasure cover working copies even though there is no response. */
static void
test_optin_resend_expiry(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = optin_form(f, "optin-resend"), other = optin_form(f, "optin-other");
	g_autoptr(VentureLogMailer) mailer = venture_log_mailer_new();
	g_autoptr(VentureMailOutbox) outbox = venture_mail_outbox_new(f->db, VENTURE_MAILER(mailer));
	g_autoptr(GDateTime) now = venture_time_now(), later = NULL;
	g_autofree gchar *first = NULL, *second = NULL, *third = NULL;
	g_autoptr(VentureEntity) result = NULL;
	g_autoptr(JsonNode) report = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	optin_submit_at(f, form, "Alice@Example.test", now); first = optin_latest_token(f);
	result = venture_forms_confirm_signup(f->db, other, first, now, TRUE, &error);
	g_assert_null(result); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND); g_clear_error(&error);
	{
		g_autofree gchar *tampered = g_strdup(first);
		tampered[0] = tampered[0] == 'a' ? 'b' : 'a';
		result = venture_forms_confirm_signup(f->db, form, tampered, now, TRUE, &error);
		g_assert_null(result); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND); g_clear_error(&error);
	}
	later = g_date_time_add_seconds(now, 59); optin_submit_at(f, form, "alice@example.test", later);
	g_assert_cmpint(count(f, VENTURE_TYPE_MAIL_MESSAGE), ==, 1); g_clear_pointer(&later, g_date_time_unref);
	later = g_date_time_add_seconds(now, 60); optin_submit_at(f, form, "alice@example.test", later); second = optin_latest_token(f);
	g_assert_cmpstr(first, !=, second); g_assert_cmpint(count(f, VENTURE_TYPE_MAIL_MESSAGE), ==, 2);
	result = venture_forms_confirm_signup(f->db, form, first, later, FALSE, &error);
	g_assert_null(result); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND); g_clear_error(&error);
	g_clear_pointer(&later, g_date_time_unref); later = g_date_time_add_seconds(now, 120);
	optin_submit_at(f, form, "alice@example.test", later); third = optin_latest_token(f);
	g_assert_cmpstr(second, !=, third); g_assert_cmpint(count(f, VENTURE_TYPE_MAIL_MESSAGE), ==, 3);
	g_clear_pointer(&later, g_date_time_unref); later = g_date_time_add_seconds(now, 180);
	optin_submit_at(f, form, "alice@example.test", later); g_assert_cmpint(count(f, VENTURE_TYPE_MAIL_MESSAGE), ==, 3);
	report = venture_forms_export_person(f->db, f->org, "alice@example.test", &error);
	g_assert_no_error(error); g_assert_nonnull(report);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(report), "unconfirmed_signups")), ==, 1);
	g_clear_pointer(&report, json_node_unref);
	g_clear_pointer(&later, g_date_time_unref); later = g_date_time_add_hours(now, 24);
	result = venture_forms_confirm_signup(f->db, form, third, later, TRUE, &error);
	g_assert_null(result); g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND); g_clear_error(&error);
	report = venture_forms_retention_sweep(f->db, f->org, 1, later, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(report);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(report), "expired_signups"), ==, 1);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_PENDING), ==, 0);
	g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, f->org, 10, later, NULL, &error), ==, 0);
	g_assert_no_error(error); g_clear_pointer(&report, json_node_unref);
	optin_submit_at(f, form, "alice@example.test", later);
	report = venture_forms_erase_person(f->db, f->org, "alice@example.test", NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(report);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_PENDING), ==, 0);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 0);
}

/* A suppression introduced between signup and confirmation must roll back
 * contact creation as well as permission; ordinary follow-up fallback would
 * otherwise keep an unconfirmed response and consume its one-use link. */
static void
test_optin_atomic_suppression(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = optin_form(f, "optin-atomic");
	g_autoptr(VentureLogMailer) mailer = venture_log_mailer_new();
	g_autoptr(VentureMailOutbox) outbox = venture_mail_outbox_new(f->db, VENTURE_MAILER(mailer));
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(VentureEntity) suppression = g_object_new(VENTURE_TYPE_SUPPRESSION,
		"organization-id", f->org, "email", "alice@example.test", NULL), result = NULL;
	g_autofree gchar *token = NULL;
	g_autoptr(GError) error = NULL;
	guint business_saves = 0;
	gulong saved_handler;
	(void)data;
	optin_submit_at(f, form, "alice@example.test", now); token = optin_latest_token(f);
	save(f, suppression);
	saved_handler = g_signal_connect(f->db, "entity-saved", G_CALLBACK(optin_business_saved), &business_saves);
	result = venture_forms_confirm_signup(f->db, form, token, now, TRUE, &error);
	g_assert_null(result); g_assert_nonnull(error); g_clear_error(&error);
	g_assert_cmpuint(business_saves, ==, 0);
	g_signal_handler_disconnect(f->db, saved_handler);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 0);
	g_assert_cmpint(count(f, VENTURE_TYPE_CONTACT), ==, 0);
	g_assert_cmpint(count(f, VENTURE_TYPE_MARKETING_CONSENT), ==, 0);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_PENDING), ==, 1);
	result = venture_forms_confirm_signup(f->db, form, token, now, FALSE, &error);
	g_assert_no_error(error); g_assert_nonnull(result);
	/* A fresh suppressed request gives the same public outcome and no mail. */
	{
		g_autoptr(VentureEntity) other = optin_form(f, "optin-suppressed");
		optin_submit_at(f, other, "alice@example.test", now);
		g_assert_cmpint(count(f, VENTURE_TYPE_FORM_PENDING), ==, 1);
		g_assert_cmpint(count(f, VENTURE_TYPE_MAIL_MESSAGE), ==, 1);
	}
}


/* Erasure must also work after transactional thank-you mail was sent. A
 * refused nested cancel used to roll the entire erasure transaction back. */
static void
test_erase_sent_confirmation(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "erase-sent-mail");
	g_autoptr(VentureLogMailer) mailer = venture_log_mailer_new();
	g_autoptr(VentureMailOutbox) outbox = venture_mail_outbox_new(f->db, VENTURE_MAILER(mailer));
	g_autoptr(GDateTime) now = venture_time_now(), due = g_date_time_add_minutes(now, 1);
	g_autoptr(JsonNode) erased = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *const pairs[] = { "name", "Alice", "email", "alice@example.test", "topic", "sales", NULL };
	(void)data;
	g_object_set(form, "confirmation-field", "email", NULL); save(f, form);
	g_assert_cmpint(submit_pairs(f, form, pairs, NULL), ==, VENTURE_FORMS_ACCEPTED);
	g_assert_cmpint(venture_mail_outbox_deliver_due(outbox, f->org, 10, due, NULL, &error), ==, 1);
	g_assert_no_error(error);
	erased = venture_forms_erase_person(f->db, f->org, "alice@example.test", NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(erased);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(erased), "erased"), ==, 1);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(erased), "confirmations_cancelled"), ==, 0);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 0);
}

/* A person named in a private pending answer can exercise access/erasure
 * even if the inbox used to confirm the signup belongs to somebody else. */
static void
test_optin_private_erasure(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = optin_form(f, "pending-private"), field = NULL;
	g_autoptr(VentureLogMailer) mailer = venture_log_mailer_new();
	g_autoptr(VentureMailOutbox) outbox = venture_mail_outbox_new(f->db, VENTURE_MAILER(mailer));
	g_autoptr(JsonNode) result = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *const pairs[] = { "email", "signup@example.test", "news", "on", "private_email", "private@example.test", NULL };
	(void)data;
	field = make_field(f, form, "private_email", "Private address", VENTURE_FORM_FIELD_EMAIL, FALSE, 30);
	g_object_set(field, "sensitive", TRUE, NULL); save(f, field); g_object_unref(publish(f, form));
	g_assert_cmpint(submit_pairs(f, form, pairs, NULL), ==, VENTURE_FORMS_PENDING);
	result = venture_forms_export_person(f->db, f->org, "private@example.test", &error);
	g_assert_no_error(error); g_assert_nonnull(result);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(result), "unconfirmed_signups")), ==, 1);
	g_clear_pointer(&result, json_node_unref);
	result = venture_forms_erase_person(f->db, f->org, "private@example.test", NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(result);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(result), "confirmations_cancelled"), ==, 1);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_PENDING), ==, 0);
}


/* Inbox confirmation reuses a current contact without overwriting its CRM
 * details, and that verified identity participates in the contact cap. */
static void
test_optin_contact_limit(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = optin_form(f, "optin-contact"), result = NULL;
	g_autoptr(VentureEntity) contact = g_object_new(VENTURE_TYPE_CONTACT,
		"organization-id", f->org, "name", "Existing CRM name", "email", "Alice@Example.test", NULL);
	g_autoptr(VentureLogMailer) mailer = venture_log_mailer_new();
	g_autoptr(VentureMailOutbox) outbox = venture_mail_outbox_new(f->db, VENTURE_MAILER(mailer));
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *token = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	save(f, contact); g_object_set(form, "one-per-contact", TRUE, NULL); save(f, form);
	optin_submit_at(f, form, "alice@example.test", now); token = optin_latest_token(f);
	/* Publishing new wording while the email waits must not change evidence. */
	{
		g_autoptr(GPtrArray) fields = venture_forms_fields(f->db, form, &error);
		guint i;
		g_assert_no_error(error);
		for (i = 0; i < fields->len; i++)
		{
			VentureEntity *field = g_ptr_array_index(fields, i);
			if (!venture_forms_get_bool(field, "marketing-consent")) continue;
			g_object_set(field, "label", "New published wording", NULL); save(f, field);
		}
		g_object_unref(publish(f, form));
	}
	result = venture_forms_confirm_signup(f->db, form, token, now, TRUE, &error);
	g_assert_no_error(error); g_assert_nonnull(result);
	g_assert_cmpint(venture_forms_get_int(result, "contact-id"), ==, venture_entity_get_id(contact));
	g_assert_cmpint(venture_forms_get_int(result, "version-number"), ==, 1);
	g_assert_cmpint(count(f, VENTURE_TYPE_CONTACT), ==, 1);
	{
		g_autoptr(VentureEntity) fresh = reread(f, contact);
		g_autofree gchar *name = venture_forms_get_string(fresh, "name");
		g_assert_cmpstr(name, ==, "Existing CRM name");
	}
	g_clear_object(&result); g_clear_pointer(&token, g_free);
	optin_submit_at(f, form, "alice@example.test", now); token = optin_latest_token(f);
	result = venture_forms_confirm_signup(f->db, form, token, now, TRUE, &error);
	g_assert_null(result); g_assert_nonnull(error); g_assert_nonnull(strstr(error->message, "already"));
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 1);
	g_assert_cmpint(count(f, VENTURE_TYPE_MARKETING_CONSENT), ==, 1);
}


/* A later edit must not turn an allowed placeholder into a forward or
 * sensitive reference; every generic writer reaches the same validator. */
static void
test_piping_validation(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "pipe-validation", VENTURE_FORM_LIVE);
	g_autoptr(VentureEntity) first = make_field(f, form, "name", "Name", VENTURE_FORM_FIELD_SHORT_TEXT, FALSE, 10);
	g_autoptr(VentureEntity) later = make_field(f, form, "detail", "Hello {name}", VENTURE_FORM_FIELD_LONG_TEXT, FALSE, 20);
	g_autoptr(VentureEntity) invalid = make_field(f, form, "bad", "Unknown {missing}", VENTURE_FORM_FIELD_SHORT_TEXT, FALSE, 30);
	(void)data;
	save(f, first); save(f, later);
	refuse(f, invalid, "placeholder");
	g_object_set(invalid, "label", "Expression {name + 1}", NULL); refuse(f, invalid, "placeholder");
	g_object_set(first, "sensitive", TRUE, NULL); refuse(f, first, "placeholder");
	g_object_set(first, "sensitive", FALSE, "position", (gint64)40, NULL); refuse(f, first, "placeholder");
	g_object_set(first, "position", (gint64)10, "help", "Later {detail}", NULL); refuse(f, first, "placeholder");
	g_object_set(form, "success-message", "Unknown {missing}", NULL); refuse(f, form, "placeholder");
	/* Equal positions retain the renderer's ID order, rather than refusing
	 * an earlier question merely because both use the default position. */
	add_field(f, form, "same_position", "Earlier {name}", VENTURE_FORM_FIELD_SHORT_TEXT, FALSE, 10);
	g_object_set(form, "success-message", "Thanks {name}; {{literal}}", NULL); save(f, form);
	g_object_unref(publish(f, form));
}

/* Refill data can include invalid or forged hidden answers. Neither may
 * become display text through a placeholder, even before final submission. */
static void
test_piping_visible_validated(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "pipe-visible", VENTURE_FORM_LIVE);
	g_autoptr(VentureEntity) mode = make_field(f, form, "mode", "Mode", VENTURE_FORM_FIELD_SINGLE_CHOICE, TRUE, 1);
	g_autoptr(VentureEntity) rule = NULL, version = NULL;
	g_autoptr(JsonObject) values = json_object_new();
	g_autofree gchar *html = NULL;
	VentureFormsRender options;
	(void)data;
	g_object_set(mode, "choices", "yes | Yes\nno | No", NULL); save(f, mode);
	add_field(f, form, "email", "Email", VENTURE_FORM_FIELD_EMAIL, TRUE, 10);
	add_field(f, form, "detail", "Detail", VENTURE_FORM_FIELD_SHORT_TEXT, FALSE, 20);
	add_field(f, form, "reply", "Address: {email}; detail: {detail}", VENTURE_FORM_FIELD_SHORT_TEXT, FALSE, 30);
	rule = make_rule(f, form, VENTURE_FORM_RULE_SHOW, "detail", "[{\"field\":\"mode\",\"operator\":\"equals\",\"value\":\"yes\"}]"); save(f, rule);
	version = publish(f, form);
	json_object_set_string_member(values, "mode", "no"); json_object_set_string_member(values, "email", "invalid");
	json_object_set_string_member(values, "detail", "FORGED-HIDDEN");
	options.mode = VENTURE_FORMS_RENDER_FRAGMENT; options.action = NULL; options.ticket = NULL;
	options.values = values; options.errors = NULL; options.version = version;
	html = venture_forms_render(f->db, form, &options, NULL);
	g_assert_nonnull(html); g_assert_nonnull(strstr(html, ">Address: ; detail: </span>"));
	g_clear_pointer(&html, g_free);
	json_object_set_string_member(values, "mode", "yes"); json_object_set_string_member(values, "email", "alice@example.test");
	json_object_set_string_member(values, "detail", "<script>alert(1)</script>");
	html = venture_forms_render(f->db, form, &options, NULL);
	g_assert_nonnull(strstr(html, ">Address: alice@example.test; detail: &lt;script&gt;alert(1)&lt;/script&gt;</span>"));
	g_assert_null(strstr(html, "<script>alert(1)</script>"));
}

/* JS-off navigation carries validated answers into later headings and
 * labels; the response supplies final text, not an untrusted echo of POST. */
static void
test_piping_pages_success(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "pipe-pages", VENTURE_FORM_LIVE), response = NULL;
	g_autofree gchar *ticket = NULL, *body = NULL, *token = NULL, *answers = NULL;
	g_autoptr(JsonNode) parsed = NULL;
	Reply reply = { 0, NULL, NULL, NULL, NULL };
	(void)data;
	add_field(f, form, "name", "Name", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 10);
	add_field(f, form, "next", "Hello {name}", VENTURE_FORM_FIELD_PAGE_BREAK, FALSE, 20);
	add_field(f, form, "agree", "I, {name}, agree", VENTURE_FORM_FIELD_CONSENT, TRUE, 30);
	g_object_set(form, "success-message", "Thanks, {name}. {{Literal}}", NULL); save(f, form); g_object_unref(publish(f, form));
	start_http(f); ticket = old_ticket(form);
	body = g_strdup_printf("%s&name=%%3Cscript%%3Ealert(1)%%3C%%2Fscript%%3E", ticket);
	request(f, "/pub/form/pipe-pages", "application/x-www-form-urlencoded", body, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_nonnull(strstr(reply.body, ">Hello &lt;script&gt;alert(1)&lt;/script&gt;</span>"));
	g_assert_nonnull(strstr(reply.body, ">I, &lt;script&gt;alert(1)&lt;/script&gt;, agree</span>"));
	g_assert_null(strstr(reply.body, "<script>alert(1)</script>"));
	token = page_token(reply.body); reply_clear(&reply); g_clear_pointer(&body, g_free);
	body = g_strdup_printf("_vf_draft=%s", token);
	request(f, "/pub/form/pipe-pages", "application/x-www-form-urlencoded", body, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 422);
	g_assert_nonnull(strstr(reply.body, "I, &lt;script&gt;alert(1)&lt;/script&gt;, agree: This box must be ticked."));
	g_clear_pointer(&token, g_free); token = page_token(reply.body); reply_clear(&reply); g_clear_pointer(&body, g_free);
	body = g_strdup_printf("_vf_draft=%s&agree=on", token);
	request(f, "/pub/form/pipe-pages", "application/x-www-form-urlencoded", body, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 200);
	g_assert_nonnull(strstr(reply.body, "Thanks, &lt;script&gt;alert(1)&lt;/script&gt;. {Literal}"));
	response = last_form_response(f); g_assert_nonnull(response);
	answers = venture_forms_get_string(response, "answers"); parsed = json_from_string(answers, NULL);
	g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(json_node_get_object(parsed), "agree"), "wording"), ==,
		"I, <script>alert(1)</script>, agree");
	reply_clear(&reply);
}

/* Same-row references cannot borrow another row's name. A group count is
 * derived from the validated row shape and is available to final text. */
static void
test_piping_rows(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "pipe-rows", VENTURE_FORM_LIVE);
	g_autoptr(VentureEntity) group = g_object_new(VENTURE_TYPE_FORM_GROUP, "organization-id", f->org,
		"form-id", venture_entity_get_id(form), "key", "attendee", "label", "Attendee", "max-rows", (gint64)3, NULL);
	g_autoptr(VentureEntity) name = make_field(f, form, "name", "Name", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 10);
	g_autoptr(VentureEntity) consent = make_field(f, form, "agree", "I, {name}, agree", VENTURE_FORM_FIELD_CONSENT, TRUE, 20);
	g_autoptr(VentureEntity) response = NULL;
	g_autofree gchar *message = NULL, *text = NULL, *html = NULL;
	g_autoptr(JsonNode) answers = NULL;
	JsonArray *rows;
	const gchar *const pairs[] = { "attendee[0][name]", "Alice", "attendee[0][agree]", "on",
		"attendee[1][name]", "Bob", "attendee[1][agree]", "on", NULL };
	(void)data;
	save(f, group); g_object_set(name, "group-id", venture_entity_get_id(group), NULL); save(f, name);
	g_object_set(consent, "group-id", venture_entity_get_id(group), NULL); save(f, consent);
	g_object_set(form, "success-message", "Registered {attendee.count} attendees.", NULL); save(f, form);
	g_object_unref(publish(f, form));
	g_assert_cmpint(submit_pairs(f, form, pairs, NULL), ==, VENTURE_FORMS_ACCEPTED);
	response = last_form_response(f); message = venture_forms_success_message(f->db, form, response);
	g_assert_cmpstr(message, ==, "Registered 2 attendees.");
	text = venture_forms_get_string(response, "answers"); answers = json_from_string(text, NULL);
	rows = json_object_get_array_member(json_node_get_object(answers), "attendee");
	html = venture_forms_render_answers(f->db, response, NULL);
	g_assert_nonnull(strstr(html, "I, Alice, agree"));
	g_assert_nonnull(strstr(html, "I, Bob, agree"));
	g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(json_array_get_object_element(rows, 0), "agree"), "wording"), ==, "I, Alice, agree");
	g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(json_array_get_object_element(rows, 1), "agree"), "wording"), ==, "I, Bob, agree");
}


/* A translation changes wording, never the identity the answer is counted
 * under. Partial catalogs intentionally fall back instead of blocking saves. */
static VentureEntity *
make_translation(Fixture *f, VentureEntity *form, const gchar *language, const gchar *key, const gchar *text)
{
	return g_object_new(VENTURE_TYPE_FORM_TRANSLATION, "organization-id", f->org,
		"form-id", venture_entity_get_id(form), "language", language, "text-key", key, "text", text, NULL);
}

static void
translate(Fixture *f, VentureEntity *form, const gchar *key, const gchar *text)
{
	g_autoptr(VentureEntity) row = make_translation(f, form, "fr", key, text);
	save(f, row);
}

static gchar *
language_seed(Fixture *f, VentureEntity *form, const gchar *language)
{
	g_autoptr(VentureEntity) version = venture_forms_published_version(f->db, form, NULL);
	g_autoptr(JsonObject) values = json_object_new();
	json_object_set_string_member(values, VENTURE_FORMS_LANGUAGE, language);
	return venture_forms_prefill_pack(form, version, values);
}

static void
test_languages_validation(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "languages-validation");
	g_autoptr(VentureEntity) row = make_translation(f, form, "FR", "field.name.label", "Votre nom");
	g_autoptr(VentureEntity) duplicate = make_translation(f, form, "fr", "field.name.label", "Nom");
	g_autoptr(GPtrArray) fields = NULL;
	g_autoptr(VentureEntity) version = NULL;
	g_autofree gchar *chosen = NULL;
	(void)data;
	save(f, row); refuse(f, duplicate, "already");
	g_object_set(duplicate, "language", "../fr", NULL); refuse(f, duplicate, "language");
	g_object_set(duplicate, "language", "fr", "text-key", "field.unknown.label", NULL); refuse(f, duplicate, "text key");
	g_object_set(duplicate, "text-key", "field.email.label", "text", "Plus tard {topic}", NULL); refuse(f, duplicate, "placeholder");
	g_object_set(duplicate, "text-key", "form.confirmation_subject", "text", "Bonjour\nBcc: attacker@example.test", NULL); refuse(f, duplicate, "one line");
	translate(f, form, "field.email.help", "Bonjour {name}");
	{
		g_autoptr(GPtrArray) rows = venture_forms_fields(f->db, form, NULL);
		guint i;
		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *source = g_ptr_array_index(rows, i);
			g_autofree gchar *key = venture_forms_get_string(source, "key");
			if (g_strcmp0(key, "name") == 0)
			{ g_object_set(source, "sensitive", TRUE, NULL); refuse(f, source, "placeholder"); }
		}
	}
	version = publish(f, form); fields = venture_forms_definition_for(f->db, form, version, NULL);
	chosen = venture_forms_language_choose(fields, NULL, "de;q=1, fr-CA;q=0.9, en;q=0.2"); g_assert_cmpstr(chosen, ==, "fr");
	g_clear_pointer(&chosen, g_free); chosen = venture_forms_language_choose(fields, "en", "fr"); g_assert_cmpstr(chosen, ==, "en");
	g_clear_pointer(&chosen, g_free); chosen = venture_forms_language_choose(fields, NULL, "fr;q=0, en;q=0.2"); g_assert_cmpstr(chosen, ==, "en");
	venture_forms_localize(fields, "fr");
	g_assert_cmpstr(venture_forms_definition_find(fields, "name")->label, ==, "Votre nom");
	g_assert_cmpstr(venture_forms_definition_find(fields, "email")->label, ==, "Email");
	g_object_set(row, "text", "Draft only", NULL); save(f, row);
	g_clear_pointer(&fields, g_ptr_array_unref); fields = venture_forms_definition_for(f->db, form, version, NULL);
	venture_forms_localize(fields, "fr"); g_assert_cmpstr(venture_forms_definition_find(fields, "name")->label, ==, "Votre nom");
}

static void
test_languages_report(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "languages-report"), response = NULL;
	g_autofree gchar *seed = NULL, *language = NULL, *answers = NULL;
	g_autoptr(JsonObject) options = json_object_new();
	g_autoptr(JsonNode) json = NULL;
	g_autoptr(VentureReportResult) result = NULL;
	g_autoptr(VentureDateRange) period = venture_date_range_new_all_time();
	g_autoptr(GError) error = NULL;
	const gchar *english[] = { "name", "Alice", "email", "alice@example.test", "topic", "sales", NULL };
	const gchar *french[] = { "name", "Anne", "email", "anne@example.test", "topic", "sales", VENTURE_FORMS_PREFILL, NULL, NULL };
	(void)data;
	translate(f, form, "message.errors", "Corrigez les réponses.");
	translate(f, form, "message.email", "Saisissez une adresse courriel.");
	translate(f, form, "field.topic.label", "Sujet"); translate(f, form, "choice.topic.sales", "Ventes");
	g_object_unref(publish(f, form)); seed = language_seed(f, form, "fr"); french[7] = seed;
	g_assert_cmpint(submit_pairs(f, form, english, NULL), ==, VENTURE_FORMS_ACCEPTED);
	g_assert_cmpint(submit_pairs(f, form, french, NULL), ==, VENTURE_FORMS_ACCEPTED);
	response = last_form_response(f); language = venture_forms_get_string(response, "language"); answers = venture_forms_get_string(response, "answers");
	g_assert_cmpstr(language, ==, "fr"); g_assert_nonnull(strstr(answers, "sales")); g_assert_null(strstr(answers, "Ventes"));
	g_object_set(response, "language", "en", NULL); refuse(f, response, "cannot be changed");
	json_object_set_int_member(options, "form_id", venture_entity_get_id(form)); json_object_set_int_member(options, "organization_id", f->org);
	json_object_set_string_member(options, "language", "fr");
	result = venture_report_generate(venture_report_registry_lookup(venture_context_get_report_registry(f->context), "form_summary"), f->context, period, options, &error);
	g_assert_no_error(error); g_assert_nonnull(result); json = venture_report_result_to_json(result);
	g_assert_cmpfloat(summary_value(json, "Ventes", "count"), ==, 2);
	{
		g_autofree gchar *path = g_strdup_printf("/api/v1/reports/form_summary?period=all&form_id=%" G_GINT64_FORMAT "&language=fr", venture_entity_get_id(form));
		g_autoptr(JsonNode) public_report = NULL;
		Reply reply = { 0, NULL, NULL, NULL, NULL };
		f->open = TRUE; start_http(f);
		request(f, path, NULL, NULL, NULL, NULL, &reply);
		g_assert_cmpuint(reply.status, ==, 200);
		public_report = json_from_string(reply.body, &error); g_assert_no_error(error);
		g_assert_cmpfloat(summary_value(public_report, "Ventes", "count"), ==, 2);
		reply_clear(&reply);
	}

	{
		g_autofree gchar *ticket = old_ticket(form), *encoded = g_uri_escape_string(seed, NULL, TRUE);
		g_autofree gchar *body = g_strdup_printf("%s&_vf_prefill=%s&name=Anne&topic=sales&email=invalid", ticket, encoded);
		g_autoptr(JsonNode) refusal = NULL;
		Reply reply = { 0, NULL, NULL, NULL, NULL };
		request(f, "/pub/form/languages-report", "application/x-www-form-urlencoded", body, NULL, "application/json", &reply);
		g_assert_cmpuint(reply.status, ==, 422); refusal = json_from_string(reply.body, NULL);
		g_assert_cmpstr(json_object_get_string_member(json_node_get_object(refusal), "message"), ==, "Corrigez les réponses.");
		g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(json_node_get_object(refusal), "errors"), "email"), ==, "Saisissez une adresse courriel.");
		reply_clear(&reply);
		request(f, "/pub/form/languages-report", "application/x-www-form-urlencoded", body, NULL, NULL, &reply);
		g_assert_cmpuint(reply.status, ==, 422);
		g_assert_nonnull(strstr(reply.body, "lang=\"fr\""));
		g_assert_nonnull(strstr(reply.body, "name=\"_vf_prefill\""));
		reply_clear(&reply);
	}

	{
		g_autoptr(JsonNode) schema = NULL;
		Reply reply = { 0, NULL, NULL, NULL, NULL };
		request(f, "/pub/form/languages-report/schema?lang=fr", NULL, NULL, NULL, NULL, &reply);
		g_assert_cmpuint(reply.status, ==, 200); schema = json_from_string(reply.body, NULL);
		g_assert_cmpstr(json_object_get_string_member(json_node_get_object(schema), "language"), ==, "fr");
		g_assert_false(json_object_has_member(json_object_get_object_member(json_node_get_object(schema), "prefill"), VENTURE_FORMS_LANGUAGE));
		g_assert_true(json_object_has_member(json_object_get_object_member(json_node_get_object(schema), "prefill"), VENTURE_FORMS_PREFILL));
		reply_clear(&reply);
	}

}

/* A hidden language parameter is not authoritative. The signed seed, and then
 * the server-owned working copy, pin the chosen language across every page. */
static void
test_languages_pages(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "languages-pages", VENTURE_FORM_LIVE), response = NULL;
	g_autofree gchar *seed = NULL, *encoded = NULL, *ticket = NULL, *body = NULL, *token = NULL, *text = NULL;
	g_autoptr(JsonNode) parsed = NULL;
	Reply reply = { 0, NULL, NULL, NULL, NULL };
	(void)data;
	add_field(f, form, "name", "Name", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 10);
	add_field(f, form, "next", "Next page", VENTURE_FORM_FIELD_PAGE_BREAK, FALSE, 20);
	add_field(f, form, "email", "Email", VENTURE_FORM_FIELD_EMAIL, TRUE, 30);
	add_field(f, form, "agree", "I, {name}, agree", VENTURE_FORM_FIELD_CONSENT, TRUE, 40);
	translate(f, form, "form.title", "Bonjour"); translate(f, form, "field.name.label", "Nom");
	translate(f, form, "field.agree.label", "Moi, {name}, je consens");
	translate(f, form, "message.email", "Saisissez une adresse courriel.");
	translate(f, form, "message.required", "Une réponse est nécessaire.");
	translate(f, form, "form.success_message", "Merci {name}.");
	g_object_unref(publish(f, form)); seed = language_seed(f, form, "fr"); encoded = g_uri_escape_string(seed, NULL, TRUE);
	start_http(f);
	request(f, "/pub/form/languages-pages?lang=fr", NULL, NULL, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 200); g_assert_nonnull(strstr(reply.body, "lang=\"fr\"")); g_assert_nonnull(strstr(reply.body, ">Bonjour</h1>")); reply_clear(&reply);
	ticket = old_ticket(form); body = g_strdup_printf("%s&_vf_prefill=%s", ticket, encoded);
	request(f, "/pub/form/languages-pages", "application/x-www-form-urlencoded", body, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 422); g_assert_nonnull(strstr(reply.body, "Une réponse est nécessaire."));
	reply_clear(&reply); g_clear_pointer(&body, g_free);
	body = g_strdup_printf("%s&_vf_prefill=%s&name=Anne", ticket, encoded);
	request(f, "/pub/form/languages-pages", "application/x-www-form-urlencoded", body, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 200); g_assert_nonnull(strstr(reply.body, "Moi, Anne, je consens"));
	g_clear_pointer(&token, g_free); token = page_token(reply.body); reply_clear(&reply); g_clear_pointer(&body, g_free);
	body = g_strdup_printf("_vf_draft=%s&email=not-email&agree=on", token);
	request(f, "/pub/form/languages-pages", "application/x-www-form-urlencoded", body, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 422); g_assert_nonnull(strstr(reply.body, "Saisissez une adresse courriel."));
	g_clear_pointer(&token, g_free); token = page_token(reply.body); reply_clear(&reply); g_clear_pointer(&body, g_free);
	body = g_strdup_printf("_vf_draft=%s&email=anne%%40example.test&agree=on", token);
	request(f, "/pub/form/languages-pages", "application/x-www-form-urlencoded", body, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 200); g_assert_nonnull(strstr(reply.body, "Merci Anne.")); reply_clear(&reply);
	response = last_form_response(f); text = venture_forms_get_string(response, "answers"); parsed = json_from_string(text, NULL);
	g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(json_node_get_object(parsed), "agree"), "wording"), ==, "Moi, Anne, je consens");
	g_clear_pointer(&text, g_free); text = venture_forms_get_string(response, "language"); g_assert_cmpstr(text, ==, "fr");
	/* Altering even one signed byte is refused before a working copy exists. */
	seed[0] = seed[0] == 'a' ? 'b' : 'a'; g_clear_pointer(&encoded, g_free); encoded = g_uri_escape_string(seed, NULL, TRUE);
	g_clear_pointer(&body, g_free); body = g_strdup_printf("%s&_vf_prefill=%s&name=Forged", ticket, encoded);
	request(f, "/pub/form/languages-pages", "application/x-www-form-urlencoded", body, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 404); reply_clear(&reply);
}

static void
test_languages_optin(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = optin_form(f, "languages-optin"), translated = NULL, response = NULL;
	g_autoptr(VentureLogMailer) mailer = venture_log_mailer_new();
	g_autoptr(VentureMailOutbox) outbox = venture_mail_outbox_new(f->db, VENTURE_MAILER(mailer));
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GDateTime) later = g_date_time_add_seconds(now, -59);
	g_autoptr(GError) error = NULL;
	g_autofree gchar *seed = NULL, *token = NULL, *language = NULL, *body = NULL;
	const gchar *pairs[] = { "email", "anne@example.test", "news", "on", VENTURE_FORMS_PREFILL, NULL, NULL };
	(void)data;
	translated = make_translation(f, form, "fr", "message.optin_intro", "Confirmez votre inscription :"); save(f, translated);
	translate(f, form, "field.news.label", "Je souhaite recevoir les nouvelles.");
	g_object_unref(publish(f, form)); seed = language_seed(f, form, "fr"); pairs[5] = seed;
	{
		g_autoptr(GHashTable) raw = venture_forms_answers_new();
		g_autoptr(GDateTime) earlier = g_date_time_add_seconds(now, -120);
		VentureFormsOutcome outcome;
		guint i;
		for (i = 0; pairs[i] != NULL; i += 2) venture_forms_answers_add(raw, pairs[i], pairs[i + 1]);
		g_assert_true(venture_forms_submit(f->db, form, raw, NULL, earlier, &outcome, NULL, NULL, &error));
		g_assert_no_error(error); g_assert_cmpint(outcome, ==, VENTURE_FORMS_PENDING);
	}
	g_object_set(translated, "text", "New published text", NULL); save(f, translated); g_object_unref(publish(f, form));
	/* A later resend in the default language must not replace original evidence. */
	optin_submit_at(f, form, "anne@example.test", later);
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
		g_autoptr(GPtrArray) rows = venture_database_find(f->db, query, NULL);
		guint i;
		g_assert_cmpuint(rows->len, ==, 2);
		for (i = 0; i < rows->len; i++)
		{
			g_autofree gchar *mail = venture_forms_get_string(g_ptr_array_index(rows, i), "private-text-body");
			g_assert_nonnull(strstr(mail, "Confirmez votre inscription")); g_assert_null(strstr(mail, "New published text"));
		}
	}
	token = optin_latest_token(f); response = venture_forms_confirm_signup(f->db, form, token, later, TRUE, &error);
	g_assert_no_error(error); g_assert_nonnull(response);
	language = venture_forms_get_string(response, "language"); body = venture_forms_get_string(response, "answers");
	g_assert_cmpstr(language, ==, "fr"); g_assert_nonnull(strstr(body, "Je souhaite recevoir les nouvelles."));
}


/* The same private-state omission also dropped personal-link binding before
 * opt-in. Confirming another inbox must never claim the addressed contact. */
static void
test_optin_personal_state(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = optin_form(f, "optin-personal-state"), response = NULL;
	g_autoptr(VentureEntity) contact = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", f->org,
		"name", "Known Anne", "email", "anne@example.test", NULL);
	g_autoptr(VentureLogMailer) mailer = venture_log_mailer_new();
	g_autoptr(VentureMailOutbox) outbox = venture_mail_outbox_new(f->db, VENTURE_MAILER(mailer));
	g_autoptr(GDateTime) now = venture_time_now(), expires = g_date_time_add_days(now, 2);
	g_autoptr(JsonObject) errors = NULL;
	g_autoptr(JsonNode) exported = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *url = NULL, *confirmation = NULL, *digest = NULL, *plain = NULL, *answers = NULL;
	const gchar *personal;
	const gchar *pairs[] = { "email", "different@example.test", "news", "on", VENTURE_FORMS_PERSONAL, NULL, NULL };
	(void)data;
	save(f, contact);
	url = venture_forms_personal_link(f->db, form, contact, "https://forms.example.test", expires, now, &error);
	g_assert_no_error(error); personal = strstr(url, "personal=") + strlen("personal="); pairs[5] = personal;
	g_assert_cmpint(submit_pairs(f, form, pairs, &errors), ==, VENTURE_FORMS_INVALID);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_PENDING), ==, 0);
	pairs[1] = "anne@example.test";
	g_assert_cmpint(submit_pairs(f, form, pairs, NULL), ==, VENTURE_FORMS_PENDING);
	exported = venture_forms_export_person(f->db, f->org, "anne@example.test", &error);
	g_assert_no_error(error); plain = json_to_string(exported, FALSE); g_assert_null(strstr(plain, personal));
	confirmation = optin_latest_token(f); response = venture_forms_confirm_signup(f->db, form, confirmation, now, TRUE, &error);
	g_assert_no_error(error); g_assert_nonnull(response);
	g_assert_cmpint(venture_forms_get_int(response, "contact-id"), ==, venture_entity_get_id(contact));
	digest = venture_forms_get_string(response, "personal-hash"); g_assert_nonnull(digest); g_assert_cmpuint(strlen(digest), ==, 64);
	answers = venture_forms_get_string(response, "answers"); g_assert_null(strstr(answers, "_vf_"));
}

/* Ordinary confirmation templates must use the same localized public answers
 * as the response, without changing choice ids or evaluating author text. */
static void
test_languages_confirmation(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = contact_form(f, "languages-mail"), response = NULL;
	g_autoptr(VentureLogMailer) mailer = venture_log_mailer_new();
	g_autoptr(VentureMailOutbox) outbox = venture_mail_outbox_new(f->db, VENTURE_MAILER(mailer));
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
	g_autoptr(VentureEntity) mail = NULL;
	g_autofree gchar *seed = NULL, *subject = NULL, *body = NULL;
	const gchar *pairs[] = { "name", "Anne", "email", "anne@example.test", "topic", "sales", VENTURE_FORMS_PREFILL, NULL, NULL };
	(void)data;
	g_object_set(form, "confirmation-field", "email", NULL); save(f, form);
	translate(f, form, "choice.topic.sales", "Ventes");
	translate(f, form, "form.confirmation_subject", "Merci {name}");
	translate(f, form, "form.confirmation_message", "Votre sujet : {topic}");
	g_object_unref(publish(f, form)); seed = language_seed(f, form, "fr"); pairs[7] = seed;
	g_assert_cmpint(submit_pairs(f, form, pairs, NULL), ==, VENTURE_FORMS_ACCEPTED);
	mail = venture_database_find_one(f->db, query, NULL); g_assert_nonnull(mail);
	subject = venture_forms_get_string(mail, "subject"); body = venture_forms_get_string(mail, "text-body");
	g_assert_cmpstr(subject, ==, "Merci Anne"); g_assert_cmpstr(body, ==, "Votre sujet : Ventes");
}

/* A resume capability is not a submission or a reusable read URL. GET exposes
 * no answers; POST rotates both it and the old active browser's draft token. */
static void
test_resume_lifecycle(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "resume-life", VENTURE_FORM_LIVE);
	g_autoptr(GDateTime) now = venture_time_now(), started = g_date_time_add_seconds(now, -10);
	g_autoptr(GDateTime) expired = g_date_time_add_days(now, 8);
	g_autoptr(GHashTable) answers = venture_forms_answers_new();
	g_autoptr(VentureFormsStep) saved = NULL, landing = NULL, resumed = NULL, next = NULL, done = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *ticket = NULL, *cap = NULL;
	guint saves = 0, audits = 0;
	(void)data;
	g_object_set(form, "allow-resume", TRUE, "public-origin", "https://forms.example.test", NULL); save(f, form);
	add_field(f, form, "name", "Name", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 10);
	add_field(f, form, "page", "Next", VENTURE_FORM_FIELD_PAGE_BREAK, FALSE, 20);
	add_field(f, form, "email", "Email", VENTURE_FORM_FIELD_EMAIL, TRUE, 30);
	/* Version record ids are global; ticket numbers are per form. */
	{
		g_autoptr(VentureEntity) other = contact_form(f, "other-version-id");
	}
	g_object_unref(publish(f, form));
	ticket = venture_forms_ticket_new(form, started);
	venture_forms_answers_add(answers, VENTURE_FORMS_TICKET, ticket);
	venture_forms_answers_add(answers, VENTURE_FORMS_MOVE, "save");
	g_signal_connect(f->db, "entity-saved", G_CALLBACK(page_saved), &saves);
	g_signal_connect(f->db, "audit", G_CALLBACK(page_audit), &audits);
	/* Saving an empty required question is deliberately allowed. */
	saved = venture_forms_step(f->db, form, answers, NULL, now, &error);
	g_assert_no_error(error); g_assert_nonnull(saved); g_assert_nonnull(saved->resume_url);
	g_assert_cmpuint(json_object_get_size(saved->errors), ==, 0); g_assert_cmpuint(saved->page, ==, 0);
	g_assert_cmpuint(saves, ==, 0); g_assert_cmpuint(audits, ==, 0);
	cap = g_strdup(strrchr(saved->resume_url, '/') + 1);
	g_assert_null(venture_forms_resume(f->db, form, cap, expired, TRUE, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND); g_clear_error(&error);
	landing = venture_forms_resume(f->db, form, cap, now, FALSE, &error);
	g_assert_no_error(error); g_assert_nonnull(landing); g_assert_cmpuint(json_object_get_size(landing->values), ==, 0);
	resumed = venture_forms_resume(f->db, form, cap, now, TRUE, &error);
	g_assert_no_error(error); g_assert_nonnull(resumed); g_assert_cmpstr(resumed->token, !=, saved->token);
	g_assert_nonnull(resumed->resume_url); g_assert_cmpstr(resumed->resume_url, !=, saved->resume_url);
	/* A later publish must not change this resumed session again. */
	add_field(f, form, "later", "Added after resume", VENTURE_FORM_FIELD_SHORT_TEXT, FALSE, 40);
	g_object_unref(publish(f, form));
	saves = 0; audits = 0;
	g_assert_null(venture_forms_resume(f->db, form, cap, now, TRUE, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND); g_clear_error(&error);
	g_hash_table_remove_all(answers); venture_forms_answers_add(answers, VENTURE_FORMS_DRAFT_TOKEN, saved->token);
	g_assert_null(venture_forms_step(f->db, form, answers, NULL, now, &error));
	g_assert_error(error, VENTURE_ERROR, VENTURE_ERROR_NOT_FOUND); g_clear_error(&error);
	g_hash_table_remove_all(answers); venture_forms_answers_add(answers, VENTURE_FORMS_DRAFT_TOKEN, resumed->token);
	venture_forms_answers_add(answers, "name", "Alice");
	next = venture_forms_step(f->db, form, answers, NULL, now, &error);
	g_assert_no_error(error); g_assert_nonnull(next); g_assert_cmpuint(next->page, ==, 1);
	g_assert_cmpuint(saves, ==, 0); g_assert_cmpuint(audits, ==, 0);
	g_hash_table_remove_all(answers); venture_forms_answers_add(answers, VENTURE_FORMS_DRAFT_TOKEN, next->token);
	venture_forms_answers_add(answers, "email", "alice@example.test");
	done = venture_forms_step(f->db, form, answers, NULL, now, &error);
	g_assert_no_error(error); g_assert_nonnull(done); g_assert_true(done->complete);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 1);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_DRAFT_RECORD), ==, 0);
	g_signal_handlers_disconnect_by_data(f->db, &saves); g_signal_handlers_disconnect_by_data(f->db, &audits);
}

/* A saved link upgrades the definition, unlike uninterrupted page navigation.
 * Removed answers disappear, changed consent is asked again, and a new required
 * question before the saved page sends the person back to that question. */
static void
test_resume_changed(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "resume-change", VENTURE_FORM_LIVE);
	g_autoptr(VentureEntity) removed = make_field(f, form, "obsolete", "Old question", VENTURE_FORM_FIELD_SHORT_TEXT, FALSE, 11);
	g_autoptr(VentureEntity) version = NULL;
	g_autoptr(GDateTime) now = venture_time_now(), started = g_date_time_add_seconds(now, -10);
	g_autoptr(GHashTable) answers = venture_forms_answers_new();
	g_autoptr(VentureFormsStep) next = NULL, saved = NULL, resumed = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *ticket = NULL, *seed = NULL;
	const gchar *cap;
	(void)data;
	g_object_set(form, "allow-resume", TRUE, "public-origin", "https://forms.example.test", NULL); save(f, form);
	add_field(f, form, "name", "Name", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 10); save(f, removed);
	add_field(f, form, "agree", "I agree", VENTURE_FORM_FIELD_CONSENT, TRUE, 12);
	add_field(f, form, "page", "Next", VENTURE_FORM_FIELD_PAGE_BREAK, FALSE, 20);
	add_field(f, form, "email", "Email", VENTURE_FORM_FIELD_EMAIL, TRUE, 30);
	translate(f, form, "form.title", "Reprendre");
	g_object_unref(publish(f, form)); seed = language_seed(f, form, "fr"); ticket = venture_forms_ticket_new(form, started);
	venture_forms_answers_add(answers, VENTURE_FORMS_TICKET, ticket); venture_forms_answers_add(answers, VENTURE_FORMS_PREFILL, seed);
	venture_forms_answers_add(answers, "name", "Alice"); venture_forms_answers_add(answers, "obsolete", "discard me"); venture_forms_answers_add(answers, "agree", "on");
	next = venture_forms_step(f->db, form, answers, NULL, now, &error);
	g_assert_no_error(error); g_assert_nonnull(next); g_assert_cmpuint(next->page, ==, 1);
	g_hash_table_remove_all(answers); venture_forms_answers_add(answers, VENTURE_FORMS_DRAFT_TOKEN, next->token);
	venture_forms_answers_add(answers, VENTURE_FORMS_MOVE, "save"); venture_forms_answers_add(answers, "email", "incomplete");
	saved = venture_forms_step(f->db, form, answers, NULL, now, &error);
	g_assert_no_error(error); g_assert_nonnull(saved); g_assert_nonnull(saved->resume_url);
	cap = strrchr(saved->resume_url, '/') + 1;
	g_assert_true(venture_database_purge(f->db, removed, NULL, &error)); g_assert_no_error(error);
	add_field(f, form, "new_required", "New required question", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 13);
	version = publish(f, form);
	resumed = venture_forms_resume(f->db, form, cap, now, TRUE, &error);
	g_assert_no_error(error); g_assert_nonnull(resumed); g_assert_true(resumed->changed);
	g_assert_cmpint(venture_entity_get_id(resumed->version), ==, venture_entity_get_id(version));
	g_assert_cmpuint(resumed->page, ==, 0);
	g_assert_false(json_object_has_member(resumed->values, "obsolete")); g_assert_false(json_object_has_member(resumed->values, "agree"));
	g_assert_cmpstr(json_object_get_string_member(resumed->values, "name"), ==, "Alice");
	g_assert_cmpstr(json_object_get_string_member(resumed->values, "email"), ==, "incomplete");
	{
		g_autofree gchar *language = venture_forms_language_from_values(form, resumed->version, resumed->values);
		g_assert_cmpstr(language, ==, "fr");
	}
}

static void
test_resume_private_mail(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "resume-mail", VENTURE_FORM_LIVE);
	g_autoptr(VentureLogMailer) mailer = venture_log_mailer_new();
	g_autoptr(VentureMailOutbox) outbox = venture_mail_outbox_new(f->db, VENTURE_MAILER(mailer));
	g_autoptr(GDateTime) now = venture_time_now(), started = g_date_time_add_seconds(now, -10);
	g_autoptr(GHashTable) answers = venture_forms_answers_new();
	g_autoptr(VentureFormsStep) saved = NULL, again = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) result = NULL;
	g_autofree gchar *ticket = NULL, *seed = NULL, *serialized = NULL;
	(void)data;
	g_object_set(form, "allow-resume", TRUE, "public-origin", "https://forms.example.test", NULL); save(f, form);
	add_field(f, form, "name", "Name", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 10);
	add_field(f, form, "page", "Next", VENTURE_FORM_FIELD_PAGE_BREAK, FALSE, 20);
	add_field(f, form, "agree", "I agree", VENTURE_FORM_FIELD_CONSENT, TRUE, 30);
	g_object_unref(publish(f, form)); seed = language_seed(f, form, "en"); ticket = venture_forms_ticket_new(form, started);
	venture_forms_answers_add(answers, VENTURE_FORMS_TICKET, ticket); venture_forms_answers_add(answers, VENTURE_FORMS_PREFILL, seed);
	venture_forms_answers_add(answers, VENTURE_FORMS_MOVE, "save"); venture_forms_answers_add(answers, "name", "Unsubmitted secret");
	venture_forms_answers_add(answers, VENTURE_FORMS_RESUME_EMAIL, "  PRIVATE@example.test  ");
	saved = venture_forms_step(f->db, form, answers, NULL, now, &error);
	g_assert_no_error(error); g_assert_nonnull(saved); g_assert_nonnull(saved->resume_url);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 0);
	g_assert_cmpint(count(f, VENTURE_TYPE_CONTACT), ==, 0);
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
		g_autoptr(VentureEntity) message = venture_database_find_one(f->db, query, &error);
		g_autofree gchar *private_body = NULL;
		g_assert_no_error(error); g_assert_nonnull(message);
		private_body = venture_forms_get_string(message, "private-text-body"); g_assert_nonnull(strstr(private_body, saved->resume_url));
		result = venture_serializable_to_json(VENTURE_SERIALIZABLE(message), FALSE); serialized = json_to_string(result, FALSE);
		g_assert_null(strstr(serialized, saved->resume_url)); g_assert_null(strstr(serialized, "Unsubmitted secret"));
		g_clear_pointer(&result, json_node_unref); g_clear_pointer(&serialized, g_free);
	}
	/* A new token cannot be used to bypass the draft's email throttle. */
	g_hash_table_remove_all(answers); venture_forms_answers_add(answers, VENTURE_FORMS_DRAFT_TOKEN, saved->token);
	venture_forms_answers_add(answers, VENTURE_FORMS_MOVE, "save"); venture_forms_answers_add(answers, "name", "Unsubmitted secret");
	venture_forms_answers_add(answers, VENTURE_FORMS_RESUME_EMAIL, "private@example.test");
	again = venture_forms_step(f->db, form, answers, NULL, now, &error);
	g_assert_no_error(error); g_assert_nonnull(again); g_assert_null(again->resume_url); g_assert_cmpuint(json_object_get_size(again->errors), >, 0);
	g_assert_cmpint(count(f, VENTURE_TYPE_MAIL_MESSAGE), ==, 1);
	/* The destination inbox is enough to find the working copy for erasure;
	 * it need not also appear in a submitted question. Exports omit bearers. */
	result = venture_forms_export_person(f->db, f->org, "private@example.test", &error);
	g_assert_no_error(error); g_assert_nonnull(result);
	g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(result), "drafts")), ==, 1);
	serialized = json_to_string(result, FALSE); g_assert_null(strstr(serialized, "_vf_prefill")); g_assert_null(strstr(serialized, saved->resume_url));
	g_clear_pointer(&result, json_node_unref);
	result = venture_forms_erase_person(f->db, f->org, "private@example.test", NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(result);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_DRAFT_RECORD), ==, 0);
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
		g_autoptr(VentureEntity) message = venture_database_find_one(f->db, query, &error);
		g_autofree gchar *state = venture_forms_get_string(message, "state");
		g_assert_cmpstr(state, ==, "cancelled");
	}
}

static void
test_resume_http(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "resume-http", VENTURE_FORM_LIVE);
	g_autofree gchar *ticket = NULL, *body = NULL, *url = NULL, *token = NULL;
	const gchar *begin, *end;
	Reply reply = { 0, NULL, NULL, NULL, NULL };
	(void)data;
	g_object_set(form, "allow-resume", TRUE, "public-origin", "https://forms.example.test", NULL); save(f, form);
	add_field(f, form, "name", "Name", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 10);
	add_field(f, form, "page", "Next", VENTURE_FORM_FIELD_PAGE_BREAK, FALSE, 20);
	add_field(f, form, "email", "Email", VENTURE_FORM_FIELD_EMAIL, TRUE, 30);
	g_object_unref(publish(f, form)); start_http(f); ticket = old_ticket(form);
	body = g_strdup_printf("%s&_vf_move=save&name=Private+Alice", ticket);
	request(f, "/pub/form/resume-http", "application/x-www-form-urlencoded", body, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 200);
	begin = strstr(reply.body, "https://forms.example.test/pub/form/resume-http/resume/"); g_assert_nonnull(begin);
	begin += strlen("https://forms.example.test"); end = strchr(begin, '"'); g_assert_nonnull(end); url = g_strndup(begin, end - begin);
	token = page_token(reply.body); reply_clear(&reply);
	request(f, url, NULL, NULL, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 200); g_assert_null(strstr(reply.body, "Private Alice")); reply_clear(&reply);
	request(f, url, "application/x-www-form-urlencoded", "", NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 200); g_assert_nonnull(strstr(reply.body, "Private Alice"));
	g_assert_nonnull(strstr(reply.body, "action=\"/pub/form/resume-http\""));
	reply_clear(&reply);
	request(f, url, NULL, NULL, NULL, NULL, &reply); g_assert_cmpuint(reply.status, ==, 404); reply_clear(&reply);
	g_free(body); body = g_strdup_printf("_vf_draft=%s&name=Stale", token);
	g_assert_cmpuint(post_form(f, "/pub/form/resume-http", body), ==, 404);
	g_assert_cmpuint(post_form(f, "/pub/form/resume-http/resume/guessed", ""), ==, 404);
}

static void
test_resume_groups_expiry(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "resume-groups", VENTURE_FORM_LIVE);
	g_autoptr(VentureEntity) group = VENTURE_ENTITY(venture_form_group_new()), name = NULL;
	g_autoptr(VentureLogMailer) mailer = venture_log_mailer_new();
	g_autoptr(VentureMailOutbox) outbox = venture_mail_outbox_new(f->db, VENTURE_MAILER(mailer));
	g_autoptr(GDateTime) now = venture_time_now(), started = g_date_time_add_seconds(now, -10), later = g_date_time_add_days(now, 8);
	g_autoptr(GHashTable) answers = venture_forms_answers_new();
	g_autoptr(VentureFormsStep) saved = NULL, resumed = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(JsonNode) swept = NULL;
	g_autofree gchar *ticket = NULL;
	(void)data;
	g_object_set(form, "allow-resume", TRUE, "public-origin", "https://forms.example.test", NULL); save(f, form);
	g_object_set(group, "organization-id", f->org, "form-id", venture_entity_get_id(form), "label", "Attendee", "key", "attendee", "max-rows", (gint64)3, NULL); save(f, group);
	name = make_field(f, form, "name", "Name", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 10);
	g_object_set(name, "group-id", venture_entity_get_id(group), NULL); save(f, name);
	add_field(f, form, "page", "Next", VENTURE_FORM_FIELD_PAGE_BREAK, FALSE, 20);
	add_field(f, form, "agree", "I agree", VENTURE_FORM_FIELD_CONSENT, TRUE, 30);
	g_object_unref(publish(f, form)); ticket = venture_forms_ticket_new(form, started);
	venture_forms_answers_add(answers, VENTURE_FORMS_TICKET, ticket); venture_forms_answers_add(answers, VENTURE_FORMS_MOVE, "save");
	venture_forms_answers_add(answers, "attendee[0][_row]", "1"); venture_forms_answers_add(answers, "attendee[0][name]", "First");
	venture_forms_answers_add(answers, "attendee[1][_row]", "1"); venture_forms_answers_add(answers, "attendee[1][name]", "Second");
	venture_forms_answers_add(answers, VENTURE_FORMS_RESUME_EMAIL, "private@example.test");
	saved = venture_forms_step(f->db, form, answers, NULL, now, &error);
	g_assert_no_error(error); g_assert_nonnull(saved); g_assert_nonnull(saved->resume_url);
	g_object_set(group, "max-rows", (gint64)1, NULL); save(f, group); g_object_unref(publish(f, form));
	resumed = venture_forms_resume(f->db, form, strrchr(saved->resume_url, '/') + 1, now, TRUE, &error);
	g_assert_no_error(error); g_assert_nonnull(resumed);
	g_assert_cmpstr(json_object_get_string_member(resumed->values, "attendee[0][name]"), ==, "First");
	g_assert_false(json_object_has_member(resumed->values, "attendee[1][name]"));
	g_assert_cmpuint(json_object_get_size(resumed->errors), ==, 0);
	swept = venture_forms_retention_sweep(f->db, f->org, 1, later, NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(swept);
	g_assert_cmpint(json_object_get_int_member(json_node_get_object(swept), "expired_drafts"), ==, 1);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_DRAFT_RECORD), ==, 0);
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
		g_autoptr(VentureEntity) message = venture_database_find_one(f->db, query, &error);
		g_autofree gchar *state = venture_forms_get_string(message, "state");
		g_assert_cmpstr(state, ==, "cancelled");
	}
}

static VentureEntity *
quiz_form(Fixture *f, const gchar *token)
{
	VentureEntity *form = make_form(f, token, VENTURE_FORM_LIVE);
	g_autoptr(VentureEntity) choice = make_field(f, form, "answer", "Knowledge question", VENTURE_FORM_FIELD_SINGLE_CHOICE, TRUE, 10);
	g_autoptr(VentureEntity) number = make_field(f, form, "number", "Scale", VENTURE_FORM_FIELD_NUMBER, TRUE, 20);
	g_autoptr(VentureEntity) multiple = make_field(f, form, "multiple", "Multiple", VENTURE_FORM_FIELD_MULTIPLE_CHOICE, FALSE, 30);
	g_autoptr(VentureEntity) low = g_object_new(VENTURE_TYPE_FORM_RESULT_BAND, "organization-id", f->org, "form-id", venture_entity_get_id(form),
		"name", "Low", "key", "low", "minimum", (gint64)-1000, "maximum", (gint64)4, "message", "Keep learning", NULL);
	g_autoptr(VentureEntity) high = g_object_new(VENTURE_TYPE_FORM_RESULT_BAND, "organization-id", f->org, "form-id", venture_entity_get_id(form),
		"name", "High", "key", "high", "minimum", (gint64)5, "maximum", (gint64)1000, "message", "Plan B <recommended>", NULL);
	g_object_set(form, "quiz-enabled", TRUE, NULL); save(f, form);
	g_object_set(choice, "choices", "right | Right answer\nwrong | Wrong answer", "scoring", "{\"choices\":{\"right\":5,\"wrong\":-2},\"correct\":[\"right\"]}", NULL); save(f, choice);
	g_object_set(number, "scoring", "{\"ranges\":[{\"min\":0,\"max\":10,\"points\":3},{\"min\":11,\"max\":20,\"points\":6}]}", NULL); save(f, number);
	g_object_set(multiple, "choices", "a | A\nb | B", "scoring", "{\"choices\":{\"a\":2,\"b\":-1}}", NULL); save(f, multiple);
	save(f, low); save(f, high); g_object_unref(publish(f, form));
	return form;
}

static void
test_quiz_validation(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = quiz_form(f, "quiz-validation");
	g_autoptr(VentureEntity) field = make_field(f, form, "new", "New", VENTURE_FORM_FIELD_SINGLE_CHOICE, FALSE, 40);
	g_autoptr(VentureEntity) band = g_object_new(VENTURE_TYPE_FORM_RESULT_BAND, "organization-id", f->org, "form-id", venture_entity_get_id(form),
		"name", "Overlapping", "key", "overlap", "minimum", (gint64)4, "maximum", (gint64)10, NULL);
	(void)data;
	g_object_set(field, "choices", "a | A", "scoring", "{\"choices\":{\"unknown\":1}}", NULL); refuse(f, field, "Scoring");
	g_object_set(field, "scoring", "{\"choices\":{\"a\":1.5}}", NULL); refuse(f, field, "Scoring");
	g_object_set(field, "scoring", "{\"choices\":{\"a\":1000001}}", NULL); refuse(f, field, "Scoring");
	g_object_set(field, "scoring", "{\"correct\":[\"a\",\"a\"]}", NULL); refuse(f, field, "Scoring");
	g_object_set(field, "scoring", "{\"choices\":{\"a\":1}}", "sensitive", TRUE, NULL); refuse(f, field, "Scoring");
	g_object_set(field, "sensitive", FALSE, NULL); save(f, field);
	g_object_set(field, "sensitive", TRUE, NULL); refuse(f, field, "Scoring");
	refuse(f, band, "Result band");
	g_object_set(band, "minimum", (gint64)1001, "maximum", (gint64)2000, "redirect-url", "javascript:alert(1)", NULL); refuse(f, band, "Result band");
	g_object_set(band, "redirect-url", "https://example.test/result", NULL); save(f, band);
	g_object_set(band, "key", "changed", NULL); refuse(f, band, "Result band");
	{
		g_autoptr(VentureEntity) number = make_field(f, form, "range", "Range", VENTURE_FORM_FIELD_NUMBER, FALSE, 50);
		g_object_set(number, "scoring", "{\"ranges\":[{\"min\":0,\"max\":10,\"points\":1},{\"min\":10,\"max\":20,\"points\":2}]}", NULL);
		refuse(f, number, "Scoring");
		g_object_set(number, "scoring", "{\"ranges\":[{\"min\":true,\"max\":10,\"points\":1}]}", NULL); refuse(f, number, "Scoring");
	}
}

/* Scores use the published server declaration, including after author edits.
 * A score-looking POST member cannot replace a computed property. */
static void
test_quiz_score_and_key(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = quiz_form(f, "quiz-score"), response = NULL;
	g_autofree gchar *message = NULL, *schema_text = NULL;
	g_autoptr(JsonNode) schema = NULL;
	const gchar *pairs[] = { "answer", "right", "number", "5.5", "multiple", "a", "multiple", "b", NULL };
	const gchar *forged[] = { "answer", "wrong", "number", "5", "score", "999999", NULL };
	(void)data;
	g_assert_cmpint(submit_pairs(f, form, forged, NULL), ==, VENTURE_FORMS_INVALID);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 0);
	g_assert_cmpint(submit_pairs(f, form, pairs, NULL), ==, VENTURE_FORMS_ACCEPTED);
	response = last_form_response(f); g_assert_true(venture_forms_get_bool(response, "scored")); g_assert_cmpint(venture_forms_get_int(response, "score"), ==, 9);
	message = venture_forms_success_message(f->db, form, response); g_assert_nonnull(strstr(message, "Score: 9")); g_assert_nonnull(strstr(message, "Plan B <recommended>")); g_assert_null(strstr(message, "Correct answers"));
	g_object_set(response, "score", (gint64)100, NULL); refuse(f, response, "cannot be changed");
	g_clear_object(&response); g_clear_pointer(&message, g_free);
	g_object_set(form, "show-answer-key", TRUE, NULL); save(f, form);
	/* An unpublished setting does not change the key shown to respondents. */
	g_assert_cmpint(submit_pairs(f, form, pairs, NULL), ==, VENTURE_FORMS_ACCEPTED);
	response = last_form_response(f); message = venture_forms_success_message(f->db, form, response); g_assert_null(strstr(message, "Correct answers"));
	g_clear_object(&response); g_clear_pointer(&message, g_free); g_object_unref(publish(f, form));
	g_assert_cmpint(submit_pairs(f, form, pairs, NULL), ==, VENTURE_FORMS_ACCEPTED);
	response = last_form_response(f); message = venture_forms_success_message(f->db, form, response); g_assert_nonnull(strstr(message, "Correct answers: Right answer"));
	{
		g_autoptr(GDateTime) now = venture_time_now();
		schema = venture_forms_schema(f->db, form, "/pub/form/quiz-score", now, NULL);
	}
	schema_text = json_to_string(schema, FALSE); g_assert_null(strstr(schema_text, "\"scoring\"")); g_assert_null(strstr(schema_text, "\"correct\""));
	{
		g_autoptr(JsonObject) options = json_object_new();
		g_autoptr(VentureDateRange) period = venture_date_range_new_all_time();
		g_autoptr(VentureReportResult) report = NULL;
		g_autoptr(JsonNode) json = NULL;
		g_autoptr(GError) error = NULL;
		guint i, bins = 0;
		JsonArray *rows;
		json_object_set_int_member(options, "form_id", venture_entity_get_id(form));
		report = venture_report_generate(venture_report_registry_lookup(venture_context_get_report_registry(f->context), "form_summary"), f->context, period, options, &error);
		g_assert_no_error(error); json = venture_report_result_to_json(report); rows = json_object_get_array_member(json_node_get_object(json), "rows");
		for (i = 0; i < json_array_get_length(rows); i++)
		{
			JsonObject *row = json_array_get_object_element(rows, i);
			if (g_strcmp0(json_object_get_string_member(row, "question"), "Assessment score") != 0) continue;
			bins++; g_assert_cmpstr(json_object_get_string_member(row, "answer"), ==, "9");
			g_assert_cmpfloat(json_object_get_double_member(row, "share"), ==, 1.0);
		}
		g_assert_cmpuint(bins, ==, 2);
	}
}

static void
test_quiz_hidden_and_repeated(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = quiz_form(f, "quiz-hidden"), rule = NULL, response = NULL;
	g_autofree gchar *message = NULL;
	const gchar *hidden[] = { "gate", "skip", "answer", "right", "number", "5", NULL };
	(void)data;
	add_field(f, form, "gate", "Gate", VENTURE_FORM_FIELD_SHORT_TEXT, FALSE, 1);
	rule = make_rule(f, form, VENTURE_FORM_RULE_HIDE, "answer", "[{\"field\":\"gate\",\"operator\":\"equals\",\"value\":\"skip\"}]"); save(f, rule);
	g_object_set(form, "show-answer-key", TRUE, NULL); save(f, form); g_object_unref(publish(f, form));
	g_assert_cmpint(submit_pairs(f, form, hidden, NULL), ==, VENTURE_FORMS_ACCEPTED);
	response = last_form_response(f); g_assert_cmpint(venture_forms_get_int(response, "score"), ==, 3);
	message = venture_forms_success_message(f->db, form, response); g_assert_null(strstr(message, "Right answer"));
	g_clear_object(&form); g_clear_object(&response);
	form = make_form(f, "quiz-repeat", VENTURE_FORM_LIVE);
	g_object_set(form, "quiz-enabled", TRUE, NULL); save(f, form);
	{
		g_autoptr(VentureEntity) group = g_object_new(VENTURE_TYPE_FORM_GROUP, "organization-id", f->org, "form-id", venture_entity_get_id(form), "key", "attendee", "label", "Attendee", "max-rows", (gint64)2, NULL);
		g_autoptr(VentureEntity) answer = make_field(f, form, "answer", "Answer", VENTURE_FORM_FIELD_SINGLE_CHOICE, TRUE, 10);
		const gchar *rows[] = { "attendee[0][answer]", "yes", "attendee[1][answer]", "yes", NULL };
		save(f, group); g_object_set(answer, "group-id", venture_entity_get_id(group), "choices", "yes | Yes\nno | No", "scoring", "{\"choices\":{\"yes\":5}}", NULL); save(f, answer); g_object_unref(publish(f, form));
		g_assert_cmpint(submit_pairs(f, form, rows, NULL), ==, VENTURE_FORMS_ACCEPTED);
		response = last_form_response(f); g_assert_cmpint(venture_forms_get_int(response, "score"), ==, 10);
	}
}

static void
test_quiz_lead_input(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = quiz_form(f, "quiz-lead"), response = NULL, lead = NULL;
	g_autoptr(VentureEntity) base = g_object_new(VENTURE_TYPE_LEAD_SCORING_RULE, "organization-id", f->org, "name", "Base", "active", TRUE, "conditions", "", "points", (gint64)5, NULL);
	g_autoptr(VentureEntity) assessment = g_object_new(VENTURE_TYPE_LEAD_SCORING_RULE, "organization-id", f->org, "name", "Assessment", "active", TRUE, "conditions", "assessment_score=8", "points", (gint64)7, NULL);
	const gchar *off[] = { "name", "Off", "email", "off@example.test", "answer", "right", "number", "5", NULL };
	const gchar *on[] = { "name", "Original CRM name", "email", "on@example.test", "answer", "right", "number", "5", NULL };
	const gchar *again[] = { "name", "Do not overwrite", "email", "on@example.test", "answer", "wrong", "number", "5", NULL };
	(void)data;
	save(f, base); save(f, assessment);
	{
		g_autoptr(VentureEntity) name = make_field(f, form, "name", "Name", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 1);
		g_autoptr(VentureEntity) email = make_field(f, form, "email", "Email", VENTURE_FORM_FIELD_EMAIL, TRUE, 2);
		g_object_set(name, "maps-to", "name", NULL); save(f, name); g_object_set(email, "maps-to", "email", NULL); save(f, email);
	}
	g_object_set(form, "create-lead", TRUE, NULL); save(f, form); g_object_unref(publish(f, form));
	g_assert_cmpint(submit_pairs(f, form, off, NULL), ==, VENTURE_FORMS_ACCEPTED); response = last_form_response(f);
	lead = venture_database_get(f->db, VENTURE_TYPE_LEAD, venture_forms_get_int(response, "lead-id"), NULL); g_assert_nonnull(lead);
	g_assert_null(venture_entity_get_attribute(lead, "assessment_score")); g_assert_cmpint(venture_forms_get_int(lead, "score"), ==, 5);
	g_clear_object(&lead); g_clear_object(&response);
	g_object_set(form, "score-to-lead", TRUE, NULL); save(f, form); g_object_unref(publish(f, form));
	g_assert_cmpint(submit_pairs(f, form, on, NULL), ==, VENTURE_FORMS_ACCEPTED); response = last_form_response(f);
	lead = venture_database_get(f->db, VENTURE_TYPE_LEAD, venture_forms_get_int(response, "lead-id"), NULL); g_assert_nonnull(lead);
	g_assert_cmpstr(venture_entity_get_attribute(lead, "assessment_score"), ==, "8"); g_assert_cmpint(venture_forms_get_int(lead, "score"), ==, 12); g_assert_false(venture_forms_get_bool(lead, "score-manual"));
	g_clear_object(&response);
	g_assert_cmpint(submit_pairs(f, form, again, NULL), ==, VENTURE_FORMS_ACCEPTED);
	{
		g_autoptr(VentureEntity) current = reread(f, lead);
		g_autofree gchar *name = venture_forms_get_string(current, "name");
		g_assert_cmpstr(name, ==, "Original CRM name"); g_assert_cmpstr(venture_entity_get_attribute(current, "assessment_score"), ==, "1"); g_assert_cmpint(venture_forms_get_int(current, "score"), ==, 5);
		g_object_set(current, "score", (gint64)50, NULL); save(f, current);
	}
	g_assert_cmpint(submit_pairs(f, form, on, NULL), ==, VENTURE_FORMS_ACCEPTED);
	{
		g_autoptr(VentureEntity) current = reread(f, lead);
		g_assert_cmpint(venture_forms_get_int(current, "score"), ==, 50); g_assert_true(venture_forms_get_bool(current, "score-manual"));
	}
	/* A refused CRM follow-up keeps the response and its computed result. */
	g_object_set(form, "on-duplicate", "reject", NULL); save(f, form);
	g_assert_cmpint(submit_pairs(f, form, on, NULL), ==, VENTURE_FORMS_ACCEPTED);
	response = last_form_response(f);
	g_assert_true(venture_forms_get_bool(response, "scored")); g_assert_cmpint(venture_forms_get_int(response, "score"), ==, 8);
	g_clear_object(&response);
	{
		g_autoptr(VentureEntity) contact = g_object_new(VENTURE_TYPE_CONTACT, "organization-id", f->org, "name", "Existing contact", "email", "known@example.test", NULL);
		const gchar *known[] = { "name", "Known", "email", "known@example.test", "answer", "right", "number", "5", NULL };
		save(f, contact); g_object_set(form, "on-duplicate", "merge", NULL); save(f, form);
		g_assert_cmpint(submit_pairs(f, form, known, NULL), ==, VENTURE_FORMS_ACCEPTED); response = last_form_response(f);
		g_assert_cmpint(venture_forms_get_int(response, "lead-id"), ==, 0);
		g_assert_cmpint(venture_forms_get_int(response, "score"), ==, 8);
		g_assert_cmpint(count(f, VENTURE_TYPE_LEAD), ==, 2);
	}
}


/* Grading and translated results remain evidence of the published version;
 * retention removes derived personal results along with their input answers. */
static void
test_quiz_version_privacy(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = quiz_form(f, "quiz-version"), response = NULL;
	g_autofree gchar *seed = NULL, *message = NULL;
	g_autoptr(GPtrArray) fields = NULL;
	g_autoptr(JsonNode) exported = NULL, swept = NULL;
	g_autoptr(GDateTime) now = venture_time_now(), later = g_date_time_add_days(now, 40);
	g_autoptr(GError) error = NULL;
	const gchar *pairs[] = { "answer", "right", "number", "5", "email", "quiz@example.test", VENTURE_FORMS_PREFILL, NULL, NULL };
	guint i;
	(void)data;
	add_field(f, form, "email", "Email", VENTURE_FORM_FIELD_EMAIL, TRUE, 40);
	g_object_set(form, "show-answer-key", TRUE, "retention-days", (gint64)30, NULL); save(f, form);
	translate(f, form, "result.high.message", "Plan B recommandé");
	translate(f, form, "choice.answer.right", "Bonne réponse");
	translate(f, form, "message.correct_answers", "Réponses correctes");
	g_object_unref(publish(f, form)); seed = language_seed(f, form, "fr"); pairs[7] = seed;
	fields = venture_forms_fields(f->db, form, NULL);
	for (i = 0; i < fields->len; i++)
	{
		VentureEntity *field = g_ptr_array_index(fields, i);
		g_autofree gchar *key = venture_forms_get_string(field, "key");
		if (g_strcmp0(key, "answer") == 0)
		{ g_object_set(field, "scoring", "{\"choices\":{\"right\":100}}", NULL); save(f, field); }
	}
	g_assert_cmpint(submit_pairs(f, form, pairs, NULL), ==, VENTURE_FORMS_ACCEPTED);
	response = last_form_response(f); g_assert_cmpint(venture_forms_get_int(response, "score"), ==, 8);
	message = venture_forms_success_message(f->db, form, response);
	g_assert_nonnull(strstr(message, "Plan B recommandé")); g_assert_nonnull(strstr(message, "Réponses correctes: Bonne réponse"));
	exported = venture_forms_export_person(f->db, f->org, "quiz@example.test", &error); g_assert_no_error(error);
	{
		JsonObject *row = json_array_get_object_element(json_object_get_array_member(json_node_get_object(exported), "responses"), 0);
		g_assert_cmpint(json_object_get_int_member(row, "score"), ==, 8);
		g_assert_cmpstr(json_object_get_string_member(row, "result_key"), ==, "high");
	}
	g_object_unref(publish(f, form));
	g_clear_pointer(&seed, g_free); seed = language_seed(f, form, "fr"); pairs[7] = seed;
	g_assert_cmpint(submit_pairs(f, form, pairs, NULL), ==, VENTURE_FORMS_ACCEPTED);
	g_clear_object(&response); response = last_form_response(f); g_assert_cmpint(venture_forms_get_int(response, "score"), ==, 103);
	swept = venture_forms_retention_sweep(f->db, f->org, 10, later, NULL, &error); g_assert_no_error(error);
	{
		g_autoptr(VentureEntity) retained = reread(f, response);
		g_autofree gchar *result = venture_forms_get_string(retained, "result-message");
		g_assert_false(venture_forms_get_bool(retained, "scored")); g_assert_cmpint(venture_forms_get_int(retained, "score"), ==, 0);
		g_assert_true(venture_string_is_empty(result));
	}
}



/* A published band chooses the destination on both public transports;
 * the browser cannot substitute its own score or destination. */
static void
test_quiz_redirect(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = quiz_form(f, "quiz-redirect");
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_RESULT_BAND);
	g_autoptr(VentureEntity) band = NULL;
	g_autofree gchar *ticket = NULL, *body = NULL;
	Reply reply = { 0, NULL, NULL, NULL, NULL };
	(void)data;
	venture_query_add_filter_string(query, "key", VENTURE_FILTER_OP_EQ, "high", NULL);
	band = venture_database_find_one(f->db, query, NULL);
	g_object_set(band, "redirect-url", "https://example.test/plan-b", NULL); save(f, band);
	g_object_set(form, "redirect-url", "https://example.test/general", NULL); save(f, form);
	g_object_unref(publish(f, form)); ticket = old_ticket(form);
	body = g_strdup_printf("%s&answer=right&number=5", ticket);
	start_http(f);
	request(f, "/pub/form/quiz-redirect", "application/x-www-form-urlencoded", body, NULL, "text/html", &reply);
	g_assert_cmpuint(reply.status, ==, 303); g_assert_cmpstr(reply.location, ==, "https://example.test/plan-b"); reply_clear(&reply);
	request(f, "/pub/form/quiz-redirect", "application/x-www-form-urlencoded", body, NULL, "application/json", &reply);
	g_assert_cmpuint(reply.status, ==, 200); g_assert_nonnull(strstr(reply.body, "https://example.test/plan-b")); reply_clear(&reply);
}

/* Price edits must not alter an order filled against a published version.
 * Client amount/currency members are never consulted by the calculator. */
static void test_payment_pricing(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "paid", VENTURE_FORM_LIVE), version = NULL;
	g_autoptr(VentureEntity) product = g_object_new(VENTURE_TYPE_PRODUCT, "organization-id", f->org, "name", "Admission", NULL);
	g_autoptr(VentureEntity) price = NULL, choice = NULL;
	g_autoptr(VentureMoney) unit = venture_money_new_for_currency(125, "USD"), total = NULL;
	g_autoptr(GPtrArray) fields = NULL;
	g_autoptr(JsonObject) answers = json_object_new();
	g_autoptr(JsonArray) lines = NULL;
	g_autoptr(GError) error = NULL;
	(void)data;
	save(f, product);
	add_field(f, form, "name", "Name", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 10);
	add_field(f, form, "email", "Email", VENTURE_FORM_FIELD_EMAIL, TRUE, 20);
	add_field(f, form, "quantity", "Quantity", VENTURE_FORM_FIELD_NUMBER, TRUE, 30);
	choice = make_field(f, form, "ticket", "Ticket", VENTURE_FORM_FIELD_SINGLE_CHOICE, TRUE, 40);
	g_object_set(choice, "choices", "standard | Standard\npremium | Premium", NULL); save(f, choice);
	g_object_set(form, "payment-enabled", TRUE, "public-origin", "https://forms.example.test", NULL); save(f, form);
	price = g_object_new(VENTURE_TYPE_FORM_PRICE, "organization-id", f->org, "form-id", venture_entity_get_id(form),
		"name", "Admission", "key", "admission", "product-id", venture_entity_get_id(product), "unit-price", unit,
		"choice-field", "ticket", "choice-id", "premium", "quantity-field", "quantity", NULL); save(f, price);
	version = publish(f, form);
	fields = venture_forms_definition_for(f->db, form, version, &error); g_assert_no_error(error);
	json_object_set_string_member(answers, "ticket", "premium"); json_object_set_int_member(answers, "quantity", 3);
	json_object_set_int_member(answers, "amount", 1); json_object_set_string_member(answers, "currency", "EUR");
	total = venture_forms_price_total(fields, answers, &lines, &error); g_assert_no_error(error);
	g_assert_cmpint(venture_money_get_amount(total), ==, 375); g_assert_cmpstr(venture_money_get_currency(total), ==, "USD");
	g_assert_cmpuint(json_array_get_length(lines), ==, 1);
	g_clear_pointer(&unit, venture_money_free); unit = venture_money_new_for_currency(9999, "USD");
	g_object_set(price, "unit-price", unit, NULL); save(f, price);
	g_clear_pointer(&total, venture_money_free); total = venture_forms_price_total(fields, answers, NULL, &error);
	g_assert_no_error(error); g_assert_cmpint(venture_money_get_amount(total), ==, 375);
	g_clear_pointer(&total, venture_money_free);
	json_object_set_double_member(answers, "quantity", 1.5);
	total = venture_forms_price_total(fields, answers, NULL, &error); g_assert_null(total); g_assert_nonnull(error); g_clear_error(&error);
	json_object_set_int_member(answers, "quantity", 1000001);
	total = venture_forms_price_total(fields, answers, NULL, &error); g_assert_null(total); g_assert_nonnull(error); g_clear_error(&error);
	json_object_set_int_member(answers, "quantity", 1); json_object_set_string_member(answers, "ticket", "standard");
	total = venture_forms_price_total(fields, answers, NULL, &error); g_assert_null(total); g_assert_nonnull(error); g_clear_error(&error);
	{
		g_autoptr(JsonObject) malformed = json_object_new();
		g_assert_false(venture_forms_price_restore(fields, malformed, &error)); g_assert_nonnull(error); g_clear_error(&error);
	}
	/* A second currency and a deleted/unknown choice cannot be published. */
	g_clear_pointer(&unit, venture_money_free); unit = venture_money_new_for_currency(100, "EUR");
	{
		g_autoptr(VentureEntity) second = g_object_new(VENTURE_TYPE_FORM_PRICE, "organization-id", f->org, "form-id", venture_entity_get_id(form),
			"name", "Fee", "key", "fee", "product-id", venture_entity_get_id(product), "unit-price", unit, NULL);
		g_autoptr(VentureEntity) refused_version = NULL;
		save(f, second); refused_version = venture_forms_publish(f->db, form, NULL, &error);
		g_assert_null(refused_version); g_assert_nonnull(error); g_clear_error(&error);
	}
}

/* Two completed intakes can carry the same offered slot. Availability must
 * be decided under the final write lock, before any contact or meeting. */
static void test_booking_form(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "booking-form", VENTURE_FORM_LIVE), page = NULL, field = NULL, response = NULL;
	g_autoptr(VentureBookingService) service = venture_booking_service_new(f->db);
	g_autoptr(JsonNode) slots = NULL, schema = NULL;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *start = NULL, *ticket = NULL, *body = NULL, *schema_text = NULL;
	Reply reply = { 0, NULL, NULL, NULL, NULL };
	(void)data;
	g_object_set(form, "public-origin", "https://forms.example.test", NULL); save(f, form);
	add_field(f, form, "name", "Name", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 10);
	add_field(f, form, "email", "Email", VENTURE_FORM_FIELD_EMAIL, TRUE, 20);
	page = g_object_new(VENTURE_TYPE_BOOKING_PAGE, "organization-id", f->org, "title", "Consultation", "slug", "forms-consultation", "owner", "owner",
		"duration-minutes", (gint64)30, "timezone", "UTC", "horizon-days", (gint64)2, "active", TRUE,
		"availability", "{\"mon\":\"00:00-23:30\",\"tue\":\"00:00-23:30\",\"wed\":\"00:00-23:30\",\"thu\":\"00:00-23:30\",\"fri\":\"00:00-23:30\",\"sat\":\"00:00-23:30\",\"sun\":\"00:00-23:30\"}", NULL);
	save(f, page);
	field = g_object_new(VENTURE_TYPE_FORM_FIELD, "organization-id", f->org, "form-id", venture_entity_get_id(form), "key", "time", "label", "Consultation time",
		"kind", VENTURE_FORM_FIELD_BOOKING, "required", TRUE, "position", (gint64)30, "booking-page-id", venture_entity_get_id(page), NULL);
	save(f, field); g_object_unref(publish(f, form));
	slots = venture_booking_service_slots(service, page, now, &error); g_assert_no_error(error);
	g_assert_cmpuint(json_array_get_length(json_node_get_array(slots)), >, 0);
	start = g_strdup(json_object_get_string_member(json_array_get_object_element(json_node_get_array(slots), 0), "start"));
	schema = venture_forms_schema(f->db, form, "/pub/form/booking-form", now, &error); g_assert_no_error(error);
	schema_text = json_to_string(schema, FALSE); g_assert_nonnull(strstr(schema_text, "\"slots\"")); g_assert_nonnull(strstr(schema_text, start));
	start_http(f); request(f, "/pub/form/booking-form", NULL, NULL, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 200); g_assert_nonnull(strstr(reply.body, "vf-slot")); g_assert_nonnull(strstr(reply.body, "type=\"radio\"")); reply_clear(&reply);
	g_assert_cmpint(count(f, VENTURE_TYPE_BOOKING_RESERVATION), ==, 0);
	ticket = old_ticket(form);
	{
		g_autofree gchar *encoded = g_uri_escape_string(start, NULL, TRUE);
		body = g_strdup_printf("%s&name=First&email=first%%40example.test&time=%s", ticket, encoded);
	}
	request(f, "/pub/form/booking-form", "application/x-www-form-urlencoded", body, NULL, "text/html", &reply);
	g_assert_cmpuint(reply.status, ==, 200); g_assert_nonnull(strstr(reply.body, "vf-success")); reply_clear(&reply);
	response = last_form_response(f); g_assert_cmpint(venture_forms_get_int(response, "booking-id"), >, 0);
	request(f, "/pub/form/booking-form", "application/x-www-form-urlencoded", body, NULL, "application/json", &reply);
	g_assert_cmpuint(reply.status, ==, 422); g_assert_nonnull(strstr(reply.body, "Choose another slot")); reply_clear(&reply);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 1); g_assert_cmpint(count(f, VENTURE_TYPE_ACTIVITY), ==, 1);
	g_object_set(response, "booking-id", (gint64)0, NULL); refuse(f, response, "cannot be changed");
	/* Scanner-safe GET, signed explicit cancellation, and replay refusal use
	 * the capability delivered through the private outbox body. */
	{
		g_autoptr(VentureQuery) mail = venture_query_new(VENTURE_TYPE_MAIL_MESSAGE);
		g_autoptr(VentureEntity) message = NULL;
		g_autofree gchar *private_body = NULL, *cancel_path = NULL;
		g_autoptr(GUri) uri = NULL;
		const gchar *url;
		venture_query_add_filter_string(mail, "related-type", VENTURE_FILTER_OP_EQ, "booking_reservation", NULL);
		message = venture_database_find_one(f->db, mail, &error); g_assert_no_error(error); g_assert_nonnull(message);
		private_body = venture_forms_get_string(message, "private-text-body");
		url = strstr(private_body, "https://forms.example.test/book/"); g_assert_nonnull(url);
		uri = g_uri_parse(url, G_URI_FLAGS_NONE, &error); g_assert_no_error(error); cancel_path = g_strdup(g_uri_get_path(uri));
		request(f, cancel_path, NULL, NULL, NULL, NULL, &reply); g_assert_cmpuint(reply.status, ==, 200);
		g_assert_nonnull(strstr(reply.body, "Cancel booking")); g_assert_null(strstr(reply.body, "first@example.test")); reply_clear(&reply);
		request(f, cancel_path, "application/x-www-form-urlencoded", "operation=cancel", NULL, NULL, &reply);
		g_assert_cmpuint(reply.status, ==, 200); g_assert_nonnull(strstr(reply.body, "cancelled")); reply_clear(&reply);
		request(f, cancel_path, NULL, NULL, NULL, NULL, &reply); g_assert_cmpuint(reply.status, ==, 404); reply_clear(&reply);
	}

	/* Navigating into a booking page keeps a private draft, not a seat. */
	add_field(f, form, "next", "Choose a time", VENTURE_FORM_FIELD_PAGE_BREAK, FALSE, 25);
	g_object_unref(publish(f, form));
	g_clear_pointer(&ticket, g_free); ticket = old_ticket(form);
	g_clear_pointer(&body, g_free); body = g_strdup_printf("%s&name=Draft&email=draft%%40example.test&_vf_move=next", ticket);
	request(f, "/pub/form/booking-form", "application/x-www-form-urlencoded", body, NULL, "text/html", &reply);
	g_assert_cmpuint(reply.status, ==, 200); g_assert_nonnull(strstr(reply.body, "vf-slot")); reply_clear(&reply);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_DRAFT_RECORD), ==, 1);
	g_assert_cmpint(count(f, VENTURE_TYPE_BOOKING_RESERVATION), ==, 1);

}

/* Every shipped template goes through the public validator, not just the
 * JSON parser. Newsletter intake must remain pending until inbox ownership. */
static void
test_portable_templates(Fixture *f, gconstpointer data)
{
	g_autoptr(JsonNode) templates = venture_forms_templates();
	guint i;
	(void)data;
	g_assert_cmpuint(json_array_get_length(json_node_get_array(templates)), ==, 8);
	for (i = 0; i < json_array_get_length(json_node_get_array(templates)); i++)
	{
		JsonObject *item = json_array_get_object_element(json_node_get_array(templates), i);
		g_autofree gchar *text = json_to_string(json_object_get_member(item, "definition"), FALSE), *html = NULL;
		g_autoptr(GError) error = NULL;
		g_autoptr(VentureEntity) form = venture_forms_import_definition(f->context, f->org, text, NULL, NULL, &error);
		g_autoptr(GPtrArray) fields = NULL, pairs = g_ptr_array_new();
		guint j;
		g_assert_no_error(error); g_assert_nonnull(form);
		g_assert_cmpint(venture_forms_get_int(form, "state"), ==, VENTURE_FORM_DRAFT);
		g_assert_cmpint(venture_forms_get_int(form, "retention-days"), ==, 90);
		g_object_set(form, "state", VENTURE_FORM_LIVE, "public-origin", "https://forms.example.test", NULL); save(f, form);
		g_object_unref(publish(f, form)); html = render(f, form, VENTURE_FORMS_RENDER_FRAGMENT); g_assert_nonnull(strstr(html, "vf-form"));
		{ g_autofree gchar *problems = a11y_check(html); g_assert_cmpstr(problems, ==, ""); }
		fields = venture_forms_definition_from_records(f->db, form, &error); g_assert_no_error(error);
		for (j = 0; j < fields->len; j++)
		{
			VentureFormsField *field = g_ptr_array_index(fields, j);
			const gchar *answer = "A useful answer";
			if (field->kind == VENTURE_FORM_FIELD_FILE) continue;
			switch (field->kind)
			{
			case VENTURE_FORM_FIELD_EMAIL: answer = "applicant@example.test"; break;
			case VENTURE_FORM_FIELD_RATING: case VENTURE_FORM_FIELD_NUMBER: answer = "3"; break;
			case VENTURE_FORM_FIELD_DATE: answer = "2027-01-15"; break;
			case VENTURE_FORM_FIELD_URL: answer = "https://example.test/resume"; break;
			case VENTURE_FORM_FIELD_CONSENT: answer = "on"; break;
			default: break;
			}
			g_ptr_array_add(pairs, field->key); g_ptr_array_add(pairs, (gpointer)answer);
		}
		g_ptr_array_add(pairs, NULL);
		if (venture_forms_get_bool(form, "double-opt-in"))
		{
			g_autofree gchar *token = NULL;
			g_autoptr(VentureEntity) response = NULL;
			g_autoptr(GDateTime) now = venture_time_now();
			g_assert_cmpint(submit_pairs(f, form, (const gchar *const *)pairs->pdata, NULL), ==, VENTURE_FORMS_PENDING);
			token = optin_latest_token(f); response = venture_forms_confirm_signup(f->db, form, token, now, TRUE, &error);
			g_assert_no_error(error); g_assert_nonnull(response);
		}
		else g_assert_cmpint(submit_pairs(f, form, (const gchar *const *)pairs->pdata, NULL), ==, VENTURE_FORMS_ACCEPTED);
	}
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 8);
}

static void
test_portable_roundtrip(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "portable-source", VENTURE_FORM_DRAFT), group = NULL, field = NULL, rule = NULL, translation = NULL, copied = NULL;
	g_autoptr(JsonNode) exported = NULL, second = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *json = NULL, *yaml = NULL, *again = NULL, *source_token = NULL, *target_token = NULL;
	(void)data;
	add_field(f, form, "gate", "Show details?", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 1);
	add_field(f, form, "page2", "Details", VENTURE_FORM_FIELD_PAGE_BREAK, FALSE, 2);
	group = g_object_new(VENTURE_TYPE_FORM_GROUP, "organization-id", f->org, "form-id", venture_entity_get_id(form), "key", "people", "label", "People", "max-rows", (gint64)3, NULL); save(f, group);
	field = make_field(f, form, "person", "Person", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 3);
	g_object_set(field, "group-id", venture_entity_get_id(group), NULL); save(f, field);
	rule = make_rule(f, form, VENTURE_FORM_RULE_SHOW, "person", "[{\"field\":\"gate\",\"operator\":\"equals\",\"value\":\"yes\"}]"); save(f, rule);
	translation = make_translation(f, form, "fr", "field.person.label", "Personne"); save(f, translation);
	exported = venture_forms_export_definition(f->db, form, &error); g_assert_no_error(error);
	json = venture_forms_definition_format(exported, "json", &error); g_assert_no_error(error);
	yaml = venture_forms_definition_format(exported, "yaml", &error); g_assert_no_error(error);
	g_assert_null(strstr(json, "portable-source")); g_assert_null(strstr(json, "ticket_key")); g_assert_null(strstr(json, "organization_id"));
	copied = venture_forms_import_definition(f->context, f->org, yaml, NULL, NULL, &error); g_assert_no_error(error); g_assert_nonnull(copied);
	second = venture_forms_export_definition(f->db, copied, &error); g_assert_no_error(error);
	again = venture_forms_definition_format(second, "json", &error); g_assert_no_error(error); g_assert_cmpstr(json, ==, again);
	source_token = venture_forms_get_string(form, "public-token"); target_token = venture_forms_get_string(copied, "public-token"); g_assert_cmpstr(source_token, !=, target_token);
	g_clear_object(&copied); copied = venture_forms_import_definition(f->context, f->org, json, NULL, NULL, &error); g_assert_no_error(error); g_assert_nonnull(copied);
	g_object_unref(publish(f, copied));
}

static void
test_portable_refusals(Fixture *f, gconstpointer data)
{
	static const gchar *const bad[] = {
		"&root {self: *root}", "---\na: 1\n---\nb: 2", "[]",
		"{\"format\":\"venture-form\",\"version\":2,\"form\":{\"name\":\"Bad\"}}",
		"{\"format\":\"venture-form\",\"version\":1,\"form\":{\"name\":\"Bad\",\"ticket_key\":\"forged\"}}",
		"{\"format\":\"venture-form\",\"version\":1,\"form\":{\"name\":\"Bad\",\"state\":\"live\"}}",
		"{\"format\":\"venture-form\",\"version\":1,\"form\":{\"name\":\"Bad\",\"retention_days\":\"forever\"}}",
		"{\"format\":\"venture-form\",\"version\":1,\"form\":{\"name\":\"Bad\"},\"fields\":[{\"key\":\"x\",\"label\":\"X\",\"kind\":\"made_up\"}]}",
		"{\"format\":\"venture-form\",\"version\":1,\"form\":{\"name\":\"Bad\"},\"fields\":[{\"key\":\"x\",\"label\":\"X\",\"group\":\"missing\"}]}", NULL
	};
	guint i;
	(void)data;
	for (i = 0; bad[i] != NULL; i++)
	{
		g_autoptr(GError) error = NULL;
		g_autoptr(VentureEntity) form = venture_forms_import_definition(f->context, f->org, bad[i], NULL, NULL, &error);
		g_assert_null(form); g_assert_nonnull(error); g_assert_cmpint(count(f, VENTURE_TYPE_FORM), ==, 0);
	}
}

static void
test_portable_bindings(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "binding-source", VENTURE_FORM_DRAFT), venture = g_object_new(VENTURE_TYPE_VENTURE, "organization-id", f->org, "name", "Destination", NULL), copied = NULL;
	g_autoptr(JsonNode) exported = NULL;
	g_autoptr(JsonObject) bindings = json_object_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL, *binding = NULL;
	(void)data;
	save(f, venture); g_object_set(form, "venture-id", venture_entity_get_id(venture), NULL); save(f, form);
	add_field(f, form, "name", "Name", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 1);
	exported = venture_forms_export_definition(f->db, form, &error); g_assert_no_error(error); text = json_to_string(exported, FALSE);
	copied = venture_forms_import_definition(f->context, f->org, text, NULL, NULL, &error); g_assert_null(copied); g_assert_nonnull(error); g_clear_error(&error);
	binding = g_strdup_printf("venture:%" G_GINT64_FORMAT, venture_entity_get_id(venture)); json_object_set_int_member(bindings, binding, venture_entity_get_id(venture));
	copied = venture_forms_import_definition(f->context, f->org, text, bindings, NULL, &error); g_assert_no_error(error); g_assert_nonnull(copied);
	g_assert_cmpint(venture_forms_get_int(copied, "venture-id"), ==, venture_entity_get_id(venture));
	g_clear_object(&copied); copied = venture_forms_import_definition(f->context, f->org + 1, text, bindings, NULL, &error); g_assert_null(copied); g_assert_nonnull(error);
}

static void
test_portable_prices(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "price-portable", VENTURE_FORM_DRAFT), product = g_object_new(VENTURE_TYPE_PRODUCT, "organization-id", f->org, "name", "Workshop", NULL), price = NULL, copied = NULL;
	g_autoptr(VentureMoney) unit = venture_money_new(12345, "USD", 2);
	g_autoptr(JsonNode) exported = NULL, again = NULL;
	g_autoptr(JsonObject) bindings = json_object_new();
	g_autoptr(GError) error = NULL;
	g_autofree gchar *text = NULL, *binding = NULL;
	(void)data;
	save(f, product);
	add_field(f, form, "name", "Name", VENTURE_FORM_FIELD_SHORT_TEXT, TRUE, 1);
	add_field(f, form, "email", "Email", VENTURE_FORM_FIELD_EMAIL, TRUE, 2);
	price = g_object_new(VENTURE_TYPE_FORM_PRICE, "organization-id", f->org, "form-id", venture_entity_get_id(form), "product-id", venture_entity_get_id(product), "key", "seat", "name", "Seat", "unit-price", unit, NULL); save(f, price);
	g_object_set(form, "payment-enabled", TRUE, "public-origin", "https://forms.example.test", NULL); save(f, form);
	exported = venture_forms_export_definition(f->db, form, &error); g_assert_no_error(error);
	text = venture_forms_definition_format(exported, "yaml", &error); g_assert_no_error(error);
	binding = g_strdup_printf("product:%" G_GINT64_FORMAT, venture_entity_get_id(product)); json_object_set_int_member(bindings, binding, venture_entity_get_id(product));
	copied = venture_forms_import_definition(f->context, f->org, text, bindings, NULL, &error); g_assert_no_error(error); g_assert_nonnull(copied);
	again = venture_forms_export_definition(f->db, copied, &error); g_assert_no_error(error);
	{
		JsonObject *line = json_array_get_object_element(json_object_get_array_member(json_node_get_object(again), "prices"), 0);
		JsonObject *money = json_object_get_object_member(line, "unit_price");
		g_assert_cmpint(json_object_get_int_member(money, "amount"), ==, 12345);
		g_assert_cmpstr(json_object_get_string_member(money, "currency"), ==, "USD");
	}
	/* A decimal cannot be rounded silently into the invoice's minor units. */
	g_clear_object(&copied); g_clear_pointer(&text, g_free);
	json_object_set_double_member(json_object_get_object_member(json_array_get_object_element(json_object_get_array_member(json_node_get_object(exported), "prices"), 0), "unit_price"), "amount", 12.345);
	text = json_to_string(exported, FALSE); copied = venture_forms_import_definition(f->context, f->org, text, bindings, NULL, &error);
	g_assert_null(copied); g_assert_nonnull(error);
}


/* Multipart bodies are binary and may repeat keys: a URL-encoded helper
 * would hide both classes of upload regression. */
static guint
upload_post(Fixture *f, VentureEntity *form, const gchar *contents, gsize size,
	const gchar *filename, guint copies, gchar **response_body)
{
	g_autofree gchar *token = venture_forms_get_string(form, "public-token");
	g_autofree gchar *uri = g_strdup_printf("http://127.0.0.1:%u/pub/form/%s", f->port, token);
	g_autoptr(SoupMessage) message = soup_message_new("POST", uri);
	g_autoptr(SoupMultipart) multipart = soup_multipart_new("multipart/form-data");
	g_autoptr(GDateTime) now = venture_time_now(), issued = g_date_time_add_seconds(now, -60);
	g_autofree gchar *ticket = venture_forms_ticket_new(form, issued), *content_type = NULL;
	g_autoptr(GBytes) file = g_bytes_new(contents, size), body = NULL;
	Pending pending = { FALSE, NULL, NULL };
	guint i;
	gsize length;
	const gchar *bytes;
	soup_multipart_append_form_string(multipart, VENTURE_FORMS_TICKET, ticket);
	soup_multipart_append_form_string(multipart, "email", "files@example.test");
	for (i = 0; i < copies; i++) soup_multipart_append_form_file(multipart, "attachment", filename, "image/png", file);
	soup_multipart_to_message(multipart, soup_message_get_request_headers(message), &body);
	content_type = g_strdup(soup_message_headers_get_one(soup_message_get_request_headers(message), "Content-Type"));
	soup_message_set_request_body_from_bytes(message, content_type, body);
	soup_message_headers_replace(soup_message_get_request_headers(message), "Accept", "application/json");
	soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
	soup_session_send_and_read_async(f->session, message, G_PRIORITY_DEFAULT, NULL, received, &pending);
	while (!pending.done) g_main_context_iteration(NULL, TRUE);
	g_assert_no_error(pending.error);
	bytes = g_bytes_get_data(pending.body, &length);
	if (response_body != NULL) *response_body = g_strndup(bytes, length);
	g_bytes_unref(pending.body);
	return soup_message_get_status(message);
}

static VentureEntity *
upload_form(Fixture *f)
{
	VentureEntity *form = make_form(f, "upload-form", VENTURE_FORM_LIVE);
	add_field(f, form, "attachment", "Attachment", VENTURE_FORM_FIELD_FILE, TRUE, 1);
	add_field(f, form, "email", "Email", VENTURE_FORM_FIELD_EMAIL, TRUE, 2);
	g_object_unref(publish(f, form));
	return form;
}

static void
test_upload_http(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = upload_form(f), upload = NULL, document = NULL, response = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_UPLOAD);
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(JsonNode) erased = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *body = NULL, *path = NULL, *url = NULL, *root = NULL, *answers = NULL, *hash = NULL;
	Reply reply = { 0, NULL, NULL, NULL, NULL };
	(void)data;
	f->open = TRUE; start_http(f);
	request(f, "/pub/form/upload-form", NULL, NULL, NULL, NULL, &reply);
	g_assert_cmpuint(reply.status, ==, 200); g_assert_nonnull(strstr(reply.body, "multipart/form-data"));
	g_assert_nonnull(strstr(reply.body, "type=\"file\"")); reply_clear(&reply);
	g_assert_cmpuint(upload_post(f, form, "private file body\n", 18, "../../<resume>.txt", 1, &body), ==, 200);
	g_assert_nonnull(strstr(body, "true"));
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 1);
	g_assert_cmpint(count(f, VENTURE_TYPE_DOCUMENT), ==, 1);
	upload = venture_database_find_one(f->db, query, &error); g_assert_no_error(error); g_assert_nonnull(upload);
	path = venture_forms_get_string(upload, "path"); hash = venture_forms_get_string(upload, "token-hash");
	g_assert_true(g_file_test(path, G_FILE_TEST_IS_REGULAR)); g_assert_null(strstr(path, "resume")); g_assert_true(venture_string_is_empty(hash));
	g_assert_cmpstr(venture_forms_get_string(upload, "mime-type"), ==, "text/plain");
	response = venture_database_get(f->db, VENTURE_TYPE_FORM_SUBMISSION, venture_forms_get_int(upload, "response-id"), &error); g_assert_no_error(error);
	answers = venture_forms_get_string(response, "answers"); g_assert_nonnull(strstr(answers, "document_id")); g_assert_null(strstr(answers, "private file body"));
	document = venture_database_get(f->db, VENTURE_TYPE_DOCUMENT, venture_forms_get_int(upload, "document-id"), &error); g_assert_no_error(error);
	root = g_build_filename(f->state_dir, "attachments", NULL);
	/* Generic attachment consumers include AI image and OCR tools. They must
	 * not turn a guessed document ID into an alternative download door. */
	bytes = venture_document_service_read_attachment(venture_document_service_get(f->db), document, root, 1000, &error);
	g_assert_null(bytes); g_assert_nonnull(error); g_clear_error(&error);
	url = g_strdup_printf("/forms/uploads/%" G_GINT64_FORMAT, venture_entity_get_id(upload));
	request(f, url, NULL, NULL, NULL, NULL, &reply); g_assert_cmpuint(reply.status, ==, 200);
	g_assert_cmpstr(reply.body, ==, "private file body\n"); g_assert_nonnull(strstr(reply.content_type, "text/plain")); reply_clear(&reply);
	g_object_set(document, "form-upload-id", (gint64)0, NULL); refuse(f, document, "identities");
	erased = venture_forms_erase_person(f->db, f->org, "files@example.test", NULL, &error);
	g_assert_no_error(error); g_assert_nonnull(erased); g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_UPLOAD), ==, 0); g_assert_cmpint(count(f, VENTURE_TYPE_DOCUMENT), ==, 0);
	request(f, url, NULL, NULL, NULL, NULL, &reply); g_assert_cmpuint(reply.status, ==, 404); reply_clear(&reply);
}

static gboolean
upload_scanner_refuse(GBytes *bytes, const gchar *mime, gpointer data, GError **error)
{
	guint *calls = data;
	(void)error;
	g_assert_cmpuint(g_bytes_get_size(bytes), >, 0); g_assert_cmpstr(mime, ==, "text/plain"); (*calls)++;
	return FALSE;
}

static void
test_upload_refusals(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = upload_form(f), field = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_FIELD);
	g_autofree gchar *oversize = g_strnfill(5 * 1024 * 1024 + 1, 'a');
	const gchar *bad[] = { "MZpretend executable", "\177ELFrenamed", "<html>not a photo", "<svg>not a photo", "#!/bin/sh\nexit" };
	guint i, calls = 0;
	(void)data;
	start_http(f);
	for (i = 0; i < G_N_ELEMENTS(bad); i++) g_assert_cmpuint(upload_post(f, form, bad[i], strlen(bad[i]), "photo.png", 1, NULL), ==, 422);
	g_assert_cmpuint(upload_post(f, form, oversize, strlen(oversize), "large.txt", 1, NULL), ==, 422);
	g_assert_cmpuint(upload_post(f, form, "hello", 5, "file.txt", 2, NULL), ==, 422);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_UPLOAD), ==, 0); g_assert_cmpint(count(f, VENTURE_TYPE_DOCUMENT), ==, 0);
	g_object_set(form, "upload-quota-bytes", (gint64)4, NULL); save(f, form);
	g_assert_cmpuint(upload_post(f, form, "hello", 5, "file.txt", 1, NULL), ==, 422);
	g_object_set(form, "upload-quota-bytes", (gint64)1000, "upload-client-hourly-bytes", (gint64)4, NULL); save(f, form);
	g_assert_cmpuint(upload_post(f, form, "hello", 5, "file.txt", 1, NULL), ==, 422);
	g_object_set(form, "upload-client-hourly-bytes", (gint64)1000, NULL); save(f, form);
	venture_forms_set_upload_scanner(f->db, upload_scanner_refuse, &calls, NULL);
	g_assert_cmpuint(upload_post(f, form, "hello", 5, "file.txt", 1, NULL), ==, 422); g_assert_cmpuint(calls, ==, 1);
	venture_forms_set_upload_scanner(f->db, NULL, NULL, NULL);
	venture_query_add_filter_string(query, "key", VENTURE_FILTER_OP_EQ, "attachment", NULL); field = venture_database_find_one(f->db, query, NULL);
	g_object_set(field, "file-types", "application/pdf", NULL); save(f, field); g_object_unref(publish(f, form));
	g_assert_cmpuint(upload_post(f, form, "hello", 5, "pretend.pdf", 1, NULL), ==, 422);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_UPLOAD), ==, 0); g_assert_cmpint(count(f, VENTURE_TYPE_FORM_SUBMISSION), ==, 0);
}

static void
upload_receive(Fixture *f, VentureEntity *form, VentureEntity *version, GHashTable *answers,
	const gchar *key, GDateTime *now)
{
	g_autoptr(GPtrArray) fields = venture_forms_definition_for(f->db, form, version, NULL);
	g_autoptr(GPtrArray) parts = g_ptr_array_new_with_free_func(venture_forms_upload_part_free);
	g_autoptr(GError) error = NULL;
	VentureFormsUploadPart *part = g_new0(VentureFormsUploadPart, 1);
	part->key = g_strdup(key); part->filename = g_strdup("resume.txt"); part->bytes = g_bytes_new_static("resume\n", 7);
	g_ptr_array_add(parts, part);
	g_assert_true(venture_forms_receive_uploads(f->db, form, version, fields, answers, parts, "127.0.0.1", now, &error)); g_assert_no_error(error);
}

/* Files remain private while paging, survive a back/next round-trip, and
 * disappear with expired drafts instead of becoming permanent orphans. */
static void
test_upload_drafts(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "file-pages", VENTURE_FORM_LIVE), version = NULL, upload = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_UPLOAD);
	g_autoptr(GDateTime) now = venture_time_now(), issued = g_date_time_add_seconds(now, -60), later = g_date_time_add_hours(now, 2);
	g_autoptr(GHashTable) answers = venture_forms_answers_new();
	g_autoptr(VentureFormsStep) step = NULL;
	g_autoptr(JsonNode) sweep = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *ticket = NULL, *draft = NULL, *capability = NULL, *path = NULL;
	guint round;
	(void)data;
	add_field(f, form, "attachment", "Resume", VENTURE_FORM_FIELD_FILE, TRUE, 1);
	add_field(f, form, "next", "Contact", VENTURE_FORM_FIELD_PAGE_BREAK, FALSE, 2);
	add_field(f, form, "email", "Email", VENTURE_FORM_FIELD_EMAIL, TRUE, 3);
	version = publish(f, form); start_http(f); ticket = venture_forms_ticket_new(form, issued);
	for (round = 0; round < 2; round++)
	{
		g_hash_table_remove_all(answers); venture_forms_answers_add(answers, VENTURE_FORMS_TICKET, ticket);
		upload_receive(f, form, version, answers, "attachment", now);
		g_clear_pointer(&capability, g_free); capability = g_strdup(g_ptr_array_index((GPtrArray *)g_hash_table_lookup(answers, "attachment"), 0));
		g_clear_pointer(&step, venture_forms_step_free); step = venture_forms_step(f->db, form, answers, NULL, now, &error); g_assert_no_error(error); g_assert_nonnull(step);
		g_assert_cmpuint(step->page, ==, 1); g_assert_cmpuint(json_object_get_size(step->errors), ==, 0);
		g_clear_pointer(&draft, g_free); draft = g_strdup(step->token);
		g_clear_object(&upload); venture_query_add_filter_int(query, "response-id", VENTURE_FILTER_OP_EQ, 0, NULL);
		upload = venture_database_find_one(f->db, query, &error); g_assert_no_error(error); g_assert_nonnull(upload);
		g_clear_pointer(&path, g_free); path = venture_forms_get_string(upload, "path");
		g_assert_cmpint(venture_forms_get_int(upload, "source-id"), >, 0);
		if (round == 1)
		{
			sweep = venture_forms_retention_sweep(f->db, f->org, 10, later, NULL, &error); g_assert_no_error(error);
			g_assert_nonnull(sweep); g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
			break;
		}
		g_hash_table_remove_all(answers); venture_forms_answers_add(answers, VENTURE_FORMS_DRAFT_TOKEN, draft); venture_forms_answers_add(answers, VENTURE_FORMS_MOVE, "back");
		g_clear_pointer(&step, venture_forms_step_free); step = venture_forms_step(f->db, form, answers, NULL, now, &error); g_assert_no_error(error); g_assert_cmpuint(step->page, ==, 0);
		g_clear_pointer(&draft, g_free); draft = g_strdup(step->token);
		g_hash_table_remove_all(answers); venture_forms_answers_add(answers, VENTURE_FORMS_DRAFT_TOKEN, draft); venture_forms_answers_add(answers, "attachment", capability);
		g_clear_pointer(&step, venture_forms_step_free); step = venture_forms_step(f->db, form, answers, NULL, now, &error); g_assert_no_error(error); g_assert_cmpuint(step->page, ==, 1);
		g_clear_pointer(&draft, g_free); draft = g_strdup(step->token);
		g_hash_table_remove_all(answers); venture_forms_answers_add(answers, VENTURE_FORMS_DRAFT_TOKEN, draft); venture_forms_answers_add(answers, "email", "files@example.test");
		g_clear_pointer(&step, venture_forms_step_free); step = venture_forms_step(f->db, form, answers, NULL, now, &error); g_assert_no_error(error); g_assert_true(step->complete);
		g_assert_nonnull(step->submission); g_assert_true(g_file_test(path, G_FILE_TEST_EXISTS));
	}
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_UPLOAD), ==, 1); g_assert_cmpint(count(f, VENTURE_TYPE_DOCUMENT), ==, 1);
	g_assert_cmpint(count(f, VENTURE_TYPE_FORM_DRAFT_RECORD), ==, 0);
}

static void
test_upload_groups(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "group-files", VENTURE_FORM_LIVE), group = NULL, field = NULL, version = NULL, response = NULL;
	g_autoptr(GDateTime) now = venture_time_now(), issued = g_date_time_add_seconds(now, -60);
	g_autoptr(GHashTable) answers = venture_forms_answers_new();
	g_autoptr(JsonObject) errors = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *ticket = NULL, *stored = NULL;
	VentureFormsOutcome outcome;
	(void)data;
	group = g_object_new(VENTURE_TYPE_FORM_GROUP, "organization-id", f->org, "form-id", venture_entity_get_id(form), "key", "people", "label", "People", "min-rows", (gint64)1, "max-rows", (gint64)2, NULL); save(f, group);
	field = make_field(f, form, "cv", "Resume", VENTURE_FORM_FIELD_FILE, TRUE, 1);
	g_object_set(field, "group-id", venture_entity_get_id(group), "sensitive", TRUE, NULL); save(f, field);
	version = publish(f, form); start_http(f); ticket = venture_forms_ticket_new(form, issued);
	venture_forms_answers_add(answers, VENTURE_FORMS_TICKET, ticket);
	upload_receive(f, form, version, answers, "people[0][cv]", now);
	g_assert_true(venture_forms_submit(f->db, form, answers, NULL, now, &outcome, &response, &errors, &error)); g_assert_no_error(error);
	g_assert_cmpint(outcome, ==, VENTURE_FORMS_ACCEPTED); g_assert_nonnull(response);
	stored = venture_forms_get_string(response, "answers"); g_assert_null(strstr(stored, "resume.txt")); g_clear_pointer(&stored, g_free);
	stored = venture_forms_get_string(response, "sensitive-answers"); g_assert_nonnull(strstr(stored, "resume.txt")); g_assert_nonnull(strstr(stored, "document_id"));
	g_assert_cmpint(count(f, VENTURE_TYPE_DOCUMENT), ==, 1);
}

/* Adopting an old upload on a newer form version must not retain the old,
 * weaker download policy when the question has become sensitive. */
static void
test_upload_version_privacy(Fixture *f, gconstpointer data)
{
	g_autoptr(VentureEntity) form = make_form(f, "file-version", VENTURE_FORM_LIVE), field = NULL, version = NULL, response = NULL, upload = NULL;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_FORM_UPLOAD);
	g_autoptr(GDateTime) now = venture_time_now(), issued = g_date_time_add_seconds(now, -60);
	g_autoptr(GHashTable) answers = venture_forms_answers_new();
	g_autoptr(JsonObject) errors = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *ticket = NULL;
	VentureFormsOutcome outcome;
	(void)data;
	field = make_field(f, form, "attachment", "Attachment", VENTURE_FORM_FIELD_FILE, TRUE, 1); save(f, field);
	version = publish(f, form); start_http(f); upload_receive(f, form, version, answers, "attachment", now);
	g_object_set(field, "sensitive", TRUE, NULL); save(f, field);
	g_clear_object(&version); version = publish(f, form); ticket = venture_forms_ticket_new(form, issued);
	venture_forms_answers_add(answers, VENTURE_FORMS_TICKET, ticket);
	g_assert_true(venture_forms_submit(f->db, form, answers, NULL, now, &outcome, &response, &errors, &error)); g_assert_no_error(error); g_assert_nonnull(response);
	upload = venture_database_find_one(f->db, query, &error); g_assert_no_error(error); g_assert_true(venture_forms_get_bool(upload, "sensitive"));
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
	g_test_add("/forms/unique-email-sensitive", Fixture, NULL, setup, test_unique_email_sensitive, teardown);
	g_test_add("/forms/query-prefill-pages", Fixture, NULL, setup, test_query_prefill_pages, teardown);
	g_test_add("/forms/personal-links", Fixture, NULL, setup, test_personal_links, teardown);
	g_test_add("/forms/repeat-rules-privacy", Fixture, NULL, setup, test_repeat_rules_privacy, teardown);
	g_test_add("/forms/repeat-early-add", Fixture, NULL, setup, test_repeat_early_add, teardown);
	g_test_add_func("/forms/repeat-json", test_repeat_json);
	g_test_add("/forms/repeat-groups", Fixture, NULL, setup, test_repeat_groups, teardown);
	g_test_add_func("/forms/rule-vocabulary", test_rule_vocabulary);
	g_test_add("/forms/rules", Fixture, NULL, setup, test_rules, teardown);
	g_test_add("/forms/draft-version", Fixture, NULL, setup, test_draft_version, teardown);
	g_test_add("/forms/draft-privacy", Fixture, NULL, setup, test_draft_privacy, teardown);
	g_test_add("/forms/multi-page", Fixture, NULL, setup, test_multi_page, teardown);
	g_test_add("/forms/unique-email", Fixture, NULL, setup, test_unique_email, teardown);
	g_test_add("/forms/last-slot", Fixture, NULL, setup, test_last_slot, teardown);
	g_test_add("/forms/schedule", Fixture, NULL, setup, test_schedule, teardown);
	g_test_add("/forms/http-limits", Fixture, NULL, setup, test_http_limits, teardown);
	g_test_add("/forms/http-spam", Fixture, NULL, setup, test_http_spam, teardown);
	g_test_add("/forms/http-origins", Fixture, NULL, setup, test_http_origins, teardown);
	g_test_add("/forms/http-ways-in", Fixture, NULL, setup, test_http_ways_in, teardown);
	g_test_add("/forms/embed-codes", Fixture, NULL, setup, test_embed_codes, teardown);
	g_test_add("/forms/builder-page", Fixture, NULL, setup, test_builder_page, teardown);
	g_test_add("/forms/consent-recorded", Fixture, NULL, setup, test_consent_recorded, teardown);
	g_test_add("/forms/sensitive-kept-apart", Fixture, NULL, setup, test_sensitive_kept_apart, teardown);
	g_test_add("/forms/retention-sweep", Fixture, NULL, setup, test_retention_sweep, teardown);
	g_test_add("/forms/erase-person", Fixture, NULL, setup, test_erase_person, teardown);
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
	g_test_add("/forms/optin-resend-expiry", Fixture, NULL, setup, test_optin_resend_expiry, teardown);
	g_test_add("/forms/optin-atomic-suppression", Fixture, NULL, setup, test_optin_atomic_suppression, teardown);
	g_test_add("/forms/erase-sent-confirmation", Fixture, NULL, setup, test_erase_sent_confirmation, teardown);
	g_test_add("/forms/optin-private-erasure", Fixture, NULL, setup, test_optin_private_erasure, teardown);
	g_test_add("/forms/optin-contact-limit", Fixture, NULL, setup, test_optin_contact_limit, teardown);
	g_test_add("/forms/piping-validation", Fixture, NULL, setup, test_piping_validation, teardown);
	g_test_add("/forms/piping-visible-validated", Fixture, NULL, setup, test_piping_visible_validated, teardown);
	g_test_add("/forms/piping-pages-success", Fixture, NULL, setup, test_piping_pages_success, teardown);
	g_test_add("/forms/piping-rows", Fixture, NULL, setup, test_piping_rows, teardown);
	g_test_add("/forms/double-optin", Fixture, NULL, setup, test_double_optin, teardown);
	g_test_add("/forms/marketing-consent", Fixture, NULL, setup, test_marketing_consent, teardown);
	g_test_add("/forms/consent-no-default", Fixture, NULL, setup, test_consent_no_default, teardown);
	g_test_add("/forms/quiz-validation", Fixture, NULL, setup, test_quiz_validation, teardown);
	g_test_add("/forms/quiz-score-key", Fixture, NULL, setup, test_quiz_score_and_key, teardown);
	g_test_add("/forms/quiz-hidden-repeated", Fixture, NULL, setup, test_quiz_hidden_and_repeated, teardown);
	g_test_add("/forms/payment-pricing", Fixture, NULL, setup, test_payment_pricing, teardown);
	g_test_add("/forms/booking", Fixture, NULL, setup, test_booking_form, teardown);
	g_test_add("/forms/quiz-redirect", Fixture, NULL, setup, test_quiz_redirect, teardown);
	g_test_add("/forms/quiz-version-privacy", Fixture, NULL, setup, test_quiz_version_privacy, teardown);
	g_test_add("/forms/quiz-lead-input", Fixture, NULL, setup, test_quiz_lead_input, teardown);
	g_test_add("/forms/resume-private-mail", Fixture, NULL, setup, test_resume_private_mail, teardown);
	g_test_add("/forms/resume-http", Fixture, NULL, setup, test_resume_http, teardown);
	g_test_add("/forms/resume-groups-expiry", Fixture, NULL, setup, test_resume_groups_expiry, teardown);
	g_test_add("/forms/resume-lifecycle", Fixture, NULL, setup, test_resume_lifecycle, teardown);
	g_test_add("/forms/resume-changed", Fixture, NULL, setup, test_resume_changed, teardown);
	g_test_add("/forms/languages-validation", Fixture, NULL, setup, test_languages_validation, teardown);
	g_test_add("/forms/languages-report", Fixture, NULL, setup, test_languages_report, teardown);
	g_test_add("/forms/languages-pages", Fixture, NULL, setup, test_languages_pages, teardown);
	g_test_add("/forms/languages-optin", Fixture, NULL, setup, test_languages_optin, teardown);
	g_test_add("/forms/optin-personal-state", Fixture, NULL, setup, test_optin_personal_state, teardown);
	g_test_add("/forms/languages-confirmation", Fixture, NULL, setup, test_languages_confirmation, teardown);
	g_test_add("/forms/portable-templates", Fixture, NULL, setup, test_portable_templates, teardown);
	g_test_add("/forms/portable-roundtrip", Fixture, NULL, setup, test_portable_roundtrip, teardown);
	g_test_add("/forms/portable-refusals", Fixture, NULL, setup, test_portable_refusals, teardown);
	g_test_add("/forms/portable-bindings", Fixture, NULL, setup, test_portable_bindings, teardown);
	g_test_add("/forms/portable-prices", Fixture, NULL, setup, test_portable_prices, teardown);
	g_test_add("/forms/upload-http", Fixture, NULL, setup, test_upload_http, teardown);
	g_test_add("/forms/upload-refusals", Fixture, NULL, setup, test_upload_refusals, teardown);
	g_test_add("/forms/upload-drafts", Fixture, NULL, setup, test_upload_drafts, teardown);
	g_test_add("/forms/upload-groups", Fixture, NULL, setup, test_upload_groups, teardown);
	g_test_add("/forms/upload-version-privacy", Fixture, NULL, setup, test_upload_version_privacy, teardown);
	return g_test_run();
}
