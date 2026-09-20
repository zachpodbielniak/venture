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
 * @as_of: (nullable): the sweep day; %NULL means now; more than a day ahead is refused
 * @limit: at most this many invoices acted on; 0 means 100, capped at 1000
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal or a failure before any invoice was read
 *
 * Selects issued, unpaid, undisputed, unpaused invoices whose policy step is
 * due and not yet recorded, renders and enqueues one reminder per invoice
 * (or the final collect next action), one transaction per invoice. A second
 * sweep over the same day produces nothing. An invoice whose transaction
 * fails is recorded as failed and skipped; the sweep goes on.
 *
 * Returns: reminders enqueued plus escalations created, or -1 when refused
 */
gint venture_dunning_service_sweep(VentureDunningService *self, gint64 organization_id,
	GDateTime *as_of, guint limit, const VentureActor *actor, GError **error);

/**
 * venture_dunning_service_sweep_detailed:
 * @self: the service
 * @organization_id: one legal entity
 * @as_of: (nullable): the sweep day; %NULL means now
 * @limit: at most this many invoices acted on; 0 means 100, capped at 1000
 * @dry_run: plan only: write nothing and return the plan
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal or a failure before any invoice was read
 *
 * The sweep, answering with what it did: an object with =organization_id=,
 * =as_of=, =dry_run=, =invoices=, =queued=, =escalated=, =suppressed=,
 * =failed=, =failed_invoice_ids= and =warnings=, plus =plan= (one entry
 * per step: invoice, step, offset, outcome, reason, recipient, subject)
 * when @dry_run is set. A dry run may look ahead of today.
 *
 * Returns: (transfer full) (nullable): the answer, or %NULL when refused
 */
JsonNode *venture_dunning_service_sweep_detailed(VentureDunningService *self, gint64 organization_id,
	GDateTime *as_of, guint limit, gboolean dry_run, const VentureActor *actor, GError **error);

/**
 * venture_dunning_service_retry:
 * @self: the service
 * @event: a failed, dead or uncertain dunning_event
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal or persistence failure
 *
 * Runs the event's step again for its invoice under a new key, the
 * original key with =:retry:N= appended, so the attempt is its own outbox
 * message. Refused when a later step or attempt is already recorded, the
 * invoice left dunning or is on hold, or the policy lost that offset.
 *
 * Returns: (transfer full) (nullable): the new event
 */
VentureEntity *venture_dunning_service_retry(VentureDunningService *self, VentureEntity *event,
	const VentureActor *actor, GError **error);

/**
 * venture_dunning_service_test_send:
 * @self: the service
 * @policy: a saved dunning_policy
 * @invoice_id: the invoice whose values fill the template
 * @has_offset: whether @offset names the step
 * @offset: the step's offset; otherwise the invoice's next email step
 * @actor: (nullable): the acting user, whose own email receives it
 * @error: (out) (optional): refusal, including an actor with no email
 *
 * Renders one email step and enqueues it to the acting user only, subject
 * prefixed "[TEST]", with a random idempotency key, the customer's pay link
 * replaced by a placeholder and no dunning_event: nothing about the invoice
 * is decided.
 *
 * Returns: (transfer full) (nullable): the queued message
 */
VentureMailMessage *venture_dunning_service_test_send(VentureDunningService *self, VentureEntity *policy,
	gint64 invoice_id, gboolean has_offset, gint64 offset, const VentureActor *actor, GError **error);

/**
 * venture_dunning_check_write: (skip)
 * @database: the database
 * @record: record being written or removed
 * @removal: whether this is a delete, restore or purge
 * @error: (out) (optional): refusal
 *
 * Refuses removing reminder history: a deleted dunning_event still counts
 * as a recorded step while no page shows it, and a purged one lets a step
 * be sent twice.
 *
 * Returns: whether the write may proceed
 */
gboolean venture_dunning_check_write(VentureDatabase *database, VentureEntity *record, gboolean removal, GError **error);

/**
 * venture_dunning_actions_register:
 * @database: database owning the action registry
 *
 * Registers the type-level =sweep= action on =dunning_policy= (the REST
 * endpoint, the CLI verb and the assistant tool), =test_send= on a saved
 * policy and =retry= on a failed, dead or uncertain =dunning_event=.
 */
void venture_dunning_actions_register(VentureDatabase *database);

/**
 * venture_dunning_register_reports:
 * @registry: the report registry
 *
 * Adds the =collections= effectiveness report and the =dunning_worklist=.
 */
void venture_dunning_register_reports(VentureReportRegistry *registry);

/**
 * VentureDunningTimelineAdd:
 * @events: (element-type gpointer): opaque timeline entries owned by the caller
 * @when: (nullable): when the event happened
 * @node: (transfer full): the event as JSON
 *
 * How venture_dunning_append_timeline() hands an event to the timeline.
 */
typedef void (*VentureDunningTimelineAdd)(GPtrArray *events, GDateTime *when, JsonNode *node);

/**
 * venture_dunning_append_timeline: (skip)
 * @context: the wiring
 * @target_type: the record type whose timeline is being built
 * @target_id: the record
 * @limit: at most this many events
 * @add: (scope call): how to add one event
 * @events: (element-type gpointer): opaque timeline entries owned by the caller
 *
 * Adds each reminder for an invoice, or for every invoice of a contact or
 * company, as a =reminder= timeline event, and for an invoice a synthetic
 * "next reminder" entry computed from its policy, which writes nothing.
 * Other types get nothing.
 *
 * This C-only adapter passes the caller's opaque timeline entry storage
 * through to @add. Language bindings should consume the public desk JSON
 * timeline rather than attempt to marshal its private entry structures.
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
