/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Behaviour segments (docs/marketing.org, "Tags and behaviour"). A saved
 * search may ask for a tag and for people who opened, clicked, replied,
 * bought or booked within N days. Each behaviour reads the evidence that
 * already exists -- campaign recipients' first-human-open and click
 * stamps, sequence tracking events not judged a machine's, inbound
 * interactions (the one reply rule), paid invoices and the business
 * feed's booking.created -- and becomes an id filter on the ordinary
 * query, so the preview's row bound still counts what was asked. */
#include "marketing/venture-marketing-segments.h"
#include <string.h>

/* At most this many people for one behaviour: past it the segment asks for
 * more than a preview may freeze, and narrowing is the answer. */
#define SEGMENT_MAX_IDS 10000

static const gchar *const behaviour_names[SEGMENT_BEHAVIOURS] = {
	"opened_within_days", "clicked_within_days", "replied_within_days", "bought_within_days", "booked_within_days"
};

static gboolean
refuse(GError **error, const gchar *message)
{
	venture_set_error_validation(error, "filters", "%s", message);
	return FALSE;
}

void
venture_marketing_criteria_clear(VentureMarketingCriteria *criteria)
{
	g_clear_pointer(&criteria->tags, g_strfreev);
}

gboolean
venture_marketing_criteria_take(GHashTable *params, gboolean contacts, gboolean tagged,
	VentureMarketingCriteria *criteria, GError **error)
{
	const gchar *tag = g_hash_table_lookup(params, "tag");
	guint i;
	criteria->tags = NULL;
	for (i = 0; i < SEGMENT_BEHAVIOURS; i++) criteria->within[i] = -1;
	if (tag != NULL)
	{
		g_auto(GStrv) tags = venture_tags_parse(tag);
		if (!tagged) return refuse(error, "This record type has no tags");
		if (!tags[0]) return refuse(error, "Name a tag");
		for (i = 0; tags[i]; i++)
			if (!venture_tags_valid(tags[i])) return refuse(error, "A tag is up to 64 characters with no line break");
		criteria->tags = g_steal_pointer(&tags);
		g_hash_table_remove(params, "tag");
	}
	for (i = 0; i < SEGMENT_BEHAVIOURS; i++)
	{
		const gchar *text = g_hash_table_lookup(params, behaviour_names[i]);
		gint64 days = 0;
		if (text == NULL) continue;
		if (!contacts) return refuse(error, "Opened, clicked, replied, bought and booked are about people: use a contact segment");
		if (!g_ascii_string_to_signed(text, 10, 1, 3650, &days, NULL))
			return refuse(error, "A behaviour window is a whole number of days, 1 to 3650");
		criteria->within[i] = days;
		g_hash_table_remove(params, behaviour_names[i]);
	}
	return TRUE;
}

/* Rows of @type in the organization with @field in [from, to]. */
static GPtrArray *
window_rows(VentureDatabase *database, GType type, gint64 org, const gchar *field,
	GDateTime *from, GDateTime *to, GError **error)
{
	g_autoptr(VentureQuery) query = venture_query_new(type);
	g_autofree gchar *low = venture_time_to_string(from), *high = venture_time_to_string(to);
	GPtrArray *rows;
	venture_query_set_organization(query, org);
	venture_query_set_limit(query, SEGMENT_MAX_IDS * 4 + 1);
	if (!venture_query_add_filter_string(query, field, VENTURE_FILTER_OP_GTE, low, error) ||
		!venture_query_add_filter_string(query, field, VENTURE_FILTER_OP_LTE, high, error))
		return NULL;
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(database, query, error);
	if (rows != NULL && rows->len > SEGMENT_MAX_IDS * 4)
	{
		g_ptr_array_unref(rows);
		refuse(error, "Too much activity in that window to segment on; shorten it");
		return NULL;
	}
	return rows;
}

static void
add_id(GHashTable *ids, gint64 id)
{
	if (id > 0) g_hash_table_add(ids, g_memdup2(&id, sizeof id));
}

static gint64
number(VentureEntity *row, const gchar *field)
{
	gint64 value = 0;
	g_object_get(row, field, &value, NULL);
	return value;
}

static gboolean
enabled(const gchar *type)
{
	return venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), type);
}

/* People with one behaviour in [from, to]. */
static GHashTable *
behaved(VentureDatabase *database, gint64 org, guint behaviour, GDateTime *from, GDateTime *to, GError **error)
{
	g_autoptr(GHashTable) ids = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
	g_autoptr(GPtrArray) rows = NULL;
	guint i;
	if (behaviour == SEGMENT_OPENED || behaviour == SEGMENT_CLICKED)
	{
		/* opened-at and clicked-at are only ever a person's: a machine's
		 * open fills machine-opened-at instead (lane 3). */
		rows = window_rows(database, VENTURE_TYPE_MARKETING_RECIPIENT, org,
			behaviour == SEGMENT_OPENED ? "opened-at" : "clicked-at", from, to, error);
		if (rows == NULL) return NULL;
		for (i = 0; i < rows->len; i++) add_id(ids, number(g_ptr_array_index(rows, i), "contact-id"));
		g_clear_pointer(&rows, g_ptr_array_unref);
		if (enabled("sequence_tracking_event"))
		{
			rows = window_rows(database, VENTURE_TYPE_SEQUENCE_TRACKING_EVENT, org, "occurred-at", from, to, error);
			if (rows == NULL) return NULL;
			for (i = 0; i < rows->len; i++)
			{
				VentureEntity *event = g_ptr_array_index(rows, i);
				g_autofree gchar *machine = NULL;
				gint kind = 0;
				g_object_get(event, "kind", &kind, "machine-reason", &machine, NULL);
				if ((machine && *machine) || kind != (behaviour == SEGMENT_OPENED ? 0 : 1)) continue;
				add_id(ids, number(event, "contact-id"));
			}
		}
	}
	else if (behaviour == SEGMENT_REPLIED)
	{
		/* The one reply rule (venture_sequence_interaction_is_reply()):
		 * anything inbound from them. */
		rows = window_rows(database, VENTURE_TYPE_INTERACTION, org, "occurred-at", from, to, error);
		if (rows == NULL) return NULL;
		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *row = g_ptr_array_index(rows, i);
			gboolean outbound = TRUE;
			g_object_get(row, "outbound", &outbound, NULL);
			if (!outbound) add_id(ids, number(row, "contact-id"));
		}
	}
	else if (behaviour == SEGMENT_BOUGHT)
	{
		if (!enabled("invoice")) return g_steal_pointer(&ids);
		rows = window_rows(database, VENTURE_TYPE_INVOICE, org, "paid-at", from, to, error);
		if (rows == NULL) return NULL;
		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *row = g_ptr_array_index(rows, i);
			gint status = 0;
			g_object_get(row, "status", &status, NULL);
			if (status == VENTURE_INVOICE_STATUS_PAID && !venture_entity_is_deleted(row))
				add_id(ids, number(row, "contact-id"));
		}
	}
	else
	{
		/* A reservation has no confirmed-at: the feed's booking.created is
		 * when the booking was made. */
		if (!enabled("booking_reservation") || !enabled("activity")) return g_steal_pointer(&ids);
		rows = window_rows(database, VENTURE_TYPE_BUSINESS_EVENT, org, "occurred-at", from, to, error);
		if (rows == NULL) return NULL;
		for (i = 0; i < rows->len; i++)
		{
			VentureEntity *event = g_ptr_array_index(rows, i);
			g_autofree gchar *name = NULL;
			g_autoptr(VentureEntity) reservation = NULL, activity = NULL;
			g_object_get(event, "event", &name, NULL);
			if (g_strcmp0(name, "booking.created")) continue;
			reservation = venture_database_get(database, VENTURE_TYPE_BOOKING_RESERVATION, number(event, "source-id"), NULL);
			if (reservation == NULL || venture_entity_get_organization_id(reservation) != org) continue;
			activity = venture_database_get(database, VENTURE_TYPE_ACTIVITY, number(reservation, "activity-id"), NULL);
			if (activity != NULL && venture_entity_get_organization_id(activity) == org)
				add_id(ids, number(activity, "contact-id"));
		}
	}
	return g_steal_pointer(&ids);
}

gboolean
venture_marketing_criteria_narrow(VentureDatabase *database, gint64 org, const VentureMarketingCriteria *criteria,
	GDateTime *now, VentureQuery *query, gboolean *empty, GError **error)
{
	g_autoptr(GHashTable) chosen = NULL;
	guint i;
	*empty = FALSE;
	for (i = 0; criteria->tags && criteria->tags[i]; i++)
	{
		g_autofree gchar *pattern = g_strdup_printf("%%%s%%", criteria->tags[i]);
		if (!venture_query_add_filter_string(query, "tags", VENTURE_FILTER_OP_ILIKE, pattern, error)) return FALSE;
	}
	for (i = 0; i < SEGMENT_BEHAVIOURS; i++)
	{
		g_autoptr(GDateTime) from = NULL;
		g_autoptr(GHashTable) ids = NULL;
		if (criteria->within[i] < 0) continue;
		from = g_date_time_add_days(now, -(gint)criteria->within[i]);
		ids = behaved(database, org, i, from, now, error);
		if (ids == NULL) return FALSE;
		if (chosen == NULL) chosen = g_steal_pointer(&ids);
		else
		{
			GHashTableIter iter;
			gpointer key;
			g_hash_table_iter_init(&iter, chosen);
			while (g_hash_table_iter_next(&iter, &key, NULL))
				if (!g_hash_table_contains(ids, key)) g_hash_table_iter_remove(&iter);
		}
	}
	if (chosen == NULL) return TRUE;
	if (g_hash_table_size(chosen) == 0) { *empty = TRUE; return TRUE; }
	if (g_hash_table_size(chosen) > SEGMENT_MAX_IDS)
		return refuse(error, "More than 10000 people behaved that way; narrow the segment");
	{
		g_autoptr(GPtrArray) values = g_ptr_array_new_with_free_func(g_free);
		GHashTableIter iter;
		gpointer key;
		g_hash_table_iter_init(&iter, chosen);
		while (g_hash_table_iter_next(&iter, &key, NULL))
			g_ptr_array_add(values, g_strdup_printf("%" G_GINT64_FORMAT, *(gint64 *)key));
		return venture_query_add_filter(query, "id", VENTURE_FILTER_OP_IN, values, error);
	}
}

gboolean
venture_marketing_criteria_keep(const VentureMarketingCriteria *criteria, VentureEntity *row)
{
	g_autofree gchar *tags = NULL;
	guint i;
	if (criteria->tags == NULL) return TRUE;
	g_object_get(row, "tags", &tags, NULL);
	for (i = 0; criteria->tags[i]; i++)
		if (!venture_tags_has(tags, criteria->tags[i])) return FALSE;
	return TRUE;
}
