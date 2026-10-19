/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

void venture_sequence_channel_register(gint channel, VentureSequenceChannelExecute execute, gpointer data)
{ (void)channel; (void)execute; (void)data; }
void venture_sequence_channel_unregister(gint channel) { (void)channel; }
void venture_sequence_add_email_gate(VentureDatabase *database, VentureSequenceEmailGate gate, gpointer data)
{ (void)database; (void)gate; (void)data; }
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
