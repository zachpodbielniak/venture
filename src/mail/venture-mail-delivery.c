/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Outbox delivery that does not need the automation engine. A hosted
 * workspace refuses the engine (it is platform machinery), so the
 * mail_deliver rule never runs there; this timer delivers instead. */
#include <venture.h>

#define MAIL_DELIVERY_SECONDS (5)
#define MAIL_DELIVERY_BATCH (100)
#define BOOKING_REPLY_SYNC_SECONDS (5)
#define BOOKING_REPLY_REFRESH_SECONDS (60)

/* Only the organization mailer can lack an account; it then refuses every
 * message permanently. Skipping keeps mail queued until sending is set up.
 * Any other transport was put there on purpose and is always ready. */
static gboolean
mail_delivery_can_send(VentureContext *context, gint64 organization)
{
	g_autoptr(VentureIntegrationConnection) connection = NULL;
	if (!VENTURE_IS_ORGANIZATION_MAILER(venture_context_get_mailer(context))) return TRUE;
	connection = venture_integration_service_find(venture_integration_service_get(
		venture_context_get_database(context)), organization, "smtp", NULL);
	return connection != NULL;
}

gint
venture_mail_delivery_sweep(VentureContext *context, GError **error)
{
	VentureMailOutbox *outbox;
	g_autoptr(VentureQuery) query = venture_query_new(VENTURE_TYPE_ORGANIZATION);
	g_autoptr(GPtrArray) businesses = NULL;
	gint total = 0;
	guint i;
	g_return_val_if_fail(VENTURE_IS_CONTEXT(context), -1);
	outbox = venture_context_get_mail_outbox(context);
	if (!outbox) return 0;
	venture_query_add_filter_int(query, "active", VENTURE_FILTER_OP_EQ, TRUE, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	businesses = venture_database_find(venture_context_get_database(context), query, error);
	if (!businesses) return -1;
	for (i = 0; i < businesses->len; i++) {
		gint64 organization = venture_entity_get_id(g_ptr_array_index(businesses, i));
		g_autoptr(GError) failure = NULL;
		gint attempted;
		if (!mail_delivery_can_send(context, organization)) continue;
		attempted = venture_mail_outbox_deliver_due(outbox, organization, MAIL_DELIVERY_BATCH, NULL, NULL, &failure);
		/* One business's broken outbox must not hold up the others. */
		if (attempted < 0) {
			g_warning("Mail delivery for organization %" G_GINT64_FORMAT ": %s", organization, failure->message);
			continue;
		}
		total += attempted;
	}
	return total;
}

/* Like the other timers, it runs only outside an authenticated request or
 * transaction, so it never borrows a caller's authority. Where an engine is
 * attached, its rules own delivery and this stays out of the way. */
static gboolean
mail_delivery_tick(gpointer data)
{
	g_autoptr(VentureContext) context = g_object_ref(data);
	VentureDatabase *database = venture_context_get_database(context);
	VentureTenantService *tenant = venture_tenant_service_get(database);
	g_autoptr(GError) error = NULL;
	if (!venture_tenant_service_is_enabled(tenant) || venture_context_get_automation(context) ||
		venture_database_has_transaction(database) || venture_access_policy_get_actor(venture_database_get_access_policy(database)))
		return G_SOURCE_CONTINUE;
	if (!venture_tenant_service_check_operation(tenant, TRUE, &error))
		return G_SOURCE_CONTINUE;
	if (venture_mail_delivery_sweep(context, &error) < 0)
		g_warning("Mail delivery did not run: %s", error->message);
	return G_SOURCE_CONTINUE;
}

/* Hosted workspaces do not run automation rules. Visit one due Lightsite
 * mailbox on each short tick; its shared inbox can serve many businesses,
 * but the sync service admits only that business's delivery alias. */
static gboolean
booking_reply_sync_tick(gpointer data)
{
	VentureContext *context = data;
	VentureDatabase *database = venture_context_get_database(context);
	VentureTenantService *tenant = venture_tenant_service_get(database);
	g_autoptr(VentureQuery) query = NULL;
	g_autoptr(GPtrArray) accounts = NULL;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GError) error = NULL;
	guint i;
	if (!venture_tenant_service_is_enabled(tenant) || venture_context_get_automation(context) ||
		!venture_context_module_enabled(context, "mail_sync") || venture_database_has_transaction(database) ||
		venture_access_policy_get_actor(venture_database_get_access_policy(database))) return G_SOURCE_CONTINUE;
	if (!venture_tenant_service_check_operation(tenant, TRUE, &error)) return G_SOURCE_CONTINUE;
	query = venture_query_new(VENTURE_TYPE_MAIL_ACCOUNT);
	venture_query_add_filter_string(query, "active", VENTURE_FILTER_OP_EQ, "true", NULL);
	venture_query_add_filter_string(query, "reply-alias-prefix", VENTURE_FILTER_OP_NE, "", NULL);
	venture_query_add_order(query, "last-synced-at", VENTURE_SORT_ASCENDING, NULL);
	venture_query_add_order(query, "id", VENTURE_SORT_ASCENDING, NULL);
	venture_query_set_limit(query, 1000);
	accounts = venture_database_find(database, query, &error);
	if (!accounts) {
		g_warning("Booking reply mailbox sweep could not list accounts: %s", error ? error->message : "unknown error");
		return G_SOURCE_CONTINUE;
	}
	now = g_date_time_new_now_utc();
	for (i = 0; i < accounts->len; i++) {
		VentureEntity *account = g_ptr_array_index(accounts, i);
		g_autoptr(GDateTime) last = NULL, next = NULL;
		g_autoptr(VentureMailSyncService) service = NULL;
		g_object_get(account, "last-synced-at", &last, "next-attempt-at", &next, NULL);
		if ((next && g_date_time_compare(next, now) > 0) ||
			(last && g_date_time_difference(now, last) < BOOKING_REPLY_REFRESH_SECONDS * G_USEC_PER_SEC)) continue;
		service = venture_mail_sync_service_new_for_context(context);
		g_object_set(service, "message-budget", 25u, "time-budget", 5u, NULL);
		if (venture_mail_sync_service_sync(service, account, NULL, &error) < 0 &&
			!g_error_matches(error, VENTURE_ERROR, VENTURE_ERROR_CONFLICT))
			g_warning("Booking reply mailbox %" G_GINT64_FORMAT " did not sync: %s",
				venture_entity_get_id(account), error ? error->message : "unknown error");
		break;
	}
	return G_SOURCE_CONTINUE;
}

static void
mail_delivery_source_free(gpointer data)
{
	guint source = GPOINTER_TO_UINT(data);
	if (source) g_source_remove(source);
}

void
venture_mail_delivery_start(VentureContext *context)
{
	guint source;
	g_return_if_fail(VENTURE_IS_CONTEXT(context));
	if (g_object_get_data(G_OBJECT(context), "mail-delivery-source")) return;
	source = g_timeout_add_seconds(MAIL_DELIVERY_SECONDS, mail_delivery_tick, context);
	g_object_set_data_full(G_OBJECT(context), "mail-delivery-source", GUINT_TO_POINTER(source), mail_delivery_source_free);
	source = g_timeout_add_seconds(BOOKING_REPLY_SYNC_SECONDS, booking_reply_sync_tick, context);
	g_object_set_data_full(G_OBJECT(context), "mail-reply-sync-source", GUINT_TO_POINTER(source), mail_delivery_source_free);
}

void
venture_mail_delivery_stop(VentureContext *context)
{
	g_return_if_fail(VENTURE_IS_CONTEXT(context));
	g_object_set_data(G_OBJECT(context), "mail-delivery-source", NULL);
	g_object_set_data(G_OBJECT(context), "mail-reply-sync-source", NULL);
}
