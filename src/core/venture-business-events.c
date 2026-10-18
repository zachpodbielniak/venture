/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture-business-events-private.h"

#define WRITE_PERMIT "venture-business-events-permit"

gboolean
venture_business_events_check_write(VentureDatabase *database, VentureEntity *entity,
	gboolean removal, GError **error)
{
	if (!VENTURE_IS_BUSINESS_EVENT(entity) && !VENTURE_IS_BUSINESS_EVENT_DELIVERY(entity))
		return TRUE;
	if (!removal && g_object_get_data(G_OBJECT(database), WRITE_PERMIT) == entity)
	{
		g_object_set_data(G_OBJECT(database), WRITE_PERMIT, NULL);
		return TRUE;
	}
	g_set_error_literal(error, VENTURE_ERROR, VENTURE_ERROR_PERMISSION_DENIED,
		"Business event history is written only by its service");
	return FALSE;
}

gboolean
venture_business_events_store(VentureDatabase *database, VentureEntity *entity, GError **error)
{
	gboolean ok;
	g_autoptr(VentureAccessScope) internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	g_object_set_data(G_OBJECT(database), WRITE_PERMIT, entity);
	ok = venture_database_save(database, entity, NULL, error);
	g_object_set_data(G_OBJECT(database), WRITE_PERMIT, NULL);
	return ok;
}

static gboolean
changed_enum(VentureEntity *entity, VentureEntity *previous, const gchar *property, gint expected)
{
	gint now = -1, before = -1;
	g_object_get(entity, property, &now, NULL);
	if (previous != NULL) g_object_get(previous, property, &before, NULL);
	return now == expected && before != now;
}

static const gchar *
event_name(VentureEntity *entity, VentureEntity *previous)
{
	/* A held slot has no business outcome yet. Only the minimal confirmed
	 * transition escapes the reservation's private working-copy boundary. */
	if (VENTURE_IS_BOOKING_RESERVATION(entity))
	{
		g_autofree gchar *state = NULL, *before = NULL;
		g_object_get(entity, "state", &state, NULL);
		if (previous != NULL) g_object_get(previous, "state", &before, NULL);
		if (g_strcmp0(state, "confirmed") == 0 && g_strcmp0(before, "held") == 0)
			return "booking.created";
		if (g_strcmp0(state, "cancelled") == 0 && g_strcmp0(before, "confirmed") == 0)
			return "booking.cancelled";
		return NULL;
	}
	if (venture_entity_is_working_copy(entity)) return NULL;
	if (previous == NULL)
	{
		if (VENTURE_IS_LEAD(entity)) return "lead.created";
		if (VENTURE_IS_SUBSCRIBER(entity)) return "subscriber.created";
		if (VENTURE_IS_PAYMENT(entity) && !venture_payment_is_bookkeeping(VENTURE_PAYMENT(entity))) return "payment.received";
		if (VENTURE_IS_REFUND(entity)) return "payment.refunded";
	}
	if (VENTURE_IS_INVOICE(entity) && changed_enum(entity, previous, "status", VENTURE_INVOICE_STATUS_PAID))
		return "invoice.paid";
	if (VENTURE_IS_QUOTE(entity))
	{
		if (changed_enum(entity, previous, "status", VENTURE_QUOTE_SENT)) return "quote.sent";
		if (changed_enum(entity, previous, "status", VENTURE_QUOTE_ACCEPTED)) return "quote.accepted";
	}
	if (VENTURE_IS_DEAL(entity) && changed_enum(entity, previous, "stage", VENTURE_DEAL_STAGE_WON))
		return "deal.won";
	return NULL;
}

gboolean
venture_business_events_capture(VentureDatabase *database, VentureEntity *entity,
	VentureEntity *previous, GError **error)
{
	const gchar *name = event_name(entity, previous);
	g_autoptr(VentureBusinessEvent) event = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) hooks = NULL;
	g_autoptr(VentureAccessScope) internal = NULL;
	gint64 organization = venture_entity_get_organization_id(entity);
	guint i;
	if (name == NULL || organization <= 0 || venture_access_policy_record_is_personal(
		venture_database_get_access_policy(database), entity)) return TRUE;
	internal = venture_access_policy_enter(venture_database_get_access_policy(database), NULL);
	if (venture_database_get_backend(database) == VENTURE_DATABASE_BACKEND_POSTGRES)
	{
		g_autoptr(OrmResult) lock = venture_database_query_raw(database,
			"SELECT pg_advisory_xact_lock(1936029285, hashtext(current_schema()))", NULL, error);
		/* Sequence allocation alone is not commit ordered. Hold this schema's
		 * feed lock until the outer transaction commits, so a reader can never
		 * advance past a lower cursor that is still uncommitted. SQLite already
		 * serializes writers. A failed lock refuses the whole source write. */
		if (lock == NULL) return FALSE;
	}
	now = venture_time_now();
	event = g_object_new(VENTURE_TYPE_BUSINESS_EVENT, "organization-id", organization,
		"event", name, "occurred-at", now, "source-type", venture_entity_get_entity_name(entity),
		"source-id", venture_entity_get_id(entity), "source-version", venture_entity_get_version(entity), NULL);
	if (!venture_business_events_store(database, VENTURE_ENTITY(event), error)) return FALSE;
	if (!venture_entity_registry_is_type_enabled(venture_entity_registry_get_default(), "webhook"))
	{
		g_autoptr(GError) inspection_error = NULL;
		g_autoptr(OrmInspector) inspector = orm_inspector_new(venture_database_get_connection(database), &inspection_error);
		gboolean present = inspector != NULL && orm_inspector_has_table(inspector, "webhooks", NULL, &inspection_error);
		if (inspection_error != NULL)
		{
			g_propagate_error(error, g_steal_pointer(&inspection_error));
			return FALSE;
		}
		/* A never-enabled module has no subscriptions. Existing subscriptions
		 * still accumulate obligations while their transport is switched off. */
		if (!present) return TRUE;
	}
	query = venture_query_new(VENTURE_TYPE_WEBHOOK);
	venture_query_set_organization(query, organization);
	venture_query_set_limit(query, 0);
	venture_query_add_filter_string(query, "business-events", VENTURE_FILTER_OP_EQ, "true", NULL);
	venture_query_add_filter_string(query, "active", VENTURE_FILTER_OP_EQ, "true", NULL);
	hooks = venture_database_find(database, query, error);
	if (hooks == NULL) return FALSE;
	for (i = 0; i < hooks->len; i++)
	{
		VentureEntity *hook = g_ptr_array_index(hooks, i);
		g_autofree gchar *patterns = NULL;
		g_autoptr(VentureBusinessEventDelivery) delivery = NULL;
		g_object_get(hook, "events", &patterns, NULL);
		if (!venture_webhook_matches(patterns, name)) continue;
		delivery = g_object_new(VENTURE_TYPE_BUSINESS_EVENT_DELIVERY, "organization-id", organization,
			"event-id", venture_entity_get_id(VENTURE_ENTITY(event)), "webhook-id", venture_entity_get_id(hook),
			"next-attempt-at", now, NULL);
		if (!venture_business_events_store(database, VENTURE_ENTITY(delivery), error)) return FALSE;
	}
	return TRUE;
}
