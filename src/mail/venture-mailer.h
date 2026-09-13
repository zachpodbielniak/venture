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
 */
struct _VentureMailerInterface {
	GTypeInterface parent_iface;
	gboolean (*send)(VentureMailer *, VentureMailMessage *, GCancellable *, GError **);
};
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
 * @callback: completion on the initiating context
 * @user_data: callback data
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
/** venture_log_mailer_new:
 * Returns: (transfer full): an in-memory recording transport
 */
VentureLogMailer *venture_log_mailer_new(void);
/** venture_log_mailer_get_messages:
 * @self: the recording transport
 * Returns: (transfer none) (element-type VentureMailMessage): snapshots, read after sends complete
 */
const GPtrArray *venture_log_mailer_get_messages(VentureLogMailer *self);
/** venture_log_mailer_set_error:
 * @self: the recording transport
 * @error: (nullable): copied error to return after recording each attempt
 */
void venture_log_mailer_set_error(VentureLogMailer *self, const GError *error);
G_END_DECLS
#endif
