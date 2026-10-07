/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_LIGHTSITE_BILLING_H
#define VENTURE_LIGHTSITE_BILLING_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
JsonNode *venture_lightsite_billing_prepay(VentureDatabase *database, gint64 billing,
	gint64 business, gint64 price_id, const gchar *payment_path, JsonArray *shares, gboolean *created, GError **error);
JsonNode *venture_lightsite_billing_enroll(VentureDatabase *database, gint64 billing,
	gint64 business, gint64 price_id, gboolean *created, GError **error);
JsonNode *venture_lightsite_billing_setup(VentureContext *context, gint64 billing,
	gint64 business, GError **error);
JsonNode *venture_lightsite_billing_first_charge(VentureContext *context, gint64 billing,
	gint64 business, gboolean published, GError **error);
JsonNode *venture_lightsite_billing_made_back(VentureDatabase *database, gint64 billing,
	gint64 business, GDateTime *until, GError **error);
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
