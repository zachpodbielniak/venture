/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_TAX_FILING_SERVICE_H
#define VENTURE_TAX_FILING_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS

#define VENTURE_TYPE_TAX_FILING_SERVICE (venture_tax_filing_service_get_type())
G_DECLARE_FINAL_TYPE(VentureTaxFilingService, venture_tax_filing_service,
	VENTURE, TAX_FILING_SERVICE, GObject)

/**
 * venture_tax_filing_service_get:
 * @database: the owning repository
 * Returns: (transfer none): its tax filing service
 */
VentureTaxFilingService *venture_tax_filing_service_get(VentureDatabase *database);

/**
 * venture_tax_filing_service_get_adapters:
 * @self: the service
 * Returns: (transfer none): the adapter registry
 */
VentureTaxFilingAdapterRegistry *venture_tax_filing_service_get_adapters(VentureTaxFilingService *self);

/**
 * venture_tax_filing_service_prepare:
 * @self: the service
 * @organization_id: legal entity
 * @country: (nullable): ISO country; required
 * @jurisdiction: (nullable): filing jurisdiction
 * @fiscal_period_id: fiscal period, or 0 when dates are supplied
 * @period_start: (nullable): inclusive start
 * @period_end: (nullable): exclusive end
 * @actor: (nullable): the audit actor
 * @error: (out) (optional): failure
 * Returns: (transfer full) (nullable): a draft filing pack
 */
VentureEntity *venture_tax_filing_service_prepare(VentureTaxFilingService *self,
	gint64 organization_id, const gchar *country, const gchar *jurisdiction,
	gint64 fiscal_period_id, GDateTime *period_start, GDateTime *period_end,
	const VentureActor *actor, GError **error);

gboolean venture_tax_filing_service_review(VentureTaxFilingService *self, VentureEntity *filing,
	const VentureActor *actor, GError **error);
gboolean venture_tax_filing_service_submit(VentureTaxFilingService *self, VentureEntity *filing,
	const VentureActor *actor, GError **error);
gboolean venture_tax_filing_service_acknowledge(VentureTaxFilingService *self, VentureEntity *filing,
	const gchar *acknowledgment_id, const VentureActor *actor, GError **error);
/**
 * venture_tax_filing_service_amend:
 * Returns: (transfer full) (nullable): a new draft; the prior pack is kept
 */
VentureEntity *venture_tax_filing_service_amend(VentureTaxFilingService *self, VentureEntity *filing,
	const VentureActor *actor, GError **error);

/**
 * venture_tax_filing_service_prepare_1099:
 * Returns: (transfer full) (nullable): a 1099-NEC pack at or above $600
 */
VentureEntity *venture_tax_filing_service_prepare_1099(VentureTaxFilingService *self,
	gint64 organization_id, gint64 vendor_id, gint year, const VentureActor *actor,
	GError **error);
gboolean venture_tax_filing_service_review_1099(VentureTaxFilingService *self, VentureEntity *pack,
	const VentureActor *actor, GError **error);
gboolean venture_tax_filing_service_approve_1099(VentureTaxFilingService *self, VentureEntity *pack,
	const VentureActor *actor, GError **error);
/**
 * venture_tax_filing_service_export_1099:
 * Returns: (transfer full) (nullable): frozen CSV bytes; a second call is identical
 */
gchar *venture_tax_filing_service_export_1099(VentureTaxFilingService *self, VentureEntity *pack,
	const VentureActor *actor, GError **error);

gboolean venture_tax_filing_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error);
gboolean venture_tax_filing_save_hook(VentureDatabase *database, VentureEntity *record,
	const VentureActor *actor, gboolean *handled, GError **error);
void venture_tax_filing_actions_register(VentureDatabase *database);

G_END_DECLS
#endif
