/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_ACTIVITY_RECORDS_H
#define VENTURE_ACTIVITY_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
/**
 * VentureActivityKind:
 * @VENTURE_ACTIVITY_KIND_TASK: task
 * @VENTURE_ACTIVITY_KIND_CALL: call
 * @VENTURE_ACTIVITY_KIND_MEETING: meeting
 * @VENTURE_ACTIVITY_KIND_EMAIL: email
 * @VENTURE_ACTIVITY_KIND_FOLLOWUP: followup
 *
 * Planned activity kind.
 */
typedef enum {
	VENTURE_ACTIVITY_KIND_TASK,
	VENTURE_ACTIVITY_KIND_CALL,
	VENTURE_ACTIVITY_KIND_MEETING,
	VENTURE_ACTIVITY_KIND_EMAIL,
	VENTURE_ACTIVITY_KIND_FOLLOWUP
} VentureActivityKind;
/**
 * venture_activity_kind_get_type:
 *
 * Returns: the kind enumeration type
 */
GType venture_activity_kind_get_type(void) G_GNUC_CONST;
/**
 * VentureActivityStatus:
 * @VENTURE_ACTIVITY_STATUS_PLANNED: planned
 * @VENTURE_ACTIVITY_STATUS_DONE: done
 * @VENTURE_ACTIVITY_STATUS_CANCELLED: cancelled
 *
 * Planned activity status.
 */
typedef enum {
	VENTURE_ACTIVITY_STATUS_PLANNED,
	VENTURE_ACTIVITY_STATUS_DONE,
	VENTURE_ACTIVITY_STATUS_CANCELLED
} VentureActivityStatus;
/**
 * venture_activity_status_get_type:
 *
 * Returns: the status enumeration type
 */
GType venture_activity_status_get_type(void) G_GNUC_CONST;
/**
 * VentureActivityRecurrence:
 * @VENTURE_ACTIVITY_RECURRENCE_NONE: none
 * @VENTURE_ACTIVITY_RECURRENCE_DAILY: daily
 * @VENTURE_ACTIVITY_RECURRENCE_WEEKLY: weekly
 * @VENTURE_ACTIVITY_RECURRENCE_MONTHLY: monthly
 *
 * Planned activity recurrence.
 */
typedef enum {
	VENTURE_ACTIVITY_RECURRENCE_NONE,
	VENTURE_ACTIVITY_RECURRENCE_DAILY,
	VENTURE_ACTIVITY_RECURRENCE_WEEKLY,
	VENTURE_ACTIVITY_RECURRENCE_MONTHLY
} VentureActivityRecurrence;
/**
 * venture_activity_recurrence_get_type:
 *
 * Returns: the recurrence enumeration type
 */
GType venture_activity_recurrence_get_type(void) G_GNUC_CONST;
#define VENTURE_TYPE_ACTIVITY (venture_activity_get_type())
#define VENTURE_TYPE_ACTIVITY_TYPE (venture_activity_type_get_type())
VENTURE_DECLARE_ENTITY(VentureActivity, venture_activity, ACTIVITY)
VENTURE_DECLARE_ENTITY(VentureActivityType, venture_activity_type, ACTIVITY_TYPE)
G_END_DECLS
#endif
