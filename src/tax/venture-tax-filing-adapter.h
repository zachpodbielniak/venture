/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_TAX_FILING_ADAPTER_H
#define VENTURE_TAX_FILING_ADAPTER_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS

#define VENTURE_TYPE_TAX_FILING_ADAPTER (venture_tax_filing_adapter_get_type())
G_DECLARE_INTERFACE(VentureTaxFilingAdapter, venture_tax_filing_adapter, VENTURE, TAX_FILING_ADAPTER, GObject)

/**
 * VentureTaxFilingAdapterInterface:
 * @parent_iface: parent interface
 * @get_name: registry key, usually the ISO country
 * @get_rule_id: versioned rule identifier recorded on the pack
 * @get_country: ISO country this adapter files
 * @prepare: fill json-pack, csv-pack and totals on @filing; no network
 */
struct _VentureTaxFilingAdapterInterface
{
	GTypeInterface parent_iface;
	const gchar *(*get_name)(VentureTaxFilingAdapter *self);
	const gchar *(*get_rule_id)(VentureTaxFilingAdapter *self);
	const gchar *(*get_country)(VentureTaxFilingAdapter *self);
	gboolean (*prepare)(VentureTaxFilingAdapter *self, VentureDatabase *database,
		VentureEntity *filing, GError **error);
};

/**
 * venture_tax_filing_adapter_get_name:
 * @self: an adapter
 * Returns: (transfer none): its registry key
 */
const gchar *venture_tax_filing_adapter_get_name(VentureTaxFilingAdapter *self);
/**
 * venture_tax_filing_adapter_get_rule_id:
 * @self: an adapter
 * Returns: (transfer none): versioned rule id
 */
const gchar *venture_tax_filing_adapter_get_rule_id(VentureTaxFilingAdapter *self);
/**
 * venture_tax_filing_adapter_get_country:
 * @self: an adapter
 * Returns: (transfer none): ISO country
 */
const gchar *venture_tax_filing_adapter_get_country(VentureTaxFilingAdapter *self);
/**
 * venture_tax_filing_adapter_prepare:
 * @self: an adapter
 * @database: source database
 * @filing: unsaved or draft filing to fill
 * @error: (out) (optional): error location
 * Returns: %TRUE on success
 */
gboolean venture_tax_filing_adapter_prepare(VentureTaxFilingAdapter *self,
	VentureDatabase *database, VentureEntity *filing, GError **error);

#define VENTURE_TYPE_TAX_FILING_ADAPTER_REGISTRY (venture_tax_filing_adapter_registry_get_type())
G_DECLARE_FINAL_TYPE(VentureTaxFilingAdapterRegistry, venture_tax_filing_adapter_registry,
	VENTURE, TAX_FILING_ADAPTER_REGISTRY, GObject)

/**
 * venture_tax_filing_adapter_registry_new:
 * Returns: (transfer full): an empty registry
 */
VentureTaxFilingAdapterRegistry *venture_tax_filing_adapter_registry_new(void);
/**
 * venture_tax_filing_adapter_registry_add:
 * @self: the registry
 * @adapter: (transfer full): implementation, replacing an existing key
 */
void venture_tax_filing_adapter_registry_add(VentureTaxFilingAdapterRegistry *self,
	VentureTaxFilingAdapter *adapter);
/**
 * venture_tax_filing_adapter_registry_lookup:
 * @self: the registry
 * @name: adapter key
 * Returns: (transfer none) (nullable): the implementation
 */
VentureTaxFilingAdapter *venture_tax_filing_adapter_registry_lookup(
	VentureTaxFilingAdapterRegistry *self, const gchar *name);
/**
 * venture_tax_filing_adapter_registry_remove:
 * @self: the registry
 * @name: adapter key
 * Returns: whether an implementation was removed
 */
gboolean venture_tax_filing_adapter_registry_remove(VentureTaxFilingAdapterRegistry *self,
	const gchar *name);
/**
 * venture_tax_filing_adapter_registry_list:
 * @self: the registry
 * Returns: (transfer container) (element-type VentureTaxFilingAdapter): sorted implementations
 */
GPtrArray *venture_tax_filing_adapter_registry_list(VentureTaxFilingAdapterRegistry *self);

/**
 * venture_us_sales_tax_adapter_new:
 * Returns: (transfer full): the built-in US sales-tax return adapter
 */
VentureTaxFilingAdapter *venture_us_sales_tax_adapter_new(void);

G_END_DECLS
#endif
