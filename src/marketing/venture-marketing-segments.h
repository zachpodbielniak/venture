/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_MARKETING_SEGMENTS_H
#define VENTURE_MARKETING_SEGMENTS_H
#include <venture.h>
/* Private to the marketing service: the saved-search criteria that are
 * not field filters -- a tag and behaviour within N days. */
enum {
	SEGMENT_OPENED, SEGMENT_CLICKED, SEGMENT_REPLIED, SEGMENT_BOUGHT, SEGMENT_BOOKED, SEGMENT_BEHAVIOURS
};
typedef struct {
	gchar **tags;
	gint64 within[SEGMENT_BEHAVIOURS];
} VentureMarketingCriteria;
/* Removes the criteria's names from @params (so the rest are ordinary
 * filters) and checks them: behaviour is for contact segments only. */
gboolean venture_marketing_criteria_take(GHashTable *params, gboolean contacts, gboolean tagged,
	VentureMarketingCriteria *criteria, GError **error);
void venture_marketing_criteria_clear(VentureMarketingCriteria *criteria);
/* Puts the criteria into @query: a substring match per tag and the ids
 * that behaved, so the row bound counts what is asked. @empty is set when
 * nobody behaved and there is nothing to query. */
gboolean venture_marketing_criteria_narrow(VentureDatabase *database, gint64 organization,
	const VentureMarketingCriteria *criteria, GDateTime *now, VentureQuery *query, gboolean *empty, GError **error);
/* The exact tag check the substring match cannot make. */
gboolean venture_marketing_criteria_keep(const VentureMarketingCriteria *criteria, VentureEntity *row);
#endif
