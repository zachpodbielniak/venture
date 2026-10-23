/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_LATE_FEE_RECORDS_H
#define VENTURE_LATE_FEE_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
#define VENTURE_TYPE_LATE_FEE_SETTING (venture_late_fee_setting_get_type())
VENTURE_DECLARE_ENTITY(VentureLateFeeSetting, venture_late_fee_setting, LATE_FEE_SETTING)
#define VENTURE_TYPE_LATE_FEE_SCHEDULE (venture_late_fee_schedule_get_type())
VENTURE_DECLARE_ENTITY(VentureLateFeeSchedule, venture_late_fee_schedule, LATE_FEE_SCHEDULE)
#define VENTURE_TYPE_LATE_FEE (venture_late_fee_get_type())
VENTURE_DECLARE_ENTITY(VentureLateFee, venture_late_fee, LATE_FEE)
G_END_DECLS
#endif
