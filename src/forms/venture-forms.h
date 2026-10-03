/*
 * venture-forms.h - Forms: the renderer, the schema and the public intake
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Everything a form does lives here, so the builder's preview, the
 * copy-and-paste snippet, the script loader's fragment and the hosted page
 * are one renderer, and the HTML door and the JSON door are one validator.
 * The web layer only parses the request and picks a response format.
 */

#ifndef VENTURE_FORMS_H
#define VENTURE_FORMS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

G_BEGIN_DECLS

/**
 * VENTURE_FORMS_MAX_BODY:
 *
 * The largest request body the public door parses, in bytes. A form's
 * answers are short; anything bigger is refused before it is read.
 */
#define VENTURE_FORMS_MAX_BODY (64 * 1024)

/**
 * VENTURE_FORMS_HONEYPOT:
 *
 * The name of the field a person never sees and a robot fills in.
 */
#define VENTURE_FORMS_HONEYPOT "_vf_hp"

/**
 * VENTURE_FORMS_TICKET:
 *
 * The name of the field carrying the signed time a form was handed out.
 */
#define VENTURE_FORMS_TICKET "_vf_t"

/**
 * VentureFormsRenderMode:
 * @VENTURE_FORMS_RENDER_FRAGMENT: a bare form element, for the script
 *   loader and for pasting into a page
 * @VENTURE_FORMS_RENDER_HOSTED: a whole document with nothing but the form
 * @VENTURE_FORMS_RENDER_HOSTED_BASIC: the hosted document plus the minimal
 *   opt-in stylesheet
 * @VENTURE_FORMS_RENDER_SNIPPET: the fragment, meant to be copied
 * @VENTURE_FORMS_RENDER_PREVIEW: the fragment inside the builder; its
 *   button is disabled and it posts nowhere
 *
 * Where a rendering will be shown. The markup of the questions is the same
 * in every mode; only the wrapper differs.
 */
typedef enum
{
	VENTURE_FORMS_RENDER_FRAGMENT = 0,
	VENTURE_FORMS_RENDER_HOSTED,
	VENTURE_FORMS_RENDER_HOSTED_BASIC,
	VENTURE_FORMS_RENDER_SNIPPET,
	VENTURE_FORMS_RENDER_PREVIEW
} VentureFormsRenderMode;

/**
 * VentureFormsRender:
 * @mode: where it will be shown
 * @action: (nullable): where the form posts; the public path when %NULL
 * @ticket: (nullable): a fill-time ticket to embed, or %NULL for none
 * @values: (nullable): answers to put back in the boxes, by key
 * @errors: (nullable): messages to show, by key; "_form" for the form
 * @version: (nullable): the published version to render; %NULL renders the
 *   draft questions, which only the builder's preview shows
 *
 * What one rendering needs besides the form. A plain struct, filled
 * member by member at every call site.
 */
typedef struct
{
	VentureFormsRenderMode	 mode;
	const gchar		*action;
	const gchar		*ticket;
	JsonObject		*values;
	JsonObject		*errors;
	VentureEntity		*version;
} VentureFormsRender;

/**
 * VentureFormsOutcome:
 * @VENTURE_FORMS_ACCEPTED: saved as a response
 * @VENTURE_FORMS_DISCARDED: looked like a robot; nothing saved, and the
 *   sender is told it succeeded
 * @VENTURE_FORMS_INVALID: refused, with a message per question
 * @VENTURE_FORMS_PENDING: private signup awaiting inbox confirmation
 * @VENTURE_FORMS_PAYMENT: private response awaiting invoice settlement
 *
 * What became of one submission.
 */
typedef enum
{
	VENTURE_FORMS_ACCEPTED = 0,
	VENTURE_FORMS_DISCARDED,
	VENTURE_FORMS_INVALID,
	VENTURE_FORMS_PENDING,
	VENTURE_FORMS_PAYMENT
} VentureFormsOutcome;

/**
 * VentureFormsEmbed:
 * @VENTURE_FORMS_EMBED_HTML: markup that posts straight to VENTURE
 * @VENTURE_FORMS_EMBED_SCRIPT: a script tag and a target element
 * @VENTURE_FORMS_EMBED_IFRAME: an iframe of the hosted page
 * @VENTURE_FORMS_EMBED_SCHEMA: the address of the JSON schema
 *
 * The ways onto another site the builder offers to copy.
 */
typedef enum
{
	VENTURE_FORMS_EMBED_HTML = 0,
	VENTURE_FORMS_EMBED_SCRIPT,
	VENTURE_FORMS_EMBED_IFRAME,
	VENTURE_FORMS_EMBED_SCHEMA
} VentureFormsEmbed;

/**
 * venture_forms_install:
 * @context: a #VentureContext
 *
 * Registers the forms module's save validators on the context's database.
 * Safe to call once per context over the same database.
 */
void venture_forms_install(VentureContext *context);

/**
 * venture_forms_register_reports:
 * @registry: a #VentureReportRegistry
 *
 * Adds the form_summary report.
 */
void venture_forms_register_reports(VentureReportRegistry *registry);

/**
 * venture_forms_fields:
 * @database: a #VentureDatabase
 * @form: a saved form
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer container) (element-type VentureFormField): the form's
 *   questions in the order they are asked, or %NULL on error
 */
GPtrArray *venture_forms_fields(VentureDatabase *database, VentureEntity *form, GError **error);

/**
 * venture_forms_find_live:
 * @database: a #VentureDatabase
 * @token: the public token from the address
 * @now: the time to judge the close date and the cap at
 * @error: (out) (optional): return location for a #GError
 *
 * Finds the form a public address names, if it is taking responses now.
 * A draft, a closed form, one past its close date, one at its response
 * limit and one that does not exist all fail with the same
 * %VENTURE_ERROR_NOT_FOUND, so the door says nothing about which.
 *
 * Returns: (transfer full) (nullable): the form
 */
VentureEntity *venture_forms_find_live(VentureDatabase *database, const gchar *token,
	GDateTime *now, GError **error);

/**
 * venture_forms_origin_allowed:
 * @form: a form
 * @origin: (nullable): the Origin a browser sent, if any
 *
 * Whether a page on @origin may use @form. A form with no allowed sites
 * may be used from anywhere; a request that names no origin is not a
 * browser on another page and is judged by the rest of the door.
 *
 * Returns: %TRUE if allowed
 */
gboolean venture_forms_origin_allowed(VentureEntity *form, const gchar *origin);

/**
 * venture_forms_ticket_new:
 * @form: a saved form
 * @issued: when the form is being handed out
 *
 * Signs the time a form was handed out, and the published version it was
 * handed out at, with the form's private key. Posted back, it says how
 * long the person took and which questions they were given; it cannot be
 * forged to say either differently.
 *
 * Returns: (transfer full): the ticket
 */
gchar *venture_forms_ticket_new(VentureEntity *form, GDateTime *issued);

/**
 * venture_forms_ticket_new_for_version:
 * @form: a saved form
 * @version: the version number the person is given
 * @issued: when the form is being handed out
 *
 * As venture_forms_ticket_new(), for a named version.
 *
 * Returns: (transfer full): the ticket
 */
gchar *venture_forms_ticket_new_for_version(VentureEntity *form, gint64 version, GDateTime *issued);

/**
 * venture_forms_published_version:
 * @database: a #VentureDatabase
 * @form: a saved form
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): what the public sees, or %NULL
 *   before the first publish
 */
VentureEntity *venture_forms_published_version(VentureDatabase *database, VentureEntity *form,
	GError **error);

/**
 * venture_forms_version_for_answers:
 * @database: a #VentureDatabase
 * @form: a saved form
 * @answers: posted answers, carrying a ticket
 *
 * The version posted answers are checked against: the one the ticket was
 * issued for when that is a real version of @form, else the published
 * one. Whoever started on a version finishes on it.
 *
 * Returns: (transfer full) (nullable): the version
 */
VentureEntity *venture_forms_version_for_answers(VentureDatabase *database, VentureEntity *form,
	GHashTable *answers);

/**
 * venture_forms_publish:
 * @database: a #VentureDatabase
 * @form: a saved form
 * @actor: (nullable): who is publishing
 * @error: (out) (optional): return location for a #GError
 *
 * Freezes the form's current questions as its next version and makes it
 * the one the public sees. Publishing what is already published returns
 * that version and makes no new one. A form with no questions is refused.
 *
 * Returns: (transfer full) (nullable): the published version
 */
VentureEntity *venture_forms_publish(VentureDatabase *database, VentureEntity *form,
	const VentureActor *actor, GError **error);

/**
 * venture_forms_has_unpublished_changes:
 * @database: a #VentureDatabase
 * @form: a saved form
 *
 * Returns: %TRUE when the questions differ from what the public sees
 */
gboolean venture_forms_has_unpublished_changes(VentureDatabase *database, VentureEntity *form);

/**
 * venture_forms_render_answers:
 * @database: a #VentureDatabase
 * @submission: a saved response
 * @error: (out) (optional): return location for a #GError
 *
 * The response's answers as a definition list, labelled by the version of
 * the questions it answered, not by today's. Sensitive answers are omitted
 * even on the response's own page.
 *
 * Returns: (transfer full) (nullable): the markup
 */
gchar *venture_forms_render_answers(VentureDatabase *database, VentureEntity *submission,
	GError **error);

/**
 * venture_forms_render:
 * @database: a #VentureDatabase
 * @form: a saved form
 * @options: where it will be shown and what to refill
 * @error: (out) (optional): return location for a #GError
 *
 * The one renderer. Unstyled semantic HTML with the documented vf-*
 * classes and data-vf-* hooks; no inline style, no stylesheet (except the
 * opt-in hosted one) and nothing of VENTURE's own look, so the host page's
 * CSS is what applies.
 *
 * Returns: (transfer full) (nullable): the markup
 */
gchar *venture_forms_render(VentureDatabase *database, VentureEntity *form,
	const VentureFormsRender *options, GError **error);

/**
 * venture_forms_render_success:
 * @form: a form
 * @hosted: whether to wrap the message in a whole document
 *
 * Returns: (transfer full): the thank-you markup, class vf-success
 */
gchar *venture_forms_render_success(VentureEntity *form, gboolean hosted);

/**
 * venture_forms_schema:
 * @database: a #VentureDatabase
 * @form: a saved form
 * @action: where answers are posted
 * @now: when the schema is handed out, for its ticket
 * @error: (out) (optional): return location for a #GError
 *
 * The form as JSON, for a site that renders it in its own framework. Only
 * what a visitor could see and the names to post under; nothing internal.
 *
 * Returns: (transfer full) (nullable): the schema
 */
JsonNode *venture_forms_schema(VentureDatabase *database, VentureEntity *form,
	const gchar *action, GDateTime *now, GError **error);

/**
 * venture_forms_embed_code:
 * @database: a #VentureDatabase
 * @form: a saved form
 * @base_url: this install's public address, without a trailing slash
 * @kind: which way onto the other site
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the text to copy
 */
gchar *venture_forms_embed_code(VentureDatabase *database, VentureEntity *form,
	const gchar *base_url, VentureFormsEmbed kind, GError **error);

/**
 * venture_forms_answers_new:
 *
 * Returns: (transfer full) (element-type utf8 GPtrArray): an empty map from
 *   a posted name to every value posted under it
 */
GHashTable *venture_forms_answers_new(void);

/**
 * venture_forms_answers_add:
 * @answers: a map from venture_forms_answers_new()
 * @name: the posted name
 * @value: one value posted under it
 */
void venture_forms_answers_add(GHashTable *answers, const gchar *name, const gchar *value);

/**
 * venture_forms_answers_from_urlencoded:
 * @body: an application/x-www-form-urlencoded body
 * @length: its length in bytes
 * @error: (out) (optional): return location for a #GError
 *
 * Parses a form post, keeping every value of a repeated name -- a ticked
 * box per choice arrives that way. Refuses bad escapes, a NUL and text
 * that is not UTF-8.
 *
 * Returns: (transfer full) (nullable): the answers
 */
GHashTable *venture_forms_answers_from_urlencoded(const gchar *body, gsize length, GError **error);

/**
 * venture_forms_answers_from_json:
 * @object: a JSON object of answers
 * @error: (out) (optional): return location for a #GError
 *
 * Reads answers posted as JSON: a string, a number or a boolean per name,
 * or an array of strings for a multiple choice. Anything else is refused.
 *
 * Returns: (transfer full) (nullable): the answers
 */
GHashTable *venture_forms_answers_from_json(JsonObject *object, GError **error);

/**
 * venture_forms_answers_to_json:
 * @answers: a map from venture_forms_answers_new()
 *
 * Returns: (transfer full): the answers as the renderer refills them
 */
JsonObject *venture_forms_answers_to_json(GHashTable *answers);

/**
 * venture_forms_screen:
 * @form: a saved form
 * @answers: the posted answers
 * @now: when they arrived
 *
 * The public door's robot check: an empty honeypot and a valid ticket at
 * least the form's minimum fill time old.
 *
 * Returns: %TRUE when the submission looks like a person's
 */
gboolean venture_forms_screen(VentureEntity *form, GHashTable *answers, GDateTime *now);

/**
 * venture_forms_submit:
 * @database: a #VentureDatabase
 * @form: a saved form taking responses
 * @answers: the posted answers
 * @origin: (nullable): where they were posted from
 * @now: when they arrived
 * @outcome: (out): what became of them
 * @submission: (out) (optional) (transfer full) (nullable): the saved response
 * @errors: (out) (optional) (transfer full) (nullable): messages by key when
 *   the outcome is %VENTURE_FORMS_INVALID
 * @error: (out) (optional): return location for a #GError
 *
 * Validates answers against the form's questions -- the server is the
 * authority on every kind -- and saves the response through the ordinary
 * path, so it is audited and automations, webhooks and notifications hear
 * of it. A lead and a confirmation are made in the same transaction when
 * the form asks; if either cannot be, the response is kept and says why.
 *
 * Returns: %FALSE only when the database fails; an invalid answer is a
 *   %TRUE return with @outcome %VENTURE_FORMS_INVALID
 */
gboolean venture_forms_submit(VentureDatabase *database, VentureEntity *form, GHashTable *answers,
	const gchar *origin, GDateTime *now, VentureFormsOutcome *outcome,
	VentureEntity **submission, JsonObject **errors, GError **error);

/**
 * venture_forms_retention_sweep:
 * @database: a #VentureDatabase
 * @organization_id: the organization to sweep
 * @limit: the most responses to remove in this run (at most 1000; 0 for 100)
 * @now: the time retention is judged at
 * @actor: (nullable): who asked
 * @error: (out) (optional): return location for a #GError
 *
 * Anonymises or purges, per each form's setting, responses older than the
 * form keeps them. Bounded, and run only when asked.
 *
 * Returns: (transfer full) (nullable): counts of anonymised and purged
 *   responses and whether the limit was reached
 */
JsonNode *venture_forms_retention_sweep(VentureDatabase *database, gint64 organization_id, guint limit,
	GDateTime *now, const VentureActor *actor, GError **error);

/**
 * venture_forms_erase_person:
 * @database: a #VentureDatabase
 * @organization_id: the organization to erase in
 * @email: the address whose responses to erase
 * @actor: (nullable): who asked
 * @error: (out) (optional): return location for a #GError
 *
 * Deletes every response whose answers carry @email, cancels their queued
 * confirmations, and records one audit entry saying an erasure happened
 * and how many, without the address or any answer.
 *
 * Returns: (transfer full) (nullable): what was erased and cancelled
 */
JsonNode *venture_forms_erase_person(VentureDatabase *database, gint64 organization_id,
	const gchar *email, const VentureActor *actor, GError **error);

/**
 * venture_forms_export_person:
 * @database: a #VentureDatabase
 * @organization_id: the organization to look in
 * @email: the address whose responses to export
 * @error: (out) (optional): return location for a #GError
 *
 * Every response whose answers carry @email, sensitive answers included,
 * with its form, version and time: what an access request is owed.
 *
 * Returns: (transfer full) (nullable): the export
 */
JsonNode *venture_forms_export_person(VentureDatabase *database, gint64 organization_id,
	const gchar *email, GError **error);

/**
 * venture_forms_personal_link:
 * @database: database
 * @form: form in the contact's organization
 * @contact: existing contact
 * @origin: public HTTPS origin (HTTP allowed on loopback)
 * @expires: expiry within 30 days
 * @now: generation time
 * @error: (out) (optional): validation failure
 * Returns: (transfer full) (nullable): bearer URL; keep private
 */
gchar *venture_forms_personal_link(VentureDatabase *database, VentureEntity *form,
	VentureEntity *contact, const gchar *origin, GDateTime *expires, GDateTime *now, GError **error);

/**
 * venture_forms_confirm_signup:
 * @database: the database
 * @form: the live form
 * @token: private signed confirmation capability
 * @now: confirmation clock
 * @confirm: whether to consume the capability
 * @error: return location for an error
 *
 * A read validates the link without consuming it. A confirmation atomically
 * creates the response, contact, permission and optional list membership.
 * Returns: (transfer full) (nullable): private pending copy on read, final
 * response on confirmation, or %NULL for an invalid/expired/consumed link
 */
VentureEntity *venture_forms_confirm_signup(VentureDatabase *database, VentureEntity *form,
	const gchar *token, GDateTime *now, gboolean confirm, GError **error);

/**
 * venture_forms_check_write:
 * @database: owning storage
 * @entity: lifecycle target
 * @removal: whether this operation removes the record
 * @error: (out) (optional): refusal
 *
 * Keeps pending payment evidence owned by its service across save, restore,
 * delete and purge. Other record types are unaffected.
 * Returns: whether the lifecycle operation may proceed
 */
gboolean venture_forms_check_write(VentureDatabase *database, VentureEntity *entity,
	gboolean removal, GError **error);

/**
 * venture_forms_payment_reconcile:
 * @database: storage with no enclosing transaction
 * @organization: organization owning the Checkout
 * @checkout_id: locally retained Checkout, never a client payment claim
 * @now: completion clock
 * @error: (out) (optional): completion failure
 *
 * Called after verified settlement commits. Repeated calls are idempotent.
 * An ordinary response is saved only for a completed Checkout with its
 * settlement payment; delayed bank settlement keeps a private pending copy.
 * Returns: whether the retained outcome was applied to any matching intake
 */
gboolean venture_forms_payment_reconcile(VentureDatabase *database, gint64 organization,
	gint64 checkout_id, GDateTime *now, GError **error);

/**
 * venture_forms_results:
 * @context: the application context
 * @organization: organization to read, or 0 for the default
 * @form_id: a form in that organization
 * @kind: one of the six form widget kind names
 * @key: (nullable): stable question key for choice, rating and NPS kinds
 * @version_number: published version number, or 0 for all
 * @period: (nullable): response period, or all dates
 * @error: return location for a #GError
 *
 * Computes bounded, version-aware statistics using the survey report's counting
 * rules. Sensitive answers and unsubmitted draft contents are never returned.
 * Drop-off counts retained unfinished drafts by their current page.
 *
 * Returns: (transfer full) (nullable): aggregate JSON, or %NULL on error
 */
JsonNode *venture_forms_results(VentureContext *context, gint64 organization,
	gint64 form_id, const gchar *kind, const gchar *key, gint64 version_number,
	VentureDateRange *period, GError **error);

/**
 * venture_forms_summarize:
 * @context: the application context
 * @organization: organization to read, or 0 for the default
 * @form_id: a form in that organization
 * @first_version: first published version number, inclusive
 * @last_version: last published version number, inclusive
 * @question: (nullable): one free-text question key, or all
 * @period: (nullable): submitted date range, or all dates
 * @limit: maximum responses to read, from 1 through 200
 * @error: return location for a #GError
 *
 * Calls the organization-bound toolless completion service with at most 200
 * nonsensitive text answers and 32 KiB of answer text. Refuses empty or excessive
 * input before a model call. Returns only bounded structured output, derives
 * theme counts from distinct valid answer IDs, and drops unverifiable quotes.
 * No submission or form is changed; the result is a proposal for a person.
 *
 * Returns: (transfer full) (nullable): a validated JSON proposal, or %NULL
 */
JsonNode *venture_forms_summarize(VentureContext *context, gint64 organization,
	gint64 form_id, gint64 first_version, gint64 last_version, const gchar *question,
	VentureDateRange *period, guint limit, GError **error);

/**
 * venture_forms_export_definition:
 * @database: storage
 * @form: saved form whose editable definition is exported
 * @error: (out) (optional): error location
 *
 * Excludes responses, identities and credentials. External references become
 * symbolic bindings that must be mapped explicitly on import.
 * Returns: (transfer full) (nullable): portable version-1 JSON definition
 */
JsonNode *venture_forms_export_definition(VentureDatabase *database, VentureEntity *form, GError **error);

/**
 * venture_forms_definition_format:
 * @definition: portable definition
 * @format: (nullable): json (default) or yaml
 * @error: (out) (optional): error location
 * Returns: (transfer full) (nullable): serialized definition
 */
gchar *venture_forms_definition_format(JsonNode *definition, const gchar *format, GError **error);

/**
 * venture_forms_import_definition:
 * @context: services and storage
 * @organization: destination organization; zero uses the default
 * @text: bounded JSON or YAML portable definition
 * @bindings: (nullable): symbolic binding names mapped to destination record IDs
 * @actor: (nullable): audited actor
 * @error: (out) (optional): error location
 *
 * Creates a draft with fresh identity and capabilities. Definition records
 * share one transaction; no response or publication is imported.
 * Returns: (transfer full) (nullable): the saved draft form
 */
VentureEntity *venture_forms_import_definition(VentureContext *context, gint64 organization,
	const gchar *text, JsonObject *bindings, const VentureActor *actor, GError **error);

/**
 * venture_forms_templates:
 *
 * Returns: (transfer full): array of names and portable template definitions
 */
JsonNode *venture_forms_templates(void);

/**
 * VentureFormsUploadScanner:
 * @bytes: bounded candidate file bytes
 * @mime_type: content-sniffed MIME type
 * @user_data: registration data
 * @error: (out) (optional): rejection reason
 *
 * Optional synchronous scanner invoked before a public upload reaches disk.
 * The callback must not retain borrowed arguments or run a nested main loop.
 * Returns: whether the candidate is accepted
 */
typedef gboolean (*VentureFormsUploadScanner)(GBytes *bytes, const gchar *mime_type,
	gpointer user_data, GError **error);

/**
 * venture_forms_set_upload_scanner:
 * @database: storage owning this registration
 * @callback: (scope notified) (nullable): optional file scanner
 * @user_data: (closure callback): registration data
 * @destroy: (destroy user_data) (nullable): registration cleanup
 *
 * Replaces the optional scanner. Size and content-type checks always run,
 * including when no scanner is registered. The database owns the registration.
 */
void venture_forms_set_upload_scanner(VentureDatabase *database, VentureFormsUploadScanner callback,
	gpointer user_data, GDestroyNotify destroy);

G_END_DECLS

#endif /* VENTURE_FORMS_H */
