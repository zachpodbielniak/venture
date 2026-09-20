/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <venture.h>

/* Timers run only outside an authenticated request/transaction. They never
 * inherit another caller's authority while synchronous HTTP pumps the loop. */
static gboolean
stripe_collection_tick(gpointer data)
{
	g_autoptr(VentureContext) context = g_object_ref(data);
	VentureDatabase *database = venture_context_get_database(context);
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_STRIPE_AUTHORIZATION);
	g_autoptr(GPtrArray) rows = NULL;
	g_autoptr(VentureStripeService) configured = NULL;
	VentureStripeService *service;
	g_autoptr(GDateTime) now = venture_time_now();
	g_autofree gchar *cutoff = venture_time_to_string(now);
	g_autoptr(GError) error = NULL;
	VentureEntity *authorization;
	VentureActor actor;
	gint64 binding;
	if (!venture_context_module_enabled(context, "stripe") || !venture_context_module_enabled(context, "billing") ||
		venture_database_has_transaction(database) || venture_access_policy_get_actor(venture_database_get_access_policy(database)))
		return G_SOURCE_CONTINUE;
	venture_query_set_limit(query, 1);
	venture_query_add_filter_int(query, "enabled", VENTURE_FILTER_OP_EQ, TRUE, NULL);
	venture_query_add_filter_string(query, "status", VENTURE_FILTER_OP_EQ, "active", NULL);
	venture_query_add_filter_string(query, "next-check-at", VENTURE_FILTER_OP_LTE, cutoff, NULL);
	venture_query_add_order(query, "next-check-at", VENTURE_SORT_ASCENDING, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	rows = venture_database_find(database, query, &error);
	if (!rows || !rows->len) return G_SOURCE_CONTINUE;
	authorization = g_ptr_array_index(rows, 0);
	g_object_get(authorization, "connection-id", &binding, NULL);
	actor.kind = VENTURE_ACTOR_KIND_SYSTEM;
	actor.name = "authorized subscription collection";
	actor.prompt = NULL;
	actor.request_id = NULL;
	actor.approved_by = NULL;
	/* The context override is explicit test injection. Production constructs
	 * from the retained organization and enabled connection on every tick. */
	service = venture_context_get_stripe_service(context);
	if (!service)
	{
		configured = venture_stripe_service_for_connection(database,
			venture_entity_get_organization_id(authorization), binding, FALSE, NULL, &error);
		service = configured;
	}
	if (service && venture_stripe_service_collect_due(service, now, 1, &actor, &error) >= 0)
		return G_SOURCE_CONTINUE;
	{
		g_autoptr(GDateTime) next = g_date_time_add_minutes(now, 5);
		g_clear_error(&error);
		g_object_set(authorization, "next-check-at", next, "collection-note",
			"Collection paused: verify this organization's Stripe connection and authorization settings", NULL);
		venture_stripe_save_owned(database, authorization, &actor, &error);
	}
	return G_SOURCE_CONTINUE;
}

static void
stripe_collection_source_free(gpointer data)
{
	guint source = GPOINTER_TO_UINT(data);
	if (source) g_source_remove(source);
}

void
venture_stripe_collection_start(VentureContext *context)
{
	guint source;
	g_return_if_fail(VENTURE_IS_CONTEXT(context));
	if (g_object_get_data(G_OBJECT(context), "stripe-collection-source")) return;
	source = g_timeout_add_seconds(5, stripe_collection_tick, context);
	g_object_set_data_full(G_OBJECT(context), "stripe-collection-source", GUINT_TO_POINTER(source), stripe_collection_source_free);
}

void
venture_stripe_collection_stop(VentureContext *context)
{
	g_return_if_fail(VENTURE_IS_CONTEXT(context));
	g_object_set_data(G_OBJECT(context), "stripe-collection-source", NULL);
}
