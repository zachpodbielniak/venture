/* Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_RECURRING_SERVICE_H
#define VENTURE_RECURRING_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_RECURRING_SERVICE (venture_recurring_service_get_type())
G_DECLARE_FINAL_TYPE(VentureRecurringService, venture_recurring_service, VENTURE, RECURRING_SERVICE, GObject)
#define VENTURE_TYPE_COLLECTION_SERVICE (venture_collection_service_get_type())
G_DECLARE_FINAL_TYPE(VentureCollectionService, venture_collection_service, VENTURE, COLLECTION_SERVICE, GObject)
VentureRecurringService *venture_recurring_service_get(VentureDatabase *database);
VentureCollectionService *venture_collection_service_get(VentureDatabase *database);
gint venture_recurring_service_run(VentureRecurringService *self, gint64 organization_id,
	GDateTime *as_of, gboolean dry_run, const VentureActor *actor, GError **error);
gboolean venture_recurring_service_pause(VentureRecurringService *self, VentureEntity *schedule,
	const VentureActor *actor, GError **error);
gboolean venture_recurring_service_resume(VentureRecurringService *self, VentureEntity *schedule,
	const VentureActor *actor, GError **error);
JsonNode *venture_recurring_service_batch(VentureRecurringService *self, const gchar *kind,
	const gchar *format, const gchar *payload, gboolean post, gboolean dry_run,
	const VentureActor *actor, GError **error);
gint venture_collection_service_run(VentureCollectionService *self, VentureContext *context,
	gint64 organization_id, GDateTime *as_of, const VentureActor *actor, GError **error);
void venture_recurring_register_reports(VentureReportRegistry *registry);
void venture_recurring_register_actions(VentureDatabase *database);
G_END_DECLS
#endif
