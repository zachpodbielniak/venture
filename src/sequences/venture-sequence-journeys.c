/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include "sequences/venture-sequence-service-private.h"

void venture_sequence_channel_register(gint channel, VentureSequenceChannelExecute execute, gpointer data)
{ (void)channel; (void)execute; (void)data; }
void venture_sequence_channel_unregister(gint channel) { (void)channel; }
typedef struct { VentureSequenceEmailGate gate; gpointer data; } EmailGate;

void
venture_sequence_add_email_gate(VentureDatabase *database, VentureSequenceEmailGate gate, gpointer data)
{
	GArray *gates;
	EmailGate entry;
	g_return_if_fail(VENTURE_IS_DATABASE(database));
	g_return_if_fail(gate != NULL);
	gates = g_object_get_data(G_OBJECT(database), "venture-sequence-email-gates");
	if (gates == NULL)
	{
		gates = g_array_new(FALSE, FALSE, sizeof(EmailGate));
		g_object_set_data_full(G_OBJECT(database), "venture-sequence-email-gates", gates, (GDestroyNotify)g_array_unref);
	}
	entry.gate = gate;
	entry.data = data;
	g_array_append_val(gates, entry);
}

gboolean
venture_sequence_email_gates_ask(VentureDatabase *database, VentureEntity *sequence, VentureEntity *contact,
	GDateTime *as_of, gchar **skip_reason, GError **error)
{
	GArray *gates = g_object_get_data(G_OBJECT(database), "venture-sequence-email-gates");
	guint i;
	*skip_reason = NULL;
	for (i = 0; gates != NULL && i < gates->len && *skip_reason == NULL; i++)
	{
		EmailGate *entry = &g_array_index(gates, EmailGate, i);
		if (!entry->gate(database, sequence, contact, as_of, skip_reason, entry->data, error))
			return FALSE;
	}
	return TRUE;
}
gint venture_sequence_service_run_triggers(VentureSequenceService *self, gint64 organization_id,
	GDateTime *now, const VentureActor *actor, GError **error)
{ (void)self; (void)organization_id; (void)now; (void)actor; (void)error; return 0; }
gboolean venture_sequence_service_record_open_full(VentureSequenceService *self, const gchar *token,
	const gchar *user_agent, GDateTime *now, const VentureActor *actor, GError **error)
{ (void)user_agent; return venture_sequence_service_record_open(self, token, now, actor, error); }
gchar *venture_sequence_service_record_click_full(VentureSequenceService *self, const gchar *token,
	gint64 position, const gchar *user_agent, GDateTime *now, const VentureActor *actor, GError **error)
{ (void)user_agent; return venture_sequence_service_record_click(self, token, position, now, actor, error); }
void venture_sequence_journeys_install(VentureDatabase *database) { (void)database; }
