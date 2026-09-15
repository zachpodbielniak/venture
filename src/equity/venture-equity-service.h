/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_EQUITY_SERVICE_H
#define VENTURE_EQUITY_SERVICE_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_CAPITAL_SERVICE (venture_capital_service_get_type())
G_DECLARE_FINAL_TYPE(VentureCapitalService, venture_capital_service, VENTURE, CAPITAL_SERVICE, GObject)
VentureCapitalService *venture_capital_service_get(VentureDatabase *database);
gboolean venture_equity_check_write(VentureDatabase *database, VentureEntity *record,
	gboolean removal, GError **error);
VentureEntity *venture_capital_service_post(VentureCapitalService *self, gint64 organization_id,
	VentureEquityKind kind, const VentureMoney *amount, GDateTime *when, const gchar *memo,
	gint64 debit_account_id, gint64 credit_account_id, const VentureActor *actor, GError **error);
G_END_DECLS
#endif
