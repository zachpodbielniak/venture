/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
G_DEFINE_INTERFACE(VentureMailer, venture_mailer, G_TYPE_OBJECT)
static void venture_mailer_default_init(VentureMailerInterface *iface) { }
void venture_mailer_get_binding(VentureMailer *self, gint64 *connection_id, gint64 *version)
{
	g_return_if_fail(VENTURE_IS_MAILER(self));
	g_return_if_fail(connection_id != NULL && version != NULL);
	*connection_id = 0;
	*version = 0;
	if (VENTURE_MAILER_GET_IFACE(self)->get_binding != NULL)
		VENTURE_MAILER_GET_IFACE(self)->get_binding(self, connection_id, version);
}
VentureMailer *venture_mailer_prepare(VentureMailer *self, VentureMailMessage *message, GError **error)
{
	g_autoptr(VentureMailer) transport = NULL;
	VentureMailerInterface *iface;
	g_return_val_if_fail(VENTURE_IS_MAILER(self), NULL);
	g_return_val_if_fail(VENTURE_IS_MAIL_MESSAGE(message), NULL);
	iface = VENTURE_MAILER_GET_IFACE(self);
	transport = iface->prepare ? iface->prepare(self, message, error) : g_object_ref(self);
	if (transport == NULL)
	{
		if (error == NULL || *error == NULL)
			g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "Mail transport is unavailable");
		return NULL;
	}
	if (!VENTURE_IS_MAILER(transport) || VENTURE_MAILER_GET_IFACE(transport)->send == NULL ||
		VENTURE_MAILER_GET_IFACE(transport)->prepare != NULL)
	{
		g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "Mail selection requires a concrete transport");
		return NULL;
	}
	return g_steal_pointer(&transport);
}
gboolean venture_mailer_send(VentureMailer *self, VentureMailMessage *message, GCancellable *cancellable, GError **error)
{
	g_autoptr(VentureMailer) transport = NULL;
	g_return_val_if_fail(VENTURE_IS_MAILER(self), FALSE);
	g_return_val_if_fail(VENTURE_IS_MAIL_MESSAGE(message), FALSE);
	if (g_cancellable_set_error_if_cancelled(cancellable, error)) return FALSE;
	transport = venture_mailer_prepare(self, message, error);
	return transport != NULL && VENTURE_MAILER_GET_IFACE(transport)->send(transport, message, cancellable, error);
}
typedef struct {
	VentureMailer *transport;
	VentureMailMessage *message;
} SendData;
static void send_data_free(gpointer data)
{
	SendData *send = data;
	g_object_unref(send->transport);
	g_object_unref(send->message);
	g_free(send);
}
static void send_task(GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
	g_autoptr(GError) error = NULL;
	SendData *send = data;
	(void)source;
	if (g_cancellable_set_error_if_cancelled(cancellable, &error))
		g_task_return_error(task, g_steal_pointer(&error));
	else if (VENTURE_MAILER_GET_IFACE(send->transport)->send(send->transport, send->message, cancellable, &error))
		g_task_return_boolean(task, TRUE);
	else
	{
		if (error == NULL) g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_MAIL_PERMANENT, "Mail transport failed without a result");
		g_task_return_error(task, g_steal_pointer(&error));
	}
}
void venture_mailer_send_async(VentureMailer *self, VentureMailMessage *message, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
	g_autoptr(GTask) task = NULL;
	g_autoptr(VentureMailer) transport = NULL;
	g_autoptr(GError) error = NULL;
	SendData *send;
	g_return_if_fail(VENTURE_IS_MAILER(self));
	g_return_if_fail(VENTURE_IS_MAIL_MESSAGE(message));
	task = g_task_new(self, cancellable, callback, user_data);
	g_task_set_source_tag(task, venture_mailer_send_async);
	/* An accepted send must win over cancellation racing with completion. */
	g_task_set_check_cancellable(task, FALSE);
	if (!g_cancellable_set_error_if_cancelled(cancellable, &error))
		transport = venture_mailer_prepare(self, message, &error);
	if (transport == NULL)
	{
		if (error == NULL) g_set_error_literal(&error, VENTURE_ERROR, VENTURE_ERROR_CONFIG, "Mail transport is unavailable");
		g_task_return_error(task, g_steal_pointer(&error));
		return;
	}
	send = g_new0(SendData, 1);
	send->transport = g_steal_pointer(&transport);
	send->message = g_object_ref(message);
	g_task_set_task_data(task, send, send_data_free);
	g_task_run_in_thread(task, send_task);
}
gboolean venture_mailer_send_finish(VentureMailer *self, GAsyncResult *result, GError **error)
{
	g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
	g_return_val_if_fail(g_async_result_is_tagged(result, venture_mailer_send_async), FALSE);
	return g_task_propagate_boolean(G_TASK(result), error);
}
struct _VentureLogMailer {
	GObject parent_instance;
	GPtrArray *messages;
	GError *error;
	GMutex mutex;
};
static void log_iface(VentureMailerInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(VentureLogMailer, venture_log_mailer, G_TYPE_OBJECT,
	G_IMPLEMENT_INTERFACE(VENTURE_TYPE_MAILER, log_iface))
static gboolean log_send(VentureMailer *mailer, VentureMailMessage *message, GCancellable *cancellable, GError **error)
{
	VentureLogMailer *self = VENTURE_LOG_MAILER(mailer);

	VentureMailMessage *copy = venture_mail_message_new();
	gboolean ok;
	venture_entity_copy_properties_from(VENTURE_ENTITY(copy), VENTURE_ENTITY(message), FALSE);
	g_mutex_lock(&self->mutex);
	g_ptr_array_add(self->messages, copy);
	ok = self->error == NULL;
	if (!ok) g_propagate_error(error, g_error_copy(self->error));
	g_mutex_unlock(&self->mutex);
	return ok;
}
static void log_iface(VentureMailerInterface *iface) { iface->send = log_send; }
static void log_finalize(GObject *object)
{
	VentureLogMailer *self = VENTURE_LOG_MAILER(object);
	g_ptr_array_unref(self->messages);
	g_clear_error(&self->error);
	g_mutex_clear(&self->mutex);
	G_OBJECT_CLASS(venture_log_mailer_parent_class)->finalize(object);
}
static void venture_log_mailer_class_init(VentureLogMailerClass *klass) { G_OBJECT_CLASS(klass)->finalize = log_finalize; }
static void venture_log_mailer_init(VentureLogMailer *self)
{
	self->messages = g_ptr_array_new_with_free_func(g_object_unref);
	g_mutex_init(&self->mutex);
}
VentureLogMailer *venture_log_mailer_new(void) { return g_object_new(VENTURE_TYPE_LOG_MAILER, NULL); }
const GPtrArray *venture_log_mailer_get_messages(VentureLogMailer *self) { return self->messages; }
void venture_log_mailer_set_error(VentureLogMailer *self, const GError *error)
{
	g_mutex_lock(&self->mutex);
	g_clear_error(&self->error);
	if (error) self->error = g_error_copy(error);
	g_mutex_unlock(&self->mutex);
}
