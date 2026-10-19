/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_LATE_FEE_SERVICE_H
#define VENTURE_LATE_FEE_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS

/*
 * Late fees. A schedule of tiers decides what an overdue invoice would be
 * charged and when; the dunning sweep is the only thing that moves a fee
 * forward (one stage of the same sweep, never a second one), the settlement
 * service is the only thing that adds one to a balance (a "late_fee" invoice
 * event) or takes one off it (a "late_fee_waiver" credit), and every
 * decision is a late_fee row written through the ordinary audited save.
 */

#define VENTURE_TYPE_LATE_FEE_SERVICE (venture_late_fee_service_get_type())
G_DECLARE_FINAL_TYPE(VentureLateFeeService, venture_late_fee_service, VENTURE, LATE_FEE_SERVICE, GObject)

/**
 * venture_late_fee_service_get:
 * @database: database owning the records
 *
 * Returns the per-database late-fee service, installing its save validators
 * on first use.
 *
 * Returns: (transfer none): the service; the database owns it
 */
VentureLateFeeService *venture_late_fee_service_get(VentureDatabase *database);

/**
 * venture_late_fee_service_preview:
 * @self: the service
 * @invoice_id: an issued invoice
 * @as_of: (nullable): "today" for the preview; %NULL is now
 * @error: (out) (optional): refusal
 *
 * What the invoice's schedule would charge, as a full timeline, writing
 * nothing: an object with =invoice_id=, =schedule_id=, =schedule_name=,
 * =currency=, =enabled=, =auto_apply=, =paused= (with =pause_scope= and
 * =paused_until=), =exempt=, =charged= and =entries=, one per tier
 * occurrence in date order with =day=, =date=, =tier=, =type=, =amount=
 * (exact), =display=, =status= (a recorded fee's status, or =upcoming=,
 * =due=, =capped=, =skipped_condition=, =before_start=), =reason= and
 * =notice= (=sent=, =queued=, =skipped=, =none=). Occurrences are listed
 * up to 365 days late.
 *
 * Returns: (transfer full) (nullable): the preview
 */
JsonNode *venture_late_fee_service_preview(VentureLateFeeService *self, gint64 invoice_id,
	GDateTime *as_of, GError **error);

/**
 * venture_late_fee_service_approve:
 * @self: the service
 * @fee: a proposed late_fee
 * @date: (nullable): the adjustment date; %NULL is the fee's due day
 * @actor: (nullable): audit actor, recorded as the decider
 * @error: (out) (optional): refusal
 *
 * Applies a proposed fee through VentureSettlementService as a separate
 * "late_fee" invoice event; the invoice's lines are never touched.
 *
 * Returns: %TRUE when applied
 */
gboolean venture_late_fee_service_approve(VentureLateFeeService *self, VentureEntity *fee,
	GDateTime *date, const VentureActor *actor, GError **error);

/**
 * venture_late_fee_service_skip:
 * @self: the service
 * @fee: a proposed, scheduled or awaiting-notice late_fee
 * @reason: (nullable): why
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 *
 * Decides that this occurrence is not charged. Nothing reaches the balance.
 *
 * Returns: %TRUE when recorded
 */
gboolean venture_late_fee_service_skip(VentureLateFeeService *self, VentureEntity *fee,
	const gchar *reason, const VentureActor *actor, GError **error);

/**
 * venture_late_fee_service_skip_notice:
 * @self: the service
 * @fee: a late_fee whose advance notice was not sent
 * @reason: (nullable): why the owner is skipping the notice
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 *
 * The owner's explicit decision that this fee may proceed without the
 * advance notice. Without it, or a reminder that was sent and stated the
 * fee, no fee is ever proposed or applied.
 *
 * Returns: %TRUE when recorded
 */
gboolean venture_late_fee_service_skip_notice(VentureLateFeeService *self, VentureEntity *fee,
	const gchar *reason, const VentureActor *actor, GError **error);

/**
 * venture_late_fee_service_waive:
 * @self: the service
 * @fee: an applied late_fee
 * @reason: why; required
 * @note: (nullable): a note queued to the customer through the mail outbox
 * @date: (nullable): the waiver's date; %NULL is today
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 *
 * Waives (reverses) an applied fee in one action: a "late_fee_waiver"
 * credit through VentureSettlementService applied to the invoice, so the
 * balance, the statement and the reports move together.
 *
 * Returns: %TRUE when waived
 */
gboolean venture_late_fee_service_waive(VentureLateFeeService *self, VentureEntity *fee,
	const gchar *reason, const gchar *note, GDateTime *date, const VentureActor *actor, GError **error);

/**
 * venture_late_fee_service_decide:
 * @self: the service
 * @organization_id: one legal entity; every fee must belong to it
 * @ids: (array length=n_ids): late_fee ids
 * @n_ids: how many
 * @approve: approve (%TRUE) or skip (%FALSE)
 * @reason: (nullable): recorded on each skipped fee
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal before anything was decided
 *
 * Approves or skips several proposed fees, each in its own transaction, so
 * one refusal does not undo the others.
 *
 * Returns: (transfer full) (nullable): =decided=, =failed= and =errors=
 */
JsonNode *venture_late_fee_service_decide(VentureLateFeeService *self, gint64 organization_id,
	const gint64 *ids, gsize n_ids, gboolean approve, const gchar *reason,
	const VentureActor *actor, GError **error);

/**
 * venture_late_fee_service_pause:
 * @self: the service
 * @scope: "invoice", "company" or "organization"
 * @id: the invoice, the company or the organization
 * @until: (nullable): the first day fees may apply again; %NULL pauses indefinitely
 * @reason: (nullable): why
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 *
 * Pauses late fees for one invoice, every invoice of a customer, or the
 * whole organization. While paused nothing is proposed or added and no
 * reminder carries fee wording. The write is an ordinary audited save.
 *
 * Returns: %TRUE when saved
 */
gboolean venture_late_fee_service_pause(VentureLateFeeService *self, const gchar *scope, gint64 id,
	GDateTime *until, const gchar *reason, const VentureActor *actor, GError **error);

/**
 * venture_late_fee_service_resume:
 * @self: the service
 * @scope: "invoice", "company" or "organization"
 * @id: the record
 * @actor: (nullable): audit actor
 * @error: (out) (optional): refusal
 *
 * Returns: %TRUE when saved
 */
gboolean venture_late_fee_service_resume(VentureLateFeeService *self, const gchar *scope, gint64 id,
	const VentureActor *actor, GError **error);

/**
 * venture_late_fee_service_pause_state:
 * @self: the service
 * @invoice: an invoice
 * @at: the day judged
 *
 * Returns: (transfer full) (nullable): "late fees paused ... until <date>"
 *   or "... indefinitely", naming the scope, or %NULL when fees may apply
 */
gchar *venture_late_fee_service_pause_state(VentureLateFeeService *self, VentureEntity *invoice, GDateTime *at);

/**
 * VentureLateFeeSweepTally:
 * @proposed: fees put up for approval
 * @applied: fees added under auto-apply
 * @awaiting_notice: fees due whose advance notice was not sent
 * @capped: occurrences a cap reduced to nothing
 * @cancelled: pending fees cancelled (settled, schedule changed)
 * @failed: invoices whose late-fee stage failed
 *
 * The late-fee stage's counts in a dunning sweep's answer.
 */
typedef struct
{
	guint proposed;
	guint applied;
	guint awaiting_notice;
	guint capped;
	guint cancelled;
	guint failed;
} VentureLateFeeSweepTally;

/**
 * venture_late_fee_service_sweep_invoice: (skip)
 * @self: the service
 * @invoice: an issued invoice the dunning sweep is visiting
 * @as_of: the sweep day
 * @dry_run: plan only
 * @plan: (nullable): the dry run's plan, appended to
 * @tally: counts, added to
 * @actor: (nullable): audit actor
 * @error: (out) (optional): failure; the sweep records it and goes on
 *
 * The late-fee stage of the dunning sweep for one invoice: due occurrences
 * become proposed (or applied under auto-apply) once their notice was sent
 * or skipped; nothing is written twice for one occurrence. Runs in its own
 * transaction, before the reminder stage, so a reminder sent today shows a
 * fee added today.
 *
 * Returns: %TRUE when anything was written (or would be, for a dry run)
 */
gboolean venture_late_fee_service_sweep_invoice(VentureLateFeeService *self, VentureEntity *invoice,
	GDateTime *as_of, gboolean dry_run, JsonArray *plan, VentureLateFeeSweepTally *tally,
	const VentureActor *actor, GError **error);

/**
 * venture_late_fee_service_sweep_needed: (skip)
 * @self: the service
 * @organization_id: one legal entity
 *
 * Returns: %TRUE when late fees are switched on for the organization, so
 *   the dunning sweep must visit every overdue invoice, not only those with
 *   a reminder step due
 */
gboolean venture_late_fee_service_sweep_needed(VentureLateFeeService *self, gint64 organization_id);

/**
 * venture_late_fee_service_notice_text: (skip)
 * @self: the service
 * @invoice: the invoice a reminder is being rendered for
 * @as_of: the reminder's day
 *
 * The advance-notice sentence for the next fee strictly after @as_of, for
 * example "A $25.00 late fee will be added on 2026-01-25." Empty while late
 * fees are off, paused or exempt, or when no fee would follow: no fee
 * wording goes to the customer then.
 *
 * Returns: (transfer full): the sentence, or an empty string
 */
gchar *venture_late_fee_service_notice_text(VentureLateFeeService *self, VentureEntity *invoice, GDateTime *as_of);

/**
 * venture_late_fee_service_record_notice: (skip)
 * @self: the service
 * @invoice: the invoice
 * @event: the dunning_event that carried the notice
 * @as_of: the reminder's day
 * @actor: (nullable): audit actor
 * @error: (out) (optional): failure, inside the reminder's transaction
 *
 * Remembers that @event announced the next fee, with the amount stated,
 * so the fee may follow once the reminder is sent.
 *
 * Returns: %TRUE on success, including when nothing was announced
 */
gboolean venture_late_fee_service_record_notice(VentureLateFeeService *self, VentureEntity *invoice,
	VentureEntity *event, GDateTime *as_of, const VentureActor *actor, GError **error);

/**
 * venture_late_fee_check_write: (skip)
 * @database: the database
 * @record: record being written or removed
 * @removal: whether this is a delete, restore or purge
 * @error: (out) (optional): refusal
 *
 * Refuses removing a late_fee: a deleted fee is an occurrence the sweep
 * would charge again.
 *
 * Returns: whether the write may proceed
 */
gboolean venture_late_fee_check_write(VentureDatabase *database, VentureEntity *record, gboolean removal, GError **error);

/**
 * venture_late_fee_actions_register:
 * @database: database owning the action registry
 *
 * Registers =approve=, =skip=, =skip_notice= and =waive= on =late_fee=,
 * the type-level =decide= (bulk) on =late_fee=, =pause_late_fees= and
 * =resume_late_fees= on =invoice=, =company= and =late_fee_setting=, and
 * =late_fee_preview= on =invoice=. The writing ones are stageable, so the
 * assistant reaches them through the staged-change approval.
 */
void venture_late_fee_actions_register(VentureDatabase *database);

/**
 * venture_late_fee_register_reports:
 * @registry: the report registry
 *
 * Adds the =late_fees= report: applied, waived and collected per period.
 */
void venture_late_fee_register_reports(VentureReportRegistry *registry);

/**
 * VentureLateFeeTimelineAdd:
 * @events: (element-type gpointer): opaque timeline entries owned by the caller
 * @when: (nullable): when the event happened
 * @node: (transfer full): the event as JSON
 *
 * How venture_late_fee_append_timeline() hands an event to the timeline.
 */
typedef void (*VentureLateFeeTimelineAdd)(GPtrArray *events, GDateTime *when, JsonNode *node);

/**
 * venture_late_fee_append_timeline: (skip)
 * @context: the wiring
 * @target_type: "invoice" or "company"
 * @target_id: the record
 * @limit: at most this many events
 * @add: (scope call): how to add one event
 * @events: (element-type gpointer): opaque timeline entries
 *
 * Adds each fee as a =late_fee= timeline entry, and for an invoice the
 * computed next fee or the pause in force (written nowhere).
 */
void venture_late_fee_append_timeline(VentureContext *context, const gchar *target_type,
	gint64 target_id, guint limit, VentureLateFeeTimelineAdd add, GPtrArray *events);

G_END_DECLS
#endif
