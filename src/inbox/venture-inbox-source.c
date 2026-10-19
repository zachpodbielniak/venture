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
	guint i;
	if (source == NULL || source->channel == NULL || source->list == NULL)
		return;
	if (sources == NULL)
		sources = g_ptr_array_new();
	for (i = 0; i < sources->len; i++)
		if (g_ptr_array_index(sources, i) == (gpointer)source ||
			g_strcmp0(((const VentureInboxSource *)g_ptr_array_index(sources, i))->channel, source->channel) == 0)
			return;
	g_ptr_array_add(sources, (gpointer)source);
}

const VentureInboxSource *
venture_inbox_source_lookup(const gchar *channel)
{
	guint i;
	for (i = 0; channel != NULL && sources != NULL && i < sources->len; i++)
	{
		const VentureInboxSource *source = g_ptr_array_index(sources, i);
		if (g_strcmp0(source->channel, channel) == 0)
			return source;
	}
	return NULL;
}

static gint
by_channel(gconstpointer a, gconstpointer b)
{
	return g_strcmp0((*(const VentureInboxSource *const *)a)->channel, (*(const VentureInboxSource *const *)b)->channel);
}

GPtrArray *
venture_inbox_sources(void)
{
	GPtrArray *copy = g_ptr_array_new();
	guint i;
	for (i = 0; sources != NULL && i < sources->len; i++)
		g_ptr_array_add(copy, g_ptr_array_index(sources, i));
	g_ptr_array_sort(copy, by_channel);
	return copy;
}
