/* SPDX-License-Identifier: AGPL-3.0-or-later */
/*
 * venture-inbox-source.c - The registry channels join from their own files
 */
#include <venture.h>

/* Filled by constructors before main() and read afterwards, so it needs
 * no lock: no channel registers from a thread. */
static GPtrArray *sources;

VentureInboxItem *
venture_inbox_item_new(const VentureInboxSource *source, VentureEntity *record, GDateTime *at, gboolean inbound)
{
	VentureInboxItem *item = g_new0(VentureInboxItem, 1);
	item->source = source;
	item->record = record != NULL ? g_object_ref(record) : NULL;
	item->at = at != NULL ? g_date_time_ref(at) : NULL;
	item->inbound = inbound;
	return item;
}

void
venture_inbox_item_free(VentureInboxItem *item)
{
	if (item == NULL)
		return;
	g_clear_object(&item->record);
	g_clear_pointer(&item->at, g_date_time_unref);
	g_free(item);
}

void
venture_inbox_source_register(const VentureInboxSource *source)
{
	(void)source;
}

const VentureInboxSource *
venture_inbox_source_lookup(const gchar *channel)
{
	(void)channel;
	return NULL;
}

GPtrArray *
venture_inbox_sources(void)
{
	(void)sources;
	return g_ptr_array_new();
}
