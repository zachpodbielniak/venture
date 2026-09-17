/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_DUNNING_SERVICE_H
#define VENTURE_DUNNING_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_DUNNING_SERVICE (venture_dunning_service_get_type())
G_DECLARE_FINAL_TYPE(VentureDunningService, venture_dunning_service, VENTURE, DUNNING_SERVICE, GObject)

/**
 * venture_dunning_service_get:
 * @database: database owning the records
 *
 * Returns the per-database reminder service, installing its save validators,
 * the outbox eligibility recheck and the delivery-status mirror on first use.
 *
 * Returns: (transfer none): the service; the database owns it
 */
VentureDunningService *venture_dunning_service_get(VentureDatabase *database);

/**
 * venture_dunning_service_sweep:
 * @self: the service
 * @organization_id: one legal entity
 * @as_of: (nullable): the sweep day; %NULL means now
 * @limit: at most this many invoices acted on; 0 means 100, capped at 1000
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal or persistence failure
 *
 * Selects issued, unpaid, undisputed invoices whose policy step is due and
 * not yet recorded, renders and enqueues one reminder per invoice (or the
 * final collect next action), one transaction per invoice. A second sweep
 * over the same day produces nothing.
 *
 * Returns: reminders enqueued plus escalations created, or -1 on failure
 */
gint venture_dunning_service_sweep(VentureDunningService *self, gint64 organization_id,
	GDateTime *as_of, guint limit, const VentureActor *actor, GError **error);

/**
 * venture_dunning_actions_register:
 * @database: database owning the action registry
 *
 * Registers the type-level =sweep= action on =dunning_policy=, which is the
 * REST endpoint, the CLI verb and the assistant tool.
 */
void venture_dunning_actions_register(VentureDatabase *database);

/**
 * venture_dunning_register_reports:
 * @registry: the report registry
 *
 * Adds the =collections= effectiveness report.
 */
void venture_dunning_register_reports(VentureReportRegistry *registry);

/**
 * VentureDunningTimelineAdd:
 * @events: the timeline under construction
 * @when: (nullable): when the event happened
 * @node: (transfer full): the event as JSON
 *
 * How venture_dunning_append_timeline() hands an event to the timeline.
 */
typedef void (*VentureDunningTimelineAdd)(GPtrArray *events, GDateTime *when, JsonNode *node);

/**
 * venture_dunning_append_timeline:
 * @context: the wiring
 * @target_type: the record type whose timeline is being built
 * @target_id: the record
 * @limit: at most this many events
 * @add: how to add one event
 * @events: the timeline under construction
 *
 * Adds each reminder for an invoice, or for every invoice of a contact or
 * company, as a =reminder= timeline event. Other types get nothing.
 */
void venture_dunning_append_timeline(VentureContext *context, const gchar *target_type,
	gint64 target_id, guint limit, VentureDunningTimelineAdd add, GPtrArray *events);

/**
 * venture_dunning_portal_summary:
 * @database: database owning the records
 * @invoice: an invoice shown in the customer portal
 *
 * Returns: (transfer full): "reminder sent <date>" lines for the customer, or an empty string
 */
gchar *venture_dunning_portal_summary(VentureDatabase *database, VentureEntity *invoice);
G_END_DECLS
#endif
