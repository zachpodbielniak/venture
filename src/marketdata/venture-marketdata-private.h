/*
 * venture-marketdata-private.h - Checks the marketdata files share
 *
 * Copyright (C) 2026 Zach Podbielniak
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Include only from src/marketdata. The validators for venues and
 * instruments (venture-marketdata.c) and for alert rules
 * (venture-marketdata-alerts.c) hold text and references to the same
 * rules, and one definition of each is what keeps them the same.
 */

#ifndef VENTURE_MARKETDATA_PRIVATE_H
#define VENTURE_MARKETDATA_PRIVATE_H

#include <glib-object.h>

G_BEGIN_DECLS

/*
 * venture_marketdata_check_text:
 * A namespace, key, group or pattern: at most the store's key length,
 * valid UTF-8 and no control characters. Empty is fine.
 */
gboolean
venture_marketdata_check_text(
	const gchar	 *value,
	const gchar	 *label,
	GError		**error
);

/*
 * venture_marketdata_check_same_organization:
 * Refuses a reference being written to a record in another organization;
 * a value kept from @previous is left alone.
 */
gboolean
venture_marketdata_check_same_organization(
	VentureDatabase	 *database,
	VentureEntity	 *entity,
	VentureEntity	 *previous,
	const gchar	 *property,
	GType		  target_type,
	const gchar	 *label,
	GError		**error
);

G_END_DECLS

#endif /* VENTURE_MARKETDATA_PRIVATE_H */
