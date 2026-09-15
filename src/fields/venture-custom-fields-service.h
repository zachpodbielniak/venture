/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_CUSTOM_FIELDS_SERVICE_H
#define VENTURE_CUSTOM_FIELDS_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_CUSTOM_FIELDS_SERVICE (venture_custom_fields_service_get_type())
G_DECLARE_FINAL_TYPE(VentureCustomFieldsService, venture_custom_fields_service, VENTURE, CUSTOM_FIELDS_SERVICE, GObject)
/**
 * venture_custom_fields_service_get:
 * @database: the owning database
 *
 * Returns: (transfer none): the per-database service
 */
VentureCustomFieldsService *venture_custom_fields_service_get(VentureDatabase *database);
/**
 * venture_custom_fields_validate:
 * @database: database owning the records
 * @record: candidate record
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_custom_fields_validate(VentureDatabase *database, VentureEntity *record, GError **error);
/**
 * venture_custom_fields_sync:
 * @database: database owning the records
 * @record: candidate record
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Updates the derived custom-value index inside the parent save transaction.
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_custom_fields_sync(VentureDatabase *database, VentureEntity *record, const VentureActor *actor, GError **error);
/**
 * venture_custom_fields_form_specs:
 * @database: the database
 * @organization_id: legal entity
 * @record_type: registered entity name
 * @record: (nullable): existing row to hydrate current values
 * @error: (out) (optional)
 *
 * Field specs for configured custom fields, ordered by layout when one exists.
 *
 * Returns: (transfer full) (element-type VentureFieldSpec) (nullable)
 */
GPtrArray *venture_custom_fields_form_specs(VentureDatabase *database, gint64 organization_id,
	const gchar *record_type, VentureEntity *record, GError **error);
/**
 * venture_custom_fields_service_define:
 * @self: the service or registry instance
 * @organization_id: target legal entity ID
 * @record_type: registered entity name
 * @name: name or registry key
 * @kind: operation or field kind
 * @required: whether an empty custom value is refused
 * @options: report or field options
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Creates a validated custom definition. Built-in property names, including sensitive properties, cannot be shadowed.
 *
 * Returns: (transfer full) (nullable): owned result
 */
VentureEntity *venture_custom_fields_service_define(VentureCustomFieldsService *self, gint64 organization_id,
	const gchar *record_type, const gchar *name, const gchar *kind, gboolean required,
	const gchar *options, const VentureActor *actor, GError **error);
/**
 * venture_custom_fields_service_put_value:
 * @self: the service or registry instance
 * @organization_id: target legal entity ID
 * @record_type: registered entity name
 * @record_id: owning record ID
 * @name: name or registry key
 * @value: value to save; an empty string clears an optional field
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Updates the owning entity and its derived custom-value index through the ordinary save path. Re-read an already loaded parent before saving it again.
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_custom_fields_service_put_value(VentureCustomFieldsService *self, gint64 organization_id,
	const gchar *record_type, gint64 record_id, const gchar *name, const gchar *value,
	const VentureActor *actor, GError **error);
/**
 * venture_custom_fields_service_set_layout:
 * @self: the service or registry instance
 * @organization_id: target legal entity ID
 * @record_type: registered entity name
 * @field_order: JSON array of field names
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: (transfer full) (nullable): owned result
 */
VentureEntity *venture_custom_fields_service_set_layout(VentureCustomFieldsService *self, gint64 organization_id,
	const gchar *record_type, const gchar *field_order, const VentureActor *actor, GError **error);
/**
 * venture_custom_fields_order_specs:
 * @database: owning database
 * @organization_id: legal entity
 * @record_type: registered entity name
 * @specs: (element-type VentureFieldSpec) (transfer none): specs to reorder in place
 *
 * Applies the configured layout to built-in and custom specs together.
 * Unspecified fields retain their relative declaration order; no ownership
 * changes and no field or sensitive flag is removed.
 */
void venture_custom_fields_order_specs(VentureDatabase *database, gint64 organization_id,
	const gchar *record_type, GPtrArray *specs);
G_END_DECLS
#endif
