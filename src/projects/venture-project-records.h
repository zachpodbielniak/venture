/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef VENTURE_PROJECT_RECORDS_H
#define VENTURE_PROJECT_RECORDS_H
#if !defined(VENTURE_INSIDE) && !defined(VENTURE_COMPILATION)
#error "Only <venture.h> can be included directly."
#endif
G_BEGIN_DECLS
/**
 * VentureProjectDeliveryStatus:
 * @VENTURE_PROJECT_DELIVERY_ACTIVE: work may be planned and accepted
 * @VENTURE_PROJECT_DELIVERY_PAUSED: new delivery work is paused
 * @VENTURE_PROJECT_DELIVERY_COMPLETED: all planned delivery was accepted
 * @VENTURE_PROJECT_DELIVERY_CANCELLED: stopped without erasing financial history
 *
 * Delivery lifecycle, independent of invoice and payment status.
 */
typedef enum {
	VENTURE_PROJECT_DELIVERY_ACTIVE,
	VENTURE_PROJECT_DELIVERY_PAUSED,
	VENTURE_PROJECT_DELIVERY_COMPLETED,
	VENTURE_PROJECT_DELIVERY_CANCELLED
} VentureProjectDeliveryStatus;
#define VENTURE_TYPE_PROJECT_DELIVERY_STATUS (venture_project_delivery_status_get_type())
/**
 * venture_project_delivery_status_get_type:
 *
 * Returns: the registered project delivery status enum type
 */
GType venture_project_delivery_status_get_type(void) G_GNUC_CONST;
#define VENTURE_TYPE_CLIENT_PROJECT (venture_client_project_get_type())
VENTURE_DECLARE_ENTITY(VentureClientProject, venture_client_project, CLIENT_PROJECT)
#define VENTURE_TYPE_PROJECT_RATE (venture_project_rate_get_type())
VENTURE_DECLARE_ENTITY(VentureProjectRate, venture_project_rate, PROJECT_RATE)
#define VENTURE_TYPE_PROJECT_TIME (venture_project_time_get_type())
VENTURE_DECLARE_ENTITY(VentureProjectTime, venture_project_time, PROJECT_TIME)
#define VENTURE_TYPE_PROJECT_COST (venture_project_cost_get_type())
VENTURE_DECLARE_ENTITY(VentureProjectCost, venture_project_cost, PROJECT_COST)
#define VENTURE_TYPE_PROJECT_BILLING (venture_project_billing_get_type())
VENTURE_DECLARE_ENTITY(VentureProjectBilling, venture_project_billing, PROJECT_BILLING)
#define VENTURE_TYPE_PROJECT_SCOPE (venture_project_scope_get_type())
VENTURE_DECLARE_ENTITY(VentureProjectScope, venture_project_scope, PROJECT_SCOPE)
#define VENTURE_TYPE_PROJECT_DELIVERABLE (venture_project_deliverable_get_type())
VENTURE_DECLARE_ENTITY(VentureProjectDeliverable, venture_project_deliverable, PROJECT_DELIVERABLE)
G_END_DECLS
#endif
