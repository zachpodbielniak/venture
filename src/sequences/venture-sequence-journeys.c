/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"
#include "sequences/venture-sequence-service-private.h"

/* --- The channel hook ------------------------------------------------------ */

/* Process-wide, like the mail preview kinds: a channel is code, not data.
 * Registered on the main thread at start-up (or by a test). */
typedef struct { VentureSequenceChannelExecute execute; gpointer data; } Channel;
static GHashTable *channels;

void
venture_sequence_channel_register(gint channel, VentureSequenceChannelExecute execute, gpointer data)
{
	Channel *entry;
	g_return_if_fail(execute != NULL);
	g_return_if_fail(channel != 0 && channel != 4);
	if (channels == NULL)
		channels = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
	entry = g_new0(Channel, 1);
	entry->execute = execute;
	entry->data = data;
	g_hash_table_replace(channels, GINT_TO_POINTER(channel), entry);
}

void
venture_sequence_channel_unregister(gint channel)
{
	if (channels != NULL)
		g_hash_table_remove(channels, GINT_TO_POINTER(channel));
}

gboolean
venture_sequence_channel_lookup(gint channel, VentureSequenceChannelExecute *execute, gpointer *data)
{
	Channel *entry = channels != NULL ? g_hash_table_lookup(channels, GINT_TO_POINTER(channel)) : NULL;
	if (entry == NULL)
		return FALSE;
	if (execute != NULL) *execute = entry->execute;
	if (data != NULL) *data = entry->data;
	return TRUE;
}

/* --- Branches ---------------------------------------------------------------- */

static gint64
number(VentureEntity *row, const gchar *field)
{
	gint64 value = 0;
	g_object_get(row, field, &value, NULL);
	return value;
}

static gint
choice(VentureEntity *row, const gchar *field)
{
	gint value = 0;
	g_object_get(row, field, &value, NULL);
	return value;
}

gboolean
venture_sequence_branch_taken(VentureDatabase *database, VentureEntity *enrollment, VentureEntity *step,
	GPtrArray *history, GDateTime *deadline, gboolean *taken, GError **error)
{
	VentureEntity *email = NULL;
	g_autoptr(GDateTime) sent = NULL;
	gint64 org = venture_entity_get_organization_id(enrollment);
	gint on = choice(step, "branch-on");
	guint i;
	*taken = FALSE;
	/* The latest email of this enrollment before the branch: the one the
	 * person could have opened, clicked or answered. */
	for (i = 0; i < history->len; i++)
	{
		VentureEntity *row = g_ptr_array_index(history, i);
		gint state = choice(row, "state");
		if (choice(row, "channel") == 0 && (state == 0 || state == 1))
			email = row;
	}
	if (email == NULL)
		return TRUE;
	g_object_get(email, "scheduled-at", &sent, NULL);
	if (on == 1 || on == 2)
	{
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_SEQUENCE_TRACKING_EVENT);
		g_autoptr(GPtrArray) events = NULL;
		g_autofree gchar *cutoff = venture_time_to_string(deadline);
		venture_query_set_organization(query, org);
		venture_query_set_limit(query, 1000);
		venture_query_add_filter_int(query, "delivery-id", VENTURE_FILTER_OP_EQ, venture_entity_get_id(email), NULL);
		venture_query_add_filter_string(query, "occurred-at", VENTURE_FILTER_OP_LTE, cutoff, NULL);
		events = venture_database_find(database, query, error);
		if (events == NULL)
			return FALSE;
		for (i = 0; i < events->len && !*taken; i++)
		{
			VentureEntity *event = g_ptr_array_index(events, i);
			g_autofree gchar *machine = NULL;
			g_object_get(event, "machine-reason", &machine, NULL);
			*taken = choice(event, "kind") == (on == 1 ? 0 : 1) && (machine == NULL || *machine == '\0');
		}
		return TRUE;
	}
	if (on == 3)
	{
		/* The one reply rule: anything inbound from them, after the email
		 * and by the end of the wait. */
		g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_INTERACTION);
		g_autoptr(GPtrArray) found = NULL;
		g_autofree gchar *cutoff = venture_time_to_string(deadline);
		venture_query_set_organization(query, org);
		venture_query_set_limit(query, 1000);
		venture_query_add_filter_int(query, "contact-id", VENTURE_FILTER_OP_EQ, number(enrollment, "contact-id"), NULL);
		venture_query_add_filter_string(query, "occurred-at", VENTURE_FILTER_OP_LTE, cutoff, NULL);
		if (sent != NULL)
		{
			g_autofree gchar *after = venture_time_to_string(sent);
			venture_query_add_filter_string(query, "occurred-at", VENTURE_FILTER_OP_GTE, after, NULL);
		}
		found = venture_database_find(database, query, error);
		if (found == NULL)
			return FALSE;
		for (i = 0; i < found->len && !*taken; i++)
		{
			gboolean outbound = TRUE;
			g_object_get(g_ptr_array_index(found, i), "outbound", &outbound, NULL);
			*taken = !outbound;
		}
	}
	return TRUE;
}
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
void venture_sequence_journeys_install(VentureDatabase *database) { (void)database; }
