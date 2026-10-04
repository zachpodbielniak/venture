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

#ifdef VENTURE_HAVE_SQLITE

/*
 * venture_marketdata_source_in:
 * The data source @data_source_id, which must be @organization_id's and
 * not deleted; NOT_FOUND otherwise.
 */
VentureEntity *
venture_marketdata_source_in(
	VentureDatabase	 *database,
	gint64		  organization_id,
	gint64		  data_source_id,
	GError		**error
);

/*
 * venture_marketdata_reader:
 * A main-thread read handle on the source's store; CONFIG with the feeds
 * module off, NOT_FOUND before the source has stored anything.
 */
VentureSeriesStore *
venture_marketdata_reader(
	VentureContext	 *context,
	gint64		  data_source_id,
	GError		**error
);

/*
 * venture_marketdata_promote_instrument_in:
 * Promotion of one instrument the store @store knows, as the public call
 * does, except that with @restore FALSE a deleted record is handed back
 * deleted rather than restored -- what the position mirror asks for, so
 * it never brings back what a person removed. *@out is set on success.
 */
gboolean
venture_marketdata_promote_instrument_in(
	VentureContext		 *context,
	VentureSeriesStore	 *store,
	VentureEntity		 *source,
	const gchar		 *key,
	gboolean		  restore,
	const VentureActor	 *actor,
	VentureEntity		**out,
	GError			**error
);

/*
 * venture_marketdata_promote_venue_in:
 * The same for a venue, from the store's row for it.
 */
gboolean
venture_marketdata_promote_venue_in(
	VentureContext			 *context,
	VentureEntity			 *source,
	const VentureSeriesVenueRow	 *row,
	gboolean			  restore,
	const VentureActor		 *actor,
	VentureEntity			**out,
	GError				**error
);

#endif /* VENTURE_HAVE_SQLITE */

G_END_DECLS

#endif /* VENTURE_MARKETDATA_PRIVATE_H */
