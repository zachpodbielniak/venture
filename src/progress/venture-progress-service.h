/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_PROGRESS_SERVICE_H
#define VENTURE_PROGRESS_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_PROGRESS_SERVICE (venture_progress_service_get_type())
G_DECLARE_FINAL_TYPE(VentureProgressService, venture_progress_service, VENTURE, PROGRESS_SERVICE, GObject)
VentureProgressService *venture_progress_service_get(VentureDatabase *database);
gboolean venture_progress_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error);
VentureMoney *venture_progress_service_remaining(VentureProgressService *self,
	VentureQuote *quote, GError **error);
VentureEntity *venture_progress_service_invoice(VentureProgressService *self,
	VentureQuote *quote, gint64 percent, const VentureMoney *amount,
	const VentureActor *actor, GError **error);
VentureEntity *venture_progress_service_collect_retainer(VentureProgressService *self,
	gint64 organization_id, gint64 company_id, gint64 liability_account_id,
	const VentureMoney *amount, const VentureActor *actor, GError **error);
gboolean venture_progress_service_release_retainer(VentureProgressService *self,
	VentureCustomerRetainer *retainer, const VentureMoney *amount,
	const VentureActor *actor, GError **error);
VentureEntity *venture_progress_service_hold_retention(VentureProgressService *self,
	VentureQuote *quote, gint64 liability_account_id, const VentureMoney *amount,
	const VentureActor *actor, GError **error);
gboolean venture_progress_service_release_retention(VentureProgressService *self,
	VentureContractRetention *retention, const VentureMoney *amount,
	const VentureActor *actor, GError **error);
void venture_progress_actions_register(VentureDatabase *database);
G_END_DECLS
#endif
