/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>
G_DEFINE_INTERFACE(VentureMailer, venture_mailer, G_TYPE_OBJECT)
static void venture_mailer_default_init(VentureMailerInterface *iface) { }
gboolean venture_mailer_send(VentureMailer *self, VentureMailMessage *message, GCancellable *cancellable, GError **error)
{
	g_return_val_if_fail(VENTURE_IS_MAILER(self), FALSE);
	g_return_val_if_fail(VENTURE_IS_MAIL_MESSAGE(message), FALSE);
	if (g_cancellable_set_error_if_cancelled(cancellable, error)) return FALSE;
	return VENTURE_MAILER_GET_IFACE(self)->send(self, message, cancellable, error);
}
static void send_task(GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
	g_autoptr(GError) error = NULL;
	if (venture_mailer_send(source, data, cancellable, &error)) g_task_return_boolean(task, TRUE);
	else g_task_return_error(task, g_steal_pointer(&error));
}
void venture_mailer_send_async(VentureMailer *self, VentureMailMessage *message, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
	g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);
	/* An accepted send must win over cancellation racing with completion. */
	g_task_set_check_cancellable(task, FALSE);
	g_task_set_task_data(task, g_object_ref(message), g_object_unref);
	g_task_run_in_thread(task, send_task);
}
gboolean venture_mailer_send_finish(VentureMailer *self, GAsyncResult *result, GError **error)
{
	g_return_val_if_fail(g_task_is_valid(result, self), FALSE);
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
