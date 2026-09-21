/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_SALES_PRIVATE_H
#define VENTURE_SALES_PRIVATE_H
/* Only the database supplies the continuation after its authorized hooks. */
typedef gboolean (*VentureSalesSaveContinuation)(VentureDatabase *database,
	VentureEntity *entity, const VentureActor *actor, GError **error);
gboolean venture_sales_save_hook(VentureDatabase *database, VentureEntity *entity,
	const VentureActor *actor, VentureSalesSaveContinuation save, gboolean *handled, GError **error);
gboolean venture_sales_check_write(VentureDatabase *database, VentureEntity *entity,
	gboolean removal, GError **error);
gboolean venture_sales_check_purge(VentureDatabase *database, VentureEntity *entity, GError **error);
#endif
