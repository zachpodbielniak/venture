/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_BUSINESS_EVENTS_PRIVATE_H
#define VENTURE_BUSINESS_EVENTS_PRIVATE_H
#include <venture.h>
gboolean venture_business_events_capture(VentureDatabase *database, VentureEntity *entity, VentureEntity *previous, GError **error);
gboolean venture_business_events_check_write(VentureDatabase *database, VentureEntity *entity, gboolean removal, GError **error);
gboolean venture_business_events_store(VentureDatabase *database, VentureEntity *entity, GError **error);
#endif
