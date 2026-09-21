/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_MAILER_H
#define VENTURE_MAILER_H
#include <gio/gio.h>
#include "mail/venture-mail-records.h"
G_BEGIN_DECLS
#define VENTURE_TYPE_MAILER (venture_mailer_get_type())
G_DECLARE_INTERFACE(VentureMailer, venture_mailer, VENTURE, MAILER, GObject)
/**
 * VentureMailerInterface:
 * @send: one submission; uncertain acceptance must report MAIL_UNCERTAIN
 * @prepare: (nullable): resolve a concrete transport on the caller's thread
 * @get_binding: (nullable): return immutable connection identity, without I/O
 */
struct _VentureMailerInterface {
	GTypeInterface parent_iface;
	gboolean (*send)(VentureMailer *self, VentureMailMessage *message, GCancellable *cancellable, GError **error);
	VentureMailer *(*prepare)(VentureMailer *self, VentureMailMessage *message, GError **error);
	void (*get_binding)(VentureMailer *self, gint64 *connection_id, gint64 *version);
	/*< private >*/
	gpointer padding[8];
};
/**
 * venture_mailer_prepare:
 * @self: transport or organization-aware selector
 * @message: message supplying the verified business organization
 * @error: (out) (optional): configuration or authorization refusal
 *
 * Resolves an immutable concrete transport on the caller's thread before
 * any worker runs. Selectors may read the database here, never from send.
 * Concrete transports implement send and leave prepare unset. The returned
 * transport must not itself require preparation. No message is submitted.
 *
 * Returns: (transfer full) (nullable): concrete transport, or NULL on refusal
 */
VentureMailer *venture_mailer_prepare(VentureMailer *self, VentureMailMessage *message, GError **error);
/**
 * venture_mailer_get_binding:
 * @self: prepared concrete transport
 * @connection_id: (out): immutable integration identity, or zero for an explicitly injected transport
 * @version: (out): configuration version, or zero when unbound
 *
 * Reads identity without database access. The outbox persists it before any
 * submission and rejects changing a retained message's connection identity.
 */
void venture_mailer_get_binding(VentureMailer *self, gint64 *connection_id, gint64 *version);
/**
 * venture_mailer_send:
 * @self: the transport
 * @message: immutable for the duration of submission
 * @cancellable: (nullable): cancellation
 * @error: (out) (optional): submission error
 * Returns: whether the relay accepted the message
 */
gboolean venture_mailer_send(VentureMailer *self, VentureMailMessage *message, GCancellable *cancellable, GError **error);
/**
 * venture_mailer_send_async:
 * @self: the transport
 * @message: message, retained until completion
 * @cancellable: (nullable): cancellation
 * @callback: (scope async) (closure user_data): completion on the initiating context
 * @user_data: (nullable): callback data
 */
void venture_mailer_send_async(VentureMailer *self, VentureMailMessage *message, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);
/**
 * venture_mailer_send_finish:
 * @self: the transport
 * @result: async result
 * @error: (out) (optional): submission error
 * Returns: whether the relay accepted the message
 */
gboolean venture_mailer_send_finish(VentureMailer *self, GAsyncResult *result, GError **error);
#define VENTURE_TYPE_LOG_MAILER (venture_log_mailer_get_type())
G_DECLARE_FINAL_TYPE(VentureLogMailer, venture_log_mailer, VENTURE, LOG_MAILER, GObject)
/**
 * venture_log_mailer_new:
 * Returns: (transfer full): an in-memory recording transport
 */
VentureLogMailer *venture_log_mailer_new(void);
/**
 * venture_log_mailer_get_messages:
 * @self: the recording transport
 * Returns: (transfer none) (element-type VentureMailMessage): snapshots, read after sends complete
 */
const GPtrArray *venture_log_mailer_get_messages(VentureLogMailer *self);
/**
 * venture_log_mailer_set_error:
 * @self: the recording transport
 * @error: (nullable): copied error to return after recording each attempt
 */
void venture_log_mailer_set_error(VentureLogMailer *self, const GError *error);
G_END_DECLS
#endif
