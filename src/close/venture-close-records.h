/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_CLOSE_RECORDS_H
#define VENTURE_CLOSE_RECORDS_H

#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif

G_BEGIN_DECLS

#define VENTURE_TYPE_CLOSE_WORKSPACE (venture_close_workspace_get_type())
VENTURE_DECLARE_ENTITY(VentureCloseWorkspace, venture_close_workspace, CLOSE_WORKSPACE)
#define VENTURE_TYPE_CLOSE_TASK (venture_close_task_get_type())
VENTURE_DECLARE_ENTITY(VentureCloseTask, venture_close_task, CLOSE_TASK)
#define VENTURE_TYPE_CLOSE_WORKPAPER (venture_close_workpaper_get_type())
VENTURE_DECLARE_ENTITY(VentureCloseWorkpaper, venture_close_workpaper, CLOSE_WORKPAPER)
#define VENTURE_TYPE_CLOSE_DISCREPANCY (venture_close_discrepancy_get_type())
VENTURE_DECLARE_ENTITY(VentureCloseDiscrepancy, venture_close_discrepancy, CLOSE_DISCREPANCY)
#define VENTURE_TYPE_CLOSE_SIGNOFF (venture_close_signoff_get_type())
VENTURE_DECLARE_ENTITY(VentureCloseSignoff, venture_close_signoff, CLOSE_SIGNOFF)

/**
 * venture_close_workspace_new:
 * Returns: (transfer full): a close workspace for one fiscal period
 */
/**
 * venture_close_task_new:
 * Returns: (transfer full): a preparer or reviewer close task
 */
/**
 * venture_close_workpaper_new:
 * Returns: (transfer full): a close workpaper
 */
/**
 * venture_close_discrepancy_new:
 * Returns: (transfer full): a subledger discrepancy with an explanation
 */
/**
 * venture_close_signoff_new:
 * Returns: (transfer full): a preparer or reviewer signoff
 */

G_END_DECLS
#endif
