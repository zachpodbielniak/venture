/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_MAIL_PREVIEW_H
#define VENTURE_MAIL_PREVIEW_H
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "mail/venture-mail-records.h"
#include "mail/venture-mail-outbox.h"
#include "core/venture-context.h"
G_BEGIN_DECLS

/**
 * VENTURE_MAIL_PREVIEW_PLACEHOLDER_URL:
 *
 * What a preview, and a test send, shows in place of a private bearer link
 * (a pay link, a quote acceptance link, a portal invitation, a campaign's
 * unsubscribe or tracking link). Reserved under .invalid so it can never
 * resolve, and named so a reader sees at once that something was withheld.
 */
#define VENTURE_MAIL_PREVIEW_PLACEHOLDER_URL "https://private-link.invalid/withheld-in-preview"

/**
 * VENTURE_MAIL_PREVIEW_CLIP_BYTES:
 *
 * The HTML size above which a major webmail client clips the message
 * behind a "view entire message" link (about 102 KB). The size check warns
 * above it.
 */
#define VENTURE_MAIL_PREVIEW_CLIP_BYTES (102 * 1024)

/**
 * VENTURE_MAIL_PREVIEW_MAX_REACHABILITY:
 *
 * At most this many distinct addresses are asked about per preview, so one
 * preview holds the main loop for a bounded time.
 */
#define VENTURE_MAIL_PREVIEW_MAX_REACHABILITY (10)

/**
 * VentureMailCheckStatus:
 * @VENTURE_MAIL_CHECK_OK: the check passed
 * @VENTURE_MAIL_CHECK_WARN: worth a look; the message can still go
 * @VENTURE_MAIL_CHECK_FAIL: the message has a real defect
 *
 * The outcome of one pre-send check.
 */
typedef enum {
	VENTURE_MAIL_CHECK_OK,
	VENTURE_MAIL_CHECK_WARN,
	VENTURE_MAIL_CHECK_FAIL
} VentureMailCheckStatus;

/**
 * VentureMailCheck:
 * @id: stable identifier: plain-text, image-alt, links, size, unsubscribe, or source
 * @status: the outcome
 * @message: what was found, in plain words
 *
 * One pre-send check.
 */
typedef struct {
	gchar *id;
	VentureMailCheckStatus status;
	gchar *message;
} VentureMailCheck;

/**
 * VentureMailPreview:
 * @kind: the kind id, or "message" for a stored outbox row
 * @to: the recipient
 * @subject: the subject line
 * @text: the plain-text alternative the transport would send, private links replaced
 * @html: (nullable): the HTML alternative the transport would send, or %NULL when it sends none
 * @headers: (element-type utf8): extra headers the transport would add, "Name: value"
 * @attachments: (element-type utf8): attachment file names
 * @checks: (element-type VentureMailCheck): the pre-send checks, in a fixed order
 * @bulk: whether the kind is bulk or marketing mail, which must carry an unsubscribe
 *
 * What one outgoing message would look like on the wire, with every private
 * bearer link replaced by %VENTURE_MAIL_PREVIEW_PLACEHOLDER_URL. Nothing in
 * it is a credential, so it may be shown and serialised.
 */
typedef struct {
	gchar *kind;
	gchar *to;
	gchar *subject;
	gchar *text;
	gchar *html;
	GPtrArray *headers;
	GPtrArray *attachments;
	GPtrArray *checks;
	gboolean bulk;
} VentureMailPreview;

/**
 * venture_mail_preview_free:
 * @preview: (nullable) (transfer full): a preview
 */
void venture_mail_preview_free(VentureMailPreview *preview);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(VentureMailPreview, venture_mail_preview_free)

/**
 * venture_mail_preview_get_check:
 * @preview: a preview
 * @id: check identifier
 * Returns: (transfer none) (nullable): the check, or %NULL
 */
const VentureMailCheck *venture_mail_preview_get_check(const VentureMailPreview *preview, const gchar *id);

#define VENTURE_TYPE_MAIL_LINK_CHECKER (venture_mail_link_checker_get_type())
G_DECLARE_INTERFACE(VentureMailLinkChecker, venture_mail_link_checker, VENTURE, MAIL_LINK_CHECKER, GObject)

/**
 * VentureMailLinkCheckerInterface:
 * @check: whether one http or https address answers
 *
 * Reachability for the links check. Tests install a fake; nothing in a
 * preview touches the network unless a checker is handed in.
 */
struct _VentureMailLinkCheckerInterface {
	GTypeInterface parent_iface;
	gboolean (*check)(VentureMailLinkChecker *self, const gchar *url, GCancellable *cancellable, GError **error);
	/*< private >*/
	gpointer padding[4];
};

/**
 * venture_mail_link_checker_check:
 * @self: a checker
 * @url: an http or https address
 * @cancellable: (nullable): cancellation
 * @error: (out) (optional): why it did not answer
 * Returns: whether the address answered
 */
gboolean venture_mail_link_checker_check(VentureMailLinkChecker *self, const gchar *url, GCancellable *cancellable, GError **error);

#define VENTURE_TYPE_HTTP_LINK_CHECKER (venture_http_link_checker_get_type())
G_DECLARE_FINAL_TYPE(VentureHttpLinkChecker, venture_http_link_checker, VENTURE, HTTP_LINK_CHECKER, GObject)

/**
 * venture_http_link_checker_new:
 *
 * The default checker, used only when an operator asks for reachability:
 * one HEAD request per address with a short timeout, no redirects followed,
 * and every address the assistant's fetch guard refuses (loopback,
 * link-local, site-local) refused without a request.
 *
 * Returns: (transfer full): a checker
 */
VentureHttpLinkChecker *venture_http_link_checker_new(void);

/**
 * venture_mail_visible_bodies:
 * @message: a message, saved or not
 * @text: (out) (transfer full): the plain-text alternative, never %NULL
 * @html: (out) (transfer full) (nullable): the HTML alternative, or %NULL
 *
 * The two alternatives the SMTP transport sends, by its rule: a private text
 * body replaces both public bodies (so an HTML client cannot hide a bearer
 * link), and a private HTML body, when present, replaces the HTML. The SMTP
 * mailer calls this, so a preview can never disagree with it.
 */
void venture_mail_visible_bodies(VentureMailMessage *message, gchar **text, gchar **html);

/**
 * venture_mail_preview_render:
 * @message: an unsaved message, or a stored outbox row
 * @kind: (nullable): kind id, or %NULL for "message"
 * @bulk: whether an unsubscribe link and header are legally required
 * @checker: (nullable): reachability checker; %NULL checks syntax only
 * @cancellable: (nullable): cancellation for reachability
 *
 * Renders what the transport would send and runs the pre-send checks.
 * Performs no database access and, with no @checker, no network access.
 *
 * Returns: (transfer full): the preview
 */
VentureMailPreview *venture_mail_preview_render(VentureMailMessage *message, const gchar *kind,
	gboolean bulk, VentureMailLinkChecker *checker, GCancellable *cancellable);

/**
 * VentureMailPreviewBuildFunc:
 * @context: the application context
 * @organization_id: the organization the source record must belong to
 * @source_id: the source record
 * @now: the clock the sender would run by
 * @notes: (element-type utf8): where a builder adds plain-words warnings
 *   about how the preview may differ from the real send
 * @error: (out) (optional): why nothing would be sent
 *
 * Builds the unsaved message the sender would enqueue for @source_id,
 * writing nothing. The sender's own enqueue path calls the same function.
 *
 * Returns: (transfer full) (nullable): the unsaved message
 */
typedef VentureMailMessage *(*VentureMailPreviewBuildFunc)(VentureContext *context,
	gint64 organization_id, gint64 source_id, GDateTime *now, GPtrArray *notes, GError **error);

/**
 * VentureMailPreviewKind:
 * @id: the kind id used in routes and the CLI
 * @label: a plain-words name
 * @module: the module that must be on
 * @source_type: the record type @build is given an id of
 * @related_type: (nullable): the outbox related-type its messages carry
 * @bulk: whether the kind is bulk or marketing mail
 * @build: the builder
 *
 * One previewable kind, registered by the module that sends it.
 */
typedef struct {
	const gchar *id;
	const gchar *label;
	const gchar *module;
	const gchar *source_type;
	const gchar *related_type;
	gboolean bulk;
	VentureMailPreviewBuildFunc build;
} VentureMailPreviewKind;

/**
 * venture_mail_preview_register:
 * @kind: (transfer none): a kind with static storage
 *
 * Adds a kind; a second kind with the same id is refused with a critical.
 */
void venture_mail_preview_register(const VentureMailPreviewKind *kind);

/**
 * venture_mail_preview_lookup:
 * @id: kind id
 * Returns: (transfer none) (nullable): the kind
 */
const VentureMailPreviewKind *venture_mail_preview_lookup(const gchar *id);

/**
 * venture_mail_preview_kinds:
 * Returns: (transfer container) (element-type VentureMailPreviewKind): every kind, in registration order
 */
GPtrArray *venture_mail_preview_kinds(void);

/**
 * venture_mail_preview_build:
 * @context: the application context
 * @kind: kind id
 * @organization_id: exact organization
 * @source_id: source record
 * @now: (nullable): the sender's clock, or now
 * @checker: (nullable): reachability checker
 * @error: (out) (optional): unknown kind, module off, missing record, or the sender's refusal
 *
 * Builds the kind's message exactly as its sender would enqueue it, without
 * enqueueing, and renders it. Builder notes become "source" warnings.
 *
 * Returns: (transfer full) (nullable): the preview
 */
VentureMailPreview *venture_mail_preview_build(VentureContext *context, const gchar *kind,
	gint64 organization_id, gint64 source_id, GDateTime *now, VentureMailLinkChecker *checker, GError **error);

/**
 * venture_mail_preview_message:
 * @context: the application context
 * @organization_id: exact organization
 * @message_id: a stored mail_message
 * @checker: (nullable): reachability checker
 * @error: (out) (optional): not found
 *
 * Previews a message already in the outbox, whatever its state. A row whose
 * related-type belongs to a bulk kind, or that carries an unsubscribe
 * endpoint, is judged as bulk.
 *
 * Returns: (transfer full) (nullable): the preview
 */
VentureMailPreview *venture_mail_preview_message(VentureContext *context, gint64 organization_id,
	gint64 message_id, VentureMailLinkChecker *checker, GError **error);

/**
 * venture_mail_preview_to_json:
 * @preview: a preview
 * Returns: (transfer full): the preview as JSON; it carries no private link
 */
JsonNode *venture_mail_preview_to_json(const VentureMailPreview *preview);

/**
 * venture_mail_preview_actor_email:
 * @database: the database
 * @actor: (nullable): the acting user
 * @error: (out) (optional): no user, or a user with no email
 * Returns: (transfer full) (nullable): the acting user's own email address
 */
gchar *venture_mail_preview_actor_email(VentureDatabase *database, const VentureActor *actor, GError **error);

/**
 * venture_mail_preview_enqueue_test:
 * @database: the database whose outbox queues it
 * @message: the unsaved message the sender built
 * @key: idempotency key; the same key returns the same row
 * @actor: the acting user, whose own email receives it
 * @error: (out) (optional): refusal
 *
 * Queues a copy of @message to the acting user's own address only, through
 * the ordinary outbox: subject prefixed "[TEST] ", the visible bodies with
 * every private bearer link replaced by the placeholder, no private bodies
 * and no unsubscribe endpoint, no Cc or Bcc, attachments kept. It is related
 * to no source record, so no sender's eligibility hook acts on it.
 *
 * Returns: (transfer full) (nullable): the queued row
 */
VentureMailMessage *venture_mail_preview_enqueue_test(VentureDatabase *database, VentureMailMessage *message,
	const gchar *key, const VentureActor *actor, GError **error);

/**
 * venture_mail_preview_send_test:
 * @context: the application context
 * @kind: kind id
 * @organization_id: exact organization
 * @source_id: source record
 * @token: a UUID naming this click; repeating it returns the same row
 * @actor: the acting user
 * @error: (out) (optional): refusal
 *
 * Builds the kind's message and queues it with
 * venture_mail_preview_enqueue_test(), keyed by kind, source and @token.
 *
 * Returns: (transfer full) (nullable): the queued row
 */
VentureMailMessage *venture_mail_preview_send_test(VentureContext *context, const gchar *kind,
	gint64 organization_id, gint64 source_id, const gchar *token, const VentureActor *actor, GError **error);

G_END_DECLS
#endif
