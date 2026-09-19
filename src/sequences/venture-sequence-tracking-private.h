/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_SEQUENCE_TRACKING_PRIVATE_H
#define VENTURE_SEQUENCE_TRACKING_PRIVATE_H
#include "venture.h"

/* Pure helpers behind VentureSequenceService's open and click tracking. */
gchar *venture_sequence_tracking_token(GError **error);
gchar *venture_sequence_tracking_wrap(const gchar *body, const gchar *base_url,
	const gchar *token, GPtrArray *urls);
const guint8 *venture_sequence_tracking_pixel(gsize *length);

#endif
