/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "venture.h"

void venture_late_fee_actions_register(VentureDatabase *database) { (void)database; }
void venture_late_fee_register_reports(VentureReportRegistry *registry) { (void)registry; }
void venture_late_fee_append_timeline(VentureContext *context, const gchar *target_type, gint64 target_id, guint limit, VentureLateFeeTimelineAdd add, GPtrArray *events)
{ (void)context; (void)target_type; (void)target_id; (void)limit; (void)add; (void)events; }
