/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_CUSTOM_FIELDS_SERVICE_H
#define VENTURE_CUSTOM_FIELDS_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_CUSTOM_FIELDS_SERVICE (venture_custom_fields_service_get_type())
G_DECLARE_FINAL_TYPE(VentureCustomFieldsService, venture_custom_fields_service, VENTURE, CUSTOM_FIELDS_SERVICE, GObject)
VentureCustomFieldsService *venture_custom_fields_service_get(VentureDatabase *database);
gboolean venture_custom_fields_validate(VentureDatabase *database, VentureEntity *record, GError **error);
gboolean venture_custom_fields_sync(VentureDatabase *database, VentureEntity *record, const VentureActor *actor, GError **error);
VentureEntity *venture_custom_fields_service_define(VentureCustomFieldsService *self, gint64 organization_id,
	const gchar *record_type, const gchar *name, const gchar *kind, gboolean required,
	const gchar *options, const VentureActor *actor, GError **error);
gboolean venture_custom_fields_service_put_value(VentureCustomFieldsService *self, gint64 organization_id,
	const gchar *record_type, gint64 record_id, const gchar *name, const gchar *value,
	const VentureActor *actor, GError **error);
VentureEntity *venture_custom_fields_service_set_layout(VentureCustomFieldsService *self, gint64 organization_id,
	const gchar *record_type, const gchar *field_order, const VentureActor *actor, GError **error);
G_END_DECLS
#endif
