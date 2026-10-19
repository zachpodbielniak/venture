/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_LATE_FEE_PRIVATE_H
#define VENTURE_LATE_FEE_PRIVATE_H

/* Between the late-fee engine, its surfaces and the dunning sweep. */
gboolean venture_late_fee_service_sweep_settled(VentureLateFeeService *self, gint64 organization_id, GDateTime *as_of,
	gboolean dry_run, JsonArray *plan, VentureLateFeeSweepTally *tally, const VentureActor *actor, GError **error);
VentureDatabase *venture_late_fee_service_get_database(VentureLateFeeService *self);
gboolean venture_late_fee_service_claim_actions(VentureLateFeeService *self);
const gchar *venture_late_fee_service_pause_scope(VentureLateFeeService *self, VentureEntity *invoice, GDateTime *at);

#endif
