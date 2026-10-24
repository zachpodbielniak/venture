/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_LIGHTSITE_BILLING_H
#define VENTURE_LIGHTSITE_BILLING_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
/**
 * venture_lightsite_billing_notifications:
 * @database: hosted source database
 * @billing: configured operator billing organization
 * @business: customer business identity
 * @error: (out) (optional): unauthorized or incomplete evidence
 *
 * Owner-facing facts for one business: verified methods, upcoming charges,
 * receipts, failed collection, cancellation and a committed guarantee.
 * Writes nothing and returns no payment credentials.
 *
 * Returns: (transfer full) (nullable): scoped events with stable identities
 */
JsonNode *venture_lightsite_billing_notifications(VentureDatabase *database, gint64 billing,
	gint64 business, GError **error);
/**
 * venture_lightsite_billing_cancel:
 * @database: hosted database
 * @billing: configured billing organization
 * @business: enrolled customer business
 * @error: return location for an error
 *
 * Cancels an unpublished enrollment with no invoice immediately and without a
 * charge, inside its thirty-day setup window or after it expired. The
 * subscription and immutable receipt commit together; delayed publication,
 * setup and collection cannot revive it. After publication or an invoice,
 * cancellation uses normal period-end cancellation and preserves incurred
 * invoices, including outstanding prepaid installments. Requires the trusted
 * service authority; the hosted client authenticates its dashboard owner.
 *
 * Returns: (transfer full) (nullable): the original cancellation decision
 */
JsonNode *venture_lightsite_billing_cancel(VentureDatabase *database, gint64 billing,
	gint64 business, GError **error);
/**
 * venture_lightsite_billing_prepay:
 * @database: hosted database
 * @billing: configured billing organization
 * @business: customer business
 * @price_id: annual fixed price
 * @payment_path: single, installments, ach, wire or split
 * @shares: (nullable): two or three split amounts in integer minor units of the prepaid total; collection scales them to Stripe charge units
 * @created: (out) (optional): whether enrollment was created
 * @error: return location for an error
 *
 * Reserves an immutable 24-month prepaid offer without issuing an invoice.
 * Requires trusted service authority. Only publication may start collection.
 *
 * Returns: (transfer full) (nullable): frozen terms and enrollment identity
 */
JsonNode *venture_lightsite_billing_prepay(VentureDatabase *database, gint64 billing,
	gint64 business, gint64 price_id, const gchar *payment_path, JsonArray *shares, gboolean *created, GError **error);
/**
 * venture_lightsite_billing_enroll:
 * @database: hosted database
 * @billing: configured billing organization
 * @business: customer business
 * @price_id: active monthly or annual price
 * @created: (out) (optional): whether enrollment was created
 * @error: return location for an error
 *
 * Starts a deferred subscription with a thirty-day setup window and never
 * invoices it before publication. The receipt carries enrolled_at and
 * expires_at (enrolled_at plus thirty days), no charge_due_at.
 * Repeating its price is safe; changing the terms conflicts. Requires trusted
 * service authority and leaves the catalogue trial unchanged.
 *
 * Returns: (transfer full) (nullable): durable enrollment identity and terms
 */
JsonNode *venture_lightsite_billing_enroll(VentureDatabase *database, gint64 billing,
	gint64 business, gint64 price_id, gboolean *created, GError **error);
/**
 * venture_lightsite_billing_setup:
 * @context: hosted application context
 * @billing: configured billing organization
 * @business: enrolled customer business
 * @error: return location for an error
 *
 * Creates or resumes hosted payment authorization without charging. No card
 * identity or secret is returned. An already verified permission returns
 * card_ready without creating another Setup session.
 *
 * Returns: (transfer full) (nullable): state and transient hosted URL
 */
JsonNode *venture_lightsite_billing_setup(VentureContext *context, gint64 billing,
	gint64 business, GError **error);
/**
 * venture_lightsite_billing_first_charge:
 * @context: hosted application context
 * @billing: configured billing organization
 * @business: enrolled customer business
 * @published: whether this is a confirmed first-publication notification
 * @error: return location for an error
 *
 * Records go-live once, then starts the first paid period at publication,
 * before or after the setup window expires. Without publication nothing is
 * invoiced: building or awaiting_card, then expired from expires_at. Verified saved-card authority is required. The durable
 * invoice is reserved before calling the provider; retries collect that same
 * invoice. Provider success alone does not mean paid: normal signed settlement
 * updates the invoice. Publication remains recorded when collection fails.
 *
 * Returns: (transfer full) (nullable): go-live, invoice identity and current state
 */
JsonNode *venture_lightsite_billing_first_charge(VentureContext *context, gint64 billing,
	gint64 business, gboolean published, GError **error);
/**
 * venture_lightsite_billing_publish:
 * @context: hosted application context
 * @billing: configured billing organization
 * @business: enrolled customer business
 * @published_at: (nullable): when the site actually went live; %NULL is now
 * @error: return location for an error
 *
 * venture_lightsite_billing_first_charge() as a publication notice, with the
 * moment the site actually went live. That moment becomes go-live, the start
 * of the billing clock, the first time only: a later notice never moves it.
 * A moment in the future or before enrollment is refused.
 *
 * Returns: (transfer full) (nullable): go-live, invoice identity and current state
 */
JsonNode *venture_lightsite_billing_publish(VentureContext *context, gint64 billing,
	gint64 business, GDateTime *published_at, GError **error);
/**
 * venture_lightsite_billing_money_back:
 * @context: hosted application context
 * @billing: configured billing organization
 * @business: enrolled and published business
 * @error: return location for an error
 *
 * The money-back promise. Within thirty days of go-live, ends the business's
 * subscription now and refunds every settled hosted card payment in full
 * through the provider, booking each refund against its receipt. The decision
 * and its refunds are kept as one receipt: a retry, including one after a lost
 * provider answer, finishes the same refunds and never makes another. After
 * thirty days, or before go-live, it is refused and nothing changes.
 *
 * Returns: (transfer full) (nullable): the cancellation and the refunds made
 */
JsonNode *venture_lightsite_billing_money_back(VentureContext *context, gint64 billing,
	gint64 business, GError **error);
/**
 * venture_lightsite_billing_made_back:
 * @database: hosted database
 * @billing: configured billing organization
 * @business: enrolled and published business
 * @until: (nullable): exclusive end, no later than now or the ninety-day cutoff
 * @error: return location for an error
 *
 * Measures retained signed acquisitions in one serializable snapshot. Won
 * value and frozen invoice net are shown separately; explicit linked overlap
 * counts once. Current source labels do not establish provenance. Missing
 * financial values, ambiguous acquisition and mixed currencies fail closed.
 *
 * Returns: (transfer full) (nullable): exact totals and measurement period
 */
JsonNode *venture_lightsite_billing_made_back(VentureDatabase *database, gint64 billing,
	gint64 business, GDateTime *until, GError **error);
/**
 * venture_lightsite_billing_guarantee:
 * @database: hosted database
 * @billing: configured billing organization
 * @business: enrolled customer business
 * @shortfall: measured nonnegative shortfall in the enrolled currency
 * @eligible: frozen checklist result from the trusted hosted client
 * @measurement: opaque identity of the retained measurement, not personal data
 * @error: return location for an error
 *
 * The hosted client owns checklist and paid-versus-made-back measurement.
 * Venture accepts that attestation only from its trusted service authority,
 * after ninety days from first publication, and applies at most the original
 * three-month cap as an ordinary customer credit. An identical retry returns
 * the original answer; a different measurement cannot pay again. Even zero
 * and ineligible outcomes are retained. No refund is created.
 *
 * Returns: (transfer full) (nullable): frozen measurement and credit identity
 */
JsonNode *venture_lightsite_billing_guarantee(VentureDatabase *database, gint64 billing,
	gint64 business, const VentureMoney *shortfall, gboolean eligible,
	const gchar *measurement, GError **error);
/**
 * venture_lightsite_billing_subscribe:
 * @database: the hosted workspace's database
 * @billing_organization_id: the configured billing organization, 0 when unset
 * @organization_id: the customer business's own organization
 * @plan_code: `team`, `growth` or `starter`
 * @idempotency_key: caller's key, 1-128 of `[A-Za-z0-9._:-]`
 * @created: (out) (optional): %TRUE when this call wrote the instruction
 * @error: return location for a #GError
 *
 * Binds a Lightsite business to a plan, in the billing organization: finds
 * or creates the customer company that refers to @organization_id, then
 * starts the plan's monthly price, or -- when the business already has a
 * live subscription -- changes it to that price at the end of the period,
 * through venture_billing_service_execute(). Requires the trusted-service
 * authority of venture_tenant_service_check_trusted_service(). A receipt
 * for the same key and request replays the first answer without writing; a
 * different request under the same key is a conflict.
 *
 * Returns: (transfer full) (nullable): the business's billing view, or %NULL
 */
JsonNode *venture_lightsite_billing_subscribe(VentureDatabase *database, gint64 billing_organization_id,
	gint64 organization_id, const gchar *plan_code, const gchar *idempotency_key, gboolean *created, GError **error);
/**
 * venture_lightsite_billing_view:
 * @database: the hosted workspace's database
 * @billing_organization_id: the configured billing organization, 0 when unset
 * @organization_id: the customer business's own organization
 * @error: return location for a #GError
 *
 * What @organization_id is on and owes in the billing organization: its
 * plan, subscription state, open balance, whether an invoice is overdue or
 * a payment failed, its latest invoice, and whether the business has
 * connected its own Stripe -- never a key or an account id. Writes nothing.
 *
 * Returns: (transfer full) (nullable): the billing view, or %NULL
 */
JsonNode *venture_lightsite_billing_view(VentureDatabase *database, gint64 billing_organization_id,
	gint64 organization_id, GError **error);
/**
 * venture_lightsite_billing_overview:
 * @database: the hosted workspace's database
 * @billing_organization_id: the configured billing organization, 0 when unset
 * @error: return location for a #GError
 *
 * The billing view of every Lightsite business that has a subscription in
 * the billing organization, ordered by organization, as
 * `{"customers": [...]}`. Writes nothing.
 *
 * Returns: (transfer full) (nullable): the overview, or %NULL
 */
JsonNode *venture_lightsite_billing_overview(VentureDatabase *database, gint64 billing_organization_id, GError **error);
/**
 * venture_lightsite_billing_check_save: (skip)
 * @database: the database with its lock held
 * @record: a proposed lightsite_billing_receipt save
 * @error: (out) (optional): error location
 *
 * Returns: %TRUE only for the receipt venture_lightsite_billing_subscribe() is writing
 */
gboolean venture_lightsite_billing_check_save(VentureDatabase *database, VentureEntity *record, GError **error);
G_END_DECLS
#endif
