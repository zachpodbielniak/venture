/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_EMAIL_DESIGN_H
#define VENTURE_EMAIL_DESIGN_H
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "emaildesign/venture-email-design-records.h"
#include "mail/venture-mail-records.h"
#include "core/venture-context.h"
G_BEGIN_DECLS

/**
 * VENTURE_EMAIL_DESIGN_MAX_BYTES:
 *
 * The largest design, as JSON text, a template or section may hold.
 */
#define VENTURE_EMAIL_DESIGN_MAX_BYTES (64 * 1024)

/**
 * VENTURE_EMAIL_DESIGN_MAX_BLOCKS:
 *
 * The most blocks a design may have at the top level, and in total once
 * its sections are put in place.
 */
#define VENTURE_EMAIL_DESIGN_MAX_BLOCKS (40)

/**
 * VENTURE_EMAIL_DESIGN_MAX_SECTION_DEPTH:
 *
 * How deep sections may reference sections.
 */
#define VENTURE_EMAIL_DESIGN_MAX_SECTION_DEPTH (4)

/**
 * VENTURE_EMAIL_DESIGN_WIDTH:
 *
 * The width of the message body in pixels; nothing inside it is wider.
 */
#define VENTURE_EMAIL_DESIGN_WIDTH (600)

/**
 * VENTURE_HOSTED_IMAGE_MAX_BYTES:
 *
 * The largest image that may be uploaded for email (5 MB). Mail clients
 * download every image of every message they show.
 */
#define VENTURE_HOSTED_IMAGE_MAX_BYTES (5 * 1024 * 1024)

/**
 * VENTURE_HOSTED_IMAGE_MAX_SIDE:
 *
 * The largest width or height, in pixels, an uploaded image may have.
 */
#define VENTURE_HOSTED_IMAGE_MAX_SIDE (8000)

/**
 * VentureEmailLook:
 * @name: the business name shown in the header and footer
 * @logo_url: (nullable): the logo's public address
 * @logo_width: the logo's display width in pixels
 * @logo_height: the logo's display height in pixels, or 0 when unknown
 * @primary: headings and buttons, "#rrggbb"
 * @accent: links and highlights
 * @text: body text
 * @background: around the message
 * @card: behind the message
 * @dark_primary: @primary in dark mode
 * @dark_text: @text in dark mode
 * @dark_background: @background in dark mode
 * @dark_card: @card in dark mode
 * @font: the font stack, web-safe faces with a generic fallback
 * @website: (nullable): the business's own web address
 * @address: (nullable): the postal address for the footer
 *
 * What an email looks like: an organization's brand with every gap filled
 * by a default, so a renderer never has to decide one.
 */
typedef struct {
	gchar *name;
	gchar *logo_url;
	gint logo_width;
	gint logo_height;
	gchar *primary;
	gchar *accent;
	gchar *text;
	gchar *background;
	gchar *card;
	gchar *dark_primary;
	gchar *dark_text;
	gchar *dark_background;
	gchar *dark_card;
	gchar *font;
	gchar *website;
	gchar *address;
} VentureEmailLook;

/**
 * venture_email_look_new_default:
 * @name: (nullable): the business name
 *
 * The plain look used when an organization has no brand: neutral colours,
 * a sans-serif stack and no logo.
 *
 * Returns: (transfer full): a look
 */
VentureEmailLook *venture_email_look_new_default(const gchar *name);

/**
 * venture_email_look_free:
 * @look: (nullable) (transfer full): a look
 */
void venture_email_look_free(VentureEmailLook *look);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureEmailLook, venture_email_look_free)

/**
 * venture_email_font_stack:
 * @font: a font choice: sans, serif, rounded, system or mono
 *
 * Returns: (nullable): the stack for @font, or %NULL for an unknown choice
 */
const gchar *venture_email_font_stack(const gchar *font);

/**
 * VentureEmailSectionLookup:
 * @section_id: the section a design names
 * @data: the resolver's data
 * @error: (out) (optional): why it cannot be used
 *
 * Returns: (transfer full) (nullable): the section's design as JSON text
 */
typedef gchar *(*VentureEmailSectionLookup)(gint64 section_id, gpointer data, GError **error);

/**
 * VentureEmailImageLookup:
 * @image_id: the hosted image a design names
 * @data: the resolver's data
 * @url: (out) (transfer full): its public address
 * @width: (out): its width in pixels
 * @height: (out): its height in pixels
 * @error: (out) (optional): why it cannot be used
 *
 * Returns: whether the image exists in the design's organization
 */
typedef gboolean (*VentureEmailImageLookup)(gint64 image_id, gpointer data, gchar **url,
	gint *width, gint *height, GError **error);

/**
 * VentureEmailResolver:
 * @section: (nullable): resolves section blocks; %NULL refuses every section
 * @image: (nullable): resolves hosted images; %NULL refuses every image id
 * @data: passed to both
 *
 * How a renderer reaches records. The renderer itself touches no database.
 */
typedef struct {
	VentureEmailSectionLookup section;
	VentureEmailImageLookup image;
	gpointer data;
} VentureEmailResolver;

/**
 * venture_email_design_validate:
 * @json: a design as JSON text
 * @error: (out) (optional): the first thing wrong, in plain words
 *
 * Strict validation: an object with "blocks" (and optionally "preheader"
 * and "version": 1); each block one of header, text, image, button,
 * columns, divider, social, footer, video or section with only its own
 * fields; every size bounded; every address http or https; image alt text
 * required. Sections are not followed.
 *
 * Returns: whether @json is a valid design
 */
gboolean venture_email_design_validate(const gchar *json, GError **error);

/**
 * venture_email_design_render: (skip)
 * @json: a valid design
 * @look: the look to apply
 * @resolver: (nullable): sections and hosted images
 * @title: (nullable): the subject, used as the document title
 * @template_syntax: whether to write the output in mail_template syntax,
 *   where a literal brace is doubled and "{field}" placeholders stay
 * @html: (out) (transfer full): email-safe HTML: tables, inline styles,
 *   600 pixels wide, a dark-mode block
 * @text: (out) (transfer full): the plain-text alternative
 * @error: (out) (optional): an invalid design, a missing section or image,
 *   or a section cycle
 *
 * Returns: whether both bodies were built
 */
gboolean venture_email_design_render(const gchar *json, const VentureEmailLook *look,
	const VentureEmailResolver *resolver, const gchar *title, gboolean template_syntax,
	gchar **html, gchar **text, GError **error);

/**
 * venture_hosted_image_sniff:
 * @data: the uploaded bytes
 * @width: (out) (optional): width in pixels
 * @height: (out) (optional): height in pixels
 * @extension: (out) (optional) (transfer none): "png", "jpg", "gif" or "webp"
 *
 * Reads what an image is from its bytes, never from a declared type or a
 * file name: PNG, JPEG, GIF or WebP, with its dimensions.
 *
 * Returns: (nullable): the content type, or %NULL for anything else
 */
const gchar *venture_hosted_image_sniff(GBytes *data, gint *width, gint *height, const gchar **extension);

/**
 * venture_hosted_image_store:
 * @context: the application context
 * @organization_id: the organization the image belongs to
 * @filename: (nullable): the name it was uploaded under, kept as its name only
 * @data: the bytes
 * @alt: (nullable): default alt text
 * @actor: (nullable): who uploaded it
 * @error: (out) (optional): not an image, too large, or refused
 *
 * Files the bytes as a document in the attachment store and makes a
 * hosted image for them, with dimensions and type sniffed from the bytes
 * and a fresh unguessable token. An image stored as the organization's
 * logo, by the brand, answers a pending logo request.
 *
 * Returns: (transfer full) (nullable): the hosted image
 */
VentureEntity *venture_hosted_image_store(VentureContext *context, gint64 organization_id,
	const gchar *filename, GBytes *data, const gchar *alt, const VentureActor *actor, GError **error);

/**
 * venture_hosted_image_url:
 * @context: the application context
 * @image: a hosted image
 *
 * Returns: (transfer full): its public address under the server's base URL
 */
gchar *venture_hosted_image_url(VentureContext *context, VentureEntity *image);

/**
 * venture_hosted_image_read:
 * @context: the application context
 * @organization_id: the organization in the address
 * @file: the last part of the address, TOKEN.EXT
 * @content_type: (out) (transfer none): the sniffed type
 * @error: (out) (optional): not found
 *
 * What the public route serves. No principal is needed: the token is the
 * capability, and it only works under its own organization.
 *
 * Returns: (transfer full) (nullable): the bytes
 */
GBytes *venture_hosted_image_read(VentureContext *context, gint64 organization_id, const gchar *file,
	const gchar **content_type, GError **error);

/**
 * venture_email_look_load:
 * @context: the application context
 * @organization_id: an organization
 *
 * The organization's brand with defaults filled in; the default look when
 * it has none.
 *
 * Returns: (transfer full): a look
 */
VentureEmailLook *venture_email_look_load(VentureContext *context, gint64 organization_id);

/**
 * venture_email_design_save:
 * @context: the application context
 * @template: a mail_template, saved or not
 * @design: the new design as JSON text
 * @source: what made it: manual, ai or ai-edit
 * @note: (nullable): the brief or instruction that made it
 * @actor: (nullable): who
 * @error: (out) (optional): invalid design or refusal
 *
 * Sets the template's design, saves the template (which rebuilds its HTML
 * and text bodies) and records the design as the template's next version,
 * in one transaction. Needs an owner or administrator of the organization
 * when called under a person's authority.
 *
 * Returns: (transfer full) (nullable): the version recorded
 */
VentureEntity *venture_email_design_save(VentureContext *context, VentureEntity *template,
	const gchar *design, const gchar *source, const gchar *note, const VentureActor *actor, GError **error);

/**
 * venture_email_design_undo:
 * @context: the application context
 * @organization_id: exact organization
 * @template_id: a mail_template with a design
 * @actor: (nullable): who
 * @error: (out) (optional): nothing to undo, or refusal
 *
 * Puts the template back to the version before its current one, byte for
 * byte. The version stays in the history; the template's current version
 * moves back. A later change is recorded after the newest version.
 *
 * Returns: (transfer full) (nullable): the version now current
 */
VentureEntity *venture_email_design_undo(VentureContext *context, gint64 organization_id, gint64 template_id,
	const VentureActor *actor, GError **error);

/**
 * VentureEmailDesignResult:
 * @template: the template the design is on
 * @version: the version recorded
 * @stripped: (element-type utf8): sentences removed because they claimed
 *   a price, amount, percentage or date found in neither the brief nor
 *   the records
 * @ask: (nullable): a request to put to the person, at most once per
 *   organization: upload a logo
 *
 * What the designer made.
 */
typedef struct {
	VentureEntity *template;
	VentureEntity *version;
	GPtrArray *stripped;
	gchar *ask;
} VentureEmailDesignResult;

/**
 * venture_email_design_result_free:
 * @result: (nullable) (transfer full): a result
 */
void venture_email_design_result_free(VentureEmailDesignResult *result);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureEmailDesignResult, venture_email_design_result_free)

/**
 * venture_email_design_from_brief:
 * @context: the application context
 * @organization_id: the organization
 * @template_id: a template to redesign, or 0 for a new one
 * @name: (nullable): a new template's name
 * @brief: plain words: what the email is for
 * @actor: (nullable): who asked
 * @error: (out) (optional): no AI, an invalid or ungrounded design, refusal
 *
 * Gathers the organization's brand, logo, images and products, asks the
 * AI provider (on the toolless path) for a design, validates it with
 * venture_email_design_validate(), holds every price, amount, percentage
 * and date in its text to the brief and the records, and saves it as a
 * new version.
 *
 * Returns: (transfer full) (nullable): the result
 */
VentureEmailDesignResult *venture_email_design_from_brief(VentureContext *context, gint64 organization_id,
	gint64 template_id, const gchar *name, const gchar *brief, const VentureActor *actor, GError **error);

/**
 * venture_email_design_edit:
 * @context: the application context
 * @organization_id: exact organization
 * @template_id: a template with a design
 * @instruction: plain words: "make the button bigger"
 * @actor: (nullable): who asked
 * @error: (out) (optional): as for venture_email_design_from_brief()
 *
 * Asks the AI provider to change the current design as told, under the
 * same validation and grounding, and saves the result as a new version.
 *
 * Returns: (transfer full) (nullable): the result
 */
VentureEmailDesignResult *venture_email_design_edit(VentureContext *context, gint64 organization_id,
	gint64 template_id, const gchar *instruction, const VentureActor *actor, GError **error);

/**
 * venture_email_design_ground:
 * @design: a valid design as JSON text
 * @sources: (array zero-terminated=1): the brief and the records, as text
 * @stripped: (element-type utf8): where removed sentences are added
 * @error: (out) (optional): a claim in a heading, a label or another
 *   field that cannot lose a sentence
 *
 * The grounding rule, in code: every number in the design's visible text
 * -- a price, an amount, a percentage, a date -- and the word "free" must
 * appear in @sources. A sentence that breaks it is removed and reported.
 *
 * Returns: (transfer full) (nullable): the grounded design
 */
gchar *venture_email_design_ground(const gchar *design, const gchar *const *sources, GPtrArray *stripped, GError **error);

/**
 * venture_email_design_dismiss_logo_ask:
 * @context: the application context
 * @organization_id: the organization
 * @actor: (nullable): who
 * @error: (out) (optional): refusal
 *
 * Records that the organization does not want to be asked for a logo.
 *
 * Returns: whether it was recorded
 */
gboolean venture_email_design_dismiss_logo_ask(VentureContext *context, gint64 organization_id,
	const VentureActor *actor, GError **error);

/**
 * venture_email_design_install:
 * @context: the application context
 *
 * The validators that keep designs, brands, hosted images and versions
 * sound on every writer, and the template actions the assistant offers.
 */
void venture_email_design_install(VentureContext *context);

/**
 * venture_email_design_preview_register:
 *
 * Registers the "email-design" mail preview kind: a template's design as
 * it would be sent.
 */
void venture_email_design_preview_register(void);

G_END_DECLS
#endif
