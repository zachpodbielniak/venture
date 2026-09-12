/*
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef VENTURE_LEDGER_PRIVATE_H
#define VENTURE_LEDGER_PRIVATE_H

/* Internal one-shot permits never cross the public posting boundary. */
gboolean venture_ledger_check_write(VentureDatabase *db, VentureEntity *entity,
	VentureEntity *previous, gboolean removing, gboolean *authorized, GError **error);
gboolean venture_ledger_wrap_source(VentureDatabase *db, VentureEntity *entity);
gboolean venture_ledger_save_source(VentureDatabase *db, VentureEntity *entity,
	const VentureActor *actor, GError **error);
gboolean venture_ledger_post_legacy(VentureDatabase *db, GPtrArray *entries,
	const VentureActor *actor, GError **error);
void venture_ledger_register_rules(VenturePostingRuleRegistry *registry);
void venture_ledger_register_report(VentureReportRegistry *registry);

#endif
