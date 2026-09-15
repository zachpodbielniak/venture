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
/**
 * venture_recurring_service_get:
 * @database: database owning the records
 *
 * Returns the per-database service. The database owns this reference.
 *
 * Returns: (transfer none): borrowed result
 */
VentureRecurringService *venture_recurring_service_get(VentureDatabase *database);
/**
 * venture_collection_service_get:
 * @database: database owning the records
 *
 * Returns the per-database service. The database owns this reference.
 *
 * Returns: (transfer none): borrowed result
 */
VentureCollectionService *venture_collection_service_get(VentureDatabase *database);
/**
 * venture_recurring_service_run:
 * @self: the service or registry instance
 * @organization_id: target legal entity ID
 * @as_of: (nullable): cutoff time; NULL uses the service default
 * @dry_run: whether to calculate without applying writes
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: number processed, or -1 on failure
 */
gint venture_recurring_service_run(VentureRecurringService *self, gint64 organization_id,
	GDateTime *as_of, gboolean dry_run, const VentureActor *actor, GError **error);
/**
 * venture_recurring_service_pause:
 * @self: the service or registry instance
 * @schedule: schedule record or expression
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_recurring_service_pause(VentureRecurringService *self, VentureEntity *schedule,
	const VentureActor *actor, GError **error);
/**
 * venture_recurring_service_resume:
 * @self: the service or registry instance
 * @schedule: schedule record or expression
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean venture_recurring_service_resume(VentureRecurringService *self, VentureEntity *schedule,
	const VentureActor *actor, GError **error);
/**
 * venture_recurring_service_batch:
 * @self: the service or registry instance
 * @kind: operation or field kind
 * @format: payload format
 * @payload: import payload
 * @post: whether imported drafts should also be posted
 * @dry_run: whether to calculate without applying writes
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: (transfer full) (nullable): owned result
 */
JsonNode *venture_recurring_service_batch(VentureRecurringService *self, const gchar *kind,
	const gchar *format, const gchar *payload, gboolean post, gboolean dry_run,
	const VentureActor *actor, GError **error);
/**
 * venture_collection_service_run:
 * @self: the service or registry instance
 * @context: application context
 * @organization_id: target legal entity ID
 * @as_of: (nullable): cutoff time; NULL uses the service default
 * @actor: (nullable): audit actor; NULL for internal service work
 * @error: (out) (optional): return location for an error
 *
 * Returns: number processed, or -1 on failure
 */
gint venture_collection_service_run(VentureCollectionService *self, VentureContext *context,
	gint64 organization_id, GDateTime *as_of, const VentureActor *actor, GError **error);
/**
 * venture_collection_service_set_context:
 * @self: collection service
 * @context: (nullable): reporting context, held weakly
 *
 * Supplies reports to collection actions invoked without an explicit context.
 */
void venture_collection_service_set_context(VentureCollectionService *self, VentureContext *context);
/**
 * venture_recurring_register_reports:
 * @registry: registry receiving the registrations
 */
void venture_recurring_register_reports(VentureReportRegistry *registry);
/**
 * venture_recurring_register_actions:
 * @database: database owning the records
 */
void venture_recurring_register_actions(VentureDatabase *database);
G_END_DECLS
#endif
