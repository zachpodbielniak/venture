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
} VentureFormsRender;

/**
 * VentureFormsOutcome:
 * @VENTURE_FORMS_ACCEPTED: saved as a response
 * @VENTURE_FORMS_DISCARDED: looked like a robot; nothing saved, and the
 *   sender is told it succeeded
 * @VENTURE_FORMS_INVALID: refused, with a message per question
 *
 * What became of one submission.
 */
typedef enum
{
	VENTURE_FORMS_ACCEPTED = 0,
	VENTURE_FORMS_DISCARDED,
	VENTURE_FORMS_INVALID
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
 * Signs the time a form was handed out with the form's private key. Posted
 * back, it says how long the person took; it cannot be forged to say
 * longer.
 *
 * Returns: (transfer full): the ticket
 */
gchar *venture_forms_ticket_new(VentureEntity *form, GDateTime *issued);

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

G_END_DECLS

#endif /* VENTURE_FORMS_H */
